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

/* A GSI the machine wires to nothing, safe for a test to route to any
 * CPU and raise by hand, or -1 where the controller offers no such
 * line. arch_test_irq_raise makes it pending as if a device had
 * asserted it; the controller then delivers it wherever it is routed,
 * which is the point of the test. */
int  arch_test_irq_spare_gsi(void);
void arch_test_irq_raise(unsigned gsi);

/* A GSI the controller's MSI allocator would hand out next and that
 * nothing is bound to, or -1 where MSIs do not come out of the GSI
 * space at all (x86-64: they are vectors). Binding it and then asking
 * for an MSI is how `irq-msi-overlap` provokes the collision firmware
 * creates when it wires a device to a line inside the MSI frame. */
int arch_test_msi_overlap_gsi(void);

/* True when the controller translates an MSI per writing device, so the
 * device id `irq_request_msi` carries is load-bearing and a device it
 * cannot describe must be refused (a GICv3 ITS). False where the id is
 * ignored, and the test that checks the refusal has nothing to check. */
bool arch_test_msi_per_device(void);

/* The host's own virtual-timer control register, for the test that a
 * guest's timer does not outlive the guest: read before and after a run,
 * it must not have changed. 0 where the architecture has no such
 * register. */
uint64_t arch_test_host_vtimer_ctl(void);

#endif /* ARCH_TESTHOOKS_H */
