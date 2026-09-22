/*
 * lockuptest.c - Self-tests of the lockup unit (docs/kernel/diagnostics/
 * testing.md, "Lockups"): a spinner in a known function on a known CPU,
 * whose sampled program counter must lie in that function; the same
 * with interrupts masked, with the outcome stated per architecture; the
 * single-reporter rule; the two detectors against a lowered threshold;
 * and the quiet control that must report nothing.
 */

#include <kernel/lockup.h>

#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/percpu.h>
#include <kernel/printf.h>
#include <kernel/sched.h>
#include <kernel/selftest.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/timer.h>

#include <arch/cpu.h>
#include <arch/irq.h>
#include <arch/irqc.h>

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            *reason = "check failed: " #cond " at line " STR(__LINE__);        \
            return false;                                                      \
        }                                                                      \
    } while (0)
#define STR_(x) #x
#define STR(x)  STR_(x)

/*
 * "Every thread this test made is gone" is an EVENTUAL condition, and
 * asserting it the instant a join returns was wrong.
 * `thread_join` returns on the exiting thread's `complete(&self->exited)`;
 * the count falls in `thread_unregister`, which runs from the LAST
 * `thread_put` -- and the exiting thread's own reference is dropped by
 * the reaper after it has switched away (kernel/scheduler/thread.c). So
 * between a join returning and the count falling there is a window the
 * implementation genuinely has, and six checks here had no allowance for
 * it. It fired for the first time on 2026-09-20, on the branch that put
 * a thread-creating self-test immediately before these
 * (`virtio-remove-inflight`), which is a change of timing and not of
 * mechanism: the window was always there.
 *
 * Waiting for the condition is no weaker than asserting it -- the bound
 * is finite, so a test that really leaks a thread still fails -- and it
 * is the repair `docs/testing/flakes.md` prescribes for this shape,
 * already made once for `lxtest`'s tgkill-after-join.
 */
static bool threads_settled(unsigned before)
{
    uint64_t deadline = clock_now_ns() + 1000ull * 1000000ull;
    while (thread_count() != before) {
        if (clock_now_ns() > deadline)
            return false;
        sched_yield();   /* the reaper needs the CPU this loop is on */
    }
    return true;
}

/* The spinner is a dozen instructions at -O1 on either architecture; a
 * compile that grew it past this bound fails the range check loudly
 * rather than letting a wrong PC pass. */
#define SPIN_FN_BOUND 128u
#define MAIN_FN_BOUND 512u

struct spinner {
    volatile bool stop;
    volatile bool running;          /* set inside spin_here, so the PC after it is in the loop */
    volatile bool masked;           /* interrupts off while spinning */
    int priority;
    unsigned cpu;
};

/* No arch_cpu_relax in the loop: it is a call, and a sample taken while
 * the CPU is inside it names that function, not this one. */
static __noinline void spin_here(struct spinner *s)
{
    s->running = true;
    while (!s->stop)
        ;
}

static __noinline void spinner_main(void *arg)
{
    struct spinner *s = arg;
    if (s->masked) {
        arch_irq_state_t st = arch_irq_save();
        spin_here(s);
        arch_irq_restore(st);
    } else {
        spin_here(s);
    }
    /* Not a tail call: the store after the call keeps this frame on the
     * stack, so the sample's second entry is a return into here. */
    s->running = false;
}

static bool in_fn(uintptr_t pc, const void *fn, unsigned bound)
{
    return pc >= (uintptr_t)fn && pc < (uintptr_t)fn + bound;
}

static struct thread *start_spinner(struct spinner *s, unsigned cpu, int prio, bool masked)
{
    memset(s, 0, sizeof(*s));
    s->cpu = cpu;
    s->priority = prio;
    s->masked = masked;
    struct thread *t = thread_create_on(spinner_main, s, "lockup-spin", prio, CPUMASK_OF(cpu));
    if (t == NULL)
        return NULL;
    while (!s->running)
        arch_cpu_relax();
    return t;
}

static void stop_spinner(struct spinner *s, struct thread *t)
{
    s->stop = true;
    thread_join(t);
}

