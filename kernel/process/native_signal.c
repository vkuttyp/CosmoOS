/*
 * native_signal.c - The native personality's signal ABI
 * (docs/kernel/process/design.md, "The native signal ABI").
 *
 * The core above this file decides everything about a signal except one
 * thing: the shape of the frame a handler runs on. That belongs to the
 * personality, and where the Linux personality inherits Linux's layout
 * bit for bit, this one is free to choose -- so it chooses the smallest
 * frame `sigreturn` can restore exactly:
 *
 *     magic | mask | registers | FP/SIMD image | siginfo
 *
 * pushed on the thread's own stack. There is no alternate signal stack,
 * so there is no choice to make about where it goes: a handler that
 * overflows the stack it was called on belongs to a program that was
 * already out of stack.
 *
 * The frame is opaque. No uapi struct describes it, so nothing outside
 * this file can depend on the layout; a handler is handed the signal
 * number, a `struct cosmo_siginfo *`, and the frame's address only so
 * that it has one to pass back. `sigreturn` finds it from the stack
 * pointer, as Linux does, and trusts none of it: a program can point
 * its stack pointer at bytes it wrote itself, so the magic is checked
 * and the registers go through the core's sanitiser before they are
 * loaded.
 *
 * Not carried, and named here so that the omissions are visible:
 * real-time signals, a queue of siginfo per signal, and the alternate
 * stack. Nothing in the native userland wants them yet.
 */

#include <kernel/errno.h>
#include <kernel/log.h>
#include <kernel/process.h>
#include <kernel/signal.h>
#include <kernel/string.h>
#include <kernel/syscall.h>
#include <kernel/thread.h>
#include <kernel/uaccess.h>

#include <arch/user.h>

#include <uapi/cosmo/syscall.h>

#define NATIVE_SIGFRAME_MAGIC 0x436f736d6f536967ull   /* "CosmoSig" */

/* Room for the FP/SIMD image the architecture hands out (x86-64: the
 * 512-byte FXSAVE area; AArch64: 32 vector registers plus FPSR and
 * FPCR), with headroom for a wider one. The frame records the length it
 * actually carries rather than assuming the size here, because getting
 * that wrong costs a handler the interrupted code's vector registers
 * and says nothing while it does it -- so the one case the length
 * cannot express, an image too large to fit, is reported. */
#define NATIVE_FPU_MAX 640u

struct native_sigframe {
    uint64_t magic;
    uint64_t mask;                 /* the blocked set the return restores */
    struct arch_user_regs regs;    /* what the signal interrupted */
    uint32_t fpu_len;              /* 0: the thread owns no FP/SIMD state */
    uint32_t pad;
    uint8_t fpu[NATIVE_FPU_MAX];
    struct cosmo_siginfo info;
};

static void fill_siginfo(struct cosmo_siginfo *si, const struct signal_info *info)
{
    memset(si, 0, sizeof(*si));
    si->sig = info->sig;
    switch (info->source) {
    case SIGSRC_USER:
    case SIGSRC_TKILL:
        si->code = COSMO_SI_USER;
        si->pid = (int32_t)info->sender_pid;
        break;
    case SIGSRC_FAULT:
        si->code = COSMO_SI_FAULT;
        si->detail = info->code;
        si->addr = info->fault_addr;
        break;
    case SIGSRC_KERNEL:
    default:
        si->code = COSMO_SI_KERNEL;
        break;
    }
}

/* Fill everything that does not depend on the architecture. */
static void fill_frame(struct native_sigframe *f, const struct arch_user_regs *r,
                       const struct signal_info *info, uint64_t blocked_before)
{
    memset(f, 0, sizeof(*f));
    f->magic = NATIVE_SIGFRAME_MAGIC;
    f->mask = blocked_before;
    f->regs = *r;
    size_t fpu_len = arch_user_fpu_image_size();
    if (fpu_len > sizeof(f->fpu)) {
        static bool warned;
        if (!warned) {
            warned = true;
            kwarn("signal: the FP/SIMD image is %zu bytes and the frame holds %zu; handlers will clobber it",
                  fpu_len, sizeof(f->fpu));
        }
    } else if (fpu_len > 0 && arch_user_fpu_image_save(f->fpu)) {
        f->fpu_len = (uint32_t)fpu_len;
    }
    fill_siginfo(&f->info, info);
}

