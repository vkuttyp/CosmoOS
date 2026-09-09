/*
 * gicv2m.h - A GIC MSI frame (kernel/arch/aarch64/gicv2m.c).
 *
 * GICv2m is a small MMIO frame that turns a device's memory write into
 * an SPI: the device writes the SPI number to `SETSPI_NS` and the
 * distributor raises it. It is the only MSI mechanism a GICv2 has, and
 * it is still what QEMU offers under `gic-version=3,msi=gicv2m`, so
 * both drivers can be handed one. The frame owns nothing but its own
 * SPI range -- routing and enabling the SPI it hands back is the
 * distributor's business, and so the driver's.
 */

#ifndef AARCH64_GICV2M_H
#define AARCH64_GICV2M_H

#include <kernel/acpi.h>
#include <kernel/spinlock.h>
#include <kernel/types.h>

struct gicv2m {
    volatile uint32_t *regs;
    paddr_t pa;
    unsigned spi_base, spi_count;   /* spi_count 0: no usable frame */
    uint64_t used[32];              /* up to 2048 SPIs */
    spinlock_t lock;
};

/* Map the frame the MADT described and read its SPI range, which the
 * MADT may override. Leaves `spi_count` 0 -- MSI unavailable -- when
 * there is no frame, or when its range falls outside the `nr_lines`
 * lines the distributor reported. */
void gicv2m_init(struct gicv2m *m, const struct acpi_gic *acpi, unsigned nr_lines);

/* Take an SPI from the frame. -EINVAL when there is no frame, -ENOSPC
 * when every SPI is taken. The caller routes and enables it. */
int  gicv2m_alloc(struct gicv2m *m, unsigned *intid);
void gicv2m_free(struct gicv2m *m, unsigned intid);
bool gicv2m_owns(const struct gicv2m *m, unsigned intid);

/* The address a device writes, with the SPI number as its data. */
paddr_t gicv2m_setspi_addr(const struct gicv2m *m);

#endif /* AARCH64_GICV2M_H */
