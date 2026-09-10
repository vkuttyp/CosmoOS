/*
 * guest_spi.S - A guest that takes an SPI an owner-side device raises
 * (docs/kernel-services/virtualization/testing.md).
 *
 * Brings up its GIC the ordinary way, enables SPI 50 in the distributor
 * (group 1, priority, routed to itself), and heartbeats. When the owner
 * raises SPI 50 through the distributor (vm_raise_spi), the handler runs
 * and reports the INTID. The point is that the *owner* raises it, not the
 * guest (no ISPENDR here): the interrupt comes from outside.
 *
 *   hvc 1: ready
 *   hvc 2: heartbeat (x1 = how many times the handler has run)
 *   handler: x0 = ICC_IAR1_EL1
 */
    .text
    .globl _start
_start:
    adr  x0, vectors
    msr  vbar_el1, x0
    movz x10, #0x0800, lsl #16     /* GICD */
    mov  w0, #0x13
    str  w0, [x10]                 /* GICD_CTLR: ARE, group 1 */
    mrs  x11, mpidr_el1
    and  x9, x11, #0xFF
    lsl  x11, x9, #17
    movz x12, #0x080A, lsl #16
    add  x11, x11, x12             /* my redistributor */
    ldr  w0, [x11, #0x14]
    bic  w0, w0, #2
    str  w0, [x11, #0x14]          /* awake */
    /* SPI 50: word 1 bit 18. group 1, priority 0xA0, routed to me, enabled. */
    mov  w0, #0x40000              /* bit 18 */
    str  w0, [x10, #0x84]          /* GICD_IGROUPR1 */
    mov  w0, #0xA0
    add  x13, x10, #0x400
    strb w0, [x13, #50]            /* GICD_IPRIORITYR[50] */
    movz x13, #0x6000
    add  x13, x10, x13
    and  x0, x9, #0xFF
    str  x0, [x13, #400]           /* GICD_IROUTER[50] = 50*8 */
    mov  w0, #0x40000
    str  w0, [x10, #0x104]         /* GICD_ISENABLER1 */
    mov  x0, #1
    msr  icc_sre_el1, x0
    isb
    mov  x0, #0xF0
    msr  icc_pmr_el1, x0
    mov  x0, #1
    msr  icc_igrpen1_el1, x0
    isb
    msr  daifclr, #2
    mov  x22, #0
    mov  x0, #1
    hvc  #0                        /* ready */
2:  mov  x1, x22
    mov  x0, #2
    hvc  #0
    b    2b

irq_spx:
    mrs  x0, icc_iar1_el1
    add  x22, x22, #1
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
