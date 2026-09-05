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

void x86_syscall_c(struct x86_syscall_frame *frame)
{
    uint64_t args[6] = { frame->rdi, frame->rsi, frame->rdx, frame->r10, frame->r8, frame->r9 };
    frame->rax = (uint64_t)syscall_dispatch(frame->rax, args, frame);
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