/* The CPU whose watch target is `k`: the inverse of the rule, by search. */
static unsigned watcher_of(unsigned k)
{
    cpumask_t online = cpu_online_mask();
    for (unsigned c = 0; c < cpu_count(); c++) {
        if ((online & CPUMASK_OF(c)) && c != k && lockup_watch_target(online, c) == k)
            return c;
    }
    return k;
}

/* A CPU other than the caller's, online. */
static int other_cpu(void)
{
    unsigned me = arch_cpu_id(), n = cpu_count();
    for (unsigned i = 1; i < n; i++) {
        unsigned c = (me + i) % n;
        if (cpu_online(c))
            return (int)c;
    }
    return -1;
}

static bool skip(const char *test)
{
    kinfo("selftest: %s: needs two online CPUs; skipped", test);
    return true;
}

/* --- the sample names the spinner --- */

static bool selftest_lockup_sample_pinned(const char **reason)
{
    int k = other_cpu();
    if (k < 0)
        return skip("lockup-sample");
    unsigned before = thread_count();
    struct spinner s;
    struct thread *t = start_spinner(&s, (unsigned)k, SCHED_PRIO_DEFAULT, false);
    CHECK(t != NULL);

    uint64_t t0 = clock_now_ns();
    cpumask_t m = 0;
    bool ok = lockup_sample_all(NULL, LOCKUP_SAMPLE_TIMEOUT_NS, &m);
    uint64_t t1 = clock_now_ns();
    const struct cpu_sample *sm = &percpu_get((unsigned)k)->sample;
    /* Read the fields before the print releases the slot. */
    uintptr_t pc = sm->pc, tr1 = sm->depth > 1 ? sm->trace[1] : 0;
    unsigned depth = sm->depth;
    uint64_t when = sm->when_ns;
    bool own = (m & CPUMASK_OF(arch_cpu_id())) != 0;
    if (ok)
        lockup_print_samples(m);
    stop_spinner(&s, t);

    CHECK(ok);
    CHECK(m & CPUMASK_OF((unsigned)k));
    CHECK(own);
    CHECK(in_fn(pc, (const void *)spin_here, SPIN_FN_BOUND));
    CHECK(depth >= 2);
    CHECK(in_fn(tr1, (const void *)spinner_main, MAIN_FN_BOUND));
    CHECK(when >= t0 && when <= t1);
    kinfo("selftest: lockup-sample: cpu %d answered in %llu us; pc in spin_here, depth %u", k,
          (unsigned long long)((t1 - t0) / 1000), depth);
    CHECK(threads_settled(before));
    return true;
}

/* Pinned to the CPU it starts on for the whole test: the CPUs it names
 * as "here" and "another" are claims about this thread's CPU that must
 * outlive its sleeps (S25). The pin is the affinity the check honours. */
bool selftest_lockup_sample(const char **reason)
{
    cpumask_t saved = thread_pin_self();
    bool r = selftest_lockup_sample_pinned(reason);
    thread_set_affinity_self(saved);
    return r;
}


/* --- the report's "last tick N ms ago" when a CPU's clock runs ahead --- */

#if CONFIG_DEBUG
struct skewcheck {
    unsigned victim;
    volatile uint64_t stamp;     /* the victim's last tick, as this CPU sees it */
    volatile uint64_t now;       /* this CPU's clock at the same moment */
    volatile uint64_t age;       /* what the report would print */
    volatile bool done;
};

/*
 * Pinned away from the skewed CPU on purpose: the whole question is what
 * a *different* CPU's clock makes of that CPU's timestamp, and a checker
 * that happened to run on the victim would read the same skewed clock
 * and see nothing wrong.
 */
static void skewcheck_main(void *arg)
{
    struct skewcheck *s = arg;
    const struct percpu *pk = percpu_get(s->victim);
    s->stamp = __atomic_load_n(&pk->last_tick_ns, __ATOMIC_RELAXED);
    s->now = clock_now_ns();
    s->age = clock_since_ns(s->stamp);
    __atomic_store_n(&s->done, true, __ATOMIC_RELEASE);
}
#endif

