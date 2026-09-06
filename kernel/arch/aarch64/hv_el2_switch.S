/*
 * hv_el2_switch.S - The EL2 world switch
 * (docs/kernel-services/virtualization/design.md, "The AArch64 EL2
 * backend").
 *
 * The kernel runs at EL1 and so does the guest, so the switch lives at
 * EL2: this vector table is installed through the loader's stub
 * (el2_set_vectors) and entered by HVC from the host with
 * x0 = HV_EL2_CALL_RUN, x1 = the physical address of a struct hv_ctx.
 *
 * It runs with the EL2 MMU off, like the stub, so every address here is
 * physical -- including the context page. Nothing is called; the host
 * side is a single HVC that returns when the guest exits.
 *
 * EL2 has a stack of its own (the top of the context page, set through
 * the stub's SET_STACK call before these vectors are installed), because
 * a guest's exception has no free register: the entry pushes the two it
 * needs and recovers the context from TPIDR_EL2, which the entry path
 * set before the ERET.
 */

#include <aarch64/hv_ctx.h>

    .section .text, "ax", @progbits
    .balign 2048
    .globl hv_el2_vectors
hv_el2_vectors:

.macro VEC label
    .balign 0x80
    b \label
.endm

    /* current EL, SP0: nothing runs there */
    VEC el2_panic
    VEC el2_panic
    VEC el2_panic
    VEC el2_panic
    /* current EL, SPx: an exception in the switch itself */
    VEC el2_panic
    VEC el2_panic
    VEC el2_panic
    VEC el2_panic
    /* lower EL, AArch64: the host's HVC and every guest exit */
    VEC el2_sync
    VEC el2_irq
    VEC el2_fiq
    VEC el2_serror
    /* lower EL, AArch32: no such guest */
    VEC el2_panic
    VEC el2_panic
    VEC el2_panic
    VEC el2_panic

/* --- helpers ---------------------------------------------------------- */

