/*
 * switch.S - Thread context switch (docs/kernel/arch/aarch64/design.md,
 * "Threads, per-CPU, user mode").
 *
 * void arch_context_switch(struct arch_context *from, struct arch_context *to)
 *
 * Saves the callee-saved registers (x19..x29) and the return address on
 * the outgoing stack, records its sp, loads the incoming sp and restores.
 * A new thread's frame (arch_context_init) has x19 = entry and
 * x30 = aarch64_context_start, so its first "return" lands there.
 */

    .section .text, "ax", @progbits
    .globl arch_context_switch
    .type arch_context_switch, @function
arch_context_switch:
    sub sp, sp, #96
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    mov x9, sp
    str x9, [x0]                /* from->sp */
    ldr x9, [x1]                /* to->sp */
    mov sp, x9
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    add sp, sp, #96
    ret
    .size arch_context_switch, . - arch_context_switch

    /* First run of a new thread: x19 holds the entry function. */
    .globl aarch64_context_start
    .type aarch64_context_start, @function
aarch64_context_start:
    mov x29, xzr                /* backtraces stop here */
    mov x30, xzr
    blr x19
1:  wfi
    b 1b
    .size aarch64_context_start, . - aarch64_context_start

    .section .note.GNU-stack, "", @progbits
