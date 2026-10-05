/*
 * arch/irq.h - Local interrupt enable/disable.
 *
 * These control the calling CPU's interrupt acceptance only. They are the
 * building block for spinlocks and short critical sections and are safe
 * in any context. arch_irq_save/restore nest correctly; plain
 * enable/disable do not and are for boot code that knows the state.
 */

#ifndef ARCH_IRQ_H
#define ARCH_IRQ_H

#include <kernel/compiler.h>

typedef unsigned long arch_irq_state_t;

/* Whether a saved state restores IRQ delivery as enabled. */
bool arch_irq_state_enabled(arch_irq_state_t state);

/* The hardware operations, per architecture. */
arch_irq_state_t arch_irq_save_hw(void);
void arch_irq_restore_hw(arch_irq_state_t state);

/*
 * Disable interrupts and return the previous state; restore it. With
 * lockdep these also check the pairing (docs/kernel/lockdep/design.md,
 * "Raw interrupt-state pairing"): a restore must undo the innermost
 * outstanding save of the same context, with interrupts still masked
 * inside it. Without lockdep they are the hardware operations.
 */
#if defined(CONFIG_LOCKDEP) && CONFIG_LOCKDEP
arch_irq_state_t arch_irq_save(void);
void arch_irq_restore(arch_irq_state_t state);
#else
static inline arch_irq_state_t arch_irq_save(void) { return arch_irq_save_hw(); }
static inline void arch_irq_restore(arch_irq_state_t state) { arch_irq_restore_hw(state); }
#endif

void arch_irq_enable(void);
void arch_irq_disable(void);
bool arch_irq_enabled(void);

#endif /* ARCH_IRQ_H */
