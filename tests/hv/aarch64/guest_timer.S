/*
 * guest_timer.S - A guest that uses its own timer
 * (docs/kernel-services/virtualization/testing.md).
 *
 * Nothing here knows it is virtualised: the guest reads CNTVCT_EL0 for
 * the time, arms CNTV_TVAL_EL0 for a deadline about 15 ms off, and takes
 * the timer's interrupt through the same virtual CPU interface the
 * interrupt guest uses. Every hypercall carries the guest's clock in x1,
 * so the owner can compare what the guest sees with what it should.
 *
 *   hvc  x0 = 1, x1 = CNTVCT     "ready", before arming
 *   (arm the timer)
 *   loop: hvc  x0 = 2, x1 = CNTVCT  heartbeat, so every run ends in an exit
 *   IRQ -> handler:
 *       mrs x0, ICC_IAR1_EL1       acknowledge: x0 is the INTID
 *       mrs x1, CNTV_CTL_EL0       the timer's own account of itself
 *       msr CNTV_CTL_EL0, IMASK    quiet it, or it fires again on eret
 *       hvc                        report both
 *       msr ICC_EOIR1_EL1, x0      complete
 *       eret
 *
 * The heartbeat is a hypercall rather than a WFI for the reason
 * guest_irq.S records: HCR_EL2.TWI traps a WFI only when it would wait.
 */
    .text
    .globl _start
_start:
    adr  x0, vectors
    msr  vbar_el1, x0
    /* The timer's PPI, in the redistributor: a kernel enables it there,
     * and so does this guest, or the expiry it arms would stay pending
     * in a controller that was never told to forward it. */
    movz x10, #0x0800, lsl #16     /* GICD */
    mov  w0, #0x13
    str  w0, [x10]                 /* GICD_CTLR: affinity routing, group 1 on */
    mrs  x11, mpidr_el1
    and  x11, x11, #0xFF
    lsl  x11, x11, #17
    movz x12, #0x080A, lsl #16
    add  x11, x11, x12             /* my redistributor, by my MPIDR */
    ldr  w0, [x11, #0x14]
    bic  w0, w0, #2
    str  w0, [x11, #0x14]          /* GICR_WAKER: awake */
    movz x12, #0x1, lsl #16
    add  x12, x11, x12             /* its SGI_base */
    mov  w0, #0x8000000
    str  w0, [x12, #0x80]          /* GICR_IGROUPR0: PPI 27 in group 1 */
    str  w0, [x12, #0x100]         /* GICR_ISENABLER0: PPI 27 on */
    mov  x0, #1
    msr  icc_sre_el1, x0
    isb
    mov  x0, #0xF0
    msr  icc_pmr_el1, x0
    mov  x0, #1
    msr  icc_igrpen1_el1, x0
    isb
    msr  daifclr, #2
    mrs  x1, cntvct_el0
    mov  x0, #1
    hvc  #0                        /* ready, with the time */

    mrs  x0, cntfrq_el0
    lsr  x0, x0, #6                /* about 15 ms at any frequency */
    msr  cntv_tval_el0, x0
    mov  x0, #1                    /* ENABLE */
    msr  cntv_ctl_el0, x0
    isb
1:  mrs  x1, cntvct_el0
    mov  x0, #2
    hvc  #0                        /* heartbeat, with the time */
    b    1b

irq_spx:
    mrs  x0, icc_iar1_el1
    mrs  x1, cntv_ctl_el0
    mov  x2, #2                    /* IMASK: the condition still holds, so mask it */
    msr  cntv_ctl_el0, x2
    isb
    hvc  #0
    msr  icc_eoir1_el1, x0
    eret

.macro ventry label
    .balign 0x80
    b \label
.endm

    .balign 2048
vectors:
    ventry .
    ventry .
    ventry .
    ventry .
    ventry .                       /* 0x200 current EL, SPx: sync */
    ventry irq_spx                 /* 0x280 IRQ */
    ventry .
    ventry .
    ventry .
    ventry .
    ventry .
    ventry .
    ventry .
    ventry .
    ventry .
    ventry .
