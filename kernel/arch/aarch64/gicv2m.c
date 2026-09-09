/*
 * gicv2m.c - The GIC MSI frame, shared by the GICv2 and GICv3 drivers
 * (aarch64/gicv2m.h).
 */

#include <kernel/errno.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/vmm.h>
#include <aarch64/gicv2m.h>
#include <aarch64/platform.h>

#define V2M_MSI_TYPER     0x008
#define V2M_MSI_SETSPI_NS 0x040

void gicv2m_init(struct gicv2m *m, const struct acpi_gic *acpi, unsigned nr_lines)
{
    static const spinlock_t init_lock = SPINLOCK_INIT("gicv2m");
    m->lock = init_lock;
    m->pa = acpi->v2m_base;
    if (m->pa == 0)
        return;

    vaddr_t va = vm_map_phys(m->pa, 0x1000, VM_PROT_RW, VM_CACHE_UC);
    if (va == 0)
        panic("gicv2m: cannot map the MSI frame at 0x%llx", (unsigned long long)m->pa);
    m->regs = (volatile uint32_t *)va;

    uint32_t typer = m->regs[V2M_MSI_TYPER / 4];
    m->spi_base = (typer >> 16) & 0x3FF;
    m->spi_count = typer & 0x3FF;
    if (acpi->v2m_spi_count) {   /* the MADT entry overrides the frame */
        m->spi_base = acpi->v2m_spi_base;
        m->spi_count = acpi->v2m_spi_count;
    }
    if (m->spi_base < GIC_SPI_BASE || m->spi_base + m->spi_count > nr_lines) {
        kwarn("gicv2m: SPI range %u+%u is outside the distributor's %u lines; MSI disabled",
              m->spi_base, m->spi_count, nr_lines);
        m->spi_count = 0;
    }
}

int gicv2m_alloc(struct gicv2m *m, unsigned *intid)
{
    if (m->spi_count == 0)
        return -EINVAL;
    arch_irq_state_t s = spin_lock_irqsave(&m->lock);
    for (unsigned k = 0; k < m->spi_count; k++) {
        if ((m->used[k / 64] & (1ull << (k % 64))) == 0) {
            m->used[k / 64] |= 1ull << (k % 64);
            spin_unlock_irqrestore(&m->lock, s);
            *intid = m->spi_base + k;
            return 0;
        }
    }
    spin_unlock_irqrestore(&m->lock, s);
    return -ENOSPC;
}

bool gicv2m_owns(const struct gicv2m *m, unsigned intid)
{
    return m->spi_count != 0 && intid >= m->spi_base && intid < m->spi_base + m->spi_count;
}

void gicv2m_free(struct gicv2m *m, unsigned intid)
{
    KASSERT(gicv2m_owns(m, intid));
    unsigned k = intid - m->spi_base;
    arch_irq_state_t s = spin_lock_irqsave(&m->lock);
    m->used[k / 64] &= ~(1ull << (k % 64));
    spin_unlock_irqrestore(&m->lock, s);
}

paddr_t gicv2m_setspi_addr(const struct gicv2m *m)
{
    return m->pa + V2M_MSI_SETSPI_NS;
}
