/*
 * pit.c - 8254 programmable interval timer as a test interrupt source.
 *
 * The PIT is not the system tick (the LAPIC timer is). Channel 0 is used
 * here only to exercise the IOAPIC routing path in the self-tests: ISA
 * IRQ 0 in rate-generator mode. Channel 2 is the calibration reference
 * in timer.c.
 */

#include <arch/testhooks.h>

#include <x86/io.h>

#define PIT_HZ       1193182u
#define PIT_CH0_DATA 0x40
#define PIT_CMD      0x43

int arch_test_periodic_irq_start(unsigned hz)
{
    if (hz == 0)
        hz = 100;
    unsigned divisor = PIT_HZ / hz;
    if (divisor == 0)
        divisor = 1;
    if (divisor > 0xFFFF)
        divisor = 0xFFFF;

    /* Channel 0, lobyte/hibyte, mode 2 (rate generator), binary. */
    outb(PIT_CMD, 0x34);
    outb(PIT_CH0_DATA, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH0_DATA, (uint8_t)(divisor >> 8));
    return 0; /* ISA IRQ 0 */
}

void arch_test_periodic_irq_stop(void)
{
    /* Mode 0 with a count of 0 fires once after 65536 ticks and then
     * stays quiet; the IOAPIC entry is masked by the caller anyway. */
    outb(PIT_CMD, 0x30);
    outb(PIT_CH0_DATA, 0);
    outb(PIT_CH0_DATA, 0);
}