/* The other half: put the interrupted registers back. The FP/SIMD image
 * goes back only if the frame still carries one -- a handler is free to
 * have used vector registers of its own, and the interrupted code's are
 * what it must find on return. */
static void restore_frame(void *syscall_frame, struct native_sigframe *f)
{
    if (f->fpu_len != 0 && f->fpu_len == arch_user_fpu_image_size())
        (void)arch_user_fpu_image_restore(f->fpu);
    /* The core sanitises the flag register and marks the frame for a
     * full restore, which is also what catches a program-chosen program
     * counter the fast return path could not load. */
    signal_return(syscall_frame, &f->regs, f->mask);
}

/* A frame the magic does not vouch for: the program pointed its stack
 * pointer somewhere else, and gets the SIGSEGV it asked for. */
static int64_t bad_frame(struct thread *t, uint64_t addr)
{
    struct signal_info info = { .sig = SIGSEGV, .source = SIGSRC_FAULT, .fault_addr = addr, .code = 1 };
    signal_send_thread(t, SIGSEGV, &info);
    t->syscall_nr = SIGNAL_NO_RESTART;
    return -EFAULT;
}

#if defined(ARCH_X86_64)

/*
 * The handler is entered as an ordinary call: the restorer's address is
 * pushed where a CALL would have left it, so a handler that simply
 * returns lands in the restorer, and the stack pointer at entry is
 * 8 mod 16 as the ABI promises. `sigreturn` therefore runs with rsp
 * exactly at the frame.
 */
int native_signal_frame(struct arch_user_regs *r, const struct sigaction_k *act,
                        const struct signal_info *info, uint64_t blocked_before)
{
    if (act->restorer == 0)
        return -EFAULT;   /* nothing to return through; the libc always sets one */

    struct native_sigframe f;
    fill_frame(&f, r, info, blocked_before);

    /* Below the red zone the ABI grants leaf functions, 16-byte aligned,
     * with the return address in the eight bytes under the frame. */
    uint64_t frame = ((r->rsp - 128) - sizeof(f)) & ~15ull;
    uint64_t sp = frame - 8;
    if (copy_to_user(frame, &f, sizeof(f)) || copy_to_user(sp, &act->restorer, 8))
        return -EFAULT;

    r->rdi = (uint64_t)info->sig;
    r->rsi = frame + offsetof(struct native_sigframe, info);
    r->rdx = frame;
    r->rax = 0;
    r->rsp = sp;
    r->rip = act->handler;
    r->rflags &= ~(0x400ull | 0x100ull | 0x10000ull);   /* DF, TF, RF */
    return 0;
}

static uint64_t sigreturn_frame_addr(const struct arch_user_regs *r)
{
    return r->rsp;   /* the handler's RET popped the restorer's address */
}

#elif defined(ARCH_AARCH64)

/*
 * The link register carries the restorer, so a handler returns through
 * it the way it returns from anything. Below the frame sits the frame
 * record -- the interrupted fp and lr -- so that a debugger unwinding
 * out of a handler finds the code the signal interrupted.
 */
int native_signal_frame(struct arch_user_regs *r, const struct sigaction_k *act,
                        const struct signal_info *info, uint64_t blocked_before)
{
    if (act->restorer == 0)
        return -EFAULT;

    struct native_sigframe f;
    fill_frame(&f, r, info, blocked_before);

    uint64_t frame = (r->sp - sizeof(f)) & ~15ull;
    uint64_t sp = frame - 16;
    uint64_t record[2] = { r->x[29], r->x[30] };
    if (copy_to_user(frame, &f, sizeof(f)) || copy_to_user(sp, record, sizeof(record)))
        return -EFAULT;

    r->x[0] = (uint64_t)info->sig;
    r->x[1] = frame + offsetof(struct native_sigframe, info);
    r->x[2] = frame;
    r->x[29] = sp;
    r->x[30] = act->restorer;
    r->sp = sp;
    r->pc = act->handler;
    return 0;
}

