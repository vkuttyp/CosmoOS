/*
 * timer.c - Clock, tick, and per-CPU timer queues.
 */

#include <kernel/errno.h>
#include <kernel/interrupt.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/string.h>
#include <kernel/timer.h>

#include <arch/cpu.h>
#include <arch/irq.h>
#include <arch/trap.h>
#include <arch/timer.h>

static struct timer_queue g_queues[CONFIG_MAX_CPUS];
static uint64_t g_clock_base;
static uint64_t g_clock_hz;
static uint64_t g_realtime_offset_ns;   /* wall time at monotonic zero */
static uint64_t g_ns_mult;   /* (1e9 << CLOCK_SHIFT) / hz */
static timer_tick_hook_fn g_tick_hook;
static bool g_initialized;
/* Measured at AP bring-up by this unit's step 4; zero until then, and
 * zero on architectures whose counter is common to every PE. */
static uint64_t g_worst_offset_ns;
/* False when the counter is not comparable across CPUs at all, which is
 * a different statement from a large measured offset. */
static bool g_clock_common = true;

#define CLOCK_SHIFT 32

/* --- clock --- */

#if CONFIG_DEBUG
/*
 * A machine whose counters disagree, on demand
 * (docs/audit/next-subsystem-cpu-clock.md).
 *
 * Every machine this project boots on has counters that agree, so the
 * cross-CPU tests would pass on a clock that was completely broken.
 * These make the skew a thing the test creates rather than a thing the
 * hardware has to supply. Debug builds only: nothing reads them in a
 * release image, and no shipping path sets them.
 */
static int64_t g_test_cpu_offset_ns[CONFIG_MAX_CPUS];

void clock_test_set_cpu_offset_ns(unsigned cpu, int64_t ns)
{
    if (cpu < CONFIG_MAX_CPUS)
        __atomic_store_n(&g_test_cpu_offset_ns[cpu], ns, __ATOMIC_RELEASE);
}

void clock_test_set_worst_offset_ns(uint64_t ns)
{
    __atomic_store_n(&g_worst_offset_ns, ns, __ATOMIC_RELEASE);
}
#endif

uint64_t clock_now_ns(void)
{
    if (!g_initialized)
        return 0;
    uint64_t delta = arch_clock_read() - g_clock_base;
    /* Fixed-point: ns = delta * (1e9 / hz). A 128-bit multiply and shift
     * compiles inline; a 128-bit divide would need a runtime library the
     * kernel does not link. Relative error is below 1e-9. */
    unsigned __int128 ns = (unsigned __int128)delta * g_ns_mult;
    uint64_t now = (uint64_t)(ns >> CLOCK_SHIFT);
#if CONFIG_DEBUG
    now = (uint64_t)((int64_t)now + __atomic_load_n(&g_test_cpu_offset_ns[arch_cpu_id()], __ATOMIC_ACQUIRE));
#endif
    return now;
}

/*
 * See the contract in timer.h. A stamp from the future is residual skew,
 * not an interval: the caller gets zero rather than a number with
 * nineteen digits in it (docs/audit/next-subsystem-cpu-clock.md).
 */
uint64_t clock_since_ns(uint64_t stamp)
{
    return clock_delta_ns(clock_now_ns(), stamp);
}

uint64_t clock_worst_offset_ns(void)
{
    return __atomic_load_n(&g_worst_offset_ns, __ATOMIC_ACQUIRE);
}

/*
 * Record the verdict and everything that follows from it, in one place,
 * so the boot and the test that exercises the gate cannot drift apart:
 * a counter that is not common advertises no bound at all, and says so.
 */
static void clock_apply_commonality(bool common, const char *why)
{
    g_clock_common = common;
    if (!common) {
        __atomic_store_n(&g_worst_offset_ns, CLOCK_OFFSET_UNBOUNDED, __ATOMIC_RELEASE);
        kwarn("timer: %s is not comparable across CPUs: %s", arch_clock_name(), why ? why : "unknown");
        kwarn("timer: timestamps stay monotonic per CPU; a difference between two CPUs' readings is not an interval");
        return;
    }
    if (__atomic_load_n(&g_worst_offset_ns, __ATOMIC_ACQUIRE) == CLOCK_OFFSET_UNBOUNDED)
        __atomic_store_n(&g_worst_offset_ns, 0u, __ATOMIC_RELEASE);
    kinfo("timer: %s is common to every CPU; cross-CPU timestamps differ by at most %llu ns",
          arch_clock_name(), (unsigned long long)clock_worst_offset_ns());
}

#if CONFIG_DEBUG
/*
 * Drive the gate as a machine without an invariant TSC would, then put
 * it back. What this can and cannot show is worth being exact about: the
 * CPUID read itself cannot be tested on a machine whose bit is set, so
 * what is tested is everything downstream of the answer -- that a
 * "no" reaches `clock_is_common`, empties the advertised bound, and
 * stops the cross-CPU tests making a claim. That is the part that can
 * silently rot; the bit read is one line in arch code.
 */
