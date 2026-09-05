/*
 * completion.c - One-shot completion on a wait queue.
 */

#include <kernel/completion.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/sched.h>

void completion_init(struct completion *c, const char *name)
{
    spinlock_init(&c->lock, name);
    c->done = false;
    waitqueue_init(&c->wq, name);
}

void complete(struct completion *c)
{
    arch_irq_state_t s = spin_lock_irqsave(&c->lock);
    c->done = true;
    spin_unlock_irqrestore(&c->lock, s);
    waitqueue_wake_all(&c->wq);
}

bool completion_done(struct completion *c)
{
    return __atomic_load_n(&c->done, __ATOMIC_ACQUIRE);
}

void wait_for_completion(struct completion *c)
{
    if (this_cpu()->irq_depth != 0)
        panic("wait_for_completion in interrupt context");
    if (this_cpu()->preempt_count != 0)
        panic("wait_for_completion with preemption disabled (count %d): a spinlock is held",
              this_cpu()->preempt_count);
    wait_event(&c->wq, completion_done(c));
}
