/*
 * guest_uart_wfi.S - A guest that waits to be typed at
 * (docs/kernel-services/virtualization/testing.md).
 *
 * The same controller and UART set-up as guest_uart_rx.S, then a timer
 * armed two seconds out -- so that a WFI has a deadline and vcpu_run
 * *waits* in it rather than returning at once -- and a loop of WFI. A
 * byte written by the owner while the vCPU waits must end that wait
 * long before the two seconds are up; the handler reports it.
 *
 *   hvc 1: ready
 *   hvc 2: woken from a WFI (x1 = handler count)
 *   handler: x0 = INTID, x1 = MIS, x2 = DR, x3 = FR
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
    /* SPI 33, the UART's: group 1, priority 0xA0, routed to me, enabled */
    mov  w0, #2
    str  w0, [x10, #0x84]          /* GICD_IGROUPR1 bit 1 */
    mov  w0, #0xA0
    add  x13, x10, #0x400
    strb w0, [x13, #33]            /* GICD_IPRIORITYR[33] */
    movz x13, #0x6000
    add  x13, x10, x13
    and  x0, x9, #0xFF
    str  x0, [x13, #264]           /* GICD_IROUTER[33] = 33 * 8 */
    mov  w0, #2
    str  w0, [x10, #0x104]         /* GICD_ISENABLER1 bit 1 */
    mov  x0, #1
    msr  icc_sre_el1, x0
    isb
    mov  x0, #0xF0
    msr  icc_pmr_el1, x0
    mov  x0, #1
    msr  icc_igrpen1_el1, x0
    isb
    /* The UART: enable it, and ask to be interrupted when a byte arrives. */
    movz x14, #0x0900, lsl #16     /* UART0 */
    mov  w0, #0x301
    str  w0, [x14, #0x30]          /* CR: UARTEN, TXE, RXE */
    mov  w0, #0x10
    str  w0, [x14, #0x38]          /* IMSC: RXIM */
    msr  daifclr, #2
    mov  x22, #0
    mrs  x0, cntfrq_el0
    lsl  x0, x0, #1                /* two seconds */
    msr  cntv_tval_el0, x0
    mov  x0, #1
    msr  cntv_ctl_el0, x0          /* armed, so a WFI has something to wait for */
    isb
    mov  x0, #1
    hvc  #0                        /* ready */
2:  wfi
    mov  x1, x22
    mov  x0, #2
    hvc  #0
    b    2b

irq_spx:
    mrs  x0, icc_iar1_el1
    cmp  x0, #33
    b.ne 1f
    ldr  w1, [x14, #0x40]          /* MIS, as the handler first sees it */
    ldr  w2, [x14]                 /* DR: one byte, and only one */
    ldr  w3, [x14, #0x18]          /* FR, after it */
1:  add  x22, x22, #1
    hvc  #0                        /* x0 = INTID, x1 = MIS, x2 = the byte, x3 = FR */
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
