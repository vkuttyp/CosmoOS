/*
 * schedtest.c - Boot-time self-tests for ACPI, interrupt routing, timers,
 * threads, scheduling, and the sleeping primitives.
 *
 * These run from thread 0 with interrupts enabled and the tick running.
 * Each test joins every thread it creates and checks the thread count
 * returns to its starting value, so leaks are caught in place.
 */

#include <kernel/acpi.h>
#include <kernel/completion.h>
#include <kernel/errno.h>
#include <kernel/interrupt.h>
#include <kernel/irq.h>
#include <kernel/log.h>
#include <kernel/mutex.h>
#include <kernel/percpu.h>
#include <kernel/printf.h>
#include <kernel/sched.h>
#include <kernel/selftest.h>
#include <kernel/semaphore.h>
#include <kernel/smp.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/wait.h>

#include <arch/cpu.h>
#include <arch/irq.h>
#include <arch/testhooks.h>

#define STR_(x) #x
#define STR(x)  STR_(x)
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            *reason = "check failed: " #cond " at line " STR(__LINE__);        \
            return false;                                                      \
        }                                                                      \
    } while (0)

#define MS(n) ((uint64_t)(n) * 1000000ULL)

/* Exited threads are freed by the reaper thread after join returns, so
 * the thread count settles asynchronously. Wait briefly for it. */
static bool threads_settle(unsigned expected)
{
    uint64_t deadline = clock_now_ns() + MS(200);
    while (thread_count() != expected) {
        if (clock_now_ns() > deadline)
            return false;
        sched_yield();
    }
    return true;
}

/* --- acpi --- */

bool selftest_acpi(const char **reason)
{
    const struct acpi_madt_cpu *cpus;
    CHECK(acpi_available());
    struct acpi_gic gic_desc;
    CHECK(acpi_madt_lapic_base() != 0 || acpi_madt_gic(&gic_desc));   /* a LAPIC (x86-64) or a GIC (AArch64) */
    CHECK(acpi_madt_cpus(&cpus) >= 1);
    CHECK(acpi_find_table("APIC") != NULL);
    CHECK(acpi_find_table("ZZZZ") == NULL);
    return true;
}

/* --- timer --- */

struct order_probe {
    unsigned seq;
    unsigned fired_at[4];
};

struct tagged_timer {
    struct timer t;
    unsigned tag;
    struct order_probe *probe;
};

static void tagged_cb(struct timer *t, void *arg)
{
    (void)arg;
    struct tagged_timer *tt = (struct tagged_timer *)t;
    struct order_probe *p = tt->probe;
    if (p->seq < 4)
        p->fired_at[p->seq++] = tt->tag;
}

struct rearm_probe {
    struct timer t;
    volatile unsigned fires;
    unsigned limit;
};

static void rearm_cb(struct timer *t, void *arg)
{
    (void)arg;
    struct rearm_probe *p = (struct rearm_probe *)t;
    p->fires++;
    if (p->fires < p->limit)
        timer_start(t, MS(2)); /* re-arm from inside the callback */
}

bool selftest_timer(const char **reason)
{
    /* A callback may re-arm its own timer (periodic pattern). The probe
     * lives on this stack, so it is cancelled -- synchronously -- before
     * any check can return: a failed check that left a re-arming timer
     * behind had it write into a dead frame from the next interrupt
     * (found by the USB unit's chain under host load: the fourth fire was
     * late, the check failed, the kernel panicked on the fifth). The
     * check is about re-arming, not about how fast TCG delivers four
     * interrupts, so the wait is generous. */
    struct rearm_probe rp = { .fires = 0, .limit = 4 };
    timer_setup(&rp.t, rearm_cb, NULL);
    timer_start(&rp.t, MS(2));
    for (unsigned waited = 0; waited < 500 && rp.fires < 4; waited++)
        udelay(1000);
    udelay(3000);   /* the fourth callback returns and the timer settles */
    unsigned fires = rp.fires;
    unsigned state = rp.t.state;
    if (fires != 4 || state != TIMER_IDLE)
        timer_cancel_sync(&rp.t);
    CHECK(fires == 4);
    CHECK(state == TIMER_IDLE);

    /* Monotonic clock. */
    uint64_t last = clock_now_ns();
    for (int i = 0; i < 1000; i++) {
        uint64_t now = clock_now_ns();
        CHECK(now >= last);
        last = now;
    }

    /*
     * The tick advances at roughly CONFIG_HZ.
     *
     * Roughly, and the slack is not symmetric. A tick that arrives when
     * the last one has not been taken is coalesced -- the interrupt is
     * level-triggered and the counter advances once -- so on a busy host
     * emulating this machine the tick count lags the clock, by more the
     * busier the host is. That is the platform being honest rather than
     * the kernel being wrong, so the lag side is bounded loosely and the
     * other side tightly: ticks *ahead* of the clock would mean a tick
     * counted without time passing, which is a real bug.
     *
     * The loose bound is half the window, and it has been widened twice.
     * It began at two ticks, went to a quarter of the window in the
     * FP/SIMD unit, and still failed about one run in three in the
     * busiest device shape (`QEMU_KBD=hub` on AArch64, where the whole
     * USB hub and keyboard are emulated alongside everything else). What
     * this check is worth is "the timer interrupt arrives at roughly the
     * right rate", and half a window still says that; a tighter bound
     * was only ever measuring the host's load. The observed values go
     * into the failure message, so the next person to widen it does not
     * have to guess again.
     */
    uint64_t t0 = timer_ticks();
    uint64_t c0 = clock_now_ns();
    udelay(40000);
    uint64_t c1 = clock_now_ns();
    uint64_t t1 = timer_ticks();
    CHECK(c1 - c0 >= MS(40));
    /* No upper bound on the delay: udelay spins on this same clock until
     * it reads `end` (timer.c), so c1 - c0 exceeds 40 ms only by the time
     * this thread spent off the CPU -- a tick, another thread, the host.
     * A `< 80 ms` that used to be here could measure only that. */
    uint64_t expected = (c1 - c0) / TICK_NS;
    uint64_t lag_allowed = expected / 2 > 2 ? expected / 2 : 2;
    if (t1 - t0 + lag_allowed < expected) {
        kwarn("selftest: timer: %llu ticks in %llu ns, expected about %llu (lag allowed %llu)",
              (unsigned long long)(t1 - t0), (unsigned long long)(c1 - c0), (unsigned long long)expected,
              (unsigned long long)lag_allowed);
        *reason = "the tick count lagged the clock by more than half the window";
        return false;
    }
    CHECK(t1 - t0 <= expected + 2);

    /* Timers fire in expiry order, not arming order. */
    struct order_probe probe = { 0 };
    struct tagged_timer a = { .tag = 1, .probe = &probe };
    struct tagged_timer b = { .tag = 2, .probe = &probe };
    struct tagged_timer c = { .tag = 3, .probe = &probe };
    timer_setup(&a.t, tagged_cb, NULL);
    timer_setup(&b.t, tagged_cb, NULL);
    timer_setup(&c.t, tagged_cb, NULL);
    timer_start(&a.t, MS(30));
    timer_start(&b.t, MS(10));
    timer_start(&c.t, MS(20));
    CHECK(timer_pending_count() >= 3);
    udelay(60000);
    CHECK(probe.seq == 3);
    CHECK(probe.fired_at[0] == 2 && probe.fired_at[1] == 3 && probe.fired_at[2] == 1);
    CHECK(a.t.state == TIMER_IDLE && b.t.state == TIMER_IDLE && c.t.state == TIMER_IDLE);

    /* Cancel a pending timer; it must not fire. */
    probe.seq = 0;
    timer_start(&a.t, MS(20));
    CHECK(timer_cancel(&a.t));
    CHECK(!timer_cancel(&a.t));
    udelay(30000);
    CHECK(probe.seq == 0);
    CHECK(a.t.state == TIMER_IDLE);
    return true;
}

