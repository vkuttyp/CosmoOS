/*
 * mutex.c - Sleeping mutex on a wait queue.
 */

#include <kernel/mutex.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/sched.h>
#include <kernel/thread.h>

void mutex_init(struct mutex *m, const char *name)
{
    spinlock_init(&m->lock, name);
    m->owner = NULL;
    m->name = name;
    waitqueue_init(&m->wq, name);
}

bool mutex_trylock(struct mutex *m)
{
    struct thread *cur = thread_current();
    arch_irq_state_t s = spin_lock_irqsave(&m->lock);
    bool got = m->owner == NULL;
    if (got)
        m->owner = cur;
    spin_unlock_irqrestore(&m->lock, s);
    return got;
}

void mutex_lock(struct mutex *m)
{
    struct thread *cur = thread_current();
    if (this_cpu()->irq_depth != 0)
        panic("mutex_lock('%s') in interrupt context", m->name);
    /* Checked on every acquisition, not only when the mutex is contended
     * and wait_event would notice: a sleeping lock taken under a spinlock
     * (preempt_count > 0) must fail deterministically, in the first test
     * that runs the path, not only under load (invariant S6). */
    if (this_cpu()->preempt_count != 0)
        panic("mutex_lock('%s') with preemption disabled (count %d): a spinlock is held", m->name,
              this_cpu()->preempt_count);
    if (m->owner == cur)
        panic("mutex_lock('%s'): recursive lock by '%s'", m->name, cur->name);

    for (;;) {
        if (mutex_trylock(m))
            return;
        wait_event(&m->wq, __atomic_load_n(&m->owner, __ATOMIC_ACQUIRE) == NULL);
    }
}

void mutex_unlock(struct mutex *m)
{
    struct thread *cur = thread_current();
    arch_irq_state_t s = spin_lock_irqsave(&m->lock);
    if (m->owner != cur)
        panic("mutex_unlock('%s') by '%s' but owner is '%s'", m->name, cur->name,
              m->owner ? m->owner->name : "nobody");
    __atomic_store_n(&m->owner, NULL, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&m->lock, s);
    waitqueue_wake_one(&m->wq);
}

bool mutex_is_locked(struct mutex *m)
{
    return __atomic_load_n(&m->owner, __ATOMIC_ACQUIRE) != NULL;
}

/* Module ABI v1 exports (docs/kernel/module/api.md). */
#include <kernel/module.h>
EXPORT_SYMBOL(mutex_init);
EXPORT_SYMBOL(mutex_lock);
EXPORT_SYMBOL(mutex_trylock);
EXPORT_SYMBOL(mutex_unlock);
