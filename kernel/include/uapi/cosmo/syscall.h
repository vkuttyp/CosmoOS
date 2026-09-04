/*
 * uapi/cosmo/syscall.h - CosmoOS native system-call ABI.
 *
 * Shared verbatim between the kernel and user space. This is user ABI:
 * numbers and structures here are stable; add, never renumber.
 *
 * Convention (x86-64): number in rax, arguments in rdi rsi rdx r10 r8
 * r9, result in rax; negative results are -errno. rcx and r11 are
 * clobbered by the instruction.
 */

#ifndef UAPI_COSMO_SYSCALL_H
#define UAPI_COSMO_SYSCALL_H

#define SYS_exit      0   /* (int status) -> never returns */
#define SYS_write     1   /* (int h, const void *buf, size_t len) -> bytes */
#define SYS_read      2   /* (int h, void *buf, size_t len) -> bytes */
#define SYS_getpid    3   /* () -> pid */
#define SYS_yield     4   /* () -> 0 */
#define SYS_sleep_ns  5   /* (uint64_t ns) -> 0 */
#define SYS_clock_ns  6   /* () -> monotonic nanoseconds */
#define SYS_mmap      7   /* (void *hint, size_t len, int prot, int flags) -> addr */
#define SYS_munmap    8   /* (void *addr, size_t len) -> 0 */
#define SYS_log       9   /* (const char *s, size_t len) -> 0 */
#define SYS_close     10  /* (int h) -> 0 */
#define SYS_COUNT     11

/* mmap protection and flags. */
#define COSMO_PROT_NONE  0
#define COSMO_PROT_READ  (1 << 0)
#define COSMO_PROT_WRITE (1 << 1)
#define COSMO_PROT_EXEC  (1 << 2)

#define COSMO_MAP_ANONYMOUS (1 << 0)
#define COSMO_MAP_FIXED     (1 << 1)  /* hint is a requirement */

/* Error numbers (subset, values as in kernel/errno.h). */
#define COSMO_EPERM   1
#define COSMO_ENOENT  2
#define COSMO_EIO     5
#define COSMO_EBADF   9
#define COSMO_EAGAIN  11
#define COSMO_ENOMEM  12
#define COSMO_EFAULT  14
#define COSMO_EBUSY   16
#define COSMO_EEXIST  17
#define COSMO_EINVAL  22
#define COSMO_EMFILE  24
#define COSMO_ENOSPC  28
#define COSMO_ENOSYS  38

/* Exit status reported for a process terminated by a fatal fault. */
#define COSMO_EXIT_FAULT 139

/* Standard handles of a new process. */
#define COSMO_STDIN  0
#define COSMO_STDOUT 1
#define COSMO_STDERR 2

/* Auxiliary vector entries on the initial stack. */
#define COSMO_AT_NULL   0
#define COSMO_AT_PAGESZ 6
#define COSMO_AT_ENTRY  9

#endif /* UAPI_COSMO_SYSCALL_H */
