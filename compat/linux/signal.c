/*
 * signal.c - Linux signals over the kernel's signal core
 * (docs/compat/linux/design.md, stage 2; kernel/process/signal.c).
 *
 * The core owns pending sets, masks, actions and the delivery points.
 * This file is the Linux view of them: the system calls, the rt_sigframe
 * a handler runs on (the x86-64 and AArch64 layouts of Linux, so that
 * musl's and glibc's restorers and any hand-written handler work), the
 * return through rt_sigreturn, and the trampoline page a handler without
 * SA_RESTORER returns through on AArch64.
 */

#include <kernel/errno.h>
#include <kernel/futex.h>
#include <kernel/log.h>
#include <kernel/page.h>
#include <kernel/process.h>
#include <kernel/signal.h>
#include <kernel/string.h>
#include <kernel/syscall.h>
#include <kernel/thread.h>
#include <kernel/uaccess.h>
#include <kernel/vmm.h>
#include <arch/mmu.h>
#include <arch/user.h>

#include "linux_abi.h"
#include "linux_internal.h"

_Static_assert(sizeof(struct lx_siginfo) == 128, "siginfo is 128 bytes");
_Static_assert(sizeof(struct lx_sigcontext_x86) == 256, "x86-64 sigcontext is 256 bytes");
_Static_assert(sizeof(struct lx_ucontext_x86) == 304, "x86-64 ucontext is 304 bytes");
_Static_assert(sizeof(struct lx_rt_sigframe_x86) == 440, "x86-64 rt_sigframe is 440 bytes");
_Static_assert(sizeof(struct lx_sigcontext_a64) == 4384, "AArch64 sigcontext is 4384 bytes");
_Static_assert(sizeof(struct lx_ucontext_a64) == 4560, "AArch64 ucontext is 4560 bytes");
_Static_assert(sizeof(struct lx_rt_sigframe_a64) == 4688, "AArch64 rt_sigframe is 4688 bytes");

/* --- the alternate stack ------------------------------------------------------ */

static bool altstack_enabled(const struct thread *t)
{
    return t->altstack.size != 0 && !(t->altstack.flags & LX_SS_DISABLE);
}

/* Linux's on_sig_stack: (base, base + size]. */
static bool on_altstack(const struct thread *t, uint64_t sp)
{
    return altstack_enabled(t) && sp > t->altstack.sp && sp - t->altstack.sp <= t->altstack.size;
}

static struct lx_stack_t altstack_out(const struct thread *t, uint64_t sp)
{
    struct lx_stack_t st = { .ss_sp = t->altstack.sp, .ss_size = t->altstack.size };
    st.ss_flags = on_altstack(t, sp) ? LX_SS_ONSTACK : altstack_enabled(t) ? 0 : LX_SS_DISABLE;
    return st;
}

/* --- siginfo ------------------------------------------------------------------ */

static void fill_siginfo(struct lx_siginfo *si, const struct signal_info *info)
{
    memset(si, 0, sizeof(*si));
    si->si_signo = info->sig;
    switch (info->source) {
    case SIGSRC_USER:
        si->si_code = LX_SI_USER;
        si->u.kill.pid = (int32_t)info->sender_pid;
        si->u.kill.uid = info->sender_uid;
        break;
    case SIGSRC_TKILL:
        si->si_code = LX_SI_TKILL;
        si->u.kill.pid = (int32_t)info->sender_pid;
        si->u.kill.uid = info->sender_uid;
        break;
    case SIGSRC_FAULT:
        /* SEGV_MAPERR/ACCERR, ILL_ILLOPC, FPE_INTDIV, TRAP_BRKPT are all 1;
         * the core's `code` is the MAPERR/ACCERR distinction. */
        si->si_code = info->code ? (int32_t)info->code : 1;
        si->u.fault.addr = info->fault_addr;
        break;
    case SIGSRC_KERNEL:
    default:
        si->si_code = LX_SI_KERNEL;
        break;
    }
}

/* --- the handler frame -------------------------------------------------------- */

/* Where the frame goes: the alternate stack when the action asks for it
 * and the thread is not already on it, else below the current stack. */
static uint64_t frame_base(const struct thread *t, const struct sigaction_k *act, uint64_t sp)
{
    if ((act->flags & SA_ONSTACK) && altstack_enabled(t) && !on_altstack(t, sp))
        return t->altstack.sp + t->altstack.size;
    return sp;
}

