/*
 * spinlock.c - Test-and-set spinlock with owner diagnostics.
 *
 * Acquire is an atomic exchange with acquire ordering; release is a store
 * with release ordering. The owner field is written after acquire and
 * cleared before release, under the lock, so it is only ever read by a
 * contender for diagnostics.
 */

#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/spinlock.h>

#include <arch/cpu.h>

void spinlock_init(spinlock_t *lock, const char *name)
{
    lock->locked = 0;
    lock->owner_cpu = SPINLOCK_NO_OWNER;
    lock->name = name;
}

static inline bool try_acquire(spinlock_t *lock)
{
    return __atomic_exchange_n(&lock->locked, 1u, __ATOMIC_ACQUIRE) == 0;
}

/*
 * Holding a spinlock disables preemption on this CPU: a preempted holder
 * would leave every other contender spinning until it happened to run
 * again, and on one CPU that is forever.
 */
void spin_lock(spinlock_t *lock)
{
    preempt_disable();
    unsigned cpu = arch_cpu_id();

    while (!try_acquire(lock)) {
        if (__atomic_load_n(&lock->owner_cpu, __ATOMIC_RELAXED) == cpu)
            panic("spinlock '%s' re-acquired on CPU %u (deadlock)", lock->name ? lock->name : "?", cpu);
        arch_cpu_relax();
    }
    __atomic_store_n(&lock->owner_cpu, cpu, __ATOMIC_RELAXED);
}

bool spin_trylock(spinlock_t *lock)
{
    preempt_disable();
    if (!try_acquire(lock)) {
        preempt_enable();
        return false;
    }
    __atomic_store_n(&lock->owner_cpu, arch_cpu_id(), __ATOMIC_RELAXED);
    return true;
}

void spin_unlock(spinlock_t *lock)
{
    KASSERT(__atomic_load_n(&lock->locked, __ATOMIC_RELAXED) != 0);
    __atomic_store_n(&lock->owner_cpu, SPINLOCK_NO_OWNER, __ATOMIC_RELAXED);
    __atomic_store_n(&lock->locked, 0u, __ATOMIC_RELEASE);
    preempt_enable();
}

arch_irq_state_t spin_lock_irqsave(spinlock_t *lock)
{
    arch_irq_state_t state = arch_irq_save();
    spin_lock(lock);
    return state;
}

void spin_unlock_irqrestore(spinlock_t *lock, arch_irq_state_t state)
{
    spin_unlock(lock);
    arch_irq_restore(state);
}

bool spin_is_held(const spinlock_t *lock)
{
    return __atomic_load_n(&lock->locked, __ATOMIC_RELAXED) != 0 &&
           __atomic_load_n(&lock->owner_cpu, __ATOMIC_RELAXED) == arch_cpu_id();
}
