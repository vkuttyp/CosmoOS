/*
 * lockdep.h - Runtime lock-order and sleep-in-atomic checking
 * (docs/kernel/lockdep/).
 *
 * CONFIG_LOCKDEP (default on in debug builds) records every spinlock and mutex acquisition
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
 * With CONFIG_LOCKDEP disabled, only the always-on half of might_sleep()
 * remains. LOCKDEP=0/1 selects this through the make configuration.
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
    LOCKDEP_R_IRQ_STATE,
    LOCKDEP_R_CALLBACK,
    LOCKDEP_R_COMPLETION,
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
void lockdep_acquire_check(const void *lock, uint16_t *class_slot, const char *name, unsigned kind, unsigned subclass, bool irqs_on,
                           uintptr_t ip);
void lockdep_acquired(const void *lock, uint16_t *class_slot, const char *name, unsigned kind, unsigned subclass,
                      bool trylock, bool irqs_on, uintptr_t ip);
void lockdep_release(const void *lock, unsigned kind, uintptr_t ip, bool irqrestore);
void lockdep_irqsave_acquired(const void *lock, bool irq_was_enabled);
void lockdep_irqrestore_check(const void *lock, bool irq_will_enable, uintptr_t ip);
void lockdep_timer_cancel_done(const void *timer);
void lockdep_timer_enter(const void *timer);
void lockdep_timer_exit(const void *timer);
bool lockdep_timer_cancel_check(const void *timer, uintptr_t ip);
/*
 * Callback classes (design.md, "Callback classes"): one pseudo-class per
 * callback function `fn`, cached in `*class_slot`. A callback executes
 * between enter and exit, so every lock it takes is recorded as taken
 * under its class; a synchronous wait for such a callback records the
 * waiter's held locks before the class, as if it acquired it. The graph
 * then reports a wait that could deadlock against any callback of that
 * function ever observed, or against a lock it reaches, in either order.
 * The wait returns false after a report (the caller must not wait).
 * Exit names only `fn`: the object holding the slot may be gone.
 */
void lockdep_callback_enter(const void *fn, uint16_t *class_slot);
void lockdep_callback_exit(const void *fn);
bool lockdep_callback_wait(const void *fn, uint16_t *class_slot, uintptr_t ip);
/*
 * Completion classes (design.md, "Completion waits"): one class per
 * completion name, keyed through the completion's own spinlock class
 * (`spin_slot` is that lock's cached class; `name` its name), so struct
 * completion carries nothing extra. Init classifies both. A wait records
 * each held mutex -> completion and reports a cycle. A complete() in
 * thread context with mutexes held keeps completion -> each held mutex
 * pending in the thread and commits it, with the cycle check, at the
 * thread's next completion event, mutex release or exit -- unless that
 * event is the thread's own wait for the same object, which makes the
 * complete() a self-signal inside the waiter's call chain (a device that
 * completes in submit, a cancel after a timed-out wait) and discards it.
 * In interrupt context, or holding no mutex, complete() records nothing.
 * `c` is the object, for that comparison only.
 */
void lockdep_completion_init(uint16_t *spin_slot, const char *name, uintptr_t ip);
void lockdep_completion_wait(const void *c, uint16_t *spin_slot, const char *name, uintptr_t ip);
void lockdep_completion_signal(const void *c, uint16_t *spin_slot, const char *name, uintptr_t ip);
/* Print the completion classes never waited for or never signalled this
 * boot (kinfo), one per line, and a total. Normal diagnostics only. */
void lockdep_dump_completion_coverage(void);

/* The debug half of might_sleep(): a report with the held stacks. */
void lockdep_might_sleep(uintptr_t ip);

/* A thread exiting must hold no mutex. */
void lockdep_thread_exit(struct thread *t);

/* Diagnostics: is `lock` on this CPU's / this thread's held stack? */
bool lockdep_is_held(const void *lock, unsigned kind);

/* Print the held stacks (the panic report calls this). */
void lockdep_dump_held(void);
/* One nonblocking snapshot attempt. out has LOCKDEP_MAX_HELD entries;
 * output is usable only on success. Does not capture thread mutexes. */
bool lockdep_snapshot_held_cpu(unsigned cpu, struct lockdep_held *out, unsigned *count);
/* Caller keeps t alive (owned reference or current thread). out has
 * LOCKDEP_MAX_HELD_MUTEX entries. No allocation, lock, or retries;
 * false returns count zero and unusable output. */
bool lockdep_snapshot_held_thread(const struct thread *t, struct lockdep_held *out, unsigned *count);
/* Print another CPU's spinlock stack (the lockup report, for a CPU that does not answer). */
void lockdep_dump_held_cpu(unsigned cpu);

/* One instant of counters under the graph raw lock. Normal diagnostics
 * only; not a panic/NMI API or a snapshot of CPU/thread held stacks. */
void lockdep_get_stats(struct lockdep_stats *out);

