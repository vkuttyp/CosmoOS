/*
 * irqc.h - Which interrupt controller this machine has
 * (kernel/arch/aarch64/irqc.c).
 *
 * `arch/irqc.h` is what generic code calls and says nothing about
 * controllers; this is the layer below it, where AArch64 chooses between
 * the GICv2 driver (gic.c) and the GICv3 one (gicv3.c) from what the
 * MADT reported. The two share nothing but this table: they keep their
 * own interrupt-id bookkeeping, because the register-level operations
 * differ at every level and a single driver pretending to be both would
 * be harder to read than two that are each simple.
 *
 * Every entry point of `arch/irqc.h` has an entry here, plus the
 * interrupt dispatch itself, which reads a controller register to learn
 * what fired.
 */

#ifndef AARCH64_IRQC_H
#define AARCH64_IRQC_H

#include <kernel/acpi.h>
#include <kernel/compiler.h>
#include <kernel/types.h>

struct arch_trap_frame;

struct aarch64_irqc_ops {
    const char *name;
    /* Boot CPU, with what the MADT said. Panics if the machine is not
     * one this driver can drive. */
    void (*init)(const struct acpi_gic *gic);
    void (*init_cpu)(void);

    int  (*vector_alloc)(void);
    void (*vector_free)(unsigned vector);

    int  (*route)(unsigned gsi, unsigned vector, unsigned cpu, unsigned flags);
    int  (*mask)(unsigned gsi);
    int  (*unmask)(unsigned gsi);
    void (*eoi)(unsigned vector);
    int  (*msi_compose)(unsigned vector, unsigned cpu, uint32_t devid, uint64_t *addr, uint32_t *data);

    /* Where this driver's MSI doorbell is, for an IOMMU to let
     * through; false when it has no MSI at all. */
    bool (*msi_doorbell)(paddr_t *pa, size_t *len);

    unsigned (*gsi_count)(void);
    unsigned (*spurious_vector)(void);

    /* The interrupt this CPU is handling, for a driver that needs the
     * controller's own id rather than the vector. */
    unsigned (*current_intid)(void);
    /* PPIs are banked per CPU: bound once, enabled on each CPU. */
    void (*bind_ppi)(unsigned intid, unsigned vector);
    void (*enable_local)(unsigned intid);
    void (*disable_local)(unsigned intid);

    void (*ipi_bind)(unsigned vector);
    void (*ipi_send)(unsigned cpu, unsigned vector);
    void (*ipi_broadcast_others)(unsigned vector);

    /* An interrupt has arrived: read the controller, set `frame->vector`,
     * run the handler, and acknowledge. */
    void (*dispatch)(struct arch_trap_frame *frame);

    /* Self-test aids (arch/testhooks.h): a line no device on this
     * machine uses, and a way to make it pending without a device, so a
     * test can prove where a routed interrupt lands. */
    int  (*test_spare_gsi)(void);
    void (*test_raise)(unsigned gsi);
    /* The next line the MSI allocator would hand out, if nothing is
     * bound to it: binding it is how a test provokes the overlap
     * firmware can create between wired interrupts and the MSI frame. */
    int  (*test_msi_overlap_gsi)(void);
};

extern const struct aarch64_irqc_ops aarch64_gicv2_ops;
extern const struct aarch64_irqc_ops aarch64_gicv3_ops;

#endif /* AARCH64_IRQC_H */