/* --- IRQ routing through the I/O APIC --- */

static unsigned g_pit_hits;      /* handler and test may run on different CPUs */

static void pit_handler(unsigned vector, struct arch_trap_frame *frame, void *arg)
{
    (void)vector;
    (void)frame;
    (void)arg;
    __atomic_fetch_add(&g_pit_hits, 1u, __ATOMIC_RELAXED);
}

/*
 * irq-route's two holdings, released however it returns (the net-leftover
 * unit). A failed count used to return with the line held and the source
 * running: irq-affinity's request then got -EBUSY, three times recorded in
 * docs/testing/flakes.md. The line's release is registered after the
 * request and the source's stop after the start, so the runner releases
 * the line first, as the test's own last lines do.
 */
static irq_t g_route_gsi;
static unsigned g_route_source;   /* its address is the source's key */

static void irq_route_stop_source(void *key)
{
    (void)key;
    arch_test_periodic_irq_stop();
}

static void irq_route_release_line(void *key)
{
    irq_t gsi = *(irq_t *)key;
    (void)irq_disable(gsi);
    (void)irq_release(gsi);
}

bool selftest_irq_route(const char **reason)
{
    int isa = arch_test_periodic_irq_start(200);
    if (isa < 0) {
        kinfo("selftest: no periodic ISA source; skipping IRQ routing");
        return true;
    }
    if (!selftest_defer(irq_route_stop_source, &g_route_source)) {
        arch_test_periodic_irq_stop();
        *reason = "its release list was full";
        return false;
    }
    unsigned flags;
    irq_t gsi = irq_legacy_to_gsi((unsigned)isa, &flags);

    __atomic_store_n(&g_pit_hits, 0u, __ATOMIC_RELAXED);
    int rc = irq_request(gsi, pit_handler, NULL, "selftest-pit", flags, raw_cpu_id());   /* a target for the line, not a claim about this thread */
    if (rc == -ENODEV) {
        selftest_release(&g_route_source);
        kinfo("selftest: no I/O APIC covers GSI %u; skipping IRQ routing", gsi);
        return true;
    }
    CHECK(rc == 0);
    g_route_gsi = gsi;
    if (!selftest_defer(irq_route_release_line, &g_route_gsi)) {
        irq_route_release_line(&g_route_gsi);
        *reason = "its release list was full";
        return false;
    }
    CHECK(irq_request(gsi, pit_handler, NULL, "dup", flags, 0) == -EBUSY);
    CHECK(irq_vector_of(gsi) >= 48);

    CHECK(irq_enable(gsi) == 0);
    /* Prove repeated delivery, not a minimum interrupt rate. The ARM
     * source re-arms from a tick callback and a descheduled vCPU can miss
     * several nominal periods. Leave the source running until five hits
     * arrive, with a deadline so a broken route still fails. */
    uint64_t deadline = clock_deadline_ns(MS(1000));
    unsigned hits;
    while ((hits = __atomic_load_n(&g_pit_hits, __ATOMIC_RELAXED)) < 5) {
        if (clock_deadline_passed(deadline)) {
            kinfo("selftest: irq-route: delivery deadline expired with %u of 5 hits", hits);
            *reason = "IRQ route did not deliver five interrupts before its deadline";
            return false;
        }
        thread_sleep_ms(1);
    }
    CHECK(irq_disable(gsi) == 0);
    udelay(20000);
    unsigned after_mask = __atomic_load_n(&g_pit_hits, __ATOMIC_RELAXED);
    udelay(20000);
    CHECK(__atomic_load_n(&g_pit_hits, __ATOMIC_RELAXED) == after_mask);

    CHECK(irq_release(gsi) == 0);
    selftest_forget(&g_route_gsi);   /* released, and checked, by the test itself */
    CHECK(irq_vector_of(gsi) == -1);
    selftest_release(&g_route_source);
    return true;
}

/* --- threads --- */

struct basic_state {
    volatile int ran;
    volatile unsigned on_cpu;
};

static void basic_entry(void *arg)
{
    struct basic_state *st = arg;
    st->on_cpu = raw_cpu_id();   /* recorded for the log, relied on by nothing */
    st->ran = 1;
    thread_exit(7);
}

bool selftest_thread(const char **reason)
{
    unsigned before = thread_count();
    struct basic_state st = { 0, 0 };

    struct thread *t = thread_create(basic_entry, &st, "selftest-basic", SCHED_PRIO_DEFAULT);
    CHECK(t != NULL);
    CHECK(thread_join(t) == 7);
    CHECK(st.ran == 1);
    CHECK(threads_settle(before));

    /* Returning from the entry function exits with code 0. */
    struct thread *t2 = thread_create((void (*)(void *))sched_yield, NULL, "selftest-ret", SCHED_PRIO_DEFAULT);
    CHECK(t2 != NULL);
    CHECK(thread_join(t2) == 0);
    CHECK(threads_settle(before));

    /* thread 0 is the current thread, on CPU 0's queue, with the boot flag. */
    struct thread *self = thread_current();
    CHECK(self != NULL && (self->flags & THREAD_FLAG_BOOT));
    CHECK(self->state == THREAD_RUNNING);
    CHECK(sched_switch_count(raw_cpu_id()) >= 2);   /* a statistic: some CPU's switches */
    return true;
}

/* Two threads alternating with yields both make progress. */
struct pingpong {
    volatile unsigned a, b;
};

static void ping_entry(void *arg)
{
    struct pingpong *pp = arg;
    for (int i = 0; i < 200; i++) {
        pp->a++;
        sched_yield();
    }
}

static void pong_entry(void *arg)
{
    struct pingpong *pp = arg;
    for (int i = 0; i < 200; i++) {
        pp->b++;
        sched_yield();
    }
}

bool selftest_yield(const char **reason)
{
    unsigned before = thread_count();
    struct pingpong pp = { 0, 0 };
    struct thread *a = thread_create(ping_entry, &pp, "ping", SCHED_PRIO_DEFAULT);
    struct thread *b = thread_create(pong_entry, &pp, "pong", SCHED_PRIO_DEFAULT);
    CHECK(a != NULL && b != NULL);
    thread_join(a);
    thread_join(b);
    CHECK(pp.a == 200 && pp.b == 200);
    CHECK(threads_settle(before));
    return true;
}

/* A thread that never yields must still be preempted by the tick so a
 * sleeping thread that wakes up gets to run. */
static void spinner_entry(void *arg)
{
    int *stop = arg;
    while (!__atomic_load_n(stop, __ATOMIC_ACQUIRE))
        arch_cpu_relax();
}

bool selftest_preempt(const char **reason)
{
    unsigned before = thread_count();
    int stop = 0;
    struct thread *s = thread_create(spinner_entry, &stop, "spinner", SCHED_PRIO_DEFAULT);
    CHECK(s != NULL);

    uint64_t t0 = clock_now_ns();
    thread_sleep_ms(30);
    uint64_t elapsed = clock_since_ns(t0);
    /* We are running again despite the spinner, and the spinner was
     * switched out to let us: preemption works. There is no upper bound
     * on `elapsed`; a `< 200 ms` used to sit here, and the failure it
     * named -- a spinner that is never preempted -- does not show as a
     * slow return but as no return, which the harness watchdog reports
     * with a scheduler dump (selftest.c). Nothing a kernel can do wrong
     * lands between "one slice late" and "never", so the bound could
     * fail only on a loaded host. */
    CHECK(elapsed >= MS(30));
    CHECK(s->switches >= 1);
    stop = 1;
    thread_join(s);
    CHECK(threads_settle(before));
    return true;
}

