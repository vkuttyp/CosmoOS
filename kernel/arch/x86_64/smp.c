/*
 * smp.c - Starting application processors on x86-64.
 *
 * The boot CPU copies the trampoline to low memory, identity-maps that
 * page, patches the data block for the target CPU, and sends
 * INIT-SIPI-SIPI. The AP arrives in x86_ap_entry() on its bootstrap
 * stack and finishes its own initialisation.
 */

#include <kernel/errno.h>
#include <kernel/log.h>
#include <kernel/page.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/timer.h>
#include <kernel/vmm.h>

#include <arch/irqc.h>
#include <arch/percpu.h>
#include <arch/smp.h>
#include <arch/user.h>

#include <x86/cpu.h>
#include <x86/gdt.h>
#include <x86/idt.h>
#include <x86/lapic.h>

#define TRAMPOLINE_BASE 0x8000ULL
#define SIPI_VECTOR     (TRAMPOLINE_BASE >> 12)

extern const uint8_t x86_trampoline_start[], x86_trampoline_end[];
extern const uint32_t x86_trampoline_off_cr3, x86_trampoline_off_entry;
extern const uint32_t x86_trampoline_off_stack, x86_trampoline_off_cpu;

static volatile uint32_t g_ap_started[CONFIG_MAX_CPUS];
static bool g_trampoline_mapped;
static bool g_ap_stranded;

/*
 * Entry for an AP that answers a SIPI only after its start timed out.
 * The trampoline data block is repointed here on timeout and the page is
 * never unmapped afterwards, so a late arrival halts instead of running
 * with resources meant for someone else.
 */
static void x86_ap_halt_entry(unsigned cpu) __noreturn;
static void x86_ap_halt_entry(unsigned cpu)
{
    (void)cpu;
    for (;;)
        __asm__ volatile("cli; hlt" ::: "memory");
}

uint32_t arch_smp_boot_hw_id(void)
{
    return x86_cpu_apic_id(0);
}

int arch_smp_prepare_cpu(unsigned cpu)
{
    return gdt_alloc_cpu(cpu);
}

static void trampoline_install(void)
{
    if (g_trampoline_mapped)
        return;

    size_t len = (size_t)(x86_trampoline_end - x86_trampoline_start);
    KASSERT(len <= PAGE_SIZE);

    /* The page is reserved low memory, RAM, covered by the direct map;
     * copy through it, then identity-map it RX for the AP's first
     * instructions after paging is enabled. */
    memcpy(phys_to_virt(TRAMPOLINE_BASE), x86_trampoline_start, len);

    arch_irq_state_t s = spin_lock_irqsave(&kernel_space.lock);
    int rc = arch_mmu_map(&kernel_space.mmu, (vaddr_t)TRAMPOLINE_BASE, (paddr_t)TRAMPOLINE_BASE, PAGE_SIZE,
                          VM_PROT_RX, VM_CACHE_WB, 0);
    spin_unlock_irqrestore(&kernel_space.lock, s);
    if (rc)
        panic("smp: cannot identity-map the trampoline page (%d)", rc);
    g_trampoline_mapped = true;
}

static void trampoline_patch(unsigned cpu, uintptr_t stack_top)
{
    uint8_t *blob = phys_to_virt(TRAMPOLINE_BASE);
    uint64_t cr3 = kernel_space.mmu.root;
    uint64_t entry = (uint64_t)(uintptr_t)x86_ap_entry;
    uint64_t stack = stack_top;
    uint64_t idx = cpu;

    KASSERT(cr3 < (1ULL << 32));
    memcpy(blob + x86_trampoline_off_cr3, &cr3, 8);
    memcpy(blob + x86_trampoline_off_entry, &entry, 8);
    memcpy(blob + x86_trampoline_off_stack, &stack, 8);
    memcpy(blob + x86_trampoline_off_cpu, &idx, 8);
    barrier();
}

int arch_smp_start_cpu(unsigned cpu, uint32_t hw_id, uintptr_t stack_top)
{
    KASSERT(cpu > 0 && cpu < CONFIG_MAX_CPUS);

    trampoline_install();
    trampoline_patch(cpu, stack_top);
    x86_cpu_set_apic_id(cpu, hw_id);
    __atomic_store_n(&g_ap_started[cpu], 0u, __ATOMIC_RELEASE);

    lapic_send_init(hw_id);
    udelay(10000);
    lapic_send_sipi(hw_id, (uint8_t)SIPI_VECTOR);
    udelay(200);
    if (!__atomic_load_n(&g_ap_started[cpu], __ATOMIC_ACQUIRE)) {
        lapic_send_sipi(hw_id, (uint8_t)SIPI_VECTOR);
        udelay(200);
    }

    for (unsigned i = 0; i < 1000; i++) {
        if (__atomic_load_n(&g_ap_started[cpu], __ATOMIC_ACQUIRE))
            return 0;
        udelay(100);
    }

    /* The SIPI may still be honoured later. Make sure that whenever it
     * is, the AP finds a halt entry rather than a stack and CPU index
     * meant for a later start, and keep the page mapped for it. */
    uint64_t halt = (uint64_t)(uintptr_t)x86_ap_halt_entry;
    memcpy((uint8_t *)phys_to_virt(TRAMPOLINE_BASE) + x86_trampoline_off_entry, &halt, 8);
    barrier();
    g_ap_stranded = true;
    return -ETIMEDOUT;
}

void arch_smp_finish(void)
{
    if (!g_trampoline_mapped)
        return;
    if (g_ap_stranded) {
        kwarn("smp: a CPU never answered its SIPI; trampoline page left mapped with a halt entry");
        return;
    }

    arch_irq_state_t s = spin_lock_irqsave(&kernel_space.lock);
    int rc = arch_mmu_unmap(&kernel_space.mmu, (vaddr_t)TRAMPOLINE_BASE, PAGE_SIZE);
    spin_unlock_irqrestore(&kernel_space.lock, s);
    KASSERT(rc == 0);
    arch_mmu_shootdown(&kernel_space.mmu, (vaddr_t)TRAMPOLINE_BASE, PAGE_SIZE);
    g_trampoline_mapped = false;
}

void x86_ap_entry(unsigned cpu)
{
    __atomic_store_n(&g_ap_started[cpu], 1u, __ATOMIC_RELEASE);

    gdt_init_cpu(cpu);
    idt_load();

    struct percpu *pc = percpu_get(cpu);
    KASSERT(pc != NULL);
    arch_percpu_install(pc);   /* after gdt_init_cpu: GS base was reset */

    x86_cpu_enable_features();
    arch_syscall_init_cpu();
    arch_irqc_init_cpu();
    pc->hw_id = lapic_id();
    timer_init_cpu();

    kdebug("smp: CPU %u (APIC %u) up", cpu, pc->hw_id);
    sched_start_cpu();
}
