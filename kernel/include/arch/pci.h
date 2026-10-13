/*
 * arch/pci.h - Legacy PCI configuration access.
 *
 * Used only when ACPI provides no MCFG (no ECAM). x86 implements it over
 * ports 0xCF8/0xCFC; an architecture without such a mechanism returns
 * -ENODEV from arch_pci_legacy_available().
 */

#ifndef ARCH_PCI_H
#define ARCH_PCI_H

#include <kernel/types.h>

bool arch_pci_legacy_available(void);
uint32_t arch_pci_legacy_read(uint8_t bus, uint8_t slot, uint8_t func, uint16_t off, unsigned width);
void arch_pci_legacy_write(uint8_t bus, uint8_t slot, uint8_t func, uint16_t off, unsigned width, uint32_t v);

/* PCI I/O space, where an I/O BAR's registers are: x86-64 reaches it with
 * its port instructions. False from arch_pci_io_available on an
 * architecture that cannot (AArch64: the host bridge's I/O window is
 * described only in the DSDT, which this kernel does not interpret); the
 * read and write are then never called. width is 1, 2 or 4. */
bool arch_pci_io_available(void);
uint32_t arch_pci_io_read(uint32_t port, unsigned width);
void arch_pci_io_write(uint32_t port, unsigned width, uint32_t v);

#endif /* ARCH_PCI_H */
