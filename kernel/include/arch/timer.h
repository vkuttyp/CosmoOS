/*
 * arch/timer.h - Clock and tick hardware interface.
 */

#ifndef ARCH_TIMER_H
#define ARCH_TIMER_H

#include <kernel/compiler.h>

/* Boot CPU, once: measure the clock counter and the tick timer against a
 * reference. Panics on implausible results. */
void arch_timer_calibrate(void);

/* Calling CPU: start delivering the tick interrupt at `hz` on the vector
 * returned by arch_timer_vector(). */
void arch_timer_start_tick(unsigned hz);
void arch_timer_stop_tick(void);
unsigned arch_timer_vector(void);

/* The real-time clock, once at boot: seconds since 1970-01-01 UTC. False
 * when the platform has none the kernel can read (the wall clock then
 * starts at the epoch). x86-64: the CMOS RTC; AArch64: the PL031 of the
 * `virt` machine. Thread context; may map MMIO. */
bool arch_rtc_read_epoch(uint64_t *seconds);
/* Monotonic counter (TSC on x86-64) and its frequency. */
uint64_t arch_clock_read(void);
uint64_t arch_clock_hz(void);
const char *arch_clock_name(void);

/*
 * Is this counter a clock two CPUs may compare readings from?
 *
 * True when the hardware guarantees the counter runs at a constant rate
 * and does not stop -- an invariant TSC on x86-64, the system counter on
 * AArch64. False means `clock_now_ns` is still monotonic on one CPU but
 * the kernel makes no cross-CPU promise about it, which `timer_init`
 * says in the boot line and `clock_worst_offset_ns` reports as an
 * unbounded offset (docs/audit/next-subsystem-cpu-clock.md).
 *
 * `*why` is set to a short phrase naming what is missing when the answer
 * is false, for that boot line.
 */
bool arch_clock_is_common(const char **why);

#endif /* ARCH_TIMER_H */
