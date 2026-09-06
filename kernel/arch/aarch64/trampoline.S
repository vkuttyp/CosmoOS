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
 * On a machine with the virtualization extensions PSCI starts the CPU
 * at EL2, as it started the boot CPU; the same arrangement applies
 * (docs/kernel/arch/aarch64/design.md, "Exception level 2"): program
 * EL2, point VBAR_EL2 at the stub the loader left, and ERET to EL1 with
 * the EL1 state already loaded. At EL1 the code below runs unchanged.
 *
 * Mailbox layout (assembly ABI, see smp.c):
 *   0x00 ttbr0   0x08 ttbr1   0x10 mair   0x18 tcr   0x20 sctlr
 *   0x28 stack_top (VA)   0x30 entry (VA)   0x38 cpu index
 *   0x40 el2_stub (PA, 0 when there is no EL2)
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
    mrs x8, CurrentEL
    lsr x8, x8, #2
    cmp x8, #2
    b.ne 1f

    /* EL2: keep the door open, then leave through it. */
    mov x9, #(1 << 31)          /* HCR_EL2.RW: EL1 is AArch64 */
    msr hcr_el2, x9
    mov x9, #0x33FF             /* CPTR_EL2 RES1 */
    bic x9, x9, #(1 << 10)      /* TFP clear: EL1/EL0 may use FP */
    msr cptr_el2, x9
    mov x9, #3                  /* CNTHCTL_EL2: EL1 counters and timers */
    msr cnthctl_el2, x9
    msr cntvoff_el2, xzr
    msr vttbr_el2, xzr
    mrs x9, midr_el1
    msr vpidr_el2, x9
    mrs x9, mpidr_el1
    msr vmpidr_el2, x9
    ldr x9, [x19, #0x40]        /* the stub the loader installed */
    cbz x9, 2f
    msr vbar_el2, x9
2:  ldr x9, =0x30C50830         /* SCTLR_EL2 with the MMU off */
    msr sctlr_el2, x9
    isb
    msr sctlr_el1, x5
    isb
    msr sp_el1, x6
    ldr x9, =0x3C5              /* SPSR_EL2: EL1h, DAIF masked */
    msr spsr_el2, x9
    msr elr_el2, x7
    mov x29, xzr
    mov x30, xzr
    eret

1:  msr sctlr_el1, x5
    isb
    mov sp, x6
    mov x29, xzr
    mov x30, xzr
    br x7
    .size aarch64_ap_trampoline, . - aarch64_ap_trampoline
    .globl aarch64_ap_trampoline_end
aarch64_ap_trampoline_end:

    .section .note.GNU-stack, "", @progbits
