/*
 * signal.c - Linux signals over the kernel's signal core
 * (docs/compat/linux/design.md, stage 2; kernel/process/signal.c).
 *
 * The core owns pending sets, masks, actions and the delivery points.
 * This file is the Linux view of them: the rt_sigframe a handler runs on
 * (the x86-64 and AArch64 layouts of Linux, so that musl's and glibc's
 * restorers and any hand-written handler work), the return through
 * rt_sigreturn, and the trampoline page a handler without SA_RESTORER
 * returns through on AArch64. The system calls are sigcalls.cee's
 * (roadmap 2.0 SA8), which keeps its own copy of the alternate-stack
 * helpers below.
 */

#include <kernel/compiler.h>
#include <kernel/errno.h>
#include <kernel/futex.h>
#include <kernel/log.h>
#include <kernel/page.h>
#include <kernel/process.h>
#include <kernel/random.h>
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
    case SIGSRC_CHILD:
        /* SIGCHLD: the CLD_* cause (SI_CLD_* match the Linux values), the
         * child's pid/uid and its exit code or killing/stop signal. */
        si->si_code = (int32_t)info->code;
        si->u.child.pid = (int32_t)info->sender_pid;
        si->u.child.uid = info->sender_uid;
        si->u.child.status = info->status;
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
/*
 * The AArch64 frame is about 4.7 KiB, 4 KiB of it the reserved area that
 * holds the context records, so it is never assembled on the 16 KiB kernel
 * stack (scripts/frame-baseline.txt): delivery zeroes the frame in user
 * memory and writes each part at its offset, and the return reads back
 * only the parts it uses. What the handler finds, and what the return
 * accepts and refuses, are the same as when the frame was built whole.
 */
#define A64_OFF(member) offsetof(struct lx_rt_sigframe_a64, member)

static const uint8_t a64_zero[256];

static int zero_user(uint64_t dst, size_t len)
{
    while (len) {
        size_t n = len < sizeof(a64_zero) ? len : sizeof(a64_zero);
        if (copy_to_user(dst, a64_zero, n))
            return -EFAULT;
        dst += n;
        len -= n;
    }
    return 0;
}

/* Fault exactly where a copy of all `len` bytes would have. */
static int probe_user(uint64_t src, size_t len)
{
    uint8_t buf[256];
    while (len) {
        size_t n = len < sizeof(buf) ? len : sizeof(buf);
        if (copy_from_user(buf, src, n))
            return -EFAULT;
        src += n;
        len -= n;
    }
    return 0;
}

