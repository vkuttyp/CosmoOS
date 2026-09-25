/*
 * completion.c - One-shot completion on a wait queue.
 */

#include <arch/cpu.h>
#include <kernel/completion.h>
#include <kernel/lockdep.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/sched.h>
#include <kernel/timer.h>

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
void complete_linger(struct completion *c, uint64_t linger_ns)
{
    arch_irq_state_t s = spin_lock_irqsave(&c->lock);
    c->done = true;
    /* The window a poller races: `done` is visible, the wake has not run,
     * the lock is still held. A test widens it to prove a waiter's
     * handshake takes this lock before returning; production passes 0. */
    if (linger_ns != 0) {
        uint64_t until = clock_now_ns() + linger_ns;
        while (clock_now_ns() < until)
            arch_cpu_relax();
    }
    waitqueue_wake_all(&c->wq);
    spin_unlock_irqrestore(&c->lock, s);
}

void complete(struct completion *c)
{
    complete_linger(c, 0);
}

bool completion_done(struct completion *c)
{
    return __atomic_load_n(&c->done, __ATOMIC_ACQUIRE);
}

void wait_for_completion(struct completion *c)
{
    if (raw_this_cpu()->irq_depth != 0)   /* identity: zero on any CPU a sleeping caller runs on */
        panic("wait_for_completion in interrupt context");
    might_sleep();
    wait_event(&c->wq, completion_done(c));
    /* The handshake: complete() holds c->lock until its wake has
     * returned, so once this lock is ours nothing is still inside `c`. */
    arch_irq_state_t s = spin_lock_irqsave(&c->lock);
    spin_unlock_irqrestore(&c->lock, s);
}

bool wait_for_completion_timeout(struct completion *c, uint64_t timeout_ns)
{
    if (raw_this_cpu()->irq_depth != 0)   /* identity: zero on any CPU a sleeping caller runs on */
        panic("wait_for_completion_timeout in interrupt context");
    might_sleep();
    if (!wait_event_timeout(&c->wq, completion_done(c), timeout_ns))
        return false;   /* the deadline: `c` may complete later, caller must not free it yet */
    /* Done: the same handshake wait_for_completion does, so the completer
     * has let go of `c` before this returns. */
    arch_irq_state_t s = spin_lock_irqsave(&c->lock);
    spin_unlock_irqrestore(&c->lock, s);
    return true;
}

/* Module ABI exports (docs/kernel/module/api.md): drivers wait for their
 * own commands with a completion (NVMe admin commands). */
#include <kernel/module.h>
EXPORT_SYMBOL(completion_init);
EXPORT_SYMBOL(complete);
EXPORT_SYMBOL(complete_linger);
EXPORT_SYMBOL(completion_done);
EXPORT_SYMBOL(wait_for_completion);
EXPORT_SYMBOL(wait_for_completion_timeout);
