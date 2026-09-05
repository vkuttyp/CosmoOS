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

#if defined(__aarch64__)
/* AArch64: svc #0, number in x8, arguments x0..x5, result x0. */
static inline long cosmo_syscall6(long nr, long a1, long a2, long a3, long a4, long a5, long a6)
{
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a1;
    register long x1 __asm__("x1") = a2;
    register long x2 __asm__("x2") = a3;
    register long x3 __asm__("x3") = a4;
    register long x4 __asm__("x4") = a5;
    register long x5 __asm__("x5") = a6;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                     : "memory");
    return x0;
}
#else
/* x86-64: syscall, number in rax, arguments rdi rsi rdx r10 r8 r9, result rax. */
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
#endif
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
    return (uint64_t)cosmo_syscall1(SYS_clock_ns, COSMO_CLOCK_MONOTONIC);
}
static inline uint64_t cosmo_clock_realtime_ns(void)
{
    return (uint64_t)cosmo_syscall1(SYS_clock_ns, COSMO_CLOCK_REALTIME);
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


/* Phase 9: processes, pipes, the working directory, introspection. */
static inline long cosmo_spawn(const struct cosmo_spawn *req)
{
    return cosmo_syscall1(SYS_spawn, req);
}
static inline long cosmo_wait(int pid, int *status, unsigned flags)
{
    return cosmo_syscall3(SYS_wait, pid, status, flags);
}
static inline long cosmo_kill(int pid, int sig)
{
    return cosmo_syscall2(SYS_kill, pid, sig);
}
static inline long cosmo_pipe(int h[2])
{
    return cosmo_syscall1(SYS_pipe, h);
}
static inline long cosmo_dup(int h, int target)
{
    return cosmo_syscall2(SYS_dup, h, target);
}
static inline long cosmo_getppid(void)
{
    return cosmo_syscall0(SYS_getppid);
}
static inline long cosmo_chdir(const char *path)
{
    return cosmo_syscall1(SYS_chdir, path);
}
static inline long cosmo_getcwd(char *buf, size_t len)
{
    return cosmo_syscall2(SYS_getcwd, buf, len);
}
static inline long cosmo_procinfo(struct cosmo_procinfo *buf, size_t count)
{
    return cosmo_syscall2(SYS_procinfo, buf, count);
}
static inline long cosmo_klog(char *buf, size_t len)
{
    return cosmo_syscall2(SYS_klog, buf, len);
}
static inline long cosmo_sysctl(const char *name, char *buf, size_t len)
{
    return cosmo_syscall3(SYS_sysctl, name, buf, len);
}

/* Credentials: -1 keeps an id. */
static inline long cosmo_setresuid(long ruid, long euid, long suid)
{
    return cosmo_syscall3(SYS_setresuid, ruid, euid, suid);
}
static inline long cosmo_setresgid(long rgid, long egid, long sgid)
{
    return cosmo_syscall3(SYS_setresgid, rgid, egid, sgid);
}
static inline long cosmo_getresuid(uint32_t *ruid, uint32_t *euid, uint32_t *suid)
{
    return cosmo_syscall3(SYS_getresuid, ruid, euid, suid);
}
static inline long cosmo_getresgid(uint32_t *rgid, uint32_t *egid, uint32_t *sgid)
{
    return cosmo_syscall3(SYS_getresgid, rgid, egid, sgid);
}
static inline long cosmo_setgroups(const uint32_t *groups, size_t n)
{
    return cosmo_syscall2(SYS_setgroups, groups, n);
}
static inline long cosmo_getgroups(uint32_t *groups, size_t n)
{
    return cosmo_syscall2(SYS_getgroups, groups, n);
}

/* Resource limits (docs/kernel/security/design.md §2). */
static inline long cosmo_getrlimit(unsigned resource, uint64_t *value)
{
    return cosmo_syscall2(SYS_getrlimit, resource, value);
}
static inline long cosmo_setrlimit(unsigned resource, uint64_t value)
{
    return cosmo_syscall2(SYS_setrlimit, resource, value);
}

/* Readiness and non-blocking mode (docs/kernel/object/api.md). */
static inline long cosmo_ioready(int h)
{
    return cosmo_syscall1(SYS_ioready, h);
}
static inline long cosmo_setnonblock(int h, int on)
{
    return cosmo_syscall2(SYS_setnonblock, h, on);
}

/* The asynchronous I/O ring (docs/kernel/io/api.md). */
static inline long cosmo_aio_create(unsigned entries, unsigned flags)
{
    return cosmo_syscall2(SYS_aio_create, entries, flags);
}
static inline long cosmo_aio_submit(int ring, const struct cosmo_sqe *sqes, unsigned n)
{
    return cosmo_syscall3(SYS_aio_submit, ring, sqes, n);
}
static inline long cosmo_aio_wait(int ring, struct cosmo_cqe *cqes, unsigned n, unsigned min, uint64_t timeout_ns)
{
    return cosmo_syscall5(SYS_aio_wait, ring, cqes, n, min, timeout_ns);
}

#endif /* COSMO_SYSCALL_H */
