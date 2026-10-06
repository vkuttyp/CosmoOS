/*
 * lockdeptest.c - Boot-time self-tests of the lock-order checker
 * (docs/kernel/lockdep/testing.md).
 *
 * Each test arms one expectation (lockdep_expect), performs the violation
 * on private locks, and checks that exactly one report of that kind was
 * counted. The violating acquisition proceeds, so every lock is released
 * in the normal way afterwards. In release builds the checker is compiled
 * out and the tests report that.
 */

#include <kernel/interrupt.h>
#include <kernel/kmalloc.h>
#include <kernel/lockdep.h>
#include <kernel/log.h>
#include <kernel/mutex.h>
#include <kernel/panic.h>
#include <kernel/printf.h>
#include <kernel/sched.h>
#include <kernel/percpu.h>
#include <kernel/selftest.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/timer.h>

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

#if CONFIG_LOCKDEP

/* --- lockdep-order: A -> B recorded, then B -> A is an inversion --- */

bool selftest_lockdep_order(const char **reason)
{
    static spinlock_t a = SPINLOCK_INIT("lockdep-test-a");
    static spinlock_t b = SPINLOCK_INIT("lockdep-test-b");
    struct lockdep_stats s0, s1;
    lockdep_get_stats(&s0);

    /* The legal order, twice: the second time no search runs. */
    arch_irq_state_t s = spin_lock_irqsave(&a);
    spin_lock(&b);
    CHECK(lockdep_is_held(&a, LOCKDEP_KIND_SPIN) && lockdep_is_held(&b, LOCKDEP_KIND_SPIN));
    spin_unlock(&b);
    spin_unlock_irqrestore(&a, s);
    CHECK(!lockdep_is_held(&a, LOCKDEP_KIND_SPIN));
    lockdep_get_stats(&s1);
    CHECK(s1.edges >= s0.edges + 1);
    CHECK(s1.acquisitions >= s0.acquisitions + 2);
    uint64_t searches = s1.searches;
    s = spin_lock_irqsave(&a);
    spin_lock(&b);
    spin_unlock(&b);
    spin_unlock_irqrestore(&a, s);
    lockdep_get_stats(&s1);
    CHECK(s1.searches == searches);   /* known edge: no search */

    /* The inversion. */
    lockdep_expect(LOCKDEP_R_INVERSION);
    s = spin_lock_irqsave(&b);
    spin_lock(&a);
    unsigned hits = lockdep_expected_hits();
    spin_unlock(&a);
    spin_unlock_irqrestore(&b, s);
    CHECK(hits == 1);

    /* Releasing out of order is legal. */
    s = spin_lock_irqsave(&a);
    spin_lock(&b);
    struct lockdep_held copy[LOCKDEP_MAX_HELD];
    unsigned count;
    unsigned cpu = raw_cpu_id();
    bool captured = lockdep_snapshot_held_cpu(cpu, copy, &count) && count == 2 &&
                    copy[0].lock == &a && copy[1].lock == &b &&
                    (copy[0].flags & LOCKDEP_HF_IRQSAVE) &&
                    (((copy[0].flags & LOCKDEP_HF_IRQSAVE_ON) != 0) == arch_irq_state_enabled(s));
    spin_unlock(&a);
    bool shifted = lockdep_snapshot_held_cpu(cpu, copy, &count) && count == 1 && copy[0].lock == &b;
    CHECK(!lockdep_is_held(&a, LOCKDEP_KIND_SPIN) && lockdep_is_held(&b, LOCKDEP_KIND_SPIN));
    spin_unlock(&b);
    bool empty = lockdep_snapshot_held_cpu(cpu, copy, &count) && count == 0;
    arch_irq_restore(s);
    CHECK(captured && shifted && empty);

    /* A long cycle uses the real hooks, not the host decision model.
     * Each pair is observed independently so closing the chain needs a
     * transitive search longer than the diagnostic's eight-node buffer. */
    static spinlock_t chain[10];
    static const char *const names[] = {
        "lockdep-chain-0", "lockdep-chain-1", "lockdep-chain-2", "lockdep-chain-3", "lockdep-chain-4",
        "lockdep-chain-5", "lockdep-chain-6", "lockdep-chain-7", "lockdep-chain-8", "lockdep-chain-9",
    };
    for (unsigned i = 0; i < 10; i++)
        spinlock_init(&chain[i], names[i]);
    for (unsigned i = 0; i < 9; i++) {
        s = spin_lock_irqsave(&chain[i]);
        spin_lock(&chain[i + 1]);
        spin_unlock(&chain[i + 1]);
        spin_unlock_irqrestore(&chain[i], s);
    }
    s = spin_lock_irqsave(&chain[9]);
    lockdep_expect(LOCKDEP_R_INVERSION);
    spin_lock_check_order(&chain[0]);
    hits = lockdep_expected_hits();
    spin_unlock_irqrestore(&chain[9], s);
    CHECK(hits == 1);

    /* Validate ownership reporting without unlocking an actual unowned
     * primitive or changing preemption state. */
    lockdep_get_stats(&s0);
    lockdep_expect(LOCKDEP_R_UNHELD);
    s = arch_irq_save();
    lockdep_release(&chain[0], LOCKDEP_KIND_SPIN, (uintptr_t)__builtin_return_address(0), false);
    hits = lockdep_expected_hits();
    arch_irq_restore(s);
    CHECK(hits == 1);
    lockdep_get_stats(&s1);
    CHECK(s1.reports >= s0.reports + 1);
    kinfo("selftest: lockdep-order: %u classes, %u edges, %llu acquisitions, %llu searches so far", s1.classes,
          s1.edges, (unsigned long long)s1.acquisitions, (unsigned long long)s1.searches);
    return true;
}

/* --- lockdep-recursion: two locks of one class nest only with an annotation --- */

bool selftest_lockdep_recursion(const char **reason)
{
    static spinlock_t pair[2];
    static bool init;
    if (!init) {
        spinlock_init(&pair[0], "lockdep-test-pair");   /* one site: one class */
        spinlock_init(&pair[1], "lockdep-test-pair");
        init = true;
    }
    lockdep_expect(LOCKDEP_R_RECURSION);
    arch_irq_state_t s = spin_lock_irqsave(&pair[0]);
    spin_lock(&pair[1]);
    unsigned hits = lockdep_expected_hits();
    spin_unlock(&pair[1]);
    spin_unlock_irqrestore(&pair[0], s);
    CHECK(hits == 1);

    /* Annotated nesting: no report. */
    s = spin_lock_irqsave(&pair[0]);
    spin_lock_nested(&pair[1], 1);
    spin_unlock(&pair[1]);
    spin_unlock_irqrestore(&pair[0], s);
    CHECK(lockdep_expected_hits() == 0);
    return true;
}

/* --- lockdep-irq: a lock held with interrupts enabled and taken in an interrupt --- */

struct irq_probe {
    spinlock_t *lock;
    bool trylock;
    bool got;
    unsigned done;
};

static void irq_lock_handler(unsigned vector, struct arch_trap_frame *frame, void *arg)
{
    (void)vector;
    (void)frame;
    struct irq_probe *p = arg;
    if (p->trylock)
        p->got = spin_trylock(p->lock);
    else {
        spin_lock(p->lock);
        p->got = true;
    }
    if (p->got)
        spin_unlock(p->lock);
    __atomic_store_n(&p->done, 1u, __ATOMIC_RELEASE);
}

/* A real self-IPI. Unregister and free even when an assertion fails so
 * the stack-backed handler argument never survives the test. */