#if defined(ARCH_X86_64)
int linux_signal_frame(struct arch_user_regs *r, const struct sigaction_k *act, const struct signal_info *info,
                       uint64_t blocked_before)
{
    struct thread *t = thread_current();
    /* Linux refuses a frame for a handler without a restorer: SIGSEGV. The
     * libcs always set one; the trampoline page exists for AArch64. */
    if (!(act->flags & SA_RESTORER))
        return -EFAULT;
    uint64_t user_sp = r->rsp;
    uint64_t sp = frame_base(t, act, user_sp);
    if (sp == user_sp)
        sp -= 128;   /* the red zone the ABI grants leaf functions */

    uint8_t fx[512];
    uint64_t fpstate = 0;
    if (arch_user_fpu_image_size() == sizeof(fx) && arch_user_fpu_image_save(fx)) {
        sp = (sp - sizeof(fx)) & ~63ull;
        fpstate = sp;
    }
    sp = ((sp - sizeof(struct lx_rt_sigframe_x86)) & ~15ull) - 8;   /* as after a CALL: rsp ≡ 8 mod 16 */

    struct lx_rt_sigframe_x86 f;
    memset(&f, 0, sizeof(f));
    f.pretcode = act->restorer;
    f.uc.uc_flags = LX_UC_SIGCONTEXT_SS | LX_UC_STRICT_RESTORE_SS;
    f.uc.uc_stack = altstack_out(t, user_sp);
    f.uc.uc_sigmask = blocked_before;
    struct lx_sigcontext_x86 *mc = &f.uc.uc_mcontext;
    mc->r8 = r->r8; mc->r9 = r->r9; mc->r10 = r->r10; mc->r11 = r->r11;
    mc->r12 = r->r12; mc->r13 = r->r13; mc->r14 = r->r14; mc->r15 = r->r15;
    mc->rdi = r->rdi; mc->rsi = r->rsi; mc->rbp = r->rbp; mc->rbx = r->rbx;
    mc->rdx = r->rdx; mc->rax = r->rax; mc->rcx = r->rcx; mc->rsp = r->rsp;
    mc->rip = r->rip; mc->eflags = r->rflags;
    mc->oldmask = blocked_before;
    mc->cr2 = info->source == SIGSRC_FAULT ? info->fault_addr : 0;
    mc->trapno = info->source == SIGSRC_FAULT && info->sig == SIGSEGV ? 14 : 0;
    mc->fpstate = fpstate;
    fill_siginfo(&f.info, info);

    if (fpstate && copy_to_user(fpstate, fx, sizeof(fx)))
        return -EFAULT;
    if (copy_to_user(sp, &f, sizeof(f)))
        return -EFAULT;

    r->rdi = (uint64_t)info->sig;
    r->rsi = sp + offsetof(struct lx_rt_sigframe_x86, info);
    r->rdx = sp + offsetof(struct lx_rt_sigframe_x86, uc);
    r->rax = 0;
    r->rsp = sp;
    r->rip = act->handler;
    r->rflags &= ~(0x400ull | 0x100ull | 0x10000ull);   /* DF, TF, RF */
    return 0;
}

/* The handler returned through the restorer: rsp points at `uc` (the
 * RET popped pretcode). A frame that cannot be read is a SIGSEGV on the
 * thread, delivered at this call's exit. */
int64_t lx_rt_sigreturn(struct syscall_args *a)
{
    struct thread *t = thread_current();
    struct arch_user_regs r;
    arch_user_regs_from_syscall(a->frame, &r);
    struct lx_ucontext_x86 uc;
    uint8_t fx[512];
    if (copy_from_user(&uc, r.rsp, sizeof(uc)) ||
        (uc.uc_mcontext.fpstate != 0 && copy_from_user(fx, uc.uc_mcontext.fpstate, sizeof(fx)))) {
        struct signal_info info = { .sig = SIGSEGV, .source = SIGSRC_FAULT, .fault_addr = r.rsp, .code = 1 };
        signal_send_thread(t, SIGSEGV, &info);
        t->syscall_nr = SIGNAL_NO_RESTART;
        return -EFAULT;
    }
    const struct lx_sigcontext_x86 *mc = &uc.uc_mcontext;
    r.r8 = mc->r8; r.r9 = mc->r9; r.r10 = mc->r10; r.r11 = mc->r11;
    r.r12 = mc->r12; r.r13 = mc->r13; r.r14 = mc->r14; r.r15 = mc->r15;
    r.rdi = mc->rdi; r.rsi = mc->rsi; r.rbp = mc->rbp; r.rbx = mc->rbx;
    r.rdx = mc->rdx; r.rax = mc->rax; r.rcx = mc->rcx; r.rsp = mc->rsp;
    r.rip = mc->rip; r.rflags = mc->eflags;
    if (mc->fpstate != 0)
        arch_user_fpu_image_restore(fx);
    /* The core sanitises rflags and marks the frame for the full IRETQ
     * restore, which also catches a non-canonical rip (the SYSRET guard). */
    signal_return(a->frame, &r, uc.uc_sigmask);
    t->syscall_nr = SIGNAL_NO_RESTART;   /* the restored rax is not a result to restart */
    return (int64_t)r.rax;
}