/*
 * The operator's one diagnostic, on a machine whose CPUs disagree.
 *
 * `lockup_print_samples` prints each CPU's "last tick N ms ago" from a
 * timestamp that CPU wrote and this one reads. With a plain subtraction
 * a CPU whose clock runs ahead makes that N about 584 years -- in the
 * one report somebody reads while the machine is wedged, next to the
 * numbers they are trying to act on.
 *
 * The real print path is run here, not just the arithmetic: a version of
 * this that only checked `clock_since_ns` would pass while the report
 * itself still computed the age some other way.
 */
static bool selftest_lockup_report_skew_pinned(const char **reason)
{
#if !CONFIG_DEBUG
    (void)reason;
    return true;
#else
    int k = other_cpu();
    if (k < 0 || k == 0)
        return skip("lockup-report-skew");

    struct skewcheck *s = kzalloc(sizeof(*s));
    CHECK(s != NULL);
    s->victim = (unsigned)k;

    /* Five seconds ahead, then long enough for that CPU to take several
     * ticks and stamp last_tick_ns with the skew in it. */
    /* The victim runs nothing but a spinner of ours for the window: the
     * skew is a lie told to whatever reads the clock on that CPU, and a
     * thread the migrator moves there would read it too (S26). Ticks go
     * on, which is what stamps last_tick_ns with the lie. */
    struct spinner sp;
    struct thread *hold = start_spinner(&sp, (unsigned)k, SCHED_PRIO_DEFAULT - 1, false);
    CHECK(hold != NULL);
    clock_test_set_cpu_offset_ns((unsigned)k, 5ll * 1000 * 1000 * 1000);
    thread_sleep_ms(50);

    struct thread *t = thread_create_on(skewcheck_main, s, "lockup-skew", SCHED_PRIO_DEFAULT,
                                        cpu_online(0) ? CPUMASK_OF(0) : CPUMASK_ALL & ~CPUMASK_OF((unsigned)k));
    bool spawned = t != NULL;
    if (spawned)
        thread_join(t);

    /* The report itself, with the skew still in place. */
    cpumask_t m = 0;
    bool sampled = lockup_sample_all(NULL, LOCKUP_SAMPLE_TIMEOUT_NS, &m);
    if (sampled)
        lockup_print_samples(m);

    clock_test_set_cpu_offset_ns((unsigned)k, 0);
    stop_spinner(&sp, hold);

    uint64_t stamp = s->stamp, now = s->now, age = s->age;
    kfree(s);

    CHECK(spawned);
    /* The premise: from an unskewed CPU, that stamp really is in the
     * future. Without this the next two assertions are vacuous. */
    if (stamp <= now) {
        kerror("selftest: lockup-report-skew: CPU %d's last tick (%llu) is not ahead of this CPU's clock (%llu); the skew did not land",
               k, (unsigned long long)stamp, (unsigned long long)now);
        *reason = "the injected skew did not reach the victim's tick stamp";
        return false;
    }
    CHECK(stamp - now > 4ull * 1000 * 1000 * 1000);
    CHECK(age == 0);

    kinfo("selftest: lockup-report-skew: CPU %d's last tick reads %llu ms in the future from another CPU; the report ages it at 0 ms, not 584 years",
          k, (unsigned long long)((stamp - now) / 1000000));
    return true;
#endif
}

/* Pinned to the CPU it starts on for the whole test: the CPUs it names
 * as "here" and "another" are claims about this thread's CPU that must
 * outlive its sleeps (S25). The pin is the affinity the check honours. */
bool selftest_lockup_report_skew(const char **reason)
{
    cpumask_t saved = thread_pin_self();
    bool r = selftest_lockup_report_skew_pinned(reason);
    thread_set_affinity_self(saved);
    return r;
}


/* --- a spinner with interrupts masked: the outcome per architecture --- */

