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


/* Phase 7: files. */
static inline long cosmo_open(const char *path, int flags, unsigned mode)
{
    return cosmo_syscall3(SYS_open, path, flags, mode);
}
static inline long cosmo_stat(const char *path, struct cosmo_stat *st)
{
    return cosmo_syscall2(SYS_stat, path, st);
}
static inline long cosmo_fstat(int h, struct cosmo_stat *st)
{
    return cosmo_syscall2(SYS_fstat, h, st);
}
static inline long cosmo_lseek(int h, long off, int whence)
{
    return cosmo_syscall3(SYS_lseek, h, off, whence);
}
static inline long cosmo_mkdir(const char *path, unsigned mode)
{
    return cosmo_syscall2(SYS_mkdir, path, mode);
}
static inline long cosmo_unlink(const char *path)
{
    return cosmo_syscall1(SYS_unlink, path);
}
static inline long cosmo_rmdir(const char *path)
{
    return cosmo_syscall1(SYS_rmdir, path);
}
static inline long cosmo_rename(const char *oldp, const char *newp)
{
    return cosmo_syscall2(SYS_rename, oldp, newp);
}
static inline long cosmo_getdents(int h, void *buf, size_t len)
{
    return cosmo_syscall3(SYS_getdents, h, buf, len);
}
static inline long cosmo_sync(void)
{
    return cosmo_syscall0(SYS_sync);
}
static inline long cosmo_mount(const char *source, const char *target, const char *fstype, unsigned flags)
{
    return cosmo_syscall4(SYS_mount, source, target, fstype, flags);
}
static inline long cosmo_umount2(const char *target, unsigned flags)
{
    return cosmo_syscall2(SYS_umount, target, flags);
}
static inline long cosmo_umount(const char *target)
{
    return cosmo_umount2(target, 0);
}


/* Phase 8: sockets. */
#define cosmo_syscall5(n, a, b, c, d, e) cosmo_syscall6((n), (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), 0)
static inline long cosmo_socket(int family, int type, int proto)
{
    return cosmo_syscall3(SYS_socket, family, type, proto);
}
static inline long cosmo_bind(int h, const struct cosmo_sockaddr *sa)
{
    return cosmo_syscall3(SYS_bind, h, sa, sizeof(*sa));
}
static inline long cosmo_listen(int h, int backlog)
{
    return cosmo_syscall2(SYS_listen, h, backlog);
}
static inline long cosmo_accept(int h, struct cosmo_sockaddr *peer, size_t *len)
{
    return cosmo_syscall3(SYS_accept, h, peer, len);
}
static inline long cosmo_connect(int h, const struct cosmo_sockaddr *sa)
{
    return cosmo_syscall3(SYS_connect, h, sa, sizeof(*sa));
}
static inline long cosmo_sendto(int h, const void *buf, size_t len, const struct cosmo_sockaddr *to)
{
    return cosmo_syscall5(SYS_sendto, h, buf, len, to, to ? sizeof(*to) : 0);
}
static inline long cosmo_recvfrom(int h, void *buf, size_t len, struct cosmo_sockaddr *from, size_t *fromlen)
{
    return cosmo_syscall5(SYS_recvfrom, h, buf, len, from, fromlen);
}
static inline long cosmo_shutdown(int h, int how)
{
    return cosmo_syscall2(SYS_shutdown, h, how);
}
static inline long cosmo_getsockname(int h, struct cosmo_sockaddr *sa, size_t *len)
{
    return cosmo_syscall3(SYS_getsockname, h, sa, len);
}

#endif /* COSMO_SYSCALL_H */
