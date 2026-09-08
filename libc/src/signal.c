/* signal.c - POSIX signals over the native ABI (docs/libc/design.md). */

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include "libc.h"

/* The kernel hands a handler the signal number, its `struct
 * cosmo_siginfo`, and the frame; siginfo_t is that struct under POSIX
 * names, so the pointer is passed straight through. If these ever drift
 * the program would read one field as another, which is why they are
 * checked here rather than in a comment. */
_Static_assert(sizeof(siginfo_t) == sizeof(struct cosmo_siginfo), "siginfo_t is the kernel's record");
_Static_assert(offsetof(siginfo_t, si_signo) == offsetof(struct cosmo_siginfo, sig), "si_signo");
_Static_assert(offsetof(siginfo_t, si_code) == offsetof(struct cosmo_siginfo, code), "si_code");
_Static_assert(offsetof(siginfo_t, si_pid) == offsetof(struct cosmo_siginfo, pid), "si_pid");
_Static_assert(offsetof(siginfo_t, si_detail) == offsetof(struct cosmo_siginfo, detail), "si_detail");
_Static_assert(offsetof(siginfo_t, si_addr) == offsetof(struct cosmo_siginfo, addr), "si_addr");

/*
 * The restorer. A handler returns to it, and it makes the one system
 * call that never returns: the kernel takes the frame from the stack
 * pointer, so this must not touch the stack -- which is why it is
 * assembly and not a C function with a naked attribute.
 */
#define STR2(x) #x
#define STR(x) STR2(x)
#if defined(__x86_64__)
__asm__(".text\n"
        ".globl __cosmo_sigreturn\n"
        ".hidden __cosmo_sigreturn\n"
        ".type __cosmo_sigreturn,@function\n"
        "__cosmo_sigreturn:\n"
        "    movl $" STR(SYS_sigreturn) ", %eax\n"
        "    syscall\n"
        "    ud2\n"                     /* it does not return */
        ".size __cosmo_sigreturn, .-__cosmo_sigreturn\n");
#elif defined(__aarch64__)
__asm__(".text\n"
        ".globl __cosmo_sigreturn\n"
        ".hidden __cosmo_sigreturn\n"
        ".type __cosmo_sigreturn,@function\n"
        "__cosmo_sigreturn:\n"
        "    mov x8, #" STR(SYS_sigreturn) "\n"
        "    svc #0\n"
        "    brk #0\n"
        ".size __cosmo_sigreturn, .-__cosmo_sigreturn\n");
#endif
void __cosmo_sigreturn(void);

int sigemptyset(sigset_t *set)
{
    *set = 0;
    return 0;
}

int sigfillset(sigset_t *set)
{
    *set = ~0ul;
    return 0;
}

static int check_sig(int sig)
{
    if (sig < 1 || sig >= NSIG) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int sigaddset(sigset_t *set, int sig)
{
    if (check_sig(sig))
        return -1;
    *set |= 1ul << (sig - 1);
    return 0;
}

int sigdelset(sigset_t *set, int sig)
{
    if (check_sig(sig))
        return -1;
    *set &= ~(1ul << (sig - 1));
    return 0;
}

int sigismember(const sigset_t *set, int sig)
{
    if (check_sig(sig))
        return -1;
    return (*set >> (sig - 1)) & 1u;
}

int sigaction(int sig, const struct sigaction *act, struct sigaction *old)
{
    struct cosmo_sigaction k, ko;
    if (act != NULL) {
        memset(&k, 0, sizeof(k));
        k.handler = (uint64_t)(uintptr_t)act->sa_handler;
        k.mask = act->sa_mask;
        k.flags = act->sa_flags & ~SA_SIGINFO;   /* every handler gets all three arguments */
        /* SIG_DFL and SIG_IGN are not addresses and need no restorer;
         * the kernel refuses a real handler without one. */
        if (k.handler != (uint64_t)COSMO_SIG_DFL && k.handler != (uint64_t)COSMO_SIG_IGN)
            k.restorer = (uint64_t)(uintptr_t)__cosmo_sigreturn;
    }
    long r = cosmo_sigaction(sig, act ? &k : NULL, old ? &ko : NULL);
    if (r < 0)
        return (int)__syscall_ret(r);
    if (old != NULL) {
        memset(old, 0, sizeof(*old));
        old->sa_handler = (void (*)(int))(uintptr_t)ko.handler;
        old->sa_mask = ko.mask;
        old->sa_flags = ko.flags;
    }
    return 0;
}

void (*signal(int sig, void (*handler)(int)))(int)
{
    struct sigaction act, old;
    memset(&act, 0, sizeof(act));
    act.sa_handler = handler;
    act.sa_flags = SA_RESTART;   /* what C's signal() has meant since BSD */
    if (sigaction(sig, &act, &old) != 0)
        return SIG_ERR;
    return old.sa_handler;
}

int sigprocmask(int how, const sigset_t *set, sigset_t *old)
{
    uint64_t s, o;
    if (set != NULL)
        s = *set;
    long r = cosmo_sigprocmask(how, set ? &s : NULL, old ? &o : NULL);
    if (r < 0)
        return (int)__syscall_ret(r);
    if (old != NULL)
        *old = o;
    return 0;
}

int sigpending(sigset_t *set)
{
    uint64_t s;
    long r = cosmo_sigpending(&s);
    if (r < 0)
        return (int)__syscall_ret(r);
    *set = s;
    return 0;
}

int raise(int sig)
{
    return kill(getpid(), sig);
}
