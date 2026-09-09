/*
 * irqc.c - Choosing an interrupt controller, and the thin layer that
 * dispatches to it (kernel/arch/aarch64/include/aarch64/irqc.h).
 *
 * Everything above this file speaks `arch/irqc.h`: GSIs, vectors, CPU
 * indices. Everything below it is one controller generation's registers.
 * This file is the joint: it reads the MADT once, picks the driver that
 * can drive what the MADT described, and forwards each entry point to
 * it. There is no fallback between generations -- a GICv3 distributor
 * cannot be driven by the GICv2 code even in its compatibility mode
 * unless firmware left that mode enabled, and guessing wrong here means
 * an interrupt that never arrives rather than an error we can report.
 */

#include <kernel/acpi.h>
#include <kernel/errno.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <arch/irqc.h>
#include <aarch64/irqc.h>
#include <aarch64/platform.h>
#include <arch/testhooks.h>
#include <aarch64/trapframe.h>

/* Set once, by the boot CPU in arch_irqc_init, before any AP runs or any
 * interrupt is enabled; read without synchronisation afterwards. */
static const struct aarch64_irqc_ops *g_ops;

/* Every field of the ops table is a pointer, and a hole in one is a
 * null call at the worst possible moment -- an interrupt, on a machine
 * nobody tested. Check the lot once, at boot, so a driver added later
 * fails loudly here instead. */
static void ops_check(const struct aarch64_irqc_ops *o)
{
    const void *const *p = (const void *const *)o;
    for (size_t i = 0; i < sizeof(*o) / sizeof(*p); i++)
        if (p[i] == NULL)
            panic("gic: the %s ops table has a hole at slot %u", o->name, (unsigned)i);
}

void arch_irqc_init(void)
{
    struct acpi_gic gic;
    if (!acpi_madt_gic(&gic)) {
        kwarn("gic: MADT has no GIC entries; using the virt defaults");
        gic = (struct acpi_gic){ 0 };
        gic.gicd_base = VIRT_GICD_BASE;
        gic.gicc_base = VIRT_GICC_BASE;
        gic.v2m_base = VIRT_GICV2M_BASE;
    }

    switch (gic.version) {
    case 0:   /* older firmware leaves it unset; the virt defaults are v2 */
    case 1:
    case 2:
        g_ops = &aarch64_gicv2_ops;
        break;
    case 3:
    case 4:   /* a GICv4 distributor drives GICv3 interrupts identically */
        g_ops = &aarch64_gicv3_ops;
        break;
    default:
        panic("gic: distributor version %u is not a GIC this kernel drives", gic.version);
    }
    ops_check(g_ops);
    g_ops->init(&gic);
}

void arch_irqc_init_cpu(void)
{
    g_ops->init_cpu();
}

int arch_vector_alloc(void)
{
    return g_ops->vector_alloc();
}

void arch_vector_free(unsigned vector)
{
    g_ops->vector_free(vector);
}

int arch_irqc_route(unsigned gsi, unsigned vector, unsigned cpu, unsigned flags)
{
    return g_ops->route(gsi, vector, cpu, flags);
}

int arch_irqc_mask(unsigned gsi)
{
    return g_ops->mask(gsi);
}

int arch_irqc_unmask(unsigned gsi)
{
    return g_ops->unmask(gsi);
}

void arch_irqc_eoi(unsigned vector)
{
    if (g_ops)   /* an exception can be taken before the controller is up */
        g_ops->eoi(vector);
}

int arch_irqc_msi_compose(unsigned vector, unsigned cpu, uint32_t devid, uint64_t *addr,
                          uint32_t *data)
{
    return g_ops->msi_compose(vector, cpu, devid, addr, data);
}

bool arch_irqc_msi_doorbell(paddr_t *pa, size_t *len)
{
    return g_ops ? g_ops->msi_doorbell(pa, len) : false;
}

unsigned arch_irqc_gsi_count(void)
{
    return g_ops ? g_ops->gsi_count() : 0;
}

unsigned arch_irqc_spurious_vector(void)
{
    return VEC_SPURIOUS;   /* the vector map is the architecture's, not a driver's */
}

unsigned gic_current_intid(void)
{
    return g_ops->current_intid();
}

void gic_bind_ppi(unsigned intid, unsigned vector)
{
    g_ops->bind_ppi(intid, vector);
}

void gic_enable_local(unsigned intid)
{
    g_ops->enable_local(intid);
}

void gic_disable_local(unsigned intid)
{
    g_ops->disable_local(intid);
}

void arch_ipi_bind(unsigned vector)
{
    g_ops->ipi_bind(vector);
}

void arch_ipi_send(unsigned cpu, unsigned vector)
{
    g_ops->ipi_send(cpu, vector);
}

void arch_ipi_broadcast_others(unsigned vector)
{
    g_ops->ipi_broadcast_others(vector);
}

void gic_irq_dispatch(struct arch_trap_frame *frame)
{
    g_ops->dispatch(frame);
}

int arch_test_irq_spare_gsi(void)
{
    return g_ops ? g_ops->test_spare_gsi() : -1;
}

void arch_test_irq_raise(unsigned gsi)
{
    g_ops->test_raise(gsi);
}

int arch_test_msi_overlap_gsi(void)
{
    return g_ops ? g_ops->test_msi_overlap_gsi() : -1;
}

bool arch_test_msi_per_device(void)
{
    return g_ops ? g_ops->test_msi_per_device() : false;
}
