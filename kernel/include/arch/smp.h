/*
 * arch/smp.h - Starting application processors.
 */

#ifndef ARCH_SMP_H
#define ARCH_SMP_H

#include <kernel/compiler.h>

/* Hardware id (local interrupt controller id) of the boot CPU, to skip
 * it when enumerating firmware processor entries. */
uint32_t arch_smp_boot_hw_id(void);

/* Allocate per-CPU architecture tables for `cpu` on the boot CPU before
 * it is started. Returns 0 or -ENOMEM. */
int arch_smp_prepare_cpu(unsigned cpu);

/* Start the CPU with hardware id `hw_id` as index `cpu` on the given
 * stack. The new CPU initialises itself and ends in sched_start_cpu().
 * Returns 0 once it has signalled it is executing kernel code, or
 * -ETIMEDOUT. */
int arch_smp_start_cpu(unsigned cpu, uint32_t hw_id, uintptr_t stack_top);

/* After every start attempt: release bring-up resources. */
void arch_smp_finish(void);

#endif /* ARCH_SMP_H */
