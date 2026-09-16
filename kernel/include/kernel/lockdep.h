/*
 * lockdep.h - Runtime lock-order and sleep-in-atomic checking
 * (docs/kernel/lockdep/).
 *
 * Debug builds (CONFIG_DEBUG) record every spinlock and mutex acquisition
 * on a held-lock stack (per CPU for spinlocks, per thread for mutexes),
 * classify locks by their initialisation name, keep the "taken while held"
 * graph and panic with both stacks on:
 *   - a lock-order inversion (the acquisition would close a cycle),
 *   - a same-class re-acquisition without a nesting annotation,
 *   - a class used in interrupt context that was also held with
 *     interrupts enabled,
 *   - a sleeping call (might_sleep) with a spinlock held or in an
 *     interrupt,
 *   - a release of a lock that is not held, a thread exiting with a mutex
 *     held, or a held stack overflowing.
 *
 * Release builds keep only the always-on half of might_sleep().
 */

#ifndef KERNEL_LOCKDEP_H
#define KERNEL_LOCKDEP_H

#include <kernel/compiler.h>
#include <kernel/lockdep_core.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>

#ifndef CONFIG_LOCKDEP
#define CONFIG_LOCKDEP CONFIG_DEBUG
#endif

/* Nesting annotations for legitimate same-class nesting (design.md,
 * "Classes and nodes"; every use is listed in invariants.md). */
#define LOCKDEP_NEST_DEFAULT 0u
#define VNODE_NESTED_PARENT2 1u   /* the second parent directory of a rename */
#define VNODE_NESTED_CHILD   2u   /* a child locked under its parent directory */

enum lockdep_report_kind {
    LOCKDEP_R_INVERSION,
    LOCKDEP_R_RECURSION,
    LOCKDEP_R_IRQ,
    LOCKDEP_R_SLEEP,
    LOCKDEP_R_OVERFLOW,
    LOCKDEP_R_UNHELD,
    LOCKDEP_R_EXIT_HELD,
    LOCKDEP_R_COUNT,
};

struct lockdep_stats {
    unsigned classes;
    unsigned edges;
    uint64_t acquisitions;
    uint64_t searches;      /* reachability searches run (new edges) */
    unsigned reports;       /* including expected ones */
};

struct thread;

#if CONFIG_LOCKDEP

/* Called by the lock primitives in two halves. The check runs before the
 * acquisition waits (a deadlocking order is reported, not hung on) and
 * pushes nothing: a contended lock is not held, and an interrupt arriving
 * during the wait must not see it on the stack. The push runs once the
 * lock is owned. Trylock callers use only the push. `class_slot` is the
 * lock's cached class (0 = unknown); `irqs_on` says interrupts were
 * enabled at the acquisition; `ip` is the caller's return address. */
void lockdep_acquire_check(uint16_t *class_slot, const char *name, unsigned kind, unsigned subclass, bool irqs_on,
                           uintptr_t ip);
void lockdep_acquired(const void *lock, uint16_t *class_slot, const char *name, unsigned kind, unsigned subclass,
                      bool trylock, bool irqs_on, uintptr_t ip);
void lockdep_release(const void *lock, unsigned kind, uintptr_t ip);

/* The debug half of might_sleep(): a report with the held stacks. */
void lockdep_might_sleep(uintptr_t ip);

/* A thread exiting must hold no mutex. */
void lockdep_thread_exit(struct thread *t);

/* Diagnostics: is `lock` on this CPU's / this thread's held stack? */
bool lockdep_is_held(const void *lock, unsigned kind);

/* Print the held stacks (the panic report calls this). */
void lockdep_dump_held(void);

void lockdep_get_stats(struct lockdep_stats *out);

/* Print every recorded edge as "'a'#n -> 'b'#m" (kdebug), one per line:
 * the lock order the tree actually has, for docs to compare against. */
void lockdep_dump_graph(void);

/* Self-tests: the next report of `kind` counts instead of panicking, and
 * the offending operation proceeds. One-shot. */
void lockdep_expect(enum lockdep_report_kind kind);
unsigned lockdep_expected_hits(void);
const char *lockdep_report_name(enum lockdep_report_kind kind);

#define lockdep_assert_held(lock, kind)     KASSERT(lockdep_is_held((lock), (kind)))
#define lockdep_assert_not_held(lock, kind) KASSERT(!lockdep_is_held((lock), (kind)))

#else

static inline void lockdep_acquire_check(uint16_t *class_slot, const char *name, unsigned kind, unsigned subclass,
                                         bool irqs_on, uintptr_t ip)
{
    (void)class_slot; (void)name; (void)kind; (void)subclass; (void)irqs_on; (void)ip;
}
static inline void lockdep_acquired(const void *lock, uint16_t *class_slot, const char *name, unsigned kind,
                                    unsigned subclass, bool trylock, bool irqs_on, uintptr_t ip)
{
    (void)lock; (void)class_slot; (void)name; (void)kind; (void)subclass; (void)trylock; (void)irqs_on; (void)ip;
}
static inline void lockdep_release(const void *lock, unsigned kind, uintptr_t ip) { (void)lock; (void)kind; (void)ip; }
static inline void lockdep_might_sleep(uintptr_t ip) { (void)ip; }
static inline void lockdep_thread_exit(struct thread *t) { (void)t; }
static inline void lockdep_dump_held(void) {}
static inline void lockdep_dump_graph(void) {}
static inline void lockdep_get_stats(struct lockdep_stats *out) { *out = (struct lockdep_stats){ 0 }; }
#define lockdep_assert_held(lock, kind)     ((void)0)
#define lockdep_assert_not_held(lock, kind) ((void)0)

#endif

/*
 * might_sleep(): this call may block. Always a panic when a spinlock (or a
 * quiesce_read_lock section) is held or in interrupt context; in debug
 * builds the report carries the held stacks. Placed in every sleeping
 * primitive and every user-memory copy.
 */
#define might_sleep()                                                                              \
    do {                                                                                           \
        struct percpu *__pc = this_cpu();                                                          \
        if (__pc->preempt_count != 0 || __pc->irq_depth != 0) {                                    \
            if (CONFIG_LOCKDEP)                                                                    \
                lockdep_might_sleep((uintptr_t)__builtin_return_address(0));                       \
            else                                                                                   \
                panic("sleeping call with preemption disabled (count %d) or in interrupt (depth %u)", \
                      __pc->preempt_count, __pc->irq_depth);                                       \
        }                                                                                          \
    } while (0)

#endif /* KERNEL_LOCKDEP_H */
