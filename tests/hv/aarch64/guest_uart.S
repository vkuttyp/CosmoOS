/*
 * guest_uart.S - A guest that prints
 * (docs/kernel-services/virtualization/testing.md).
 *
 * What an earlycon does: wait for the transmitter (FR.TXFF clear), store
 * a byte to DR, repeat. Six bytes, then the flag register and the
 * identification registers are read and reported, so the test can see
 * both that the bytes arrived and that a driver checking for a PL011
 * would find one.
 *
 *   hvc 1: x1 = FR, x2 = PeriphID0, x3 = PeriphID1, x4 = PCellID3
 */
    .text
    .globl _start
_start:
    movz x10, #0x0900, lsl #16     /* 0x09000000: UART0 on virt */
    adr  x11, msg
    mov  x12, #6
1:  ldr  w0, [x10, #0x18]          /* FR */
    tbnz w0, #5, 1b                /* TXFF: wait, as a driver would */
    ldrb w0, [x11], #1
    str  w0, [x10]                 /* DR */
    subs x12, x12, #1
    b.ne 1b
    ldr  w1, [x10, #0x18]          /* FR */
    ldr  w2, [x10, #0xFE0]         /* PeriphID0 */
    ldr  w3, [x10, #0xFE4]         /* PeriphID1 */
    ldr  w4, [x10, #0xFFC]         /* PCellID3 */
    mov  x0, #1
    hvc  #0
2:  b    2b

msg:
    .ascii "hello\n"