/* Save the EL1 system registers into the block at \base + \off. */
.macro SAVE_SYS base, off, tmp
    add \tmp, \base, #\off
    mrs x2, sctlr_el1
    mrs x3, ttbr0_el1
    stp x2, x3, [\tmp, #0]
    mrs x2, ttbr1_el1
    mrs x3, tcr_el1
    stp x2, x3, [\tmp, #16]
    mrs x2, mair_el1
    mrs x3, amair_el1
    stp x2, x3, [\tmp, #32]
    mrs x2, vbar_el1
    mrs x3, esr_el1
    stp x2, x3, [\tmp, #48]
    mrs x2, far_el1
    mrs x3, elr_el1
    stp x2, x3, [\tmp, #64]
    mrs x2, spsr_el1
    mrs x3, sp_el0
    stp x2, x3, [\tmp, #80]
    mrs x2, tpidr_el0
    mrs x3, tpidrro_el0
    stp x2, x3, [\tmp, #96]
    mrs x2, tpidr_el1
    mrs x3, contextidr_el1
    stp x2, x3, [\tmp, #112]
    mrs x2, cpacr_el1
    mrs x3, par_el1
    stp x2, x3, [\tmp, #128]
    mrs x2, mdscr_el1
    mrs x3, cntkctl_el1
    stp x2, x3, [\tmp, #144]
.endm

.macro LOAD_SYS base, off, tmp
    add \tmp, \base, #\off
    ldp x2, x3, [\tmp, #0]
    msr sctlr_el1, x2
    msr ttbr0_el1, x3
    ldp x2, x3, [\tmp, #16]
    msr ttbr1_el1, x2
    msr tcr_el1, x3
    ldp x2, x3, [\tmp, #32]
    msr mair_el1, x2
    msr amair_el1, x3
    ldp x2, x3, [\tmp, #48]
    msr vbar_el1, x2
    msr esr_el1, x3
    ldp x2, x3, [\tmp, #64]
    msr far_el1, x2
    msr elr_el1, x3
    ldp x2, x3, [\tmp, #80]
    msr spsr_el1, x2
    msr sp_el0, x3
    ldp x2, x3, [\tmp, #96]
    msr tpidr_el0, x2
    msr tpidrro_el0, x3
    ldp x2, x3, [\tmp, #112]
    msr tpidr_el1, x2
    msr contextidr_el1, x3
    ldp x2, x3, [\tmp, #128]
    msr cpacr_el1, x2
    msr par_el1, x3
    ldp x2, x3, [\tmp, #144]
    msr mdscr_el1, x2
    msr cntkctl_el1, x3
.endm

/* --- the host's HVC: enter a guest ------------------------------------ */

el2_sync:
    /* The host's HVC and a guest's synchronous exception arrive here.
     * Tell them apart by HCR_EL2.VM -- stage 2 is on only while a guest
     * runs -- after saving the registers the check itself uses. */
    stp x0, x1, [sp, #-32]!
    stp x2, x3, [sp, #16]
    mrs x2, hcr_el2
    tbnz x2, #0, el2_guest_sync      /* VM set: from the guest */
    ldp x2, x3, [sp, #16]
    ldp x0, x1, [sp], #32

    mrs x2, esr_el2
    lsr x2, x2, #26
    cmp x2, #0x16                    /* HVC64 */
    b.ne el2_return_x0_minus1

    cmp x0, #HV_EL2_CALL_VERSION
    b.ne 1f
    mov x0, #HV_EL2_VERSION
    eret
1:  cmp x0, #HV_EL2_CALL_RUN
    b.ne el2_return_x0_minus1

    str x18, [x1, #HV_CTX_HOST_X18]  /* the host's x18 is reserved (-ffixed-x18): keep it */
    mov x18, x1                      /* the context, physically addressed */
    msr tpidr_el2, x18               /* how the exit path finds it again */

    /* Save the host: callee-saved registers, its stack, and where the
     * HVC came from, so the exit path can return into it. */
    add x4, x18, #HV_CTX_HOST_X
    stp x19, x20, [x4, #0]
    stp x21, x22, [x4, #16]
    stp x23, x24, [x4, #32]
    stp x25, x26, [x4, #48]
    stp x27, x28, [x4, #64]
    stp x29, x30, [x4, #80]
    mrs x2, sp_el1
    str x2, [x4, #96]
    mrs x2, elr_el2
    mrs x3, spsr_el2
    str x2, [x18, #HV_CTX_HOST_ELR]
    str x3, [x18, #HV_CTX_HOST_SPSR]
    SAVE_SYS x18, HV_CTX_HOST_SYS, x4

    /* Stage 2 and the trap configuration for this guest. */
    ldr x2, [x18, #HV_CTX_VTCR]
    msr vtcr_el2, x2
    ldr x2, [x18, #HV_CTX_VTTBR]
    msr vttbr_el2, x2
    ldr x2, [x18, #HV_CTX_HCR]
    msr hcr_el2, x2
    isb
    /* The stage-2 tables changed since the last entry: drop what this
     * VMID has cached. VTTBR_EL2 is set above, so the VMID is ours; the
     * invalidation is inner-shareable and reaches every CPU. */
    ldr x2, [x18, #HV_CTX_FLUSH]
    cbz x2, 2f
    tlbi vmalls12e1is
    dsb ish
    isb
    str xzr, [x18, #HV_CTX_FLUSH]
2:

    LOAD_SYS x18, HV_CTX_GUEST_SYS, x4
    ldr x2, [x18, #HV_CTX_GUEST_SP_EL1]
    msr sp_el1, x2
    ldr x2, [x18, #HV_CTX_GUEST_PC]
    msr elr_el2, x2
    ldr x2, [x18, #HV_CTX_GUEST_PSTATE]
    msr spsr_el2, x2
    add x2, x18, #4096               /* the rest of the context page is EL2's stack */
    mov sp, x2

    /* Guest general-purpose registers; x18 last, from itself. */
    ldp x0, x1, [x18, #(HV_CTX_GUEST_X + 0)]
    ldp x2, x3, [x18, #(HV_CTX_GUEST_X + 16)]
    ldp x4, x5, [x18, #(HV_CTX_GUEST_X + 32)]
    ldp x6, x7, [x18, #(HV_CTX_GUEST_X + 48)]
    ldp x8, x9, [x18, #(HV_CTX_GUEST_X + 64)]
    ldp x10, x11, [x18, #(HV_CTX_GUEST_X + 80)]
    ldp x12, x13, [x18, #(HV_CTX_GUEST_X + 96)]
    ldp x14, x15, [x18, #(HV_CTX_GUEST_X + 112)]
    ldp x16, x17, [x18, #(HV_CTX_GUEST_X + 128)]
    ldp x19, x20, [x18, #(HV_CTX_GUEST_X + 152)]
    ldp x21, x22, [x18, #(HV_CTX_GUEST_X + 168)]
    ldp x23, x24, [x18, #(HV_CTX_GUEST_X + 184)]
    ldp x25, x26, [x18, #(HV_CTX_GUEST_X + 200)]
    ldp x27, x28, [x18, #(HV_CTX_GUEST_X + 216)]
    ldp x29, x30, [x18, #(HV_CTX_GUEST_X + 232)]
    ldr x18, [x18, #(HV_CTX_GUEST_X + 144)]
    eret

/* --- a guest exception: leave the guest ------------------------------- */

/* Every entry arrives with the guest's x0..x3 pushed on the EL2 stack
 * (el2_sync pushed them before it could tell host from guest; the others
 * push their own), so those four are the only scratch available until
 * the context pointer is in hand. Nothing else may be touched before it
 * is saved. */
el2_guest_sync:
    mov x0, #0
    b el2_exit
el2_irq:
    stp x0, x1, [sp, #-32]!
    stp x2, x3, [sp, #16]
    mov x0, #1
    b el2_exit
el2_fiq:
    stp x0, x1, [sp, #-32]!
    stp x2, x3, [sp, #16]
    mov x0, #2
    b el2_exit
el2_serror:
    stp x0, x1, [sp, #-32]!
    stp x2, x3, [sp, #16]
    mov x0, #3
    /* fall through */

/* x0 = the kind of exception; the guest's x0..x3 are on the stack. */
el2_exit:
    mrs x1, tpidr_el2
    str x18, [x1, #(HV_CTX_GUEST_X + 144)]   /* the guest's x18, before x18 becomes ours */
    str x0, [x1, #HV_CTX_EXIT_KIND]
    mov x18, x1
    ldp x0, x1, [sp, #0]                     /* the guest's x0..x3 as they were */
    stp x0, x1, [x18, #(HV_CTX_GUEST_X + 0)]
    ldp x0, x1, [sp, #16]
    stp x0, x1, [x18, #(HV_CTX_GUEST_X + 16)]
    add sp, sp, #32
    stp x4, x5, [x18, #(HV_CTX_GUEST_X + 32)]
    stp x6, x7, [x18, #(HV_CTX_GUEST_X + 48)]
    stp x8, x9, [x18, #(HV_CTX_GUEST_X + 64)]
    stp x10, x11, [x18, #(HV_CTX_GUEST_X + 80)]
    stp x12, x13, [x18, #(HV_CTX_GUEST_X + 96)]
    stp x14, x15, [x18, #(HV_CTX_GUEST_X + 112)]
    stp x16, x17, [x18, #(HV_CTX_GUEST_X + 128)]
    stp x19, x20, [x18, #(HV_CTX_GUEST_X + 152)]
    stp x21, x22, [x18, #(HV_CTX_GUEST_X + 168)]
    stp x23, x24, [x18, #(HV_CTX_GUEST_X + 184)]
    stp x25, x26, [x18, #(HV_CTX_GUEST_X + 200)]
    stp x27, x28, [x18, #(HV_CTX_GUEST_X + 216)]
    stp x29, x30, [x18, #(HV_CTX_GUEST_X + 232)]

    mrs x2, elr_el2
    str x2, [x18, #HV_CTX_GUEST_PC]
    mrs x2, spsr_el2
    str x2, [x18, #HV_CTX_GUEST_PSTATE]
    mrs x2, sp_el1
    str x2, [x18, #HV_CTX_GUEST_SP_EL1]
    mrs x2, esr_el2
    str x2, [x18, #HV_CTX_EXIT_ESR]
    mrs x2, far_el2
    str x2, [x18, #HV_CTX_EXIT_FAR]
    mrs x2, hpfar_el2
    str x2, [x18, #HV_CTX_EXIT_HPFAR]
    SAVE_SYS x18, HV_CTX_GUEST_SYS, x4

    /* Back to the host: stage 2 off, its EL1 state and its registers. */
    msr vttbr_el2, xzr
    mov x2, #(1 << 31)               /* HCR_EL2.RW, nothing else */
    msr hcr_el2, x2
    isb
    LOAD_SYS x18, HV_CTX_HOST_SYS, x4
    add x4, x18, #HV_CTX_HOST_X
    ldr x2, [x4, #96]
    msr sp_el1, x2
    ldr x2, [x18, #HV_CTX_HOST_ELR]
    ldr x3, [x18, #HV_CTX_HOST_SPSR]
    msr elr_el2, x2
    msr spsr_el2, x3
    ldp x19, x20, [x4, #0]
    ldp x21, x22, [x4, #16]
    ldp x23, x24, [x4, #32]
    ldp x25, x26, [x4, #48]
    ldp x27, x28, [x4, #64]
    ldp x29, x30, [x4, #80]
    mov x0, #0                       /* the HVC returns 0; the exit is in the context */
    ldr x18, [x18, #HV_CTX_HOST_X18] /* last: x18 was the context pointer */
    eret

el2_return_x0_minus1:
    mov x0, #-1
    eret

/* An exception at EL2 itself: there is no handler and no way to report
 * it, so park rather than return into an unknown state. */
el2_panic:
    b el2_panic

    .globl hv_el2_vectors_end
hv_el2_vectors_end:

    .section .note.GNU-stack, "", @progbits
