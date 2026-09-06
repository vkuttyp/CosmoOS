/*
 * linux_abi.h - The Linux ABI as the personality needs it: system call
 * numbers (per architecture: nr_x86_64.h, nr_aarch64.h), structure
 * layouts, flags. Everything here is Linux's, written out by hand so the
 * translation depends on no Linux headers. Used by the compat/linux
 * sources, the host tests and the tests/linux programs (freestanding).
 */

#ifndef COMPAT_LINUX_ABI_H
#define COMPAT_LINUX_ABI_H

#include <stddef.h>
#include <stdint.h>

/* --- system call numbers, per architecture (milestone 10) --- */
#if defined(ARCH_X86_64) || (!defined(ARCH_AARCH64) && defined(__x86_64__))
#include "nr_x86_64.h"
#define LX_MACHINE "x86_64"
#define LX_ABI_X86_64 1
#elif defined(ARCH_AARCH64) || defined(__aarch64__)
#include "nr_aarch64.h"
#define LX_MACHINE "aarch64"
#define LX_ABI_AARCH64 1
#else
#error "linux_abi.h: unknown architecture"
#endif
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

/* --- rlimits (getrlimit/setrlimit/prlimit64) --- */
#define LX_RLIMIT_DATA 2
#define LX_RLIMIT_STACK 3
#define LX_RLIMIT_RSS 5
#define LX_RLIMIT_NPROC 6
#define LX_RLIMIT_NOFILE 7
#define LX_RLIMIT_AS 9
#define LX_RLIM_NLIMITS 16
#define LX_RLIM_INFINITY (~0ull)
struct lx_rlimit {
    uint64_t rlim_cur;
    uint64_t rlim_max;
};

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
#define LX_FUTEX_REQUEUE 3
#define LX_FUTEX_CMP_REQUEUE 4
#define LX_FUTEX_WAIT_BITSET 9
#define LX_FUTEX_WAKE_BITSET 10
#define LX_FUTEX_BITSET_MATCH_ANY 0xffffffffu
#define LX_FUTEX_PRIVATE_FLAG 128
#define LX_FUTEX_CLOCK_REALTIME 256
#define LX_FUTEX_CMD_MASK ~(LX_FUTEX_PRIVATE_FLAG | LX_FUTEX_CLOCK_REALTIME)

/* --- poll --- */
#define LX_POLLIN 0x001
#define LX_POLLPRI 0x002
#define LX_POLLOUT 0x004
#define LX_POLLERR 0x008
#define LX_POLLHUP 0x010
#define LX_POLLNVAL 0x020
#define LX_POLLRDNORM 0x040
#define LX_POLLWRNORM 0x100
#define LX_POLLRDHUP 0x2000
#define LX_POLL_MAX 1024
struct lx_pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

/* --- clone --- */
#define LX_CLONE_VM 0x00000100ull
#define LX_CLONE_FS 0x00000200ull
#define LX_CLONE_FILES 0x00000400ull
#define LX_CLONE_SIGHAND 0x00000800ull
#define LX_CLONE_THREAD 0x00010000ull
#define LX_CLONE_SYSVSEM 0x00040000ull
#define LX_CLONE_SETTLS 0x00080000ull
#define LX_CLONE_PARENT_SETTID 0x00100000ull
#define LX_CLONE_CHILD_CLEARTID 0x00200000ull
#define LX_CLONE_DETACHED 0x00400000ull
#define LX_CLONE_UNTRACED 0x00800000ull
#define LX_CLONE_CHILD_SETTID 0x01000000ull
/* The thread set: what pthread_create passes (musl, glibc). */
#define LX_CLONE_THREAD_REQUIRED (LX_CLONE_VM | LX_CLONE_THREAD | LX_CLONE_SIGHAND)
#define LX_CLONE_THREAD_ALLOWED                                                                              \
    (LX_CLONE_THREAD_REQUIRED | LX_CLONE_FS | LX_CLONE_FILES | LX_CLONE_SYSVSEM | LX_CLONE_SETTLS |         \
     LX_CLONE_PARENT_SETTID | LX_CLONE_CHILD_CLEARTID | LX_CLONE_CHILD_SETTID | LX_CLONE_DETACHED |         \
     LX_CLONE_UNTRACED)

