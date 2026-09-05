/*
 * lapic.c - Local APIC driver, xAPIC (MMIO) mode.
 *
 * x2APIC is detected by cpu.c but not enabled: every access goes through
 * lapic_read/lapic_write below, so switching to MSR access later is
 * confined to those two functions.
 */

#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/vmm.h>

#include <arch/cpu.h>
#include <arch/irq.h>

#include <x86/cpu.h>
#include <x86/lapic.h>

#define REG_ID          0x020
#define REG_VERSION     0x030
#define REG_TPR         0x080
#define REG_EOI         0x0B0
#define REG_SVR         0x0F0
#define REG_ESR         0x280
#define REG_ICR_LO      0x300
#define REG_ICR_HI      0x310
#define REG_LVT_TIMER   0x320
#define REG_LVT_THERMAL 0x330
#define REG_LVT_PMC     0x340
#define REG_LVT_LINT0   0x350
#define REG_LVT_LINT1   0x360
#define REG_LVT_ERROR   0x370
#define REG_TIMER_ICR   0x380
#define REG_TIMER_CCR   0x390
#define REG_TIMER_DCR   0x3E0

#define SVR_ENABLE       (1u << 8)
#define LVT_MASKED       (1u << 16)
#define LVT_TIMER_PERIODIC (1u << 17)
#define DCR_DIVIDE_16    0x3u

#define ICR_DELIVERY_FIXED 0x0u
#define ICR_DELIVERY_INIT  (5u << 8)
#define ICR_DELIVERY_SIPI  (6u << 8)
#define ICR_LEVEL_ASSERT   (1u << 14)
#define ICR_TRIGGER_LEVEL  (1u << 15)
#define ICR_PENDING        (1u << 12)
#define ICR_DEST_ALL_OTHERS (3u << 18)

#define MSR_APIC_BASE_ENABLE (1ULL << 11)

static volatile uint32_t *g_lapic;

static inline uint32_t lapic_read(unsigned reg)
{
    return g_lapic[reg / 4];
}

static inline void lapic_write(unsigned reg, uint32_t v)
{
    g_lapic[reg / 4] = v;
}

static void lapic_enable_local(void)
{
    /* Make sure the APIC is globally enabled at the MSR level. */
    uint64_t base = rdmsr(MSR_APIC_BASE);
    if ((base & MSR_APIC_BASE_ENABLE) == 0)
        wrmsr(MSR_APIC_BASE, base | MSR_APIC_BASE_ENABLE);

    lapic_write(REG_TPR, 0);
    lapic_write(REG_LVT_TIMER, LVT_MASKED);
    lapic_write(REG_LVT_LINT0, LVT_MASKED);
    lapic_write(REG_LVT_LINT1, LVT_MASKED);
    lapic_write(REG_LVT_ERROR, LVT_MASKED);
    lapic_write(REG_LVT_PMC, LVT_MASKED);
    lapic_write(REG_LVT_THERMAL, LVT_MASKED);
    lapic_write(REG_ESR, 0);
    lapic_write(REG_ESR, 0);
    lapic_write(REG_SVR, SVR_ENABLE | LAPIC_SPURIOUS_VECTOR);
    lapic_write(REG_TIMER_DCR, DCR_DIVIDE_16);
}

void lapic_init_bsp(paddr_t base)
{
    KASSERT(g_lapic == NULL);
    vaddr_t va = vm_map_phys(base, PAGE_SIZE, VM_PROT_RW, VM_CACHE_UC);
    if (va == 0)
        panic("lapic: cannot map registers at 0x%llx", (unsigned long long)base);
    g_lapic = (volatile uint32_t *)va;

    lapic_enable_local();
    kdebug("lapic: id %u version 0x%x at 0x%llx", lapic_id(), lapic_read(REG_VERSION),
           (unsigned long long)base);
}

void lapic_init_cpu(void)
{
    KASSERT(g_lapic != NULL);
    lapic_enable_local();
}

bool lapic_present(void)
{
    return g_lapic != NULL;
}

uint32_t lapic_id(void)
{
    return lapic_read(REG_ID) >> 24;
}

void lapic_eoi(void)
{
    lapic_write(REG_EOI, 0);
}

void lapic_timer_oneshot_raw(uint32_t count)
{
    lapic_write(REG_TIMER_DCR, DCR_DIVIDE_16);
    lapic_write(REG_LVT_TIMER, LVT_MASKED); /* one-shot, no interrupt */
    lapic_write(REG_TIMER_ICR, count);
}

uint32_t lapic_timer_current(void)
{
    return lapic_read(REG_TIMER_CCR);
}

void lapic_timer_periodic(unsigned vector, uint32_t count)
{
    lapic_write(REG_TIMER_DCR, DCR_DIVIDE_16);
    lapic_write(REG_LVT_TIMER, LVT_TIMER_PERIODIC | (vector & 0xFF));
    lapic_write(REG_TIMER_ICR, count);
}

void lapic_timer_stop(void)
{
    lapic_write(REG_TIMER_ICR, 0);
    lapic_write(REG_LVT_TIMER, LVT_MASKED);
}

static void icr_wait(void)
{
    while (lapic_read(REG_ICR_LO) & ICR_PENDING)
        arch_cpu_relax();
}

/*
 * The xAPIC ICR is two registers and the write to ICR_LO is what sends,
 * with whatever ICR_HI holds at that moment. An interrupt handler on this
 * CPU that sends its own IPI (a tick waking a thread on another CPU does
 * exactly that) between the two writes would redirect ours to its
 * destination. The pair is therefore one critical section with local
 * interrupts off; nothing else in the kernel writes the ICR. An NMI
 * handler must never send an IPI for the same reason (it cannot be
 * masked); see docs/kernel/arch/invariants.md I-ARCH-15. The trailing
 * wait for delivery-idle needs no protection: any later sender waits for
 * idle itself before writing. x2APIC would make this a single MSR write.
 */
static void icr_write_pair(uint32_t hi, uint32_t lo)
{
    arch_irq_state_t s = arch_irq_save();
    icr_wait();
    lapic_write(REG_ICR_HI, hi);
    lapic_write(REG_ICR_LO, lo);
    arch_irq_restore(s);
    icr_wait();
}

static void icr_send(uint32_t apic_id, uint32_t lo)
{
    icr_write_pair(apic_id << 24, lo);
}

void lapic_send_ipi(uint32_t apic_id, unsigned vector)
{
    icr_send(apic_id, ICR_DELIVERY_FIXED | (vector & 0xFF));
}

void lapic_send_ipi_all_others(unsigned vector)
{
    /* Shorthand ignores ICR_HI, but the register pair is still written as
     * one unit so a nested sender cannot interleave with this one. */
    icr_write_pair(0, ICR_DEST_ALL_OTHERS | ICR_DELIVERY_FIXED | (vector & 0xFF));
}

void lapic_send_init(uint32_t apic_id)
{
    icr_send(apic_id, ICR_DELIVERY_INIT | ICR_LEVEL_ASSERT | ICR_TRIGGER_LEVEL);
    icr_send(apic_id, ICR_DELIVERY_INIT | ICR_TRIGGER_LEVEL); /* deassert */
}

void lapic_send_sipi(uint32_t apic_id, uint8_t start_page)
{
    icr_send(apic_id, ICR_DELIVERY_SIPI | start_page);
}
