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
void linux_thread_exit(struct thread *t);
int linux_sigtramp_map(struct process *p);

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

#endif /* COMPAT_LINUX_INTERNAL_H */