static bool irq_probe_run(spinlock_t *lock, bool trylock, bool held, unsigned expected, const char **reason)
{
    struct irq_probe p = { .lock = lock, .trylock = trylock };
    int vec = arch_vector_alloc();
    CHECK(vec >= 0);
    int rc = interrupt_register((unsigned)vec, irq_lock_handler, &p, "selftest-lockdep-irq");
    if (rc != 0) {
        arch_vector_free((unsigned)vec);
        CHECK(rc == 0);
    }
    arch_ipi_bind((unsigned)vec);
    if (expected)
        lockdep_expect(LOCKDEP_R_IRQ);
    if (held)
        spin_lock(lock);   /* only used with a nonblocking handler */
    arch_ipi_send(raw_cpu_id(), (unsigned)vec);
    uint64_t end = clock_now_ns() + 1000000000ULL;
    while (__atomic_load_n(&p.done, __ATOMIC_ACQUIRE) == 0 && clock_now_ns() < end)
        arch_cpu_relax();
    bool done = __atomic_load_n(&p.done, __ATOMIC_ACQUIRE) != 0;
    if (held)
        spin_unlock(lock);
    unsigned hits = lockdep_expected_hits();
    rc = interrupt_unregister_sync((unsigned)vec, irq_lock_handler);
    arch_vector_free((unsigned)vec);
    CHECK(rc == 0);
    CHECK(done);
    CHECK(p.got == !held);
    CHECK(hits == expected);
    return true;
}

static void observe_pair(spinlock_t *a, spinlock_t *b)
{
    arch_irq_state_t s = spin_lock_irqsave(a);
    spin_lock(b);
    spin_unlock(b);
    spin_unlock_irqrestore(a, s);
}

bool selftest_lockdep_irq(const char **reason)
{
    static spinlock_t direct = SPINLOCK_INIT("lockdep-test-irq");
    static spinlock_t tryheld = SPINLOCK_INIT("lockdep-irq-try-held");
    static spinlock_t irqtry = SPINLOCK_INIT("lockdep-irq-try-handler");
    CHECK(arch_irq_enabled());
    spin_lock(&direct);
    spin_unlock(&direct);
    CHECK(irq_probe_run(&direct, false, false, 1, reason));

    /* Successful thread trylock must be classified. IRQ trylock, both
     * successful and failed against an interrupted holder, must not be
     * mistaken for a blocking IRQ acquisition. */
    CHECK(spin_trylock(&tryheld));
    spin_unlock(&tryheld);
    CHECK(irq_probe_run(&tryheld, false, false, 1, reason));
    spin_lock(&irqtry);
    spin_unlock(&irqtry);
    CHECK(irq_probe_run(&irqtry, true, false, 0, reason));
    CHECK(irq_probe_run(&irqtry, true, true, 0, reason));

    static spinlock_t a[3], b[3], c[3];
    static const char *const an[] = { "irq-edge-a", "irq-safe-last-a", "irq-unsafe-last-a" };
    static const char *const bn[] = { "irq-edge-b", "irq-safe-last-b", "irq-unsafe-last-b" };
    static const char *const cn[] = { "irq-edge-c", "irq-safe-last-c", "irq-unsafe-last-c" };
    for (unsigned i = 0; i < 3; i++) {
        spinlock_init(&a[i], an[i]);
        spinlock_init(&b[i], bn[i]);
        spinlock_init(&c[i], cn[i]);
        if (i != 0)
            observe_pair(&a[i], &b[i]);
        observe_pair(&b[i], &c[i]);
    }
    /* Edge last: IRQ A, B -> C, IRQ-enabled C; new A -> B is invalid. */
    CHECK(irq_probe_run(&a[0], false, false, 0, reason));
    spin_lock(&c[0]);
    spin_unlock(&c[0]);
    arch_irq_state_t saved = spin_lock_irqsave(&a[0]);
    lockdep_expect(LOCKDEP_R_IRQ);
    spin_lock_check_order(&b[0]);
    unsigned hits = lockdep_expected_hits();
    spin_unlock_irqrestore(&a[0], saved);
    CHECK(hits == 1);

    /* Safe label last, with both dependency edges already observed. */
    spin_lock(&c[1]);
    spin_unlock(&c[1]);
    CHECK(irq_probe_run(&a[1], false, false, 1, reason));

    /* Unsafe label last, via successful trylock. */
    CHECK(irq_probe_run(&a[2], false, false, 0, reason));
    lockdep_expect(LOCKDEP_R_IRQ);
    CHECK(spin_trylock(&c[2]));
    spin_unlock(&c[2]);
    CHECK(lockdep_expected_hits() == 1);

    /* A restore must match the IRQ state saved with this exact lock.
     * These probes call the checker without actually enabling interrupts
     * under a held lock; the real, matching restore follows each probe. */
    static spinlock_t restore = SPINLOCK_INIT("lockdep-irq-restore");
    arch_irq_state_t rs = spin_lock_irqsave(&restore);
    lockdep_expect(LOCKDEP_R_IRQ_STATE);
    lockdep_irqrestore_check(&restore, !arch_irq_state_enabled(rs),
                             (uintptr_t)__builtin_return_address(0));
    CHECK(lockdep_expected_hits() == 1);
    spin_unlock_irqrestore(&restore, rs);

    static spinlock_t nested = SPINLOCK_INIT("lockdep-irq-restore-nested");
    static spinlock_t inner = SPINLOCK_INIT("lockdep-irq-restore-inner");
    rs = spin_lock_irqsave(&nested);
    spin_lock(&inner);
    lockdep_expect(LOCKDEP_R_IRQ_STATE);
    lockdep_irqrestore_check(&nested, arch_irq_state_enabled(rs),
                             (uintptr_t)__builtin_return_address(0));
    CHECK(lockdep_expected_hits() == 1);
    spin_unlock(&inner);
    spin_unlock_irqrestore(&nested, rs);

    rs = spin_lock_irqsave(&inner);
    /* A valid restore must run with no expectation armed: otherwise a
     * later real IRQ-state violation could consume this unused probe. */
    lockdep_irqrestore_check(&inner, arch_irq_state_enabled(rs),
                             (uintptr_t)__builtin_return_address(0));
    CHECK(lockdep_expected_hits() == 0);
    spin_unlock_irqrestore(&inner, rs);
    return true;
}

/* --- lockdep-sleep: might_sleep under a spinlock --- */

bool selftest_lockdep_sleep(const char **reason)
{
    static spinlock_t l = SPINLOCK_INIT("lockdep-test-sleep");
    lockdep_expect(LOCKDEP_R_SLEEP);
    arch_irq_state_t s = spin_lock_irqsave(&l);
    might_sleep();
    spin_unlock_irqrestore(&l, s);
    CHECK(lockdep_expected_hits() == 1);
    might_sleep();   /* nothing held: silent */
    CHECK(lockdep_expected_hits() == 0);
    return true;
}

/* --- lockdep-mutex: the per-thread stack and mutex ordering --- */

struct mutex_snapshot_probe {
    struct mutex a, b;
    unsigned ready, proceed, done;
};

static void mutex_snapshot_writer(void *arg)
{
    struct mutex_snapshot_probe *p = arg;
    mutex_lock(&p->a);
    mutex_lock(&p->b);
    __atomic_store_n(&p->ready, 1u, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&p->proceed, __ATOMIC_ACQUIRE))
        sched_yield();
    mutex_unlock(&p->a);
    mutex_unlock(&p->b);
    for (unsigned i = 0; i < 1024; i++) {
        mutex_lock(&p->a);
        mutex_lock(&p->b);
        mutex_unlock(&p->a); /* shift the remaining entry */
        mutex_unlock(&p->b);
        if (!(i % 8))
            sched_yield();
    }
    __atomic_store_n(&p->done, 1u, __ATOMIC_RELEASE);
}

static bool test_remote_mutex_snapshot(const char **reason)
{
    struct mutex_snapshot_probe p = {0};
    mutex_init(&p.a, "snapshot-thread-a");
    mutex_init(&p.b, "snapshot-thread-b");
    struct thread *t = thread_create(mutex_snapshot_writer, &p, "snapshot-writer", SCHED_PRIO_DEFAULT);
    CHECK(t != NULL);
    struct lockdep_held copy[LOCKDEP_MAX_HELD_MUTEX];
    unsigned count;
    uint64_t start = clock_now_ns();
    while (!__atomic_load_n(&p.ready, __ATOMIC_ACQUIRE) && clock_since_ns(start) < 1000000000ULL)
        sched_yield();
    bool ok = __atomic_load_n(&p.ready, __ATOMIC_ACQUIRE) &&
              lockdep_snapshot_held_thread(t, copy, &count) && count == 2 &&
              copy[0].lock == &p.a && copy[1].lock == &p.b;
    __atomic_store_n(&p.proceed, 1u, __ATOMIC_RELEASE);
    start = clock_now_ns();
    unsigned accepted = 0;
    while (!__atomic_load_n(&p.done, __ATOMIC_ACQUIRE) && clock_since_ns(start) < 2000000000ULL) {
        if (lockdep_snapshot_held_thread(t, copy, &count)) {
            accepted++;
            ok = ok && count <= 2;
            if (count == 1)
                ok = ok && (copy[0].lock == &p.a || copy[0].lock == &p.b);
            if (count == 2)
                ok = ok && copy[0].lock == &p.a && copy[1].lock == &p.b;
        } else {
            ok = ok && count == 0;
        }
        sched_yield();
    }
    /* A stuck worker may still use p and its mutexes. Do not join it
     * without a completion acknowledgement, or return and expire p. */
    if (!__atomic_load_n(&p.done, __ATOMIC_ACQUIRE))
        panic("selftest lockdep-mutex: worker completion timeout; retaining thread and probe");
    /* The creator reference covers the reads above. Retain another
     * reference before join drops it, then inspect the exited object. */
    thread_get(t);
    thread_join(t);
    bool empty = lockdep_snapshot_held_thread(t, copy, &count) && count == 0;
    thread_put(t);
    CHECK(ok && empty);
    kinfo("selftest: lockdep-mutex: remote initial pair, %u concurrent snapshots, exited stack empty", accepted);
    return true;
}

bool selftest_lockdep_mutex(const char **reason)
{
    static struct mutex m1, m2;
    static spinlock_t under = SPINLOCK_INIT("lockdep-test-under");
    static bool init;
    if (!init) {
        mutex_init(&m1, "lockdep-test-m1");
        mutex_init(&m2, "lockdep-test-m2");
        init = true;
    }
    mutex_lock(&m1);
    CHECK(lockdep_is_held(&m1, LOCKDEP_KIND_MUTEX));
    mutex_lock(&m2);
    arch_irq_state_t s = spin_lock_irqsave(&under);   /* spinlock under mutexes: legal */
    spin_unlock_irqrestore(&under, s);
    mutex_unlock(&m2);
    mutex_unlock(&m1);
    CHECK(!lockdep_is_held(&m1, LOCKDEP_KIND_MUTEX));

    lockdep_expect(LOCKDEP_R_INVERSION);
    mutex_lock(&m2);
    mutex_lock(&m1);
    unsigned hits = lockdep_expected_hits();
    mutex_unlock(&m1);
    mutex_unlock(&m2);
    CHECK(hits == 1);

    /* Snapshot actual mutex publication, trylock metadata and removal
     * out of order. A busy writer is refused without waiting for us. */
    struct thread *self = thread_current();
    struct lockdep_held copy[LOCKDEP_MAX_HELD_MUTEX];
    unsigned count;
    CHECK(!lockdep_snapshot_held_thread(NULL, copy, &count) && count == 0);
    CHECK(mutex_trylock(&m1));
    mutex_lock(&m2);
    bool snap = lockdep_snapshot_held_thread(self, copy, &count);
    bool pair = snap && count == 2 && copy[0].lock == &m1 && copy[1].lock == &m2 &&
                (copy[0].flags & LOCKDEP_HF_TRYLOCK);
    mutex_unlock(&m1);
    snap = lockdep_snapshot_held_thread(self, copy, &count);
    bool shifted = snap && count == 1 && copy[0].lock == &m2;
    lockdep_core_held_begin(&self->held_mutex_seq);
    bool busy = !lockdep_snapshot_held_thread(self, copy, &count) && count == 0;
    lockdep_core_held_end(&self->held_mutex_seq);
    mutex_unlock(&m2);
    CHECK(pair && shifted && busy);
    CHECK(lockdep_snapshot_held_thread(self, copy, &count) && count == 0);

    /* A mutex under a spinlock is a sleep report (might_sleep in
     * mutex_lock). A fresh spinlock and mutex, so no recorded order is
     * involved: the sleep report is the only one. */
    static spinlock_t under2 = SPINLOCK_INIT("lockdep-test-under2");
    static struct mutex m3;
    static bool init3;
    if (!init3) {
        mutex_init(&m3, "lockdep-test-m3");
        init3 = true;
    }
    lockdep_expect(LOCKDEP_R_SLEEP);
    s = spin_lock_irqsave(&under2);
    mutex_lock(&m3);   /* uncontended: acquired without waiting */
    mutex_unlock(&m3);
    spin_unlock_irqrestore(&under2, s);
    CHECK(lockdep_expected_hits() == 1);
    return test_remote_mutex_snapshot(reason);
}

/* --- lockdep-contention: a lock waited for is not held ---
 *
 * Another CPU holds L until the callback observes this CPU's real wait.
 * This CPU spins on a plain spin_lock(L) with
 * interrupts enabled while a timer callback here takes M. If the checker
 * pushed L before owning it, the callback would record L -> M; the
 * legitimate M -> L order taken afterwards would then be an inversion.
 */
static spinlock_t g_cont_l = SPINLOCK_INIT("lockdep-test-cont-l");
static spinlock_t g_cont_m = SPINLOCK_INIT("lockdep-test-cont-m");
static unsigned g_cont_holding, g_cont_timer_ran;
static unsigned g_cont_ready, g_cont_start;
static spinlock_t g_cont_nested = SPINLOCK_INIT("lockdep-test-cont-nested");
static unsigned g_cont_cpu, g_cont_nested_seen;

static void cont_holder(void *arg)
{
    (void)arg;
    preempt_disable();
    __atomic_store_n(&g_cont_ready, 1u, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&g_cont_start, __ATOMIC_ACQUIRE))
        arch_cpu_relax();
    arch_irq_state_t s = spin_lock_irqsave(&g_cont_l);
    preempt_enable(); /* the held lock now keeps this CPU nonpreemptible */
    spin_lock(&g_cont_nested);
    __atomic_store_n(&g_cont_holding, 1u, __ATOMIC_RELEASE);
    /* This CPU cannot migrate while holding L. Do not use the deadline
     * clock's tick fallback: both test CPUs can have IRQs masked. */
    uint64_t begin = clock_raw_ns();
    while (!spin_test_waiting_on(g_cont_cpu, &g_cont_nested) && clock_raw_ns() - begin < 1000000000ULL)
        arch_cpu_relax();
    __atomic_store_n(&g_cont_nested_seen, spin_test_waiting_on(g_cont_cpu, &g_cont_nested), __ATOMIC_RELEASE);
    spin_unlock(&g_cont_nested);
    while (!__atomic_load_n(&g_cont_timer_ran, __ATOMIC_ACQUIRE) && clock_raw_ns() - begin < 1000000000ULL)
        arch_cpu_relax();
    spin_unlock_irqrestore(&g_cont_l, s);
}

static void cont_timer(struct timer *t, void *arg)
{
    (void)arg;
    if (!spin_test_waiting_on(arch_cpu_id(), &g_cont_l)) {
        timer_start(t, 1000000ULL); /* a callback before the wait proves nothing */
        return;
    }
    spin_lock(&g_cont_nested); /* a real nested wait temporarily replaces L */
    spin_unlock(&g_cont_nested);
    KASSERT(spin_test_waiting_on(arch_cpu_id(), &g_cont_l));
    spin_lock(&g_cont_m);   /* interrupt context, during the contention on L */
    spin_unlock(&g_cont_m);
    KASSERT(spin_test_waiting_on(arch_cpu_id(), &g_cont_l));
    __atomic_store_n(&g_cont_timer_ran, 1u, __ATOMIC_RELEASE);
}

static bool selftest_lockdep_contention_pinned(const char **reason)
{
    unsigned me = arch_cpu_id(), other = me;   /* pinned by the wrapper */
    for (unsigned i = 1; i < cpu_count(); i++)
        if (cpu_online((me + i) % cpu_count())) {
            other = (me + i) % cpu_count();
            break;
        }
    if (other == me) {
        kinfo("selftest: lockdep-contention: one CPU, nothing to contend with");
        return true;
    }
    /* Check preconditions before creating a worker or publishing a timer.
     * Every returning path after publication must drain those lifetimes. */
    CHECK(arch_irq_enabled());
    __atomic_store_n(&g_cont_holding, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_cont_timer_ran, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_cont_ready, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_cont_start, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_cont_nested_seen, 0u, __ATOMIC_RELAXED);
    g_cont_cpu = me;
    struct thread *h = thread_create_on(cont_holder, NULL, "cont-holder", SCHED_PRIO_DEFAULT, CPUMASK_OF(other));
    CHECK(h != NULL);
    uint64_t end = clock_deadline_ns(1000000000ULL);
    while (!__atomic_load_n(&g_cont_ready, __ATOMIC_ACQUIRE) && !clock_deadline_passed(end))
        sched_yield();
    if (!__atomic_load_n(&g_cont_ready, __ATOMIC_ACQUIRE))
        panic("selftest lockdep-contention: holder readiness timeout; retaining thread");
    /* Both participants must be running before the holder masks IRQs.
     * Otherwise a reaper on this CPU can wait for that holder's TLB ack
     * while the holder waits for this thread to start its callback. */
    preempt_disable();
    __atomic_store_n(&g_cont_start, 1u, __ATOMIC_RELEASE);
    end = clock_deadline_ns(1000000000ULL);
    while (__atomic_load_n(&g_cont_holding, __ATOMIC_ACQUIRE) == 0 && !clock_deadline_passed(end))
        arch_cpu_relax();
    if (!__atomic_load_n(&g_cont_holding, __ATOMIC_ACQUIRE))
        panic("selftest lockdep-contention: holder readiness timeout; retaining thread");

    struct timer t;
    timer_setup(&t, cont_timer, NULL);
    timer_start(&t, 5000000ULL);   /* fires on this CPU while we spin below */
    spin_lock(&g_cont_l);          /* holder releases only after the observed callback, or its guard */
    preempt_enable();             /* the acquired lock replaces the startup pin */
    /* Preserve the observation before cleanup: a late timer callback
     * must not turn a missed-window failure into a pass. */
    bool timer_ran = __atomic_load_n(&g_cont_timer_ran, __ATOMIC_ACQUIRE) == 1;
    spin_unlock(&g_cont_l);
    timer_cancel_sync(&t);        /* stack timer cannot outlive this frame */
    if (!wait_for_completion_timeout(&h->exited, 1000000000ULL))
        panic("selftest lockdep-contention: holder exit timeout; retaining thread");
    thread_join(h);
    CHECK(!spin_test_waiting_on(me, &g_cont_l));
    CHECK(timer_ran);             /* only now may a failure return safely */
    CHECK(__atomic_load_n(&g_cont_nested_seen, __ATOMIC_ACQUIRE) == 1);

    /* No phantom L -> M may have been recorded: that would report an
     * inversion. M -> L itself is now rejected as IRQ-used -> IRQ-enabled,
     * so check the order without performing a potentially unsafe wait. */
    arch_irq_state_t s = spin_lock_irqsave(&g_cont_m);
    lockdep_expect(LOCKDEP_R_IRQ);
    spin_lock_check_order(&g_cont_l);
    unsigned hits = lockdep_expected_hits();
    spin_unlock_irqrestore(&g_cont_m, s);
    CHECK(hits == 1);
    return true;
}

/* Pinned for the whole test: "another CPU than mine" is a claim about
 * this thread's CPU that must outlive its sleeps (S25); a holder or a
 * spinner parked on that other CPU must never find this thread queued
 * behind it. */
bool selftest_lockdep_contention(const char **reason)
{
    cpumask_t saved = thread_pin_self();
    bool r = selftest_lockdep_contention_pinned(reason);
    thread_set_affinity_self(saved);
    return r;
}


/*
 * Callback classes (design.md, "Callback classes"): a synchronous wait for
 * a timer callback is checked against every callback of that function the
 * graph has seen, transitively and in either order. Each case below is one
 * the per-object profile (lockdep_timer_cancel_check) cannot see: it only
 * knows the locks taken by the callback execution in progress on the very
 * timer being cancelled.
 */
static spinlock_t g_cb_a = SPINLOCK_INIT("lockdep-cb-a");
static spinlock_t g_cb_b1 = SPINLOCK_INIT("lockdep-cb-b1");
static spinlock_t g_cb_b2 = SPINLOCK_INIT("lockdep-cb-b2");
static spinlock_t g_cb_c = SPINLOCK_INIT("lockdep-cb-c");
static spinlock_t g_cb_d = SPINLOCK_INIT("lockdep-cb-d");
static unsigned g_cb_ran;

static void cb_take(spinlock_t *l)
{
    spin_lock(l);
    spin_unlock(l);
    __atomic_fetch_add(&g_cb_ran, 1u, __ATOMIC_RELEASE);
}
static void cb_a(struct timer *t, void *arg) { (void)t; (void)arg; cb_take(&g_cb_a); }
static void cb_b(struct timer *t, void *arg) { (void)t; (void)arg; cb_take(&g_cb_b1); }
static void cb_c(struct timer *t, void *arg) { (void)t; (void)arg; cb_take(&g_cb_c); }

/* Arm on this (pinned) CPU, wait for the callback, then synchronise with
 * the queue's tail holding nothing, so the stack timer may be reused. */
static bool cb_run_once(struct timer *t)
{
    unsigned before = __atomic_load_n(&g_cb_ran, __ATOMIC_ACQUIRE);
    timer_start(t, 1000000ULL);
    uint64_t end = clock_now_ns() + 1000000000ULL;
    while (__atomic_load_n(&g_cb_ran, __ATOMIC_ACQUIRE) == before) {
        if (clock_now_ns() > end) {
            /* The timer lives on the caller's stack: it must be idle and
             * its callback finished before the failure unwinds the frame. */
            (void)timer_cancel_sync(t);
            return false;
        }
        thread_sleep_ms(1);
    }
    (void)timer_cancel_sync(t);
    return true;
}

static bool selftest_lockdep_callback_pinned(const char **reason)
{
    struct timer t1, t2;
    arch_irq_state_t s;
    bool pending;
    unsigned hits;

    /* 1. The same function, another timer -- armed, never run, not
     * running: a held lock that any cb_a callback has taken is a
     * dependency. The report skips only the wait: the timer is still
     * cancelled and the result still says it was pending. */
    timer_setup(&t1, cb_a, NULL);
    CHECK(cb_run_once(&t1));
    timer_setup(&t2, cb_a, NULL);
    timer_start(&t2, 500000000ULL);   /* armed, far off: the report must still cancel it */
    s = spin_lock_irqsave(&g_cb_a);
    lockdep_expect(LOCKDEP_R_CALLBACK);
    pending = timer_cancel_sync(&t2);
    hits = lockdep_expected_hits();
    spin_unlock_irqrestore(&g_cb_a, s);
    bool idle = t2.state == TIMER_IDLE;
    /* Before any check can return: t2 lives in this frame, and a cancel
     * that went wrong may have left it armed. No lock is held now, so
     * this cancel reports nothing and leaves it idle either way. */
    (void)timer_cancel_sync(&t2);
    CHECK(pending);                     /* it was pending, and the report did not hide that */
    CHECK(idle);
    CHECK(hits == 1);

    /* No false positive: a lock no cb_a callback reaches. */
    s = spin_lock_irqsave(&g_cb_d);
    (void)timer_cancel_sync(&t2);
    spin_unlock_irqrestore(&g_cb_d, s);

    /* 2. Transitive: cb_b takes b1, and b1 -> b2 is recorded elsewhere,
     * so holding b2 across the wait closes b2 -> cb_b -> b1 -> b2. The
     * callback never took b2. */
    timer_setup(&t1, cb_b, NULL);
    CHECK(cb_run_once(&t1));
    s = spin_lock_irqsave(&g_cb_b1);
    spin_lock(&g_cb_b2);
    spin_unlock(&g_cb_b2);
    spin_unlock_irqrestore(&g_cb_b1, s);
    s = spin_lock_irqsave(&g_cb_b2);
    lockdep_expect(LOCKDEP_R_CALLBACK);
    pending = timer_cancel_sync(&t1);
    hits = lockdep_expected_hits();
    spin_unlock_irqrestore(&g_cb_b2, s);
    CHECK(!pending);
    CHECK(hits == 1);

    /* 3. The other order: the wait comes first, with no callback of cb_c
     * ever run, so nothing is known and nothing is reported. The first
     * callback then takes c under its class, closing c -> cb_c -> c --
     * an inversion reported in the callback itself. */
    timer_setup(&t1, cb_c, NULL);
    s = spin_lock_irqsave(&g_cb_c);
    pending = timer_cancel_sync(&t1);
    spin_unlock_irqrestore(&g_cb_c, s);
    CHECK(!pending);
    lockdep_expect(LOCKDEP_R_INVERSION);
    CHECK(cb_run_once(&t1));
    CHECK(lockdep_expected_hits() == 1);

    kinfo("selftest: lockdep-callback: a wait is checked against every observed callback of the function: "
          "another timer, a transitive lock, and the callback after the wait all reported");
    return true;
}

