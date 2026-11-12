/*
 * linux_internal.h - what the Linux personality's files share
 * (docs/compat/linux/design.md). Not an ABI header: linux_abi.h is.
 */

#ifndef COMPAT_LINUX_INTERNAL_H
#define COMPAT_LINUX_INTERNAL_H

#include <kernel/process.h>
#include <kernel/spinlock.h>
#include <kernel/syscall.h>
#include <arch/user.h>

struct signal_info;
struct sigaction_k;

/* sigcalls.cee: the frame builder the process core calls, the trampoline
 * page, and the signal-related system calls. */
int linux_signal_frame(struct arch_user_regs *regs, const struct sigaction_k *act, const struct signal_info *info,
                       uint64_t blocked_before);
/* The trampoline page (SEC-4): mapped at a random page on AArch64 and its
 * address stored in *where; not mapped on x86-64 (*where 0), where a frame
 * needs SA_RESTORER. */
int linux_sigtramp_map(struct process *p, uint64_t *where);
/* syscalls.c: where the calling process's trampoline is (0: none). */
uint64_t linux_sigtramp_addr(const struct process *p);

int64_t lx_rt_sigaction(struct syscall_args *a);
int64_t lx_rt_sigprocmask(struct syscall_args *a);
int64_t lx_rt_sigreturn(struct syscall_args *a);
int64_t lx_rt_sigpending(struct syscall_args *a);
int64_t lx_rt_sigsuspend(struct syscall_args *a);
int64_t lx_sigaltstack(struct syscall_args *a);
int64_t lx_pause(struct syscall_args *a);
int64_t lx_kill(struct syscall_args *a);
int64_t lx_tgkill(struct syscall_args *a);
int64_t lx_tkill(struct syscall_args *a);
int64_t lx_gettid(struct syscall_args *a);
int64_t lx_set_tid_address(struct syscall_args *a);
int64_t lx_exit(struct syscall_args *a);
int64_t lx_exit_group(struct syscall_args *a);

/* lxident.cee: a process's identity, limits, credentials, groups and
 * sessions; arch_prctl and wait4; and the two answers the table gives calls
 * it does not build. */
int64_t lx_getpid(struct syscall_args *a);
int64_t lx_getppid(struct syscall_args *a);
int64_t lx_getuid(struct syscall_args *a);
int64_t lx_getgid(struct syscall_args *a);
int64_t lx_geteuid(struct syscall_args *a);
int64_t lx_getegid(struct syscall_args *a);
int64_t lx_getrlimit(struct syscall_args *a);
int64_t lx_setrlimit(struct syscall_args *a);
int64_t lx_prlimit64(struct syscall_args *a);
int64_t lx_setresuid(struct syscall_args *a);
int64_t lx_setresgid(struct syscall_args *a);
int64_t lx_setuid(struct syscall_args *a);
int64_t lx_setgid(struct syscall_args *a);
int64_t lx_setreuid(struct syscall_args *a);
int64_t lx_setregid(struct syscall_args *a);
int64_t lx_getresuid(struct syscall_args *a);
int64_t lx_getresgid(struct syscall_args *a);
int64_t lx_getgroups(struct syscall_args *a);
int64_t lx_setgroups(struct syscall_args *a);
int64_t lx_zero(struct syscall_args *a);
int64_t lx_nosys(struct syscall_args *a);
int64_t lx_getpgrp(struct syscall_args *a);
int64_t lx_setpgid(struct syscall_args *a);
int64_t lx_getpgid(struct syscall_args *a);
int64_t lx_setsid(struct syscall_args *a);
int64_t lx_getsid(struct syscall_args *a);
int64_t lx_arch_prctl(struct syscall_args *a);
int64_t lx_wait4(struct syscall_args *a);
int64_t lx_getrusage(struct syscall_args *a);

/* lxtime.cee: the clocks, sleeping, nice values and rebooting; and a
 * struct lx_timespec read from user memory as nanoseconds (clamped at 2^62;
 * -EFAULT or -EINVAL, and then *ns is not written) or written to it. */
