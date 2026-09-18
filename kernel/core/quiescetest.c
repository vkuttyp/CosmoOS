/*
 * quiescetest.c - Boot-time self-tests of the quiescence subsystem
 * (docs/kernel/quiesce/testing.md).
 *
 * Run from thread 0 on CPU 0 after smp_init. Each test puts a real
 * reader in a real read-side section on another CPU (a preemption-
 * disabled spin, an interrupt handler, a timer callback) and checks that
 * the corresponding wait does not return until the reader is out. With
 * one CPU the same tests check the single-CPU behaviour: the calling CPU
 * is quiescent by construction and the waits return at once.
 */

#include <kernel/interrupt.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/percpu.h>
#include <kernel/quiesce.h>
#include <kernel/sched.h>
#include <kernel/selftest.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/wait.h>

#include <arch/cpu.h>
#include <arch/irq.h>
#include <arch/irqc.h>

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
#define MAGIC_LIVE 0x4c495645u
#define MAGIC_DEAD 0x44454144u

static unsigned other_cpu(void)
{
    for (unsigned c = 1; c < cpu_count(); c++)
        if (cpu_online(c))
            return c;
    return 0;
}

/* Spin without sleeping until `*flag` is non-zero or `ms` elapse. */
static bool wait_flag(const volatile unsigned *flag, unsigned ms)
{
    uint64_t end = clock_now_ns() + MS(ms);
    while (__atomic_load_n(flag, __ATOMIC_ACQUIRE) == 0) {
        if (clock_now_ns() > end)
            return false;
        arch_cpu_relax();
    }
    return true;
}

static bool threads_settle(unsigned expected)
{
    uint64_t deadline = clock_now_ns() + MS(500);
    while (thread_count() != expected) {
        if (clock_now_ns() > deadline)
            return false;
        sched_yield();
    }
    return true;
}

/* --- the straggler kick: what may honestly be claimed about it ------------
 *
 * docs/audit/next-subsystem-lifetime-windows.md. `synchronize_quiesce`
 * kicks a CPU that has not published for more than two ticks, counting
 * the rounds in `straggler_ipis` -- a counter incremented in one place
 * and, until these tests, read in none.
 *
 * What these tests may claim is narrower than "the kick works", and the
 * first draft of them claimed the wrong thing. A CPU publishes at
 * interrupt return only when `preempt_count == 0`, so a CPU spinning
 * inside a read-side section takes the kick and returns without
 * publishing: the period ends when that CPU leaves its section, kick or
 * no kick. A test asserting that the kick completed the period would be
 * reporting positive evidence for a kick that did nothing.
 *
 * So: the waiter's side is asserted (it kicks, and bounds the kicks),
 * the spinner's side is asserted (it is *not* helped -- the wait
 * outlasts the section), the system's side is asserted (other CPUs keep
 * working), and the idle case is asserted to need no kick at all. What
 * the kick is worth for the one population it could help -- a CPU whose
 * tick keeps colliding with a short disabled region -- is left as a
 * question, because arranging that is a phase coincidence and a test
 * that cannot be made deterministic is not worth having.
 */

struct straggler_reader {
    unsigned hold_ms;
    volatile unsigned entered;
    volatile unsigned done;
};

/* A read-side section held past the two-tick kick threshold. Preemption
 * is disabled for its duration, which is what makes this CPU a
 * straggler and what stops the kick from helping it. */
static void straggler_main(void *arg)
{
    struct straggler_reader *r = arg;
    quiesce_read_lock();
    __atomic_store_n(&r->entered, 1u, __ATOMIC_RELEASE);
    uint64_t end = clock_now_ns() + MS(r->hold_ms);
    while (clock_now_ns() < end)
        arch_cpu_relax();
    __atomic_store_n(&r->done, 1u, __ATOMIC_RELEASE);
    quiesce_read_unlock();
}

