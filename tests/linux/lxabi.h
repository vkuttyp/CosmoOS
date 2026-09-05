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

#if defined(__x86_64__)
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
#elif defined(__aarch64__)
static inline long lx_syscall6(long nr, long a1, long a2, long a3, long a4, long a5, long a6)
{
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a1;
    register long x1 __asm__("x1") = a2;
    register long x2 __asm__("x2") = a3;
    register long x3 __asm__("x3") = a4;
    register long x4 __asm__("x4") = a5;
    register long x5 __asm__("x5") = a6;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
    return x0;
}
#else
#error "lxabi.h: unsupported architecture"
#endif
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

/* Start a thread: clone with `fn(arg)` run on the new stack and its
 * result passed to exit (the thread, not the group). Returns the child's
 * tid or -errno. The argument order of clone differs per architecture
 * (x86-64: flags, stack, ptid, ctid, tls; AArch64: flags, stack, ptid,
 * tls, ctid). */
typedef int (*lx_thread_fn)(void *);
long lx_clone(lx_thread_fn fn, void *stack_top, void *arg, unsigned long flags, int32_t *ptid, int32_t *ctid,
              void *tls);
#if defined(__x86_64__)
__asm__(".text\n"
        ".globl lx_clone\n"
        "lx_clone:\n"
        /* rdi=fn rsi=stack rdx=arg rcx=flags r8=ptid r9=ctid 8(%rsp)=tls */
        "    andq $-16, %rsi\n"
        "    subq $16, %rsi\n"
        "    movq %rdi, 0(%rsi)\n"     /* fn on the child's stack */
        "    movq %rdx, 8(%rsi)\n"     /* arg */
        "    movl $56, %eax\n"          /* clone */
        "    movq %rcx, %rdi\n"         /* flags */
        "    movq %r8, %rdx\n"          /* ptid */
        "    movq %r9, %r10\n"          /* ctid */
        "    movq 8(%rsp), %r8\n"       /* tls */
        "    syscall\n"
        "    testq %rax, %rax\n"
        "    jnz 1f\n"
        "    popq %rax\n"               /* the child: fn */
        "    popq %rdi\n"               /* arg */
        "    call *%rax\n"
        "    movl %eax, %edi\n"
        "    movl $60, %eax\n"          /* exit: this thread only */
        "    syscall\n"
        "    ud2\n"
        "1:  ret\n");
#else
__asm__(".text\n"
        ".globl lx_clone\n"
        "lx_clone:\n"
        /* x0=fn x1=stack x2=arg x3=flags x4=ptid x5=ctid x6=tls */
        "    and x1, x1, #-16\n"
        "    stp x0, x2, [x1, #-16]!\n"   /* fn, arg on the child's stack */
        "    mov x0, x3\n"                /* flags */
        "    mov x2, x4\n"                /* ptid */
        "    mov x3, x6\n"                /* tls */
        "    mov x4, x5\n"                /* ctid */
        "    mov x8, #220\n"              /* clone */
        "    svc #0\n"
        "    cbnz x0, 1f\n"
        "    ldp x1, x0, [sp], #16\n"     /* the child: fn, arg */
        "    blr x1\n"
        "    mov x8, #93\n"               /* exit: this thread only */
        "    svc #0\n"
        "    brk #0\n"
        "1:  ret\n");
#endif

/* The program entry: the stack has argc at sp; main(argc, argv). A program
 * with its own entry (lxinterp) defines LXABI_NO_START. */
#ifndef LXABI_NO_START
int main(int argc, char **argv);
#if defined(__x86_64__)
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
#else
__asm__(".section .text\n"
        ".globl _start\n"
        "_start:\n"
        "    mov x29, #0\n"
        "    mov x30, #0\n"
        "    ldr x0, [sp]\n"
        "    add x1, sp, #8\n"
        "    bl main\n"
        "    mov x8, #94\n"               /* exit_group */
        "    svc #0\n"
        "1:  b 1b\n"
        ".section .note.GNU-stack,\"\",@progbits\n");
#endif
#endif /* LXABI_NO_START */

#endif
