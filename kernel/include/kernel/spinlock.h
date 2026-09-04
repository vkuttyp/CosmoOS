/*
 * spinlock.h - Busy-wait mutual exclusion for short critical sections.
 *
 * Rules:
 *   - A lock shared with interrupt context must be taken with
 *     spin_lock_irqsave() on every path, or an interrupt on the holding
 *     CPU deadlocks against it.
 *   - Never sleep, allocate with a blocking allocator, or take a sleeping
 *     lock while holding a spinlock. (There are no sleeping locks yet;
 *     the rule is stated now so it never becomes a surprise.)
 *   - Not recursive. Re-acquiring on the same CPU is a bug and panics
 *     immediately: on one CPU it would otherwise spin forever, and the
 *     owner check turns that hang into a report.
 *
 * Diagnostics: each lock records its owning CPU and a name. There is no
 * lock-order checker yet; the documented order in
 * docs/kernel/memory/architecture.md is enforced by review.
 */

#ifndef KERNEL_SPINLOCK_H
#define KERNEL_SPINLOCK_H

#include <kernel/compiler.h>

#include <arch/irq.h>

#define SPINLOCK_NO_OWNER 0xFFFFFFFFu

typedef struct spinlock {
    uint32_t locked;      /* 0 free, 1 held; accessed atomically */
    uint32_t owner_cpu;   /* diagnostics only */
    const char *name;
} spinlock_t;

#define SPINLOCK_INIT(lockname) { .locked = 0, .owner_cpu = SPINLOCK_NO_OWNER, .name = (lockname) }

void spinlock_init(spinlock_t *lock, const char *name);

/* Acquire without touching the interrupt state. Use only for locks that
 * are never taken from interrupt context. */
void spin_lock(spinlock_t *lock);
void spin_unlock(spinlock_t *lock);

/* Try once; true if acquired. */
bool spin_trylock(spinlock_t *lock);

/* Disable interrupts, acquire; restore on unlock. */
arch_irq_state_t spin_lock_irqsave(spinlock_t *lock);
void spin_unlock_irqrestore(spinlock_t *lock, arch_irq_state_t state);

/* True if the calling CPU holds the lock (diagnostics and assertions). */
bool spin_is_held(const spinlock_t *lock);

#endif /* KERNEL_SPINLOCK_H */
