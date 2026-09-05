/*
 * entry.S - AArch64 kernel entry (docs/kernel/arch/aarch64/design.md).
 *
 * The loader enters at _start at EL1 with the MMU on (TTBR1 holds the
 * higher-half image and the direct map, TTBR0 the loader's identity
 * map), interrupts masked, x0 = the cosmoboot_info pointer (a direct-map
 * address). We move to the kernel's own boot stack, install the vector
 * table and continue in C.
 */

#include <cosmoboot.h>

    .section .note.cosmoboot, "a", @note
    .balign 4
    .long 6                     /* namesz: "COSMO\0" */
    .long 4                     /* descsz */
    .long COSMOBOOT_NOTE_TYPE   /* type */
    .asciz "COSMO"
    .balign 4
    .long COSMOBOOT_VERSION     /* desc */
    .balign 4

    .section .text.entry, "ax", @progbits
    .globl _start
    .type _start, @function
_start:
    msr daifset, #0xF
    mov x19, x0
    adrp x9, aarch64_boot_stack_top
    add x9, x9, :lo12:aarch64_boot_stack_top
    mov sp, x9
    adrp x9, aarch64_vectors
    add x9, x9, :lo12:aarch64_vectors
    msr vbar_el1, x9
    isb
    mov x29, xzr
    mov x30, xzr
    mov x0, x19
    bl aarch64_start
1:  wfi
    b 1b
    .size _start, . - _start

    .section .bss.boot_stack, "aw", @nobits
    .balign 16
    .globl aarch64_boot_stack_bottom
    .globl aarch64_boot_stack_top
aarch64_boot_stack_bottom:
    .skip 65536
aarch64_boot_stack_top:

    .section .note.GNU-stack, "", @progbits