/* Pinned: expectations are per CPU, and timer_start arms the timer on the
 * caller's CPU, so case 3's report in the callback lands on this CPU. */
bool selftest_lockdep_callback(const char **reason)
{
    cpumask_t saved = thread_pin_self();
    bool r = selftest_lockdep_callback_pinned(reason);
    thread_set_affinity_self(saved);
    return r;
}

/*
 * Raw interrupt-state pairing (design.md, "Raw interrupt-state pairing"):
 * each violation, through the real arch_irq_save/arch_irq_restore, made so
 * that it produces exactly one report.
 */
static void irqp_leaky(void *arg)
{
    (void)arg;
    (void)arch_irq_save();   /* never restored ... */
    arch_irq_enable();       /* ... and interrupts back on, so the exit itself is legal */
}

static bool selftest_lockdep_irq_pairing_pinned(const char **reason)
{
    unsigned here = arch_cpu_id();   /* pinned by the wrapper */
    CHECK(arch_irq_enabled());
    arch_irq_state_t s1, s2;

    /* 1. A restore with nothing outstanding: the pair is complete, and a
     * second restore has no save to undo. */
    s1 = arch_irq_save();
    arch_irq_restore(s1);
    lockdep_expect(LOCKDEP_R_IRQ_STATE);
    arch_irq_restore(s1);   /* restores "enabled", which it already is */
    CHECK(lockdep_expected_hits() == 1);

    /* 2. Out of order: the outer save's state restored while the inner
     * save is innermost. It consumes the innermost slot and enables
     * interrupts; masking again leaves the outer save to be undone
     * cleanly, so this is the only report. */
    s1 = arch_irq_save();
    s2 = arch_irq_save();
    CHECK(arch_irq_state_enabled(s1) && !arch_irq_state_enabled(s2));
    lockdep_expect(LOCKDEP_R_IRQ_STATE);
    arch_irq_restore(s1);
    arch_irq_disable();
    arch_irq_restore(s1);
    CHECK(lockdep_expected_hits() == 1);

    /* 3. Interrupts enabled inside a saved region. */
    s1 = arch_irq_save();
    arch_irq_enable();
    lockdep_expect(LOCKDEP_R_IRQ_STATE);
    arch_irq_restore(s1);
    CHECK(lockdep_expected_hits() == 1);

    /* 4. A thread that exits with a save outstanding, on this CPU so the
     * report lands where the expectation is armed. */
    lockdep_expect(LOCKDEP_R_IRQ_STATE);
    struct thread *t = thread_create_on(irqp_leaky, NULL, "irqp-leak", SCHED_PRIO_DEFAULT, CPUMASK_OF(here));
    CHECK(t != NULL);
    thread_join(t);
    CHECK(lockdep_expected_hits() == 1);

    /* 5. One save past capacity: reported once -- the report's own raw
     * lock saves again at full depth, and must not report (and so save,
     * and report) again -- and every restore still pairs afterwards. */
    arch_irq_state_t deep[LOCKDEP_MAX_IRQ_SAVES + 1];
    lockdep_expect(LOCKDEP_R_OVERFLOW);
    for (unsigned i = 0; i <= LOCKDEP_MAX_IRQ_SAVES; i++)
        deep[i] = arch_irq_save();
    for (unsigned i = LOCKDEP_MAX_IRQ_SAVES + 1; i-- > 0;)
        arch_irq_restore(deep[i]);
    CHECK(lockdep_expected_hits() == 1);

    CHECK(arch_irq_enabled());
    kinfo("selftest: lockdep-irq-pairing: a restore without a save, out of order, with interrupts enabled inside, "
          "a save left at thread exit and a save past capacity were each reported once");
    return true;
}

/* Pinned: expectations are per CPU. */
bool selftest_lockdep_irq_pairing(const char **reason)
{
    cpumask_t saved = thread_pin_self();
    bool r = selftest_lockdep_irq_pairing_pinned(reason);
    thread_set_affinity_self(saved);
    return r;
}

#else

static bool skip(const char **reason, const char *name)
{
    (void)reason;
    kinfo("selftest: %s: lockdep is compiled out of this build", name);
    return true;
}
bool selftest_lockdep_order(const char **reason) { return skip(reason, "lockdep-order"); }
bool selftest_lockdep_recursion(const char **reason) { return skip(reason, "lockdep-recursion"); }
bool selftest_lockdep_irq(const char **reason) { return skip(reason, "lockdep-irq"); }
bool selftest_lockdep_sleep(const char **reason) { return skip(reason, "lockdep-sleep"); }
bool selftest_lockdep_mutex(const char **reason) { return skip(reason, "lockdep-mutex"); }
bool selftest_lockdep_contention(const char **reason) { return skip(reason, "lockdep-contention"); }
bool selftest_lockdep_callback(const char **reason) { return skip(reason, "lockdep-callback"); }
bool selftest_lockdep_irq_pairing(const char **reason) { return skip(reason, "lockdep-irq-pairing"); }

#endif

/* Same workload in debug LOCKDEP=0 and LOCKDEP=1. This measures warmed,
 * uncontended spin/mutex paths, not contended waits or first-edge searches.
 * Keep IRQs and scheduling enabled; pinning makes elapsed counter samples
 * CPU-local, while the distribution exposes interrupt/scheduling noise. */
static spinlock_t g_bench_a = SPINLOCK_INIT("lockdep-bench-a");
static spinlock_t g_bench_b = SPINLOCK_INIT("lockdep-bench-b");
static struct mutex g_bench_mutex;

static void bench_iteration(unsigned kind)
{
    if (kind == 0) {
        __asm__ volatile("" ::: "memory");
    } else if (kind == 1) {
        spin_lock(&g_bench_a);
        spin_unlock(&g_bench_a);
    } else if (kind == 2) {
        arch_irq_state_t s = spin_lock_irqsave(&g_bench_a);
        spin_unlock_irqrestore(&g_bench_a, s);
    } else if (kind == 3) {
        arch_irq_state_t s = spin_lock_irqsave(&g_bench_a);
        spin_lock(&g_bench_b);
        spin_unlock(&g_bench_b);
        spin_unlock_irqrestore(&g_bench_a, s);
    } else if (kind == 4) {
        mutex_lock(&g_bench_mutex);
        mutex_unlock(&g_bench_mutex);
    } else {
        /* Private object: success is guaranteed without a contending
         * owner. Keep the call in LOCKDEP=0 builds too. */
        KASSERT(mutex_trylock(&g_bench_mutex));
        mutex_unlock(&g_bench_mutex);
    }
}