static bool selftest_lockup_sample_irqoff_pinned(const char **reason)
{
    int k = other_cpu();
    if (k < 0)
        return skip("lockup-sample-irqoff");
    unsigned before = thread_count();
    struct spinner s;
    struct thread *t = start_spinner(&s, (unsigned)k, SCHED_PRIO_DEFAULT, true);
    CHECK(t != NULL);
    thread_sleep_ms(20);            /* 20 ms into the mask: at least four ticks missed */

    cpumask_t m = 0;
    bool ok = lockup_sample_all(NULL, LOCKUP_SAMPLE_TIMEOUT_NS, &m);
    const struct percpu *pk = percpu_get((unsigned)k);
    bool answered = (m & CPUMASK_OF((unsigned)k)) != 0;
    bool nmi = pk->sample.nmi;
    uintptr_t pc = pk->sample.pc;
    uint64_t tick_age = clock_since_ns(pk->last_tick_ns);
    if (ok)
        lockup_print_samples(m);

    /* Then it restores, and the next sample answers on both. */
    stop_spinner(&s, t);
    cpumask_t m2 = 0;
    bool ok2 = lockup_sample_all(NULL, LOCKUP_SAMPLE_TIMEOUT_NS, &m2);
    if (ok2)
        lockup_print_samples(m2);

    CHECK(ok);
    CHECK(tick_age >= 15 * 1000 * 1000);   /* the tick sample is from before the mask */
    if (arch_ipi_nmi_capable()) {
        CHECK(answered);
        CHECK(nmi);
        CHECK(in_fn(pc, (const void *)spin_here, SPIN_FN_BOUND));
        kinfo("selftest: lockup-sample-irqoff: nmi answered through the mask; tick sample %llu ms old",
              (unsigned long long)(tick_age / 1000000));
    } else {
        CHECK(!answered);
        kinfo("selftest: lockup-sample-irqoff: no answer through the mask (no NMI); tick sample %llu ms old",
              (unsigned long long)(tick_age / 1000000));
    }
    CHECK(ok2);
    CHECK(m2 & CPUMASK_OF((unsigned)k));
    CHECK(threads_settled(before));
    return true;
}

/* Pinned to the CPU it starts on for the whole test: the CPUs it names
 * as "here" and "another" are claims about this thread's CPU that must
 * outlive its sleeps (S25). The pin is the affinity the check honours. */
bool selftest_lockup_sample_irqoff(const char **reason)
{
    cpumask_t saved = thread_pin_self();
    bool r = selftest_lockup_sample_irqoff_pinned(reason);
    thread_set_affinity_self(saved);
    return r;
}


/* --- one reporter at a time, and nobody waits for it --- */

struct racer {
    volatile bool *go;
    bool ok;
    cpumask_t mask;
    uint64_t elapsed_ns;
};

static void racer_main(void *arg)
{
    struct racer *r = arg;
    while (!*r->go)
        arch_cpu_relax();
    uint64_t t0 = clock_now_ns();
    r->ok = lockup_sample_all(NULL, LOCKUP_SAMPLE_TIMEOUT_NS, &r->mask);
    r->elapsed_ns = clock_since_ns(t0);
    if (r->ok) {
        /* Hold the slot long enough for the loser to have asked. */
        uint64_t until = clock_now_ns() + 2 * 1000 * 1000;
        while (clock_now_ns() < until)
            arch_cpu_relax();
        lockup_print_samples(r->mask);
    }
}

