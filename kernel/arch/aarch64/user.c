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

/* --- the user register set (milestone 10) ------------------------------------ */

#include <kernel/string.h>

void aarch64_user_enter_regs(const struct arch_user_regs *r) __attribute__((noreturn));

/* The system-call frame is the trap frame on this architecture. */
void arch_user_regs_from_syscall(const void *frame, struct arch_user_regs *r)
{
    arch_user_regs_from_trap(frame, r);
}

void arch_user_regs_to_syscall(void *frame, const struct arch_user_regs *r)
{
    arch_user_regs_to_trap(frame, r);
}

void arch_user_regs_from_trap(const struct arch_trap_frame *f, struct arch_user_regs *r)
{
    memcpy(r->x, f->x, sizeof(r->x));
    r->sp = f->sp;
    r->pc = f->elr;
    r->pstate = f->spsr;
}

void arch_user_regs_to_trap(struct arch_trap_frame *f, const struct arch_user_regs *r)
{
    memcpy(f->x, r->x, sizeof(f->x));
    f->sp = r->sp;
    f->elr = r->pc;
    f->spsr = (r->pstate & AARCH64_PSTATE_USER_MASK) | SPSR_M_EL0T;
}

uintptr_t arch_user_regs_pc(const struct arch_user_regs *r) { return (uintptr_t)r->pc; }
uintptr_t arch_user_regs_sp(const struct arch_user_regs *r) { return (uintptr_t)r->sp; }
void arch_user_regs_set_pc(struct arch_user_regs *r, uintptr_t pc) { r->pc = pc; }
void arch_user_regs_set_sp(struct arch_user_regs *r, uintptr_t sp) { r->sp = sp; }
void arch_user_regs_set_result(struct arch_user_regs *r, int64_t v) { r->x[0] = (uint64_t)v; }
int64_t arch_user_regs_result(const struct arch_user_regs *r) { return (int64_t)r->x[0]; }

void arch_user_regs_restart_syscall(struct arch_user_regs *r, uint64_t nr, uint64_t arg0)
{
    r->x[8] = nr;
    r->x[0] = arg0;
    r->pc -= 4;   /* the SVC instruction */
}

void arch_user_regs_sanitize(struct arch_user_regs *r)
{
    r->pstate = (r->pstate & AARCH64_PSTATE_USER_MASK) | SPSR_M_EL0T;
}

void arch_user_enter_regs(const struct arch_user_regs *r)
{
    struct thread *t = thread_current();
    KASSERT(t != NULL && t->proc != NULL);
    struct arch_user_regs regs = *r;
    arch_user_regs_sanitize(&regs);
    arch_irq_disable();
    this_cpu()->kernel_stack_top = t->stack_base + t->stack_size;
    WRITE_SYSREG(tpidr_el0, t->tls_base);
    aarch64_user_enter_regs(&regs);
}

void arch_user_regs_set_result_in_frame(void *frame, int64_t v) { ((struct arch_trap_frame *)frame)->x[0] = (uint64_t)v; }
int64_t arch_user_regs_result_in_frame(const void *frame) { return (int64_t)((const struct arch_trap_frame *)frame)->x[0]; }

/* No user FP/SIMD state on AArch64 yet (fpu.c): the signal frame carries none. */
size_t arch_user_fpu_image_size(void) { return 0; }
bool arch_user_fpu_image_save(void *buf) { (void)buf; return false; }
bool arch_user_fpu_image_restore(const void *buf) { (void)buf; return false; }
