/*
 * smp.c - Bringing up application processors.
 */

#include <kernel/acpi.h>
#include <kernel/ipi.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/smp.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/vmm.h>

#include <arch/cpu.h>
#include <arch/irq.h>
#include <arch/smp.h>

#define AP_ONLINE_TIMEOUT_MS 500

static bool wait_online(unsigned cpu)
{
    for (unsigned i = 0; i < AP_ONLINE_TIMEOUT_MS * 10; i++) {
        if (cpu_online(cpu))
            return true;
        udelay(100);
    }
    return false;
}

void smp_init(void)
{
    const struct acpi_madt_cpu *cpus;
    size_t n = acpi_madt_cpus(&cpus);
    uint32_t boot_id = arch_smp_boot_hw_id();
    unsigned next = 1;
    unsigned started = 0;

    KASSERT(arch_irq_enabled());
    this_cpu()->hw_id = boot_id;

    for (size_t i = 0; i < n; i++) {
        if (cpus[i].apic_id == boot_id)
            continue;
        if (next >= CONFIG_MAX_CPUS) {
            kwarn("smp: more than %u CPUs; APIC %u not started", CONFIG_MAX_CPUS, cpus[i].apic_id);
            continue;
        }

        unsigned cpu = next;
        struct percpu *pc = kmalloc(sizeof(*pc), KMEM_ZERO);
        if (pc == NULL) {
            kwarn("smp: out of memory for CPU %u", cpu);
            break;
        }
        vaddr_t stack = vm_kernel_alloc(THREAD_STACK_SIZE, VM_KALLOC_GUARD | VM_KALLOC_POPULATE, VM_PROT_RW);
        if (stack == 0) {
            kfree(pc);
            kwarn("smp: out of memory for CPU %u stack", cpu);
            break;
        }
        int rc = arch_smp_prepare_cpu(cpu);
        if (rc) {
            vm_kernel_free(stack);
            kfree(pc);
            kwarn("smp: cannot prepare CPU %u (%d)", cpu, rc);
            break;
        }

        pc->boot_stack = stack;
        pc->hw_id = cpus[i].apic_id;
        percpu_register(pc, cpu);
        next++;

        rc = arch_smp_start_cpu(cpu, cpus[i].apic_id, stack + THREAD_STACK_SIZE);
        if (rc) {
            kwarn("smp: CPU %u (APIC %u) did not start (%d)", cpu, cpus[i].apic_id, rc);
            continue;
        }
        if (!wait_online(cpu)) {
            kwarn("smp: CPU %u (APIC %u) started but never came online", cpu, cpus[i].apic_id);
            continue;
        }
        started++;
    }

    arch_smp_finish();
    kinfo("smp: %u CPUs online of %zu reported", started + 1, n);
}

void smp_stop_others(void)
{
    cpumask_t online = cpu_online_mask() & ~CPUMASK_OF(arch_cpu_id());
    if (online == 0)
        return;

    ipi_broadcast_others(IPI_HALT);

    /* Give them a moment to acknowledge by going offline; a CPU with
     * interrupts disabled may never answer, which is acceptable here. */
    for (unsigned i = 0; i < 100; i++) {
        if ((cpu_online_mask() & ~CPUMASK_OF(arch_cpu_id())) == 0)
            return;
        udelay(100);
    }
}
