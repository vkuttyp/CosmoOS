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
 * Diagnostics: each lock records its owning CPU and a name. The name is
 * also the lock's class for the debug-build lock-order checker
 * (docs/kernel/lockdep/): one initialisation site, one class. Taking a
 * lock while another of the same class is held is a report unless the
 * inner one is annotated with spin_lock_nested(lock, subclass).
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
    uint16_t class;       /* lockdep class + 1, cached at first acquisition; 0 unknown */
    uint16_t pad[3];
} spinlock_t;

#define SPINLOCK_INIT(lockname) { .locked = 0, .owner_cpu = SPINLOCK_NO_OWNER, .name = (lockname), .class = 0 }

void spinlock_init(spinlock_t *lock, const char *name);

/* Acquire without touching the interrupt state. Use only for locks that
 * are never taken from interrupt context. */
void spin_lock(spinlock_t *lock);
void spin_unlock(spinlock_t *lock);

/* As spin_lock, annotated for legitimate nesting inside another lock of
 * the same class (subclass 1..3; docs/kernel/lockdep/invariants.md lists
 * every use). */
void spin_lock_nested(spinlock_t *lock, unsigned subclass);
arch_irq_state_t spin_lock_irqsave_nested(spinlock_t *lock, unsigned subclass);

/* Try once; true if acquired. */
bool spin_trylock(spinlock_t *lock);

/* Disable interrupts, acquire; restore on unlock. */
arch_irq_state_t spin_lock_irqsave(spinlock_t *lock);
void spin_unlock_irqrestore(spinlock_t *lock, arch_irq_state_t state);

/* True if the calling CPU holds the lock (diagnostics and assertions). */
bool spin_is_held(const spinlock_t *lock);

#endif /* KERNEL_SPINLOCK_H */
