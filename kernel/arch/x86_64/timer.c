/*
 * timer.c - TSC clock, LAPIC tick timer, PIT-based calibration.
 *
 * Calibration runs once on the boot CPU: PIT channel 2 counts a known
 * 10 ms while the TSC and the LAPIC timer (divide 16, free-running from
 * 0xFFFFFFFF) are sampled before and after. Both frequencies follow.
 */

#include <kernel/errno.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/string.h>

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

/* Days since 1970-01-01 for a proleptic Gregorian date. */
static uint64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (uint64_t)(era * 146097 + (int64_t)doe - 719468);
}

bool arch_rtc_read_epoch(uint64_t *seconds)
{
    /* Wait for an update cycle to end (bit 7 of status A), then read twice
     * until the two reads agree. */
    for (unsigned spin = 0; spin < 100000 && (cmos_read(0x0a) & 0x80); spin++)
        arch_cpu_relax();
    uint8_t raw[7], again[7];
    static const uint8_t regs[7] = { 0x00, 0x02, 0x04, 0x07, 0x08, 0x09, 0x32 };
    for (unsigned tries = 0; tries < 8; tries++) {
        for (unsigned i = 0; i < 7; i++)
            raw[i] = cmos_read(regs[i]);
        for (unsigned i = 0; i < 7; i++)
            again[i] = cmos_read(regs[i]);
        if (memcmp(raw, again, sizeof(raw)) == 0)
            break;
    }
    uint8_t status_b = cmos_read(0x0b);
    bool binary = (status_b & 0x04) != 0, hour24 = (status_b & 0x02) != 0;
    unsigned sec = binary ? raw[0] : from_bcd(raw[0]);
    unsigned min = binary ? raw[1] : from_bcd(raw[1]);
    bool pm = (raw[2] & 0x80) != 0;
    unsigned hour = binary ? (raw[2] & 0x7f) : from_bcd(raw[2] & 0x7f);
    unsigned day = binary ? raw[3] : from_bcd(raw[3]);
    unsigned mon = binary ? raw[4] : from_bcd(raw[4]);
    unsigned year = binary ? raw[5] : from_bcd(raw[5]);
    unsigned century = binary ? raw[6] : from_bcd(raw[6]);
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
    *seconds = days_from_civil(full_year, mon, day) * 86400ull + hour * 3600ull + min * 60ull + sec;
    return true;
}