int lx_ns_from_timespec(uint64_t uptr, uint64_t *ns);
int64_t lx_put_timespec(uint64_t uptr, uint64_t ns);
int64_t lx_clock_gettime(struct syscall_args *a);
int64_t lx_times(struct syscall_args *a);
int64_t lx_gettimeofday(struct syscall_args *a);
int64_t lx_time(struct syscall_args *a);
int64_t lx_settimeofday(struct syscall_args *a);
int64_t lx_clock_settime(struct syscall_args *a);
int64_t lx_reboot(struct syscall_args *a);
int64_t lx_nanosleep(struct syscall_args *a);
int64_t lx_clock_nanosleep(struct syscall_args *a);
int64_t lx_sched_yield(struct syscall_args *a);
int64_t lx_setitimer(struct syscall_args *a);
int64_t lx_getitimer(struct syscall_args *a);
int64_t lx_alarm(struct syscall_args *a);
int64_t lx_futex(struct syscall_args *a);
int64_t lx_setpriority(struct syscall_args *a);
int64_t lx_getpriority(struct syscall_args *a);

/* lxproc.cee: making processes and threads. */
int64_t lx_execve(struct syscall_args *a);
int64_t lx_fork(struct syscall_args *a);
int64_t lx_vfork(struct syscall_args *a);
int64_t lx_clone(struct syscall_args *a);
int64_t lx_sched_setaffinity(struct syscall_args *a);
int64_t lx_sched_getaffinity(struct syscall_args *a);

/* lxsys.cee: sysinfo, uname, the host name and getrandom. */
int64_t lx_sysinfo(struct syscall_args *a);
int64_t lx_getrandom(struct syscall_args *a);
int64_t lx_uname(struct syscall_args *a);
int64_t lx_sethostname(struct syscall_args *a);

/* lxpoll.cee: poll, ppoll, select and pselect6. */
int64_t lx_poll(struct syscall_args *a);
int64_t lx_ppoll(struct syscall_args *a);
int64_t lx_pselect6(struct syscall_args *a);
int64_t lx_select(struct syscall_args *a);

/* lxmount.cee: mount, umount2, statfs and fstatfs. */
int64_t lx_mount(struct syscall_args *a);
int64_t lx_umount2(struct syscall_args *a);
int64_t lx_statfs(struct syscall_args *a);
int64_t lx_fstatfs(struct syscall_args *a);

/* lxbase.cee: a path from user memory (an empty one is -ENOENT), the
 * directory an *at call resolves from, and the file a descriptor names. */
struct vnode;
struct file;
int lx_get_path(uint64_t uptr, char *buf);
int lx_at_base(int64_t dirfd, const char *path, unsigned rights, struct vnode **out, char *basepath, size_t n,
               unsigned *have);
struct file *lx_file_of(int h, unsigned rights);
struct file *lx_file_of_manage(int h, int *err);

/* One System V shared-memory attach of a process: the address it was
 * mapped at, its size, and a reference-holding pointer to the segment
 * record (not a shmid, which may be gone after IPC_RMID). */
struct shm_segment;
struct lx_shm_attach {
    struct lx_shm_attach *next;
    uint64_t addr;
    size_t size;
    uint64_t tag;              /* the mapping's VM identity, for shmdt */
    struct shm_segment *seg;
};

/* A process's state at the Linux door (process->linux): syscalls.c makes
 * it at exec and fork and releases it; lxmem.cee moves the break and
 * keeps the attaches. */
struct linux_state {
    uint64_t brk_start, brk;
    uint64_t sigtramp;   /* the trampoline page, drawn per exec (SEC-4); 0 on x86-64 */
    unsigned unknown_syscalls;
    uint32_t wx_logged;   /* W^X's refusal has said why, once per exec; swapped atomically (lxmem.cee), so a word */
    struct lx_shm_attach *shm_attaches;   /* singly linked; head insert */
    spinlock_t shm_lock;                  /* guards shm_attaches (threads share it) */
};

/* lxmem.cee: the break, mappings and System V shared memory. */
int64_t lx_brk(struct syscall_args *a);
int64_t lx_mmap(struct syscall_args *a);
int64_t lx_msync(struct syscall_args *a);
int64_t lx_munmap(struct syscall_args *a);
int64_t lx_mremap(struct syscall_args *a);
int64_t lx_mprotect(struct syscall_args *a);
int64_t lx_madvise(struct syscall_args *a);
int64_t lx_shmget(struct syscall_args *a);
int64_t lx_shmat(struct syscall_args *a);
int64_t lx_shmdt(struct syscall_args *a);
int64_t lx_shmctl(struct syscall_args *a);

