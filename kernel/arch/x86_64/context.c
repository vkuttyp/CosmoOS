/*
 * context.c - Initial stack layout for a new thread context.
 *
 * Layout at stack_top (16-byte aligned), growing down:
 *   top-8   entry function            (popped by x86_context_start)
 *   top-16  &x86_context_start        (the `ret` target of the first switch)
 *   top-24  rbp = 0
 *   top-32  rbx = 0
 *   top-40  r12 = 0
 *   top-48  r13 = 0
 *   top-56  r14 = 0
 *   top-64  r15 = 0                   <- saved sp
 *
 * After the six pops and the ret, rsp = top-8, so x86_context_start pops
 * the entry, pushes a zero return address, and jumps with rsp = top-8:
 * 8 mod 16, exactly what the ABI expects at function entry.
 */

#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/process.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/vmm.h>

#include <arch/context.h>
#include <arch/mmu.h>

#include <x86/cpu.h>
#include <x86/fpu.h>
#include <x86/gdt.h>

#define MSR_FS_BASE 0xC0000100u

void x86_context_start(void);

void arch_thread_switch_prepare(struct thread *prev, struct thread *next)
{
    /* Vector/x87 registers: the outgoing owner's are saved, the incoming
     * owner's loaded (fpu.c). Interrupts are off and nothing between here
     * and the switch touches them. */
    x86_fpu_switch(prev, next);

    /* Kernel stack for traps from ring 3 and for SYSCALL. */
    uintptr_t kstack = next->stack_base + next->stack_size;
    this_cpu()->kernel_stack_top = kstack;
    gdt_set_kernel_stack(kstack);

    /* Address space: a process's own tables, or the kernel's. Skipped
     * when unchanged so kernel-thread to kernel-thread switches do not
     * flush non-global entries needlessly. */
    struct vm_space *space = next->proc ? next->proc->space : &kernel_space;
    if (read_cr3() != space->mmu.root) {
        struct percpu *pc = this_cpu();
        vm_space_switch(pc->cur_space, space);   /* maintains active_cpus around the CR3 write */
        pc->cur_space = space;
    }

    /* The user thread pointer (%fs base): kernel code never uses %fs, so
     * kernel threads keep whatever is there. */
    if (next->proc)
        wrmsr(MSR_FS_BASE, next->tls_base);
}
extern char x86_boot_stack_bottom[], x86_boot_stack_top[];

void arch_boot_stack(uintptr_t *base, size_t *size)
{
    *base = (uintptr_t)x86_boot_stack_bottom;
    *size = (size_t)(x86_boot_stack_top - x86_boot_stack_bottom);
}

void arch_context_init(struct arch_context *ctx, uintptr_t stack_top, void (*entry)(void))
{
    KASSERT((stack_top & 0xF) == 0);

    uint64_t *sp = (uint64_t *)stack_top;
    *--sp = (uint64_t)(uintptr_t)entry;
    *--sp = (uint64_t)(uintptr_t)x86_context_start;
    for (int i = 0; i < 6; i++)
        *--sp = 0;

    ctx->sp = (uintptr_t)sp;
}