/* --- a same-CPU wake of a higher-priority thread preempts the waker ---
 *
 * The waker's very next statement is the observation: it stores `after`
 * right after the post, and the waiter's first statement on waking reads
 * it. `saw == 0` means the waiter ran between the post and that store,
 * which only a preemption inside the post can produce; `saw == 1` is
 * what the tree did before the restore-point preemption
 * (docs/audit/next-subsystem-wake-preempt.md): the waker ran on to the
 * tick. `saw` starts at 2 so a waiter that never ran -- or never blocked,
 * so the post woke nobody -- cannot pass. Both threads are on CPU 0, so
 * there is no cross-CPU wake to confuse the reading.
 */
struct wake_probe {
    struct semaphore sem;
    volatile unsigned after;
    volatile unsigned saw;
    volatile uint64_t woke_at;
};

static void wake_probe_entry(void *arg)
{
    struct wake_probe *w = arg;
    semaphore_down(&w->sem);
    w->saw = w->after;
    w->woke_at = clock_now_ns();
}

static bool selftest_preempt_wake_pinned(const char **reason)
{
    unsigned before = thread_count();
    unsigned here = arch_cpu_id();   /* pinned by the wrapper: the waiter goes where this thread stays */
    struct wake_probe w;
    semaphore_init(&w.sem, 0, "preempt-wake");
    w.after = 0;
    w.saw = 2;
    w.woke_at = 0;
    struct thread *t = thread_create_on(wake_probe_entry, &w, "wake-probe", SCHED_PRIO_DEFAULT - 16,
                                        CPUMASK_OF(here));
    CHECK(t != NULL);
    /* Post only once it is blocked: a post that finds no waiter wakes
     * nobody and the sentinel would then fail this for the wrong reason
     * (a one-word read of a field written under a lock this test does
     * not hold, used to decide when to post and never as a claim). */
    uint64_t deadline = clock_now_ns() + MS(1000);
    while (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) != THREAD_BLOCKED) {
        CHECK(clock_now_ns() < deadline);
        thread_sleep_ms(1);
    }

    uint64_t sent = clock_now_ns();
    semaphore_up(&w.sem);
    w.after = 1;   /* the very next statement */
    thread_join(t);

    CHECK(w.saw == 0);
    kinfo("selftest: preempt-wake: the waiter ran %llu us after the post, before the waker's next statement",
          (unsigned long long)((w.woke_at - sent) / 1000));
    CHECK(threads_settle(before));
    return true;
}

/* Pinned to the CPU it starts on for the whole test: the CPUs it names
 * as "here" and "another" are claims about this thread's CPU that must
 * outlive its sleeps (S25). The pin is the affinity the check honours. */
bool selftest_preempt_wake(const char **reason)
{
    cpumask_t saved = thread_pin_self();
    bool r = selftest_preempt_wake_pinned(reason);
    thread_set_affinity_self(saved);
    return r;
}


/* --- priority inheritance: a mutex owner is boosted to a waiter's priority -
 *
 * The three-thread inversion, staged on one CPU. A low-priority thread L
 * holds a mutex; a high-priority thread H blocks on it; a medium-priority
 * thread Mid is runnable. With inheritance, H blocking on the mutex boosts L
 * to H's priority, so L outranks Mid, runs, and releases the mutex before Mid
 * finishes -- and H, at the instant it acquires, sees that Mid had not yet
 * finished (h_saw_mid == 0). Without inheritance it would see the opposite,
 * which is what tools/priority-inheritance-probe.py showed on the tree before
 * this unit. Deterministic: L parks holding the mutex, H blocks, Mid parks,
 * then Mid and L are released together and priority order alone decides.
 * Every wait is bounded, so a regression is a failed check, not a hang.
 */
struct pi_test {
    struct mutex m;
    struct semaphore l_run, mid_run;
    volatile int held, mid_done, h_got, h_saw_mid;
};

static void pi_test_low(void *arg)
{
    struct pi_test *p = arg;
    mutex_lock(&p->m);
    __atomic_store_n(&p->held, 1, __ATOMIC_RELEASE);
    semaphore_down(&p->l_run);
    mutex_unlock(&p->m);
}

static void pi_test_mid(void *arg)
{
    struct pi_test *p = arg;
    semaphore_down(&p->mid_run);
    for (volatile unsigned i = 0; i < 2000000u; i++)
        arch_cpu_relax();
    __atomic_store_n(&p->mid_done, 1, __ATOMIC_RELEASE);
}

static void pi_test_high(void *arg)
{
    struct pi_test *p = arg;
    mutex_lock(&p->m);
    __atomic_store_n(&p->h_saw_mid, __atomic_load_n(&p->mid_done, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
    __atomic_store_n(&p->h_got, 1, __ATOMIC_RELEASE);
    mutex_unlock(&p->m);
}

static bool pi_wait_state(struct thread *t, enum thread_state st, uint64_t deadline)
{
    while (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) != st) {
        if (clock_now_ns() >= deadline)
            return false;
        thread_sleep_ms(1);
    }
    return true;
}

static bool pi_wait_flag(volatile int *flag, uint64_t deadline)
{
    while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE)) {
        if (clock_now_ns() >= deadline)
            return false;
        thread_sleep_ms(1);
    }
    return true;
}

static bool selftest_prio_inversion_pinned(const char **reason)
{
    unsigned before = thread_count();
    unsigned here = arch_cpu_id();
    int base = thread_current()->priority;
    CHECK(base + 12 < SCHED_PRIO_COUNT);
    int ph = base + 4, pm = base + 8, pl = base + 12;   /* H > Mid > L, all below this thread */

    static struct pi_test p;
    memset(&p, 0, sizeof(p));
    mutex_init(&p.m, "pi-test");
    semaphore_init(&p.l_run, 0, "pi-l");
    semaphore_init(&p.mid_run, 0, "pi-mid");
    uint64_t dl = clock_now_ns() + MS(6000);   /* whole-test bound, under the 8 s budget */

    struct thread *L = NULL, *H = NULL, *Mid = NULL;
    const char *why = NULL;

    L = thread_create_on(pi_test_low, &p, "pi-low", pl, CPUMASK_OF(here));
    if (L == NULL) { why = "pi-low: thread_create_on"; goto out; }
    if (!pi_wait_flag(&p.held, dl)) { why = "L did not take the mutex"; goto out; }
    if (!pi_wait_state(L, THREAD_BLOCKED, dl)) { why = "L did not park holding the mutex"; goto out; }

    H = thread_create_on(pi_test_high, &p, "pi-high", ph, CPUMASK_OF(here));
    if (H == NULL) { why = "pi-high: thread_create_on"; goto out; }
    if (!pi_wait_state(H, THREAD_BLOCKED, dl)) { why = "H did not block on the mutex"; goto out; }

    Mid = thread_create_on(pi_test_mid, &p, "pi-mid", pm, CPUMASK_OF(here));
    if (Mid == NULL) { why = "pi-mid: thread_create_on"; goto out; }
    if (!pi_wait_state(Mid, THREAD_BLOCKED, dl)) { why = "Mid did not park"; goto out; }

    /* Release Mid and L together: both runnable, priority alone decides. */
    semaphore_up(&p.mid_run);
    semaphore_up(&p.l_run);
    if (!pi_wait_flag(&p.h_got, dl)) { why = "H never acquired the mutex"; goto out; }
    if (p.h_saw_mid != 0)
        why = "priority was not inherited: the medium thread finished before the high thread acquired";

out:
    semaphore_up(&p.l_run);
    semaphore_up(&p.mid_run);
    if (L != NULL) thread_join(L);
    if (H != NULL) thread_join(H);
    if (Mid != NULL) thread_join(Mid);
    if (why != NULL) {
        *reason = why;
        return false;
    }
    /* The workers run below this thread, and one may have been preempted
     * inside thread_exit (it signalled the join before leaving the run
     * queue). threads_settle spins with sched_yield, which never lets a
     * lower-priority thread finish, so wait by sleeping -- that yields the
     * CPU down to them. */
    bool settled = false;
    for (unsigned i = 0; i < 400 && !settled; i++) {
        if (thread_count() == before) { settled = true; break; }
        thread_sleep_ms(1);
    }
    CHECK(settled);
    kinfo("selftest: prio-inversion: the low owner was boosted to the high waiter's priority and ran before the "
          "medium thread");
    return true;
}

