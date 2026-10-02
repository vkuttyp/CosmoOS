/*
 * signal.h - POSIX signals as a kernel service (docs/kernel/process/design.md
 * §11, "The signal core"). Both personalities share it: the native ABI
 * installs no handlers, so for it every action is the default.
 *
 * Numbers are Linux's (1..64). Actions are per process (`p->sigactions`,
 * under p->lock); pending and blocked sets are per thread, plus a
 * process-wide pending set that any thread not blocking a signal may
 * take. Delivery happens only at a return to user mode of the receiving
 * thread, through the personality's frame builder.
 */
#ifndef KERNEL_SIGNAL_H
#define KERNEL_SIGNAL_H

#include <kernel/types.h>

struct process;
struct thread;
struct arch_trap_frame;
struct arch_user_regs;

#define SIG_MAX 64
#define SIGMASK(sig) (1ull << ((sig) - 1))

/* Linux numbers, used by both personalities. */
#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGBUS 7
#define SIGFPE 8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGURG 23
#define SIGWINCH 28
#define SIGSYS 31

/* An action, Linux's struct k_sigaction shape. */
#define SIG_DFL 0ull
#define SIG_IGN 1ull
#define SA_NOCLDSTOP 0x00000001ull
#define SA_SIGINFO   0x00000004ull
#define SA_ONSTACK   0x08000000ull
#define SA_RESTART   0x10000000ull
#define SA_NODEFER   0x40000000ull
#define SA_RESETHAND 0x80000000ull
#define SA_RESTORER  0x04000000ull

struct sigaction_k {
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint64_t mask;
};

struct sigaltstack_k {
    uint64_t sp;
    uint64_t size;
    uint32_t flags;   /* 1 = SS_ONSTACK (in use), 2 = SS_DISABLE */
};

/* How a signal arrived, for siginfo. */
enum signal_source { SIGSRC_USER, SIGSRC_TKILL, SIGSRC_FAULT, SIGSRC_KERNEL, SIGSRC_CHILD };

/* SIGCHLD cause codes (SIGSRC_CHILD, carried in `code`), the CLD_* values. */
#define SI_CLD_EXITED    1   /* child called _exit */
#define SI_CLD_KILLED    2   /* child died on a signal */
#define SI_CLD_STOPPED   5   /* child stopped */
#define SI_CLD_CONTINUED 6   /* child continued */

struct signal_info {
    int sig;
    enum signal_source source;
    uint64_t fault_addr;   /* SIGSRC_FAULT */
    uint32_t code;         /* SIGSRC_FAULT: 1 unmapped, 2 protection; SIGSRC_CHILD: the SI_CLD_* cause */
    uint32_t sender_pid;   /* pid_t; SIGSRC_USER/TKILL sender, SIGSRC_CHILD the child */
    uint32_t sender_uid;
    int32_t  status;       /* SIGSRC_CHILD: exit code, or the signal that killed/stopped the child */
};

/* What the personality must do to run a handler: rewrite `regs` (already
 * loaded from the frame; written back by the core, which also marks the
 * frame for a full restore). `blocked_before` is the mask the handler's
 * return restores. Returns 0, or a negative errno when the frame cannot
 * be built (an unmapped stack): the core then terminates the process
 * with SIGSEGV. */
typedef int (*signal_frame_fn)(struct arch_user_regs *regs, const struct sigaction_k *act,
                               const struct signal_info *info, uint64_t blocked_before);

/* --- sending (any thread context; wakes blocked targets) --- */
/* To the process: a thread not blocking `sig` receives it. `info` may be NULL (a kernel source). */
int signal_send(struct process *p, int sig, const struct signal_info *info);
/* To one thread. */
int signal_send_thread(struct thread *t, int sig, const struct signal_info *info);
/* A fault on the calling thread, with the frame at hand: delivered before
 * the trap returns (a handler) or fatal now. Returns only when a handler
 * frame was set up. */
void signal_fault(int sig, uint64_t addr, struct arch_trap_frame *frame);
void signal_fault_info(const struct signal_info *info, struct arch_trap_frame *frame);

/* --- state --- */
int signal_set_action(struct process *p, int sig, const struct sigaction_k *act, struct sigaction_k *old);
void signal_get_action(struct process *p, int sig, struct sigaction_k *out);
/* The calling thread's blocked set (SIGKILL/SIGSTOP never block). */
uint64_t signal_blocked(void);
void signal_set_blocked(uint64_t mask);
uint64_t signal_pending_set(void);   /* pending on the thread or the process, blocked or not */

/* Consume the lowest pending signal in `mask` (thread- or process-directed,
 * blocked or not) for the current thread into *out; true if one was found.
 * *from_shared says which set it came from, so signal_reinject_sets can put
 * it back there on a failed copy. Clears only the pending bit, not the
 * siginfo slot. For signalfd(2). */
bool signal_consume_mask(uint64_t mask, struct signal_info *out, bool *from_shared);
/* Put back signals signal_consume_mask took but a signalfd could not copy to
 * user: re-set the pending bit of each signal in `thr`/`shr`, skipping any a
 * newer instance has already re-made pending. For signalfd(2). */
void signal_reinject_sets(uint64_t thr, uint64_t shr);
/* A call that swaps the mask while it waits (rt_sigsuspend, ppoll):
 * the mask the handler's frame records, and the one restored when no
 * handler runs, is `saved`, not the temporary one. */
void signal_set_blocked_saved(uint64_t saved);

/* A handler that must not be restarted after -EINTR sets the thread's
 * syscall_nr to this (rt_sigreturn: the restored result register is the
 * interrupted code's, not a result). */
#define SIGNAL_NO_RESTART (~0ull)

/* --- delivery points --- */
/* True when the calling thread has a signal it can take now, or the
 * process is ending: killable waits return -EINTR. */
bool signal_pending(void);
/* At a return to user mode: deliver what is pending on `frame` (a
 * system-call frame when `is_syscall`, else a trap frame). May not
 * return (a fatal default, an exiting process). */
void signal_deliver(void *frame, bool is_syscall);
/* rt_sigreturn: `regs` came from the user frame; the core sanitises,
 * restores the blocked mask and writes them back to the syscall frame. */
void signal_return(void *syscall_frame, const struct arch_user_regs *regs, uint64_t blocked);
/* Wait until a signal is delivered (sigsuspend/pause): -EINTR always. */
int signal_wait(void);
/* Default disposition of a signal: 0 terminate, 1 ignore. */
int signal_default_is_ignore(int sig);
/* ... and the third outcome: the stop signals, whose default parks the
 * process until a SIGCONT (docs/kernel/process/design.md, "Stopping").
 * SIGSTOP additionally cannot be caught or blocked. */
int signal_default_is_stop(int sig);
/* Raise a stop signal on the calling process and say whether anything
 * will come of it: false when it is ignored or blocked, so nothing will
 * stop and no handler will run. The test and the send are one critical
 * section -- asking first and sending after is a race a sibling thread
 * can win. */
bool signal_raise_stop_self(int sig, const struct signal_info *info);

/* Process-side setup and teardown (kernel/process/process.c). */
int signal_process_init(struct process *p);
void signal_process_release(struct process *p);

#endif /* KERNEL_SIGNAL_H */
