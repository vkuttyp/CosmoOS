/*
 * crt0.S - Program entry for CosmoOS user programs.
 *
 * The kernel enters at _start with the System V initial stack:
 *   rsp -> argc, argv[0..argc-1], NULL, envp[0..], NULL, auxv...
 * and every general register zero. We pass argc/argv/envp to main and
 * exit with its return value.
 */

    .section .text, "ax", @progbits
    .globl _start
    .type _start, @function
_start:
    xorl %ebp, %ebp
    movq (%rsp), %rdi            /* argc */
    leaq 8(%rsp), %rsi           /* argv */
    leaq 16(%rsp, %rdi, 8), %rdx /* envp = argv + argc + 1 */
    andq $-16, %rsp              /* ABI: 16-byte alignment at call */
    call main
    movl %eax, %edi
    movl $0, %eax                /* SYS_exit */
    syscall
1:  jmp 1b
    .size _start, . - _start

    .section .note.GNU-stack, "", @progbits
