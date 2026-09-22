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

/* Logical CPU index of the caller. Always 0 until SMP bring-up.
 *
 * `arch_cpu_id_raw` is the architecture's read, unchecked. `arch_cpu_id`
 * (kernel/core/percpu.c) is the same read and, in debug builds, the
 * check that the answer can be kept: invariant S25 in
 * docs/kernel/scheduler/invariants.md. Code wants `arch_cpu_id`, or
 * `raw_cpu_id` (percpu.h) with a reason. */
unsigned arch_cpu_id_raw(void);
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

/* One line per boot naming the CPU's protection features that are on,
 * and a WARN naming what is absent, with the consequence when the
 * missing feature is the guard on kernel access to user memory
 * (docs/kernel/security/design.md, "Hardening"). Called once the
 * kernel's own page tables are active, since that is when WXN is set. */
void arch_hardening_report(void);

#endif /* ARCH_CPU_H */
