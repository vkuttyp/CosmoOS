/*
 * trap.c - x86-64 trap dispatch and the arch/trap.h interface.
 *
 * Every vector arrives here from isr.S. Exceptions and interrupts share
 * the path: the generic dispatcher looks up a handler; if none is
 * registered, arch_trap_unhandled() decides between panic (exceptions)
 * and a counted warning (spurious external interrupts). Legacy PIC
 * vectors are acknowledged after dispatch so a registered handler never
 * has to know about the controller.
 */

#include <kernel/extable.h>
#include <kernel/interrupt.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/process.h>
#include <kernel/quiesce.h>
#include <arch/user.h>
#include <kernel/sched.h>
#include <kernel/signal.h>
#include <kernel/thread.h>

#include <arch/irq.h>
#include <arch/irqc.h>
#include <arch/testhooks.h>
#include <arch/trap.h>

#include <x86/cpu.h>
#include <x86/gdt.h>
#include <x86/idt.h>
#include <x86/pic.h>
#include <x86/trapframe.h>

static const char *const exception_names[X86_EXCEPTION_COUNT] = {
    [0] = "#DE divide error",
    [1] = "#DB debug",
    [2] = "NMI",
    [3] = "#BP breakpoint",
    [4] = "#OF overflow",
    [5] = "#BR bound range",
    [6] = "#UD invalid opcode",
    [7] = "#NM device not available",
    [8] = "#DF double fault",
    [9] = "coprocessor segment overrun",
    [10] = "#TS invalid TSS",
    [11] = "#NP segment not present",
    [12] = "#SS stack fault",
    [13] = "#GP general protection",
    [14] = "#PF page fault",
    [15] = "reserved",
    [16] = "#MF x87 floating point",
    [17] = "#AC alignment check",
    [18] = "#MC machine check",
    [19] = "#XM SIMD floating point",
    [20] = "#VE virtualization",
    [21] = "#CP control protection",
    [22] = "reserved", [23] = "reserved", [24] = "reserved", [25] = "reserved",
    [26] = "reserved", [27] = "reserved",
    [28] = "#HV hypervisor injection",
    [29] = "#VC VMM communication",
    [30] = "#SX security",
    [31] = "reserved",
};

static uint64_t g_spurious_count;

void x86_trap_dispatch(struct arch_trap_frame *frame)
{
    unsigned vector = (unsigned)frame->vector;
    bool is_interrupt = vector >= X86_EXCEPTION_COUNT;
    struct percpu *pc = this_cpu();

    if (is_interrupt) {
        pc->irq_depth++;
        pc->irq_count++;
    }

    interrupt_dispatch(vector, frame);

    if (is_interrupt) {
        arch_irqc_eoi(vector);
        pc->irq_depth--;

        /* Returning to a context that holds no spinlock, is not itself an
         * interrupt and had interrupts enabled: this CPU is outside every
         * read-side section, so it is quiescent (docs/kernel/quiesce/),
         * and it is the preemption point. The switch happens here, on the
         * interrupted thread's stack; the iretq completes when it is
         * switched back. */
        if (pc->irq_depth == 0 && pc->preempt_count == 0 && (frame->rflags & RFLAGS_IF)) {
            quiesce_note_quiescent();
            if (pc->need_resched)
                sched_preempt();
        }
    }

    /* Returning to ring 3: a pending kill ends the process here, so a
     * CPU-bound loop dies at its next timer tick. */
    if (arch_trap_frame_is_user(frame) && pc->irq_depth == 0 && pc->preempt_count == 0)
        process_return_to_user(frame);
}

/*
 * The paranoid vectors (#DB, NMI, #DF, #MC) arrive here from isr_paranoid
 * on their IST stacks, with the per-CPU pointer already recovered. They
 * count as interrupt context: a handler must not block, and this tail
 * neither preempts nor delivers a kill, because the interrupted context
 * may be the scheduler holding a run-queue lock, or a SYSCALL/SYSRET
 * window with the user's stack live. An unregistered vector ends in
 * panic_frame through arch_trap_unhandled, as for any exception.
 */
void x86_trap_paranoid(struct arch_trap_frame *frame)
{
    struct percpu *pc = this_cpu();
    pc->irq_depth++;
    interrupt_dispatch((unsigned)frame->vector, frame);
    pc->irq_depth--;
}

/* --- arch/testhooks.h: the paranoid path under test --- */

struct paranoid_probe {
    unsigned hits;
    uintptr_t frame;
    struct percpu *pc;
    unsigned irq_depth;
};

