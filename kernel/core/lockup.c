/*
 * lockup.c - A CPU's program counter, seen from another CPU; the soft-
 * and hard-lockup detectors (docs/kernel/diagnostics/design.md,
 * "Lockups").
 *
 * The rule, stated once: a CPU's interrupted context is recorded by
 * that CPU, in its own handler, into its own per-CPU buffer, without a
 * lock and without printing; whoever asked reads the buffers and
 * prints. It follows from what an NMI may do (nothing that blocks,
 * nothing that takes a lock the interrupted code may hold -- the
 * console's included) and from what a frame-pointer walk needs (the
 * walker's checks are written for the local thread's stack; another
 * CPU's live stack cannot be walked from outside).
 *
 * There is one reporter at a time and nobody waits to become it: a
 * reporter may be waiting with interrupts off (the tick is where the
 * detectors run), and a second CPU spinning for the slot with its own
 * interrupts off would stop its own ticks and, on AArch64, its own
 * answers -- a diagnostic that manufactures the stall it diagnoses.
 */

#include <kernel/lockup.h>

#include <kernel/ipi.h>
#include <kernel/kernel.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/printf.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/thread.h>
#include <kernel/timer.h>

#include <arch/backtrace.h>
#include <arch/cpu.h>
#include <arch/irqc.h>
#include <arch/trap.h>

static uint64_t g_sample_seq;               /* the last request issued */
static volatile int g_reporter;             /* 0 = free, else CPU id + 1 */
static bool g_enabled;                      /* the detectors run (lockup_init) */
static uint64_t g_soft_ns = LOCKUP_SOFT_NS_DEFAULT;
static uint64_t g_hard_ns = LOCKUP_HARD_NS_DEFAULT;
static bool g_expected;                     /* a test asked for the report it is about to see */
static struct lockup_stats g_stats;         /* the reports' facts: under g_stats_lock */
static uint64_t g_samples, g_samples_busy;   /* the sample counters: atomic, outside the lock */
/* Every report writes its fields and bumps its counter under this leaf
 * lock, and the reader takes it too, so a snapshot is one report's, never
 * two watchers' fields mixed (a soft and a hard report may land in the
 * same tick on different CPUs). Never held across a sample or a print. */
static spinlock_t g_stats_lock = SPINLOCK_INIT("lockup-stats");

/* --- the sample --- */

static void record(struct cpu_sample *s, const struct arch_trap_frame *frame, uint64_t seq, bool nmi)
{
    s->depth = (unsigned)arch_backtrace(s->trace, LOCKUP_TRACE_MAX, frame);
    if (frame != NULL) {
        s->pc = arch_trap_frame_pc(frame);
        s->sp = arch_trap_frame_sp(frame);
    } else {
        s->pc = s->depth > 0 ? s->trace[0] : 0;
        s->sp = (uintptr_t)__builtin_frame_address(0);
    }
    s->nmi = nmi;
    s->when_ns = clock_now_ns();
    __atomic_store_n(&s->seq, seq, __ATOMIC_RELEASE);
}

bool lockup_answer(struct arch_trap_frame *frame, bool nmi)
{
    struct percpu *pc = this_cpu();
    uint64_t want = __atomic_load_n(&pc->sample.want, __ATOMIC_ACQUIRE);
    if (want == __atomic_load_n(&pc->sample.seq, __ATOMIC_RELAXED))
        return false;               /* nothing pending for this CPU: not ours */
    record(&pc->sample, frame, want, nmi);
    return true;
}

int lockup_reporter(void)
{
    return __atomic_load_n(&g_reporter, __ATOMIC_ACQUIRE) - 1;
}

