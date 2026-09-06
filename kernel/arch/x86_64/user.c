/*
 * user.c - User-mode transitions for x86-64: SYSCALL/SYSRET setup, the
 * first entry into ring 3, SMAP access windows.
 */

#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/syscall.h>
#include <kernel/thread.h>

#include <arch/irq.h>
#include <arch/user.h>
#include <x86/fpu.h>

#include <x86/cpu.h>
#include <x86/gdt.h>
#include <x86/trapframe.h>

#define MSR_STAR           0xC0000081u
#define MSR_LSTAR          0xC0000082u
#define MSR_SFMASK         0xC0000084u
#define MSR_KERNEL_GS_BASE 0xC0000102u

#define RFLAGS_TF (1ULL << 8)
#define RFLAGS_DF (1ULL << 10)
#define RFLAGS_NT (1ULL << 14)
#define RFLAGS_AC (1ULL << 18)

/* User selectors carry RPL 3. */
#define USER_CS (GDT_USER_CODE | 3)
#define USER_SS (GDT_USER_DATA | 3)

void x86_syscall_entry(void);

void arch_syscall_init_cpu(void)
{
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
    /* SYSCALL: CS = 0x08, SS = 0x10. SYSRET: CS = 0x10 + 16 = 0x20 (|3),
     * SS = 0x10 + 8 = 0x18 (|3). The GDT layout was chosen for this. */
    wrmsr(MSR_STAR, ((uint64_t)GDT_KERNEL_DATA << 48) | ((uint64_t)GDT_KERNEL_CODE << 32));
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)x86_syscall_entry);
    wrmsr(MSR_SFMASK, RFLAGS_IF | RFLAGS_TF | RFLAGS_DF | RFLAGS_AC | RFLAGS_NT);
    /* While in the kernel GS_BASE is the per-CPU block and the "other"
     * base (swapped in on return to user) is 0. */
    wrmsr(MSR_KERNEL_GS_BASE, 0);
}

void arch_user_enter(uintptr_t entry, uintptr_t sp)
{
    struct thread *t = thread_current();
    KASSERT(t != NULL && t->proc != NULL);

    arch_irq_disable();

    /* Publish this thread's kernel stack for the SYSCALL entry and for
     * traps from ring 3 (TSS rsp0), in case we got here without a
     * switch. */
    uintptr_t kstack = t->stack_base + t->stack_size;
    this_cpu()->kernel_stack_top = kstack;
    gdt_set_kernel_stack(kstack);

    /* IRETQ frame: ss, rsp, rflags (IF set, everything else clear),
     * cs, rip. General registers are zeroed so nothing kernel-side
     * leaks to the first user instruction. */
    __asm__ volatile(
        "swapgs\n\t"
        "pushq %[ss]\n\t"
        "pushq %[sp]\n\t"
        "pushq %[fl]\n\t"
        "pushq %[cs]\n\t"
        "pushq %[ip]\n\t"
        "xorl %%eax, %%eax\n\t"
        "xorl %%ebx, %%ebx\n\t"
        "xorl %%ecx, %%ecx\n\t"
        "xorl %%edx, %%edx\n\t"
        "xorl %%esi, %%esi\n\t"
        "xorl %%edi, %%edi\n\t"
        "xorl %%ebp, %%ebp\n\t"
        "xorl %%r8d, %%r8d\n\t"
        "xorl %%r9d, %%r9d\n\t"
        "xorl %%r10d, %%r10d\n\t"
        "xorl %%r11d, %%r11d\n\t"
        "xorl %%r12d, %%r12d\n\t"
        "xorl %%r13d, %%r13d\n\t"
        "xorl %%r14d, %%r14d\n\t"
        "xorl %%r15d, %%r15d\n\t"
        "iretq\n\t"
        :
        : [ss] "i"(USER_SS), [sp] "r"(sp), [fl] "i"(RFLAGS_IF | 0x2), [cs] "i"(USER_CS), [ip] "r"(entry)
        : "memory");
    __builtin_unreachable();
}

void x86_syscall_return_check(struct x86_syscall_frame *frame);

void x86_syscall_c(struct x86_syscall_frame *frame)
{
    uint64_t args[6] = { frame->rdi, frame->rsi, frame->rdx, frame->r10, frame->r8, frame->r9 };
    frame->rax = (uint64_t)syscall_dispatch(frame->rax, args, frame);
    x86_syscall_return_check(frame);
}