bool selftest_lockdep_bench(const char **reason)
{
    (void)reason;
    enum { SAMPLES = 9, ITERATIONS = 1024, WARMUP = 64 };
    static const char *const paths[] = { "empty", "spin", "irqsave", "nested", "mutex", "mutex-try" };
    uint64_t elapsed[ARRAY_SIZE(paths)][SAMPLES];
    mutex_init(&g_bench_mutex, "lockdep-bench-mutex");
    cpumask_t saved = thread_pin_self();
    for (unsigned kind = 0; kind < ARRAY_SIZE(paths); kind++) {
        for (unsigned i = 0; i < WARMUP; i++)
            bench_iteration(kind);
        for (unsigned sample = 0; sample < SAMPLES; sample++) {
            uint64_t begin = clock_now_ns();
            for (unsigned i = 0; i < ITERATIONS; i++)
                bench_iteration(kind);
            elapsed[kind][sample] = clock_since_ns(begin);
        }
    }
    thread_set_affinity_self(saved);
    /* Print only after timing. No background CPU is asked to stop; these
     * guest-clock observations are descriptive, never pass thresholds. */
    for (unsigned kind = 0; kind < ARRAY_SIZE(paths); kind++) {
        for (unsigned i = 1; i < SAMPLES; i++) {
            uint64_t value = elapsed[kind][i];
            unsigned j = i;
            while (j && elapsed[kind][j - 1] > value) {
                elapsed[kind][j] = elapsed[kind][j - 1];
                j--;
            }
            elapsed[kind][j] = value;
        }
        kinfo("lockdep-bench: enabled=%u path=%s samples=%u iterations=%u ns/iteration min=%llu median=%llu max=%llu",
              (unsigned)CONFIG_LOCKDEP, paths[kind], SAMPLES, ITERATIONS,
              (unsigned long long)(elapsed[kind][0] / ITERATIONS),
              (unsigned long long)(elapsed[kind][SAMPLES / 2] / ITERATIONS),
              (unsigned long long)(elapsed[kind][SAMPLES - 1] / ITERATIONS));
    }
    return true;
}

/* Measure the public acquisition call(s), stopping the clock while the
 * private lock is still owned. Release and assertions are outside timing.
 * Even a failed ownership check must release everything before returning. */
static uint64_t first_bench_acquire(unsigned kind, spinlock_t *a, spinlock_t *b,
                                    struct mutex *m, bool *owned)
{
    arch_irq_state_t state = 0;
    uint64_t begin = clock_now_ns();
    if (kind == 1)
        spin_lock(a);
    else if (kind == 2 || kind == 3) {
        state = spin_lock_irqsave(a);
        if (kind == 3)
            spin_lock(b);
    } else if (kind == 4)
        mutex_lock(m);
    uint64_t elapsed = clock_since_ns(begin);
    *owned = kind == 0 || (kind == 4 ? m->owner == thread_current() : spin_is_held(a));
    if (kind == 3) {
        *owned = *owned && spin_is_held(b);
        spin_unlock(b);
    }
    if (kind == 1)
        spin_unlock(a);
    else if (kind == 2 || kind == 3)
        spin_unlock_irqrestore(a, state);
    else if (kind == 4)
        mutex_unlock(m);
    return elapsed;
}

bool selftest_lockdep_first_bench(const char **reason)
{
    /* Three fresh samples consume 18 live classes in total: do not
     * reset the graph or grow its capacity for benchmark repetition. */
    enum { SAMPLES = 3, PATHS = 5 };
    static const char *const paths[PATHS] = { "empty", "spin", "irqsave", "nested", "mutex" };
    static char names[PATHS][SAMPLES][2][40];
    uint64_t elapsed[PATHS][2][SAMPLES];
    unsigned classes_before[PATHS], classes_after[PATHS];
    bool ok = true;
    cpumask_t saved = thread_pin_self();
    for (unsigned kind = 0; kind < PATHS && ok; kind++) {
        struct lockdep_stats before, after;
        lockdep_get_stats(&before);
        classes_before[kind] = before.classes;
        for (unsigned sample = 0; sample < SAMPLES && ok; sample++) {
            spinlock_t a, b;
            struct mutex m;
            ksnprintf(names[kind][sample][0], sizeof(names[kind][sample][0]),
                      "lockdep-first-%s-%u-a", paths[kind], sample);
            ksnprintf(names[kind][sample][1], sizeof(names[kind][sample][1]),
                      "lockdep-first-%s-%u-b", paths[kind], sample);
            spinlock_init(&a, names[kind][sample][0]);
            spinlock_init(&b, names[kind][sample][1]);
            mutex_init(&m, names[kind][sample][0]);
            lockdep_get_stats(&before);
            bool owned;
            ok = a.class == 0 && b.class == 0 && m.class == 0 && m.lock.class == 0;
            elapsed[kind][0][sample] = first_bench_acquire(kind, &a, &b, &m, &owned);
            ok = ok && owned && !a.locked && !b.locked && !m.owner;
            lockdep_get_stats(&after);
#if CONFIG_LOCKDEP
            unsigned added = kind == 0 ? 0 : kind >= 3 ? 2 : 1;
            ok = ok && after.classes >= before.classes + added;
            if (kind == 1 || kind == 2 || kind == 3)
                ok = ok && a.class != 0;
            if (kind == 3)
                ok = ok && b.class != 0 && b.class != a.class && after.searches > before.searches;
            if (kind == 4)
                ok = ok && m.class != 0 && m.lock.class != 0 && m.class != m.lock.class;
#endif
            uint16_t ca = a.class, cb = b.class, cm = m.class, ci = m.lock.class;
            elapsed[kind][1][sample] = first_bench_acquire(kind, &a, &b, &m, &owned);
            ok = ok && owned && !a.locked && !b.locked && !m.owner &&
                 a.class == ca && b.class == cb && m.class == cm && m.lock.class == ci;
        }
        lockdep_get_stats(&after);
        classes_after[kind] = after.classes;
    }
    thread_set_affinity_self(saved);
    CHECK(ok);
    for (unsigned kind = 0; kind < PATHS; kind++) {
        for (unsigned phase = 0; phase < 2; phase++) {
            uint64_t *ns = elapsed[kind][phase];
            for (unsigned i = 1; i < SAMPLES; i++) {
                uint64_t value = ns[i];
                unsigned j = i;
                while (j && ns[j - 1] > value) {
                    ns[j] = ns[j - 1];
                    j--;
                }
                ns[j] = value;
            }
            kinfo("lockdep-first-bench: enabled=%u path=%s phase=%s samples=%u ns/acquisition min=%llu median=%llu max=%llu classes=%u..%u",
                  (unsigned)CONFIG_LOCKDEP, paths[kind], phase ? "reuse" : "first", SAMPLES,
                  (unsigned long long)ns[0], (unsigned long long)ns[SAMPLES / 2],
                  (unsigned long long)ns[SAMPLES - 1], classes_before[kind], classes_after[kind]);
        }
    }
    return true;
}

struct spin_bench_probe {
    spinlock_t lock;
    unsigned ready, go, acquired;
    unsigned value, expected;
    bool irqsave, owned, payload_ok, context_ok;
    uint64_t elapsed;
};

static void spin_bench_waiter(void *arg)
{
    struct spin_bench_probe *p = arg;
    bool entry_ok = arch_irq_enabled() && raw_this_cpu()->preempt_count == 0;
    /* Be running before the owner can mask IRQs. A merely runnable
     * waiter can be behind the reaper's TLB shootdown, which then waits
     * for an acknowledgment from that owner. Keep IRQs enabled here so
     * other CPUs' shootdowns still complete during the rendezvous. */
    preempt_disable();
    __atomic_store_n(&p->ready, 1u, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&p->go, __ATOMIC_ACQUIRE))
        arch_cpu_relax();
    uint64_t begin = clock_now_ns();
    arch_irq_state_t s = 0;
    if (p->irqsave)
        s = spin_lock_irqsave(&p->lock);
    else
        spin_lock(&p->lock);
    p->elapsed = clock_since_ns(begin);
    preempt_enable(); /* transfer the startup pin to the acquired lock */
    p->owned = spin_is_held(&p->lock);
    p->context_ok = entry_ok && raw_this_cpu()->preempt_count == 1 &&
                    arch_irq_enabled() == !p->irqsave &&
                    !spin_test_waiting_on(arch_cpu_id(), &p->lock);
    p->payload_ok = p->value == p->expected;
    p->value++;
    __atomic_store_n(&p->acquired, 1u, __ATOMIC_RELEASE);
    if (p->irqsave)
        spin_unlock_irqrestore(&p->lock, s);
    else
        spin_unlock(&p->lock);
    p->context_ok = p->context_ok && arch_irq_enabled() && raw_this_cpu()->preempt_count == 0;
}

