/*
 * smp.h - Bringing up and coordinating multiple CPUs.
 */

#ifndef KERNEL_SMP_H
#define KERNEL_SMP_H

#include <kernel/compiler.h>

typedef void (*smp_call_fn)(void *arg);

/* Start every application processor the firmware reports. Requires
 * sched_init and interrupts enabled on the boot CPU. CPUs that fail to
 * come up are logged and skipped. */
void smp_init(void);

/* Run fn(arg) on `cpu` in interrupt context and wait until it returns.
 * Calling the current CPU runs fn directly. Must be called with
 * interrupts enabled and no spinlock held. Panics on timeout. */
void smp_call_function_single(unsigned cpu, smp_call_fn fn, void *arg);

/* Halt every other online CPU (panic, shutdown). Returns once they have
 * reported offline or after a short bound. Safe with interrupts off. */
void smp_stop_others(void);

#endif /* KERNEL_SMP_H */