static void paranoid_probe_handler(unsigned vector, struct arch_trap_frame *frame, void *arg)
{
    struct paranoid_probe *p = arg;
    (void)vector;
    p->hits++;
    p->frame = (uintptr_t)frame;   /* lives on whatever stack the CPU switched to */
    p->pc = this_cpu();
    p->irq_depth = this_cpu()->irq_depth;
}

static bool on_ist(uintptr_t frame, uintptr_t top)
{
    return frame < top && frame >= top - IST_STACK_SIZE;
}

bool arch_test_paranoid_entry(const char **why)
{
    struct paranoid_probe p = { 0 };
    struct percpu *me = this_cpu();
    uintptr_t top = gdt_ist_top(IST_NMI);

    if (interrupt_register(X86_TRAP_NMI, paranoid_probe_handler, &p, "selftest-nmi") != 0) {
        *why = "cannot register the NMI probe";
        return false;
    }

    /* 1. A software NMI from ordinary kernel context: IST stack, interrupt depth. */
    __asm__ volatile("int $2" ::: "memory");
    bool ok = p.hits == 1 && p.pc == me && p.irq_depth == 1 && on_ist(p.frame, top);
    if (!ok)
        *why = "NMI from kernel context: wrong stack, per-CPU block or depth";

    /* 2. The same with the user's GS base live, the state inside the
     * SYSCALL entry window before its swapgs (KERNEL_GS_BASE holds 0 for
     * user mode). A CS-based swap decision would run the handler with GS
     * base 0 and fault on this_cpu(); the MSR-based one must recover the
     * block and hand back exactly the state it found. */
    if (ok) {
        arch_irq_state_t s = arch_irq_save();
        __asm__ volatile("swapgs\n\tint $2\n\tswapgs" ::: "memory");
        uint64_t gs_after = rdmsr(0xC0000101u);   /* MSR_GS_BASE */
        arch_irq_restore(s);
        ok = p.hits == 2 && p.pc == me && p.irq_depth == 1 && on_ist(p.frame, top) &&
             gs_after == (uint64_t)(uintptr_t)me && this_cpu() == me;
        if (!ok)
            *why = "NMI with the user's GS base: per-CPU block not recovered or not restored";
    }

    interrupt_unregister(X86_TRAP_NMI, paranoid_probe_handler);
    if (ok)
        *why = NULL;
    return ok;
}

/* --- arch/trap.h --- */

unsigned arch_trap_vector_count(void)
{
    return IDT_VECTORS;
}

int arch_trap_vector(enum arch_trap_kind kind)
{
    switch (kind) {
    case ARCH_TRAP_BREAKPOINT:         return (int)X86_TRAP_BP;
    case ARCH_TRAP_DEBUG:              return (int)X86_TRAP_DB;
    case ARCH_TRAP_DIVIDE_ERROR:       return (int)X86_TRAP_DE;
    case ARCH_TRAP_INVALID_OPCODE:     return (int)X86_TRAP_UD;
    case ARCH_TRAP_GENERAL_PROTECTION: return (int)X86_TRAP_GP;
    case ARCH_TRAP_PAGE_FAULT:         return (int)X86_TRAP_PF;
    case ARCH_TRAP_KIND_COUNT:
    default:                           return -1;
    }
}

bool arch_trap_is_exception(unsigned vector)
{
    return vector < X86_EXCEPTION_COUNT;
}

const char *arch_trap_name(unsigned vector)
{
    if (vector < X86_EXCEPTION_COUNT)
        return exception_names[vector];
    if (vector >= X86_VECTOR_IRQ_BASE && vector < X86_VECTOR_IRQ_BASE + X86_VECTOR_IRQ_COUNT)
        return "legacy IRQ";
    return "interrupt";
}

uintptr_t arch_trap_frame_pc(const struct arch_trap_frame *frame)
{
    return (uintptr_t)frame->rip;
}

uintptr_t arch_trap_frame_sp(const struct arch_trap_frame *frame)
{
    return (uintptr_t)frame->rsp;
}

uintptr_t arch_trap_frame_fp(const struct arch_trap_frame *frame)
{
    return (uintptr_t)frame->rbp;
}

