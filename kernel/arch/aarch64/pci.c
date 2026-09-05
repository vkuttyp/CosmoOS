/* pci.c - No legacy PCI configuration mechanism on AArch64; ECAM from the MCFG only. */

#include <arch/pci.h>

bool arch_pci_legacy_available(void)
{
    return false;
}

uint32_t arch_pci_legacy_read(uint8_t bus, uint8_t slot, uint8_t func, uint16_t off, unsigned width)
{
    (void)bus; (void)slot; (void)func; (void)off; (void)width;
    return 0xFFFFFFFFu;
}

void arch_pci_legacy_write(uint8_t bus, uint8_t slot, uint8_t func, uint16_t off, unsigned width, uint32_t v)
{
    (void)bus; (void)slot; (void)func; (void)off; (void)width; (void)v;
}
