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
#define LX_S_IFLNK 0120000
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

/* mremap flags. v1 honours none of them to relocate: MAYMOVE never moves,
 * FIXED/DONTUNMAP are rejected. */
#define LX_MREMAP_MAYMOVE   1
#define LX_MREMAP_FIXED     2
#define LX_MREMAP_DONTUNMAP 4
#define LX_MAP_POPULATE 0x8000
#define LX_MS_ASYNC 1
#define LX_MS_INVALIDATE 2
#define LX_MS_SYNC 4

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
#define LX_FD_SETSIZE 1024   /* select's fd_set: 16 words of 64 bits */
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
#define LX_CLONE_VFORK 0x00004000ull
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
/* A fork-like clone (no CLONE_THREAD): CLONE_VM only with CLONE_VFORK. */
#define LX_CLONE_FORK_ALLOWED                                                                                \
    (LX_CLONE_VM | LX_CLONE_VFORK | LX_CLONE_SETTLS | LX_CLONE_PARENT_SETTID | LX_CLONE_CHILD_CLEARTID |       \
     LX_CLONE_CHILD_SETTID)
#define LX_CLONE_EXIT_SIGCHLD 17u   /* the only exit signal accepted: Linux's SIGCHLD, this kernel's too */

/* --- wait4, signals --- */
/* Terminal ioctls (asm-generic/ioctls.h) and the structure they carry.
 * `c_cc` is 19 bytes on Linux with VMIN at 6 and VTIME at 5. */
#define LX_TCGETS     0x5401
#define LX_TCSETS     0x5402
#define LX_TCSETSW    0x5403
#define LX_TCSETSF    0x5404
#define LX_TIOCGWINSZ 0x5413

#define LX_NCCS 19
#define LX_VTIME 5
#define LX_VMIN  6

struct lx_termios {
    uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
    uint8_t c_line;
    uint8_t c_cc[LX_NCCS];
};

struct lx_winsize {
    uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel;
};

/* Linux's flag bits, which are not this tree's. */
#define LX_ICRNL  0000400   /* c_iflag */
#define LX_ISIG   0000001   /* c_lflag */
#define LX_ICANON 0000002
#define LX_ECHO   0000010
#define LX_OPOST  0000001   /* c_oflag */

#define LX_WNOHANG 1
#define LX_WUNTRACED 2
#define LX_WCONTINUED 8
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
#define LX_AF_UNIX 1
#define LX_AF_INET 2
#define LX_AF_INET6 10
#define LX_SOCK_STREAM 1
#define LX_SOCK_DGRAM 2
#define LX_SOCK_SEQPACKET 5
#define LX_SO_PEERCRED 17
#define LX_SCM_RIGHTS 1
#define LX_MSG_CTRUNC   0x08
#define LX_MSG_TRUNC    0x20
#define LX_MSG_DONTWAIT 0x40
#define LX_MSG_NOSIGNAL 0x4000
#define LX_SOCK_NONBLOCK 04000
#define LX_SOCK_CLOEXEC 02000000
#define LX_SOL_SOCKET 1
#define LX_SO_ERROR   4   /* Linux's SO_ERROR; this stack's COSMO_SO_ERROR behind the same kernel call */

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

/* getrandom flags. */
#define LX_GRND_NONBLOCK 0x1
#define LX_GRND_RANDOM   0x2
#define LX_GRND_INSECURE 0x4

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

/* statx(2): one 256-byte layout on both architectures (a modern syscall with
 * a fixed struct, unlike the per-arch struct stat above). */
struct lx_statx_timestamp {
    int64_t tv_sec;
    uint32_t tv_nsec;
    int32_t __reserved;
};
struct lx_statx {
    uint32_t stx_mask;         /* which fields below are filled */
    uint32_t stx_blksize;
    uint64_t stx_attributes;
    uint32_t stx_nlink;
    uint32_t stx_uid;
    uint32_t stx_gid;
    uint16_t stx_mode;
    uint16_t __spare0[1];
    uint64_t stx_ino;
    uint64_t stx_size;
    uint64_t stx_blocks;
    uint64_t stx_attributes_mask;
    struct lx_statx_timestamp stx_atime;
    struct lx_statx_timestamp stx_btime;
    struct lx_statx_timestamp stx_ctime;
    struct lx_statx_timestamp stx_mtime;
    uint32_t stx_rdev_major;
    uint32_t stx_rdev_minor;
    uint32_t stx_dev_major;
    uint32_t stx_dev_minor;
    uint64_t stx_mnt_id;
    uint32_t stx_dio_mem_align;
    uint32_t stx_dio_offset_align;
    uint64_t __spare3[12];
};
#define LX_STATX_BYTES 256

#define LX_STATX_TYPE   0x0001u
#define LX_STATX_MODE   0x0002u
#define LX_STATX_NLINK  0x0004u
#define LX_STATX_UID    0x0008u
#define LX_STATX_GID    0x0010u
#define LX_STATX_ATIME  0x0020u
#define LX_STATX_MTIME  0x0040u
#define LX_STATX_CTIME  0x0080u
#define LX_STATX_INO    0x0100u
#define LX_STATX_SIZE   0x0200u
#define LX_STATX_BLOCKS 0x0400u
#define LX_STATX_BTIME  0x0800u
#define LX_STATX_BASIC_STATS 0x07ffu
#define LX_STATX__RESERVED   0x80000000u   /* a reserved mask bit: EINVAL */
#define LX_AT_STATX_SYNC_TYPE 0x6000        /* FORCE_SYNC | DONT_SYNC; both set is EINVAL */

/* eventfd2 flags (EFD_*): the CLOEXEC and NONBLOCK bits are O_CLOEXEC/O_NONBLOCK. */
#define LX_EFD_SEMAPHORE 1u
#define LX_EFD_CLOEXEC   LX_O_CLOEXEC
#define LX_EFD_NONBLOCK  LX_O_NONBLOCK

/* timerfd flags: TFD_CLOEXEC/TFD_NONBLOCK are O_CLOEXEC/O_NONBLOCK (for
 * timerfd_create); TFD_TIMER_ABSTIME marks an absolute deadline (settime). */
#define LX_TFD_CLOEXEC       LX_O_CLOEXEC
#define LX_TFD_NONBLOCK      LX_O_NONBLOCK
#define LX_TFD_TIMER_ABSTIME 1u

/* signalfd4 flags: SFD_CLOEXEC/SFD_NONBLOCK are O_CLOEXEC/O_NONBLOCK. */
#define LX_SFD_CLOEXEC   LX_O_CLOEXEC
#define LX_SFD_NONBLOCK  LX_O_NONBLOCK

/* The record signalfd read() returns, one per pending signal (128 bytes, the
 * same on both architectures). */
struct lx_signalfd_siginfo {
    uint32_t ssi_signo;
    int32_t  ssi_errno;
    int32_t  ssi_code;
    uint32_t ssi_pid;
    uint32_t ssi_uid;
    int32_t  ssi_fd;
    uint32_t ssi_tid;
    uint32_t ssi_band;
    uint32_t ssi_overrun;
    uint32_t ssi_trapno;
    int32_t  ssi_status;
    int32_t  ssi_int;
    uint64_t ssi_ptr;
    uint64_t ssi_utime;
    uint64_t ssi_stime;
    uint64_t ssi_addr;
    uint16_t ssi_addr_lsb;
    uint8_t  __pad[46];
};
_Static_assert(sizeof(struct lx_signalfd_siginfo) == 128, "signalfd_siginfo is 128 bytes");

/* memfd_create flags (MFD_*): their own small bitset, not the O_* bits.
 * Only MFD_CLOEXEC is supported; sealing and hugepages are out of scope. */
#define LX_MFD_CLOEXEC       0x0001u
#define LX_MFD_ALLOW_SEALING 0x0002u
#define LX_MFD_HUGETLB       0x0004u

/* epoll. The event bits are the poll bits; ET/ONESHOT are the high bits.
 * struct epoll_event is packed on x86-64 only (12 bytes, data unaligned at
 * offset 4), matching Linux's 32-bit-compatible ABI; on AArch64 it is the
 * natural 16-byte layout. */
#define LX_EPOLLIN      0x001u
#define LX_EPOLLOUT     0x004u
#define LX_EPOLLERR     0x008u
#define LX_EPOLLHUP     0x010u
#define LX_EPOLLRDHUP   0x2000u
#define LX_EPOLLONESHOT (1u << 30)
#define LX_EPOLLET      (1u << 31)
#define LX_EPOLL_CTL_ADD 1
#define LX_EPOLL_CTL_DEL 2
#define LX_EPOLL_CTL_MOD 3
#define LX_EPOLL_CLOEXEC LX_O_CLOEXEC
#ifdef LX_ABI_X86_64
#define LX_EPOLL_PACKED __attribute__((packed))
#else
#define LX_EPOLL_PACKED
#endif
struct lx_epoll_event {
    uint32_t events;
    uint64_t data;
} LX_EPOLL_PACKED;
/* The fields this kernel can supply: the basic set minus the access time,
 * which `struct cosmo_stat` does not record (no btime either). */
#define LX_STATX_SUPPORTED (LX_STATX_TYPE | LX_STATX_MODE | LX_STATX_NLINK | LX_STATX_UID | \
                            LX_STATX_GID | LX_STATX_MTIME | LX_STATX_CTIME | LX_STATX_INO | \
                            LX_STATX_SIZE | LX_STATX_BLOCKS)

struct lx_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

/* timerfd itimerspec: it_interval first, it_value second (Linux layout). */
struct lx_itimerspec {
    struct lx_timespec it_interval;
    struct lx_timespec it_value;
};

struct lx_timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

/* sysinfo(2): the LP64 layout, identical on x86-64 and AArch64 (so no arch
 * packing). The compiler inserts the same padding before `totalhigh` (after
 * the two shorts) and at the tail as glibc's struct sysinfo, so sizeof agrees
 * on both sides. */
struct lx_sysinfo {
    int64_t  uptime;        /* seconds since boot */
    uint64_t loads[3];      /* 1/5/15-min load average (fixed point); 0 here */
    uint64_t totalram;      /* in units of mem_unit */
    uint64_t freeram;
    uint64_t sharedram;
    uint64_t bufferram;
    uint64_t totalswap;
    uint64_t freeswap;
    uint16_t procs;         /* current process count */
    uint16_t pad;
    uint64_t totalhigh;
    uint64_t freehigh;
    uint32_t mem_unit;      /* the unit totalram etc. are counted in (bytes) */
};

/* System V shared memory (shmget/shmat/shmdt/shmctl). */
#define LX_IPC_PRIVATE  0
#define LX_IPC_CREAT    01000
#define LX_IPC_EXCL     02000
#define LX_IPC_RMID     0
#define LX_IPC_SET      1
#define LX_IPC_STAT     2
#define LX_SHM_RDONLY   010000
#define LX_SHM_RND      020000
#define LX_SHM_REMAP    040000

/* ipc64_perm / shmid64_ds, the LP64 asm-generic layout x86-64 and AArch64
 * share (both have a 32-bit __kernel_mode_t, so no mode padding). */
struct lx_ipc_perm {
    int32_t  key;
    uint32_t uid, gid, cuid, cgid;
    uint32_t mode;
    uint16_t seq;
    uint16_t __pad2;
    uint64_t __unused1, __unused2;
};

struct lx_shmid_ds {
    struct lx_ipc_perm shm_perm;
    uint64_t shm_segsz;        /* segment size in bytes */
    int64_t  shm_atime;        /* last attach (0: not tracked) */
    int64_t  shm_dtime;        /* last detach */
    int64_t  shm_ctime;        /* last change */
    int32_t  shm_cpid;         /* creator pid */
    int32_t  shm_lpid;         /* last shmat/shmdt pid */
    uint64_t shm_nattch;       /* live attaches */
    uint64_t __unused4, __unused5;
};

_Static_assert(sizeof(struct lx_ipc_perm) == 48, "ipc64_perm LP64 layout");
_Static_assert(sizeof(struct lx_shmid_ds) == 112, "shmid64_ds LP64 layout");
_Static_assert(__builtin_offsetof(struct lx_shmid_ds, shm_segsz) == 48, "shm_segsz offset");
_Static_assert(__builtin_offsetof(struct lx_shmid_ds, shm_nattch) == 88, "shm_nattch offset");

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
#define LX_DT_LNK     10
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

struct lx_sockaddr_un {          /* 110 bytes */
    uint16_t sun_family;
    char sun_path[108];          /* NUL-terminated, or as long as the length says; a leading NUL: abstract */
};

struct lx_msghdr {               /* 56 bytes, both architectures */
    uint64_t msg_name;
    uint32_t msg_namelen;
    uint32_t pad0;
    uint64_t msg_iov;
    uint64_t msg_iovlen;
    uint64_t msg_control;
    uint64_t msg_controllen;
    int32_t msg_flags;
    uint32_t pad1;
};

struct lx_cmsghdr {              /* 16 bytes; data follows, the whole padded to 8 */
    uint64_t cmsg_len;
    int32_t cmsg_level;
    int32_t cmsg_type;
};

struct lx_ucred {
    int32_t pid;
    uint32_t uid;
    uint32_t gid;
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
        struct { int32_t pid; uint32_t uid; int32_t status; } child;   /* SIGCHLD */
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

/* The FP/SIMD state a handler must not lose: the two status words then
 * Q0-Q31, behind an eight-byte header, in the same reserved area. */
struct lx_fpsimd_context {
    uint32_t magic;              /* 0x46508001 */
    uint32_t size;               /* 528 */
    uint32_t fpsr;
    uint32_t fpcr;
    uint8_t vregs[32][16];
};
#define LX_FPSIMD_MAGIC 0x46508001u

/* The end of the reserved area's list of records. */
struct lx_ctx_terminator {
    uint32_t magic;              /* 0 */
    uint32_t size;               /* 0 */
};

#endif /* COMPAT_LINUX_ABI_H */
