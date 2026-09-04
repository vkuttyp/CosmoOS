/*
 * Host shim for arch/irq.h. Host unit tests are single-threaded; the
 * interrupt state is a no-op that always reports "enabled".
 */

#ifndef HOST_SHIM_ARCH_IRQ_H
#define HOST_SHIM_ARCH_IRQ_H

#include <stdbool.h>

typedef unsigned long arch_irq_state_t;

static inline arch_irq_state_t arch_irq_save(void) { return 1; }
static inline void arch_irq_restore(arch_irq_state_t state) { (void)state; }
static inline void arch_irq_enable(void) {}
static inline void arch_irq_disable(void) {}
static inline bool arch_irq_enabled(void) { return true; }

#endif /* HOST_SHIM_ARCH_IRQ_H */