static bool selftest_lockup_sample_busy_pinned(const char **reason)
{
    unsigned n = cpu_count();
    if (n < 2)
        return skip("lockup-sample-busy");
    unsigned before = thread_count();
    struct lockup_stats s0, s1;
    lockup_get_stats(&s0);

    volatile bool go = false;
    struct racer r[2];
    struct thread *t[2];
    unsigned cpus[2] = { 0, 1 };
    for (int i = 0; i < 2; i++) {
        memset(&r[i], 0, sizeof(r[i]));
        r[i].go = &go;
        t[i] = thread_create_on(racer_main, &r[i], "lockup-racer", SCHED_PRIO_DEFAULT, CPUMASK_OF(cpus[i]));
        CHECK(t[i] != NULL);
    }
    thread_sleep_ms(5);             /* both are spinning on the flag */
    go = true;
    thread_join(t[0]);
    thread_join(t[1]);
    lockup_get_stats(&s1);

    CHECK(r[0].ok != r[1].ok);      /* exactly one got the slot */
    const struct racer *loser = r[0].ok ? &r[1] : &r[0];
    const struct racer *winner = r[0].ok ? &r[0] : &r[1];
    CHECK(loser->mask == 0);
    /* Refused at once: far under the winner's 2 ms hold, which is what a
     * loser that waited for the slot would take (an interrupt landing on
     * the loser's CPU in between costs tens of microseconds under TCG). */
    CHECK(loser->elapsed_ns < 1000 * 1000);
    CHECK(winner->elapsed_ns < LOCKUP_SAMPLE_TIMEOUT_NS + 2 * 1000 * 1000);
    CHECK(s1.samples == s0.samples + 1 && s1.samples_busy == s0.samples_busy + 1);
    kinfo("selftest: lockup-sample-busy: winner %llu us, loser refused in %llu us",
          (unsigned long long)(winner->elapsed_ns / 1000), (unsigned long long)(loser->elapsed_ns / 1000));

    /* The bound is total, not per target: two CPUs that cannot answer
     * (interrupts masked; on x86-64 the NMI answers anyway) cost one
     * timeout together, not one each. Needs three CPUs. */
    if (n >= 3) {
        unsigned me = arch_cpu_id();
        unsigned a = (me + 1) % n, b = (me + 2) % n;
        struct spinner sa, sb;
        struct thread *ta = start_spinner(&sa, a, SCHED_PRIO_DEFAULT, true);
        struct thread *tb = start_spinner(&sb, b, SCHED_PRIO_DEFAULT, true);
        CHECK(ta != NULL && tb != NULL);
        uint64_t t0 = clock_now_ns();
        cpumask_t m = 0;
        bool ok = lockup_sample_all(NULL, LOCKUP_SAMPLE_TIMEOUT_NS, &m);
        uint64_t el = clock_since_ns(t0);
        if (ok)
            lockup_print_samples(m);
        /* Both stop before either is joined: a join frees a stack, and
         * that TLB shootdown waits for every CPU's acknowledgement,
         * which a CPU with interrupts masked cannot give. */
        sa.stop = true;
        sb.stop = true;
        thread_join(ta);
        thread_join(tb);
        CHECK(ok);
        CHECK(el < LOCKUP_SAMPLE_TIMEOUT_NS + 2 * 1000 * 1000);
        kinfo("selftest: lockup-sample-busy: two masked targets, one bound: %llu us", (unsigned long long)(el / 1000));
    }
    CHECK(threads_settled(before));
    return true;
}

/* Pinned to the CPU it starts on for the whole test: the CPUs it names
 * as "here" and "another" are claims about this thread's CPU that must
 * outlive its sleeps (S25). The pin is the affinity the check honours. */
bool selftest_lockup_sample_busy(const char **reason)
{
    cpumask_t saved = thread_pin_self();
    bool r = selftest_lockup_sample_busy_pinned(reason);
    thread_set_affinity_self(saved);
    return r;
}


/* --- soft lockup: no switch while something waits --- */

static void victim_main(void *arg)
{
    (void)arg;
}

static bool wait_reports(uint64_t *field_now, uint64_t want, bool soft, uint64_t budget_ms)
{
    struct lockup_stats st;
    uint64_t until = clock_now_ns() + budget_ms * 1000 * 1000;
    for (;;) {
        lockup_get_stats(&st);
        *field_now = soft ? st.soft_reports : st.hard_reports;
        if (*field_now >= want)
            return true;
        if (clock_now_ns() >= until)
            return false;
        thread_sleep_ms(5);
    }
}

