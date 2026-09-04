/*
 * ioapic.c - I/O APIC driver.
 *
 * Indirect register access: write the register index to IOREGSEL, read
 * or write IOWIN. Each redirection entry is two 32-bit registers.
 */

#include <kernel/errno.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/spinlock.h>
#include <kernel/vmm.h>

#include <arch/irqc.h>

#include <x86/ioapic.h>

#define IOREGSEL 0x00
#define IOWIN    0x10

#define REG_ID      0x00
#define REG_VERSION 0x01
#define REG_REDTBL  0x10

#define RED_MASKED        (1u << 16)
#define RED_TRIGGER_LEVEL (1u << 15)
#define RED_POLARITY_LOW  (1u << 13)
#define RED_DEST_PHYSICAL 0u

#define MAX_IOAPICS 8

struct ioapic {
    volatile uint32_t *mmio;
    uint8_t id;
    uint32_t gsi_base;
    unsigned entries;
};

static struct ioapic g_ioapics[MAX_IOAPICS];
static unsigned g_count;
static unsigned g_gsi_count;
static spinlock_t g_lock = SPINLOCK_INIT("ioapic");

static uint32_t reg_read(struct ioapic *io, uint32_t reg)
{
    io->mmio[IOREGSEL / 4] = reg;
    return io->mmio[IOWIN / 4];
}

static void reg_write(struct ioapic *io, uint32_t reg, uint32_t v)
{
    io->mmio[IOREGSEL / 4] = reg;
    io->mmio[IOWIN / 4] = v;
}

int ioapic_add(uint8_t id, paddr_t address, uint32_t gsi_base)
{
    if (g_count >= MAX_IOAPICS)
        return -ENOSPC;

    vaddr_t va = vm_map_phys(page_align_down(address), PAGE_SIZE, VM_PROT_RW, VM_CACHE_UC);
    if (va == 0)
        return -ENOMEM;

    struct ioapic *io = &g_ioapics[g_count];
    io->mmio = (volatile uint32_t *)(va + (address & (PAGE_SIZE - 1)));
    io->id = id;
    io->gsi_base = gsi_base;
    io->entries = ((reg_read(io, REG_VERSION) >> 16) & 0xFF) + 1;

    /* Mask everything until a driver asks. */
    for (unsigned i = 0; i < io->entries; i++) {
        reg_write(io, REG_REDTBL + 2 * i, RED_MASKED);
        reg_write(io, REG_REDTBL + 2 * i + 1, 0);
    }

    g_count++;
    if (gsi_base + io->entries > g_gsi_count)
        g_gsi_count = gsi_base + io->entries;

    kdebug("ioapic: id %u at 0x%llx, GSI %u-%u", id, (unsigned long long)address, gsi_base,
           gsi_base + io->entries - 1);
    return 0;
}

static struct ioapic *find(unsigned gsi, unsigned *pin)
{
    for (unsigned i = 0; i < g_count; i++) {
        struct ioapic *io = &g_ioapics[i];
        if (gsi >= io->gsi_base && gsi < io->gsi_base + io->entries) {
            *pin = gsi - io->gsi_base;
            return io;
        }
    }
    return NULL;
}

bool ioapic_covers(unsigned gsi)
{
    unsigned pin;
    return find(gsi, &pin) != NULL;
}

unsigned ioapic_gsi_count(void)
{
    return g_gsi_count;
}

int ioapic_route(unsigned gsi, unsigned vector, uint32_t dest_apic_id, unsigned flags)
{
    unsigned pin;
    struct ioapic *io = find(gsi, &pin);
    if (io == NULL)
        return -ENODEV;
    if (vector < 32 || vector > 255)
        return -EINVAL;

    uint32_t lo = (vector & 0xFF) | RED_DEST_PHYSICAL | RED_MASKED;
    if (flags & ARCH_IRQ_TRIGGER_LEVEL)
        lo |= RED_TRIGGER_LEVEL;
    if (flags & ARCH_IRQ_POLARITY_LOW)
        lo |= RED_POLARITY_LOW;

    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    reg_write(io, REG_REDTBL + 2 * pin + 1, dest_apic_id << 24);
    reg_write(io, REG_REDTBL + 2 * pin, lo);
    spin_unlock_irqrestore(&g_lock, s);
    return 0;
}

static int set_mask(unsigned gsi, bool masked)
{
    unsigned pin;
    struct ioapic *io = find(gsi, &pin);
    if (io == NULL)
        return -ENODEV;

    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    uint32_t lo = reg_read(io, REG_REDTBL + 2 * pin);
    if (masked)
        lo |= RED_MASKED;
    else
        lo &= ~RED_MASKED;
    reg_write(io, REG_REDTBL + 2 * pin, lo);
    spin_unlock_irqrestore(&g_lock, s);
    return 0;
}

int ioapic_mask(unsigned gsi)
{
    return set_mask(gsi, true);
}

int ioapic_unmask(unsigned gsi)
{
    return set_mask(gsi, false);
}
