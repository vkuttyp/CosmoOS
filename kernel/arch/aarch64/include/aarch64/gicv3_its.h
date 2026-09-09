/*
 * gicv3_its.h - The GICv3 Interrupt Translation Service
 * (kernel/arch/aarch64/gicv3_its.c).
 *
 * A GICv2m frame turns a device's write into one fixed SPI, which is
 * why its interrupts come out of the same small space every wired
 * device uses. An ITS instead *translates*: a device writes an event
 * number to one address, and the ITS looks up (DeviceID, EventID) in
 * tables the kernel built and raises a locally-specific interrupt (an
 * LPI) on the redistributor of whichever CPU that event was assigned
 * to. So it needs to know who wrote -- hence the device id that
 * `arch_irqc_msi_compose` now carries -- and it needs a command queue
 * to be told about each device and each event.
 */

#ifndef AARCH64_GICV3_ITS_H
#define AARCH64_GICV3_ITS_H

#include <kernel/types.h>

struct gicv3_its;

/* Bring up the ITS at `base`, able to translate to LPIs
 * `lpi_base .. lpi_base + nr_lpis`. NULL when it cannot be used; the
 * caller then falls back to a GICv2m frame or declines MSI. */
struct gicv3_its *its_init(paddr_t base, unsigned lpi_base, unsigned nr_lpis);

/* Give `cpu` a collection of its own, pointing at its redistributor.
 * `rd_pa`/`rd_index` are that redistributor's physical base and its
 * processor number; which one the ITS wants is its own business
 * (GITS_TYPER.PTA). */
void its_init_cpu(struct gicv3_its *its, unsigned cpu, paddr_t rd_pa, unsigned rd_index);

/* Route (`devid`, `event`) to `lpi` on `cpu`, mapping the device the
 * first time it is seen. -ENOSPC when the ITS has no room for another
 * device, -EINVAL when the device id is outside its tables. */
int  its_map_event(struct gicv3_its *its, uint32_t devid, uint32_t event, unsigned lpi, unsigned cpu);
void its_unmap_event(struct gicv3_its *its, uint32_t devid, uint32_t event, unsigned cpu);

/* The address a device writes, with the event id as its data. */
paddr_t its_translater(const struct gicv3_its *its);

#endif /* AARCH64_GICV3_ITS_H */
