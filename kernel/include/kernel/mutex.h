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

/* struct mutex { lock; owner; wq; name; lock_class (lockdep class + 1,
 * cached at first acquisition); pi_active (a waiter has donated to the
 * owner: unlock runs the PI restore); pi_link (in its owner's pi_held list
 * while it has waiters); handoff (a waiter overtaken once, which the next
 * unlock hands the mutex to, S33); } is defined in CeeSafe
 * (kernel/scheduler/mutex.cee) and generated into mutex_types.h (roadmap
 * 2.0 SA3; CeeSafe ADR 0029, 0030). `lock_class` was `class`, a CeeSafe keyword; `name` is `char *` (a CeeSafe
 * struct's pointer members carry no const), so MUTEX_INIT_AT casts it. */
#include <kernel/mutex_types.h>

void mutex_init(struct mutex *m, const char *name);
/* A mutex in static storage, built at compile time as mutex_init leaves
 * one (mutex.c, wait.c, spinlock.c): p is the mutex's own address, an
 * address constant, which the self-pointing lists need. CeeSafe's static
 * Mutex<T> uses it (CeeSafe ADR 0027); the self-test cee-kcore compares a
 * mutex it built with one from mutex_init. A change to struct mutex or to
 * mutex_init must keep the two equal. */
#define MUTEX_INIT_AT(p, n) { .lock = SPINLOCK_INIT(n), .owner = NULL, \
    .wq = { .lock = SPINLOCK_INIT(n), .waiters = LIST_HEAD_INIT((p)->wq.waiters), \
            .callbacks = LIST_HEAD_INIT((p)->wq.callbacks) }, \
    .name = (char *)(uintptr_t)(n), .lock_class = 0, .pi_active = 0, .pi_link = LIST_HEAD_INIT((p)->pi_link), .handoff = NULL }
void mutex_lock(struct mutex *m);
/* As mutex_lock, annotated for nesting inside another mutex of the same
 * class (docs/kernel/lockdep/invariants.md lists every use). */
void mutex_lock_nested(struct mutex *m, unsigned subclass);
bool mutex_trylock(struct mutex *m);
void mutex_unlock(struct mutex *m);
bool mutex_is_locked(struct mutex *m);

#endif /* KERNEL_MUTEX_H */
