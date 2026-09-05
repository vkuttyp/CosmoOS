/*
 * isr.S - Interrupt and exception entry stubs.
 *
 * 256 stubs of exactly 16 bytes each, so idt.c can compute stub i as
 * x86_isr_stubs + i * 16 without a table in the binary. Vectors that push
 * a hardware error code (8, 10-14, 17, 21, 29, 30) skip the dummy push so
 * the frame layout is uniform (see x86/trapframe.h).
 *
 * The common path saves the general registers, decides SWAPGS from the
 * saved CS (ring 3 arrived with the user's GS base), calls C with the
 * frame pointer, restores, and returns with IRETQ.
 *
 * Four vectors cannot use that rule: #DB (1), NMI (2), #DF (8) and #MC
 * (18) can arrive at any instruction, including the SYSCALL entry
 * window between `swapgs` and the kernel-stack load (CS is already the
 * kernel's but GS is still the user's and RSP the user's) and the
 * SYSRET exit window after `popq %rsp` (RSP is the user's again). They
 * take the paranoid path: the IDT gives each its own IST stack, and
 * isr_paranoid decides SWAPGS from the GS base MSR itself (a kernel
 * per-CPU block is a higher-half address; a user GS base never is),
 * remembering the decision for the exit. Nothing on that path may
 * schedule or deliver signals (x86_trap_paranoid). Documented in
 * docs/kernel/arch/design.md and invariants I-ARCH-7, I-ARCH-15.
 */

#define MSR_GS_BASE 0xC0000101

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
    .if vec == 1 || vec == 2 || vec == 8 || vec == 18
    jmp isr_paranoid
    .else
    jmp isr_common
    .endif
    .set vec, vec + 1
    .endr
    .size x86_isr_stubs, . - x86_isr_stubs

    /*
     * Paranoid entry: on an IST stack (the IDT put us here), interrupts
     * off (interrupt gate). Save the frame, then read the GS base MSR: if
     * it is not a kernel address the interrupted context had the user's
     * GS (ring 3, or ring 0 inside a SYSCALL/SYSRET window), so swap and
     * remember it in rbx, which the C call preserves and the frame pop
     * restores afterwards.
     */
    .type isr_paranoid, @function
isr_paranoid:
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

    movl $MSR_GS_BASE, %ecx
    rdmsr                       /* edx:eax = GS base */
    xorl %ebx, %ebx
    testl %edx, %edx            /* bit 63 set: a kernel address, GS already ours */
    js 1f
    swapgs
    movl $1, %ebx
1:
    cld
    mov %rsp, %rdi
    call x86_trap_paranoid

    testl %ebx, %ebx
    jz 2f
    swapgs
2:
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
    add $16, %rsp
    iretq
    .size isr_paranoid, . - isr_paranoid

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

    /* From ring 3 the GS base is the user's: swap to the kernel's. The
     * saved CS sits above the 15 registers, vector, and error code:
     * offset 15*8 + 16 + 8 = 144. */
    testb $3, 144(%rsp)
    jz 1f
    swapgs
1:
    cld
    mov %rsp, %rdi
    call x86_trap_dispatch

    /* Returning to ring 3: swap back. Interrupts are off here (the
     * dispatch tail leaves IF clear), so nothing can run between the
     * swap and iretq. */
    testb $3, 144(%rsp)
    jz 2f
    swapgs
2:
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
