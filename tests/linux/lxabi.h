/*
 * lxabi.h - Raw Linux x86-64 system calls for the freestanding test
 * programs (no libc, no crt0, therefore no CosmoOS note: the kernel runs
 * them under the Linux personality). Numbers and layouts come from
 * compat/linux/linux_abi.h, the same file the kernel side uses.
 */
#ifndef LXABI_H
#define LXABI_H

#include <stddef.h>
#include <stdint.h>

#include "../../compat/linux/linux_abi.h"

static inline long lx_syscall6(long nr, long a1, long a2, long a3, long a4, long a5, long a6)
{
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8 __asm__("r8") = a5;
    register long r9 __asm__("r9") = a6;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}
#define sc0(n) lx_syscall6((n), 0, 0, 0, 0, 0, 0)
#define sc1(n, a) lx_syscall6((n), (long)(a), 0, 0, 0, 0, 0)
#define sc2(n, a, b) lx_syscall6((n), (long)(a), (long)(b), 0, 0, 0, 0)
#define sc3(n, a, b, c) lx_syscall6((n), (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define sc4(n, a, b, c, d) lx_syscall6((n), (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)
#define sc6(n, a, b, c, d, e, f) lx_syscall6((n), (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), (long)(f))

static inline size_t lx_strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

static inline void lx_puts(const char *s)
{
    sc3(LX_write, 1, s, lx_strlen(s));
}

static inline void lx_exit(int code)
{
    for (;;)
        sc1(LX_exit_group, code);
}

/* The program entry: the System V stack has argc at rsp; main(argc, argv). */
int main(int argc, char **argv);
__asm__(".section .text\n"
        ".globl _start\n"
        "_start:\n"
        "    xorl %ebp, %ebp\n"
        "    movq (%rsp), %rdi\n"
        "    leaq 8(%rsp), %rsi\n"
        "    andq $-16, %rsp\n"
        "    call main\n"
        "    movl %eax, %edi\n"
        "    movl $231, %eax\n"
        "    syscall\n"
        "1:  jmp 1b\n"
        ".section .note.GNU-stack,\"\",@progbits\n");

#endif
