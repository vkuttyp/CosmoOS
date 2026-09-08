/*
 * signal.h - POSIX signals (docs/libc/design.md, "Signals").
 *
 * The kernel's ABI is `struct cosmo_sigaction` and a restorer the
 * program supplies; this header is the POSIX face of it, and
 * libc/src/signal.c owns the restorer so that no program has to write
 * one. `siginfo_t` is the kernel's `struct cosmo_siginfo` under POSIX
 * names -- the same bytes, asserted field by field, so that a handler
 * reads the kernel's own record rather than a copy of it.
 *
 * Not here, because the kernel does not have them: real-time signals,
 * queued siginfo, sigaltstack and SA_ONSTACK, sigsuspend, sigwait.
 */
#ifndef _SIGNAL_H
#define _SIGNAL_H
#include <sys/types.h>
#include <uapi/cosmo/syscall.h>

#define SIGHUP COSMO_SIGHUP
#define SIGINT COSMO_SIGINT
#define SIGQUIT COSMO_SIGQUIT
#define SIGILL COSMO_SIGILL
#define SIGTRAP COSMO_SIGTRAP
#define SIGABRT COSMO_SIGABRT
#define SIGBUS COSMO_SIGBUS
#define SIGFPE COSMO_SIGFPE
#define SIGKILL COSMO_SIGKILL
#define SIGUSR1 COSMO_SIGUSR1
#define SIGSEGV COSMO_SIGSEGV
#define SIGUSR2 COSMO_SIGUSR2
#define SIGPIPE COSMO_SIGPIPE
#define SIGALRM COSMO_SIGALRM
#define SIGTERM COSMO_SIGTERM
#define SIGCHLD COSMO_SIGCHLD
#define SIGCONT COSMO_SIGCONT
#define SIGSTOP COSMO_SIGSTOP
#define SIGTSTP COSMO_SIGTSTP
#define SIGTTIN COSMO_SIGTTIN
#define SIGTTOU COSMO_SIGTTOU
#define SIGSYS COSMO_SIGSYS
#define NSIG COSMO_NSIG

typedef unsigned long sigset_t;

#define SIG_DFL ((void (*)(int))COSMO_SIG_DFL)
#define SIG_IGN ((void (*)(int))COSMO_SIG_IGN)
#define SIG_ERR ((void (*)(int))-1)

#define SA_NODEFER COSMO_SA_NODEFER
#define SA_RESETHAND COSMO_SA_RESETHAND
#define SA_RESTART COSMO_SA_RESTART
/* Accepted and stripped: the kernel hands every handler the signal, the
 * siginfo and the frame, so a three-argument handler needs no flag. It
 * exists so that code written for POSIX compiles unchanged. */
#define SA_SIGINFO 0x00000004u

#define SIG_BLOCK COSMO_SIG_BLOCK
#define SIG_UNBLOCK COSMO_SIG_UNBLOCK
#define SIG_SETMASK COSMO_SIG_SETMASK

/* si_code */
#define SI_USER COSMO_SI_USER
#define SI_KERNEL COSMO_SI_KERNEL
#define SI_FAULT COSMO_SI_FAULT

typedef struct {
    int si_signo;
    int si_code;
    pid_t si_pid;      /* SI_USER: who sent it */
    unsigned si_detail; /* SI_FAULT: 1 nothing is mapped there, 2 a protection fault */
    void *si_addr;     /* SI_FAULT: the address the fault names */
} siginfo_t;

struct sigaction {
    union {
        void (*sa_handler)(int);
        void (*sa_sigaction)(int, siginfo_t *, void *);
    };
    sigset_t sa_mask;
    unsigned sa_flags;
};

int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int sig);
int sigdelset(sigset_t *set, int sig);
int sigismember(const sigset_t *set, int sig);

int sigaction(int sig, const struct sigaction *act, struct sigaction *old);
void (*signal(int sig, void (*handler)(int)))(int);
int sigprocmask(int how, const sigset_t *set, sigset_t *old);
int sigpending(sigset_t *set);
int raise(int sig);
int kill(pid_t pid, int sig);

#endif
