/*
 * entry.S - x86-64 kernel entry from the bootloader.
 *
 * Entry state is defined by boot/protocol/cosmoboot.h: long mode, paging
 * on with the kernel mapped at its link address, RDI = info pointer,
 * interrupts disabled, a loader-owned stack. The only jobs here are to
 * move onto a kernel-owned stack, terminate the frame chain so
 * backtraces stop cleanly, and call C. Nothing else belongs in assembly.
 */

#include <cosmoboot.h>

/* --- boot protocol note: lets the loader verify the protocol version --- */
    .section .note.cosmoboot, "a", @note
    .balign 4
    .long 6                     /* namesz: "COSMO\0" */
    .long 4                     /* descsz */
    .long COSMOBOOT_NOTE_TYPE   /* type */
    .asciz "COSMO"
    .balign 4
    .long COSMOBOOT_VERSION     /* desc */
    .balign 4

/* --- entry --- */
    .section .text.entry, "ax", @progbits
    .globl _start
    .type _start, @function
_start:
    cli
    cld

    /* Kernel-owned boot stack; the loader's stack is reclaimable. */
    lea x86_boot_stack_top(%rip), %rsp
    xor %ebp, %ebp

    /* Fake outermost frame: return address 0, saved rbp 0. Keeps the
     * stack 16-byte aligned at the call below and ends frame walks. */
    pushq $0
    pushq $0

    /* RDI still holds the boot info pointer. */
    call x86_start

    /* x86_start never returns; belt and braces. */
1:  cli
    hlt
    jmp 1b
    .size _start, . - _start

/* --- boot stack --- */
    .section .bss.boot_stack, "aw", @nobits
    .balign 16
    .globl x86_boot_stack_bottom
    .globl x86_boot_stack_top
x86_boot_stack_bottom:
    .skip 65536
x86_boot_stack_top:

    .section .note.GNU-stack, "", @progbits
