/*
 * timer.c - Clock, tick, and per-CPU timer queues.
 */

#include <kernel/errno.h>
#include <kernel/interrupt.h>
#include <kernel/log.h>
#include <kernel/thread.h>
#include <kernel/sched.h>
#include <kernel/kmalloc.h>
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
static void clock_tick_advance(unsigned me);
/* Per-CPU addends that cancel each CPU's measured offset from CPU 0, and
 * whether they are being applied (they are not, on a counter this kernel
 * may not trust across CPUs). */
static int64_t g_cpu_offset_ns[CONFIG_MAX_CPUS];
static unsigned g_measured;   /* CPUs whose offset was actually measured */
/* The bound the measurement produced, kept whether or not it was
 * applied: on a machine that measures and then declines to trust the
 * result, this is still the number that was computed, and the only way
 * to check the computation on such a machine. */
static uint64_t g_measured_bound_ns;
/* Set once, after every CPU is online and the measurement has run: see
 * clock_now_ns for why this gates a per-CPU access rather than an add. */
static bool g_apply_offset;

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

/* The counter as this CPU reads it, with no cross-CPU correction: what
 * the measurement itself must use, or it would be measuring the
 * correction it is trying to produce. */
uint64_t clock_raw_ns(void)
{
    if (!g_initialized)
        return 0;
    uint64_t delta = arch_clock_read() - g_clock_base;
    /* Fixed-point: ns = delta * (1e9 / hz). A 128-bit multiply and shift
     * compiles inline; a 128-bit divide would need a runtime library the
     * kernel does not link. Relative error is below 1e-9. */
    unsigned __int128 ns = (unsigned __int128)delta * g_ns_mult;
    return (uint64_t)(ns >> CLOCK_SHIFT);
}

uint64_t clock_now_ns(void)
{
    /*
     * The flag is not here to save the add. `arch_cpu_id()` reads the
     * per-CPU block through GS, so indexing the offsets unconditionally
     * would make every `clock_now_ns` -- including the ones an AP takes
     * partway through its own bring-up, before its block is installed --
     * depend on percpu being up. The flag is set once, after
     * `clock_measure_offsets` has run and every CPU is online, so the
     * per-CPU access happens only when it is certainly safe.
     *
     * Dropping it and relying on the addends being zero was tried and
     * reverted: the values would have been right, and the load to get
     * them would not have been safe.
     */
    uint64_t now = clock_raw_ns();
    /* Raw, and the one place a stale id changes a value: a thread moved
     * between reading its id and reading the counter adds the CPU it
     * left's offset to the CPU it is on's counter. The error is the
     * difference of two offsets, bounded by their half-widths -- the
     * residual skew this tree already tolerates (clock_since_ns) -- and
     * a checked read here would put every clock_now_ns caller under
     * preemption off for nothing better than that. */
    unsigned cpu = raw_cpu_id();
    if (__atomic_load_n(&g_apply_offset, __ATOMIC_ACQUIRE))
        now = (uint64_t)((int64_t)now + g_cpu_offset_ns[cpu]);
#if CONFIG_DEBUG
    now = (uint64_t)((int64_t)now + __atomic_load_n(&g_test_cpu_offset_ns[cpu], __ATOMIC_ACQUIRE));
#endif
    return now;
}

/*
 * The clock the kernel keeps time by: the counter and the measured
 * per-CPU offset, and never the debug builds' injected test offset.
 *
 * The test offset (`clock_test_set_cpu_offset_ns`) is a lie told to
 * *readers* of `clock_now_ns` on one CPU, so that a skewed stamp's
 * handling can be checked. A timer armed against a lying clock and
 * expired against the truth fires late by the lie -- five seconds, in
 * the lockup-report-skew test -- and before threads migrated only the
 * test's own pinned thread could arm one on the victim CPU during the
 * window. Now any thread can be there (S26), so timers, deadlines and
 * delays keep time here, where nothing is injected, and only what reads
 * the clock as a value sees the test's skew.
 */