bool selftest_quiesce_straggler(const char **reason)
{
#if !CONFIG_DEBUG
    /* The per-waiter kick count is a debug-build thing, and the
     * machine-wide one cannot carry a per-waiter claim: see where it is
     * asserted. Adding that counter is what turned this test, which
     * needed no hook, into one that does. */
    (void)reason;
    kinfo("selftest: quiesce-straggler: no per-waiter kick count in this build; skipping");
    return true;
#else
    unsigned threads0 = thread_count();
    unsigned cpu = other_cpu();
    if (cpu == 0) {
        /* Said rather than skipped silently: a property about two CPUs
         * needs two CPUs, and a test that quietly does nothing on the
         * machine CI runs is worse than no test. */
        kinfo("selftest: quiesce-straggler: one CPU, nothing to straggle");
        return true;
    }

    /* Well past 2 * TICK_NS (8 ms at CONFIG_HZ 250), and short of the
     * eight-kick bound at one kick per loop iteration. */
    struct straggler_reader r = { .hold_ms = 30 };
    struct quiesce_stats before, after;
    quiesce_get_stats(&before);

    struct thread *t = thread_create_on(straggler_main, &r, "qstrag", SCHED_PRIO_DEFAULT, CPUMASK_OF(cpu));
    CHECK(t != NULL);
    CHECK(wait_flag(&r.entered, 1000));

    uint64_t t0 = clock_now_ns();
    /*
     * The wait, handing back the kicks it sent.
     *
     * Not the machine-wide `straggler_ipis`, which another waiter moves
     * too; and not a per-CPU slot either, because the callback worker is
     * unpinned and can run its own grace period on this CPU between the
     * call returning and this preemptible thread reading the slot. The
     * count comes back on the stack, where it cannot be anyone else's.
     */
    unsigned kicks = quiesce_test_sync_kicks();
    uint64_t waited_ns = clock_since_ns(t0);
    quiesce_get_stats(&after);
    CHECK(kicks >= 1);
    CHECK(kicks <= 8);   /* the bound in the code, and this waiter's own */
    CHECK(after.straggler_ipis >= before.straggler_ipis + kicks);

    /*
     * The spinner's side, and the assertion that keeps this test honest:
     * the period was *still waiting* while those kicks were sent. The
     * reader finished its section before synchronize_quiesce returned,
     * which is what says the kick did not shorten anything -- and it is
     * the assertion that fails if someone makes the trap publish without
     * checking preempt_count.
     */
    CHECK(__atomic_load_n(&r.done, __ATOMIC_ACQUIRE) == 1);
    CHECK(waited_ns >= MS(r.hold_ms) / 2);   /* it spanned the section, not a tick */

    thread_join(t);
    CHECK(threads_settle(threads0));
    kinfo("selftest: quiesce-straggler: %u kick(s) from this waiter over a %llu ms wait, and the spinner was not "
          "helped by any of them",
          kicks, (unsigned long long)(waited_ns / 1000000));
    return true;
#endif
}

/* --- quiesce-straggler-system: the stall is the waiter's, not the machine's --- */

struct bystander {
    volatile unsigned go;
    volatile unsigned ticks;
    volatile unsigned stop;
};

static void bystander_main(void *arg)
{
    struct bystander *b = arg;
    while (!__atomic_load_n(&b->go, __ATOMIC_ACQUIRE))
        arch_cpu_relax();
    while (!__atomic_load_n(&b->stop, __ATOMIC_ACQUIRE)) {
        /* Ordinary work: allocate, free, yield. Nothing privileged and
         * nothing that touches quiescence. */
        void *p = kmalloc(64, 0);
        if (p)
            kfree(p);
        __atomic_fetch_add(&b->ticks, 1u, __ATOMIC_RELEASE);
        sched_yield();
    }
}

/*
 * The claim the lifetime report actually makes (risk 2): a long
 * preempt-disabled section stalls the waiter, not the system. One CPU
 * spins inside a read section, this CPU waits for it, and a third does
 * ordinary work throughout -- which must keep completing.
 */