void clock_test_force_uncommon(bool on)
{
    if (on) {
        clock_apply_commonality(false, "forced by clock-invariant-gate");
        return;
    }
    const char *why = NULL;
    __atomic_store_n(&g_worst_offset_ns, 0u, __ATOMIC_RELEASE);
    clock_apply_commonality(arch_clock_is_common(&why), why);
}
#endif

uint64_t clock_realtime_ns(void)
{
    return g_realtime_offset_ns + clock_now_ns();
}

uint64_t clock_hz(void)
{
    return g_clock_hz;
}

const char *clock_name(void)
{
    return arch_clock_name();
}

void ndelay(uint64_t ns)
{
    uint64_t end = clock_now_ns() + ns;
    while (clock_now_ns() < end)
        arch_cpu_relax();
}

void udelay(uint64_t us)
{
    ndelay(us * 1000);
}

/* --- timer queue --- */

static struct timer_queue *local_queue(void)
{
    return this_cpu()->timers;
}

void timer_setup(struct timer *t, timer_fn fn, void *arg)
{
    list_init(&t->link);
    t->expires_ns = 0;
    t->fn = fn;
    t->arg = arg;
    t->cpu = 0;
    t->state = TIMER_IDLE;
}

void timer_start(struct timer *t, uint64_t delay_ns)
{
    KASSERT(g_initialized);
    KASSERT(t->fn != NULL);

    arch_irq_state_t s = arch_irq_save();
    struct timer_queue *q = local_queue();
    spin_lock(&q->lock);

    /* IDLE is the normal case. RUNNING means the callback is executing
     * and is re-arming its own timer, which is allowed: run_expired
     * leaves a timer alone after the callback when it is no longer
     * RUNNING. PENDING is a double start and a bug. */
    if (t->state == TIMER_PENDING)
        panic("timer_start: timer %p is already pending", (void *)t);

    /* A zero delay would expire at "now", which can equal the time
     * run_expired captured for the current pass; a callback re-arming
     * with 0 would then be popped again inside the same pass, forever.
     * One nanosecond puts every re-arm into a later pass. */
    t->expires_ns = clock_now_ns() + (delay_ns == 0 ? 1 : delay_ns);
    t->cpu = arch_cpu_id();
    t->state = TIMER_PENDING;

    /* Sorted insert, ascending expiry; ties keep FIFO order. */
    struct timer *it;
    struct list_node *pos = &q->pending;
    list_for_each_entry(it, &q->pending, link) {
        if (it->expires_ns > t->expires_ns) {
            pos = &it->link;
            break;
        }
    }
    list_insert_before(pos, &t->link);
    q->count++;

    spin_unlock(&q->lock);
    arch_irq_restore(s);
}

static bool cancel_locked(struct timer_queue *q, struct timer *t)
{
    bool was_pending = t->state == TIMER_PENDING;
    if (was_pending) {
        list_remove(&t->link);
        q->count--;
        t->state = TIMER_IDLE;
    }
    return was_pending;
}

bool timer_cancel(struct timer *t)
{
    if (t->cpu >= CONFIG_MAX_CPUS)
        return false;
    struct timer_queue *q = &g_queues[t->cpu];

    arch_irq_state_t s = spin_lock_irqsave(&q->lock);
    bool was_pending = cancel_locked(q, t);
    spin_unlock_irqrestore(&q->lock, s);
    return was_pending;
}

void quiesce_count_timer_wait(void);   /* quiesce.c statistics */

#if CONFIG_DEBUG
static unsigned g_test_cancel_spins;
unsigned timer_test_cancel_spins(void) { return __atomic_load_n(&g_test_cancel_spins, __ATOMIC_ACQUIRE); }
void timer_test_reset_cancel_spins(void) { __atomic_store_n(&g_test_cancel_spins, 0u, __ATOMIC_RELEASE); }
#endif

bool timer_cancel_sync(struct timer *t)
{
    if (t->cpu >= CONFIG_MAX_CPUS)
        return false;
    struct timer_queue *q = &g_queues[t->cpu];
    bool was_pending = false;
    bool waited = false;

    /* The callback runs under q->running with the queue lock dropped and
     * takes the lock again when it returns. Holding the lock while
     * q->running != t therefore means the callback is not executing;
     * seeing q->running == t means it is, on another CPU (on this CPU it
     * would have to be interrupt context, which cannot be pre-empted by
     * us: interrupts are masked while we hold the lock). A callback may
     * re-arm itself, so cancel again after every wait. */
    for (;;) {
        arch_irq_state_t s = spin_lock_irqsave(&q->lock);
        was_pending |= cancel_locked(q, t);
        if (q->running != t) {
            spin_unlock_irqrestore(&q->lock, s);
            break;
        }
        if (t->cpu == arch_cpu_id())
            panic("timer_cancel_sync: timer %p cancelled from its own callback", (void *)t);
        spin_unlock_irqrestore(&q->lock, s);
        waited = true;
#if CONFIG_DEBUG
        /* A test waits for this to move before releasing the callback it
         * parked: it says the cancel is really waiting, where the
         * counter below only says it waited once it is over
         * (docs/audit/next-subsystem-lifetime-windows.md). */
        __atomic_fetch_add(&g_test_cancel_spins, 1u, __ATOMIC_ACQ_REL);
#endif
        arch_cpu_relax();
    }
    if (waited)
        quiesce_count_timer_wait();
    return was_pending;
}

