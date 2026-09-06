/*
 * x86/trapframe.h - Register state saved on every interrupt/exception.
 * Private to x86-64; generic code sees only `struct arch_trap_frame *`.
 *
 * Layout matches the push order in isr.S exactly: the fifteen general
 * registers pushed by the common stub, then vector and error code pushed
 * by the per-vector stub, then the five words the CPU pushes. A vector
 * without a hardware error code gets 0 pushed in its place so the layout
 * is uniform.
 */

#ifndef X86_TRAPFRAME_H
#define X86_TRAPFRAME_H

#include <stdint.h>

struct arch_trap_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code;
    /* pushed by the CPU */
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

_Static_assert(sizeof(struct arch_trap_frame) == 22 * 8, "trap frame layout");

/* Called from isr.S with the frame at the top of the stack. */
void x86_trap_dispatch(struct arch_trap_frame *frame);
/* Same, for the paranoid vectors (#DB, NMI, #DF, #MC) on their IST
 * stacks: dispatches the handler, never schedules or delivers a kill. */
void x86_trap_paranoid(struct arch_trap_frame *frame);

/* Register state saved by syscall_entry.S. Order matches the pushes. */
struct x86_syscall_frame {
    uint64_t flags;                          /* X86_SYSCALL_*: how to return */
    uint64_t rcx, r11;                       /* the two registers SYSRET overwrites; restored on the IRETQ path */
    uint64_t r15, r14, r13, r12, rbp, rbx;   /* callee-saved, diagnostics */
    uint64_t r9, r8, r10, rdx, rsi, rdi;     /* arguments 6..1 */
    uint64_t rax;                            /* number in, result out */
    uint64_t rip, cs, rflags, rsp, ss;       /* user return state: an IRETQ frame */
};

/* Return through IRETQ with every register loaded from the frame: set when
 * the frame was rewritten (a signal frame, rt_sigreturn, a new thread's
 * initial registers) or when SYSRET would be unsafe (non-canonical rip:
 * the SYSRET canonical guard, docs/kernel/process/design.md §11). */
#define X86_SYSCALL_FULL_RESTORE 1ull

_Static_assert(sizeof(struct x86_syscall_frame) == 21 * 8, "syscall frame layout");

/* Called from syscall_entry.S with interrupts enabled. */
void x86_syscall_c(struct x86_syscall_frame *frame);

#endif /* X86_TRAPFRAME_H */