bool selftest_quiesce_straggler_system(const char **reason)
{
    unsigned threads0 = thread_count();
    if (cpu_count() < 3) {
        kinfo("selftest: quiesce-straggler-system: needs three CPUs, have %u", cpu_count());
        return true;
    }
    unsigned spin_cpu = 0, work_cpu = 0;
    for (unsigned c = 1; c < cpu_count(); c++) {
        if (!cpu_online(c))
            continue;
        if (spin_cpu == 0)
            spin_cpu = c;
        else if (work_cpu == 0)
            work_cpu = c;
    }
    if (spin_cpu == 0 || work_cpu == 0) {
        kinfo("selftest: quiesce-straggler-system: fewer than three CPUs online");
        return true;
    }

    struct straggler_reader r = { .hold_ms = 30 };
    struct bystander b = { 0 };
    struct thread *w = thread_create_on(bystander_main, &b, "qwork", SCHED_PRIO_DEFAULT, CPUMASK_OF(work_cpu));
    CHECK(w != NULL);
    struct thread *t = thread_create_on(straggler_main, &r, "qstrag", SCHED_PRIO_DEFAULT, CPUMASK_OF(spin_cpu));
    CHECK(t != NULL);
    CHECK(wait_flag(&r.entered, 1000));

    unsigned work_before = __atomic_load_n(&b.ticks, __ATOMIC_ACQUIRE);
    __atomic_store_n(&b.go, 1u, __ATOMIC_RELEASE);
    synchronize_quiesce();
    unsigned work_after = __atomic_load_n(&b.ticks, __ATOMIC_ACQUIRE);
    __atomic_store_n(&b.stop, 1u, __ATOMIC_RELEASE);

    CHECK(__atomic_load_n(&r.done, __ATOMIC_ACQUIRE) == 1);
    /* The third CPU got real work done while the other two were stuck
     * with each other. A handful of iterations is enough to say it was
     * never blocked; the number is reported rather than bounded tightly,
     * because it is a scheduler's throughput and not a claim. */
    CHECK(work_after > work_before);

    thread_join(t);
    thread_join(w);
    CHECK(threads_settle(threads0));
    kinfo("selftest: quiesce-straggler-system: %u units of ordinary work on CPU %u while CPU %u stalled the waiter",
          work_after - work_before, work_cpu, spin_cpu);
    return true;
}

/*
 * An idle CPU needs no kick, and this test exists to say that the case
 * never reaches the kick at all.
 *
 * It is deliberately *not* the positive case for the straggler IPI. An
 * idle CPU publishes at the top of its idle loop and again after each
 * periodic tick, so a grace period with one completes in about a tick --
 * before the two-tick threshold that sends the first kick. A test
 * asserting "the kick helped the idle CPU" would pass with the kick path
 * deleted, which is how the first draft of this was wrong.
 */
bool selftest_quiesce_straggler_idle(const char **reason)
{
#if !CONFIG_DEBUG
    (void)reason;
    kinfo("selftest: quiesce-straggler-idle: no per-waiter kick count in this build; skipping");
    return true;
#else
    struct quiesce_stats before, after;
    quiesce_get_stats(&before);
    uint64_t t0 = clock_now_ns();
    /* This call sent no kick, asserted against its own returned count:
     * an equality on the machine-wide total would fail for someone
     * else's kicks and say nothing about the idle case. */
    unsigned kicks = quiesce_test_sync_kicks();
    uint64_t waited_ns = clock_since_ns(t0);
    quiesce_get_stats(&after);

    CHECK(kicks == 0);
    CHECK(waited_ns < MS(100));   /* and it did not take the kick path's time */
    (void)before;
    (void)after;

    kinfo("selftest: quiesce-straggler-idle: a grace period over idle CPUs took %llu us and sent no kick",
          (unsigned long long)(waited_ns / 1000));
    return true;
#endif
}

/* --- quiesce-grace: a reader inside quiesce_read_lock holds the grace period --- */

struct grace_obj {
    unsigned magic;
    unsigned reads;
};

struct grace_reader {
    struct grace_obj *volatile *slot;   /* the pointer the reader dereferences */
    unsigned hold_ms;
    volatile unsigned entered;
    volatile unsigned done;             /* set inside the read section, before unlock */
    unsigned bad;                       /* saw a dead object */
};