/* Print every recorded edge as "'a'#n -> 'b'#m" (kdebug), one per line:
 * a consistent graph snapshot for docs to compare against. Requires a
 * working heap and raw lock; not a panic/NMI diagnostic. Allocation
 * failure prints an explicit unavailable message. */
void lockdep_dump_graph(void);

/* Self-tests: the next report of `kind` counts instead of panicking, and
 * the offending operation proceeds. One-shot. */
void lockdep_expect(enum lockdep_report_kind kind);
unsigned lockdep_expected_hits(void);
const char *lockdep_report_name(enum lockdep_report_kind kind);

#if CONFIG_SELFTEST
/* Test-only: invoke a nonblocking probe with the graph raw lock held,
 * optionally during an unfinished local held-stack update. */
void lockdep_test_snapshot_context(bool updating, void (*probe)(void *), void *arg);
#endif

#define lockdep_assert_held(lock, kind)     KASSERT(lockdep_is_held((lock), (kind)))
#define lockdep_assert_not_held(lock, kind) KASSERT(!lockdep_is_held((lock), (kind)))

#else

static inline void lockdep_acquire_check(const void *lock, uint16_t *class_slot, const char *name, unsigned kind, unsigned subclass,
                                         bool irqs_on, uintptr_t ip)
{
    (void)lock; (void)class_slot; (void)name; (void)kind; (void)subclass; (void)irqs_on; (void)ip;
}
static inline void lockdep_acquired(const void *lock, uint16_t *class_slot, const char *name, unsigned kind,
                                    unsigned subclass, bool trylock, bool irqs_on, uintptr_t ip)
{
    (void)lock; (void)class_slot; (void)name; (void)kind; (void)subclass; (void)trylock; (void)irqs_on; (void)ip;
}
static inline void lockdep_release(const void *lock, unsigned kind, uintptr_t ip, bool irqrestore)
{ (void)lock; (void)kind; (void)ip; (void)irqrestore; }
static inline void lockdep_irqsave_acquired(const void *lock, bool enabled) { (void)lock; (void)enabled; }
static inline void lockdep_irqrestore_check(const void *lock, bool enabled, uintptr_t ip)
{ (void)lock; (void)enabled; (void)ip; }
static inline void lockdep_timer_cancel_done(const void *timer) { (void)timer; }
static inline void lockdep_timer_enter(const void *timer) { (void)timer; }
static inline void lockdep_timer_exit(const void *timer) { (void)timer; }
static inline bool lockdep_timer_cancel_check(const void *timer, uintptr_t ip)
{ (void)timer; (void)ip; return true; }
static inline void lockdep_callback_enter(const void *fn, uint16_t *class_slot) { (void)fn; (void)class_slot; }
static inline void lockdep_callback_exit(const void *fn) { (void)fn; }
static inline bool lockdep_callback_wait(const void *fn, uint16_t *class_slot, uintptr_t ip)
{ (void)fn; (void)class_slot; (void)ip; return true; }
static inline void lockdep_completion_init(uint16_t *spin_slot, const char *name, uintptr_t ip)
{ (void)spin_slot; (void)name; (void)ip; }
static inline void lockdep_completion_wait(const void *c, uint16_t *spin_slot, const char *name, uintptr_t ip)
{ (void)c; (void)spin_slot; (void)name; (void)ip; }
static inline void lockdep_completion_signal(const void *c, uint16_t *spin_slot, const char *name, uintptr_t ip)
{ (void)c; (void)spin_slot; (void)name; (void)ip; }
static inline void lockdep_dump_completion_coverage(void) {}
static inline void lockdep_might_sleep(uintptr_t ip) { (void)ip; }
static inline void lockdep_thread_exit(struct thread *t) { (void)t; }
static inline void lockdep_dump_held(void) {}
static inline bool lockdep_snapshot_held_cpu(unsigned cpu, struct lockdep_held *out, unsigned *count)
{ (void)cpu; (void)out; *count = 0; return false; }
static inline void lockdep_dump_held_cpu(unsigned cpu) { (void)cpu; }
static inline bool lockdep_snapshot_held_thread(const struct thread *t, struct lockdep_held *out, unsigned *count)
{ (void)t; (void)out; *count = 0; return false; }
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
        struct percpu *__pc = raw_this_cpu(); /* identity: zero wherever a sleeper may run */      \
        if (__pc->preempt_count != 0 || __pc->irq_depth != 0) {                                    \
            if (CONFIG_LOCKDEP)                                                                    \
                lockdep_might_sleep((uintptr_t)__builtin_return_address(0));                       \
            else                                                                                   \
                panic("sleeping call with preemption disabled (count %d) or in interrupt (depth %u)", \
                      __pc->preempt_count, __pc->irq_depth);                                       \
        }                                                                                          \
    } while (0)

#endif /* KERNEL_LOCKDEP_H */