bool lockup_sample_all(const struct arch_trap_frame *self, uint64_t timeout_ns, cpumask_t *answered)
{
    unsigned me = arch_cpu_id();
    int expected = 0;
    *answered = 0;
    if (!__atomic_compare_exchange_n(&g_reporter, &expected, (int)me + 1, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE)) {
        __atomic_fetch_add(&g_samples_busy, 1, __ATOMIC_RELAXED);
        return false;
    }
    __atomic_fetch_add(&g_samples, 1, __ATOMIC_RELAXED);

    uint64_t seq = __atomic_add_fetch(&g_sample_seq, 1, __ATOMIC_ACQ_REL);
    cpumask_t targets = cpu_online_mask() & ~CPUMASK_OF(me);

    /* The claim, then the interrupt, per target: a target that answers
     * an earlier NMI in between sees this request pending and answers
     * it too, which is the right outcome. */
    for (unsigned c = 0; c < cpu_count(); c++) {
        if (!(targets & CPUMASK_OF(c)))
            continue;
        struct percpu *pc = percpu_get(c);
        __atomic_store_n(&pc->sample.want, seq, __ATOMIC_RELEASE);
        if (!arch_ipi_send_nmi(c))
            ipi_send(c, IPI_SAMPLE);
    }

    record(&this_cpu()->sample, self, seq, false);

    /* One wait for every target together: the bound is total. */
    uint64_t deadline = clock_deadline_ns(timeout_ns);
    cpumask_t got = 0;
    for (;;) {
        for (unsigned c = 0; c < cpu_count(); c++) {
            if ((targets & CPUMASK_OF(c)) && !(got & CPUMASK_OF(c)) &&
                __atomic_load_n(&percpu_get(c)->sample.seq, __ATOMIC_ACQUIRE) == seq)
                got |= CPUMASK_OF(c);
        }
        if (got == targets || clock_deadline_passed(deadline))
            break;
        arch_cpu_relax();
    }
    *answered = got | CPUMASK_OF(me);
    return true;
}

bool lockup_sample_cpu(unsigned cpu, uint64_t timeout_ns, struct cpu_sample *out)
{
    unsigned me = arch_cpu_id();
    int expected = 0;
    if (cpu == me || !cpu_online(cpu))
        return false;
    if (!__atomic_compare_exchange_n(&g_reporter, &expected, (int)me + 1, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE))
        return false;
    uint64_t seq = __atomic_add_fetch(&g_sample_seq, 1, __ATOMIC_ACQ_REL);
    struct percpu *pc = percpu_get(cpu);
    __atomic_store_n(&pc->sample.want, seq, __ATOMIC_RELEASE);
    if (!arch_ipi_send_nmi(cpu))
        ipi_send(cpu, IPI_SAMPLE);
    uint64_t deadline = clock_deadline_ns(timeout_ns);
    bool got = false;
    while (!(got = __atomic_load_n(&pc->sample.seq, __ATOMIC_ACQUIRE) == seq) && !clock_deadline_passed(deadline))
        arch_cpu_relax();
    if (got)
        *out = pc->sample;
    __atomic_store_n(&g_reporter, 0, __ATOMIC_RELEASE);
    return got;
}

void lockup_profile(unsigned cpu, unsigned n, uint64_t gap_ns)
{
    kprintf("cpu %u: %u samples, %llu us apart:\n", cpu, n, (unsigned long long)(gap_ns / 1000));
    for (unsigned i = 0; i < n; i++) {
        struct cpu_sample s;
        if (lockup_sample_cpu(cpu, LOCKUP_SAMPLE_TIMEOUT_NS, &s)) {
            kprintf("  pc %p", (void *)s.pc);
            for (unsigned k = 1; k < s.depth && k < 4; k++)
                kprintf("  #%u %p", k, (void *)s.trace[k]);
            kprintf("\n");
        } else {
            kprintf("  (no answer)\n");
        }
        udelay(gap_ns / 1000);
    }
}