static void grace_reader_main(void *arg)
{
    struct grace_reader *r = arg;
    quiesce_read_lock();
    quiesce_read_lock();   /* nesting */
    struct grace_obj *o = __atomic_load_n(r->slot, __ATOMIC_ACQUIRE);
    __atomic_store_n(&r->entered, 1u, __ATOMIC_RELEASE);
    uint64_t end = clock_now_ns() + MS(r->hold_ms);
    while (clock_now_ns() < end) {
        /* The object was unlinked by now (the updater waited for
         * `entered`), but the section keeps it alive. */
        if (o->magic != MAGIC_LIVE)
            r->bad++;
        o->reads++;
        arch_cpu_relax();
    }
    __atomic_store_n(&r->done, 1u, __ATOMIC_RELEASE);
    quiesce_read_unlock();
    quiesce_read_unlock();
}

bool selftest_quiesce_grace(const char **reason)
{
    unsigned threads0 = thread_count();
    struct quiesce_stats before, after;
    quiesce_get_stats(&before);

    /* Single CPU (or the calling CPU alone): the wait completes without
     * a tick because this CPU publishes as part of the call. */
    uint64_t t0 = clock_now_ns();
    synchronize_quiesce();
    uint64_t solo_ns = clock_since_ns(t0);
    quiesce_get_stats(&after);
    CHECK(after.epoch == before.epoch + 1);
    CHECK(after.synchronizes == before.synchronizes + 1);

    unsigned cpu = other_cpu();
    if (cpu == 0) {
        kinfo("selftest: quiesce-grace: one CPU, solo grace period in %llu us", (unsigned long long)(solo_ns / 1000));
        return true;
    }

    struct grace_obj *obj = kzalloc(sizeof(*obj));
    CHECK(obj != NULL);
    obj->magic = MAGIC_LIVE;
    struct grace_obj *volatile slot = obj;
    struct grace_reader r = { .slot = &slot, .hold_ms = 30 };

    struct thread *t = thread_create_on(grace_reader_main, &r, "qreader", SCHED_PRIO_DEFAULT, CPUMASK_OF(cpu));
    CHECK(t != NULL);
    CHECK(wait_flag(&r.entered, 1000));

    /* Unlink, then wait. The reader is inside its section for another
     * ~30 ms with preemption disabled; ticks on its CPU must not count as
     * quiescent states (preempt_count != 0), so the wait spans the hold. */
    __atomic_store_n(&slot, (struct grace_obj *)NULL, __ATOMIC_RELEASE);
    t0 = clock_now_ns();
    synchronize_quiesce();
    uint64_t grace_ns = clock_since_ns(t0);

    CHECK(__atomic_load_n(&r.done, __ATOMIC_ACQUIRE) == 1);   /* the wait outlasted the section */
    obj->magic = MAGIC_DEAD;                                    /* now safe: no reader can hold it */
    unsigned reads = obj->reads;
    kfree(obj);

    thread_join(t);
    CHECK(r.bad == 0);
    CHECK(reads > 0);
    CHECK(grace_ns >= MS(20));   /* the reader held for 30 ms; allow for the entered->unlink gap */
    CHECK(threads_settle(threads0));
    kinfo("selftest: quiesce-grace: reader on CPU %u held %u ms, grace period took %llu ms (solo %llu us)", cpu,
          r.hold_ms, (unsigned long long)(grace_ns / 1000000), (unsigned long long)(solo_ns / 1000));
    return true;
}

/* --- quiesce-call: deferred callbacks run once, in order, in thread context --- */

struct call_probe {
    struct quiesce_head head;
    unsigned index;
    unsigned *order;         /* shared sequence counter */
    unsigned seen_at;        /* value of *order when this callback ran */
    unsigned runs;
    bool thread_ctx;
};

static volatile unsigned g_calls_done;

static void call_probe_fn(struct quiesce_head *h)
{
    struct call_probe *p = (struct call_probe *)h;
    p->runs++;
    p->seen_at = (*p->order)++;
    p->thread_ctx = this_cpu()->irq_depth == 0 && this_cpu()->preempt_count == 0;
    __atomic_fetch_add(&g_calls_done, 1u, __ATOMIC_RELEASE);
}