bool selftest_prio_inversion(const char **reason)
{
    cpumask_t saved = thread_pin_self();
    bool r = selftest_prio_inversion_pinned(reason);
    thread_set_affinity_self(saved);
    return r;
}

/* --- the other shape: a direct sched_wake, no wait-queue wake ---
 *
 * The futex, poll, the AIO ring, process kill and signal delivery find
 * their thread by other means and call `sched_wake` on it directly. The
 * point belongs to `sched_wake`'s own irqsave unlock, not the wait
 * queue's, and this is what shows it: the waiter parks itself as a futex
 * waiter does (`wait_event` on a private queue) and is woken with
 * `sched_wake(t)` after its condition is set -- the same observation as
 * `preempt-wake`, through the other door.
 */
struct direct_probe {
    struct waitqueue wq;
    volatile unsigned go;
    volatile unsigned after;
    volatile unsigned saw;
    volatile uint64_t woke_at;
};

static void direct_probe_entry(void *arg)
{
    struct direct_probe *d = arg;
    wait_event(&d->wq, __atomic_load_n(&d->go, __ATOMIC_ACQUIRE));
    d->saw = d->after;
    d->woke_at = clock_now_ns();
}

static bool selftest_preempt_wake_direct_pinned(const char **reason)
{
    unsigned before = thread_count();
    unsigned here = arch_cpu_id();   /* pinned by the wrapper */
    struct direct_probe d;
    waitqueue_init(&d.wq, "preempt-wake-direct");
    d.go = 0;
    d.after = 0;
    d.saw = 2;
    d.woke_at = 0;
    struct thread *t = thread_create_on(direct_probe_entry, &d, "direct-probe", SCHED_PRIO_DEFAULT - 16,
                                        CPUMASK_OF(here));
    CHECK(t != NULL);
    uint64_t deadline = clock_now_ns() + MS(1000);
    while (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) != THREAD_BLOCKED) {
        CHECK(clock_now_ns() < deadline);
        thread_sleep_ms(1);
    }

    __atomic_store_n(&d.go, 1, __ATOMIC_RELEASE);   /* the condition, as a futex word */
    uint64_t sent = clock_now_ns();
    CHECK(sched_wake(t));                            /* the wake, with no wait queue in between */
    d.after = 1;   /* the very next statement */
    thread_join(t);

    CHECK(d.saw == 0);
    kinfo("selftest: preempt-wake-direct: the waiter ran %llu us after the wake, before the waker's next statement",
          (unsigned long long)((d.woke_at - sent) / 1000));
    CHECK(threads_settle(before));
    return true;
}

/* Pinned to the CPU it starts on for the whole test: the CPUs it names
 * as "here" and "another" are claims about this thread's CPU that must
 * outlive its sleeps (S25). The pin is the affinity the check honours. */
bool selftest_preempt_wake_direct(const char **reason)
{
    cpumask_t saved = thread_pin_self();
    bool r = selftest_preempt_wake_direct_pinned(reason);
    thread_set_affinity_self(saved);
    return r;
}


/* --- the post inside a bare interrupts-off region ---
 *
 * The post's own unlock restores interrupts to *off* (the caller had
 * them off), so nothing fires there; the caller's `arch_irq_restore` is
 * the first enable, and it must be the point -- which is why the point
 * lives in the restore and not in `spin_unlock_irqrestore`. The store
 * comes after the restore: inside the region nothing can run.
 */
static bool selftest_preempt_wake_locked_pinned(const char **reason)
{
    unsigned before = thread_count();
    unsigned here = arch_cpu_id();   /* pinned by the wrapper */
    struct wake_probe w;
    semaphore_init(&w.sem, 0, "preempt-wake-locked");
    w.after = 0;
    w.saw = 2;
    w.woke_at = 0;
    struct thread *t = thread_create_on(wake_probe_entry, &w, "wake-probe-locked", SCHED_PRIO_DEFAULT - 16,
                                        CPUMASK_OF(here));
    CHECK(t != NULL);
    uint64_t deadline = clock_now_ns() + MS(1000);
    while (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) != THREAD_BLOCKED) {
        CHECK(clock_now_ns() < deadline);
        thread_sleep_ms(1);
    }

    uint64_t sent = clock_now_ns();
    arch_irq_state_t s = arch_irq_save();
    semaphore_up(&w.sem);      /* its unlock restores to "off": no point here */
    arch_irq_restore(s);       /* the first enable: the point */
    w.after = 1;               /* the very next statement */
    thread_join(t);

    CHECK(w.saw == 0);
    kinfo("selftest: preempt-wake-locked: the waiter ran %llu us after the post, before the waker's next statement",
          (unsigned long long)((w.woke_at - sent) / 1000));
    CHECK(threads_settle(before));
    return true;
}

/* Pinned to the CPU it starts on for the whole test: the CPUs it names
 * as "here" and "another" are claims about this thread's CPU that must
 * outlive its sleeps (S25). The pin is the affinity the check honours. */
bool selftest_preempt_wake_locked(const char **reason)
{
    cpumask_t saved = thread_pin_self();
    bool r = selftest_preempt_wake_locked_pinned(reason);
    thread_set_affinity_self(saved);
    return r;
}


#if CONFIG_SELFTEST
/*
 * The debug probe behind `init --selftest`'s preempt-wake-syscall step:
 * the *read* of sysctl `debug.preempt_probe` is the system call under
 * test. It creates a priority-16 thread pinned to the caller's CPU,
 * waits for it to block, posts -- a wake made inside a system call --
 * and stores `after` as its next statement; the value it returns says
 * what the waiter saw. A wake that preempts gives `saw=0` before this
 * call returns to user mode, let alone reaches its own next line. The
 * report described two sequence numbers; the one flag says the same
 * thing. Privilege is checked by the caller in native.c.
 */
static int sched_preempt_probe_sysctl_pinned(char *out, size_t n)
{
    struct wake_probe w;
    semaphore_init(&w.sem, 0, "preempt-probe");
    w.after = 0;
    w.saw = 2;
    w.woke_at = 0;
    unsigned cpu = arch_cpu_id();   /* pinned by the wrapper: the caller stays here */
    struct thread *t = thread_create_on(wake_probe_entry, &w, "preempt-probe", SCHED_PRIO_DEFAULT - 16,
                                        CPUMASK_OF(cpu));
    if (t == NULL)
        return -ENOMEM;
    uint64_t deadline = clock_now_ns() + MS(1000);
    while (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) != THREAD_BLOCKED) {
        if (clock_now_ns() > deadline)
            break;   /* a thread cannot be abandoned: post and join regardless; the value says what happened */
        thread_sleep_ms(1);
    }
    uint64_t sent = clock_now_ns();
    semaphore_up(&w.sem);
    w.after = 1;   /* the very next statement */
    thread_join(t);
    return ksnprintf(out, n, "saw=%u latency_us=%llu", w.saw,
                     (unsigned long long)((w.woke_at - sent) / 1000));
}

/* Pinned to the CPU it starts on for the whole test: the CPUs it names
 * as "here" and "another" are claims about this thread's CPU that must
 * outlive its sleeps (S25). The pin is the affinity the check honours. */
int sched_preempt_probe_sysctl(char *out, size_t n)
{
    cpumask_t saved = thread_pin_self();
    int r = sched_preempt_probe_sysctl_pinned(out, n);
    thread_set_affinity_self(saved);
    return r;
}

#endif

