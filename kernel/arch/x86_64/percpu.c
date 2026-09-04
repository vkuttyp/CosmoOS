/*
 * percpu.c - Per-CPU pointer through the GS segment base.
 *
 * The kernel GS base MSR points at this CPU's struct percpu, whose first
 * field is a pointer to itself, so `mov %gs:0, %rax` yields the pointer
 * in one instruction. There is no user mode yet, so no SWAPGS handling;
 * when user mode arrives, the kernel value moves to KERNEL_GS_BASE and
 * the trap entry swaps.
 */

#include <kernel/percpu.h>

#include <arch/cpu.h>
#include <arch/percpu.h>

#include <x86/cpu.h>

#define MSR_GS_BASE 0xC0000101u

/*
 * Hazard: any `mov %ax, %gs` (gdt_init reloads all segment registers)
 * replaces the GS base with the descriptor's base, which is 0. Install
 * the per-CPU pointer after the GDT is loaded on every CPU, and never
 * reload GS afterwards.
 */
void arch_percpu_install(struct percpu *pc)
{
    pc->self = pc;
    wrmsr(MSR_GS_BASE, (uint64_t)(uintptr_t)pc);
}

struct percpu *arch_percpu_get(void)
{
    struct percpu *pc;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(pc));
    return pc;
}

unsigned arch_cpu_id(void)
{
    unsigned id;
    __asm__ volatile("mov %%gs:%c1, %0" : "=r"(id) : "i"(__builtin_offsetof(struct percpu, cpu_id)));
    return id;
}
