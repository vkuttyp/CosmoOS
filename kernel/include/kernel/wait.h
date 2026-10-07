/*
 * wait.h - Wait queues and timed sleep.
 *
 * wait_event(wq, cond) blocks the calling thread until `cond` is true.
 * The protocol is lost-wakeup free: the waiter is enqueued and marked
 * BLOCKED before evaluating `cond`; a wake between the evaluation and
 * the switch turns the state back to READY and schedule() returns at
 * once. `cond` is re-evaluated after every wake (Mesa semantics).
 *
 * Not usable from interrupt context or with preemption disabled
 * (asserted). Wakers are interrupt-safe.
 */

#ifndef KERNEL_WAIT_H
#define KERNEL_WAIT_H

#include <kernel/errno.h>
#include <kernel/list.h>
#include <kernel/spinlock.h>
#include <kernel/timer.h>

struct thread;
struct wait_entry;

/* A callback entry's function: run by every wake of the queue it is on,
 * under that queue's spinlock, in the waker's context -- which may be an
 * interrupt. It may take spinlocks ordered after the queue's (an epoll set's
 * ready-list lock and the set's own queue), never sleep, and never touch
 * the queue it is on. `flags` is 0 for a wake, WAIT_CB_FREED when the
 * queue's owner is about to free the queue: the entry has already been
 * unlinked and the owner of the entry must forget the queue
 * (docs/kernel/scheduler/design.md, "Wait queues"). */
typedef void (*wait_callback_fn)(struct wait_entry *e, unsigned flags);
#define WAIT_CB_FREED 1u

/* One of two kinds, each on its own list of the queue: a sleeping thread
 * (waitqueue_prepare, `thread` set, `fn` NULL) that a wake marks READY, on
 * `waiters`; or a callback (waitqueue_add_callback, `fn` set) that a wake
 * runs, on `callbacks`. A callback entry stays on the queue until its owner
 * removes it; it is what lets a set of objects (epoll) be told of a member's
 * event without a thread sleeping on each member's queue. The two lists keep
 * a wake_one at one thread: it runs the callbacks list whole and stops at the
 * first thread it wakes (a contended mutex unlock is not O(waiters) under its
 * spinlock; the first version of the callback kind walked one list to its
 * end). */
struct wait_entry {
    struct list_node link;
    struct thread *thread;
    wait_callback_fn fn;
};

struct waitqueue {
    spinlock_t lock;
    struct list_node waiters;     /* sleeping threads */
    struct list_node callbacks;   /* callback entries, run by every wake */
};

#define WAITQUEUE_INIT(name) { .lock = SPINLOCK_INIT(#name), .waiters = LIST_HEAD_INIT((name).waiters), \
                               .callbacks = LIST_HEAD_INIT((name).callbacks) }

void waitqueue_init(struct waitqueue *wq, const char *name);

/* Initialise an entry (once, before the first prepare or add). */
static inline void wait_entry_init(struct wait_entry *e)
{
    list_init(&e->link);
    e->thread = NULL;
    e->fn = NULL;
}

/* Put a callback entry on the queue; every later wake runs `fn` under the
 * queue's lock. Thread context. */
void waitqueue_add_callback(struct waitqueue *wq, struct wait_entry *e, wait_callback_fn fn);
/* Take a callback entry off its queue. Under the queue's lock, so when this
 * returns no wake is running the callback and none will: the entry (and
 * what owns it) may be freed. A no-op for an entry the queue's owner has
 * already detached (WAIT_CB_FREED). */
void waitqueue_remove_callback(struct waitqueue *wq, struct wait_entry *e);
/* The owner of a queue that is about to free it: every callback entry is
 * unlinked and told WAIT_CB_FREED under the lock, so no entry outlives the
 * queue (Linux's POLLFREE). Thread entries are untouched: a thread asleep
 * on a dying queue is the owner's own bug. */
unsigned waitqueue_detach_callbacks(struct waitqueue *wq);   /* returns how many were detached */

/* The wake, split for a waker that must nest the queue's lock inside another
 * queue's (an epoll set woken from inside a member set's wake) and record
 * something under it: lock with a lockdep subclass, wake every waiter and
 * run every callback, unlock. `waitqueue_wake_all` is the three in one. */
arch_irq_state_t waitqueue_lock_nested(struct waitqueue *wq, unsigned subclass);
unsigned waitqueue_wake_all_locked(struct waitqueue *wq);
void waitqueue_unlock(struct waitqueue *wq, arch_irq_state_t s);

/* Enqueue the current thread (if not already queued) and mark it
 * BLOCKED. Safe to call repeatedly with the same entry: a woken waiter
 * whose condition is still false calls it again without finishing. */
void waitqueue_prepare(struct waitqueue *wq, struct wait_entry *e);
/* Dequeue and mark RUNNING. */
void waitqueue_finish(struct waitqueue *wq, struct wait_entry *e);

/* Wake the first / every sleeping waiter and run every callback entry --
 * every one, on either kind of wake, from the queue's own callbacks list
 * before the threads are looked at. Return the number of threads woken
 * (callbacks are not counted). */
unsigned waitqueue_wake_one(struct waitqueue *wq);
unsigned waitqueue_wake_all(struct waitqueue *wq);

/* No sleeping thread and no callback entry on the queue (a snapshot under
 * its lock): a wake may be skipped. A callback entry counts -- it is owed a
 * wake as a thread is -- so the test is never a reason to pass an epoll
 * item over. */
