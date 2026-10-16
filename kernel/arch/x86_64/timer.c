/*
 * timer.c - TSC clock, LAPIC tick timer, PIT-based calibration.
 *
 * Calibration runs once on the boot CPU: PIT channel 2 counts a known
 * 10 ms while the TSC and the LAPIC timer (divide 16, free-running from
 * 0xFFFFFFFF) are sampled before and after. Both frequencies follow.
 */

#include <kernel/acpi.h>
#include <kernel/errno.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/timer.h>

#include <arch/cpu.h>
#include <arch/irqc.h>
#include <arch/timer.h>

#include <x86/cpu.h>
#include <x86/io.h>
#include <x86/lapic.h>

#define PIT_HZ         1193182u
#define PIT_CH2_DATA   0x42
#define PIT_CMD        0x43
#define PIT_GATE       0x61
#define CALIB_MS       10u
#define CALIB_PIT_TICKS (PIT_HZ * CALIB_MS / 1000u)

static uint64_t g_tsc_hz;
static uint64_t g_lapic_hz;   /* LAPIC timer input after divide-by-16 */
static int g_tick_vector = -1;

static inline uint64_t rdtsc_ordered(void)
{
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) : : "memory");
    return ((uint64_t)hi << 32) | lo;
}

void arch_timer_calibrate(void)
{
    /* Gate channel 2 off, speaker off. */
    uint8_t gate = inb(PIT_GATE);
    outb(PIT_GATE, (uint8_t)((gate & ~0x03u) | 0x01u)); /* gate on, speaker off */

    /* Channel 2, lobyte/hibyte, mode 0 (interrupt on terminal count). */
    outb(PIT_CMD, 0xB0);
    outb(PIT_CH2_DATA, (uint8_t)(CALIB_PIT_TICKS & 0xFF));
    outb(PIT_CH2_DATA, (uint8_t)(CALIB_PIT_TICKS >> 8));

    /* Retrigger the gate so the count actually starts. */
    gate = inb(PIT_GATE);
    outb(PIT_GATE, (uint8_t)(gate & ~0x01u));
    outb(PIT_GATE, (uint8_t)(gate | 0x01u));

    lapic_timer_oneshot_raw(0xFFFFFFFFu);
    uint64_t tsc0 = rdtsc_ordered();
    uint32_t lapic0 = lapic_timer_current();

    /* Wait for OUT2 (bit 5) to go high: terminal count reached. Bound the
     * wait so a broken PIT panics instead of hanging. */
    uint64_t spins = 0;
    while ((inb(PIT_GATE) & 0x20) == 0) {
        if (++spins > 200000000ULL)
            panic("timer: PIT calibration timed out");
        arch_cpu_relax();
    }

    uint64_t tsc1 = rdtsc_ordered();
    uint32_t lapic1 = lapic_timer_current();
    lapic_timer_stop();

    /* Restore gate state. */
    outb(PIT_GATE, (uint8_t)(inb(PIT_GATE) & ~0x03u));

    uint64_t tsc_delta = tsc1 - tsc0;
    uint64_t lapic_delta = (uint64_t)(lapic0 - lapic1);

    g_tsc_hz = tsc_delta * (1000u / CALIB_MS);
    g_lapic_hz = lapic_delta * (1000u / CALIB_MS);

    if (g_tsc_hz < 100000000ULL || g_tsc_hz > 10000000000ULL)
        panic("timer: implausible TSC frequency %llu Hz", (unsigned long long)g_tsc_hz);
    if (g_lapic_hz < 1000000ULL || g_lapic_hz > 10000000000ULL)
        panic("timer: implausible LAPIC timer frequency %llu Hz", (unsigned long long)g_lapic_hz);

    kdebug("timer: calibrated over %u ms: TSC %llu Hz, LAPIC timer %llu Hz (/16)", CALIB_MS,
           (unsigned long long)g_tsc_hz, (unsigned long long)g_lapic_hz);

    int v = arch_vector_alloc();
    if (v < 0)
        panic("timer: no vector for the tick");
    g_tick_vector = v;
}

