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

#endif /* ARCH_PCI_H */
