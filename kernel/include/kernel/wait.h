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

#include <kernel/list.h>
#include <kernel/spinlock.h>

struct thread;

struct wait_entry {
    struct list_node link;
    struct thread *thread;
};

struct waitqueue {
    spinlock_t lock;
    struct list_node waiters;
};

#define WAITQUEUE_INIT(name) { .lock = SPINLOCK_INIT(#name), .waiters = LIST_HEAD_INIT((name).waiters) }

void waitqueue_init(struct waitqueue *wq, const char *name);

/* Initialise an entry (once, before the first prepare). */
static inline void wait_entry_init(struct wait_entry *e)
{
    list_init(&e->link);
    e->thread = NULL;
}

/* Enqueue the current thread (if not already queued) and mark it
 * BLOCKED. Safe to call repeatedly with the same entry: a woken waiter
 * whose condition is still false calls it again without finishing. */
void waitqueue_prepare(struct waitqueue *wq, struct wait_entry *e);
/* Dequeue and mark RUNNING. */
void waitqueue_finish(struct waitqueue *wq, struct wait_entry *e);

/* Wake the first / every waiter. Return the number woken. */
unsigned waitqueue_wake_one(struct waitqueue *wq);
unsigned waitqueue_wake_all(struct waitqueue *wq);

bool waitqueue_empty(struct waitqueue *wq);

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

/* Sleep for at least `ns` (granularity: one tick). */
void thread_sleep_ns(uint64_t ns);
static inline void thread_sleep_ms(uint64_t ms) { thread_sleep_ns(ms * 1000000ULL); }

#endif /* KERNEL_WAIT_H */
