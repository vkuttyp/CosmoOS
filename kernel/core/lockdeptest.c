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
#include <kernel/lockdep.h>
#include <kernel/log.h>
#include <kernel/mutex.h>
#include <kernel/percpu.h>
#include <kernel/selftest.h>
#include <kernel/spinlock.h>
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
    spin_unlock(&a);
    CHECK(!lockdep_is_held(&a, LOCKDEP_KIND_SPIN) && lockdep_is_held(&b, LOCKDEP_KIND_SPIN));
    spin_unlock(&b);
    arch_irq_restore(s);
    lockdep_get_stats(&s1);
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

static spinlock_t g_irq_lock = SPINLOCK_INIT("lockdep-test-irq");
static volatile unsigned g_irq_hits;

static void irq_lock_handler(unsigned vector, struct arch_trap_frame *frame, void *arg)
{
    (void)vector;
    (void)frame;
    (void)arg;
    spin_lock(&g_irq_lock);   /* interrupt context: the class becomes IRQ-safe */
    spin_unlock(&g_irq_lock);
    __atomic_store_n(&g_irq_hits, 1u, __ATOMIC_RELEASE);
}

bool selftest_lockdep_irq(const char **reason)
{
    /* First: held with interrupts enabled (a plain spin_lock from thread
     * context), legal on its own. */
    CHECK(arch_irq_enabled());
    spin_lock(&g_irq_lock);
    spin_unlock(&g_irq_lock);

    int vec = arch_vector_alloc();
    CHECK(vec >= 0);
    CHECK(interrupt_register((unsigned)vec, irq_lock_handler, NULL, "selftest-lockdep-irq") == 0);
    arch_ipi_bind((unsigned)vec);
    lockdep_expect(LOCKDEP_R_IRQ);
    arch_ipi_send(arch_cpu_id(), (unsigned)vec);   /* self-IPI: the handler runs here */
    uint64_t end = clock_now_ns() + 1000000000ULL;
    while (__atomic_load_n(&g_irq_hits, __ATOMIC_ACQUIRE) == 0 && clock_now_ns() < end)
        arch_cpu_relax();
    CHECK(g_irq_hits == 1);
    unsigned hits = lockdep_expected_hits();
    CHECK(interrupt_unregister_sync((unsigned)vec, irq_lock_handler) == 0);
    arch_vector_free((unsigned)vec);
    CHECK(hits == 1);
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
    return true;
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

#endif