void arch_trap_frame_dump(const struct arch_trap_frame *f)
{
    kprintf("trap %llu (%s) error=0x%llx\n",
            (unsigned long long)f->vector, arch_trap_name((unsigned)f->vector),
            (unsigned long long)f->error_code);
    kprintf("RIP=%016llx CS=%04llx RFLAGS=%08llx RSP=%016llx SS=%04llx\n",
            (unsigned long long)f->rip, (unsigned long long)f->cs,
            (unsigned long long)f->rflags, (unsigned long long)f->rsp,
            (unsigned long long)f->ss);
    kprintf("RAX=%016llx RBX=%016llx RCX=%016llx RDX=%016llx\n",
            (unsigned long long)f->rax, (unsigned long long)f->rbx,
            (unsigned long long)f->rcx, (unsigned long long)f->rdx);
    kprintf("RSI=%016llx RDI=%016llx RBP=%016llx R8 =%016llx\n",
            (unsigned long long)f->rsi, (unsigned long long)f->rdi,
            (unsigned long long)f->rbp, (unsigned long long)f->r8);
    kprintf("R9 =%016llx R10=%016llx R11=%016llx R12=%016llx\n",
            (unsigned long long)f->r9, (unsigned long long)f->r10,
            (unsigned long long)f->r11, (unsigned long long)f->r12);
    kprintf("R13=%016llx R14=%016llx R15=%016llx\n",
            (unsigned long long)f->r13, (unsigned long long)f->r14,
            (unsigned long long)f->r15);
    kprintf("CR0=%016llx CR3=%016llx CR4=%016llx\n",
            (unsigned long long)read_cr0(), (unsigned long long)read_cr3(),
            (unsigned long long)read_cr4());
    if (f->vector == X86_TRAP_PF) {
        uint64_t err = f->error_code;
        kprintf("CR2=%016llx (%s %s %s%s%s)\n",
                (unsigned long long)read_cr2(),
                (err & 1) ? "protection" : "not-present",
                (err & 2) ? "write" : "read",
                (err & 4) ? "user" : "kernel",
                (err & 8) ? " reserved-bit" : "",
                (err & 16) ? " instruction-fetch" : "");
    }
}

/* An exception from user mode that no handler claimed: the process gets
 * the signal Linux would send (a stack fault after a signal return with a
 * bad rsp, an x87 or SIMD exception a restored control word unmasked, a
 * bound-range or alignment trap). Queued, not delivered here: the return
 * to user mode (or the next tick) delivers it, whatever stack this
 * exception arrived on. A kernel-mode one is still a panic. */
static int user_exception_signal(unsigned vector)
{
    switch (vector) {
    case X86_TRAP_DE: case X86_TRAP_MF: case X86_TRAP_XM: return SIGFPE;
    case X86_TRAP_DB: case X86_TRAP_BP: return SIGTRAP;
    case X86_TRAP_UD: return SIGILL;
    case X86_TRAP_AC: return SIGBUS;
    default: return SIGSEGV;   /* #OF, #BR, #TS, #NP, #SS, #GP, ... */
    }
}

void arch_trap_unhandled(unsigned vector, struct arch_trap_frame *frame)
{
    if (arch_trap_is_exception(vector)) {
        if (arch_trap_frame_is_user(frame) && process_current() != NULL) {
            int sig = user_exception_signal(vector);
            struct signal_info info = { .sig = sig, .source = SIGSRC_FAULT, .fault_addr = frame->rip, .code = 1 };
            kdebug("x86: user %s at %p: signal %d", arch_trap_name(vector), (void *)frame->rip, sig);
            signal_send_thread(thread_current(), sig, &info);
            return;
        }
        panic_frame(frame, "unhandled exception %u (%s)", vector, arch_trap_name(vector));
    }

    g_spurious_count++;
    if (vector >= X86_VECTOR_IRQ_BASE && vector < X86_VECTOR_IRQ_BASE + X86_VECTOR_IRQ_COUNT &&
        pic_is_spurious(vector - X86_VECTOR_IRQ_BASE)) {
        kdebug("x86: spurious legacy IRQ %u", vector - X86_VECTOR_IRQ_BASE);
        return;
    }
    kwarn("x86: unhandled interrupt vector %u (%llu total unhandled)",
          vector, (unsigned long long)g_spurious_count);
}

void arch_debug_break(void)
{
    __asm__ volatile("int3" ::: "memory");
}

bool arch_trap_fixup(struct arch_trap_frame *frame)
{
    uintptr_t fixup = extable_fixup((uintptr_t)frame->rip);
    if (fixup == 0)
        return false;
    frame->rip = fixup;
    return true;
}

uintptr_t arch_trap_fault_address(const struct arch_trap_frame *frame)
{
    KASSERT(frame->vector == X86_TRAP_PF);
    return (uintptr_t)read_cr2();
}

unsigned arch_trap_fault_flags(const struct arch_trap_frame *frame)
{
    KASSERT(frame->vector == X86_TRAP_PF);
    uint64_t err = frame->error_code;
    unsigned f = 0;
    if (err & 1)
        f |= ARCH_FAULT_PRESENT;
    if (err & 2)
        f |= ARCH_FAULT_WRITE;
    if (err & 4)
        f |= ARCH_FAULT_USER;
    if (err & 8)
        f |= ARCH_FAULT_RESERVED;
    if (err & 16)
        f |= ARCH_FAULT_EXEC;
    return f;
}
