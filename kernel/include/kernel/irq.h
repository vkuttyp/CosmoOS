/*
 * irq.h - Hardware interrupt lines by global system interrupt number.
 *
 * Drivers request a GSI with a handler; the subsystem allocates a
 * vector, installs the handler in the vector table, and programs the
 * controller with the entry masked. irq_enable() unmasks it. The handler
 * runs in interrupt context with the vector number; the controller is
 * acknowledged by the arch layer after the handler returns.
 *
 * Concurrency: the IRQ table is protected by a spinlock (irqsave).
 * irq_request/irq_release are not usable from interrupt context;
 * irq_enable/irq_disable are.
 */

#ifndef KERNEL_IRQ_H
#define KERNEL_IRQ_H

#include <kernel/compiler.h>
#include <kernel/interrupt.h>

typedef unsigned irq_t;

#define IRQ_MAX 1024u   /* GSIs: IOAPIC pins on x86-64, GIC INTIDs on AArch64 */

#define IRQ_TRIGGER_EDGE  0u
#define IRQ_TRIGGER_LEVEL (1u << 0)
#define IRQ_POLARITY_LOW  (1u << 1)

/* Called once by kernel_main after ACPI; brings up the controllers. */
void irq_init(void);

/* Returns 0, -EINVAL, -EBUSY (already requested), -ENODEV (no controller
 * for the GSI), -ENOSPC (no vector). Delivery goes to `cpu`. */
int irq_request(irq_t irq, interrupt_handler_fn fn, void *arg, const char *name, unsigned flags,
                unsigned cpu);
int irq_release(irq_t irq);

int irq_enable(irq_t irq);
int irq_disable(irq_t irq);

/* ISA IRQ (0-15) to GSI with the firmware's interrupt source overrides
 * applied; `flags_out` receives the trigger/polarity to pass to
 * irq_request. */
irq_t irq_legacy_to_gsi(unsigned isa_irq, unsigned *flags_out);

/* Diagnostics: vector assigned to a GSI, or -1. */
int irq_vector_of(irq_t irq);

/* Message-signalled interrupts. The bus programs the returned message
 * into the device; the interrupt then arrives as a plain vector on
 * `cpu` and is dispatched to fn like any other. Returns the vector
 * (>= 0) or -ENOSPC/-EINVAL. Not for interrupt context (spinlock, but
 * allocation-free); release with the vector. */
struct irq_msi_msg {
    uint64_t addr;
    uint32_t data;
};
int irq_request_msi(interrupt_handler_fn fn, void *arg, const char *name, unsigned cpu,
                    struct irq_msi_msg *msg);
int irq_release_msi(int vector);

#endif /* KERNEL_IRQ_H */