static void run_expired(struct timer_queue *q, uint64_t now)
{
    spin_lock(&q->lock);
    while (!list_empty(&q->pending)) {
        struct timer *t = list_first_entry(&q->pending, struct timer, link);
        if (t->expires_ns > now)
            break;
        list_remove(&t->link);
        q->count--;
        t->state = TIMER_RUNNING;
        q->running = t;
        spin_unlock(&q->lock);

        t->fn(t, t->arg);

        spin_lock(&q->lock);
        q->running = NULL;
        if (t->state == TIMER_RUNNING)
            t->state = TIMER_IDLE; /* unless the callback re-armed it */
    }
    spin_unlock(&q->lock);
}

/* --- tick --- */

static void tick_isr(unsigned vector, struct arch_trap_frame *frame, void *arg)
{
    (void)vector;
    (void)arg;

    struct percpu *pc = this_cpu();
    pc->ticks++;

    uint64_t now = clock_now_ns();
    /* The tick sample (kernel/core/lockup.c): what this CPU was doing,
     * and when. Two stores; the frame is already in a register. */
    pc->last_tick_pc = arch_trap_frame_pc(frame);
    pc->last_tick_ns = now;
    run_expired(pc->timers, now);
#if CONFIG_SELFTEST
    /* Local by construction, and the only subtraction in the tree that
     * is: both reads are this CPU's, inside one tick, with interrupts
     * disabled between them. No `clock_since_ns` here -- saturating
     * would hide a backwards counter on a single CPU, which is a bug in
     * the time source rather than the skew this tree tolerates. */
    pc->tick_cost_ns += clock_now_ns() - now;
#endif
    if (g_tick_hook)
        g_tick_hook(now, frame);
}

uint64_t timer_tick_cost_ns(void)
{
    return this_cpu()->tick_cost_ns;
}

void timer_set_tick_hook(timer_tick_hook_fn hook)
{
    g_tick_hook = hook;
}

void timer_init_cpu(void)
{
    struct percpu *pc = this_cpu();
    struct timer_queue *q = &g_queues[pc->cpu_id];
    spinlock_init(&q->lock, "timer_queue");
    list_init(&q->pending);
    q->count = 0;
    q->running = NULL;
    pc->timers = q;
    arch_timer_start_tick(CONFIG_HZ);
}

void timer_init(void)
{
    KASSERT(!g_initialized);

    arch_timer_calibrate();
    g_clock_hz = arch_clock_hz();
    KASSERT(g_clock_hz > 0);
    g_ns_mult = ((uint64_t)NS_PER_SEC << CLOCK_SHIFT) / g_clock_hz;
    g_clock_base = arch_clock_read();
    g_initialized = true;

    int rc = interrupt_register(arch_timer_vector(), tick_isr, NULL, "timer-tick");
    if (rc)
        panic("timer: cannot register tick handler (%d)", rc);

    timer_init_cpu();

    /* The wall clock: whole seconds from the platform's RTC, anchored to
     * the monotonic clock now. Sub-second phase starts at zero. */
    uint64_t epoch = 0;
    if (arch_rtc_read_epoch(&epoch)) {
        g_realtime_offset_ns = epoch * NS_PER_SEC - clock_now_ns();
        kinfo("timer: wall clock %llu s since 1970 from the RTC", (unsigned long long)epoch);
    } else {
        kwarn("timer: no real-time clock; the wall clock starts at 1970");
    }

    kinfo("timer: %s at %llu.%03llu MHz, tick %u Hz", arch_clock_name(),
          (unsigned long long)(g_clock_hz / 1000000), (unsigned long long)((g_clock_hz / 1000) % 1000),
          CONFIG_HZ);

    /*
     * Whether this counter is a clock two CPUs may compare. The offset
     * itself is measured at AP bring-up (step 4); this is the prior
     * question, and until this unit nothing in the tree asked it.
     */
    const char *why_buf = NULL;
    clock_apply_commonality(arch_clock_is_common(&why_buf), why_buf);
}

bool clock_is_common(void)
{
    return g_clock_common;
}

uint64_t timer_ticks(void)
{
    return this_cpu()->ticks;
}

unsigned timer_pending_count(void)
{
    struct timer_queue *q = local_queue();
    arch_irq_state_t s = spin_lock_irqsave(&q->lock);
    unsigned n = q->count;
    spin_unlock_irqrestore(&q->lock, s);
    return n;
}

/* Module ABI v1 exports (docs/kernel/module/api.md). */
#include <kernel/module.h>
EXPORT_SYMBOL(clock_now_ns);
EXPORT_SYMBOL(timer_setup);
EXPORT_SYMBOL(timer_start);
EXPORT_SYMBOL(timer_cancel);
EXPORT_SYMBOL(timer_cancel_sync);
EXPORT_SYMBOL(ndelay);
EXPORT_SYMBOL(udelay);
