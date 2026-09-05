/*
 * completion.c - One-shot completion on a wait queue.
 */

#include <kernel/completion.h>
#include <kernel/lockdep.h>
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
    might_sleep();
    wait_event(&c->wq, completion_done(c));
}

/* Module ABI exports (docs/kernel/module/api.md): drivers wait for their
 * own commands with a completion (NVMe admin commands). */
#include <kernel/module.h>
EXPORT_SYMBOL(completion_init);
EXPORT_SYMBOL(complete);
EXPORT_SYMBOL(completion_done);
EXPORT_SYMBOL(wait_for_completion);
