/*
 * guest_uart_poll.S - A guest that both polls its UART and is interrupted
 * by it (docs/kernel-services/virtualization/testing.md).
 *
 * The same set-up as guest_uart_rx.S, then a loop that reads FR and, when
 * a byte waits, takes it from DR -- while RXIM is unmasked, so the same
 * byte may instead arrive by interrupt. Every iteration heartbeats. The
 * point is the race between an owner typing and a guest draining: a
 * transition told to the distributor out of order shows up here as an
 * interrupt whose MIS reads zero, and the handler reports MIS.
 *
 *   hvc 1: ready
 *   hvc 2: heartbeat, x1 = interrupts taken, x2 = bytes consumed (both paths)
 *   handler: x0 = INTID, x1 = MIS, x2 = DR (if a byte waited), x3 = FR
 */
    .text
    .globl _start
_start:
    adr  x0, vectors
    msr  vbar_el1, x0
    movz x10, #0x0800, lsl #16
    mov  w0, #0x13
    str  w0, [x10]
    mrs  x11, mpidr_el1
    and  x9, x11, #0xFF
    lsl  x11, x9, #17
    movz x12, #0x080A, lsl #16
    add  x11, x11, x12
    ldr  w0, [x11, #0x14]
    bic  w0, w0, #2
    str  w0, [x11, #0x14]
    mov  w0, #2
    str  w0, [x10, #0x84]
    mov  w0, #0xA0
    add  x13, x10, #0x400
    strb w0, [x13, #33]
    movz x13, #0x6000
    add  x13, x10, x13
    and  x0, x9, #0xFF
    str  x0, [x13, #264]
    mov  w0, #2
    str  w0, [x10, #0x104]
    mov  x0, #1
    msr  icc_sre_el1, x0
    isb
    mov  x0, #0xF0
    msr  icc_pmr_el1, x0
    mov  x0, #1
    msr  icc_igrpen1_el1, x0
    isb
    movz x14, #0x0900, lsl #16
    mov  w0, #0x301
    str  w0, [x14, #0x30]
    mov  w0, #0x10
    str  w0, [x14, #0x38]
    msr  daifclr, #2
    mov  x22, #0
    mov  x23, #0
    mov  x0, #1
    hvc  #0
2:  msr  daifset, #2               /* the poll is FR then DR: an interrupt between them
                                      would take the byte and leave the poll counting an
                                      empty read -- so, as any driver does, mask around it */
    ldr  w0, [x14, #0x18]          /* FR */
    tbnz w0, #4, 3f                /* RXFE: nothing to take */
    ldr  w1, [x14]                 /* DR: take one by polling */
    cbz  w1, 3f                    /* a sibling CPU took it first: an empty read is 0, and no
                                      byte typed is ever 0 -- masking cannot exclude another CPU */
    add  x23, x23, #1
3:  msr  daifclr, #2
    mov  x1, x22
    mov  x2, x23
    mov  x0, #2
    hvc  #0
    b    2b

irq_spx:
    mrs  x0, icc_iar1_el1
    cmp  x0, #33
    b.ne 1f
    ldr  w1, [x14, #0x40]          /* MIS: zero here is a spurious interrupt */
    ldr  w3, [x14, #0x18]
    tbnz w3, #4, 1f
    ldr  w2, [x14]                 /* a byte waited: take it */
    cbz  w2, 1f                    /* unless a sibling CPU took it between FR and DR */
    add  x23, x23, #1
1:  add  x22, x22, #1
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
