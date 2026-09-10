/*
 * guest_sgi.S - Two vCPUs of one guest, and an interrupt between them
 * (docs/kernel-services/virtualization/testing.md).
 *
 * Both run this image. Each finds its redistributor by its own MPIDR,
 * wakes it, puts SGI 3 in group 1 with a priority and an enable, brings
 * up its CPU interface, and reports ready with its MPIDR. Then the one
 * whose Aff0 is 0 writes ICC_SGI1R_EL1 naming SGI 3 for Aff0 = 1 -- the
 * secondary-CPU wake-up every SMP kernel does -- and both heartbeat. The
 * handler reports the INTID it acknowledged and whose CPU it ran on.
 *
 *   hvc 1: ready, x1 = MPIDR_EL1
 *   hvc 9: the SGI has been sent (vCPU 0 only)
 *   hvc 2: heartbeat
 *   handler: x0 = ICC_IAR1_EL1, x1 = MPIDR_EL1
 */
    .text
    .globl _start
_start:
    adr  x0, vectors
    msr  vbar_el1, x0
    movz x10, #0x0800, lsl #16     /* GICD */
    mov  w0, #0x13
    str  w0, [x10]                 /* GICD_CTLR */
    mrs  x11, mpidr_el1
    and  x9, x11, #0xFF
    lsl  x11, x9, #17
    movz x12, #0x080A, lsl #16
    add  x11, x11, x12             /* my redistributor */
    ldr  w0, [x11, #0x14]
    bic  w0, w0, #2
    str  w0, [x11, #0x14]          /* awake */
    movz x12, #0x1, lsl #16
    add  x12, x11, x12             /* its SGI_base */
    mov  w0, #8
    str  w0, [x12, #0x80]          /* GICR_IGROUPR0: SGI 3 in group 1 */
    mov  w0, #0xA0
    add  x13, x12, #0x400
    strb w0, [x13, #3]             /* GICR_IPRIORITYR: SGI 3 */
    mov  w0, #8
    str  w0, [x12, #0x100]         /* GICR_ISENABLER0: SGI 3 on */
    mov  x0, #1
    msr  icc_sre_el1, x0
    isb
    mov  x0, #0xF0
    msr  icc_pmr_el1, x0
    mov  x0, #1
    msr  icc_igrpen1_el1, x0
    isb
    msr  daifclr, #2
    mrs  x1, mpidr_el1
    mov  x0, #1
    hvc  #0                        /* ready */
    cbnz x9, 1f
    /* vCPU 0: SGI 3 to Aff0 = 1 -- INTID at 24, target list bit 1. */
    movz x0, #0x0300, lsl #16
    orr  x0, x0, #2
    msr  icc_sgi1r_el1, x0
    isb
    mov  x0, #9
    hvc  #0                        /* sent */
1:  mov  x0, #2
    hvc  #0                        /* heartbeat */
    b    1b

irq_spx:
    mrs  x0, icc_iar1_el1
    mrs  x1, mpidr_el1
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
    ventry irq_spx
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
