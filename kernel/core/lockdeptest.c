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
 * CPU 1 holds L for 20 ms; this CPU spins on a plain spin_lock(L) with
 * interrupts enabled while a timer callback here takes M. If the checker
 * pushed L before owning it, the callback would record L -> M; the
 * legitimate M -> L order taken afterwards would then be an inversion.
 */
static spinlock_t g_cont_l = SPINLOCK_INIT("lockdep-test-cont-l");
static spinlock_t g_cont_m = SPINLOCK_INIT("lockdep-test-cont-m");
static volatile unsigned g_cont_holding, g_cont_timer_ran;

static void cont_holder(void *arg)
{
    (void)arg;
    arch_irq_state_t s = spin_lock_irqsave(&g_cont_l);
    __atomic_store_n(&g_cont_holding, 1u, __ATOMIC_RELEASE);
    uint64_t end = clock_now_ns() + 20000000ULL;
    while (clock_now_ns() < end)
        arch_cpu_relax();
    spin_unlock_irqrestore(&g_cont_l, s);
}

static void cont_timer(struct timer *t, void *arg)
{
    (void)t;
    (void)arg;
    spin_lock(&g_cont_m);   /* interrupt context, during the contention on L */
    spin_unlock(&g_cont_m);
    __atomic_store_n(&g_cont_timer_ran, 1u, __ATOMIC_RELEASE);
}

static bool selftest_lockdep_contention_pinned(const char **reason)
{
    unsigned other = 0, me = arch_cpu_id();   /* pinned by the wrapper */
    for (unsigned i = 1; i < cpu_count(); i++)
        if (cpu_online((me + i) % cpu_count())) {
            other = (me + i) % cpu_count();
            break;
        }
    if (other == me) {
        kinfo("selftest: lockdep-contention: one CPU, nothing to contend with");
        return true;
    }
    struct thread *h = thread_create_on(cont_holder, NULL, "cont-holder", SCHED_PRIO_DEFAULT, CPUMASK_OF(other));
    CHECK(h != NULL);
    uint64_t end = clock_now_ns() + 1000000000ULL;
    while (__atomic_load_n(&g_cont_holding, __ATOMIC_ACQUIRE) == 0 && clock_now_ns() < end)
        arch_cpu_relax();
    CHECK(g_cont_holding == 1);

    struct timer t;
    timer_setup(&t, cont_timer, NULL);
    timer_start(&t, 5000000ULL);   /* fires on this CPU while we spin below */
    CHECK(arch_irq_enabled());
    spin_lock(&g_cont_l);          /* contended for ~15 ms with interrupts enabled */
    CHECK(g_cont_timer_ran == 1);  /* the interrupt landed inside the wait */
    spin_unlock(&g_cont_l);
    thread_join(h);

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