void arch_user_access_begin(void)
{
    if (x86_cpu_info()->has_smap)
        __asm__ volatile("stac" ::: "memory");
}

void arch_user_access_end(void)
{
    if (x86_cpu_info()->has_smap)
        __asm__ volatile("clac" ::: "memory");
}

void arch_set_tls_base(uintptr_t base)
{
    struct thread *t = thread_current();
    t->tls_base = base;
    wrmsr(0xC0000100u, base);   /* MSR_FS_BASE */
}

bool arch_trap_frame_is_user(const struct arch_trap_frame *frame)
{
    return (frame->cs & 3) != 0;
}

/* --- the user register set (milestone 10) ------------------------------------ */


_Static_assert(offsetof(struct arch_user_regs, rsp) == 0x38 && offsetof(struct arch_user_regs, rip) == 0x80 &&
                   offsetof(struct arch_user_regs, rflags) == 0x88 && offsetof(struct arch_user_regs, r15) == 0x78,
               "arch_user_regs layout used by arch_user_enter_regs");

static bool canonical(uint64_t va)
{
    return va < 0x0000800000000000ull || va >= 0xffff800000000000ull;
}

void arch_user_regs_from_syscall(const void *frame, struct arch_user_regs *r)
{
    const struct x86_syscall_frame *f = frame;
    *r = (struct arch_user_regs){ .rax = f->rax, .rbx = f->rbx, .rcx = f->rcx, .rdx = f->rdx, .rsi = f->rsi,
                                  .rdi = f->rdi, .rbp = f->rbp, .rsp = f->rsp, .r8 = f->r8, .r9 = f->r9,
                                  .r10 = f->r10, .r11 = f->r11, .r12 = f->r12, .r13 = f->r13, .r14 = f->r14,
                                  .r15 = f->r15, .rip = f->rip, .rflags = f->rflags };
}

void arch_user_regs_to_syscall(void *frame, const struct arch_user_regs *r)
{
    struct x86_syscall_frame *f = frame;
    f->rax = r->rax; f->rbx = r->rbx; f->rcx = r->rcx; f->rdx = r->rdx; f->rsi = r->rsi; f->rdi = r->rdi;
    f->rbp = r->rbp; f->rsp = r->rsp; f->r8 = r->r8; f->r9 = r->r9; f->r10 = r->r10; f->r11 = r->r11;
    f->r12 = r->r12; f->r13 = r->r13; f->r14 = r->r14; f->r15 = r->r15; f->rip = r->rip; f->rflags = r->rflags;
    f->cs = USER_CS;
    f->ss = USER_SS;
    f->flags |= X86_SYSCALL_FULL_RESTORE;
}

void arch_user_regs_from_trap(const struct arch_trap_frame *f, struct arch_user_regs *r)
{
    *r = (struct arch_user_regs){ .rax = f->rax, .rbx = f->rbx, .rcx = f->rcx, .rdx = f->rdx, .rsi = f->rsi,
                                  .rdi = f->rdi, .rbp = f->rbp, .rsp = f->rsp, .r8 = f->r8, .r9 = f->r9,
                                  .r10 = f->r10, .r11 = f->r11, .r12 = f->r12, .r13 = f->r13, .r14 = f->r14,
                                  .r15 = f->r15, .rip = f->rip, .rflags = f->rflags };
}

void arch_user_regs_to_trap(struct arch_trap_frame *f, const struct arch_user_regs *r)
{
    f->rax = r->rax; f->rbx = r->rbx; f->rcx = r->rcx; f->rdx = r->rdx; f->rsi = r->rsi; f->rdi = r->rdi;
    f->rbp = r->rbp; f->rsp = r->rsp; f->r8 = r->r8; f->r9 = r->r9; f->r10 = r->r10; f->r11 = r->r11;
    f->r12 = r->r12; f->r13 = r->r13; f->r14 = r->r14; f->r15 = r->r15; f->rip = r->rip; f->rflags = r->rflags;
    f->cs = USER_CS;
    f->ss = USER_SS;
}