static bool selftest_lockup_soft_pinned(const char **reason)
{
    int k = other_cpu();
    if (k < 0)
        return skip("lockup-soft");
    unsigned before = thread_count();
    struct lockup_stats s0, s1;
    lockup_get_stats(&s0);
    lockup_set_thresholds(200ull * 1000 * 1000, 0, true);

    /* A priority-16 spinner on CPU k, and a default-priority thread
     * created there that can never run while it spins. */
    struct spinner s;
    struct thread *t = start_spinner(&s, (unsigned)k, 16, false);
    struct thread *v = t ? thread_create_on(victim_main, NULL, "lockup-victim", SCHED_PRIO_DEFAULT, CPUMASK_OF((unsigned)k)) : NULL;
    uint64_t reports = 0;
    bool fired = t != NULL && v != NULL && wait_reports(&reports, s0.soft_reports + 1, true, 1000);
    lockup_get_stats(&s1);
    /* The episode continues: no second report for the same stall. */
    thread_sleep_ms(300);
    struct lockup_stats s2;
    lockup_get_stats(&s2);
    if (t != NULL)
        stop_spinner(&s, t);
    if (v != NULL)
        thread_join(v);
    lockup_set_thresholds(0, 0, false);

    CHECK(t != NULL && v != NULL);
    CHECK(fired);
    CHECK(s1.soft_reports == s0.soft_reports + 1);
    CHECK(s1.soft_cpu == (unsigned)k);
    /* At least the victim this test queued behind the spinner: a migrator
     * may have queued others there too (S26), and that is not a defect. */
    CHECK(s1.soft_runnable >= 1);
    CHECK(in_fn(s1.soft_pc, (const void *)spin_here, SPIN_FN_BOUND));
    CHECK(s2.soft_reports == s1.soft_reports);
    CHECK(s2.hard_reports == s0.hard_reports);
    CHECK(threads_settled(before));
    return true;
}

/* Pinned to the CPU it starts on for the whole test: the CPUs it names
 * as "here" and "another" are claims about this thread's CPU that must
 * outlive its sleeps (S25). The pin is the affinity the check honours. */
bool selftest_lockup_soft(const char **reason)
{
    cpumask_t saved = thread_pin_self();
    bool r = selftest_lockup_soft_pinned(reason);
    thread_set_affinity_self(saved);
    return r;
}


/* --- hard lockup: the watched CPU stopped ticking --- */

static bool selftest_lockup_hard_pinned(const char **reason)
{
    int k = other_cpu();
    if (k < 0)
        return skip("lockup-hard");
    unsigned before = thread_count();
    struct lockup_stats s0, s1, s2;
    lockup_get_stats(&s0);
    lockup_set_thresholds(0, 200ull * 1000 * 1000, true);

    struct spinner s;
    struct thread *t = start_spinner(&s, (unsigned)k, SCHED_PRIO_DEFAULT, true);
    uint64_t reports = 0;
    uint64_t t0 = clock_now_ns();
    /* The budget and the hold after the report together stay under the
     * TLB shootdown's one-second acknowledgement bound: a masked spinner
     * that outlived it would panic the kernel. */
    bool fired = t != NULL && wait_reports(&reports, s0.hard_reports + 1, false, 600);
    uint64_t fired_after = clock_since_ns(t0);
    lockup_get_stats(&s1);
    uintptr_t pc = percpu_get((unsigned)k)->sample.pc;
    bool nmi = percpu_get((unsigned)k)->sample.nmi;
    /* The episode continues 300 ms: one report, not one per tick. */
    thread_sleep_ms(300);
    struct lockup_stats s1b;
    lockup_get_stats(&s1b);
    if (t != NULL)
        stop_spinner(&s, t);
    /* Ticks resume; a second stretch of normal running reports nothing. */
    thread_sleep_ms(600);
    lockup_get_stats(&s2);
    lockup_set_thresholds(0, 0, false);

    CHECK(t != NULL);
    CHECK(fired);
    CHECK(s1.hard_reports == s0.hard_reports + 1);
    CHECK(s1.hard_target == (unsigned)k);
    CHECK(s1.hard_cpu == watcher_of((unsigned)k));
    CHECK(s1.hard_stall_ms >= 200 && s1.hard_stall_ms < 400);
    CHECK(fired_after < 600ull * 1000 * 1000);        /* the check runs every tick, not once a second */
    if (arch_ipi_nmi_capable()) {
        CHECK(s1.hard_answered & CPUMASK_OF((unsigned)k));
        CHECK(nmi);
        CHECK(in_fn(pc, (const void *)spin_here, SPIN_FN_BOUND));
    } else {
        CHECK(!(s1.hard_answered & CPUMASK_OF((unsigned)k)));
        /* The stall is counted in the watcher's ticks; the target's last
         * tick may sit up to one tick earlier in phase. */
        CHECK(s1.hard_tick_age_ms >= 200 - TICK_NS / 1000000);
    }
    CHECK(s1b.hard_reports == s1.hard_reports);
    CHECK(s2.hard_reports == s1.hard_reports);
    CHECK(s2.soft_reports == s0.soft_reports);
    CHECK(threads_settled(before));
    return true;
}

