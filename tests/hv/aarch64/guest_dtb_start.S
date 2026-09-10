/*
 * guest_dtb_start.S - The entry of the tree's first C guest
 * (docs/kernel-services/virtualization/testing.md).
 *
 * The first 64 bytes are an arm64 Image header, as the boot protocol
 * defines it, so that what loads a kernel image loads this the same way:
 * code0 branches over the header, text_offset says where the image goes
 * from a 2 MiB base, image_size bounds it (generously: the stacks live in
 * .bss past the file), the flags say 4 KiB pages and "any 2 MiB base",
 * and the magic at 56 says ARM\x64. The image is linked at
 * COSMO_HVM_RAM_BASE + text_offset and expects to be loaded there.
 *
 * _start keeps x0 -- the device tree the boot protocol hands over -- and
 * gives guest_main a stack. secondary_start is what CPU_ON is asked to
 * start the second vCPU at; it keeps x0 -- the context the caller gave.
 */
    .text
    .globl _head
_head:
    b    _start                    /* code0 */
    .long 0                        /* code1 */
    .quad 0x80000                  /* text_offset */
    .quad 0x40000                  /* image_size: 256 KiB, a bound */
    .quad 0xA                      /* flags: 4 KiB pages, any 2 MiB base */
    .quad 0, 0, 0                  /* res2, res3, res4 */
    .ascii "ARM\x64"               /* magic, at 56 */
    .long 0                        /* res5 */

    .globl _start
_start:
    adrp x9, stack0_top
    add  x9, x9, :lo12:stack0_top
    mov  sp, x9
    bl   guest_main                /* x0 = the device tree */
1:  b    1b

    .globl secondary_start
secondary_start:
    adrp x9, stack1_top
    add  x9, x9, :lo12:stack1_top
    mov  sp, x9
    bl   secondary_main            /* x0 = the context CPU_ON carried */
1:  b    1b

    .bss
    .balign 16
stack0: .space 8192
stack0_top:
stack1: .space 4096
stack1_top:
