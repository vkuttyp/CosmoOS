/*
 * linux_internal.h - what the Linux personality's files share
 * (docs/compat/linux/design.md). Not an ABI header: linux_abi.h is.
 */

#ifndef COMPAT_LINUX_INTERNAL_H
#define COMPAT_LINUX_INTERNAL_H

#include <kernel/process.h>
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

/* lxtime.cee: the clocks, sleeping, nice values and rebooting. */
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