#elif defined(ARCH_AARCH64)
int linux_signal_frame(struct arch_user_regs *r, const struct sigaction_k *act, const struct signal_info *info,
                       uint64_t blocked_before)
{
    struct thread *t = thread_current();
    uint64_t user_sp = r->sp;
    uint64_t sp = frame_base(t, act, user_sp);
    sp = (sp - sizeof(struct lx_rt_sigframe_a64)) & ~15ull;
    uint64_t frame = sp;
    sp -= 16;   /* the frame record: the interrupted fp and lr */

    struct lx_rt_sigframe_a64 f;
    memset(&f, 0, sizeof(f));
    fill_siginfo(&f.info, info);
    f.uc.uc_stack = altstack_out(t, user_sp);
    f.uc.uc_sigmask = blocked_before;
    struct lx_sigcontext_a64 *mc = &f.uc.uc_mcontext;
    mc->fault_address = info->source == SIGSRC_FAULT ? info->fault_addr : 0;
    memcpy(mc->regs, r->x, sizeof(mc->regs));
    mc->sp = r->sp;
    mc->pc = r->pc;
    mc->pstate = r->pstate;
    /* No fpsimd_context: FP/SIMD is off at EL0 (kernel/arch/aarch64/fpu.c).
     * An esr_context (syndrome 0: not carried yet) and the terminator. */
    struct lx_esr_context esr = { .magic = LX_ESR_MAGIC, .size = sizeof(esr), .esr = 0 };
    memcpy(mc->reserved, &esr, sizeof(esr));
    uint64_t record[2] = { r->x[29], r->x[30] };
    if (copy_to_user(frame, &f, sizeof(f)) || copy_to_user(sp, record, sizeof(record)))
        return -EFAULT;

    r->x[0] = (uint64_t)info->sig;
    r->x[1] = frame + offsetof(struct lx_rt_sigframe_a64, info);
    r->x[2] = frame + offsetof(struct lx_rt_sigframe_a64, uc);
    r->x[29] = sp;
    r->x[30] = (act->flags & SA_RESTORER) ? act->restorer : LX_SIGTRAMP;
    r->sp = sp;
    r->pc = act->handler;
    return 0;
}

int64_t lx_rt_sigreturn(struct syscall_args *a)
{
    struct thread *t = thread_current();
    struct arch_user_regs r;
    arch_user_regs_from_syscall(a->frame, &r);
    uint64_t frame = r.sp + 16;   /* the restorer runs with sp at the frame record */
    struct lx_ucontext_a64 uc;
    if (copy_from_user(&uc, frame + offsetof(struct lx_rt_sigframe_a64, uc), sizeof(uc))) {
        struct signal_info info = { .sig = SIGSEGV, .source = SIGSRC_FAULT, .fault_addr = frame, .code = 1 };
        signal_send_thread(t, SIGSEGV, &info);
        t->syscall_nr = SIGNAL_NO_RESTART;
        return -EFAULT;
    }
    memcpy(r.x, uc.uc_mcontext.regs, sizeof(r.x));
    r.sp = uc.uc_mcontext.sp;
    r.pc = uc.uc_mcontext.pc;
    r.pstate = uc.uc_mcontext.pstate;
    signal_return(a->frame, &r, uc.uc_sigmask);
    t->syscall_nr = SIGNAL_NO_RESTART;
    return (int64_t)r.x[0];
}
#endif

/* --- the trampoline page and the exit hook ------------------------------------- */

