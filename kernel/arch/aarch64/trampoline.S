/*
 * trampoline.S - Secondary CPU trampoline (docs/kernel/arch/aarch64/design.md, "SMP").
 *
 * PSCI CPU_ON starts the CPU at this physical address with the MMU off,
 * x0 = the physical address of its struct aarch64_ap_mailbox (smp.c).
 * The page holding this code is identity-mapped in the temporary TTBR0
 * table the mailbox names, so enabling the MMU keeps the PC valid; the
 * jump to the mailbox's entry (a higher-half address) then leaves the
 * identity map behind. Position-independent: no absolute references.
 *
 * Mailbox layout (assembly ABI, see smp.c):
 *   0x00 ttbr0   0x08 ttbr1   0x10 mair   0x18 tcr   0x20 sctlr
 *   0x28 stack_top (VA)   0x30 entry (VA)   0x38 cpu index
 */

    .section .text, "ax", @progbits
    .balign 64
    .globl aarch64_ap_trampoline
    .type aarch64_ap_trampoline, @function
aarch64_ap_trampoline:
    msr daifset, #0xF
    mov x19, x0
    ldp x1, x2, [x19, #0x00]
    ldp x3, x4, [x19, #0x10]
    ldr x5, [x19, #0x20]
    ldp x6, x7, [x19, #0x28]    /* stack_top, entry: read before the MMU is on (the mailbox
                                   is not identity-mapped, only this page is) */
    ldr x0, [x19, #0x38]        /* cpu index */
    msr ttbr0_el1, x1
    msr ttbr1_el1, x2
    msr mair_el1, x3
    msr tcr_el1, x4
    dsb sy
    tlbi vmalle1
    dsb sy
    isb
    msr sctlr_el1, x5
    isb
    mov sp, x6
    mov x29, xzr
    mov x30, xzr
    br x7
    .size aarch64_ap_trampoline, . - aarch64_ap_trampoline
    .globl aarch64_ap_trampoline_end
aarch64_ap_trampoline_end:

    .section .note.GNU-stack, "", @progbits