static uint64_t clock_time_ns(void)
{
    uint64_t now = clock_raw_ns();
    if (__atomic_load_n(&g_apply_offset, __ATOMIC_ACQUIRE))
        now = (uint64_t)((int64_t)now + g_cpu_offset_ns[raw_cpu_id()]);   /* raw: the same bounded error as above */
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

/* --- the machine-wide tick: one counter two CPUs may compare --------------- */

/*
 * A deadline built on one CPU and tested on another cannot use the clock
 * when the counter is not common: the two readings differ by an unbounded
 * amount, and saturating arithmetic does not help a comparison. What is
 * needed is a quantity both CPUs read from the *same* place, and the tick
 * is the only one this kernel has.
 *
 * **One designated CPU advances it**, so it runs at CONFIG_HZ rather than
 * CONFIG_HZ times the CPU count -- which is the first design that was
 * tried here and reverted, because every CPU contributing its own delta
 * makes deadlines expire ncpus times too early. The second reverted
 * design took the maximum of the per-CPU `pc->ticks`, which stalls: those
 * counters do not share an origin, each starting when its CPU comes
 * online, so the leader stopping blocks every follower for the whole of
 * bring-up.
 *
 * **And ownership moves**, which is what makes a single owner safe. A CPU
 * that is not the owner watches the counter; if it has not advanced for
 * `TICK_OWNER_STALE` of that CPU's own ticks -- the owner offline, wedged,
 * or not taking interrupts -- it claims ownership with a compare-exchange.
 * Several may notice at once and exactly one wins. The counter can
 * therefore be late by at most `TICK_OWNER_STALE` ticks across a handover,
 * and cannot stop while any CPU is still ticking.
 *
 * Deliberately *not* fixed by this: a deadline written by hand as
 * `clock_now_ns() + x`. Only the two calls below are safe, which is why
 * every deadline loop in the tree uses them.
 */
#define TICK_OWNER_STALE 4u

static uint64_t g_global_ticks;
static unsigned g_tick_owner;                  /* the CPU that advances it */
static uint64_t g_owner_seen[CONFIG_MAX_CPUS]; /* what a non-owner last saw */
static unsigned g_owner_stale[CONFIG_MAX_CPUS];

static void clock_tick_advance(unsigned me)
{
    if (me >= CONFIG_MAX_CPUS)
        return;
    unsigned owner = __atomic_load_n(&g_tick_owner, __ATOMIC_ACQUIRE);
    if (me == owner) {
        __atomic_fetch_add(&g_global_ticks, 1u, __ATOMIC_RELEASE);
        return;
    }
    uint64_t seen = __atomic_load_n(&g_global_ticks, __ATOMIC_ACQUIRE);
    if (seen != g_owner_seen[me]) {
        g_owner_seen[me] = seen;   /* the owner is alive */
        g_owner_stale[me] = 0;
        return;
    }
    if (++g_owner_stale[me] < TICK_OWNER_STALE)
        return;
    /* The owner has not advanced it for several of this CPU's ticks.
     * Take over; if another CPU got there first the exchange fails and
     * this one goes back to watching. */
    g_owner_stale[me] = 0;
    (void)__atomic_compare_exchange_n(&g_tick_owner, &owner, me, false,
                                      __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
}

/*
 * The instant a deadline is measured against: the corrected clock when
 * every CPU agrees on it, the machine-wide tick when they do not. Both
 * `clock_deadline_ns` and `clock_deadline_passed` read this, so a
 * deadline built on one CPU and tested on another compares the same
 * quantity and migration cannot distort it.
 *
 * Before the first tick there is one CPU and no scheduler, so the clock
 * is exact rather than a compromise.
 */
static uint64_t deadline_now_ns(void)
{
    if (clock_is_common())
        return clock_time_ns();   /* never the test offset: a deadline is a mechanism, not a reading */
    uint64_t ticks = __atomic_load_n(&g_global_ticks, __ATOMIC_ACQUIRE);
    return ticks == 0 ? clock_time_ns() : ticks * TICK_NS;
}

#if CONFIG_DEBUG
uint64_t clock_test_global_ticks(void) { return __atomic_load_n(&g_global_ticks, __ATOMIC_ACQUIRE); }
unsigned clock_test_tick_owner(void) { return __atomic_load_n(&g_tick_owner, __ATOMIC_ACQUIRE); }
void clock_test_set_tick_owner(unsigned cpu) { __atomic_store_n(&g_tick_owner, cpu, __ATOMIC_RELEASE); }
#endif

uint64_t clock_deadline_ns(uint64_t budget_ns)
{
    uint64_t now = deadline_now_ns();
    uint64_t at = now + budget_ns;
    return at < now ? UINT64_MAX : at;   /* a budget so large it wraps never expires */
}

bool clock_deadline_passed(uint64_t deadline)
{
    return deadline_now_ns() >= deadline;
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
    /*
     * The raw clock, not `clock_deadline_ns`: this is a busy-wait of
     * nanoseconds to microseconds on one CPU, far below the 4 ms tick
     * the deadline helpers fall back to when the clock is not common,
     * and it does not sleep, so the thread it runs on is the thread that
     * finishes it.
     */
    uint64_t end = clock_time_ns() + ns;   /* a delay is a mechanism: never the test offset */
    while (clock_time_ns() < end)
        arch_cpu_relax();
}

void udelay(uint64_t us)
{
    ndelay(us * 1000);
}

/* --- timer queue --- */

static struct timer_queue *local_queue(void)
{
    return this_cpu()->timers;   /* checked: every caller arms or cancels with interrupts off */
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
    /*
     * The queue is per-CPU: a timer is armed on one CPU and fired from
     * that CPU's tick, against `clock_now_ns()` (timers_run). So this is
     * a same-CPU deadline, the raw clock is exactly right for it, and
     * there is no migration for the helpers to protect against. The
     * saturation they would have given is kept by hand: a budget large
     * enough to wrap must still not expire at once.
     *
     * When a machine-wide tick counter was briefly the deadline domain,
     * arming here through `clock_deadline_ns` while the tick compared
     * against `clock_now_ns()` made every timer in the kernel fire at
     * once -- `preempt`, `sleep` and `completion` returned in single
     * milliseconds. Kept as a note, because the two look interchangeable.
     */
    uint64_t start = clock_time_ns();   /* the timer clock: never the test offset (see clock_time_ns) */
    uint64_t delay = delay_ns == 0 ? 1 : delay_ns;
    t->expires_ns = start + delay < start ? UINT64_MAX : start + delay;
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
    clock_tick_advance(pc->cpu_id);

    uint64_t now = clock_now_ns();
#if CONFIG_DEBUG
    /* The tick-gap detector: a second without a tick on this CPU is an
     * interrupts-off window neither lockup detector sees (their bar is
     * ten). Named with where the CPU was when interrupts came back. */
    if (pc->last_tick_ns != 0 && clock_delta_ns(now, pc->last_tick_ns) > NS_PER_SEC &&
        __atomic_load_n(&g_test_cpu_offset_ns[pc->cpu_id], __ATOMIC_ACQUIRE) == 0)   /* a test's injected skew reads as a gap */
        kwarn("timer: cpu %u: no tick for %llu ms; interrupts came back at pc %p (last tick interrupted pc %p, thread '%s')",
              pc->cpu_id, (unsigned long long)(clock_delta_ns(now, pc->last_tick_ns) / 1000000),
              (void *)arch_trap_frame_pc(frame), (void *)pc->last_tick_pc,
              pc->current ? pc->current->name : "-");
#endif
    /* The tick sample (kernel/core/lockup.c): what this CPU was doing,
     * and when. Two stores; the frame is already in a register. */
    pc->last_tick_pc = arch_trap_frame_pc(frame);
    pc->last_tick_ns = now;   /* a reading: the skew tests want the lie in this stamp */
    run_expired(pc->timers, clock_time_ns());   /* the timer clock: armed and expired against the same truth */
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
    return raw_this_cpu()->tick_cost_ns;   /* a statistic: some CPU's, for a bench line */
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

/* --- measuring the offset between two CPUs' counters ---------------------- */

/*
 * The classic three-read exchange. CPU 0 reads, the AP reads, CPU 0
 * reads again: the AP's reading was taken somewhere inside that bracket,
 * so its offset from CPU 0 lies within +-(width/2) of the midpoint. Run
 * many times; the narrowest bracket gives both the best estimate and the
 * bound on how wrong it can be.
 *
 * Which is why the bound is the *narrowest half-width* and not the worst
 * offset seen: after the correction is applied, what is left is the
 * uncertainty of the estimate, not the offset it removed.
 */

#define OFFSET_ROUNDS 1000u

struct offmeas {
    volatile unsigned turn;      /* 0: CPU 0's, 1: the AP's, 2: the AP is done */
    volatile uint64_t tb;
    volatile bool ready, stop, stalled;
    volatile unsigned bcpu;
    int64_t offset_ns;           /* the AP's clock minus CPU 0's, best estimate */
    uint64_t halfwidth_ns;       /* how wrong that estimate can be */
    unsigned rounds;
};

static void offmeas_ap(void *arg)
{
    struct offmeas *m = arg;
    m->bcpu = arch_cpu_id();
    __atomic_store_n(&m->ready, true, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&m->stop, __ATOMIC_ACQUIRE)) {
        if (__atomic_load_n(&m->turn, __ATOMIC_ACQUIRE) == 1) {
            m->tb = clock_raw_ns();
            __atomic_store_n(&m->turn, 2u, __ATOMIC_RELEASE);
        }
        arch_cpu_relax();
    }
}

static void offmeas_bsp(void *arg)
{
    struct offmeas *m = arg;
    uint64_t spin0 = clock_raw_ns();
    while (!__atomic_load_n(&m->ready, __ATOMIC_ACQUIRE)) {
        if (clock_raw_ns() - spin0 > NS_PER_SEC) {
            m->stalled = true;
            return;
        }
        arch_cpu_relax();
    }

    uint64_t best = UINT64_MAX;
    for (unsigned r = 0; r < OFFSET_ROUNDS; r++) {
        uint64_t t0 = clock_raw_ns();
        __atomic_store_n(&m->turn, 1u, __ATOMIC_RELEASE);
        spin0 = t0;
        while (__atomic_load_n(&m->turn, __ATOMIC_ACQUIRE) != 2) {
            if (clock_raw_ns() - spin0 > NS_PER_SEC) {
                m->stalled = true;
                return;
            }
            arch_cpu_relax();
        }
        uint64_t t1 = clock_raw_ns();
        uint64_t tb = m->tb;

        uint64_t width = t1 - t0;
        if (width < best) {
            best = width;
            /* midpoint of the bracket, and how far the AP's reading sits
             * from it -- signed, because an AP may run either way. */
            uint64_t mid = t0 + width / 2;
            m->offset_ns = (int64_t)(tb - mid);
            m->halfwidth_ns = width / 2;
        }
        m->rounds++;
        __atomic_store_n(&m->turn, 0u, __ATOMIC_RELEASE);
    }
}

uint64_t clock_resolution_ns(void)
{
    uint64_t hz = g_clock_hz;
    if (hz == 0)
        return 1;
    uint64_t r = (NS_PER_SEC + hz - 1) / hz;
    return r ? r : 1;
}

bool clock_offsets_measured(void)
{
    return g_measured != 0;
}

uint64_t clock_measured_bound_ns(void)
{
    return __atomic_load_n(&g_measured_bound_ns, __ATOMIC_ACQUIRE);
}

void clock_measure_offsets(void)
{
    if (!arch_clock_is_percpu()) {
        /* One counter for the whole system: there is nothing to measure,
         * and a measured correction could only add error. */
        kinfo("timer: %s is one counter for the whole system; no per-CPU offset to measure", arch_clock_name());
        return;
    }
    if (cpu_count() < 2)
        return;

    /*
     * The floor on any bound this can produce: one tick of the counter.
     *
     * The first run of this code reported an uncertainty of +-0 ns,
     * which is not a measurement -- it means the narrowest bracket had
     * width 0, the counter not having advanced across a cross-CPU
     * handshake that certainly took real time. (TCG runs a vCPU in long
     * translated blocks, so a whole exchange can land between two
     * counter values.) An offset cannot be known more precisely than the
     * counter can express, whatever the brackets say, and a bound of
     * zero is a promise no measurement can make.
     */
    uint64_t resolution_ns = clock_resolution_ns();

    int64_t worst_offset = 0;
    uint64_t worst_halfwidth = resolution_ns;
    unsigned measured = 0, wanted = 0, failed = 0;

    for (unsigned c = 1; c < cpu_count(); c++) {
        if (!cpu_online(c))
            continue;
        wanted++;
        struct offmeas *m = kzalloc(sizeof(*m));
        if (m == NULL) {
            kwarn("timer: out of memory measuring CPU %u's offset", c);
            failed++;
            break;
        }
        struct thread *ap = thread_create_on(offmeas_ap, m, "clk-off-ap", SCHED_PRIO_DEFAULT, CPUMASK_OF(c));
        struct thread *bsp = NULL;
        if (ap != NULL)
            bsp = thread_create_on(offmeas_bsp, m, "clk-off-bsp", SCHED_PRIO_DEFAULT, CPUMASK_OF(0));
        if (ap == NULL || bsp == NULL) {
            __atomic_store_n(&m->stop, true, __ATOMIC_RELEASE);
            if (ap)
                thread_join(ap);
            kfree(m);
            failed++;
            kwarn("timer: cannot measure CPU %u's offset: no thread", c);
            continue;
        }
        thread_join(bsp);
        __atomic_store_n(&m->stop, true, __ATOMIC_RELEASE);
        thread_join(ap);

        if (m->stalled || m->bcpu != c || m->rounds == 0) {
            kwarn("timer: CPU %u's offset could not be measured (%u rounds%s)", c, m->rounds,
                  m->stalled ? ", stalled" : "");
            kfree(m);
            failed++;
            continue;
        }
        g_cpu_offset_ns[c] = -m->offset_ns;   /* the addend that cancels it */
        int64_t mag = m->offset_ns < 0 ? -m->offset_ns : m->offset_ns;
        if (mag > worst_offset)
            worst_offset = mag;
        if (m->halfwidth_ns > worst_halfwidth)
            worst_halfwidth = m->halfwidth_ns;
        measured++;
        g_measured++;
        kdebug("timer: CPU %u offset %lld ns, narrowest bracket %llu ns, over %u exchanges", c,
               (long long)m->offset_ns, (unsigned long long)m->halfwidth_ns * 2, m->rounds);
        kfree(m);
    }

    if (measured == 0)
        return;
    __atomic_store_n(&g_measured_bound_ns, worst_halfwidth, __ATOMIC_RELEASE);

    /*
     * Reported whether or not it is applied. On a machine whose counter
     * is not a clock (no invariant TSC) the correction must not be used
     * -- the offset it measured will not stay put -- but the number is
     * still the most informative line this boot can print about its own
     * timekeeping, and nobody has ever printed it.
     */
    kinfo("timer: measured %u CPU offset%s against CPU 0 over %u exchanges each: worst %lld ns, uncertainty +-%llu ns (counter resolution %llu ns)",
          measured, measured == 1 ? "" : "s", OFFSET_ROUNDS, (long long)worst_offset,
          (unsigned long long)worst_halfwidth, (unsigned long long)resolution_ns);

    /*
     * All of them, or none.
     *
     * `clock_worst_offset_ns()` says two readings taken on *any* two
     * CPUs differ by at most that much. A CPU whose measurement failed
     * keeps a zero correction and contributed nothing to the bound, so
     * publishing a finite bound while one is missing states something
     * about that CPU which nothing established. One failure and the
     * machine keeps its raw counter and says so -- the same answer as a
     * counter that is not a clock, for the same reason: no claim is
     * better than one that is not backed.
     */
    if (failed != 0 || measured != wanted) {
        memset(g_cpu_offset_ns, 0, sizeof(g_cpu_offset_ns));
        __atomic_store_n(&g_worst_offset_ns, CLOCK_OFFSET_UNBOUNDED, __ATOMIC_RELEASE);
        g_clock_common = false;
        kwarn("timer: measured %u of %u CPU offsets; not applying a correction and advertising no bound",
              measured, wanted);
        return;
    }

    if (!g_clock_common) {
        memset(g_cpu_offset_ns, 0, sizeof(g_cpu_offset_ns));
        kwarn("timer: not applying the correction: %s is not a clock this kernel may trust across CPUs",
              arch_clock_name());
        return;
    }
    __atomic_store_n(&g_apply_offset, true, __ATOMIC_RELEASE);
    __atomic_store_n(&g_worst_offset_ns, worst_halfwidth, __ATOMIC_RELEASE);
    kinfo("timer: correction applied; two CPUs' readings now differ by at most %llu ns",
          (unsigned long long)worst_halfwidth);
}

uint64_t timer_ticks(void)
{
    return raw_this_cpu()->ticks;   /* a statistic: some CPU's tick count, for diagnostics */
}

unsigned timer_pending_count(void)
{
    struct timer_queue *q = raw_this_cpu()->timers;   /* a statistic: some CPU's queue, counted under its lock */
    arch_irq_state_t s = spin_lock_irqsave(&q->lock);
    unsigned n = q->count;
    spin_unlock_irqrestore(&q->lock, s);
    return n;
}

/* Module ABI v1 exports (docs/kernel/module/api.md). */
#include <kernel/module.h>
EXPORT_SYMBOL(clock_now_ns);
EXPORT_SYMBOL(clock_since_ns);
EXPORT_SYMBOL(clock_deadline_ns);
EXPORT_SYMBOL(clock_deadline_passed);
EXPORT_SYMBOL(timer_setup);
EXPORT_SYMBOL(timer_start);
EXPORT_SYMBOL(timer_cancel);
EXPORT_SYMBOL(timer_cancel_sync);
EXPORT_SYMBOL(ndelay);
EXPORT_SYMBOL(udelay);
