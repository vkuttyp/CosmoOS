/*
 * crt0.S - Program entry for CosmoOS user programs on AArch64 (docs/libc/design.md).
 *
 * The kernel enters at _start with the System V initial stack:
 *   sp -> argc, argv[0..argc-1], NULL, envp[0..], NULL, auxv...
 * and every general register zero. __libc_start(argc, argv, envp) sets
 * up the library, calls main and exits with its return value.
 */
    .section .text, "ax", @progbits
    .globl _start
    .type _start, @function
_start:
    mov x29, xzr
    mov x30, xzr
    ldr x0, [sp]                 /* argc */
    add x1, sp, #8               /* argv */
    add x2, x1, x0, lsl #3
    add x2, x2, #8               /* envp = argv + argc + 1 */
    mov x9, sp
    and x9, x9, #-16
    mov sp, x9
    bl __libc_start
1:  b 1b
    .size _start, . - _start

    /* The CosmoOS ABI note: its presence selects the native personality
     * (docs/compat/linux/design.md). namesz 8, descsz 4, type 1, name "CosmoOS", desc = ABI version. */
    .section .note.cosmo, "a", @note
    .balign 4
    .long 8, 4, 1
    .asciz "CosmoOS"
    .long 1

    .section .note.GNU-stack, "", @progbits
