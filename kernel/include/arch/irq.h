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

/* Disable interrupts and return the previous state for restore. */
arch_irq_state_t arch_irq_save(void);
void arch_irq_restore(arch_irq_state_t state);

void arch_irq_enable(void);
void arch_irq_disable(void);
bool arch_irq_enabled(void);

#endif /* ARCH_IRQ_H */
