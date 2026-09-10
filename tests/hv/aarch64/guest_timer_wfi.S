/*
 * guest_timer_wfi.S - A guest that arms its timer and waits for it
 * (docs/kernel-services/virtualization/testing.md).
 *
 * The shape a real guest kernel's idle loop has: arm the timer, WFI, be
 * woken by it. Unlike guest_irq.S this guest *can* wait in WFI, because
 * nothing is pending-but-masked; the WFI either traps (nothing to wake
 * it: a WFI exit) or the interrupt is taken first.
 *
 *   hvc  x0 = 1, x1 = CNTVCT, x2 = CVAL    "armed", with the deadline
 *   loop: wfi
 *   IRQ -> handler:
 *       x0 = ICC_IAR1_EL1                   the INTID
 *       x1 = CNTVCT_EL0                     when the handler ran
 *       x2 = CNTV_CVAL_EL0                  when it was asked to
 *       msr CNTV_CTL_EL0, IMASK
 *       hvc                                 lateness is x1 - x2, in the guest's own ticks
 *       msr ICC_EOIR1_EL1, x0
 *       eret
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

    mrs  x0, cntfrq_el0
    lsr  x0, x0, #6                /* about 15 ms */
    msr  cntv_tval_el0, x0
    mov  x0, #1
    msr  cntv_ctl_el0, x0
    isb
    mrs  x1, cntvct_el0
    mrs  x2, cntv_cval_el0
    mov  x0, #1
    hvc  #0                        /* armed */
1:  wfi
    b    1b

irq_spx:
    mrs  x0, icc_iar1_el1
    mrs  x1, cntvct_el0
    mrs  x2, cntv_cval_el0
    mov  x3, #2
    msr  cntv_ctl_el0, x3          /* IMASK */
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
    ventry .
    ventry irq_spx                 /* 0x280: current EL, SPx, IRQ */
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