void arch_timer_start_tick(unsigned hz)
{
    KASSERT(g_tick_vector >= 0 && g_lapic_hz > 0);
    uint64_t count = g_lapic_hz / hz;
    if (count == 0 || count > 0xFFFFFFFFULL)
        panic("timer: tick count %llu out of range", (unsigned long long)count);
    lapic_timer_periodic((unsigned)g_tick_vector, (uint32_t)count);
}

void arch_timer_stop_tick(void)
{
    lapic_timer_stop();
}

unsigned arch_timer_vector(void)
{
    KASSERT(g_tick_vector >= 0);
    return (unsigned)g_tick_vector;
}

uint64_t arch_clock_read(void)
{
    return rdtsc_ordered();
}

uint64_t arch_clock_hz(void)
{
    return g_tsc_hz;
}

const char *arch_clock_name(void)
{
    return "tsc";
}

/*
 * The invariant-TSC bit (CPUID 0x80000007 EDX[8]), detected at boot in
 * cpu.c since this kernel had an x86 port and, until now, read by
 * nothing.
 *
 * Without it the TSC counts core clocks: it changes rate with frequency
 * scaling and stops in the deeper C-states, so two CPUs' readings drift
 * apart by an amount nothing bounds. There is nowhere to fall back to --
 * this port calibrates against PIT channel 2 as a one-shot gate, which
 * is not a free-running counter, and the tree has no HPET driver -- so
 * the kernel keeps using the TSC and stops promising that two CPUs'
 * readings may be subtracted. Saying so is the useful thing; pretending
 * otherwise is what this unit exists to stop.
 */
bool arch_clock_is_percpu(void)
{
    return true;   /* each core has its own TSC */
}

bool arch_clock_is_common(const char **why)
{
    if (!x86_cpu_info()->has_invariant_tsc) {
        *why = "the TSC is not invariant (CPUID 0x80000007 EDX[8] clear): it varies with core frequency and halts in deep C-states";
        return false;
    }
    return true;
}

/* --- the CMOS real-time clock (MC146818): seconds since 1970 ------------ */

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static uint8_t cmos_read(uint8_t reg)
{
    outb(CMOS_ADDR, (uint8_t)(0x80 | reg));   /* NMI disabled bit kept set, as firmware left it */
    return inb(CMOS_DATA);
}

static unsigned from_bcd(uint8_t v)
{
    return (unsigned)((v >> 4) * 10 + (v & 0x0f));
}

