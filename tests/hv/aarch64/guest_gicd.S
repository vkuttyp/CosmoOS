/*
 * guest_gicd.S - A guest that probes its interrupt controller
 * (docs/kernel-services/virtualization/testing.md).
 *
 * The first thing every GIC driver does, and nothing else: read the
 * distributor's identification, then find the redistributor that is this
 * CPU's by its own MPIDR. No CPU interface, no interrupts, so it runs on
 * any host; where there is no distributor the first load faults to the
 * owner as MMIO, and that is the exit the test refuses to see.
 *
 *   in:  x8 = an SPI whose enable word to report, or 0
 *   hvc x0 = 7: x1 = GICD_TYPER, x2 = GICD_IIDR, x3 = GICD_PIDR2,
 *               x4 = GICR_TYPER of my frame, x5 = MPIDR_EL1,
 *               x6 = GICD_ISENABLER word holding x8, x7 = my GICR_ISENABLER0
 */
    .text
    .globl _start
_start:
    mrs  x5, mpidr_el1
    and  x9, x5, #0xFF             /* Aff0: which frame is mine */
    movz x10, #0x0800, lsl #16     /* GICD */
    ldr  w1, [x10, #0x04]          /* TYPER */
    ldr  w2, [x10, #0x08]          /* IIDR */
    movz x11, #0xFFE8
    ldr  w3, [x10, x11]            /* PIDR2 */
    movz x11, #0x080A, lsl #16     /* GICR frame 0 */
    lsl  x12, x9, #17              /* frames are 128 KiB apart */
    add  x11, x11, x12
    ldr  x4, [x11, #0x08]          /* GICR_TYPER, all 64 bits */
    mov  x6, #0
    cbz  x8, 1f
    lsr  x12, x8, #5               /* ISENABLER word for x8 */
    lsl  x12, x12, #2
    add  x12, x12, #0x100
    ldr  w6, [x10, x12]
1:  movz x12, #0x1, lsl #16        /* SGI_base */
    add  x12, x12, #0x100          /* GICR_ISENABLER0 */
    ldr  w7, [x11, x12]
    mov  x0, #7
    hvc  #0
2:  mov  x0, #2
    hvc  #0
    b    2b
