/*
 * pci_legacy.c - PCI configuration mechanism #1 (ports 0xCF8/0xCFC).
 *
 * Reaches the first 256 bytes of each function's configuration space.
 * The address/data pair is one shared register, hence the spinlock.
 */

#include <kernel/spinlock.h>

#include <arch/pci.h>

#include <x86/io.h>

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

static spinlock_t g_lock = SPINLOCK_INIT("pci-legacy");

static uint32_t address(uint8_t bus, uint8_t slot, uint8_t func, uint16_t off)
{
    return 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)(slot & 0x1f) << 11) |
           ((uint32_t)(func & 7) << 8) | (uint32_t)(off & 0xfc);
}

bool arch_pci_legacy_available(void)
{
    return true;
}

uint32_t arch_pci_legacy_read(uint8_t bus, uint8_t slot, uint8_t func, uint16_t off, unsigned width)
{
    if (off >= 256)
        return 0xffffffffu;
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    outl(PCI_CONFIG_ADDR, address(bus, slot, func, off));
    uint32_t v = inl(PCI_CONFIG_DATA);
    spin_unlock_irqrestore(&g_lock, s);
    v >>= 8 * (off & 3);
    return width == 1 ? (v & 0xff) : width == 2 ? (v & 0xffff) : v;
}

void arch_pci_legacy_write(uint8_t bus, uint8_t slot, uint8_t func, uint16_t off, unsigned width, uint32_t v)
{
    if (off >= 256)
        return;
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    outl(PCI_CONFIG_ADDR, address(bus, slot, func, off));
    if (width == 4) {
        outl(PCI_CONFIG_DATA, v);
    } else if (width == 2) {
        outw((uint16_t)(PCI_CONFIG_DATA + (off & 2)), (uint16_t)v);
    } else {
        outb((uint16_t)(PCI_CONFIG_DATA + (off & 3)), (uint8_t)v);
    }
    spin_unlock_irqrestore(&g_lock, s);
}
