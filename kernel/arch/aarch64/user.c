/* user.c - Entering EL0, user-memory access windows, TLS (docs/kernel/arch/aarch64/design.md). */

#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/thread.h>
#include <arch/irq.h>
#include <arch/user.h>
#include <aarch64/platform.h>
#include <aarch64/sysreg.h>
#include <aarch64/trapframe.h>

void aarch64_user_enter(uint64_t entry, uint64_t sp) __attribute__((noreturn));

void arch_syscall_init_cpu(void)
{
    /* SVC is a vector table slot; nothing to program. PAN policy is set in aarch64_cpu_init. */
}

void arch_user_enter(uintptr_t entry, uintptr_t sp)
{
    struct thread *t = thread_current();
    KASSERT(t != NULL && t->proc != NULL);
    arch_irq_disable();
    this_cpu()->kernel_stack_top = t->stack_base + t->stack_size;
    WRITE_SYSREG(tpidr_el0, t->tls_base);
    aarch64_user_enter(entry, sp);
}

/* PSTATE.PAN: MSR PAN, #imm is an MSR (immediate) encoding the ARMv8.0 assembler may refuse. */
void arch_user_access_begin(void)
{
    if (aarch64_cpu_info()->has_pan)
        __asm__ volatile(".inst 0xd500409f" ::: "memory");   /* msr pan, #0 */
}

void arch_user_access_end(void)
{
    if (aarch64_cpu_info()->has_pan)
        __asm__ volatile(".inst 0xd500419f" ::: "memory");   /* msr pan, #1 */
}

void arch_set_tls_base(uintptr_t base)
{
    struct thread *t = thread_current();
    t->tls_base = base;
    WRITE_SYSREG(tpidr_el0, base);
}

bool arch_trap_frame_is_user(const struct arch_trap_frame *frame)
{
    return (frame->spsr & (SPSR_M_MASK | SPSR_M_AARCH32)) == SPSR_M_EL0T;
}