uintptr_t arch_user_regs_pc(const struct arch_user_regs *r) { return (uintptr_t)r->rip; }
uintptr_t arch_user_regs_sp(const struct arch_user_regs *r) { return (uintptr_t)r->rsp; }
void arch_user_regs_set_pc(struct arch_user_regs *r, uintptr_t pc) { r->rip = pc; }
void arch_user_regs_set_sp(struct arch_user_regs *r, uintptr_t sp) { r->rsp = sp; }
void arch_user_regs_set_result(struct arch_user_regs *r, int64_t v) { r->rax = (uint64_t)v; }
int64_t arch_user_regs_result(const struct arch_user_regs *r) { return (int64_t)r->rax; }

void arch_user_regs_restart_syscall(struct arch_user_regs *r, uint64_t nr, uint64_t arg0)
{
    r->rax = nr;
    r->rdi = arg0;
    r->rip -= 2;   /* the two-byte SYSCALL instruction */
}

void arch_user_regs_sanitize(struct arch_user_regs *r)
{
    r->rflags = (r->rflags & X86_RFLAGS_USER_MASK) | X86_RFLAGS_FIXED;
    /* IRETQ to a non-canonical rip raises #GP in the kernel; a kernel-half
     * rip would be a user fault at a kernel address. Both become a user
     * fault at 0 instead (SIGSEGV), the register file otherwise intact. */
    if (r->rip >= 0x0000800000000000ull)
        r->rip = 0;
}

void arch_user_enter_regs(const struct arch_user_regs *r)
{
    struct thread *t = thread_current();
    KASSERT(t != NULL && t->proc != NULL);
    struct arch_user_regs regs = *r;
    arch_user_regs_sanitize(&regs);
    arch_irq_disable();
    uintptr_t kstack = t->stack_base + t->stack_size;
    this_cpu()->kernel_stack_top = kstack;
    gdt_set_kernel_stack(kstack);
    wrmsr(0xC0000100u, t->tls_base);   /* MSR_FS_BASE for this thread */
    /* Build the IRETQ frame, then load every general register from the
     * copy with rax as the base pointer, rax itself last. */
    __asm__ volatile(
        "swapgs\n\t"
        "pushq %[ss]\n\t"
        "pushq 0x38(%%rax)\n\t"   /* rsp */
        "pushq 0x88(%%rax)\n\t"   /* rflags */
        "pushq %[cs]\n\t"
        "pushq 0x80(%%rax)\n\t"   /* rip */
        "movq 0x08(%%rax), %%rbx\n\t"
        "movq 0x10(%%rax), %%rcx\n\t"
        "movq 0x18(%%rax), %%rdx\n\t"
        "movq 0x20(%%rax), %%rsi\n\t"
        "movq 0x28(%%rax), %%rdi\n\t"
        "movq 0x30(%%rax), %%rbp\n\t"
        "movq 0x40(%%rax), %%r8\n\t"
        "movq 0x48(%%rax), %%r9\n\t"
        "movq 0x50(%%rax), %%r10\n\t"
        "movq 0x58(%%rax), %%r11\n\t"
        "movq 0x60(%%rax), %%r12\n\t"
        "movq 0x68(%%rax), %%r13\n\t"
        "movq 0x70(%%rax), %%r14\n\t"
        "movq 0x78(%%rax), %%r15\n\t"
        "movq 0x00(%%rax), %%rax\n\t"
        "iretq\n\t"
        :
        : "a"(&regs), [ss] "i"(USER_SS), [cs] "i"(USER_CS)
        : "memory");
    __builtin_unreachable();
}

/* Decide the return path after a system call (docs/kernel/process/design.md
 * §11, "The SYSRET canonical guard"): SYSRET is used only when every value
 * it loads is one it can load safely. */
void x86_syscall_return_check(struct x86_syscall_frame *frame)
{
    if (!canonical(frame->rip) || (frame->rflags & ~(X86_RFLAGS_USER_MASK | X86_RFLAGS_FIXED)) != 0)
        frame->flags |= X86_SYSCALL_FULL_RESTORE;
}

void arch_user_regs_set_result_in_frame(void *frame, int64_t v) { ((struct x86_syscall_frame *)frame)->rax = (uint64_t)v; }
int64_t arch_user_regs_result_in_frame(const void *frame) { return (int64_t)((const struct x86_syscall_frame *)frame)->rax; }

size_t arch_user_fpu_image_size(void) { return X86_FXSAVE_SIZE; }
bool arch_user_fpu_image_save(void *buf) { return x86_fpu_legacy_get(buf); }
bool arch_user_fpu_image_restore(const void *buf) { return x86_fpu_legacy_set(buf); }
