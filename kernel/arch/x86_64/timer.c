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
