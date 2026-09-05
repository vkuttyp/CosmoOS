/*
 * irqc.c - arch/irqc.h for x86-64: ties ACPI discovery, the local APIC,
 * the I/O APICs, and the dynamic vector allocator together.
 */

#include <kernel/acpi.h>
#include <kernel/errno.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/spinlock.h>

#include <arch/cpu.h>
#include <arch/irqc.h>

#include <x86/idt.h>
#include <x86/ioapic.h>
#include <x86/lapic.h>
#include <x86/pic.h>

#define VECTOR_DYNAMIC_FIRST 48u
#define VECTOR_DYNAMIC_LAST  238u

static uint64_t g_vector_used[4];
static spinlock_t g_vector_lock = SPINLOCK_INIT("vectors");
static uint32_t g_cpu_apic_id[CONFIG_MAX_CPUS];

int arch_vector_alloc(void)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_vector_lock);
    for (unsigned v = VECTOR_DYNAMIC_FIRST; v <= VECTOR_DYNAMIC_LAST; v++) {
        if ((g_vector_used[v / 64] & (1ULL << (v % 64))) == 0) {
            g_vector_used[v / 64] |= 1ULL << (v % 64);
            spin_unlock_irqrestore(&g_vector_lock, s);
            return (int)v;
        }
    }
    spin_unlock_irqrestore(&g_vector_lock, s);
    return -ENOSPC;
}

void arch_vector_free(unsigned vector)
{
    KASSERT(vector >= VECTOR_DYNAMIC_FIRST && vector <= VECTOR_DYNAMIC_LAST);
    arch_irq_state_t s = spin_lock_irqsave(&g_vector_lock);
    g_vector_used[vector / 64] &= ~(1ULL << (vector % 64));
    spin_unlock_irqrestore(&g_vector_lock, s);
}

void arch_irqc_init(void)
{
    paddr_t lapic_base = acpi_madt_lapic_base();
    if (lapic_base == 0)
        panic("irqc: ACPI reports no local APIC");

    lapic_init_bsp(lapic_base);
    g_cpu_apic_id[arch_cpu_id()] = lapic_id();

    const struct acpi_madt_ioapic *ioapics;
    size_t n = acpi_madt_ioapics(&ioapics);
    for (size_t i = 0; i < n; i++) {
        int rc = ioapic_add(ioapics[i].id, ioapics[i].address, ioapics[i].gsi_base);
        if (rc)
            kwarn("irqc: IOAPIC %u at 0x%llx not usable (%d)", ioapics[i].id,
                  (unsigned long long)ioapics[i].address, rc);
    }
    if (n == 0)
        kwarn("irqc: no IOAPIC; only local-APIC interrupts are available");
}

void arch_irqc_init_cpu(void)
{
    lapic_init_cpu();
    g_cpu_apic_id[arch_cpu_id()] = lapic_id();
}

uint32_t x86_cpu_apic_id(unsigned cpu)
{
    return cpu < CONFIG_MAX_CPUS ? g_cpu_apic_id[cpu] : 0;
}

void x86_cpu_set_apic_id(unsigned cpu, uint32_t apic_id)
{
    KASSERT(cpu < CONFIG_MAX_CPUS);
    g_cpu_apic_id[cpu] = apic_id;
}

int arch_irqc_route(unsigned gsi, unsigned vector, unsigned cpu, unsigned flags)
{
    if (cpu >= CONFIG_MAX_CPUS || percpu_get(cpu) == NULL)
        return -EINVAL;
    return ioapic_route(gsi, vector, g_cpu_apic_id[cpu], flags);
}

int arch_irqc_mask(unsigned gsi)
{
    return ioapic_mask(gsi);
}

int arch_irqc_unmask(unsigned gsi)
{
    return ioapic_unmask(gsi);
}

int arch_irqc_msi_compose(unsigned vector, unsigned cpu, uint64_t *addr, uint32_t *data)
{
    if (cpu >= CONFIG_MAX_CPUS || vector < VECTOR_DYNAMIC_FIRST || vector > VECTOR_DYNAMIC_LAST)
        return -EINVAL;
    uint32_t apic = x86_cpu_apic_id(cpu);
    if (apic > 0xff)
        return -EINVAL;   /* xAPIC physical destination only */
    /* Physical destination mode, fixed delivery, edge triggered. */
    *addr = 0xFEE00000ULL | ((uint64_t)apic << 12);
    *data = vector;
    return 0;
}

void arch_irqc_eoi(unsigned vector)
{
    if (vector < X86_EXCEPTION_COUNT)
        return;
    if (vector >= X86_VECTOR_IRQ_BASE && vector < X86_VECTOR_IRQ_BASE + X86_VECTOR_IRQ_COUNT) {
        pic_eoi(vector - X86_VECTOR_IRQ_BASE);
        return;
    }
    if (vector == LAPIC_SPURIOUS_VECTOR)
        return;
    if (lapic_present())
        lapic_eoi();
}

unsigned arch_irqc_gsi_count(void)
{
    return ioapic_gsi_count();
}

unsigned arch_irqc_spurious_vector(void)
{
    return LAPIC_SPURIOUS_VECTOR;
}

void arch_ipi_bind(unsigned vector)
{
    (void)vector;   /* the LAPIC delivers any vector directly */
}

void arch_ipi_send(unsigned cpu, unsigned vector)
{
    KASSERT(cpu < CONFIG_MAX_CPUS);
    lapic_send_ipi(g_cpu_apic_id[cpu], vector);
}

void arch_ipi_broadcast_others(unsigned vector)
{
    lapic_send_ipi_all_others(vector);
}
