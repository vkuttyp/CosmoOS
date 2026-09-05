/*
 * arch/testhooks.h - Hardware test aids for kernel self-tests.
 *
 * Generic self-tests must not touch architecture devices directly. The
 * few devices a test needs (a periodic interrupt source for the IRQ
 * routing test) are behind this interface. Only self-test code may use
 * these; they are compiled in every build because the interface is
 * tiny and the implementation is a normal driver.
 */

#ifndef ARCH_TESTHOOKS_H
#define ARCH_TESTHOOKS_H

#include <kernel/compiler.h>

/* Start a periodic interrupt at roughly `hz` on a legacy ISA line and
 * return that ISA IRQ number (to be mapped with irq_legacy_to_gsi).
 * Returns -1 if the platform has no such source. */
int arch_test_periodic_irq_start(unsigned hz);
void arch_test_periodic_irq_stop(void);

/* Exercise the architecture's exception-entry paths that must work from
 * any instruction (x86-64: NMI-class vectors on their own stacks, with
 * the per-CPU pointer recovered even when the interrupted context had the
 * user's GS base). Returns true when every check passed, or when the
 * architecture has no such path (then *why is NULL); false with *why set
 * on failure. Runs with interrupts enabled from a kernel thread. */
bool arch_test_paranoid_entry(const char **why);

#endif /* ARCH_TESTHOOKS_H */
