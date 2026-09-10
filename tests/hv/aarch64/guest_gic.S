/*
 * guest_gic.S - A guest that drives its interrupt controller the
 * ordinary way (docs/kernel-services/virtualization/testing.md).
 *
 * What a kernel does at boot, in the order it does it: the distributor
 * on with affinity routing; its own redistributor found by its MPIDR and
 * woken; the timer PPI grouped, prioritised and enabled there; one SPI
 * grouped, prioritised, routed to this CPU and enabled in the
 * distributor; then the CPU interface. After that the timer is armed and
 * its interrupt taken through the controller so configured. Then the
 * phases that only a distributor can pass:
 *
 *   hvc 1              configured, ready (x1 = CNTVCT)
 *   hvc 2 ...          heartbeats until the timer's handler has run
 *   hvc 3              PPI disabled in the redistributor, timer re-armed
 *                      (x1 = CNTVCT, x2 = CNTV_CVAL)
 *   hvc 4 ...          heartbeats until well past that deadline
 *   hvc 5              past it: x1 = how many times the handler ran (0)
 *   (PPI re-enabled)
 *   hvc 6 ...          heartbeats until the held expiry arrives
 *   (SPI x8 made pending through GICD_ISPENDR)
 *   hvc 7 ...          heartbeats until it arrives
 *   hvc 8              done
 *   handler:           x0 = ICC_IAR1_EL1, x1 = CNTV_CTL; the timer is
 *                      masked so its level drops; x22 counts the runs
 *
 *   in: x8 = the SPI to use (40 when 0)
 */
    .text
    .globl _start
_start:
    adr  x0, vectors
    msr  vbar_el1, x0
    cbnz x8, 1f
    mov  x8, #40
1:  movz x10, #0x0800, lsl #16     /* GICD */
    mov  w0, #0x13
    str  w0, [x10]                 /* GICD_CTLR: ARE, group 1, group 0 */
    mrs  x11, mpidr_el1
    and  x9, x11, #0xFF
    lsl  x11, x9, #17
    movz x12, #0x080A, lsl #16
    add  x11, x11, x12             /* my redistributor */
    ldr  w0, [x11, #0x14]
    bic  w0, w0, #2
    str  w0, [x11, #0x14]          /* GICR_WAKER: ProcessorSleep off */
    mov  x21, #1000
2:  ldr  w0, [x11, #0x14]
    tbz  w0, #2, 3f                /* until ChildrenAsleep clears */
    subs x21, x21, #1
    b.ne 2b
3:  movz x12, #0x1, lsl #16
    add  x12, x11, x12             /* its SGI_base */
    mov  w0, #0x8000000
    str  w0, [x12, #0x80]          /* GICR_IGROUPR0: PPI 27 in group 1 */
    mov  w0, #0xA0
    add  x13, x12, #0x400
    strb w0, [x13, #27]            /* GICR_IPRIORITYR: PPI 27 */
    mov  w0, #0x8000000
    str  w0, [x12, #0x100]         /* GICR_ISENABLER0: PPI 27 on */

    lsr  x14, x8, #5
    lsl  x14, x14, #2              /* the SPI's word */
    and  x15, x8, #31
    mov  w16, #1
    lsl  w16, w16, w15             /* the SPI's bit */
    add  x13, x10, #0x80
    str  w16, [x13, x14]           /* GICD_IGROUPR: group 1 */
    mov  w0, #0xA0
    add  x13, x10, #0x400
    strb w0, [x13, x8]             /* GICD_IPRIORITYR: the SPI */
    movz x13, #0x6000
    add  x13, x10, x13
    lsl  x17, x8, #3
    and  x0, x9, #0xFF
    str  x0, [x13, x17]            /* GICD_IROUTER: to me */
    add  x13, x10, #0x100
    str  w16, [x13, x14]           /* GICD_ISENABLER */

    mov  x0, #1
    msr  icc_sre_el1, x0           /* the CPU interface, last */
    isb
    mov  x0, #0xF0
    msr  icc_pmr_el1, x0
    mov  x0, #1
    msr  icc_igrpen1_el1, x0
    isb
    msr  daifclr, #2
    mov  x22, #0
    mrs  x1, cntvct_el0
    mov  x0, #1
    hvc  #0                        /* ready */

    /* Phase A: arm about 15 ms out; heartbeat until the handler has run. */
    mrs  x0, cntfrq_el0
    lsr  x0, x0, #6
    msr  cntv_tval_el0, x0
    mov  x0, #1
    msr  cntv_ctl_el0, x0
    isb
4:  mrs  x1, cntvct_el0
    mov  x0, #2
    hvc  #0
    cbz  x22, 4b

    /* Phase B: the PPI off in the redistributor, the timer re-armed, and
     * heartbeats until about 8 ms past its deadline. */
    mov  w0, #0x8000000
    str  w0, [x12, #0x180]         /* GICR_ICENABLER0: PPI 27 off */
    mov  x22, #0
    mrs  x0, cntfrq_el0
    lsr  x0, x0, #6
    msr  cntv_tval_el0, x0
    mov  x0, #1
    msr  cntv_ctl_el0, x0          /* ENABLE, IMASK clear again */
    isb
    mrs  x20, cntv_cval_el0
    mrs  x1, cntvct_el0
    mov  x2, x20
    mov  x0, #3
    hvc  #0                        /* re-armed, disabled */
    mrs  x21, cntfrq_el0
    lsr  x21, x21, #7
    add  x21, x20, x21             /* the deadline plus a grace */
5:  mrs  x1, cntvct_el0
    mov  x0, #4
    hvc  #0
    cmp  x1, x21
    b.lo 5b
    mov  x1, x22
    mov  x0, #5
    hvc  #0                        /* past the deadline: x1 = handler runs */

    /* Phase C: on again; the held expiry comes through. */
    mov  w0, #0x8000000
    str  w0, [x12, #0x100]         /* GICR_ISENABLER0 */
6:  mrs  x1, cntvct_el0
    mov  x0, #6
    hvc  #0
    cbz  x22, 6b

    /* Phase D: the SPI, made pending by hand. */
    mov  x22, #0
    add  x13, x10, #0x200
    str  w16, [x13, x14]           /* GICD_ISPENDR */
7:  mov  x0, #7
    hvc  #0
    cbz  x22, 7b
8:  mov  x0, #8
    hvc  #0
    b    8b

irq_spx:
    mrs  x0, icc_iar1_el1
    cmp  x0, #27
    b.ne 1f
    mrs  x1, cntv_ctl_el0
    orr  x1, x1, #2
    msr  cntv_ctl_el0, x1          /* mask the timer: the level drops */
1:  add  x22, x22, #1
    mrs  x1, cntv_ctl_el0
    hvc  #0                        /* x0 = the INTID taken */
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
    ventry irq_spx                 /* current EL, SPx: IRQ */
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
