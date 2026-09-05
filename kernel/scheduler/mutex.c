/*
 * mutex.c - Sleeping mutex on a wait queue.
 */

#include <kernel/lockdep.h>
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
    m->class = 0;
    waitqueue_init(&m->wq, name);
}

static bool try_take(struct mutex *m, struct thread *cur)
{
    arch_irq_state_t s = spin_lock_irqsave(&m->lock);
    bool got = m->owner == NULL;
    if (got)
        m->owner = cur;
    spin_unlock_irqrestore(&m->lock, s);
    return got;
}

bool mutex_trylock(struct mutex *m)
{
    struct thread *cur = thread_current();
    if (!try_take(m, cur))
        return false;
    lockdep_acquired(m, &m->class, m->name, LOCKDEP_KIND_MUTEX, 0, true, false, (uintptr_t)__builtin_return_address(0));
    return true;
}

static void lock_common(struct mutex *m, unsigned subclass, uintptr_t ip)
{
    struct thread *cur = thread_current();
    if (this_cpu()->irq_depth != 0)
        panic("mutex_lock('%s') in interrupt context", m->name);
    /* Checked on every acquisition, not only when the mutex is contended
     * and wait_event would notice: a sleeping lock taken under a spinlock
     * (preempt_count > 0) must fail deterministically, in the first test
     * that runs the path, not only under load (invariant S6). */
    might_sleep();
    if (m->owner == cur)
        panic("mutex_lock('%s'): recursive lock by '%s'", m->name, cur->name);
    /* The order check runs before the wait so a deadlocking acquisition
     * is reported, not hung on; the push waits for ownership. */
    lockdep_acquire_check(&m->class, m->name, LOCKDEP_KIND_MUTEX, subclass, false, ip);

    for (;;) {
        if (try_take(m, cur))
            break;
        wait_event(&m->wq, __atomic_load_n(&m->owner, __ATOMIC_ACQUIRE) == NULL);
    }
    lockdep_acquired(m, &m->class, m->name, LOCKDEP_KIND_MUTEX, subclass, false, false, ip);
}

void mutex_lock(struct mutex *m)
{
    lock_common(m, 0, (uintptr_t)__builtin_return_address(0));
}

void mutex_lock_nested(struct mutex *m, unsigned subclass)
{
    lock_common(m, subclass, (uintptr_t)__builtin_return_address(0));
}

void mutex_unlock(struct mutex *m)
{
    struct thread *cur = thread_current();
    arch_irq_state_t s = spin_lock_irqsave(&m->lock);
    if (m->owner != cur)
        panic("mutex_unlock('%s') by '%s' but owner is '%s'", m->name, cur->name,
              m->owner ? m->owner->name : "nobody");
    lockdep_release(m, LOCKDEP_KIND_MUTEX, (uintptr_t)__builtin_return_address(0));
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
EXPORT_SYMBOL(mutex_lock_nested);
EXPORT_SYMBOL(mutex_trylock);
EXPORT_SYMBOL(mutex_unlock);