bool selftest_quiesce_call(const char **reason)
{
    enum { N = 8 };
    struct call_probe probes[N];
    unsigned order = 0;
    struct quiesce_stats before, after;
    quiesce_get_stats(&before);
    g_calls_done = 0;

    for (unsigned i = 0; i < N; i++) {
        probes[i] = (struct call_probe){ .index = i, .order = &order };
        /* Half from a spinlock-held context: call_quiesce is usable there. */
        if (i & 1) {
            arch_irq_state_t s = arch_irq_save();
            preempt_disable();
            call_quiesce(&probes[i].head, call_probe_fn);
            preempt_enable();
            arch_irq_restore(s);
        } else {
            call_quiesce(&probes[i].head, call_probe_fn);
        }
    }

    uint64_t end = clock_now_ns() + MS(2000);
    while (__atomic_load_n(&g_calls_done, __ATOMIC_ACQUIRE) < N && clock_now_ns() < end)
        thread_sleep_ns(MS(1));
    CHECK(g_calls_done == N);
    for (unsigned i = 0; i < N; i++) {
        CHECK(probes[i].runs == 1);
        CHECK(probes[i].thread_ctx);
        CHECK(probes[i].seen_at == i);   /* submission order */
    }
    quiesce_get_stats(&after);
    CHECK(after.callbacks == before.callbacks + N);
    CHECK(after.epoch > before.epoch);
    kinfo("selftest: quiesce-call: %u callbacks in order after %llu grace period(s)", N,
          (unsigned long long)(after.synchronizes - before.synchronizes));
    return true;
}

/* --- irq-sync: interrupt_unregister_sync outlasts a running handler --- */

struct irq_probe {
    unsigned magic;
    volatile unsigned entered;
    volatile unsigned done;      /* set by the handler before it returns */
    unsigned hold_ms;
    unsigned hits;
    unsigned bad;
};

static void irq_probe_handler(unsigned vector, struct arch_trap_frame *frame, void *arg)
{
    (void)vector;
    (void)frame;
    struct irq_probe *p = arg;
    p->hits++;
    if (p->magic != MAGIC_LIVE)
        p->bad++;
    __atomic_store_n(&p->entered, 1u, __ATOMIC_RELEASE);
    /* A long handler: interrupts are masked on this CPU for the hold. */
    uint64_t end = clock_now_ns() + MS(p->hold_ms);
    while (clock_now_ns() < end)
        arch_cpu_relax();
    __atomic_store_n(&p->done, 1u, __ATOMIC_RELEASE);
}

bool selftest_irq_sync(const char **reason)
{
    struct irq_probe *p = kzalloc(sizeof(*p));
    CHECK(p != NULL);
    p->magic = MAGIC_LIVE;
    p->hold_ms = 20;

    int vec = arch_vector_alloc();
    CHECK(vec >= 0);
    CHECK(interrupt_register((unsigned)vec, irq_probe_handler, p, "selftest-irqsync") == 0);
    arch_ipi_bind((unsigned)vec);

    unsigned cpu = other_cpu();
    struct quiesce_stats before, after;
    quiesce_get_stats(&before);

    if (cpu == 0) {
        /* Self-IPI: the handler runs before arch_ipi_send's caller can
         * proceed far (interrupts are enabled here). */
        arch_ipi_send(0, (unsigned)vec);
        CHECK(wait_flag(&p->done, 1000));
        uint64_t t0 = clock_now_ns();
        CHECK(interrupt_unregister_sync((unsigned)vec, irq_probe_handler) == 0);
        uint64_t sync_ns = clock_since_ns(t0);
        CHECK(p->hits == 1 && p->bad == 0);
        kinfo("selftest: irq-sync: one CPU, self-IPI handled, unregister_sync in %llu us",
              (unsigned long long)(sync_ns / 1000));
    } else {
        arch_ipi_send(cpu, (unsigned)vec);
        CHECK(wait_flag(&p->entered, 1000));
        /* The handler is running on `cpu` right now, for ~20 ms. */
        uint64_t t0 = clock_now_ns();
        CHECK(interrupt_unregister_sync((unsigned)vec, irq_probe_handler) == 0);
        uint64_t sync_ns = clock_since_ns(t0);
        CHECK(__atomic_load_n(&p->done, __ATOMIC_ACQUIRE) == 1);   /* returned only after the handler */
        CHECK(sync_ns >= MS(10));
        CHECK(p->hits == 1 && p->bad == 0);
        kinfo("selftest: irq-sync: handler on CPU %u held %u ms, unregister_sync took %llu ms", cpu, p->hold_ms,
              (unsigned long long)(sync_ns / 1000000));
    }
    quiesce_get_stats(&after);
    CHECK(after.irq_syncs == before.irq_syncs + 1);

    /* No handler now: a second unregister finds nothing; the record's
     * memory can go. */
    CHECK(interrupt_unregister((unsigned)vec, irq_probe_handler) == -ENOENT);
    p->magic = MAGIC_DEAD;
    kfree(p);
    arch_vector_free((unsigned)vec);
    return true;
}