#if CONFIG_DEBUG
/*
 * Some CI boots run the pair loop 7x slower from the start of the boot
 * (docs/testing/flakes.md, 2026-10-10). Per CPU, three loops separate
 * the candidates: the pair as the benchmark runs it (calls, lockdep, the
 * restore point), the interrupt flag alone in one asm sequence (no calls),
 * and dependent arithmetic with interrupts left alone (the control).
 */
struct irq_probe {
    unsigned cpu;
    uint64_t pair_ns, flag_ns, arith_ns;
};

static void irq_probe_thread(void *arg)
{
    struct irq_probe *r = arg;
    enum { M = 200000 };
    r->cpu = arch_cpu_id_raw();
    uint64_t t0 = clock_now_ns();
    for (unsigned i = 0; i < M; i++) {
        arch_irq_state_t st = arch_irq_save();
        arch_irq_restore(st);
    }
    r->pair_ns = clock_since_ns(t0) * 10 / M;
    t0 = clock_now_ns();
    for (unsigned i = 0; i < M; i++) {
#if defined(ARCH_X86_64)
        __asm__ volatile("pushfq\n\tcli\n\tpopfq" ::: "memory", "cc");
#else
        __asm__ volatile("mrs x9, daif\n\tmsr daifset, #2\n\tmsr daif, x9" ::: "memory", "x9");
#endif
    }
    r->flag_ns = clock_since_ns(t0) * 10 / M;
    volatile uint64_t acc = 1;
    t0 = clock_now_ns();
    for (unsigned i = 0; i < M; i++)
        acc = acc * 6364136223846793005ull + i;
    r->arith_ns = clock_since_ns(t0) * 10 / M;
}

static void irq_probe_all_cpus(void)
{
    cpumask_t online = cpu_online_mask();
    for (unsigned c = 0; c < 64; c++) {
        if (!(online & CPUMASK_OF(c)))
            continue;
        struct irq_probe r = { 0 };
        struct thread *t = thread_create_on(irq_probe_thread, &r, "irq-probe", SCHED_PRIO_DEFAULT, CPUMASK_OF(c));
        if (t == NULL)
            continue;
        thread_join(t);
        kinfo("selftest: irqrestore-bench: cpu %u: pair %llu.%llu ns, flag only %llu.%llu ns, arithmetic %llu.%llu ns",
              r.cpu, (unsigned long long)(r.pair_ns / 10), (unsigned long long)(r.pair_ns % 10),
              (unsigned long long)(r.flag_ns / 10), (unsigned long long)(r.flag_ns % 10),
              (unsigned long long)(r.arith_ns / 10), (unsigned long long)(r.arith_ns % 10));
    }
}
#endif

/* --- the cost of the restore point: a million save/restore pairs --- */
bool selftest_irqrestore_bench(const char **reason)
{
    (void)reason;
    enum { N = 1000000 };
    uint64_t t0 = clock_now_ns();
    for (unsigned i = 0; i < N; i++) {
        arch_irq_state_t s = arch_irq_save();
        arch_irq_restore(s);   /* with need_resched clear: the predicate's two loads and a branch */
    }
    uint64_t dt = clock_since_ns(t0);
#if CONFIG_DEBUG
    /* Some CI boots run this loop 7x slower from the start of the boot
     * (docs/testing/flakes.md, 2026-10-10): name the interrupt state. */
    char irqs[200];
    arch_test_irq_state(irqs, sizeof(irqs));
    kinfo("selftest: irqrestore-bench: interrupt controller: %s", irqs);
    irq_probe_all_cpus();
#endif
    kinfo("selftest: irqrestore-bench: %u save/restore pairs in %llu us, %llu ns a pair; restore-point preemptions so far on this CPU: %llu",
          N, (unsigned long long)(dt / 1000), (unsigned long long)(dt / N),
          (unsigned long long)preempt_point_count(raw_cpu_id()));   /* a statistic */
    return true;
}

/* --- the restore loop: reschedules pending at schedule()'s restore, in a row ---
 *
 * A thread resumed inside `schedule()` with a reschedule pending takes it
 * by another pass of the loop in the same frame (S31). Before the
 * restore-loop unit it took it by entering `schedule()` again from the
 * restore, one frame per reschedule unless every link was a tail call.
 * This drives that case thousands of times on one CPU: the seam
 * (`test_resume_resched`) leaves a reschedule pending and the slice spent
 * at each of this thread's next RESTORE_RUN resumptions, and a partner of
 * the same priority on the same CPU yields back each time, so every one
 * is a real switch out and back. The claims, per run of trips: the stack
 * this thread enters `schedule()` on stays within RESTORE_BOUND bytes of
 * this function's own frame, it is never inside `schedule()` twice, and
 * every armed resumption was a trip. tools/sched-restore-loop-probe.py
 * shows the first claim fails on the recursive structure with one link
 * forced to be a call and holds for the loop under the same link.
 */
enum {
    RESTORE_RUNS = 100,
    RESTORE_RUN = 32,       /* trips in a row: a recursion of 32 frames of 32 bytes or more clears the bound */
    RESTORE_BOUND = 1024,   /* above this frame: a yield's two frames, or a trap frame and the trap return's preemption */
};

#if CONFIG_DEBUG
struct restore_partner {
    volatile bool stop;
    uint64_t yields;
};

static void restore_partner_entry(void *arg)
{
    struct restore_partner *p = arg;
    while (!__atomic_load_n(&p->stop, __ATOMIC_ACQUIRE)) {
        sched_yield();
        p->yields++;
    }
}
#endif

static bool selftest_sched_restore_loop_pinned(const char **reason)
{
#if !CONFIG_DEBUG
    (void)reason;
    kinfo("selftest: sched-restore-loop: the per-thread record is written in debug builds only; skipped");
    return true;
#else
    unsigned before = thread_count();
    unsigned here = arch_cpu_id();   /* pinned by the wrapper */
    struct thread *self = thread_current();
    struct restore_partner p = { .stop = false, .yields = 0 };
    struct thread *t = thread_create_on(restore_partner_entry, &p, "restore-partner", self->priority,
                                        CPUMASK_OF(here));
    CHECK(t != NULL);

    size_t base = self->stack_base + self->stack_size - (uintptr_t)__builtin_frame_address(0);
    self->sched_depth_max = 0;
    self->sched_nest_max = 0;
    uint64_t trips0 = self->sched_trips;
    uint32_t longest = 0;
    size_t worst = 0;
    unsigned runs = 0, unspent = 0;
    uint64_t t0 = clock_now_ns();
    for (; runs < RESTORE_RUNS; runs++) {
        self->test_resume_resched = RESTORE_RUN;
        sched_yield();
        unspent = self->test_resume_resched;
        worst = self->sched_depth_max;
        if (self->sched_chain > longest)
            longest = self->sched_chain;
        if (unspent != 0 || worst > base + RESTORE_BOUND || self->sched_nest_max != 1)
            break;
    }
    uint64_t dt = clock_since_ns(t0);
    self->test_resume_resched = 0;
    __atomic_store_n(&p.stop, true, __ATOMIC_RELEASE);
    thread_join(t);

    uint64_t trips = self->sched_trips - trips0;
    kinfo("selftest: sched-restore-loop: %u runs of %u, %llu trips (longest %u in one call), %llu ns a trip; "
          "deepest entry %zu bytes above this frame (bound %u), nesting %u; partner yielded %llu times",
          runs, (unsigned)RESTORE_RUN, (unsigned long long)trips, longest,
          (unsigned long long)(trips ? dt / trips : 0), worst > base ? worst - base : 0, (unsigned)RESTORE_BOUND,
          self->sched_nest_max, (unsigned long long)p.yields);
    CHECK(worst <= base + RESTORE_BOUND);
    CHECK(self->sched_nest_max == 1);
    CHECK(unspent == 0);
    CHECK(runs == RESTORE_RUNS && trips >= (uint64_t)RESTORE_RUNS * RESTORE_RUN && longest >= RESTORE_RUN);
    CHECK(threads_settle(before));
    return true;
#endif
}

