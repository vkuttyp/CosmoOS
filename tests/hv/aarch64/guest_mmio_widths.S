/*
 * guest_mmio_widths.S - Every width and extension a load can have
 * (docs/kernel-services/virtualization/testing.md).
 *
 * Seven loads from one device word, then a halfword store, then a load
 * from an address no device claims so the owner has to answer it. The
 * device the test registers at 0x0A001000 holds 0x80018081F0F1F2F3, and
 * each register below must end up holding what the architecture says
 * that load leaves in it -- the part a completion that merely wrote the
 * value would get wrong.
 *
 *   hvc 1: x1 = ldrb w, x2 = ldrh w, x3 = ldr w, x4 = ldr x;
 *          x5 = ldrsb w, x6 = ldrsh x, x7 = ldrsw x (via regs)
 *   hvc 2: x9 = the owner's answer to `ldr x9, [0x40000000]`
 */
    .text
    .globl _start
_start:
    movz  x0, #0x0A00, lsl #16
    add   x0, x0, #0x1000          /* the test's device */
    ldrb  w1, [x0]
    ldrh  w2, [x0]
    ldr   w3, [x0]
    ldr   x4, [x0]
    ldrsb w5, [x0]
    ldrsh x6, [x0]
    ldrsw x7, [x0]
    movz  w8, #0xBEEF
    strh  w8, [x0, #8]             /* a halfword store: size 2, value 0xBEEF, at +8 */
    mov   x0, #1
    hvc   #0
    movz  x0, #0x4000, lsl #16     /* 0x40000000: no device; the owner answers */
    ldr   x9, [x0]
    mov   x0, #2
    hvc   #0
1:  b     1b