int linux_sigtramp_map(struct process *p)
{
#if defined(ARCH_X86_64)
    static const uint8_t code[] = { 0xb8, 0x0f, 0x00, 0x00, 0x00, 0x0f, 0x05 };   /* mov $15,%eax; syscall */
#else
    static const uint8_t code[] = { 0x68, 0x11, 0x80, 0xd2, 0x01, 0x00, 0x00, 0xd4 };   /* mov x8,#139; svc #0 */
#endif
    int rc = vm_user_map_anon(p->space, LX_SIGTRAMP, PAGE_SIZE, VM_PROT_RW, VM_REGION_POPULATED, "sigtramp");
    if (rc)
        return rc;
    paddr_t pa;
    if (!arch_mmu_query(&p->space->mmu, (vaddr_t)LX_SIGTRAMP, &pa, NULL, NULL, NULL))
        return -EFAULT;   /* populated: cannot happen */
    memcpy(phys_to_virt(pa), code, sizeof(code));
    return vm_user_protect(p->space, LX_SIGTRAMP, PAGE_SIZE, VM_PROT_RX);
}

/* set_tid_address / CLONE_CHILD_CLEARTID: zero the word, wake one waiter. */
void linux_thread_exit(struct thread *t)
{
    uint64_t addr = t->clear_child_tid;
    if (addr == 0 || t->proc == NULL || t->proc->space == NULL)
        return;
    t->clear_child_tid = 0;
    uint32_t zero = 0;
    if (copy_to_user(addr, &zero, sizeof(zero)) == 0)
        futex_wake(t->proc->space, addr, 1);
}

/* --- system calls -------------------------------------------------------------- */

static bool valid_sig(int sig)
{
    return sig >= 1 && sig < LX_NSIG;
}

int64_t lx_rt_sigaction(struct syscall_args *a)
{
    int sig = (int)a->a[0];
    if (!valid_sig(sig) || a->a[3] != 8)
        return -EINVAL;
    struct process *p = process_current();
    struct sigaction_k act, old;
    if (a->a[1]) {
        if (sig == SIGKILL || sig == SIGSTOP)
            return -EINVAL;
        if (copy_from_user(&act, a->a[1], sizeof(act)))
            return -EFAULT;
        signal_set_action(p, sig, &act, &old);
    } else {
        signal_get_action(p, sig, &old);
    }
    if (a->a[2] && copy_to_user(a->a[2], &old, sizeof(old)))
        return -EFAULT;
    return 0;
}

int64_t lx_rt_sigprocmask(struct syscall_args *a)
{
    if (a->a[3] != 8)
        return -EINVAL;
    uint64_t old = signal_blocked();
    if (a->a[1]) {
        uint64_t set;
        if (copy_from_user(&set, a->a[1], 8))
            return -EFAULT;
        switch ((int)a->a[0]) {
        case LX_SIG_BLOCK: signal_set_blocked(old | set); break;
        case LX_SIG_UNBLOCK: signal_set_blocked(old & ~set); break;
        case LX_SIG_SETMASK: signal_set_blocked(set); break;
        default: return -EINVAL;
        }
    }
    if (a->a[2] && copy_to_user(a->a[2], &old, 8))
        return -EFAULT;
    return 0;
}

int64_t lx_rt_sigpending(struct syscall_args *a)
{
    if (a->a[1] != 8)
        return -EINVAL;
    uint64_t set = signal_pending_set();
    return copy_to_user(a->a[0], &set, 8) ? -EFAULT : 0;
}

/* Wait with `mask` in place; the handler's frame records the previous
 * mask and the return restores it (the core's saved-mask rule). */
int64_t lx_rt_sigsuspend(struct syscall_args *a)
{
    if (a->a[1] != 8)
        return -EINVAL;
    uint64_t mask;
    if (copy_from_user(&mask, a->a[0], 8))
        return -EFAULT;
    uint64_t old = signal_blocked();
    signal_set_blocked(mask);
    signal_set_blocked_saved(old);
    return signal_wait();
}

int64_t lx_pause(struct syscall_args *a)
{
    (void)a;
    return signal_wait();
}