/* Pinned: the partner must share this thread's CPU for every resumption
 * to be a switch out and back on it. */
bool selftest_sched_restore_loop(const char **reason)
{
    cpumask_t saved = thread_pin_self();
    bool r = selftest_sched_restore_loop_pinned(reason);
    thread_set_affinity_self(saved);
    return r;
}

bool selftest_sleep(const char **reason)
{
    uint64_t t0 = clock_now_ns();
    thread_sleep_ms(20);
    uint64_t d = clock_since_ns(t0);
    CHECK(d >= MS(20));
    /* LOAD-SENSITIVE (docs/testing/flakes.md). A sleep wakes at the first
     * tick past its deadline, so its overshoot is a tick or so; the bound
     * says it is not a coarser mechanism (a sleep serviced every 100 ms
     * would fail it). It was `3 * TICK_NS + 10 ms` of slack and failed on
     * a correct kernel once a host held this vCPU for longer than that
     * (2026-09-13); the slack is now 100 ms, which is still an order of
     * magnitude under a coarse-mechanism bug, and a failure here on a
     * busy host is a re-run before it is an investigation. */
    CHECK(d < MS(20) + 3 * TICK_NS + MS(100));

    /* Short sleeps must not wake early. */
    t0 = clock_now_ns();
    thread_sleep_ns(MS(1));
    CHECK(clock_since_ns(t0) >= MS(1));
    return true;
}

/* --- mutex --- */

struct mutex_test {
    struct mutex m;
    volatile unsigned counter;
    volatile unsigned inside;
    volatile bool violated;
};

static void mutex_worker(void *arg)
{
    struct mutex_test *mt = arg;
    for (int i = 0; i < 100; i++) {
        mutex_lock(&mt->m);
        if (mt->inside != 0)
            mt->violated = true;
        mt->inside++;
        unsigned v = mt->counter;
        sched_yield();          /* force contention inside the section */
        mt->counter = v + 1;
        mt->inside--;
        mutex_unlock(&mt->m);
    }
}

bool selftest_mutex(const char **reason)
{
    unsigned before = thread_count();
    struct mutex_test mt;
    mutex_init(&mt.m, "selftest-mutex");
    mt.counter = 0;
    mt.inside = 0;
    mt.violated = false;

    CHECK(mutex_trylock(&mt.m));
    CHECK(!mutex_trylock(&mt.m));
    CHECK(mutex_is_locked(&mt.m));
    mutex_unlock(&mt.m);
    CHECK(!mutex_is_locked(&mt.m));

    struct thread *w[4];
    for (int i = 0; i < 4; i++) {
        w[i] = thread_create(mutex_worker, &mt, "mutex-worker", SCHED_PRIO_DEFAULT);
        CHECK(w[i] != NULL);
    }
    for (int i = 0; i < 4; i++)
        thread_join(w[i]);
    CHECK(mt.counter == 400);
    CHECK(!mt.violated);
    CHECK(!mutex_is_locked(&mt.m));
    CHECK(threads_settle(before));
    return true;
}

/* --- mutex-wake-bench: a contended unlock against the number of waiters --------
 *
 * N threads block on a mutex the test holds; the test unlocks and measures
 * two things, medians of the rounds: how long the unlock call itself takes
 * (the wake runs under the wait queue's spinlock with interrupts off) and
 * how long until the first woken waiter runs (unlock to the waiter's
 * acquire). Between the epoll-callback unit and its wake_one follow-up,
 * wake_one walked the whole list for callback entries, so the unlock grew
 * with N; the callbacks list of their own gives wake_one back its one
 * thread. Figures only, printed for the report; N = 1, 8, 32 and 256 (the
 * walk of a 32-entry list is below what a 50 us unlock under TCG can show;
 * 256 entries are a few microseconds).
 */
#define WAKE_BENCH_ROUNDS 10u
#define WAKE_BENCH_MAX    256u

struct wake_bench {
    struct mutex m;
    volatile uint64_t first_at;   /* the first woken waiter's acquire time; 0 until then */
};