bool waitqueue_empty(struct waitqueue *wq);
/* How many threads sleep on the queue right now (callback entries are not
 * counted): the self-tests' proof that their waiters are enrolled, not a
 * synchronisation primitive. */
unsigned waitqueue_waiting(struct waitqueue *wq);

#define wait_event(wq, cond)                                                   \
    do {                                                                       \
        struct wait_entry __we;                                                \
        wait_entry_init(&__we);                                                \
        for (;;) {                                                             \
            waitqueue_prepare((wq), &__we);                                    \
            if (cond)                                                          \
                break;                                                         \
            sched_block_current();                                             \
        }                                                                      \
        waitqueue_finish((wq), &__we);                                         \
    } while (0)

/* Killable variant: evaluates to 0, or -EINTR when the calling process is
 * being killed (process_kill sets the flag and wakes the thread). The
 * flag is checked after the thread is queued and BLOCKED, so a kill that
 * lands between the check and the block finds a waiter to wake. */
bool process_kill_pending(void);
#define wait_event_killable(wq, cond)                                          \
    ({                                                                         \
        int __rc = 0;                                                          \
        struct wait_entry __we;                                                \
        wait_entry_init(&__we);                                                \
        for (;;) {                                                             \
            waitqueue_prepare((wq), &__we);                                    \
            if (cond)                                                          \
                break;                                                         \
            if (process_kill_pending()) {                                      \
                __rc = -EINTR;                                                 \
                break;                                                         \
            }                                                                  \
            sched_block_current();                                             \
        }                                                                      \
        waitqueue_finish((wq), &__we);                                         \
        __rc;                                                                  \
    })

/* The timed wait's private state: a timer wakes the caller's own queue,
 * so a waiter is woken either by whoever makes the condition true or by
 * the deadline, and re-checks the same condition in both cases. */
struct wait_timeout {
    struct waitqueue *wq;
    bool expired;
};
void wait_timeout_init(struct wait_timeout *wt, struct waitqueue *wq);
void wait_timeout_fired(struct timer *t, void *arg);
bool wait_timeout_expired(const struct wait_timeout *wt);

/*
 * wait_event_timeout(wq, cond, ns) -- block until `cond` or until `ns`
 * has passed. Evaluates to true when the condition became true, false at
 * the deadline.
 *
 * The condition is tested before anything is armed, so a caller whose
 * condition already holds neither starts a timer nor sleeps.
 *
 * The timer is cancelled with timer_cancel_sync, not timer_cancel:
 * both live on this stack frame, and plain cancel only promises the
 * callback will not START (timer.h) -- one already running would touch
 * them after the frame is gone.
 */
#define wait_event_timeout(wq, cond, ns)                                       \
    ({                                                                         \
        bool __ok = (cond);                                                    \
        if (!__ok) {                                                           \
            struct wait_entry __we;                                            \
            struct wait_timeout __wt;                                          \
            struct timer __t;                                                  \
            wait_entry_init(&__we);                                            \
            wait_timeout_init(&__wt, (wq));                                    \
            timer_setup(&__t, wait_timeout_fired, &__wt);                      \
            timer_start(&__t, (ns));                                           \
            for (;;) {                                                         \
                waitqueue_prepare((wq), &__we);                                \
                __ok = (cond);                                                 \
                if (__ok || wait_timeout_expired(&__wt))                       \
                    break;                                                     \
                sched_block_current();                                         \
            }                                                                  \
            waitqueue_finish((wq), &__we);                                     \
            timer_cancel_sync(&__t);                                           \
        }                                                                      \
        __ok;                                                                  \
    })

/*
 * wait_event_killable_timeout(wq, cond, ns) -- the two above composed:
 * evaluates to 0 when the condition became true, -ETIMEDOUT at the
 * deadline, -EINTR when the calling process is being killed. Added for
 * the VFS's held-walk seam (docs/audit/next-subsystem-cwd-hold.md),
 * which parks a thread inside a system call and must let it leave with
 * a dying process as any blocked system call does; the bound is for a
 * caller that is wrong, not for one that is dying.
 */
#define wait_event_killable_timeout(wq, cond, ns)                              \
    ({                                                                         \
        int __rc = 0;                                                          \
        if (!(cond)) {                                                         \
            struct wait_entry __we;                                            \
            struct wait_timeout __wt;                                          \
            struct timer __t;                                                  \
            wait_entry_init(&__we);                                            \
            wait_timeout_init(&__wt, (wq));                                    \
            timer_setup(&__t, wait_timeout_fired, &__wt);                      \
            timer_start(&__t, (ns));                                           \
            for (;;) {                                                         \
                waitqueue_prepare((wq), &__we);                                \
                if (cond)                                                      \
                    break;                                                     \
                if (process_kill_pending()) {                                  \
                    __rc = -EINTR;                                             \
                    break;                                                     \
                }                                                              \
                if (wait_timeout_expired(&__wt)) {                             \
                    __rc = -ETIMEDOUT;                                         \
                    break;                                                     \
                }                                                              \
                sched_block_current();                                         \
            }                                                                  \
            waitqueue_finish((wq), &__we);                                     \
            timer_cancel_sync(&__t);                                           \
        }                                                                      \
        __rc;                                                                  \
    })

/* Sleep for at least `ns` (granularity: one tick). */
void thread_sleep_ns(uint64_t ns);
/* Same, but returns -EINTR early when the calling process is being killed. */
int thread_sleep_ns_killable(uint64_t ns);
static inline void thread_sleep_ms(uint64_t ms) { thread_sleep_ns(ms * 1000000ULL); }

#endif /* KERNEL_WAIT_H */
