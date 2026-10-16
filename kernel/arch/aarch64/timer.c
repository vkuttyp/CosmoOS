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
#include <kernel/timer.h>
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
static uint64_t g_next_cval[CONFIG_MAX_CPUS];   /* the tick's next absolute compare value, per CPU */

/*
 * GTDT (ACPI 5.0+): after the 36-byte header, CntControlBase (8) and a
 * reserved word, the secure EL1 timer's GSIV and flags at 48, the
 * non-secure EL1 timer's at 56, the virtual timer's at 64, the EL2
 * timer's at 72. Until v1.0.1 this read 80 and 88 -- CntReadBase and the
 * platform timer count -- which never passed the PPI range check, so
 * every boot took the virt defaults below and was right only because they
 * are QEMU's numbers too. The defaults now apply only on QEMU's firmware.
 */
bool aarch64_timer_gtdt(unsigned *phys_out, unsigned *virt_out)
{
    const struct acpi_sdt_header *gtdt = acpi_find_table("GTDT");
    uint32_t phys = 0, virt = 0;
    if (gtdt != NULL && gtdt->length >= 72) {
        const uint8_t *b = (const uint8_t *)gtdt;
        memcpy(&phys, b + 56, 4);
        memcpy(&virt, b + 64, 4);
    }
    *phys_out = phys;
    *virt_out = virt;
    return phys >= GIC_PPI_BASE && phys < GIC_SPI_BASE && virt >= GIC_PPI_BASE && virt < GIC_SPI_BASE;
}

static void read_gtdt(void)
{
    const struct acpi_sdt_header *gtdt = acpi_find_table("GTDT");
    unsigned phys, virt;
    if (aarch64_timer_gtdt(&phys, &virt)) {
        g_phys_intid = phys;
        g_virt_intid = virt;
        return;
    }
    if (!acpi_platform_is_qemu())
        panic("timer: the GTDT gives no usable timer interrupts (EL1 physical %u, virtual %u%s), and this "
              "firmware is not QEMU's", phys, virt, gtdt == NULL ? "; no GTDT at all" : "");
    kwarn("timer: GTDT %s; QEMU firmware: using the virt PPIs %u (physical) and %u (virtual)",
          gtdt == NULL ? "absent" : "unusable", g_phys_intid, g_virt_intid);
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

/*
 * `cntpct_el0` is the system counter: the architecture requires one
 * counter for the whole system, not one per PE (Arm ARM D11.1.2), so
 * readings from two PEs are comparable by construction. The
 * clock-scope-aarch64 selftest is what keeps this a measurement rather
 * than a quotation.
 */
bool arch_clock_is_percpu(void)
{
    return false;   /* one system counter, not one per PE */
}

bool arch_clock_is_common(const char **why)
{
    (void)why;
    return true;
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
}

unsigned aarch64_timer_virt_intid(void)
{
    return g_virt_intid;
}

void aarch64_timer_init_cpu(void)
{
    gic_enable_local(g_phys_intid);
}

/*
 * arch/testhooks.h: a periodic interrupt for the interrupt tests.
 *
 * This used to be the virtual timer, which was the one hardware source
 * to hand and was nobody else's -- until guests. A guest's timer *is*
 * CNTV now, and the hypervisor owns its PPI, so the hook raises a line
 * of its own instead: the distributor's spare SPI, made pending from a
 * kernel timer at the requested rate. The interrupt the test requests,
 * enables, counts, masks and releases is as real as before; only what
 * asserts it has moved from a compare register to a callback.
 */
static struct timer g_test_timer;
static uint64_t g_test_ns;
static int g_test_gsi = -1;

static void test_tick(struct timer *t, void *arg)
{
    (void)arg;
    if (g_test_gsi >= 0) {
        arch_test_irq_raise((unsigned)g_test_gsi);
        timer_start(t, g_test_ns);
    }
}

int arch_test_periodic_irq_start(unsigned hz)
{
    int gsi = arch_test_irq_spare_gsi();
    if (gsi < 0)
        return -1;
    if (hz == 0)
        hz = 100;
    g_test_ns = NS_PER_SEC / hz;
    if (g_test_ns < TICK_NS)
        g_test_ns = TICK_NS;   /* a kernel timer fires at tick granularity, no faster */
    g_test_gsi = gsi;
    timer_setup(&g_test_timer, test_tick, NULL);
    timer_start(&g_test_timer, g_test_ns);
    return gsi;
}

void arch_test_periodic_irq_stop(void)
{
    g_test_gsi = -1;
    timer_cancel_sync(&g_test_timer);
}

/* --- the PL031 real-time clock of the virt machine ------------------------- */

/* One register read or write each: a PL031 access needs no lock. */
static vaddr_t pl031_map(void)
{
    if (!acpi_platform_is_qemu())
        return 0;   /* the fixed address is QEMU virt's; elsewhere there is no RTC this kernel finds */
    vaddr_t va = vm_map_phys((paddr_t)VIRT_PL031_BASE, PAGE_SIZE, VM_PROT_RW, VM_CACHE_UC);
    if (va != 0 && (*(volatile uint32_t *)(va + 0xfe0) & 0xff) != 0x31) {   /* PeriphID0: 0x31 for a PL031 */
        vm_unmap_phys(va);
        return 0;
    }
    return va;
}

bool arch_rtc_present(void)
{
    vaddr_t va = pl031_map();
    if (va != 0)
        vm_unmap_phys(va);
    return va != 0;
}

/* RTCLR (offset 0x08), the load register: RTCDR counts on from what is
 * written. 32 bits of seconds: until 2106. */
int arch_rtc_write_epoch(uint64_t seconds)
{
    if (seconds > UINT32_MAX)
        return -EINVAL;
    vaddr_t va = pl031_map();
    if (va == 0)
        return -ENODEV;
    *(volatile uint32_t *)(va + 0x08) = (uint32_t)seconds;
    vm_unmap_phys(va);
    return 0;
}

bool arch_rtc_read_epoch(uint64_t *seconds)
{
    /* QEMU's virt describes the PL031 in its DSDT (ARMH0011), which this
     * kernel does not interpret; the machine's memory map is fixed, so the
     * register is read at its known address and validated by its
     * peripheral id (pl031_map; the range is device memory on every virt
     * variant). */
    vaddr_t va = pl031_map();
    if (va == 0)
        return false;
    uint32_t dr = *(volatile uint32_t *)va;        /* RTCDR */
    vm_unmap_phys(va);
    /* Zero is 1970-01-01, a time RTC_SET_TIME may store (found in
     * review): the device's presence is the peripheral id's. */
    *seconds = dr;
    return true;
}