/* Pinned to the CPU it starts on for the whole test: the CPUs it names
 * as "here" and "another" are claims about this thread's CPU that must
 * outlive its sleeps (S25). The pin is the affinity the check honours. */
bool selftest_lockup_hard(const char **reason)
{
    cpumask_t saved = thread_pin_self();
    bool r = selftest_lockup_hard_pinned(reason);
    thread_set_affinity_self(saved);
    return r;
}


/* --- the quiet control: a spinner nobody waits on, and an idle CPU --- */

static bool selftest_lockup_quiet_pinned(const char **reason)
{
    int k = other_cpu();
    if (k < 0)
        return skip("lockup-quiet");
    unsigned before = thread_count();
    struct lockup_stats s0, s1;
    lockup_get_stats(&s0);
    lockup_set_thresholds(200ull * 1000 * 1000, 200ull * 1000 * 1000, true);

    /* Default priority: anything that becomes runnable on CPU k gets its
     * slice, so nothing is starved and nothing is reported. */
    struct spinner s;
    struct thread *t = start_spinner(&s, (unsigned)k, SCHED_PRIO_DEFAULT, false);
    thread_sleep_ms(600);
    if (t != NULL)
        stop_spinner(&s, t);
    thread_sleep_ms(600);           /* every CPU ticking, nothing spinning */
    lockup_get_stats(&s1);
    lockup_set_thresholds(0, 0, false);

    CHECK(t != NULL);
    CHECK(s1.soft_reports == s0.soft_reports);
    CHECK(s1.hard_reports == s0.hard_reports);
    CHECK(threads_settled(before));
    return true;
}

/* Pinned to the CPU it starts on for the whole test: the CPUs it names
 * as "here" and "another" are claims about this thread's CPU that must
 * outlive its sleeps (S25). The pin is the affinity the check honours. */
bool selftest_lockup_quiet(const char **reason)
{
    cpumask_t saved = thread_pin_self();
    bool r = selftest_lockup_quiet_pinned(reason);
    thread_set_affinity_self(saved);
    return r;
}


/* --- the tick's cost, and the two stores' --- */

bool selftest_lockup_tick_bench(const char **reason)
{
    (void)reason;
    /* The two stores the tick gained, a million times: their cost per tick.
     * Raw: a bench on some CPU's two fields, which the tick rewrites anyway. */
    struct percpu *pc = raw_this_cpu();
    uintptr_t save_pc = pc->last_tick_pc;
    uint64_t save_ns = pc->last_tick_ns;
    uint64_t t0 = clock_now_ns();
    for (uint32_t i = 0; i < 1000000; i++) {
        __atomic_store_n(&pc->last_tick_pc, (uintptr_t)i, __ATOMIC_RELAXED);
        __atomic_store_n(&pc->last_tick_ns, t0 + i, __ATOMIC_RELAXED);
    }
    uint64_t t1 = clock_now_ns();
    pc->last_tick_pc = save_pc;
    pc->last_tick_ns = save_ns;
    /* The tick's own cost from entry to the scheduler hook, as the timer
     * records it (timer.c), over the ticks of a 200 ms sleep. */
    uint64_t c0 = timer_tick_cost_ns(), n0 = pc->ticks;
    thread_sleep_ms(200);
    uint64_t c1 = timer_tick_cost_ns(), n1 = pc->ticks;
    kinfo("selftest: lockup-tick-bench: two stores %llu ns per tick; tick entry-to-hook %llu ns mean over %llu ticks",
          (unsigned long long)((t1 - t0) / 1000000), (unsigned long long)(n1 > n0 ? (c1 - c0) / (n1 - n0) : 0),
          (unsigned long long)(n1 - n0));
    return true;
}
