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

/* signal.c: the frame builder the process core calls, the exit hook, the
 * trampoline page, and the signal-related system calls. */
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

/* netioctl.c: the interface ioctls (SIOCGIF*, SIOCSIF*) on a socket. */
int64_t lx_ioctl_net(unsigned req, uint64_t arg);

#endif /* COMPAT_LINUX_INTERNAL_H */
