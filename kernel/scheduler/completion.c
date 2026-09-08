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
    /* Its own lock class: complete() wakes with c->lock held, and two
     * locks of one class nested is what lockdep calls recursion. */
    waitqueue_init(&c->wq, "completion-wq");
}

/*
 * `done` becomes visible and the waiters are woken under one hold of
 * c->lock, and wait_for_completion takes c->lock once after it has seen
 * `done`. So a waiter cannot return -- and free the completion, which
 * usually lives on its stack -- until complete() has finished touching
 * it. The first version dropped the lock between the two: a waiter that
 * arrived (or polled) in that window saw `done`, returned, and its frame
 * was reused while the wake ran on it; the AHCI unit's concurrent block
 * benchmark hit it as a spinlock assertion in wake() on a stack that by
 * then belonged to the next read (selftest completion-race).
 */
void complete(struct completion *c)
{
    arch_irq_state_t s = spin_lock_irqsave(&c->lock);
    c->done = true;
    waitqueue_wake_all(&c->wq);
    spin_unlock_irqrestore(&c->lock, s);
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
    /* The handshake: complete() holds c->lock until its wake has
     * returned, so once this lock is ours nothing is still inside `c`. */
    arch_irq_state_t s = spin_lock_irqsave(&c->lock);
    spin_unlock_irqrestore(&c->lock, s);
}

/* Module ABI exports (docs/kernel/module/api.md): drivers wait for their
 * own commands with a completion (NVMe admin commands). */
#include <kernel/module.h>
EXPORT_SYMBOL(completion_init);
EXPORT_SYMBOL(complete);
EXPORT_SYMBOL(completion_done);
EXPORT_SYMBOL(wait_for_completion);
