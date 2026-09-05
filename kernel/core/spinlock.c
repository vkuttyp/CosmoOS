/*
 * spinlock.c - Test-and-set spinlock with owner diagnostics.
 *
 * Acquire is an atomic exchange with acquire ordering; release is a store
 * with release ordering. The owner field is written after acquire and
 * cleared before release, under the lock, so it is only ever read by a
 * contender for diagnostics.
 */

#include <kernel/lockdep.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/spinlock.h>

#include <arch/cpu.h>

void spinlock_init(spinlock_t *lock, const char *name)
{
    lock->locked = 0;
    lock->owner_cpu = SPINLOCK_NO_OWNER;
    lock->name = name;
    lock->class = 0;
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
static void lock_common(spinlock_t *lock, unsigned subclass, uintptr_t ip)
{
    preempt_disable();
    unsigned cpu = arch_cpu_id();
    bool irqs_on = arch_irq_enabled();
    /* The order check runs before the wait: a deadlocking acquisition is
     * reported instead of hanging. The push waits for ownership: while we
     * spin with interrupts enabled a handler may run here and must not see
     * this lock as held. */
    lockdep_acquire_check(&lock->class, lock->name, LOCKDEP_KIND_SPIN, subclass, irqs_on, ip);

    while (!try_acquire(lock)) {
        if (__atomic_load_n(&lock->owner_cpu, __ATOMIC_RELAXED) == cpu)
            panic("spinlock '%s' re-acquired on CPU %u (deadlock)", lock->name ? lock->name : "?", cpu);
        arch_cpu_relax();
    }
    __atomic_store_n(&lock->owner_cpu, cpu, __ATOMIC_RELAXED);
    lockdep_acquired(lock, &lock->class, lock->name, LOCKDEP_KIND_SPIN, subclass, false, irqs_on, ip);
}

void spin_lock(spinlock_t *lock)
{
    lock_common(lock, 0, (uintptr_t)__builtin_return_address(0));
}

void spin_lock_nested(spinlock_t *lock, unsigned subclass)
{
    lock_common(lock, subclass, (uintptr_t)__builtin_return_address(0));
}

bool spin_trylock(spinlock_t *lock)
{
    preempt_disable();
    if (!try_acquire(lock)) {
        preempt_enable();
        return false;
    }
    __atomic_store_n(&lock->owner_cpu, arch_cpu_id(), __ATOMIC_RELAXED);
    lockdep_acquired(lock, &lock->class, lock->name, LOCKDEP_KIND_SPIN, 0, true, arch_irq_enabled(),
                     (uintptr_t)__builtin_return_address(0));
    return true;
}

void spin_unlock(spinlock_t *lock)
{
    KASSERT(__atomic_load_n(&lock->locked, __ATOMIC_RELAXED) != 0);
    lockdep_release(lock, LOCKDEP_KIND_SPIN, (uintptr_t)__builtin_return_address(0));
    __atomic_store_n(&lock->owner_cpu, SPINLOCK_NO_OWNER, __ATOMIC_RELAXED);
    __atomic_store_n(&lock->locked, 0u, __ATOMIC_RELEASE);
    preempt_enable();
}

arch_irq_state_t spin_lock_irqsave(spinlock_t *lock)
{
    arch_irq_state_t state = arch_irq_save();
    lock_common(lock, 0, (uintptr_t)__builtin_return_address(0));
    return state;
}

arch_irq_state_t spin_lock_irqsave_nested(spinlock_t *lock, unsigned subclass)
{
    arch_irq_state_t state = arch_irq_save();
    lock_common(lock, subclass, (uintptr_t)__builtin_return_address(0));
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

/* Module ABI v1 exports (docs/kernel/module/api.md). */
#include <kernel/module.h>
EXPORT_SYMBOL(spinlock_init);
EXPORT_SYMBOL(spin_lock);
EXPORT_SYMBOL(spin_lock_nested);
EXPORT_SYMBOL(spin_lock_irqsave_nested);
EXPORT_SYMBOL(spin_unlock);
EXPORT_SYMBOL(spin_trylock);
EXPORT_SYMBOL(spin_lock_irqsave);
EXPORT_SYMBOL(spin_unlock_irqrestore);
