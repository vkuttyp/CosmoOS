/*
 * mutex.h - Sleeping mutual exclusion.
 *
 * Owner-tracked, not recursive, with priority inheritance: an owner is
 * boosted to the priority of the highest-priority thread blocked on it (and
 * up the chain), and restored on release. Must not be used from interrupt
 * context or with preemption disabled (asserted).
 * Unlock by a non-owner panics.
 */

#ifndef KERNEL_MUTEX_H
#define KERNEL_MUTEX_H

#include <kernel/list.h>
#include <kernel/spinlock.h>
#include <kernel/wait.h>

struct thread;

struct mutex {
    spinlock_t lock;
    struct thread *owner;
    struct waitqueue wq;
    const char *name;
    uint16_t class;   /* lockdep class + 1 (kind mutex), cached at first acquisition */
    uint16_t pi_active;   /* a waiter has donated to the owner: unlock must run the PI restore (under g_pi_lock) */
    /* (pi, under g_pi_lock in mutex.c) links this mutex into its owner's
     * pi_held list while it has waiters, so the owner inherits their priority. */
    struct list_node pi_link;
    /* (lock) A waiter that was woken and found the mutex taken again (a
     * releaser re-took it before the waiter ran): the next unlock hands
     * the mutex to it instead of releasing it, so no waiter is overtaken
     * twice in a row (S33). NULL when none has asked. */
    struct thread *handoff;
};

void mutex_init(struct mutex *m, const char *name);
/* A mutex in static storage, built at compile time as mutex_init leaves
 * one (mutex.c, wait.c, spinlock.c): p is the mutex's own address, an
 * address constant, which the self-pointing lists need. Nova's static
 * Mutex<T> uses it (Nova ADR 0027); the self-test nova-kcore compares a
 * mutex it built with one from mutex_init. A change to struct mutex or to
 * mutex_init must keep the two equal. */
#define MUTEX_INIT_AT(p, n) { .lock = SPINLOCK_INIT(n), .owner = NULL, \
    .wq = { .lock = SPINLOCK_INIT(n), .waiters = LIST_HEAD_INIT((p)->wq.waiters), \
            .callbacks = LIST_HEAD_INIT((p)->wq.callbacks) }, \
    .name = (n), .class = 0, .pi_active = 0, .pi_link = LIST_HEAD_INIT((p)->pi_link), .handoff = NULL }
void mutex_lock(struct mutex *m);
/* As mutex_lock, annotated for nesting inside another mutex of the same
 * class (docs/kernel/lockdep/invariants.md lists every use). */
void mutex_lock_nested(struct mutex *m, unsigned subclass);
bool mutex_trylock(struct mutex *m);
void mutex_unlock(struct mutex *m);
bool mutex_is_locked(struct mutex *m);

#endif /* KERNEL_MUTEX_H */
