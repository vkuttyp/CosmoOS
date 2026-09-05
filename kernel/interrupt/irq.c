/*
 * irq.c - GSI to vector mapping and controller programming policy.
 */

#include <kernel/acpi.h>
#include <kernel/errno.h>
#include <kernel/irq.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/spinlock.h>

#include <arch/irqc.h>
#include <arch/trap.h>

struct irq_slot {
    int vector;         /* -1 = free */
    const char *name;
    unsigned flags;
    unsigned cpu;
    bool enabled;
};

static struct irq_slot g_irqs[IRQ_MAX];
static spinlock_t g_irq_lock = SPINLOCK_INIT("irq");
static bool g_initialized;

void irq_init(void)
{
    for (unsigned i = 0; i < IRQ_MAX; i++)
        g_irqs[i].vector = -1;
    arch_irqc_init();
    g_initialized = true;
    kinfo("irq: controllers up, %u GSIs", arch_irqc_gsi_count());
}

static unsigned arch_flags(unsigned flags)
{
    unsigned f = 0;
    if (flags & IRQ_TRIGGER_LEVEL)
        f |= ARCH_IRQ_TRIGGER_LEVEL;
    if (flags & IRQ_POLARITY_LOW)
        f |= ARCH_IRQ_POLARITY_LOW;
    return f;
}

int irq_request(irq_t irq, interrupt_handler_fn fn, void *arg, const char *name, unsigned flags,
                unsigned cpu)
{
    KASSERT(g_initialized);
    if (irq >= IRQ_MAX || fn == NULL)
        return -EINVAL;

    arch_irq_state_t s = spin_lock_irqsave(&g_irq_lock);

    struct irq_slot *slot = &g_irqs[irq];
    if (slot->vector >= 0) {
        spin_unlock_irqrestore(&g_irq_lock, s);
        return -EBUSY;
    }

    int vector = arch_vector_alloc();
    if (vector < 0) {
        spin_unlock_irqrestore(&g_irq_lock, s);
        return vector;
    }

    int rc = interrupt_register((unsigned)vector, fn, arg, name);
    if (rc == 0) {
        rc = arch_irqc_route(irq, (unsigned)vector, cpu, arch_flags(flags));
        if (rc)
            interrupt_unregister((unsigned)vector, fn);
    }
    if (rc) {
        arch_vector_free((unsigned)vector);
        spin_unlock_irqrestore(&g_irq_lock, s);
        return rc;
    }

    slot->vector = vector;
    slot->name = name;
    slot->flags = flags;
    slot->cpu = cpu;
    slot->enabled = false;

    spin_unlock_irqrestore(&g_irq_lock, s);
    kdebug("irq: GSI %u -> vector %d on CPU %u (%s)", irq, vector, cpu, name ? name : "?");
    return 0;
}

int irq_request_msi(interrupt_handler_fn fn, void *arg, const char *name, unsigned cpu,
                    struct irq_msi_msg *msg)
{
    KASSERT(g_initialized);
    if (fn == NULL || msg == NULL)
        return -EINVAL;

    arch_irq_state_t s = spin_lock_irqsave(&g_irq_lock);
    int vector = arch_vector_alloc();
    if (vector < 0) {
        spin_unlock_irqrestore(&g_irq_lock, s);
        return vector;
    }
    int rc = interrupt_register((unsigned)vector, fn, arg, name);
    if (rc == 0) {
        rc = arch_irqc_msi_compose((unsigned)vector, cpu, &msg->addr, &msg->data);
        if (rc)
            interrupt_unregister((unsigned)vector, fn);
    }
    if (rc) {
        arch_vector_free((unsigned)vector);
        spin_unlock_irqrestore(&g_irq_lock, s);
        return rc;
    }
    spin_unlock_irqrestore(&g_irq_lock, s);
    kdebug("irq: MSI vector %d on CPU %u (%s)", vector, cpu, name ? name : "?");
    return vector;
}

/*
 * Release order (docs/kernel/quiesce/design.md, "Interrupt handlers"):
 * unpublish the handler under the lock, drop the lock, wait one grace
 * period so a CPU still inside the handler has returned, then give the
 * vector back. The vector stays allocated across the wait, so no new
 * registrant can take it while the old handler may still be running.
 * Thread context only (the wait sleeps).
 */
int irq_release_msi(int vector)
{
    if (vector < 0 || (unsigned)vector >= arch_trap_vector_count())
        return -EINVAL;
    arch_irq_state_t s = spin_lock_irqsave(&g_irq_lock);
    interrupt_unregister_vector((unsigned)vector);
    spin_unlock_irqrestore(&g_irq_lock, s);

    synchronize_irq((unsigned)vector);

    s = spin_lock_irqsave(&g_irq_lock);
    arch_vector_free((unsigned)vector);
    spin_unlock_irqrestore(&g_irq_lock, s);
    return 0;
}

int irq_release(irq_t irq)
{
    if (irq >= IRQ_MAX)
        return -EINVAL;

    arch_irq_state_t s = spin_lock_irqsave(&g_irq_lock);
    struct irq_slot *slot = &g_irqs[irq];
    if (slot->vector < 0) {
        spin_unlock_irqrestore(&g_irq_lock, s);
        return -ENOENT;
    }

    arch_irqc_mask(irq);
    unsigned vector = (unsigned)slot->vector;
    interrupt_unregister_vector(vector);
    slot->vector = -1;
    slot->enabled = false;
    spin_unlock_irqrestore(&g_irq_lock, s);

    /* Masked and unpublished; a handler entered before the mask may
     * still be running on its CPU. */
    synchronize_irq(vector);

    s = spin_lock_irqsave(&g_irq_lock);
    arch_vector_free(vector);
    spin_unlock_irqrestore(&g_irq_lock, s);
    return 0;
}

int irq_enable(irq_t irq)
{
    if (irq >= IRQ_MAX || g_irqs[irq].vector < 0)
        return -EINVAL;
    arch_irq_state_t s = spin_lock_irqsave(&g_irq_lock);
    int rc = arch_irqc_unmask(irq);
    if (rc == 0)
        g_irqs[irq].enabled = true;
    spin_unlock_irqrestore(&g_irq_lock, s);
    return rc;
}

int irq_disable(irq_t irq)
{
    if (irq >= IRQ_MAX || g_irqs[irq].vector < 0)
        return -EINVAL;
    arch_irq_state_t s = spin_lock_irqsave(&g_irq_lock);
    int rc = arch_irqc_mask(irq);
    if (rc == 0)
        g_irqs[irq].enabled = false;
    spin_unlock_irqrestore(&g_irq_lock, s);
    return rc;
}

irq_t irq_legacy_to_gsi(unsigned isa_irq, unsigned *flags_out)
{
    const struct acpi_madt_override *ov;
    size_t n = acpi_madt_overrides(&ov);
    unsigned flags = IRQ_TRIGGER_EDGE;
    irq_t gsi = isa_irq;

    for (size_t i = 0; i < n; i++) {
        if (ov[i].bus != 0 || ov[i].source != isa_irq)
            continue;
        gsi = ov[i].gsi;
        unsigned polarity = ov[i].flags & 0x3;
        unsigned trigger = (ov[i].flags >> 2) & 0x3;
        if (polarity == 3)
            flags |= IRQ_POLARITY_LOW;
        if (trigger == 3)
            flags |= IRQ_TRIGGER_LEVEL;
        break;
    }
    if (flags_out)
        *flags_out = flags;
    return gsi;
}

int irq_vector_of(irq_t irq)
{
    return irq < IRQ_MAX ? g_irqs[irq].vector : -1;
}