static void wake_bench_waiter(void *arg)
{
    struct wake_bench *b = arg;
    mutex_lock(&b->m);
    uint64_t now = clock_now_ns();
    uint64_t zero = 0;
    __atomic_compare_exchange_n(&b->first_at, &zero, now, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    mutex_unlock(&b->m);
    thread_exit(0);
}

static void sort_ns(uint64_t *v, unsigned n)
{
    for (unsigned i = 1; i < n; i++) {
        uint64_t x = v[i];
        unsigned j = i;
        while (j > 0 && v[j - 1] > x) {
            v[j] = v[j - 1];
            j--;
        }
        v[j] = x;
    }
}

/* A round that cannot go on: release the mutex so the waiters already
 * created can take it and leave, and join them, before the test reports
 * its reason. A CHECK that returned with the mutex held would leave them
 * blocked for the rest of the boot. */
static bool wake_bench_abort(struct wake_bench *b, struct thread **th, unsigned created, const char **reason,
                             const char *why)
{
    mutex_unlock(&b->m);
    for (unsigned i = 0; i < created; i++)
        thread_join(th[i]);
    *reason = why;
    return false;
}

static bool wake_bench_one(const char **reason, unsigned nwaiters, uint64_t *unlock_ns, uint64_t *wake_ns)
{
    static struct wake_bench b;
    static struct thread *th[WAKE_BENCH_MAX];
    uint64_t unlock[WAKE_BENCH_ROUNDS], wake[WAKE_BENCH_ROUNDS];
    mutex_init(&b.m, "wake-bench");
    for (unsigned r = 0; r < WAKE_BENCH_ROUNDS; r++) {
        /* A round creates, settles and joins up to 256 threads: on a slow
         * host the ten of a size add up, so the watchdog is kicked per
         * round, not per size. The run stays under the harness's per-test
         * budget by being quick (1.6 to 1.9 s for all four sizes on TCG),
         * not by a budget of its own. */
        sched_watchdog_kick();
        b.first_at = 0;
        mutex_lock(&b.m);
        unsigned created = 0;
        for (; created < nwaiters; created++) {
            th[created] = thread_create(wake_bench_waiter, &b, "wake-bench-w", SCHED_PRIO_DEFAULT);
            if (th[created] == NULL)
                return wake_bench_abort(&b, th, created, reason, "mutex-wake-bench: thread_create failed");
        }
        /* Every waiter is on the mutex's wait queue -- the queue's own
         * count, under its lock, not a counter the waiter bumps on the way
         * in nor a settle: what the unlock then walks is exactly nwaiters
         * entries, and the first acquire it times is a wake. */
        unsigned i;
        for (i = 0; i < 400 && waitqueue_waiting(&b.m.wq) < nwaiters; i++)
            thread_sleep_ms(5);
        if (waitqueue_waiting(&b.m.wq) != nwaiters)
            return wake_bench_abort(&b, th, created, reason, "mutex-wake-bench: the waiters did not all block in 2 s");
        uint64_t t0 = clock_now_ns();
        mutex_unlock(&b.m);
        uint64_t t1 = clock_now_ns();
        for (i = 0; i < nwaiters; i++)
            thread_join(th[i]);
        CHECK(b.first_at != 0 && b.first_at >= t0);
        unlock[r] = t1 - t0;
        wake[r] = b.first_at - t0;
    }
    sort_ns(unlock, WAKE_BENCH_ROUNDS);
    sort_ns(wake, WAKE_BENCH_ROUNDS);
    *unlock_ns = unlock[WAKE_BENCH_ROUNDS / 2];
    *wake_ns = wake[WAKE_BENCH_ROUNDS / 2];
    return true;
}

bool selftest_mutex_wake_bench(const char **reason)
{
    unsigned before = thread_count();
    static const unsigned sizes[4] = { 1, 8, 32, 256 };
    uint64_t unlock_ns[4], wake_ns[4];
    for (unsigned k = 0; k < 4; k++) {
        if (!wake_bench_one(reason, sizes[k], &unlock_ns[k], &wake_ns[k]))
            return false;
    }
    kinfo("selftest: mutex-wake-bench: waiters 1/8/32/256: unlock call %llu/%llu/%llu/%llu ns, unlock to the first waiter's "
          "acquire %llu/%llu/%llu/%llu ns (medians of %u)",
          (unsigned long long)unlock_ns[0], (unsigned long long)unlock_ns[1], (unsigned long long)unlock_ns[2],
          (unsigned long long)unlock_ns[3], (unsigned long long)wake_ns[0], (unsigned long long)wake_ns[1],
          (unsigned long long)wake_ns[2], (unsigned long long)wake_ns[3], WAKE_BENCH_ROUNDS);
    CHECK(threads_settle(before));
    return true;
}

/* --- semaphore --- */

struct sem_test {
    struct semaphore items;
    volatile unsigned consumed;
};

static void consumer_entry(void *arg)
{
    struct sem_test *st = arg;
    for (int i = 0; i < 5; i++) {
        semaphore_down(&st->items);
        /* Two consumers on two CPUs share this counter: a plain increment
         * lost updates once in several hundred runs (seen as 9 of 10). */
        __atomic_fetch_add(&st->consumed, 1, __ATOMIC_RELAXED);
    }
}

bool selftest_semaphore(const char **reason)
{
    unsigned before = thread_count();
    struct sem_test st;
    semaphore_init(&st.items, 0, "selftest-sem");
    st.consumed = 0;

    CHECK(!semaphore_trydown(&st.items));
    struct thread *c = thread_create(consumer_entry, &st, "consumer", SCHED_PRIO_DEFAULT);
    CHECK(c != NULL);

    thread_sleep_ms(5);
    CHECK(st.consumed == 0);
    for (int i = 0; i < 5; i++) {
        semaphore_up(&st.items);
        thread_sleep_ms(2);
    }
    thread_join(c);
    CHECK(st.consumed == 5);
    CHECK(semaphore_count(&st.items) == 0);

    /* Two consumers blocked at once, then posts back to back with no
     * sleep in between: every post must reach a distinct blocked waiter
     * even though the first woken one is still linked in the queue. */
    struct sem_test st2;
    semaphore_init(&st2.items, 0, "selftest-sem2");
    st2.consumed = 0;
    struct thread *c1 = thread_create(consumer_entry, &st2, "consumer-1", SCHED_PRIO_DEFAULT);
    struct thread *c2 = thread_create(consumer_entry, &st2, "consumer-2", SCHED_PRIO_DEFAULT);
    CHECK(c1 != NULL && c2 != NULL);
    thread_sleep_ms(5); /* both blocked in semaphore_down */
    for (int i = 0; i < 10; i++)
        semaphore_up(&st2.items);
    /* The joins are the check: a post that woke the already-woken
     * consumer again leaves the other blocked with items on the
     * semaphore, and this never returns -- the harness watchdog reports
     * it (selftest.c). A `< 500 ms` on the joins used to follow; it named
     * no failure the joins do not, and could fail only on a loaded host. */
    thread_join(c1);
    thread_join(c2);
    CHECK(st2.consumed == 10);
    CHECK(semaphore_count(&st2.items) == 0);
    CHECK(threads_settle(before));
    return true;
}

/* --- completion --- */

static void completer_entry(void *arg)
{
    struct completion *c = arg;
    thread_sleep_ms(10);
    complete(c);
}

bool selftest_completion(const char **reason)
{
    unsigned before = thread_count();
    struct completion c;
    completion_init(&c, "selftest-completion");
    struct thread *t = thread_create(completer_entry, &c, "completer", SCHED_PRIO_DEFAULT);
    CHECK(t != NULL);
    CHECK(!completion_done(&c));
    uint64_t t0 = clock_now_ns();
    wait_for_completion(&c);
    CHECK(clock_since_ns(t0) >= MS(10));
    CHECK(completion_done(&c));
    wait_for_completion(&c); /* already done: returns immediately */
    thread_join(t);
    CHECK(threads_settle(before));
    return true;
}

/* --- wait queue: wake_one wakes exactly one --- */

struct wq_test {
    struct waitqueue wq;
    volatile int go;
    volatile unsigned woke;
};

static void waiter_entry(void *arg)
{
    struct wq_test *wt = arg;
    wait_event(&wt->wq, wt->go != 0);
    wt->woke++;
}

bool selftest_waitqueue(const char **reason)
{
    unsigned before = thread_count();
    struct wq_test wt;
    waitqueue_init(&wt.wq, "selftest-wq");
    wt.go = 0;
    wt.woke = 0;

    struct thread *a = thread_create(waiter_entry, &wt, "waiter-a", SCHED_PRIO_DEFAULT);
    struct thread *b = thread_create(waiter_entry, &wt, "waiter-b", SCHED_PRIO_DEFAULT);
    CHECK(a != NULL && b != NULL);
    thread_sleep_ms(5);
    CHECK(!waitqueue_empty(&wt.wq));
    CHECK(wt.woke == 0);

    /* Wake with the condition still false: both re-check and re-block. */
    CHECK(waitqueue_wake_all(&wt.wq) == 2);
    thread_sleep_ms(5);
    CHECK(wt.woke == 0);

    wt.go = 1;
    CHECK(waitqueue_wake_one(&wt.wq) == 1);
    thread_sleep_ms(5);
    CHECK(wt.woke == 1);
    CHECK(waitqueue_wake_all(&wt.wq) == 1);
    thread_join(a);
    thread_join(b);
    CHECK(wt.woke == 2);
    CHECK(waitqueue_empty(&wt.wq));
    CHECK(threads_settle(before));
    return true;
}

/* --- completion-race ---------------------------------------------------------- */

/*
 * A completion on the waiter's stack, completed from another CPU, and the
 * frame reused the moment the waiter returns. complete() must have let go
 * of the completion by then: the first version set `done`, dropped its
 * lock and only then woke -- a waiter that arrived in that window (or
 * polled) saw `done`, returned, and the wake ran on memory that belonged
 * to the next call. The AHCI unit's concurrent block benchmark hit it as
 * a spinlock assertion inside wake(). Here the waiter varies its arrival
 * so the completer often completes first, then poisons the frame; with
 * the old complete() the wake walks a zeroed wait queue and faults.
 */
struct cr_shared {
    struct completion *volatile c;
    volatile unsigned round;
    volatile bool stop;
};

static void cr_completer(void *arg)
{
    struct cr_shared *sh = arg;
    unsigned seen = 0;
    while (!__atomic_load_n(&sh->stop, __ATOMIC_ACQUIRE)) {
        unsigned r = __atomic_load_n(&sh->round, __ATOMIC_ACQUIRE);
        if (r != seen) {
            struct completion *c = __atomic_load_n(&sh->c, __ATOMIC_ACQUIRE);
            seen = r;
            complete(c);
        } else {
            arch_cpu_relax();
        }
    }
    thread_exit(0);
}

bool selftest_completion_race(const char **reason)
{
    if (cpu_count() < 2) {
        kinfo("selftest: completion-race: one CPU; skipping");
        return true;
    }
    struct cr_shared sh = { NULL, 0, false };
    struct thread *t = thread_create_on(cr_completer, &sh, "cr-completer", SCHED_PRIO_DEFAULT, CPUMASK_OF(1));
    CHECK(t != NULL);
    enum { ROUNDS = 20000 };
    struct completion c;
    uint64_t t0 = clock_now_ns();
    for (unsigned r = 1; r <= ROUNDS; r++) {
        completion_init(&c, "cr");
        __atomic_store_n(&sh.c, &c, __ATOMIC_RELEASE);
        __atomic_store_n(&sh.round, r, __ATOMIC_RELEASE);
        for (unsigned k = 0; k < (r % 64); k++)
            arch_cpu_relax();   /* sometimes the completer is first: that is the window */
        wait_for_completion(&c);
        memset(&c, 0, sizeof(c));   /* the frame is the next call's now */
    }
    __atomic_store_n(&sh.stop, true, __ATOMIC_RELEASE);
    thread_join(t);
    kinfo("selftest: completion-race: %u completions across two CPUs, the frame reused after each, in %llu ms", ROUNDS,
          (unsigned long long)((clock_since_ns(t0)) / 1000000));
    return true;
}

/*
 * completion-timeout: wait_for_completion_timeout does the handshake, so a
 * poller can no longer see `done` and return while complete() still holds
 * the completion (the NVMe admin bug, docs/audit/next-subsystem-nvme-admin.md).
 *
 * A completer on CPU 1 calls complete_linger, which sets `done` and then
 * holds the completion's lock for a spell before waking. The test thread
 * waits until `done` is visible -- the completer is now lingering, lock
 * held -- and only then calls wait_for_completion_timeout. It must return
 * true, and the completion's lock must be free on return: the handshake
 * took that lock, so it cannot return until the completer let go. Without
 * the handshake it returns during the linger and the check sees the lock
 * still held. The completion is static, so a miss is a named failure, not
 * corruption. A wait on a completion nobody completes returns false.
 */
struct ct_shared {
    struct completion *volatile c;
    volatile unsigned round;
    volatile bool stop;
};

static void ct_completer(void *arg)
{
    struct ct_shared *sh = arg;
    unsigned seen = 0;
    while (!__atomic_load_n(&sh->stop, __ATOMIC_ACQUIRE)) {
        unsigned r = __atomic_load_n(&sh->round, __ATOMIC_ACQUIRE);
        if (r != seen) {
            struct completion *c = __atomic_load_n(&sh->c, __ATOMIC_ACQUIRE);
            seen = r;
            complete_linger(c, 2ull * 1000000ull);   /* 2 ms held between `done` and the wake */
        } else {
            arch_cpu_relax();
        }
    }
    thread_exit(0);
}

bool selftest_completion_timeout(const char **reason)
{
    if (cpu_count() < 2) {
        kinfo("selftest: completion-timeout: one CPU; skipping");
        return true;
    }
    struct ct_shared sh = { NULL, 0, false };
    struct thread *t = thread_create_on(ct_completer, &sh, "ct-completer", SCHED_PRIO_DEFAULT, CPUMASK_OF(1));
    CHECK(t != NULL);
    enum { ROUNDS = 200 };
    struct completion c;
    uint64_t t0 = clock_now_ns();
    /* Every exit from here goes through `done`: the completer holds pointers
     * to `sh` and `c` on this stack, so it is stopped and joined before this
     * frame can go, on a failure as much as on success. */
    const char *fail = NULL;
    for (unsigned r = 1; r <= ROUNDS && fail == NULL; r++) {
        completion_init(&c, "ct");
        __atomic_store_n(&sh.c, &c, __ATOMIC_RELEASE);
        __atomic_store_n(&sh.round, r, __ATOMIC_RELEASE);
        /* Wait until the completer has published `done` and is lingering with
         * the lock held: that is the window a bare poller would return in.
         * Bounded, so a stuck completer fails the round rather than hanging. */
        uint64_t rdv = clock_deadline_ns(1000ull * 1000000ull);
        while (!completion_done(&c) && !clock_deadline_passed(rdv))
            arch_cpu_relax();
        if (!completion_done(&c)) {
            fail = "the completer did not publish done within 1 s";
            break;
        }
        if (!wait_for_completion_timeout(&c, 1000ull * 1000000ull)) {
            fail = "wait_for_completion_timeout returned false on a completed completion";
            break;
        }
        /* The completer (on CPU 1) drops c->lock only at the end of its
         * linger; a bare read of the lock word, not spin_is_held (which asks
         * "do *I* hold it" and reads a per-CPU id), sees whether it has. The
         * handshake guarantees it has. */
        if (__atomic_load_n(&c.lock.locked, __ATOMIC_ACQUIRE) != 0) {
            fail = "the wait returned while the completer still held the completion's lock";
            break;
        }
        memset(&c, 0, sizeof(c));        /* the frame is the next round's now */
    }
    if (fail != NULL)   /* said before the join, so the reason is in the log even if the join waits */
        kerror("selftest: completion-timeout: %s", fail);
    __atomic_store_n(&sh.stop, true, __ATOMIC_RELEASE);
    thread_join(t);   /* a lingering completer finishes on `c`, still live, before this */
    if (fail != NULL) {
        *reason = fail;
        return false;
    }

    /* A completion nobody completes: the wait returns false at the deadline,
     * decided by the return value, not by how long it took. */
    struct completion nc;
    completion_init(&nc, "ct-none");
    CHECK(!wait_for_completion_timeout(&nc, 30ull * 1000000ull));

    kinfo("selftest: completion-timeout: %u lingered completions, the lock free on every return, "
          "and a timeout returned false, in %llu ms", ROUNDS,
          (unsigned long long)(clock_since_ns(t0) / 1000000));
    return true;
}

/*
 * wait_event_timeout: the three ways it can end (the quiesce-wake unit,
 * docs/audit/next-subsystem-quiesce-wake.md). Tested here rather than
 * only through its first caller, because a primitive whose only coverage
 * is one user's happy path is a primitive nobody can reuse.
 *
 * None of the three assertions is a duration. The first two are decided
 * by what the call returns, and the third by a flag a timer set -- a
 * loaded host makes them slower, not wrong.
 */
struct wt_probe {
    struct waitqueue wq;
    bool ready;
};

static void wt_waker(void *arg)
{
    struct wt_probe *p = arg;
    thread_sleep_ns(TICK_NS);
    __atomic_store_n(&p->ready, true, __ATOMIC_RELEASE);
    waitqueue_wake_all(&p->wq);
}

bool selftest_wait_timeout(const char **reason)
{
    struct wt_probe p;
    waitqueue_init(&p.wq, "wait-timeout-test");

    /* 1. The condition already holds: no timer, no block, true. */
    p.ready = true;
    CHECK(wait_event_timeout(&p.wq, __atomic_load_n(&p.ready, __ATOMIC_ACQUIRE), NS_PER_SEC));

    /* 2. The deadline arrives first and nothing ever sets the condition:
     * false, and the call returns rather than waiting for a waker that
     * is not coming. A generous deadline would make this test slow; a
     * short one cannot make it wrong, because the assertion is the
     * return value and not the elapsed time. */
    p.ready = false;
    CHECK(!wait_event_timeout(&p.wq, __atomic_load_n(&p.ready, __ATOMIC_ACQUIRE), TICK_NS));

    /* 3. A waker beats the deadline: true, and from a deadline long
     * enough that a loaded host cannot turn this into case 2. */
    p.ready = false;
    struct thread *t = thread_create(wt_waker, &p, "wt-waker", SCHED_PRIO_DEFAULT);
    CHECK(t != NULL);
    bool woken = wait_event_timeout(&p.wq, __atomic_load_n(&p.ready, __ATOMIC_ACQUIRE), 10 * NS_PER_SEC);
    thread_join(t);
    CHECK(woken);
    CHECK(__atomic_load_n(&p.ready, __ATOMIC_ACQUIRE));

    kinfo("selftest: wait-timeout: a condition already true arms nothing; a deadline with no waker "
          "returns false; a waker inside a 10 s deadline returns true");
    return true;
}
