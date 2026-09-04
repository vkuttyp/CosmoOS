/*
 * semaphore.h - Counting semaphore.
 *
 * semaphore_down blocks until the count is positive (not usable from
 * interrupt context); semaphore_up is interrupt-safe.
 */

#ifndef KERNEL_SEMAPHORE_H
#define KERNEL_SEMAPHORE_H

#include <kernel/spinlock.h>
#include <kernel/wait.h>

struct semaphore {
    spinlock_t lock;
    int count;
    struct waitqueue wq;
};

void semaphore_init(struct semaphore *s, int count, const char *name);
void semaphore_down(struct semaphore *s);
bool semaphore_trydown(struct semaphore *s);
void semaphore_up(struct semaphore *s);
int  semaphore_count(struct semaphore *s);

#endif /* KERNEL_SEMAPHORE_H */
