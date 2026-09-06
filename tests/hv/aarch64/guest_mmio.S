/*
 * guest_mmio.S - A guest touching guest-physical memory that is not
 * there (docs/kernel-services/virtualization/testing.md).
 *
 * The VM has memory at 0 and nothing at 0x4000_0000, so the store below
 * takes a stage-2 data abort, which the backend reports as an MMIO exit
 * with the faulting address and the direction. A load follows, to show
 * the direction is not guessed.
 */
    .text
    .globl _start
_start:
    movz x0, #0x4000, lsl #16      /* 0x40000000: no stage-2 mapping */
    movz x1, #0xABCD
    str x1, [x0]                   /* MMIO exit: write */
    ldr x2, [x0]                   /* MMIO exit: read */
    wfi
1:  b 1b
