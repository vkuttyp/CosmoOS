/*
 * guest_gicc.S - A guest that configures its interrupt controller
 * (docs/kernel-services/virtualization/testing.md).
 *
 * The configuration a GIC driver makes, and nothing else -- no CPU
 * interface, no interrupts, so it runs on any host. The distributor is
 * switched on; one SPI is given a group, a priority (by a whole-word
 * write, as drivers do), a route (to this CPU's affinity, 64 bits) and
 * an enable; this CPU's redistributor is woken and its timer PPI given a
 * group, a priority (by a byte write, as drivers also do) and an enable.
 * Then everything is read back and reported, the SPI's enable is taken
 * away again, and what is left is reported once more. The point is that
 * the register file returns what was written through each of its
 * access sizes, and that a set/clear pair behaves as one.
 *
 *   in:  x8 = the SPI to use (40 when 0)
 *   hvc x0 = 3: x1 = GICD_ISENABLER word, x2 = GICD_IPRIORITYR word,
 *               x3 = GICR_ISENABLER0, x4 = GICR_IPRIORITYR word 24..27 (0x90332211),
 *               x6 = GICD_IROUTER (64-bit), x7 = GICD_IGROUPR word,
 *               x9 = GICR_WAKER, x15 = GICD_CTLR
 *   hvc x0 = 4: x1 = GICD_ISENABLER word after ICENABLER, x3 = GICR_ISENABLER0
 */
    .text
    .globl _start
_start:
    cbnz x8, 1f
    mov  x8, #40
1:  mrs  x5, mpidr_el1
    and  x9, x5, #0xFF
    movz x10, #0x0800, lsl #16     /* GICD */
    movz x11, #0x080A, lsl #16     /* GICR frame 0 */
    lsl  x12, x9, #17
    add  x11, x11, x12             /* my RD_base */
    movz x13, #0x1, lsl #16
    add  x13, x11, x13             /* my SGI_base */

    mov  w0, #0x13
    str  w0, [x10]                 /* GICD_CTLR: ARE, group 1, group 0 */

    lsr  x14, x8, #5
    lsl  x14, x14, #2              /* the SPI's word offset */
    and  x15, x8, #31
    mov  w16, #1
    lsl  w16, w16, w15             /* the SPI's bit */
    add  x17, x10, #0x80
    str  w16, [x17, x14]           /* GICD_IGROUPR: group 1 */
    add  x17, x10, #0x400
    and  x18, x8, #0xFFFFFFFFFFFFFFFC
    ldr  w0, [x17, x18]
    and  w19, w8, #3
    lsl  w19, w19, #3              /* its byte lane */
    mov  w20, #0xA0
    lsl  w20, w20, w19
    orr  w0, w0, w20
    str  w0, [x17, x18]            /* GICD_IPRIORITYR: the whole word */
    movz x17, #0x6000
    add  x17, x10, x17
    lsl  x20, x8, #3
    and  x0, x5, #0xFFFFFF
    str  x0, [x17, x20]            /* GICD_IROUTER: to me, all 64 bits */
    add  x17, x10, #0x100
    str  w16, [x17, x14]           /* GICD_ISENABLER */

    ldr  w0, [x11, #0x14]          /* GICR_WAKER */
    bic  w0, w0, #2                /* ProcessorSleep off */
    str  w0, [x11, #0x14]
    mov  x21, #1000
2:  ldr  w0, [x11, #0x14]
    tbz  w0, #2, 3f                /* until ChildrenAsleep clears */
    subs x21, x21, #1
    b.ne 2b
3:  mov  w0, #0x8000000
    str  w0, [x13, #0x80]          /* GICR_IGROUPR0: PPI 27 in group 1 */
    add  x17, x13, #0x400
    movz w0, #0x2211
    movk w0, #0x4433, lsl #16
    str  w0, [x17, #24]            /* GICR_IPRIORITYR 24..27: four lanes, all different */
    mov  w0, #0x90
    strb w0, [x17, #27]            /* then PPI 27 alone, one byte: its neighbours must keep theirs */
    mov  w0, #0x8000000
    str  w0, [x13, #0x100]         /* GICR_ISENABLER0 */

    add  x17, x10, #0x100
    ldr  w1, [x17, x14]
    add  x17, x10, #0x400
    ldr  w2, [x17, x18]
    ldr  w3, [x13, #0x100]
    add  x17, x13, #0x400
    ldr  w4, [x17, #24]
    movz x17, #0x6000
    add  x17, x10, x17
    ldr  x6, [x17, x20]
    add  x17, x10, #0x80
    ldr  w7, [x17, x14]
    ldr  w9, [x11, #0x14]
    ldr  w15, [x10]
    mov  x0, #3
    hvc  #0

    add  x17, x10, #0x180
    str  w16, [x17, x14]           /* GICD_ICENABLER: take it back */
    add  x17, x10, #0x100
    ldr  w1, [x17, x14]
    ldr  w3, [x13, #0x100]
    mov  x0, #4
    hvc  #0
4:  mov  x0, #2
    hvc  #0
    b    4b