/* lxattr.cee: a file's mode, owner and times, and chroot. */
int64_t lx_chmod(struct syscall_args *a);
int64_t lx_fchmodat(struct syscall_args *a);
int64_t lx_fchmodat2(struct syscall_args *a);
int64_t lx_fchmod(struct syscall_args *a);
int64_t lx_chown(struct syscall_args *a);
int64_t lx_lchown(struct syscall_args *a);
int64_t lx_fchownat(struct syscall_args *a);
int64_t lx_fchown(struct syscall_args *a);
int64_t lx_chroot(struct syscall_args *a);
int64_t lx_utimensat(struct syscall_args *a);

/* lxname.cee: names and descriptors. */
int64_t lx_unlinkat(struct syscall_args *a);
int64_t lx_faccessat(struct syscall_args *a);
int64_t lx_rename(struct syscall_args *a);
int64_t lx_renameat(struct syscall_args *a);
int64_t lx_link(struct syscall_args *a);
int64_t lx_linkat(struct syscall_args *a);
int64_t lx_chdir(struct syscall_args *a);
int64_t lx_fchdir(struct syscall_args *a);
int64_t lx_getcwd(struct syscall_args *a);
int64_t lx_dup(struct syscall_args *a);
int64_t lx_dup2(struct syscall_args *a);
int64_t lx_dup3(struct syscall_args *a);
int64_t lx_pipe(struct syscall_args *a);
int64_t lx_pipe2(struct syscall_args *a);
int64_t lx_fcntl(struct syscall_args *a);
int64_t lx_sync(struct syscall_args *a);
int64_t lx_fsync(struct syscall_args *a);
int64_t lx_umask(struct syscall_args *a);

/* lxtty.cee: ioctl. */
int64_t lx_ioctl(struct syscall_args *a);

/* lxio.cee: reads, writes and opens. */
int64_t lx_read(struct syscall_args *a);
int64_t lx_write(struct syscall_args *a);
int64_t lx_pread64(struct syscall_args *a);
int64_t lx_pwrite64(struct syscall_args *a);
int64_t lx_readv(struct syscall_args *a);
int64_t lx_writev(struct syscall_args *a);
int64_t lx_open(struct syscall_args *a);
int64_t lx_creat(struct syscall_args *a);
int64_t lx_openat(struct syscall_args *a);
int64_t lx_close(struct syscall_args *a);
int64_t lx_lseek(struct syscall_args *a);

/* lxstat.cee: the stat calls and symbolic links. */
int64_t lx_stat(struct syscall_args *a);
int64_t lx_lstat(struct syscall_args *a);
int64_t lx_fstat(struct syscall_args *a);
int64_t lx_newfstatat(struct syscall_args *a);
int64_t lx_statx(struct syscall_args *a);
int64_t lx_readlink(struct syscall_args *a);
int64_t lx_readlinkat(struct syscall_args *a);
int64_t lx_symlink(struct syscall_args *a);
int64_t lx_symlinkat(struct syscall_args *a);

/* lxsock.cee: the socket calls. lx_accept is accept4 too (its fourth
 * argument, the flags, is 0 from accept). */
int64_t lx_socket(struct syscall_args *a);
int64_t lx_bind(struct syscall_args *a);
int64_t lx_connect(struct syscall_args *a);
int64_t lx_listen(struct syscall_args *a);
int64_t lx_accept(struct syscall_args *a);
int64_t lx_sendto(struct syscall_args *a);
int64_t lx_recvfrom(struct syscall_args *a);
int64_t lx_shutdown(struct syscall_args *a);
int64_t lx_getsockname(struct syscall_args *a);
int64_t lx_getpeername(struct syscall_args *a);
int64_t lx_setsockopt(struct syscall_args *a);
int64_t lx_getsockopt(struct syscall_args *a);
int64_t lx_sendmsg(struct syscall_args *a);
int64_t lx_recvmsg(struct syscall_args *a);
int64_t lx_socketpair(struct syscall_args *a);

/* netioctl.cee: the interface ioctls (SIOCGIF*, SIOCSIF*) on a socket. */
int64_t lx_ioctl_net(unsigned req, uint64_t arg);

#endif /* COMPAT_LINUX_INTERNAL_H */