int64_t lx_sigaltstack(struct syscall_args *a)
{
    struct thread *t = thread_current();
    struct arch_user_regs r;
    arch_user_regs_from_syscall(a->frame, &r);
    uint64_t sp = arch_user_regs_sp(&r);
    struct lx_stack_t old = altstack_out(t, sp);
    if (a->a[0]) {
        struct lx_stack_t st;
        if (copy_from_user(&st, a->a[0], sizeof(st)))
            return -EFAULT;
        if (on_altstack(t, sp))
            return -EPERM;
        uint32_t flags = (uint32_t)st.ss_flags & ~LX_SS_AUTODISARM;
        if (flags != 0 && flags != LX_SS_DISABLE && flags != LX_SS_ONSTACK)
            return -EINVAL;
        if (flags == LX_SS_DISABLE) {
            t->altstack = (struct sigaltstack_k){ .flags = LX_SS_DISABLE };
        } else {
            if (st.ss_size < LX_MINSIGSTKSZ)
                return -ENOMEM;
            if (!user_range_ok(st.ss_sp, (size_t)st.ss_size))
                return -EFAULT;
            t->altstack = (struct sigaltstack_k){ .sp = st.ss_sp, .size = st.ss_size, .flags = 0 };
        }
    }
    if (a->a[1] && copy_to_user(a->a[1], &old, sizeof(old)))
        return -EFAULT;
    return 0;
}

static int64_t send_to_process(struct process *target, int sig, enum signal_source src)
{
    struct process *cur = process_current();
    if (!cred_may_signal(&cur->cred, &target->cred))
        return -EPERM;
    if (sig == 0)
        return 0;
    struct signal_info info = { .sig = sig, .source = src, .sender_pid = cur->pid, .sender_uid = cur->cred.ruid };
    return signal_send(target, sig, &info);
}

int64_t lx_kill(struct syscall_args *a)
{
    int pid = (int)a->a[0], sig = (int)a->a[1];
    if (pid <= 0)
        return -ESRCH;   /* process groups do not exist */
    if (sig != 0 && !valid_sig(sig))
        return -EINVAL;
    struct process *target = process_lookup((pid_t)pid);
    if (target == NULL)
        return -ESRCH;
    int64_t rc = send_to_process(target, sig, SIGSRC_USER);
    process_put(target);
    return rc;
}

static int64_t send_to_thread(struct process *target, uint32_t tid, int sig)
{
    struct process *cur = process_current();
    if (!cred_may_signal(&cur->cred, &target->cred))
        return -EPERM;
    struct thread *t = process_find_thread(target, tid);
    if (t == NULL)
        return -ESRCH;
    if (sig == 0)
        return 0;
    struct signal_info info = { .sig = sig, .source = SIGSRC_TKILL, .sender_pid = cur->pid, .sender_uid = cur->cred.ruid };
    return signal_send_thread(t, sig, &info);
}

int64_t lx_tgkill(struct syscall_args *a)
{
    int tgid = (int)a->a[0], tid = (int)a->a[1], sig = (int)a->a[2];
    if (tgid <= 0 || tid <= 0)
        return -EINVAL;
    if (sig != 0 && !valid_sig(sig))
        return -EINVAL;
    struct process *target = process_lookup((pid_t)tgid);
    if (target == NULL)
        return -ESRCH;
    int64_t rc = send_to_thread(target, (uint32_t)tid, sig);
    process_put(target);
    return rc;
}

/* tkill names a thread without its group: the caller's own process is the
 * only one whose threads are searched (thread ids are per process here). */
int64_t lx_tkill(struct syscall_args *a)
{
    int tid = (int)a->a[0], sig = (int)a->a[1];
    if (tid <= 0)
        return -EINVAL;
    if (sig != 0 && !valid_sig(sig))
        return -EINVAL;
    struct process *cur = process_current();
    if ((uint32_t)tid == cur->pid || (uint32_t)tid >= 0x10000u)
        return send_to_thread(cur, (uint32_t)tid, sig);
    struct process *target = process_lookup((pid_t)tid);   /* another process's main thread */
    if (target == NULL)
        return -ESRCH;
    int64_t rc = send_to_thread(target, (uint32_t)tid, sig);
    process_put(target);
    return rc;
}

int64_t lx_gettid(struct syscall_args *a)
{
    (void)a;
    return thread_current()->lx_tid;
}

int64_t lx_set_tid_address(struct syscall_args *a)
{
    struct thread *t = thread_current();
    t->clear_child_tid = a->a[0];
    return t->lx_tid;
}

/* exit ends the calling thread; the process ends with the last one. */
int64_t lx_exit(struct syscall_args *a)
{
    process_thread_exit((int)a->a[0] & 0xff);
}

int64_t lx_exit_group(struct syscall_args *a)
{
    process_exit((int)a->a[0] & 0xff);
}