static bool spin_contention_bench_pinned(const char **reason)
{
    enum { WARMUP = 2, SAMPLES = 9, HOLD_US = 1000 };
    unsigned me = arch_cpu_id(), other = me;
    for (unsigned i = 1; i < cpu_count(); i++)
        if (cpu_online((me + i) % cpu_count())) {
            other = (me + i) % cpu_count();
            break;
        }
    if (other == me) {
        kinfo("selftest: lockdep-spin-bench: one CPU, cross-CPU contention unavailable");
        return true;
    }
    CHECK(arch_irq_enabled() && raw_this_cpu()->preempt_count == 0);
    struct spin_bench_probe p = {0};
    spinlock_init(&p.lock, "lockdep-bench-contended-spin");
    spin_lock(&p.lock);
    spin_unlock(&p.lock);
    for (unsigned path = 0; path < 2; path++) {
        p.irqsave = path != 0;
        uint64_t elapsed[SAMPLES];
        for (unsigned round = 0; round < WARMUP + SAMPLES; round++) {
            __atomic_store_n(&p.ready, 0u, __ATOMIC_RELAXED);
            __atomic_store_n(&p.go, 0u, __ATOMIC_RELAXED);
            __atomic_store_n(&p.acquired, 0u, __ATOMIC_RELAXED);
            p.owned = p.payload_ok = p.context_ok = false;
            p.elapsed = 0;
            p.value = 0;
            p.expected = round + 1;
            struct thread *waiter = thread_create_on(spin_bench_waiter, &p, "spin-bench",
                                                     SCHED_PRIO_DEFAULT, CPUMASK_OF(other));
            CHECK(waiter != NULL);
            uint64_t ready_end = clock_deadline_ns(1000000000ULL);
            while (!__atomic_load_n(&p.ready, __ATOMIC_ACQUIRE) && !clock_deadline_passed(ready_end))
                sched_yield();
            if (!__atomic_load_n(&p.ready, __ATOMIC_ACQUIRE))
                panic("selftest lockdep-spin-bench: waiter readiness timeout; retaining thread and probe");
            arch_irq_state_t s = 0;
            if (p.irqsave)
                s = spin_lock_irqsave(&p.lock);
            else
                spin_lock(&p.lock);
            __atomic_store_n(&p.go, 1u, __ATOMIC_RELEASE);
            /* Pinned by the held spinlock; keep the guard advancing even
             * if every CPU has IRQs masked on a non-common-clock host. */
            uint64_t begin = clock_raw_ns();
            bool waiting;
            while (!(waiting = spin_test_waiting_on(other, &p.lock)) &&
                   clock_raw_ns() - begin < 1000000000ULL)
                arch_cpu_relax();
            if (waiting)
                udelay(HOLD_US);
            bool excluded = __atomic_load_n(&p.acquired, __ATOMIC_ACQUIRE) == 0;
            p.value = p.expected; /* handoff must be ordered by the real spinlock */
            if (p.irqsave)
                spin_unlock_irqrestore(&p.lock, s);
            else
                spin_unlock(&p.lock);
            /* A failed wait observation still releases and joins; the
             * stack probe cannot expire while the worker can access it. */
            if (!wait_for_completion_timeout(&waiter->exited, 1000000000ULL))
                panic("selftest lockdep-spin-bench: waiter exit timeout; retaining thread and probe");
            thread_join(waiter);
            CHECK(waiting && excluded && p.owned && p.payload_ok && p.context_ok &&
                  p.value == p.expected + 1);
            CHECK(__atomic_load_n(&p.lock.locked, __ATOMIC_RELAXED) == 0 &&
                  !spin_test_waiting_on(other, &p.lock));
            CHECK(arch_irq_enabled() && raw_this_cpu()->preempt_count == 0);
            if (round >= WARMUP)
                elapsed[round - WARMUP] = p.elapsed;
        }
        for (unsigned i = 1; i < SAMPLES; i++) {
            uint64_t value = elapsed[i];
            unsigned j = i;
            while (j && elapsed[j - 1] > value) {
                elapsed[j] = elapsed[j - 1];
                j--;
            }
            elapsed[j] = value;
        }
        kinfo("lockdep-spin-bench: enabled=%u path=%s placement=cross-cpu samples=%u hold-us=%u ns/acquisition min=%llu median=%llu max=%llu",
              (unsigned)CONFIG_LOCKDEP, path ? "irqsave" : "plain", SAMPLES, HOLD_US,
              (unsigned long long)elapsed[0], (unsigned long long)elapsed[SAMPLES / 2],
              (unsigned long long)elapsed[SAMPLES - 1]);
    }
    return true;
}

bool selftest_lockdep_spin_bench(const char **reason)
{
    cpumask_t saved = thread_pin_self();
    bool ok = spin_contention_bench_pinned(reason);
    thread_set_affinity_self(saved);
    return ok;
}

struct mutex_bench_probe {
    struct mutex mutex;
    unsigned go, acquired;
    unsigned value, expected;
    bool owned, payload_ok;
    uint64_t elapsed;
};

static void mutex_bench_waiter(void *arg)
{
    struct mutex_bench_probe *p = arg;
    while (!__atomic_load_n(&p->go, __ATOMIC_ACQUIRE))
        sched_yield();
    uint64_t begin = clock_now_ns();
    mutex_lock(&p->mutex);
    p->elapsed = clock_since_ns(begin);
    p->owned = p->mutex.owner == thread_current();
    p->payload_ok = p->value == p->expected;
    p->value++;
    __atomic_store_n(&p->acquired, 1u, __ATOMIC_RELEASE);
    mutex_unlock(&p->mutex);
}

static bool mutex_contention_bench_pinned(const char **reason)
{
    enum { WARMUP = 2, SAMPLES = 9, HOLD_US = 1000 };
    unsigned me = arch_cpu_id(), other = me;
    for (unsigned i = 1; i < cpu_count(); i++)
        if (cpu_online((me + i) % cpu_count())) {
            other = (me + i) % cpu_count();
            break;
        }
    struct mutex_bench_probe p = {0};
    mutex_init(&p.mutex, "lockdep-bench-contended-mutex");
    /* Register classes and owner-side edges before creating any worker.
     * The two warmup rounds also exercise the queued waiter path. */
    mutex_lock(&p.mutex);
    mutex_unlock(&p.mutex);
    uint64_t elapsed[SAMPLES];
    for (unsigned round = 0; round < WARMUP + SAMPLES; round++) {
        __atomic_store_n(&p.go, 0u, __ATOMIC_RELAXED);
        __atomic_store_n(&p.acquired, 0u, __ATOMIC_RELAXED);
        p.owned = p.payload_ok = false;
        p.elapsed = 0;
        p.value = 0;
        p.expected = round + 1;
        struct thread *waiter = thread_create_on(mutex_bench_waiter, &p, "mutex-bench",
                                                 SCHED_PRIO_DEFAULT, CPUMASK_OF(other));
        CHECK(waiter != NULL); /* no lock held or worker published on failure */
        mutex_lock(&p.mutex);
        __atomic_store_n(&p.go, 1u, __ATOMIC_RELEASE);
        uint64_t begin = clock_now_ns();
        bool queued;
        while (!(queued = !waitqueue_empty(&p.mutex.wq)) && clock_since_ns(begin) < 1000000000ULL)
            sched_yield();
        /* Same CPU works too: yielding above lets the waiter queue and
         * block. Hold time is workload, never a performance threshold. */
        if (queued)
            udelay(HOLD_US);
        bool excluded = __atomic_load_n(&p.acquired, __ATOMIC_ACQUIRE) == 0;
        p.value = p.expected; /* publication must come through the mutex */
        mutex_unlock(&p.mutex);
        /* Even a queue-observation failure must release the owner and
         * drain the worker before the stack probe can expire. */
        if (!wait_for_completion_timeout(&waiter->exited, 1000000000ULL))
            panic("selftest lockdep-mutex-bench: waiter exit timeout; retaining thread and probe");
        thread_join(waiter);
        CHECK(queued && excluded && p.owned && p.payload_ok && p.value == p.expected + 1);
        CHECK(!mutex_is_locked(&p.mutex) && waitqueue_empty(&p.mutex.wq));
        if (round >= WARMUP)
            elapsed[round - WARMUP] = p.elapsed;
    }
    for (unsigned i = 1; i < SAMPLES; i++) {
        uint64_t value = elapsed[i];
        unsigned j = i;
        while (j && elapsed[j - 1] > value) {
            elapsed[j] = elapsed[j - 1];
            j--;
        }
        elapsed[j] = value;
    }
    kinfo("lockdep-mutex-bench: enabled=%u placement=%s samples=%u hold-us=%u ns/acquisition min=%llu median=%llu max=%llu",
          (unsigned)CONFIG_LOCKDEP, other == me ? "same-cpu" : "cross-cpu", SAMPLES, HOLD_US,
          (unsigned long long)elapsed[0], (unsigned long long)elapsed[SAMPLES / 2],
          (unsigned long long)elapsed[SAMPLES - 1]);
    return true;
}

