/*
 * pci_bar.h - The size a PCI memory BAR decodes (drivers/pci/pci.c,
 * decode_bars; docs/drivers/pci/invariants.md P2).
 */

#ifndef KERNEL_PCI_BAR_H
#define KERNEL_PCI_BAR_H

#include <stdint.h>

/*
 * `mask` is what the BAR reads back after all ones were written, flag bits
 * cleared; a 32-bit BAR's upper half is all ones. The size is the mask's
 * lowest set bit: the address bits the device decodes are writable, the
 * ones below its size read zero. Not ~mask + 1, which assumes every bit
 * above the size is writable: a 64-bit BAR may hard-wire its upper half
 * to zero (the device decodes below 4 GiB only), and ~mask + 1 then gave
 * Parallels' NVMe controller a 16 KiB BAR of about 16 EiB, which nothing
 * could map (docs/audit/2026-10-13-acpi-discovery-report.md). For every
 * BAR whose upper bits are writable -- every one QEMU offers -- the two
 * agree. 0 for a mask of 0.
 */
static inline uint64_t pci_bar_mem_size(uint64_t mask)
{
    return mask & ~(mask - 1);
}

#endif /* KERNEL_PCI_BAR_H */