static void print_one(unsigned c, const struct cpu_sample *s, bool self)
{
    kprintf("cpu %u: pc %p sp %p (%s, %llu us ago)\n", c, (void *)s->pc, (void *)s->sp,
            self ? "self" : s->nmi ? "nmi" : "ipi", (unsigned long long)(clock_since_ns(s->when_ns) / 1000));
    for (unsigned i = 0; i < s->depth; i++) {
        const char *where = kernel_text_contains(s->trace[i]) ? "" : " (outside kernel text)";
        kprintf("  #%-2u %p%s\n", i, (void *)s->trace[i], where);
    }
    if (s->depth == 0)
        kprintf("  (no frames)\n");
}

void lockup_print_samples(cpumask_t answered)
{
    /* No local `now`: every age below is a stamp another CPU wrote, and
     * clock_since_ns reads the clock itself so the subtraction cannot
     * underflow (docs/audit/next-subsystem-cpu-clock.md). */
    unsigned n = cpu_count();
    for (unsigned c = 0; c < n; c++) {
        struct percpu *pc = percpu_get(c);
        if (pc == NULL || !cpu_online(c))
            continue;
        if (answered & CPUMASK_OF(c)) {
            print_one(c, &pc->sample, (int)c == lockup_reporter());
        } else {
            uint64_t age = clock_since_ns(pc->last_tick_ns);
            kprintf("cpu %u: no answer in %llu ms; last tick %llu ms ago at pc %p\n", c,
                    (unsigned long long)(LOCKUP_SAMPLE_TIMEOUT_NS / 1000000), (unsigned long long)(age / 1000000),
                    (void *)pc->last_tick_pc);
        }
    }
    __atomic_store_n(&g_reporter, 0, __ATOMIC_RELEASE);
}

/* --- the detectors --- */

static void sample_others_and_print(const struct arch_trap_frame *frame, cpumask_t *answered_out)
{
    cpumask_t answered;
    if (lockup_sample_all(frame, LOCKUP_SAMPLE_TIMEOUT_NS, &answered)) {
        lockup_print_samples(answered);
        *answered_out = answered;
    } else {
        kprintf("  sample in progress on cpu %d\n", lockup_reporter());
        *answered_out = 0;
    }
}

static void report_soft(struct percpu *pc, struct arch_trap_frame *frame, uint64_t now)
{
    struct runqueue *rq = pc->rq;
    struct thread *cur = rq->current;
    unsigned runnable = rq->nr_running;
    kwarn("%ssoft lockup: cpu %u running '%s' for %llu ms with %u runnable", g_expected ? "expected " : "",
          pc->cpu_id, cur ? cur->name : "?", (unsigned long long)(pc->stall_ns / 1000000), runnable);
    backtrace_print(frame);
    cpumask_t answered;
    sample_others_and_print(frame, &answered);
    (void)now;
    arch_irq_state_t st = spin_lock_irqsave(&g_stats_lock);
    g_stats.soft_cpu = pc->cpu_id;
    g_stats.soft_runnable = runnable;
    g_stats.soft_pc = arch_trap_frame_pc(frame);
    g_stats.soft_reports++;
    spin_unlock_irqrestore(&g_stats_lock, st);
}

static void report_hard(struct percpu *pc, unsigned target, struct arch_trap_frame *frame)
{
    struct percpu *t = percpu_get(target);
    uint64_t age = clock_since_ns(t->last_tick_ns);
    kwarn("%shard lockup: cpu %u no tick for %llu ms; last tick %llu ms ago at pc %p (seen from cpu %u)",
          g_expected ? "expected " : "", target, (unsigned long long)(pc->watch_stall_ns / 1000000),
          (unsigned long long)(age / 1000000), (void *)t->last_tick_pc, pc->cpu_id);
    cpumask_t answered;
    sample_others_and_print(frame, &answered);
    if (arch_ipi_nmi_capable())
        lockup_profile(target, 8, 250 * 1000);
    arch_irq_state_t st = spin_lock_irqsave(&g_stats_lock);
    g_stats.hard_cpu = pc->cpu_id;
    g_stats.hard_target = target;
    g_stats.hard_stall_ms = pc->watch_stall_ns / 1000000;
    g_stats.hard_tick_age_ms = age / 1000000;
    g_stats.hard_answered = answered;
    g_stats.hard_reports++;
    spin_unlock_irqrestore(&g_stats_lock, st);
}