int linux_signal_frame(struct arch_user_regs *r, const struct sigaction_k *act, const struct signal_info *info,
                       uint64_t blocked_before)
{
    struct thread *t = thread_current();
    uint64_t user_sp = r->sp;
    uint64_t sp = frame_base(t, act, user_sp);
    sp = (sp - sizeof(struct lx_rt_sigframe_a64)) & ~15ull;
    uint64_t frame = sp;
    sp -= 16;   /* the frame record: the interrupted fp and lr */

    struct lx_siginfo si;
    fill_siginfo(&si, info);
    struct lx_stack_t ss = altstack_out(t, user_sp);
    uint64_t fault = info->source == SIGSRC_FAULT ? info->fault_addr : 0;
    /*
     * The reserved area holds a list of records: the FP/SIMD state, then
     * an esr_context (syndrome 0: not carried yet), then the terminator.
     * A handler that uses vector registers -- which is every handler in
     * a program built by an ordinary toolchain -- would otherwise return
     * into an interrupted function whose registers it had overwritten.
     */
    struct lx_fpsimd_context fp;
    memset(&fp, 0, sizeof(fp));
    fp.magic = LX_FPSIMD_MAGIC;
    fp.size = sizeof(fp);
    bool have_fp = arch_user_fpu_image_size() == sizeof(fp) - offsetof(struct lx_fpsimd_context, fpsr) &&
                   arch_user_fpu_image_save(&fp.fpsr);
    struct lx_esr_context esr = { .magic = LX_ESR_MAGIC, .size = sizeof(esr), .esr = 0 };
    uint64_t rsv = frame + A64_OFF(uc.uc_mcontext.reserved);
    uint64_t esr_at = rsv + (have_fp ? sizeof(fp) : 0);
    /* The terminator is eight zero bytes after the last record, which the
     * zeroing already wrote. */
    uint64_t record[2] = { r->x[29], r->x[30] };
    if (zero_user(frame, sizeof(struct lx_rt_sigframe_a64)) ||
        copy_to_user(frame + A64_OFF(info), &si, sizeof(si)) ||
        copy_to_user(frame + A64_OFF(uc.uc_stack), &ss, sizeof(ss)) ||
        copy_to_user(frame + A64_OFF(uc.uc_sigmask), &blocked_before, sizeof(blocked_before)) ||
        copy_to_user(frame + A64_OFF(uc.uc_mcontext.fault_address), &fault, sizeof(fault)) ||
        copy_to_user(frame + A64_OFF(uc.uc_mcontext.regs), r->x, sizeof(r->x)) ||
        copy_to_user(frame + A64_OFF(uc.uc_mcontext.sp), &r->sp, sizeof(r->sp)) ||
        copy_to_user(frame + A64_OFF(uc.uc_mcontext.pc), &r->pc, sizeof(r->pc)) ||
        copy_to_user(frame + A64_OFF(uc.uc_mcontext.pstate), &r->pstate, sizeof(r->pstate)) ||
        (have_fp && copy_to_user(rsv, &fp, sizeof(fp))) ||
        copy_to_user(esr_at, &esr, sizeof(esr)) ||
        copy_to_user(sp, record, sizeof(record)))
        return -EFAULT;

    r->x[0] = (uint64_t)info->sig;
    r->x[1] = frame + A64_OFF(info);
    r->x[2] = frame + A64_OFF(uc);
    r->x[29] = sp;
    r->x[30] = (act->flags & SA_RESTORER) ? act->restorer : linux_sigtramp_addr(process_current());
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
    uint64_t uc = frame + A64_OFF(uc);
    const size_t mc = A64_OFF(uc.uc_mcontext) - A64_OFF(uc);
    uint64_t mask;
    if (probe_user(uc, sizeof(struct lx_ucontext_a64)) ||
        copy_from_user(&mask, uc + offsetof(struct lx_ucontext_a64, uc_sigmask), sizeof(mask)) ||
        copy_from_user(r.x, uc + mc + offsetof(struct lx_sigcontext_a64, regs), sizeof(r.x)) ||
        copy_from_user(&r.sp, uc + mc + offsetof(struct lx_sigcontext_a64, sp), sizeof(r.sp)) ||
        copy_from_user(&r.pc, uc + mc + offsetof(struct lx_sigcontext_a64, pc), sizeof(r.pc)) ||
        copy_from_user(&r.pstate, uc + mc + offsetof(struct lx_sigcontext_a64, pstate), sizeof(r.pstate))) {
        struct signal_info info = { .sig = SIGSEGV, .source = SIGSRC_FAULT, .fault_addr = frame, .code = 1 };
        signal_send_thread(t, SIGSEGV, &info);
        t->syscall_nr = SIGNAL_NO_RESTART;
        return -EFAULT;
    }
    /* Put the interrupted code's vector registers back, if the frame
     * still carries them: a handler is free to have used its own. The
     * records are walked rather than assumed, because a program may
     * build its own frame. A record that cannot be read ends the walk,
     * as a malformed one does. */
    const uint64_t rsv = uc + mc + offsetof(struct lx_sigcontext_a64, reserved);
    const size_t rsv_len = sizeof(((struct lx_sigcontext_a64 *)0)->reserved);
    for (unsigned off = 0; off + sizeof(struct lx_ctx_terminator) <= rsv_len;) {
        struct lx_ctx_terminator head;
        if (copy_from_user(&head, rsv + off, sizeof(head)))
            break;
        if (head.magic == 0 || head.size < sizeof(head) || head.size > rsv_len - off)
            break;
        if (head.magic == LX_FPSIMD_MAGIC && head.size == sizeof(struct lx_fpsimd_context)) {
            struct lx_fpsimd_context fp;
            if (copy_from_user(&fp, rsv + off, sizeof(fp)) == 0)
                (void)arch_user_fpu_image_restore(&fp.fpsr);
            break;
        }
        off += head.size;
    }
    signal_return(a->frame, &r, mask);
    t->syscall_nr = SIGNAL_NO_RESTART;
    return (int64_t)r.x[0];
}
#endif

/* --- the trampoline page and the exit hook ------------------------------------- */

/*
 * The signal-return trampoline (SEC-4). x86-64 has none: a frame needs
 * SA_RESTORER, as on Linux, so nothing is mapped. AArch64 returns a
 * handler without SA_RESTORER through it -- Linux's arm64 counterpart is
 * the vDSO's sigreturn, randomised with the vDSO -- so it goes at a random
 * page of the mmap range (the first free page at or above a random page
 * offset from USER_MMAP_BASE, ASLR_MMAP_BITS of them), drawn at every
 * exec and kept by fork. It used to sit at the fixed LX_SIGTRAMP in every
 * process: a known executable address in an otherwise randomised layout.
 */
int linux_sigtramp_map(struct process *p, uint64_t *where)
{
    *where = 0;
#if defined(ARCH_X86_64)
    (void)p;
    return 0;
#else
    static const uint8_t code[] = { 0x68, 0x11, 0x80, 0xd2, 0x01, 0x00, 0x00, 0xd4 };   /* mov x8,#139; svc #0 */
    uint64_t hint = USER_MMAP_BASE + ((random_u64() & (((uint64_t)1 << ASLR_MMAP_BITS) - 1)) << PAGE_SHIFT);
    uint64_t at = vm_user_find_free(p->space, hint, PAGE_SIZE);
    if (at == 0)
        return -ENOMEM;
    int rc = vm_user_map_anon(p->space, at, PAGE_SIZE, VM_PROT_RW, VM_REGION_POPULATED, "sigtramp");
    if (rc)
        return rc;
    paddr_t pa;
    if (!arch_mmu_query(&p->space->mmu, (vaddr_t)at, &pa, NULL, NULL, NULL))
        return -EFAULT;   /* populated: cannot happen */
    memcpy(phys_to_virt(pa), code, sizeof(code));
    rc = vm_user_protect(p->space, at, PAGE_SIZE, VM_PROT_RX);
    if (rc == 0)
        *where = at;
    return rc;
#endif
}
