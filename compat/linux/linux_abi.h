/*
 * linux_abi.h - The Linux x86-64 ABI as the personality needs it: system
 * call numbers, structure layouts, flags. Everything here is Linux's,
 * written out by hand so the translation depends on no Linux headers.
 * Used by the compat/linux sources and by the tests/linux programs (freestanding).
 */

#ifndef COMPAT_LINUX_ABI_H
#define COMPAT_LINUX_ABI_H

#include <stddef.h>
#include <stdint.h>

/* --- system call numbers (arch/x86/entry/syscalls/syscall_64.tbl) --- */
#define LX_read 0
#define LX_write 1
#define LX_open 2
#define LX_close 3
#define LX_stat 4
#define LX_fstat 5
#define LX_lstat 6
#define LX_poll 7
#define LX_lseek 8
#define LX_mmap 9
#define LX_mprotect 10
#define LX_munmap 11
#define LX_brk 12
#define LX_rt_sigaction 13
#define LX_rt_sigprocmask 14
#define LX_rt_sigreturn 15
#define LX_ioctl 16
#define LX_pread64 17
#define LX_pwrite64 18
#define LX_readv 19
#define LX_writev 20
#define LX_access 21
#define LX_pipe 22
#define LX_select 23
#define LX_sched_yield 24
#define LX_mremap 25
#define LX_msync 26
#define LX_madvise 28
#define LX_dup 32
#define LX_dup2 33
#define LX_pause 34
#define LX_nanosleep 35
#define LX_getpid 39
#define LX_socket 41
#define LX_connect 42
#define LX_accept 43
#define LX_sendto 44
#define LX_recvfrom 45
#define LX_sendmsg 46
#define LX_recvmsg 47
#define LX_shutdown 48
#define LX_bind 49
#define LX_listen 50
#define LX_getsockname 51
#define LX_getpeername 52
#define LX_setsockopt 54
#define LX_getsockopt 55
#define LX_clone 56
#define LX_fork 57
#define LX_vfork 58
#define LX_execve 59
#define LX_exit 60
#define LX_wait4 61
#define LX_kill 62
#define LX_uname 63
#define LX_fcntl 72
#define LX_fsync 74
#define LX_fdatasync 75
#define LX_getcwd 79
#define LX_chdir 80
#define LX_rename 82
#define LX_mkdir 83
#define LX_rmdir 84
#define LX_creat 85
#define LX_unlink 87
#define LX_readlink 89
#define LX_umask 95
#define LX_gettimeofday 96
#define LX_getrlimit 97
#define LX_sysinfo 99
#define LX_getuid 102
#define LX_getgid 104
#define LX_geteuid 107
#define LX_getegid 108
#define LX_setpgid 109
#define LX_getppid 110
#define LX_getpgrp 111
#define LX_setsid 112
#define LX_sigaltstack 131
#define LX_arch_prctl 158
#define LX_setrlimit 160
#define LX_sync 162
#define LX_gettid 186
#define LX_time 201
#define LX_futex 202
#define LX_sched_getaffinity 204
#define LX_getdents64 217
#define LX_set_tid_address 218
#define LX_clock_gettime 228
#define LX_clock_nanosleep 230
#define LX_exit_group 231
#define LX_tgkill 234
#define LX_openat 257
#define LX_mkdirat 258
#define LX_newfstatat 262
#define LX_unlinkat 263
#define LX_renameat 264
#define LX_readlinkat 267
#define LX_faccessat 269
#define LX_set_robust_list 273
#define LX_accept4 288
#define LX_dup3 292
#define LX_pipe2 293
#define LX_prlimit64 302
#define LX_getrandom 318
#define LX_rseq 334
#define LX_clone3 435
#define LX_NR_MAX 512

/* --- errno (identical to the native values the kernel produces) --- */

/* --- open flags (octal, x86-64) --- */
#define LX_O_RDONLY 00
#define LX_O_WRONLY 01
#define LX_O_RDWR 02
#define LX_O_ACCMODE 03
#define LX_O_CREAT 0100
#define LX_O_EXCL 0200
#define LX_O_NOCTTY 0400
#define LX_O_TRUNC 01000
#define LX_O_APPEND 02000
#define LX_O_NONBLOCK 04000
#define LX_O_DIRECTORY 0200000
#define LX_O_NOFOLLOW 0400000
#define LX_O_CLOEXEC 02000000
#define LX_O_LARGEFILE 0100000
#define LX_AT_FDCWD (-100)
#define LX_AT_REMOVEDIR 0x200
#define LX_AT_EMPTY_PATH 0x1000
#define LX_AT_SYMLINK_NOFOLLOW 0x100

/* --- file types in st_mode --- */
#define LX_S_IFMT 0170000
#define LX_S_IFSOCK 0140000
#define LX_S_IFREG 0100000
#define LX_S_IFDIR 0040000
#define LX_S_IFCHR 0020000
#define LX_S_IFIFO 0010000

/* --- mmap --- */
#define LX_PROT_READ 1
#define LX_PROT_WRITE 2
#define LX_PROT_EXEC 4
#define LX_MAP_SHARED 0x01
#define LX_MAP_PRIVATE 0x02
#define LX_MAP_FIXED 0x10
#define LX_MAP_ANONYMOUS 0x20
#define LX_MAP_NORESERVE 0x4000
#define LX_MAP_STACK 0x20000
#define LX_MAP_POPULATE 0x8000

/* --- arch_prctl --- */
#define LX_ARCH_SET_GS 0x1001
#define LX_ARCH_SET_FS 0x1002
#define LX_ARCH_GET_FS 0x1003
#define LX_ARCH_GET_GS 0x1004

/* --- futex --- */
#define LX_FUTEX_WAIT 0
#define LX_FUTEX_WAKE 1
#define LX_FUTEX_PRIVATE_FLAG 128
#define LX_FUTEX_CLOCK_REALTIME 256
#define LX_FUTEX_CMD_MASK ~(LX_FUTEX_PRIVATE_FLAG | LX_FUTEX_CLOCK_REALTIME)

/* --- wait4, signals --- */
#define LX_WNOHANG 1
#define LX_SIGKILL 9
#define LX_SIGSEGV 11
#define LX_SIGSTOP 19
#define LX_NSIG 64

/* --- fcntl --- */
#define LX_F_DUPFD 0
#define LX_F_GETFD 1
#define LX_F_SETFD 2
#define LX_F_GETFL 3
#define LX_F_SETFL 4
#define LX_F_DUPFD_CLOEXEC 1030

/* --- clocks --- */
#define LX_CLOCK_REALTIME 0
#define LX_CLOCK_MONOTONIC 1
#define LX_CLOCK_MONOTONIC_RAW 4
#define LX_CLOCK_REALTIME_COARSE 5
#define LX_CLOCK_MONOTONIC_COARSE 6
#define LX_CLOCK_BOOTTIME 7
#define LX_TIMER_ABSTIME 1

/* --- sockets --- */
#define LX_AF_INET 2
#define LX_AF_INET6 10
#define LX_SOCK_STREAM 1
#define LX_SOCK_DGRAM 2
#define LX_SOCK_NONBLOCK 04000
#define LX_SOCK_CLOEXEC 02000000
#define LX_SOL_SOCKET 1

/* --- auxiliary vector --- */
#define LX_AT_NULL 0
#define LX_AT_PHDR 3
#define LX_AT_PHENT 4
#define LX_AT_PHNUM 5
#define LX_AT_PAGESZ 6
#define LX_AT_ENTRY 9
#define LX_AT_UID 11
#define LX_AT_EUID 12
#define LX_AT_GID 13
#define LX_AT_EGID 14
#define LX_AT_HWCAP 16
#define LX_AT_CLKTCK 17
#define LX_AT_SECURE 23
#define LX_AT_RANDOM 25

/* --- structures --- */

struct lx_stat {                 /* x86-64 struct stat: 144 bytes */
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t pad0;
    uint64_t st_rdev;
    int64_t st_size;
    int64_t st_blksize;
    int64_t st_blocks;
    int64_t st_atime, st_atime_nsec;
    int64_t st_mtime, st_mtime_nsec;
    int64_t st_ctime, st_ctime_nsec;
    int64_t unused[3];
};

struct lx_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct lx_timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

struct lx_iovec {
    uint64_t iov_base;
    uint64_t iov_len;
};

struct lx_utsname {              /* 6 x 65 bytes */
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

#define LX_DT_UNKNOWN 0
#define LX_DT_FIFO    1
#define LX_DT_CHR     2
#define LX_DT_DIR     4
#define LX_DT_REG     8
#define LX_DT_SOCK    12

struct lx_dirent64 {             /* header of a getdents64 record; d_name follows */
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[];
};

struct lx_sockaddr_in {          /* 16 bytes */
    uint16_t sin_family;
    uint16_t sin_port;           /* network order */
    uint32_t sin_addr;           /* network order */
    uint8_t sin_zero[8];
};

struct lx_sockaddr_in6 {         /* 28 bytes */
    uint16_t sin6_family;
    uint16_t sin6_port;          /* network order */
    uint32_t sin6_flowinfo;
    uint8_t sin6_addr[16];
    uint32_t sin6_scope_id;
};

struct lx_sigaction {            /* struct k_sigaction: 32 bytes */
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint64_t mask;
};

struct lx_stack_t {
    uint64_t ss_sp;
    int32_t ss_flags;
    int32_t pad;
    uint64_t ss_size;
};

#endif /* COMPAT_LINUX_ABI_H */
