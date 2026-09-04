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

#endif /* X86_TRAPFRAME_H */
