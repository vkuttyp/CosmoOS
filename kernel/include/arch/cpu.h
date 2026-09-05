/*
 * arch/cpu.h - CPU control interface every architecture implements.
 *
 * This is the generic kernel's only view of the processor. Implementations
 * live in kernel/arch/<arch>/. None of these functions allocate or sleep.
 */

#ifndef ARCH_CPU_H
#define ARCH_CPU_H

#include <kernel/compiler.h>

/* Architecture name for banners and logs, e.g. "x86_64". */
const char *arch_name(void);

/* Human-readable processor description, NUL-terminated, truncated to len. */
void arch_cpu_brand_string(char *buf, size_t len);

/* Logical CPU index of the caller. Always 0 until SMP bring-up. */
unsigned arch_cpu_id(void);

/* Spin-wait hint. */
void arch_cpu_relax(void);

/* Wait for the next interrupt with interrupts enabled on return only if
 * they were enabled on entry. */
void arch_cpu_wait_for_interrupt(void);

/* Disable interrupts and halt forever. Used by panic and shutdown. */
void arch_cpu_halt_forever(void) __noreturn;

/* Order this CPU's memory writes before a device observes them (a DMA
 * descriptor before its doorbell). x86-64: a store fence; AArch64: dsb sy. */
void arch_dma_barrier(void);

#endif /* ARCH_CPU_H */