void lockup_tick(struct arch_trap_frame *frame, uint64_t now)
{
    if (!__atomic_load_n(&g_enabled, __ATOMIC_ACQUIRE))
        return;
    struct percpu *pc = this_cpu();
    struct runqueue *rq = pc->rq;
    if (rq == NULL)
        return;

    /* Soft: this CPU has not switched while something waits. The reads
     * are unlocked: `switches` and `nr_running` change under the
     * run-queue lock another CPU may hold; a torn read is a late report,
     * never a false one, because the counter only grows. */
    uint64_t sw = __atomic_load_n(&rq->switches, __ATOMIC_RELAXED);
    unsigned runnable = __atomic_load_n(&rq->nr_running, __ATOMIC_RELAXED);
    if (rq->current != rq->idle && runnable > 0 && sw == pc->last_switches) {
        pc->stall_ns += TICK_NS;
    } else {
        pc->stall_ns = 0;
        pc->soft_reported = false;
    }
    pc->last_switches = sw;
    if (pc->stall_ns >= g_soft_ns && !pc->soft_reported) {
        pc->soft_reported = true;
        report_soft(pc, frame, now);
    }

    /* Hard: the watched CPU has stopped ticking. */
    unsigned target = lockup_watch_target(cpu_online_mask(), pc->cpu_id);
    if (target != pc->watch_target) {
        pc->watch_target = target;
        pc->watch_ticks = target == pc->cpu_id ? 0 : __atomic_load_n(&percpu_get(target)->ticks, __ATOMIC_RELAXED);
        pc->watch_stall_ns = 0;
        pc->hard_reported = false;
        return;
    }
    if (target == pc->cpu_id)
        return;
    uint64_t ticks = __atomic_load_n(&percpu_get(target)->ticks, __ATOMIC_RELAXED);
    if (ticks != pc->watch_ticks) {
        pc->watch_ticks = ticks;
        pc->watch_stall_ns = 0;
        pc->hard_reported = false;
    } else {
        pc->watch_stall_ns += TICK_NS;
    }
    if (pc->watch_stall_ns >= g_hard_ns && !pc->hard_reported) {
        pc->hard_reported = true;
        report_hard(pc, target, frame);
    }
}

void lockup_init(void)
{
    __atomic_store_n(&g_enabled, true, __ATOMIC_RELEASE);
    kdebug("lockup: detectors on (soft %llu s, hard %llu s)", (unsigned long long)(g_soft_ns / NS_PER_SEC),
           (unsigned long long)(g_hard_ns / NS_PER_SEC));
}

void lockup_get_stats(struct lockup_stats *out)
{
    /* The reports' fields under their lock; the two sample counters live
     * outside the struct and are read atomically, so no field is read two
     * ways at once. */
    arch_irq_state_t st = spin_lock_irqsave(&g_stats_lock);
    *out = g_stats;
    spin_unlock_irqrestore(&g_stats_lock, st);
    out->samples = __atomic_load_n(&g_samples, __ATOMIC_RELAXED);
    out->samples_busy = __atomic_load_n(&g_samples_busy, __ATOMIC_RELAXED);
}

void lockup_set_thresholds(uint64_t soft_ns, uint64_t hard_ns, bool expected)
{
    g_soft_ns = soft_ns ? soft_ns : LOCKUP_SOFT_NS_DEFAULT;
    g_hard_ns = hard_ns ? hard_ns : LOCKUP_HARD_NS_DEFAULT;
    g_expected = expected;
}
