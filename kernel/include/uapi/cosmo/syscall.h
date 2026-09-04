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

#include <stdint.h>

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
/* Phase 7: the filesystem calls. */
#define SYS_open      11  /* (const char *path, int flags, uint32_t mode) -> handle */
#define SYS_stat      12  /* (const char *path, struct cosmo_stat *st) -> 0 */
#define SYS_fstat     13  /* (int h, struct cosmo_stat *st) -> 0 */
#define SYS_lseek     14  /* (int h, int64_t off, int whence) -> new position */
#define SYS_mkdir     15  /* (const char *path, uint32_t mode) -> 0 */
#define SYS_unlink    16  /* (const char *path) -> 0 */
#define SYS_rmdir     17  /* (const char *path) -> 0 */
#define SYS_rename    18  /* (const char *old, const char *new) -> 0 */
#define SYS_getdents  19  /* (int h, void *buf, size_t len) -> bytes, 0 at end */
#define SYS_sync      20  /* () -> 0 */
#define SYS_mount     21  /* (const char *source, const char *target, const char *fstype, unsigned flags) -> 0 */
#define SYS_umount    22  /* (const char *target, unsigned flags) -> 0 */
/* Phase 8: sockets. */
#define SYS_socket    23  /* (int family, int type, int proto) -> handle */
#define SYS_bind      24  /* (int h, const struct cosmo_sockaddr *, size_t len) -> 0 */
#define SYS_listen    25  /* (int h, int backlog) -> 0 */
#define SYS_accept    26  /* (int h, struct cosmo_sockaddr *peer, size_t *len) -> handle */
#define SYS_connect   27  /* (int h, const struct cosmo_sockaddr *, size_t len) -> 0 */
#define SYS_sendto    28  /* (int h, const void *buf, size_t len, const struct cosmo_sockaddr *to (NULL ok), size_t tolen) -> bytes */
#define SYS_recvfrom  29  /* (int h, void *buf, size_t len, struct cosmo_sockaddr *from (NULL ok), size_t *fromlen) -> bytes */
#define SYS_shutdown  30  /* (int h, int how) -> 0 */
#define SYS_getsockname 31 /* (int h, struct cosmo_sockaddr *, size_t *len) -> 0 */
#define SYS_COUNT     32

#define COSMO_AF_INET  2
#define COSMO_AF_INET6 10
#define COSMO_SOCK_STREAM 1
#define COSMO_SOCK_DGRAM  2
#define COSMO_SHUT_RD   0
#define COSMO_SHUT_WR   1
#define COSMO_SHUT_RDWR 2

/* One address shape for both families: addr holds 4 (AF_INET) or 16
 * (AF_INET6) bytes in network byte order; port is in host byte order. */
struct cosmo_sockaddr {
    uint16_t family;
    uint16_t port;
    uint32_t flowinfo;
    uint8_t  addr[16];
    uint32_t scope;
};

/* open() flags. */
#define COSMO_O_RDONLY    0x0000
#define COSMO_O_WRONLY    0x0001
#define COSMO_O_RDWR      0x0002
#define COSMO_O_ACCMODE   0x0003
#define COSMO_O_CREAT     0x0040
#define COSMO_O_EXCL      0x0080
#define COSMO_O_TRUNC     0x0200
#define COSMO_O_APPEND    0x0400
#define COSMO_O_DIRECTORY 0x10000

/* lseek() whence. */
#define COSMO_SEEK_SET 0
#define COSMO_SEEK_CUR 1
#define COSMO_SEEK_END 2

/* Directory entry and stat types. */
#define COSMO_DT_UNKNOWN 0
#define COSMO_DT_REG     1
#define COSMO_DT_DIR     2
#define COSMO_DT_CHR     3

struct cosmo_stat {
    uint64_t ino;
    uint32_t type;      /* COSMO_DT_* */
    uint32_t mode;      /* permission bits */
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint32_t pad;
    uint64_t size;
    uint64_t mtime_ns;
    uint64_t ctime_ns;
};

/* getdents() record: reclen bytes, name NUL terminated, 8-byte aligned. */
struct cosmo_dirent {
    uint64_t ino;
    uint16_t reclen;
    uint8_t  type;      /* COSMO_DT_* */
    uint8_t  namelen;
    char     name[];
};

/* mount() flags. */
#define COSMO_MOUNT_RDONLY (1u << 0)

/* umount() flags. FORCE skips the final commit and drops the open
 * transaction (recovery when a commit keeps failing). */
#define COSMO_UMOUNT_FORCE (1u << 0)

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
#define COSMO_EXDEV   18
#define COSMO_ENODEV  19
#define COSMO_ENOTDIR 20
#define COSMO_EISDIR  21
#define COSMO_EFBIG   27
#define COSMO_EROFS   30
#define COSMO_ENOTEMPTY 39
#define COSMO_ENAMETOOLONG 36
#define COSMO_EPIPE   32
#define COSMO_EMSGSIZE 90
#define COSMO_EOPNOTSUPP 95
#define COSMO_EAFNOSUPPORT 97
#define COSMO_EADDRINUSE 98
#define COSMO_EADDRNOTAVAIL 99
#define COSMO_ECONNRESET 104
#define COSMO_EISCONN 106
#define COSMO_ENOTCONN 107
#define COSMO_ETIMEDOUT 110
#define COSMO_ECONNREFUSED 111
#define COSMO_EHOSTUNREACH 113

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