/* --- timer-cancel-sync: the wait outlasts a running callback; re-arming loses --- */

struct timer_probe {
    struct timer t;
    unsigned magic;
    volatile unsigned entered;
    volatile unsigned done;
    unsigned hold_ms;
    unsigned fires;
    unsigned bad;
    bool rearm;
    volatile unsigned stop;
};

static void timer_probe_fn(struct timer *t, void *arg)
{
    struct timer_probe *p = arg;
    (void)t;
    p->fires++;
    if (p->magic != MAGIC_LIVE)
        p->bad++;
    __atomic_store_n(&p->entered, 1u, __ATOMIC_RELEASE);
    uint64_t end = clock_now_ns() + MS(p->hold_ms);
    while (clock_now_ns() < end)
        arch_cpu_relax();
    if (p->rearm && !__atomic_load_n(&p->stop, __ATOMIC_ACQUIRE))
        timer_start(&p->t, MS(1));
    __atomic_store_n(&p->done, 1u, __ATOMIC_RELEASE);
}

/* Arms the probe's timer on the CPU this thread is pinned to. */
static void timer_arm_main(void *arg)
{
    struct timer_probe *p = arg;
    timer_start(&p->t, MS(2));
}

bool selftest_timer_cancel_sync(const char **reason)
{
    unsigned threads0 = thread_count();
    struct timer_probe *p = kzalloc(sizeof(*p));
    CHECK(p != NULL);
    p->magic = MAGIC_LIVE;
    p->hold_ms = 20;
    timer_setup(&p->t, timer_probe_fn, p);

    /* 1. Pending, not running: behaves as timer_cancel. */
    timer_start(&p->t, MS(500));
    CHECK(timer_cancel_sync(&p->t));
    CHECK(!timer_cancel_sync(&p->t));
    CHECK(p->fires == 0);

    unsigned cpu = other_cpu();
    struct quiesce_stats before, after;
    quiesce_get_stats(&before);
    if (cpu == 0) {
        /* One CPU: a callback cannot be running while we hold the queue
         * lock; the sync form is free. Let one fire and cancel after. */
        p->hold_ms = 1;
        timer_start(&p->t, MS(1));
        CHECK(wait_flag(&p->done, 1000));
        CHECK(!timer_cancel_sync(&p->t));
        CHECK(p->fires == 1 && p->bad == 0);
        kfree(p);
        kinfo("selftest: timer-cancel-sync: one CPU, cancel of a fired timer is immediate");
        return true;
    }

    /* 2. Running on another CPU: the wait spans the callback. */
    struct thread *t = thread_create_on(timer_arm_main, p, "qtimer", SCHED_PRIO_DEFAULT, CPUMASK_OF(cpu));
    CHECK(t != NULL);
    thread_join(t);
    CHECK(wait_flag(&p->entered, 1000));
    uint64_t t0 = clock_now_ns();
    bool was_pending = timer_cancel_sync(&p->t);
    uint64_t sync_ns = clock_since_ns(t0);
    CHECK(!was_pending);
    CHECK(__atomic_load_n(&p->done, __ATOMIC_ACQUIRE) == 1);
    CHECK(sync_ns >= MS(10));
    quiesce_get_stats(&after);
    CHECK(after.timer_sync_waits == before.timer_sync_waits + 1);

    /* 3. A self-re-arming callback: cancel_sync must catch the re-arm
     * made after our first cancel and leave the timer idle for good. */
    p->entered = 0;
    p->done = 0;
    p->hold_ms = 5;
    p->rearm = true;
    t = thread_create_on(timer_arm_main, p, "qtimer", SCHED_PRIO_DEFAULT, CPUMASK_OF(cpu));
    CHECK(t != NULL);
    thread_join(t);
    CHECK(wait_flag(&p->entered, 1000));
    CHECK(timer_cancel_sync(&p->t) || true);   /* result depends on the phase; the postcondition is below */
    unsigned fires = p->fires;
    CHECK(p->t.state == TIMER_IDLE);
    thread_sleep_ns(MS(30));
    CHECK(p->fires == fires);   /* nothing fired after the sync cancel */
    CHECK(p->bad == 0);
    p->magic = MAGIC_DEAD;
    kfree(p);
    CHECK(threads_settle(threads0));
    kinfo("selftest: timer-cancel-sync: callback on CPU %u held 20 ms, cancel_sync took %llu ms; re-arming timer stopped after %u fires",
          cpu, (unsigned long long)(sync_ns / 1000000), fires);
    return true;
}

