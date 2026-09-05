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

/* Two threads owning vector state, pinned to the calling CPU, alternately
 * load distinct patterns and check they survive every switch. True when
 * no thread saw the other's registers, or when the architecture lets no
 * thread hold such state (*why NULL); false with *why on a leak. */
bool arch_test_fpu_switch(const char **why);

/* The calling thread's first vector register (x86-64: xmm0), for tests
 * that check state isolation across a guest run. The thread must own
 * state (arch_fpu_alloc); both return false when it does not or the
 * architecture has none. */
bool arch_test_fpu_set(const uint8_t pattern[16]);
bool arch_test_fpu_get(uint8_t out[16]);

#endif /* ARCH_TESTHOOKS_H */
