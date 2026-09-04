/*
 * x86/ioapic.h - I/O APIC redirection programming. Private to x86-64.
 */

#ifndef X86_IOAPIC_H
#define X86_IOAPIC_H

#include <kernel/types.h>

/* Register an I/O APIC discovered by ACPI; maps its MMIO. */
int ioapic_add(uint8_t id, paddr_t address, uint32_t gsi_base);

/* True if some I/O APIC covers `gsi`. */
bool ioapic_covers(unsigned gsi);
unsigned ioapic_gsi_count(void);

/* Program a redirection entry, masked. `flags` uses ARCH_IRQ_*. */
int ioapic_route(unsigned gsi, unsigned vector, uint32_t dest_apic_id, unsigned flags);
int ioapic_mask(unsigned gsi);
int ioapic_unmask(unsigned gsi);

#endif /* X86_IOAPIC_H */
