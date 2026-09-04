/*
 * cosmo/syscall.h - Raw system-call wrappers for CosmoOS user programs.
 *
 * The first piece of the native libc: the numbers come from the kernel's
 * uapi header, the calling convention is the x86-64 SYSCALL ABI. Higher
 * layers (errno, stdio) build on these.
 */

#ifndef COSMO_SYSCALL_H
#define COSMO_SYSCALL_H

#include <stddef.h>
#include <stdint.h>

#include <uapi/cosmo/syscall.h>

static inline long cosmo_syscall6(long nr, long a1, long a2, long a3, long a4, long a5, long a6)
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

#define cosmo_syscall0(n)                     cosmo_syscall6((n), 0, 0, 0, 0, 0, 0)
#define cosmo_syscall1(n, a)                  cosmo_syscall6((n), (long)(a), 0, 0, 0, 0, 0)
#define cosmo_syscall2(n, a, b)               cosmo_syscall6((n), (long)(a), (long)(b), 0, 0, 0, 0)
#define cosmo_syscall3(n, a, b, c)            cosmo_syscall6((n), (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define cosmo_syscall4(n, a, b, c, d)         cosmo_syscall6((n), (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)

static inline void cosmo_exit(int status) __attribute__((noreturn));
static inline void cosmo_exit(int status)
{
    for (;;)
        cosmo_syscall1(SYS_exit, status);
}

static inline long cosmo_write(int h, const void *buf, size_t len)
{
    return cosmo_syscall3(SYS_write, h, buf, len);
}

static inline long cosmo_read(int h, void *buf, size_t len)
{
    return cosmo_syscall3(SYS_read, h, buf, len);
}

static inline long cosmo_getpid(void)
{
    return cosmo_syscall0(SYS_getpid);
}

static inline long cosmo_yield(void)
{
    return cosmo_syscall0(SYS_yield);
}

static inline long cosmo_sleep_ns(uint64_t ns)
{
    return cosmo_syscall1(SYS_sleep_ns, ns);
}

static inline uint64_t cosmo_clock_ns(void)
{
    return (uint64_t)cosmo_syscall0(SYS_clock_ns);
}

static inline long cosmo_mmap(void *hint, size_t len, int prot, int flags)
{
    return cosmo_syscall4(SYS_mmap, hint, len, prot, flags);
}

static inline long cosmo_munmap(void *addr, size_t len)
{
    return cosmo_syscall2(SYS_munmap, addr, len);
}

static inline long cosmo_log(const char *s, size_t len)
{
    return cosmo_syscall2(SYS_log, s, len);
}

static inline long cosmo_close(int h)
{
    return cosmo_syscall1(SYS_close, h);
}

#endif /* COSMO_SYSCALL_H */
