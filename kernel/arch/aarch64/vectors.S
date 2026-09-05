/*
 * vectors.S - Exception vector table and frame save/restore
 * (docs/kernel/arch/aarch64/design.md, "Vector table and frame").
 *
 * The kernel always runs on SP_EL1 (SPSel = 1). An exception from EL0
 * lands on SP_EL1, which is wherever the last eret left it: the thread's
 * own kernel stack. An exception from EL1 pushes onto the current kernel
 * stack. Every used slot builds a struct arch_trap_frame (trapframe.h),
 * calls aarch64_trap_entry(frame) and restores. The slots for SP0 and
 * AArch32 lower EL states, FIQ and SError also build a frame so the panic
 * can print it, then never return.
 */

#include <aarch64/trapframe.h>

    .macro save_frame kind
    sub sp, sp, #FRAME_SIZE
    stp x0, x1, [sp, #0x00]
    stp x2, x3, [sp, #0x10]
    stp x4, x5, [sp, #0x20]
    stp x6, x7, [sp, #0x30]
    stp x8, x9, [sp, #0x40]
    stp x10, x11, [sp, #0x50]
    stp x12, x13, [sp, #0x60]
    stp x14, x15, [sp, #0x70]
    stp x16, x17, [sp, #0x80]
    stp x18, x19, [sp, #0x90]
    stp x20, x21, [sp, #0xA0]
    stp x22, x23, [sp, #0xB0]
    stp x24, x25, [sp, #0xC0]
    stp x26, x27, [sp, #0xD0]
    stp x28, x29, [sp, #0xE0]
    str x30, [sp, #0xF0]
    mrs x21, elr_el1
    mrs x22, spsr_el1
    mrs x23, esr_el1
    mrs x24, far_el1
    stp x21, x22, [sp, #FRAME_OFF_ELR]
    stp x23, x24, [sp, #FRAME_OFF_ESR]
    mov x21, #\kind
    str x21, [sp, #FRAME_OFF_KIND]
    .endm

    /* User frames record SP_EL0; kernel frames the interrupted SP. */
    .macro save_sp_user
    mrs x21, sp_el0
    str x21, [sp, #FRAME_OFF_SP]
    .endm
    .macro save_sp_kernel
    add x21, sp, #FRAME_SIZE
    str x21, [sp, #FRAME_OFF_SP]
    .endm

    .macro restore_frame_common
    ldp x21, x22, [sp, #FRAME_OFF_ELR]
    msr elr_el1, x21
    msr spsr_el1, x22
    ldp x0, x1, [sp, #0x00]
    ldp x2, x3, [sp, #0x10]
    ldp x4, x5, [sp, #0x20]
    ldp x6, x7, [sp, #0x30]
    ldp x8, x9, [sp, #0x40]
    ldp x10, x11, [sp, #0x50]
    ldp x12, x13, [sp, #0x60]
    ldp x14, x15, [sp, #0x70]
    ldp x16, x17, [sp, #0x80]
    ldp x18, x19, [sp, #0x90]
    ldp x20, x21, [sp, #0xA0]
    ldp x22, x23, [sp, #0xB0]
    ldp x24, x25, [sp, #0xC0]
    ldp x26, x27, [sp, #0xD0]
    ldp x28, x29, [sp, #0xE0]
    ldr x30, [sp, #0xF0]
    add sp, sp, #FRAME_SIZE
    eret
    .endm

    .macro ventry target
    .balign 128
    b \target
    .endm

    .section .text, "ax", @progbits
    .balign 2048
    .globl aarch64_vectors
    .type aarch64_vectors, @object
aarch64_vectors:
    /* Current EL with SP0: the kernel never runs on SP_EL0. */
    ventry bad_0
    ventry bad_1
    ventry bad_2
    ventry bad_3
    /* Current EL with SPx: from the kernel itself. */
    ventry el1_sync
    ventry el1_irq
    ventry bad_6
    ventry bad_7
    /* Lower EL, AArch64: from user mode. */
    ventry el0_sync
    ventry el0_irq
    ventry bad_10
    ventry bad_11
    /* Lower EL, AArch32: never configured. */
    ventry bad_12
    ventry bad_13
    ventry bad_14
    ventry bad_15
    .size aarch64_vectors, . - aarch64_vectors

    .type el1_sync, @function
el1_sync:
    save_frame AARCH64_ENTRY_EL1_SYNC
    save_sp_kernel
    mov x0, sp
    bl aarch64_trap_entry
    restore_frame_common
    .size el1_sync, . - el1_sync

    .type el1_irq, @function
el1_irq:
    save_frame AARCH64_ENTRY_EL1_IRQ
    save_sp_kernel
    mov x0, sp
    bl aarch64_trap_entry
    restore_frame_common
    .size el1_irq, . - el1_irq

    .type el0_sync, @function
el0_sync:
    save_frame AARCH64_ENTRY_EL0_SYNC
    save_sp_user
    mov x0, sp
    bl aarch64_trap_entry
    ldr x21, [sp, #FRAME_OFF_SP]
    msr sp_el0, x21
    restore_frame_common
    .size el0_sync, . - el0_sync

    .type el0_irq, @function
el0_irq:
    save_frame AARCH64_ENTRY_EL0_IRQ
    save_sp_user
    mov x0, sp
    bl aarch64_trap_entry
    ldr x21, [sp, #FRAME_OFF_SP]
    msr sp_el0, x21
    restore_frame_common
    .size el0_irq, . - el0_irq

    .macro bad_slot n
    .type bad_\n, @function
bad_\n:
    save_frame (AARCH64_ENTRY_BAD_BASE + \n)
    save_sp_kernel
    mov x0, sp
    bl aarch64_trap_entry
1:  wfi
    b 1b
    .size bad_\n, . - bad_\n
    .endm
    bad_slot 0
    bad_slot 1
    bad_slot 2
    bad_slot 3
    bad_slot 6
    bad_slot 7
    bad_slot 10
    bad_slot 11
    bad_slot 12
    bad_slot 13
    bad_slot 14
    bad_slot 15

    /*
     * void aarch64_user_enter(uint64_t entry, uint64_t sp)
     * First entry into user mode: EL0t, interrupts enabled, every
     * general register zero. Interrupts are masked by the caller.
     */
    .globl aarch64_user_enter
    .type aarch64_user_enter, @function
aarch64_user_enter:
    msr elr_el1, x0
    msr sp_el0, x1
    mov x0, xzr                 /* SPSR: EL0t, DAIF clear */
    msr spsr_el1, x0
    mov x1, xzr
    mov x2, xzr
    mov x3, xzr
    mov x4, xzr
    mov x5, xzr
    mov x6, xzr
    mov x7, xzr
    mov x8, xzr
    mov x9, xzr
    mov x10, xzr
    mov x11, xzr
    mov x12, xzr
    mov x13, xzr
    mov x14, xzr
    mov x15, xzr
    mov x16, xzr
    mov x17, xzr
    mov x18, xzr
    mov x19, xzr
    mov x20, xzr
    mov x21, xzr
    mov x22, xzr
    mov x23, xzr
    mov x24, xzr
    mov x25, xzr
    mov x26, xzr
    mov x27, xzr
    mov x28, xzr
    mov x29, xzr
    mov x30, xzr
    eret
    .size aarch64_user_enter, . - aarch64_user_enter

    .section .note.GNU-stack, "", @progbits