/* --- quiesce-stress: readers on every other CPU against a churning pointer --- */

struct stress_obj {
    unsigned magic;
    unsigned gen;
};

struct stress_shared {
    struct stress_obj *volatile cur;
    volatile unsigned stop;
};

struct stress_reader {
    struct stress_shared *sh;
    unsigned long reads;
    unsigned bad;
};

static void stress_reader_main(void *arg)
{
    struct stress_reader *r = arg;
    while (!__atomic_load_n(&r->sh->stop, __ATOMIC_ACQUIRE)) {
        quiesce_read_lock();
        struct stress_obj *o = __atomic_load_n(&r->sh->cur, __ATOMIC_ACQUIRE);
        if (o->magic != MAGIC_LIVE)
            r->bad++;
        r->reads++;
        quiesce_read_unlock();
        if ((r->reads & 0x3FF) == 0)
            sched_yield();   /* leave the CPU a quiescent point of its own now and then */
    }
}

struct stress_free {
    struct quiesce_head head;
    struct stress_obj *obj;
};

static volatile unsigned g_stress_freed;

static void stress_free_fn(struct quiesce_head *h)
{
    struct stress_free *f = (struct stress_free *)h;
    f->obj->magic = MAGIC_DEAD;
    kfree(f->obj);
    kfree(f);
    __atomic_fetch_add(&g_stress_freed, 1u, __ATOMIC_RELEASE);
}

bool selftest_quiesce_stress(const char **reason)
{
    unsigned threads0 = thread_count();
    unsigned cpu = other_cpu();
    struct stress_shared sh = { 0 };
    struct stress_reader readers[CONFIG_MAX_CPUS];
    struct thread *th[CONFIG_MAX_CPUS];
    unsigned nr = 0;

    struct stress_obj *first = kzalloc(sizeof(*first));
    CHECK(first != NULL);
    first->magic = MAGIC_LIVE;
    sh.cur = first;

    for (unsigned c = 1; c < cpu_count(); c++) {
        if (!cpu_online(c))
            continue;
        readers[nr] = (struct stress_reader){ .sh = &sh };
        th[nr] = thread_create_on(stress_reader_main, &readers[nr], "qstress", SCHED_PRIO_DEFAULT, CPUMASK_OF(c));
        CHECK(th[nr] != NULL);
        nr++;
    }

    unsigned syncs = 0, calls = 0, gen = 0;
    g_stress_freed = 0;
    uint64_t end = clock_now_ns() + MS(cpu ? 400 : 100);
    while (clock_now_ns() < end) {
        struct stress_obj *n = kzalloc(sizeof(*n));
        CHECK(n != NULL);
        n->magic = MAGIC_LIVE;
        n->gen = ++gen;
        struct stress_obj *old = sh.cur;
        __atomic_store_n(&sh.cur, n, __ATOMIC_RELEASE);
        if (gen & 1) {
            synchronize_quiesce();
            old->magic = MAGIC_DEAD;
            kfree(old);
            syncs++;
        } else {
            struct stress_free *f = kzalloc(sizeof(*f));
            CHECK(f != NULL);
            f->obj = old;
            call_quiesce(&f->head, stress_free_fn);
            calls++;
        }
    }

    __atomic_store_n(&sh.stop, 1u, __ATOMIC_RELEASE);
    unsigned long reads = 0, bad = 0;
    for (unsigned i = 0; i < nr; i++) {
        thread_join(th[i]);
        reads += readers[i].reads;
        bad += readers[i].bad;
    }
    /* Drain the deferred frees. */
    end = clock_now_ns() + MS(2000);
    while (__atomic_load_n(&g_stress_freed, __ATOMIC_ACQUIRE) < calls && clock_now_ns() < end)
        thread_sleep_ns(MS(1));
    CHECK(g_stress_freed == calls);
    synchronize_quiesce();
    kfree(sh.cur);

    CHECK(bad == 0);
    CHECK(syncs > 0 && calls > 0);
    if (nr > 0)
        CHECK(reads > 0);
    for (unsigned c = 0; c < cpu_count(); c++)
        CHECK(quiesce_cpu_depth(c) == 0);   /* every read section closed */
    CHECK(threads_settle(threads0));
    kinfo("selftest: quiesce-stress: %u readers made %lu reads across %u synchronize + %u call_quiesce generations",
          nr, reads, syncs, calls);
    return true;
}