static uint64_t sigreturn_frame_addr(const struct arch_user_regs *r)
{
    return r->sp + 16;   /* the restorer runs with sp at the frame record */
}

#else

int native_signal_frame(struct arch_user_regs *r, const struct sigaction_k *act,
                        const struct signal_info *info, uint64_t blocked_before)
{
    (void)r; (void)act; (void)info; (void)blocked_before;
    return -ENOSYS;
}

static uint64_t sigreturn_frame_addr(const struct arch_user_regs *r)
{
    (void)r;
    return 0;
}

#endif

/* --- the system calls ---------------------------------------------------- */

static bool valid_sig(int sig)
{
    return sig >= 1 && sig < COSMO_NSIG;
}

int64_t sys_sigaction(struct syscall_args *a)
{
    int sig = (int)a->a[0];
    if (!valid_sig(sig))
        return -EINVAL;
    struct process *p = process_current();
    struct sigaction_k old;
    if (a->a[1]) {
        /* The two a process may not take: a target that could block or
         * catch SIGKILL could not be killed, and one that caught SIGSTOP
         * could not be stopped. */
        if (sig == SIGKILL || sig == SIGSTOP)
            return -EINVAL;
        struct cosmo_sigaction u;
        if (copy_from_user(&u, a->a[1], sizeof(u)))
            return -EFAULT;
        if (u.reserved != 0 || (u.flags & ~(COSMO_SA_NODEFER | COSMO_SA_RESETHAND | COSMO_SA_RESTART)))
            return -EINVAL;
        if (u.handler != COSMO_SIG_DFL && u.handler != COSMO_SIG_IGN && u.restorer == 0)
            return -EINVAL;   /* a handler with no way back */
        struct sigaction_k act = { .handler = u.handler, .flags = u.flags, .restorer = u.restorer,
                                   .mask = u.mask };
        signal_set_action(p, sig, &act, &old);
    } else {
        signal_get_action(p, sig, &old);
    }
    if (a->a[2]) {
        struct cosmo_sigaction u = { .handler = old.handler, .mask = old.mask, .flags = (uint32_t)old.flags,
                                     .restorer = old.restorer };
        if (copy_to_user(a->a[2], &u, sizeof(u)))
            return -EFAULT;
    }
    return 0;
}

int64_t sys_sigprocmask(struct syscall_args *a)
{
    uint64_t old = signal_blocked();
    if (a->a[1]) {
        uint64_t set;
        if (copy_from_user(&set, a->a[1], sizeof(set)))
            return -EFAULT;
        switch ((int)a->a[0]) {
        case COSMO_SIG_BLOCK: signal_set_blocked(old | set); break;
        case COSMO_SIG_UNBLOCK: signal_set_blocked(old & ~set); break;
        case COSMO_SIG_SETMASK: signal_set_blocked(set); break;
        default: return -EINVAL;
        }
    }
    if (a->a[2] && copy_to_user(a->a[2], &old, sizeof(old)))
        return -EFAULT;
    return 0;
}

int64_t sys_sigpending(struct syscall_args *a)
{
    uint64_t set = signal_pending_set() & signal_blocked();
    return copy_to_user(a->a[0], &set, sizeof(set)) ? -EFAULT : 0;
}

int64_t sys_sigreturn(struct syscall_args *a)
{
    struct thread *t = thread_current();
    struct arch_user_regs r;
    arch_user_regs_from_syscall(a->frame, &r);
    uint64_t addr = sigreturn_frame_addr(&r);
    struct native_sigframe f;
    if (copy_from_user(&f, addr, sizeof(f)) || f.magic != NATIVE_SIGFRAME_MAGIC)
        return bad_frame(t, addr);
    restore_frame(a->frame, &f);
    /* The restored result register belongs to the interrupted code; it
     * is not this call's result and must not be restarted over. */
    t->syscall_nr = SIGNAL_NO_RESTART;
    return arch_user_regs_result(&f.regs);
}
