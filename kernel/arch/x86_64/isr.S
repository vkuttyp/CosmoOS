/*
 * isr.S - Interrupt and exception entry stubs.
 *
 * 256 stubs of exactly 16 bytes each, so idt.c can compute stub i as
 * x86_isr_stubs + i * 16 without a table in the binary. Vectors that push
 * a hardware error code (8, 10-14, 17, 21, 29, 30) skip the dummy push so
 * the frame layout is uniform (see x86/trapframe.h).
 *
 * The common path saves the general registers, calls C with the frame
 * pointer, restores, and returns with IRETQ. No SWAPGS yet: there is no
 * user mode, so every trap arrives from ring 0. Adding user mode means
 * adding SWAPGS here conditioned on the saved CS.
 */

    .section .text, "ax", @progbits

    .globl x86_isr_stubs
    .type x86_isr_stubs, @object
    .balign 16
x86_isr_stubs:
    .set vec, 0
    .rept 256
    .balign 16
    .if vec == 8 || vec == 10 || vec == 11 || vec == 12 || vec == 13 || vec == 14 || vec == 17 || vec == 21 || vec == 29 || vec == 30
    /* CPU pushed an error code */
    .else
    pushq $0
    .endif
    pushq $vec
    jmp isr_common
    .set vec, vec + 1
    .endr
    .size x86_isr_stubs, . - x86_isr_stubs

    .type isr_common, @function
isr_common:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    cld
    mov %rsp, %rdi
    call x86_trap_dispatch

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    /* Drop vector and error code. */
    add $16, %rsp
    iretq
    .size isr_common, . - isr_common

    .section .note.GNU-stack, "", @progbits
