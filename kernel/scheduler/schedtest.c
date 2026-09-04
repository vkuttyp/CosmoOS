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
#include <kernel/sched.h>
#include <kernel/selftest.h>
#include <kernel/semaphore.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/wait.h>

#include <arch/cpu.h>
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
    CHECK(acpi_madt_lapic_base() != 0);
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
    /* A callback may re-arm its own timer (periodic pattern). */
    struct rearm_probe rp = { .fires = 0, .limit = 4 };
    timer_setup(&rp.t, rearm_cb, NULL);
    timer_start(&rp.t, MS(2));
    udelay(40000);
    CHECK(rp.fires == 4);
    CHECK(rp.t.state == TIMER_IDLE);

    /* Monotonic clock. */
    uint64_t last = clock_now_ns();
    for (int i = 0; i < 1000; i++) {
        uint64_t now = clock_now_ns();
        CHECK(now >= last);
        last = now;
    }

    /* The tick advances at roughly CONFIG_HZ. */
    uint64_t t0 = timer_ticks();
    uint64_t c0 = clock_now_ns();
    udelay(40000);
    uint64_t c1 = clock_now_ns();
    uint64_t t1 = timer_ticks();
    CHECK(c1 - c0 >= MS(40));
    CHECK(c1 - c0 < MS(80));
    uint64_t expected = (c1 - c0) / TICK_NS;
    CHECK(t1 - t0 + 2 >= expected);
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

static volatile unsigned g_pit_hits;

static void pit_handler(unsigned vector, struct arch_trap_frame *frame, void *arg)
{
    (void)vector;
    (void)frame;
    (void)arg;
    g_pit_hits++;
}

bool selftest_irq_route(const char **reason)
{
    int isa = arch_test_periodic_irq_start(200);
    if (isa < 0) {
        kinfo("selftest: no periodic ISA source; skipping IRQ routing");
        return true;
    }
    unsigned flags;
    irq_t gsi = irq_legacy_to_gsi((unsigned)isa, &flags);

    g_pit_hits = 0;
    int rc = irq_request(gsi, pit_handler, NULL, "selftest-pit", flags, arch_cpu_id());
    if (rc == -ENODEV) {
        arch_test_periodic_irq_stop();
        kinfo("selftest: no I/O APIC covers GSI %u; skipping IRQ routing", gsi);
        return true;
    }
    CHECK(rc == 0);
    CHECK(irq_request(gsi, pit_handler, NULL, "dup", flags, 0) == -EBUSY);
    CHECK(irq_vector_of(gsi) >= 48);

    CHECK(irq_enable(gsi) == 0);
    udelay(50000);
    unsigned hits = g_pit_hits;
    CHECK(hits >= 5);            /* 200 Hz over 50 ms = 10, allow slack */
    CHECK(irq_disable(gsi) == 0);
    udelay(20000);
    unsigned after_mask = g_pit_hits;
    udelay(20000);
    CHECK(g_pit_hits == after_mask);

    CHECK(irq_release(gsi) == 0);
    CHECK(irq_vector_of(gsi) == -1);
    arch_test_periodic_irq_stop();
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
    st->on_cpu = arch_cpu_id();
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
    CHECK(sched_switch_count(arch_cpu_id()) >= 2);
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
    uint64_t elapsed = clock_now_ns() - t0;
    /* We are running again despite the spinner: preemption works.
     * Bound: sleep + up to one slice of the spinner + slack. */
    CHECK(elapsed >= MS(30));
    CHECK(elapsed < MS(200));
    CHECK(s->switches >= 1);
    stop = 1;
    thread_join(s);
    CHECK(threads_settle(before));
    return true;
}

bool selftest_sleep(const char **reason)
{
    uint64_t t0 = clock_now_ns();
    thread_sleep_ms(20);
    uint64_t d = clock_now_ns() - t0;
    CHECK(d >= MS(20));
    CHECK(d < MS(20) + 3 * TICK_NS + MS(10));

    /* Short sleeps must not wake early. */
    t0 = clock_now_ns();
    thread_sleep_ns(MS(1));
    CHECK(clock_now_ns() - t0 >= MS(1));
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
        st->consumed++;
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
    uint64_t t0 = clock_now_ns();
    thread_join(c1);
    thread_join(c2);
    CHECK(clock_now_ns() - t0 < MS(500));
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
    CHECK(clock_now_ns() - t0 >= MS(10));
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
