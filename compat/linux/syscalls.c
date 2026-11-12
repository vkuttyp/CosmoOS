/*
 * syscalls.c - The Linux personality's system-call table and its record
 * (docs/compat/linux/).
 *
 * The calls themselves are CeeSafe's: every entry validates its arguments,
 * converts Linux structures and flags (lxconv.cee), and calls the same
 * VFS, socket, process and memory services the native personality uses.
 * linux_internal.h declares them, by file: sigcalls.cee, lxsock.cee,
 * lxident.cee, lxtime.cee, lxproc.cee, lxsys.cee, lxpoll.cee, lxmount.cee,
 * lxbase.cee, lxmem.cee, lxattr.cee, lxname.cee, lxtty.cee, lxio.cee,
 * lxstat.cee, lxfd.cee, lxdir.cee, netioctl.cee and lxhooks.cee, which
 * also holds the hooks the record names. Nothing here is reachable by a
 * native process, and nothing native depends on this file.
 *
 * The table is an array of C functions, which CeeSafe cannot write yet
 * (roadmap 1.3 K1's one exception, C2's to move).
 */

#include <kernel/process.h>
#include <kernel/syscall.h>

#include "linux_abi.h"
#include "linux_internal.h"

/* Every number first, then the ones with a call: a number with none is
 * lx_unknown's (lxhooks.cee), which counts and logs the miss, where the
 * dispatcher would answer -ENOSYS itself for a NULL entry. The later
 * initialisers override the first on purpose. */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winitializer-overrides"
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverride-init"
#endif
static const syscall_fn linux_table[LX_NR_MAX] = {
    [0 ... LX_NR_MAX - 1] = lx_unknown,
    [LX_read] = lx_read,
    [LX_write] = lx_write,
#ifdef LX_open
    [LX_open] = lx_open,
#endif
    [LX_close] = lx_close,
#ifdef LX_stat
    [LX_stat] = lx_stat,
#endif
    [LX_fstat] = lx_fstat,
#ifdef LX_lstat
    [LX_lstat] = lx_lstat,
#endif
    [LX_lseek] = lx_lseek,
    [LX_mmap] = lx_mmap,
    [LX_mprotect] = lx_mprotect,
    [LX_munmap] = lx_munmap,
    [LX_mremap] = lx_mremap,
    [LX_msync] = lx_msync,
    [LX_shmget] = lx_shmget,
    [LX_shmat] = lx_shmat,
    [LX_shmdt] = lx_shmdt,
    [LX_shmctl] = lx_shmctl,
    [LX_brk] = lx_brk,
    [LX_rt_sigaction] = lx_rt_sigaction,
    [LX_rt_sigprocmask] = lx_rt_sigprocmask,
    [LX_rt_sigreturn] = lx_rt_sigreturn,
    [LX_rt_sigpending] = lx_rt_sigpending,
    [LX_rt_sigsuspend] = lx_rt_sigsuspend,
#ifdef LX_pause
    [LX_pause] = lx_pause,
#endif
#ifdef LX_poll
    [LX_poll] = lx_poll,
#endif
    [LX_ppoll] = lx_ppoll,
    [LX_pselect6] = lx_pselect6,
#ifdef LX_select
    [LX_select] = lx_select,
#endif
    [LX_tkill] = lx_tkill,
    [LX_ioctl] = lx_ioctl,
    [LX_pread64] = lx_pread64,
    [LX_pwrite64] = lx_pwrite64,
    [LX_readv] = lx_readv,
    [LX_writev] = lx_writev,
#ifdef LX_access
    [LX_access] = lx_access,
#endif
#ifdef LX_pipe
    [LX_pipe] = lx_pipe,
#endif
    [LX_sched_yield] = lx_sched_yield,
    [LX_madvise] = lx_madvise,
    [LX_dup] = lx_dup,
#ifdef LX_dup2
    [LX_dup2] = lx_dup2,
#endif
    [LX_nanosleep] = lx_nanosleep,
    [LX_getpid] = lx_getpid,
    [LX_socket] = lx_socket,
    [LX_connect] = lx_connect,
    [LX_accept] = lx_accept,
    [LX_sendto] = lx_sendto,
    [LX_recvfrom] = lx_recvfrom,
    [LX_shutdown] = lx_shutdown,
    [LX_bind] = lx_bind,
    [LX_listen] = lx_listen,
    [LX_getsockname] = lx_getsockname,
    [LX_getpeername] = lx_getpeername,
    [LX_setsockopt] = lx_setsockopt,
    [LX_getsockopt] = lx_getsockopt,
    [LX_sendmsg] = lx_sendmsg,
    [LX_recvmsg] = lx_recvmsg,
    [LX_socketpair] = lx_socketpair,
    [LX_clone] = lx_clone,
#ifdef LX_fork
    [LX_fork] = lx_fork,
#endif
#ifdef LX_vfork
    [LX_vfork] = lx_vfork,
#endif
    [LX_execve] = lx_execve,
    [LX_exit] = lx_exit,
    [LX_wait4] = lx_wait4,
    [LX_kill] = lx_kill,
    [LX_uname] = lx_uname,
    [LX_sethostname] = lx_sethostname,
    [LX_fcntl] = lx_fcntl,
    [LX_fsync] = lx_fsync,
    [LX_fdatasync] = lx_fsync,
    [LX_getcwd] = lx_getcwd,
    [LX_chdir] = lx_chdir,
    [LX_fchdir] = lx_fchdir,
#ifdef LX_rename
    [LX_rename] = lx_rename,
#endif
#ifdef LX_mkdir
    [LX_mkdir] = lx_mkdir,
#endif
#ifdef LX_rmdir
    [LX_rmdir] = lx_rmdir,
#endif
#ifdef LX_creat
    [LX_creat] = lx_creat,
#endif
#ifdef LX_unlink
    [LX_unlink] = lx_unlink,
#endif
#ifdef LX_readlink
#ifdef LX_readlink
    [LX_readlink] = lx_readlink,
#endif
#endif
    [LX_umask] = lx_umask,
    [LX_gettimeofday] = lx_gettimeofday,
    [LX_getrlimit] = lx_getrlimit,
    [LX_sysinfo] = lx_sysinfo,
    [LX_statfs] = lx_statfs,
    [LX_setitimer] = lx_setitimer,
    [LX_prctl] = lx_prctl,
    [LX_getitimer] = lx_getitimer,
    [LX_getrusage] = lx_getrusage,
    [LX_times] = lx_times,
    [LX_getpriority] = lx_getpriority,
    [LX_setpriority] = lx_setpriority,
#ifdef LX_alarm
    [LX_alarm] = lx_alarm,
#endif
    [LX_fstatfs] = lx_fstatfs,
    [LX_getuid] = lx_getuid,
    [LX_getgid] = lx_getgid,
    [LX_geteuid] = lx_geteuid,
    [LX_getegid] = lx_getegid,
    [LX_setuid] = lx_setuid,
    [LX_setgid] = lx_setgid,
    [LX_setreuid] = lx_setreuid,
    [LX_setregid] = lx_setregid,
    [LX_getgroups] = lx_getgroups,
    [LX_setgroups] = lx_setgroups,
    [LX_setresuid] = lx_setresuid,
    [LX_getresuid] = lx_getresuid,
    [LX_setresgid] = lx_setresgid,
    [LX_getresgid] = lx_getresgid,
    [LX_setpgid] = lx_setpgid,
    [LX_getpgid] = lx_getpgid,
    [LX_getsid] = lx_getsid,
    [LX_getppid] = lx_getppid,
#ifdef LX_getpgrp
    [LX_getpgrp] = lx_getpgrp,
#endif
    [LX_setsid] = lx_setsid,
    [LX_sigaltstack] = lx_sigaltstack,
#ifdef LX_arch_prctl
    [LX_arch_prctl] = lx_arch_prctl,
#endif
    [LX_setrlimit] = lx_setrlimit,
    [LX_sync] = lx_sync,
    [LX_gettid] = lx_gettid,
#ifdef LX_time
    [LX_time] = lx_time,
#endif
    [LX_futex] = lx_futex,
    [LX_sched_setaffinity] = lx_sched_setaffinity,
    [LX_sched_getaffinity] = lx_sched_getaffinity,
    [LX_getdents64] = lx_getdents64,
    [LX_set_tid_address] = lx_set_tid_address,
    [LX_clock_gettime] = lx_clock_gettime,
    [LX_clock_nanosleep] = lx_clock_nanosleep,
    [LX_exit_group] = lx_exit_group,
    [LX_tgkill] = lx_tgkill,
    [LX_openat] = lx_openat,
    [LX_mkdirat] = lx_mkdirat,
    [LX_mknodat] = lx_mknodat,
#ifdef LX_mknod
    [LX_mknod] = lx_mknod,
#endif
    [LX_newfstatat] = lx_newfstatat,
    [LX_statx] = lx_statx,
    [LX_eventfd2] = lx_eventfd2,
#ifdef LX_eventfd
    [LX_eventfd] = lx_eventfd,   /* x86-64 only: the older number, no flags */
#endif
    [LX_signalfd4] = lx_signalfd4,
#ifdef LX_signalfd
    [LX_signalfd] = lx_signalfd,   /* x86-64 only: the older number, no flags */
#endif
    [LX_timerfd_create] = lx_timerfd_create,
    [LX_timerfd_settime] = lx_timerfd_settime,
    [LX_timerfd_gettime] = lx_timerfd_gettime,
    [LX_memfd_create] = lx_memfd_create,
    [LX_ftruncate] = lx_ftruncate,
    [LX_epoll_create1] = lx_epoll_create1,
    [LX_epoll_ctl] = lx_epoll_ctl,
    [LX_epoll_pwait] = lx_epoll_pwait,
#ifdef LX_epoll_create
    [LX_epoll_create] = lx_epoll_create,   /* x86-64: the legacy create and wait */
    [LX_epoll_wait] = lx_epoll_wait,
#endif
    [LX_unlinkat] = lx_unlinkat,
#ifdef LX_chown
    [LX_chown] = lx_chown,
#endif
#ifdef LX_lchown
    [LX_lchown] = lx_lchown,
#endif
    [LX_fchown] = lx_fchown,
    [LX_fchownat] = lx_fchownat,
    [LX_chroot] = lx_chroot,
    [LX_settimeofday] = lx_settimeofday,
    [LX_clock_settime] = lx_clock_settime,
    [LX_reboot] = lx_reboot,
    [LX_mount] = lx_mount,
    [LX_umount2] = lx_umount2,
#ifdef LX_chmod
    [LX_chmod] = lx_chmod,
#endif
    [LX_fchmod] = lx_fchmod,
    [LX_fchmodat] = lx_fchmodat,
    [LX_fchmodat2] = lx_fchmodat2,
    [LX_utimensat] = lx_utimensat,
    [LX_renameat] = lx_renameat,
    [LX_linkat] = lx_linkat,
#ifdef LX_link
    [LX_link] = lx_link,
#endif
    [LX_readlinkat] = lx_readlinkat,
#ifdef LX_symlink
    [LX_symlink] = lx_symlink,
#endif
    [LX_symlinkat] = lx_symlinkat,
    [LX_faccessat] = lx_faccessat,
    [LX_set_robust_list] = lx_zero,
    [LX_accept4] = lx_accept,
    [LX_dup3] = lx_dup3,
    [LX_pipe2] = lx_pipe2,
    [LX_prlimit64] = lx_prlimit64,
    [LX_getrandom] = lx_getrandom,
    [LX_rseq] = lx_nosys,
    [LX_clone3] = lx_nosys,
};
#if defined(__clang__)
#pragma clang diagnostic pop
#else
#pragma GCC diagnostic pop
#endif

/* As for the native personality, plus rt_sigreturn: a signal handler
 * must be able to return, or the first signal after a filter is
 * installed is fatal for a reason that has nothing to do with the
 * filter. */
static const uint16_t linux_always_allowed[] = { LX_exit, LX_exit_group, LX_rt_sigreturn };

#if defined(ARCH_X86_64)
#define LINUX_PLATFORM "x86_64"
#else
#define LINUX_PLATFORM "aarch64"
#endif

const struct personality personality_linux = {
    .name = "linux",
    .claims_elf = linux_claims_elf,
    .init = linux_process_init,
    .release = linux_process_release,
    .fork = linux_process_fork,
    .exec = linux_process_exec,
    .platform = LINUX_PLATFORM,   /* AT_PLATFORM: the string Linux gives on this machine */
    .auxv = linux_auxv,
    .table = linux_table,
    .count = LX_NR_MAX,
    .always_allowed = linux_always_allowed,
    .nr_always_allowed = sizeof(linux_always_allowed) / sizeof(linux_always_allowed[0]),
    .signal_frame = linux_signal_frame,
};