static uint8_t to_bcd(unsigned v)
{
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

static void cmos_write(uint8_t reg, uint8_t v)
{
    outb(CMOS_ADDR, (uint8_t)(0x80 | reg));
    outb(CMOS_DATA, v);
}

/* The FADT's CENTURY (offset 108): the CMOS index of the century register,
 * 0 for none (ACPI); QEMU's q35 names 0x32. With no FADT, none. A century
 * written to a register the firmware did not name could overwrite its
 * settings (found in review), so the write skips it as the read does. */
static uint8_t cmos_century_reg(void)
{
    const struct acpi_sdt_header *fadt = acpi_find_table("FACP");
    if (fadt == NULL || fadt->length <= 108)
        return 0;
    uint8_t r = ((const uint8_t *)fadt)[108];
    return r >= 0x0e && r < 0x80 ? r : 0;   /* past the clock's own registers, inside the bank */
}

/* IAPC_BOOT_ARCH (offset 109) bit 5, ACPI 5.0a's "CMOS RTC not present". */
bool arch_rtc_present(void)
{
    const struct acpi_sdt_header *fadt = acpi_find_table("FACP");
    uint16_t flags = 0;
    if (fadt != NULL && fadt->length >= 111)
        memcpy(&flags, (const uint8_t *)fadt + 109, 2);
    return !(flags & (1u << 5));
}

/* The index register makes every access two port writes: one CPU at a
 * time, from boot's read to hwclock's (shell completeness S5). */
static spinlock_t g_cmos_lock = SPINLOCK_INIT("cmos");

bool arch_rtc_read_epoch(uint64_t *seconds)
{
    const uint8_t century_reg = cmos_century_reg();
    arch_irq_state_t irq = spin_lock_irqsave(&g_cmos_lock);
    /* Wait for an update cycle to end (bit 7 of status A), then read twice
     * until the two reads agree. */
    for (unsigned spin = 0; spin < 100000 && (cmos_read(0x0a) & 0x80); spin++)
        arch_cpu_relax();
    uint8_t raw[7], again[7];
    const uint8_t regs[7] = { 0x00, 0x02, 0x04, 0x07, 0x08, 0x09, century_reg ? century_reg : 0x09 };
    for (unsigned tries = 0; tries < 8; tries++) {
        for (unsigned i = 0; i < 7; i++)
            raw[i] = cmos_read(regs[i]);
        for (unsigned i = 0; i < 7; i++)
            again[i] = cmos_read(regs[i]);
        if (memcmp(raw, again, sizeof(raw)) == 0)
            break;
    }
    uint8_t status_b = cmos_read(0x0b);
    spin_unlock_irqrestore(&g_cmos_lock, irq);
    bool binary = (status_b & 0x04) != 0, hour24 = (status_b & 0x02) != 0;
    unsigned sec = binary ? raw[0] : from_bcd(raw[0]);
    unsigned min = binary ? raw[1] : from_bcd(raw[1]);
    bool pm = (raw[2] & 0x80) != 0;
    unsigned hour = binary ? (raw[2] & 0x7f) : from_bcd(raw[2] & 0x7f);
    unsigned day = binary ? raw[3] : from_bcd(raw[3]);
    unsigned mon = binary ? raw[4] : from_bcd(raw[4]);
    unsigned year = binary ? raw[5] : from_bcd(raw[5]);
    unsigned century = century_reg == 0 ? 0 : binary ? raw[6] : from_bcd(raw[6]);
    if (!hour24) {
        hour %= 12;
        if (pm)
            hour += 12;
    }
    if (century < 19 || century > 22)
        century = year < 70 ? 20 : 19;   /* no century register: 1970..2069 */
    int64_t full_year = (int64_t)century * 100 + year;
    if (mon < 1 || mon > 12 || day < 1 || day > 31 || hour > 23 || min > 59 || sec > 59 || full_year < 1970)
        return false;
    *seconds = (uint64_t)time_days_from_civil(full_year, mon, day) * 86400ull + hour * 3600ull + min * 60ull + sec;
    return true;
}

/* The MC146818's SET bit (status B bit 7) holds the update cycle off while
 * the fields are written, in the encoding status B names, as the read
 * decodes them; the century goes where the FADT says, or nowhere. */
int arch_rtc_write_epoch(uint64_t seconds)
{
    int64_t y;
    unsigned mon, day;
    time_civil_from_days((int64_t)(seconds / 86400), &y, &mon, &day);
    unsigned rem = (unsigned)(seconds % 86400), hour = rem / 3600, min = rem / 60 % 60, sec = rem % 60;
    if (y < 1970 || y > 2299)
        return -EINVAL;
    const uint8_t century_reg = cmos_century_reg();
    arch_irq_state_t irq = spin_lock_irqsave(&g_cmos_lock);
    uint8_t status_b = cmos_read(0x0b);
    bool binary = (status_b & 0x04) != 0, hour24 = (status_b & 0x02) != 0;
    unsigned h = hour;
    bool pm = false;
    if (!hour24) {
        pm = hour >= 12;
        h = hour % 12 == 0 ? 12 : hour % 12;
    }
    const unsigned vals[7] = { sec, min, h, day, mon, (unsigned)(y % 100), (unsigned)(y / 100) };
    const uint8_t regs[7] = { 0x00, 0x02, 0x04, 0x07, 0x08, 0x09, century_reg };
    cmos_write(0x0b, (uint8_t)(status_b | 0x80));
    for (unsigned i = 0; i < (century_reg ? 7u : 6u); i++) {
        uint8_t v = binary ? (uint8_t)vals[i] : to_bcd(vals[i]);
        if (i == 2 && pm)
            v |= 0x80;
        cmos_write(regs[i], v);
    }
    cmos_write(0x0b, status_b);
    spin_unlock_irqrestore(&g_cmos_lock, irq);
    return 0;
}