bool selftest_lockdep_mutex_bench(const char **reason)
{
    cpumask_t saved = thread_pin_self();
    bool ok = mutex_contention_bench_pinned(reason);
    thread_set_affinity_self(saved);
    return ok;
}

#if CONFIG_LOCKDEP
/* Private graph: new-edge searches must not consume live classes or
 * reset dependencies used by the rest of the kernel. "New" describes
 * the absent edge, not a cold CPU cache. Setup is outside each sample. */
struct graph_bench_result {
    unsigned kind, path_len;
    uint16_t path[8], safe, unsafe;
};

/* Existing sizes spread subclass-zero nodes across the bitmap; the
 * maximum size fills every class/subclass slot. */
static uint16_t graph_bench_node(unsigned index, unsigned nodes)
{
    return nodes == LOCKDEP_MAX_NODES ? (uint16_t)index : lockdep_node(index, 0);
}

static bool graph_bench_seed(struct lockdep_graph *g, unsigned nodes, unsigned kind, bool dense)
{
    memset(g, 0, sizeof(*g));
    unsigned classes = nodes == LOCKDEP_MAX_NODES ? LOCKDEP_MAX_CLASSES : nodes;
    for (unsigned i = 0; i < classes; i++) {
        char name[32];
        ksnprintf(name, sizeof(name), "graph-bench-%u", i);
        if (lockdep_core_class(g, name, LOCKDEP_KIND_SPIN) != (int)i)
            return false;
    }
    /* Two disjoint components, or one for cycle rejection. Dense DAGs
     * contain every forward edge within each component. */
    for (unsigned i = 0; i < nodes; i++)
        for (unsigned j = i + 1; j < nodes; j++)
            if ((dense || j == i + 1) &&
                (kind == 1 || (i < nodes / 2) == (j < nodes / 2)))
                lockdep_core_add_edge(g, graph_bench_node(i, nodes), graph_bench_node(j, nodes));
    unsigned expected = dense ? (kind == 1 ? nodes * (nodes - 1) / 2 :
                                 (nodes / 2) * (nodes / 2 - 1)) :
                                nodes - (kind == 1 ? 1 : 2);
    if (g->nr_edges != expected)
        return false;
    if (kind == 2) {
        /* Each component is valid; only the proposed bridge would join
         * an IRQ-used ancestor to an IRQ-enabled descendant. */
        g->classes[0].usage = LOCKDEP_USED_IN_IRQ;
        g->classes[classes - 1].usage = LOCKDEP_HELD_IRQS_ON;
    }
    return true;
}

static void graph_bench_insert(struct lockdep_graph *g, struct lockdep_scratch *scratch,
                               uint16_t from, uint16_t to, struct graph_bench_result *r)
{
    /* Same check/search/insertion order as acquire_check, without its
     * class lookup, held-stack scan, statistics, raw lock, or reports. */
    if (lockdep_core_reaches(g, scratch, to, from, r->path, ARRAY_SIZE(r->path), &r->path_len)) {
        r->kind = 1;
    } else if (lockdep_core_irq_edge(g, scratch, from, to, &r->safe, &r->unsafe)) {
        r->kind = 2;
    } else {
        r->kind = lockdep_core_add_edge(g, from, to) ? 0 : 3;
    }
}

bool selftest_lockdep_graph_bench(const char **reason)
{
    enum { SAMPLES = 9, WARMUP = 2 };
    static const unsigned sizes[] = { 16, 64, 256, LOCKDEP_MAX_CLASSES, LOCKDEP_MAX_NODES };
    static const char *const paths[] = { "insert", "cycle", "irq-bridge" };
    struct lockdep_graph *g = kmalloc(sizeof(*g), 0);
    struct lockdep_scratch *scratch = kmalloc(sizeof(*scratch), 0);
    if (!g || !scratch) {
        kfree(scratch);
        kfree(g);
        *reason = "graph benchmark allocation failed";
        return false;
    }
    bool ok = true;
    cpumask_t saved = thread_pin_self();
    for (unsigned topology = 0; topology < 2 && ok; topology++) {
        bool dense = topology != 0;
        for (unsigned size = 0; size < ARRAY_SIZE(sizes) && ok; size++) {
            unsigned nodes = sizes[size];
            for (unsigned kind = 0; kind < ARRAY_SIZE(paths) && ok; kind++) {
                uint64_t elapsed[SAMPLES];
                uint16_t from = graph_bench_node(kind == 1 ? nodes - 1 : nodes / 2 - 1, nodes);
                uint16_t to = graph_bench_node(kind == 1 ? 0 : nodes / 2, nodes);
                for (unsigned sample = 0; sample < WARMUP + SAMPLES; sample++) {
                    if (!graph_bench_seed(g, nodes, kind, dense) || lockdep_core_has_edge(g, from, to)) {
                        ok = false;
                        break;
                    }
                    unsigned edges = g->nr_edges;
                    struct graph_bench_result r = {0};
                    uint64_t begin = clock_now_ns();
                    graph_bench_insert(g, scratch, from, to, &r);
                    uint64_t ns = clock_since_ns(begin);
                    /* Validate every measured operation, outside its interval.
                     * Rejected proposals must leave the edge set unchanged. */
                    ok = r.kind == kind && g->nr_edges == edges + (kind == 0) &&
                         lockdep_core_has_edge(g, from, to) == (kind == 0);
                    if (kind == 1)
                        ok = ok && r.path_len == (dense ? 2 : ARRAY_SIZE(r.path)) &&
                             r.path[0] == (dense ? to : graph_bench_node(nodes - ARRAY_SIZE(r.path), nodes)) &&
                             r.path[r.path_len - 1] == from;
                    for (unsigned i = 1; i < r.path_len && ok; i++)
                        ok = lockdep_core_has_edge(g, r.path[i - 1], r.path[i]);
                    if (kind == 2) {
                        /* Usage labels all subclasses. In a full chain,
                         * multi-source BFS reaches from subclass 3 of the
                         * first class; dense edges reach from subclass 0.
                         * The first unsafe node is subclass 0 of the last. */
                        bool full = nodes == LOCKDEP_MAX_NODES;
                        uint16_t safe = full && !dense ? LOCKDEP_SUBCLASSES - 1 : 0;
                        uint16_t unsafe = full ? lockdep_node(LOCKDEP_MAX_CLASSES - 1, 0) :
                                                graph_bench_node(nodes - 1, nodes);
                        ok = ok && r.safe == safe && r.unsafe == unsafe;
                    }
                    if (!ok)
                        break;
                    if (sample >= WARMUP)
                        elapsed[sample - WARMUP] = ns;
                }
                if (!ok)
                    break;
                for (unsigned i = 1; i < SAMPLES; i++) {
                    uint64_t value = elapsed[i];
                    unsigned j = i;
                    while (j && elapsed[j - 1] > value) {
                        elapsed[j] = elapsed[j - 1];
                        j--;
                    }
                    elapsed[j] = value;
                }
                kinfo("lockdep-graph-bench: topology=%s nodes=%u edges=%u path=%s samples=%u ns/operation min=%llu median=%llu max=%llu",
                      dense ? "dense" : "chain", nodes, g->nr_edges - (kind == 0), paths[kind], SAMPLES, (unsigned long long)elapsed[0],
                      (unsigned long long)elapsed[SAMPLES / 2], (unsigned long long)elapsed[SAMPLES - 1]);
            }
        }
    }
    thread_set_affinity_self(saved);
    kfree(scratch);
    kfree(g);
    if (!ok)
        *reason = "graph benchmark topology or validation result mismatch";
    return ok;
}
#else
bool selftest_lockdep_graph_bench(const char **reason)
{
    return skip(reason, "lockdep-graph-bench");
}
#endif
