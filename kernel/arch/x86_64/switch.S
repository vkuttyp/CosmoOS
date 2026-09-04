/*
 * context.S - Thread context switch for x86-64.
 *
 * arch_context_switch(from, to):
 *   push callee-saved registers, store rsp into from->sp, load to->sp,
 *   pop callee-saved registers, ret. The "return" lands wherever the
 *   target last called arch_context_switch, or, for a new thread, at the
 *   entry address arch_context_init placed on its stack.
 *
 * Caller-saved registers and flags are the C caller's problem, exactly
 * as for any function call. Interrupts must be disabled by the caller;
 * RFLAGS is not saved here.
 */

    .section .text, "ax", @progbits

    .globl arch_context_switch
    .type arch_context_switch, @function
arch_context_switch:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, (%rdi)       /* from->sp */
    movq (%rsi), %rsp       /* to->sp */

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret
    .size arch_context_switch, . - arch_context_switch

/*
 * First-run trampoline for a new thread. arch_context_init places this
 * as the "return address" above the six callee-saved slots, and the C
 * entry above that. When arch_context_switch pops into the new context
 * and returns here, rsp points at the entry address; jump to it with a
 * 16-byte-aligned stack and a null frame pointer.
 */
    .globl x86_context_start
    .type x86_context_start, @function
x86_context_start:
    popq %rax               /* entry function */
    xorl %ebp, %ebp
    pushq $0                /* fake return address: backtraces stop here */
    jmp *%rax
    .size x86_context_start, . - x86_context_start

    .section .note.GNU-stack, "", @progbits
