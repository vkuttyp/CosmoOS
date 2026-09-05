/*
 * timer.c - The generic timer: clock and tick, plus the test periodic IRQ
 * (docs/kernel/arch/aarch64/design.md, "Timer").
 *
 * Clock: CNTPCT_EL0 at CNTFRQ_EL0 Hz. Tick: the EL1 physical timer, a
 * one-shot compare re-armed from the acknowledge hook gic.c calls before
 * dispatching its PPI. The test hook uses the EL1 virtual timer the same
 * way. Interrupt ids come from the GTDT, defaults from the virt machine.
 */

#include <kernel/acpi.h>
#include <kernel/errno.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/string.h>
#include <kernel/percpu.h>
#include <kernel/vmm.h>
#include <arch/cpu.h>
#include <arch/irqc.h>
#include <arch/testhooks.h>
#include <arch/timer.h>
#include <aarch64/platform.h>
#include <aarch64/sysreg.h>

void gic_bind_ppi(unsigned intid, unsigned vector);
void gic_enable_local(unsigned intid);
void gic_disable_local(unsigned intid);
void aarch64_timer_ack(unsigned intid);

static uint64_t g_hz;
static int g_tick_vector = -1;
static uint64_t g_tick_period;          /* counter ticks per tick */
static unsigned g_phys_intid = VIRT_TIMER_EL1_PHYS_INTID;
static unsigned g_virt_intid = VIRT_TIMER_EL1_VIRT_INTID;
static uint64_t g_test_period;
static uint64_t g_next_cval[CONFIG_MAX_CPUS];   /* the tick's next absolute compare value, per CPU */

/* GTDT (ACPI 5.1+): non-secure EL1 timer GSIV at 80, virtual timer GSIV at 88. */
static void read_gtdt(void)
{
    const struct acpi_sdt_header *gtdt = acpi_find_table("GTDT");
    if (gtdt == NULL || gtdt->length < 96) {
        kwarn("timer: no GTDT; using the virt PPIs %u (physical) and %u (virtual)", g_phys_intid, g_virt_intid);
        return;
    }
    const uint8_t *b = (const uint8_t *)gtdt;
    uint32_t phys, virt;
    memcpy(&phys, b + 80, 4);
    memcpy(&virt, b + 88, 4);
    if (phys >= GIC_PPI_BASE && phys < GIC_SPI_BASE)
        g_phys_intid = phys;
    if (virt >= GIC_PPI_BASE && virt < GIC_SPI_BASE)
        g_virt_intid = virt;
}

void arch_timer_calibrate(void)
{
    g_hz = READ_SYSREG(cntfrq_el0);
    if (g_hz < 1000000ull || g_hz > 10000000000ull)
        panic("timer: implausible generic timer frequency %llu Hz", (unsigned long long)g_hz);
    read_gtdt();
    int v = arch_vector_alloc();
    if (v < 0)
        panic("timer: no vector for the tick");
    g_tick_vector = v;
    gic_bind_ppi(g_phys_intid, (unsigned)v);
    kdebug("timer: generic timer %llu Hz, tick PPI %u, test PPI %u", (unsigned long long)g_hz, g_phys_intid,
           g_virt_intid);
}

/* Absolute compares keep the average rate exact: each tick is scheduled at
 * the previous one plus the period, skipping ahead only if that is already
 * in the past (a long interrupts-off stretch). */
static void arm_phys(void)
{
    unsigned cpu = arch_cpu_id();
    uint64_t now = READ_SYSREG(cntpct_el0);
    uint64_t next = g_next_cval[cpu] ? g_next_cval[cpu] + g_tick_period : now + g_tick_period;
    if (next <= now)
        next = now + g_tick_period;
    g_next_cval[cpu] = next;
    WRITE_SYSREG(cntp_cval_el0, next);
    WRITE_SYSREG(cntp_ctl_el0, CNT_CTL_ENABLE);
    isb();
}

void arch_timer_start_tick(unsigned hz)
{
    KASSERT(g_tick_vector >= 0 && g_hz > 0 && hz > 0);
    g_tick_period = g_hz / hz;
    if (g_tick_period == 0)
        g_tick_period = 1;
    arm_phys();
    gic_enable_local(g_phys_intid);
}

void arch_timer_stop_tick(void)
{
    WRITE_SYSREG(cntp_ctl_el0, CNT_CTL_IMASK);
    isb();
    gic_disable_local(g_phys_intid);
    g_next_cval[arch_cpu_id()] = 0;
}

unsigned arch_timer_vector(void)
{
    KASSERT(g_tick_vector >= 0);
    return (unsigned)g_tick_vector;
}

uint64_t arch_clock_read(void)
{
    isb();
    return READ_SYSREG(cntpct_el0);
}

uint64_t arch_clock_hz(void)
{
    return g_hz;
}

const char *arch_clock_name(void)
{
    return "arch-timer";
}

/* Called by gic.c before dispatching a PPI: re-arm the one-shot compare. */
void aarch64_timer_ack(unsigned intid)
{
    if (intid == g_phys_intid && g_tick_period)
        arm_phys();
    else if (intid == g_virt_intid && g_test_period) {
        WRITE_SYSREG(cntv_tval_el0, g_test_period);
        WRITE_SYSREG(cntv_ctl_el0, CNT_CTL_ENABLE);
        isb();
    }
}

void aarch64_timer_init_cpu(void)
{
    gic_enable_local(g_phys_intid);
}

/* arch/testhooks.h: a periodic interrupt for the interrupt tests, on the virtual timer. */
int arch_test_periodic_irq_start(unsigned hz)
{
    if (hz == 0)
        hz = 100;
    g_test_period = g_hz / hz;
    if (g_test_period == 0)
        g_test_period = 1;
    WRITE_SYSREG(cntv_tval_el0, g_test_period);
    WRITE_SYSREG(cntv_ctl_el0, CNT_CTL_ENABLE);
    isb();
    return (int)g_virt_intid;
}

void arch_test_periodic_irq_stop(void)
{
    WRITE_SYSREG(cntv_ctl_el0, CNT_CTL_IMASK);
    isb();
    g_test_period = 0;
}

/* --- the PL031 real-time clock of the virt machine ------------------------- */

bool arch_rtc_read_epoch(uint64_t *seconds)
{
    /* QEMU's virt describes the PL031 in its DSDT (ARMH0011), which this
     * kernel does not interpret; the machine's memory map is fixed, so the
     * register is read at its known address and validated: a machine
     * without the device reads zero (or faults on nothing: the range is
     * device memory on every virt variant). */
    vaddr_t va = vm_map_phys((paddr_t)VIRT_PL031_BASE, PAGE_SIZE, VM_PROT_RW, VM_CACHE_UC);
    if (va == 0)
        return false;
    uint32_t dr = *(volatile uint32_t *)va;        /* RTCDR */
    uint32_t pid0 = *(volatile uint32_t *)(va + 0xfe0);   /* PeriphID0: 0x31 for a PL031 */
    vm_unmap_phys(va);
    if ((pid0 & 0xff) != 0x31 || dr == 0)
        return false;
    *seconds = dr;
    return true;
}