/* --- wait4, signals --- */
#define LX_WNOHANG 1
#define LX_SIGKILL 9
#define LX_SIGSEGV 11
#define LX_SIGSTOP 19
#define LX_NSIG 64
#define LX_SIG_BLOCK 0
#define LX_SIG_UNBLOCK 1
#define LX_SIG_SETMASK 2
#define LX_SA_SIGINFO 0x00000004u
#define LX_SA_RESTORER 0x04000000u
#define LX_SA_ONSTACK 0x08000000u
#define LX_SA_RESTART 0x10000000u
#define LX_SA_NODEFER 0x40000000u
#define LX_SA_RESETHAND 0x80000000u
#define LX_SS_ONSTACK 1
#define LX_SS_DISABLE 2
#define LX_SS_AUTODISARM (1u << 31)
#define LX_MINSIGSTKSZ 2048
#define LX_SI_USER 0
#define LX_SI_KERNEL 0x80
#define LX_SI_TKILL (-6)
#define LX_SEGV_MAPERR 1
#define LX_SEGV_ACCERR 2
#define LX_UC_SIGCONTEXT_SS 0x2
#define LX_UC_STRICT_RESTORE_SS 0x4
/* The kernel's signal-return trampoline page (docs/compat/linux/design.md,
 * stage 2): `mov $15, %eax; syscall` or `mov x8, #139; svc #0`. The page
 * above the stack's top (USER_STACK_TOP), with one unmapped page between. */
#define LX_SIGTRAMP 0x7FFFFFFF1000ull

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
#define LX_AT_BASE 7
#define LX_AT_PLATFORM 15
#define LX_AT_HWCAP2 26
#define LX_AT_EXECFN 31
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

#if defined(LX_ABI_X86_64)
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
#define LX_STAT_SIZE 144
#else
struct lx_stat {                 /* AArch64 (asm-generic) struct stat: 128 bytes */
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint64_t pad1;
    int64_t st_size;
    int32_t st_blksize;
    int32_t pad2;
    int64_t st_blocks;
    int64_t st_atime, st_atime_nsec;
    int64_t st_mtime, st_mtime_nsec;
    int64_t st_ctime, st_ctime_nsec;
    uint32_t unused[2];
};
#define LX_STAT_SIZE 128
#endif

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

struct lx_siginfo {              /* 128 bytes, the same on both architectures */
    int32_t si_signo;
    int32_t si_errno;
    int32_t si_code;
    int32_t pad;
    union {
        struct { int32_t pid; uint32_t uid; } kill;
        struct { uint64_t addr; } fault;
        uint8_t fill[112];
    } u;
};

/* x86-64: struct rt_sigframe as the handler sees it (rsp points at pretcode). */
struct lx_sigcontext_x86 {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags;
    uint16_t cs, gs, fs, ss;
    uint64_t err, trapno, oldmask, cr2;
    uint64_t fpstate;            /* user pointer to the 512-byte FXSAVE image, or 0 */
    uint64_t reserved[8];
};
struct lx_ucontext_x86 {
    uint64_t uc_flags;
    uint64_t uc_link;
    struct lx_stack_t uc_stack;
    struct lx_sigcontext_x86 uc_mcontext;
    uint64_t uc_sigmask;
};
struct lx_rt_sigframe_x86 {
    uint64_t pretcode;
    struct lx_ucontext_x86 uc;
    struct lx_siginfo info;
};

/* AArch64: struct rt_sigframe (sp points at it; a frame record lies below). */
struct lx_sigcontext_a64 {
    uint64_t fault_address;
    uint64_t regs[31];
    uint64_t sp, pc, pstate;
    uint8_t reserved[4096] __attribute__((aligned(16)));   /* esr_context, terminator */
};
struct lx_ucontext_a64 {
    uint64_t uc_flags;
    uint64_t uc_link;
    struct lx_stack_t uc_stack;
    uint64_t uc_sigmask;
    uint8_t unused[120];
    struct lx_sigcontext_a64 uc_mcontext;
};
struct lx_rt_sigframe_a64 {
    struct lx_siginfo info;
    struct lx_ucontext_a64 uc;
};
struct lx_esr_context {
    uint32_t magic;              /* 0x45535201 */
    uint32_t size;               /* 16 */
    uint64_t esr;
};
#define LX_ESR_MAGIC 0x45535201u

#endif /* COMPAT_LINUX_ABI_H */
