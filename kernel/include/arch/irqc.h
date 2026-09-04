/*
 * arch/irqc.h - Interrupt controller interface every architecture
 * implements. Generic code speaks in global system interrupt numbers
 * (GSI), vectors, and CPU indices; controller registers never appear
 * above this line.
 */

#ifndef ARCH_IRQC_H
#define ARCH_IRQC_H

#include <kernel/compiler.h>

/* Routing flags (mirrored by kernel/irq.h). */
#define ARCH_IRQ_TRIGGER_LEVEL (1u << 0)  /* default edge */
#define ARCH_IRQ_POLARITY_LOW  (1u << 1)  /* default active high */

/* Boot CPU: discover and initialise all controllers. Requires ACPI. */
void arch_irqc_init(void);

/* Calling CPU: initialise its local controller (APs during bring-up). */
void arch_irqc_init_cpu(void);

/* Allocate/free a vector from the dynamic range. -ENOSPC when full. */
int  arch_vector_alloc(void);
void arch_vector_free(unsigned vector);

/* Program `gsi` to deliver `vector` to `cpu`, initially masked.
 * -ENODEV if no controller covers the GSI. */
int  arch_irqc_route(unsigned gsi, unsigned vector, unsigned cpu, unsigned flags);
int  arch_irqc_mask(unsigned gsi);
int  arch_irqc_unmask(unsigned gsi);

/* Acknowledge `vector` at the controller after its handler ran. Safe for
 * every vector; a no-op for exceptions and for controllers not yet up. */
void arch_irqc_eoi(unsigned vector);

/* Compose the message a device must write to raise `vector` on `cpu`
 * (x86: APIC address + data). -EINVAL for an unknown CPU. */
int arch_irqc_msi_compose(unsigned vector, unsigned cpu, uint64_t *addr, uint32_t *data);

/* Highest GSI + 1 the controllers cover (0 if none). */
unsigned arch_irqc_gsi_count(void);

/* Vector reserved for spurious local-controller interrupts. */
unsigned arch_irqc_spurious_vector(void);

/* Inter-processor interrupts (used by the SMP work). */
void arch_ipi_send(unsigned cpu, unsigned vector);
void arch_ipi_broadcast_others(unsigned vector);

#endif /* ARCH_IRQC_H */