/*
 * A grace period ends by being woken, not by waiting out a deadline
 * (invariant Q-W). The observable is how the block ENDED, because on more
 * than one CPU the waiter always blocks: quiesce_core_begin bumps the
 * epoch and nobody has published the new one yet. "It did not sleep"
 * would be a test that fails on the change it exists to prove.
 *
 * This call's own timeouts, not the machine's. The first version sampled
 * quiesce_stats.gp_timeouts before and after, which counts every CPU's
 * grace periods -- so a concurrent one on another thread failed it, and
 * the failure looked exactly like the wake not working. Three runs were
 * spent chasing that before the counter was made per-call.
 *
 * Not a duration. A loaded host makes a grace period slower; it does not
 * make it end at a deadline, because the deadline is only reached when no
 * CPU published for two milliseconds -- which is what a missing wake
 * looks like and what a busy machine does not.
 */
bool selftest_quiesce_wake(const char **reason)
{
    if (cpu_count() < 2) {
        kinfo("selftest: quiesce-wake: one CPU, so a grace period completes before it can block; "
              "the wake has nothing to do here and this says so rather than passing quietly");
        return true;
    }
    /*
     * The observable is whether the wake FIRES, not how long a grace
     * period takes. Two earlier versions of this test asserted timing in
     * disguise and both were unsound:
     *
     *   - "gp_timeouts did not move" reads a machine-wide counter, so
     *     another thread's grace period failed it;
     *   - "ten grace periods cost fewer than ten deadline-ends" looked
     *     like a counting argument and is not one. With the wake a grace
     *     period still usually reaches its first deadline -- the other
     *     CPUs' tick is 4 ms away and the deadline is 2 ms -- and then is
     *     woken. Without the wake the timer's own wake re-checks the
     *     condition, which by then is often true, so the wait returns
     *     "condition met" and counts no timeout at all. The number
     *     measures where the ticks fell, not whether the wake works.
     *
     * gp_wakes counts wakes delivered to a queued waiter. It is
     * identically zero unless the wake path runs, and non-zero as soon as
     * any CPU publishes from a preemptible context while a waiter is
     * blocked -- which, with three other CPUs and a 4 ms tick, happens
     * inside a grace period of a few milliseconds. The duration is
     * reported and not asserted, because that is the part a loaded host
     * changes.
     */
    struct quiesce_stats before, after;
    quiesce_get_stats(&before);
    uint64_t t0 = clock_now_ns();
    unsigned n = 10;
    for (unsigned i = 0; i < n; i++)
        synchronize_quiesce();
    uint64_t per = (clock_now_ns() - t0) / n;
    quiesce_get_stats(&after);
    CHECK(after.gp_wakes > before.gp_wakes);
    kinfo("selftest: quiesce-wake: %u grace periods on %u CPUs, %llu wakes delivered to waiters, "
          "%llu us each (polling measured 4.3-7.5 ms)", n, cpu_count(),
          (unsigned long long)(after.gp_wakes - before.gp_wakes), (unsigned long long)(per / 1000));
    return true;
}
