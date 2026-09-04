/*
 * mutex.h - Sleeping mutual exclusion.
 *
 * Owner-tracked, not recursive, no priority inheritance. Must not be
 * used from interrupt context or with preemption disabled (asserted).
 * Unlock by a non-owner panics.
 */

#ifndef KERNEL_MUTEX_H
#define KERNEL_MUTEX_H

#include <kernel/spinlock.h>
#include <kernel/wait.h>

struct thread;

struct mutex {
    spinlock_t lock;
    struct thread *owner;
    struct waitqueue wq;
    const char *name;
};

void mutex_init(struct mutex *m, const char *name);
void mutex_lock(struct mutex *m);
bool mutex_trylock(struct mutex *m);
void mutex_unlock(struct mutex *m);
bool mutex_is_locked(struct mutex *m);

#endif /* KERNEL_MUTEX_H */
