/* context.c - Thread contexts, the boot stack, the switch hook (docs/kernel/arch/aarch64/design.md). */

#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/process.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/vmm.h>
#include <arch/context.h>
#include <arch/mmu.h>
#include <aarch64/sysreg.h>

void aarch64_context_start(void);

void arch_thread_switch_prepare(struct thread *next)
{
    /* No TSS: the kernel stack for the next exception from EL0 is simply
     * SP_EL1 at the moment of eret, i.e. the thread's own stack. */
    uintptr_t kstack = next->stack_base + next->stack_size;
    this_cpu()->kernel_stack_top = kstack;
    struct vm_space *space = next->proc ? next->proc->space : &kernel_space;
    if ((READ_SYSREG(ttbr0_el1) & DESC_ADDR_MASK) != (space->mmu.root & DESC_ADDR_MASK) ||
        (READ_SYSREG(ttbr1_el1) & DESC_ADDR_MASK) != (kernel_space.mmu.root & DESC_ADDR_MASK))
        arch_mmu_activate(&space->mmu);
    if (next->proc)
        WRITE_SYSREG(tpidr_el0, next->tls_base);
}

extern char aarch64_boot_stack_bottom[], aarch64_boot_stack_top[];

void arch_boot_stack(uintptr_t *base, size_t *size)
{
    *base = (uintptr_t)aarch64_boot_stack_bottom;
    *size = (size_t)(aarch64_boot_stack_top - aarch64_boot_stack_bottom);
}

/* The frame arch_context_switch restores: x19..x28, x29, x30 (96 bytes). */
void arch_context_init(struct arch_context *ctx, uintptr_t stack_top, void (*entry)(void))
{
    KASSERT((stack_top & 0xF) == 0);
    uint64_t *frame = (uint64_t *)(stack_top - 96);
    memset(frame, 0, 96);
    frame[0] = (uint64_t)(uintptr_t)entry;                     /* x19 */
    frame[11] = (uint64_t)(uintptr_t)aarch64_context_start;    /* x30 */
    ctx->sp = (uintptr_t)frame;
}
