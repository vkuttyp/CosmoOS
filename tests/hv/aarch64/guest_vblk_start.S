/*
 * guest_vblk_start.S - flat entry for the virtio-blk C guest
 * (docs/kernel-services/virtualization/testing.md). Linked at 0x1000 like
 * the assembly guests: set a stack, call main, loop if it returns.
 */
    .text
    .globl _start
_start:
    adrp x0, stack_top
    add  x0, x0, :lo12:stack_top
    mov  sp, x0
    bl   guest_main
1:  b    1b
    .bss
    .balign 16
stack: .space 8192
stack_top:
