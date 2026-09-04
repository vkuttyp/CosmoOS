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

/* Monotonic counter (TSC on x86-64) and its frequency. */
uint64_t arch_clock_read(void);
uint64_t arch_clock_hz(void);
const char *arch_clock_name(void);

#endif /* ARCH_TIMER_H */
