/*
 * semaphore.c - Counting semaphore on a wait queue.
 */

#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/sched.h>
#include <kernel/semaphore.h>

void semaphore_init(struct semaphore *s, int count, const char *name)
{
    spinlock_init(&s->lock, name);
    s->count = count;
    waitqueue_init(&s->wq, name);
}

bool semaphore_trydown(struct semaphore *s)
{
    arch_irq_state_t st = spin_lock_irqsave(&s->lock);
    bool got = s->count > 0;
    if (got)
        s->count--;
    spin_unlock_irqrestore(&s->lock, st);
    return got;
}

void semaphore_down(struct semaphore *s)
{
    if (this_cpu()->irq_depth != 0)
        panic("semaphore_down in interrupt context");
    if (this_cpu()->preempt_count != 0)
        panic("semaphore_down with preemption disabled (count %d): a spinlock is held", this_cpu()->preempt_count);
    for (;;) {
        if (semaphore_trydown(s))
            return;
        wait_event(&s->wq, __atomic_load_n(&s->count, __ATOMIC_ACQUIRE) > 0);
    }
}

void semaphore_up(struct semaphore *s)
{
    arch_irq_state_t st = spin_lock_irqsave(&s->lock);
    s->count++;
    spin_unlock_irqrestore(&s->lock, st);
    waitqueue_wake_one(&s->wq);
}

int semaphore_count(struct semaphore *s)
{
    return __atomic_load_n(&s->count, __ATOMIC_ACQUIRE);
}
