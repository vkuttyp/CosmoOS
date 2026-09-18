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
#include <kernel/lockup.h>
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
    /* An NMI answers a pending lockup sample for this CPU first
     * (kernel/core/lockup.c: records into this CPU's buffer, no lock, no
     * printing). A registered handler is then dispatched whatever the
     * answer, so nothing a handler owns is ever swallowed; only with no
     * handler does the answer decide -- an NMI that answered a request
     * returns, one that did not is unhandled as before. An x86 NMI
     * carries no vector and no source, so this one-delivery window is
     * the architecture's limit (docs/kernel/diagnostics/design.md). */
    bool answered = frame->vector == X86_TRAP_NMI && lockup_answer(frame, true);
    if (!answered || interrupt_handler_name((unsigned)frame->vector) != NULL)
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

uint64_t arch_trap_frame_detail(const struct arch_trap_frame *frame)
{
    return frame->error_code;
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

/* --- asynchronous errors (invariant I-ARCH-16) --------------------------------------
 *
 * A machine check's severity, from the banks rather than from
 * MCG_STATUS alone: RIPV says execution can continue, and says nothing
 * about whether a bank recorded an uncorrected error or whether the
 * processor's context is corrupt. Reading one register and trusting it
 * is what made the first draft of this classifier wrong
 * (docs/audit/next-subsystem-async-error.md, Design 2).
 *
 * CORRECTED requires all four, positively:
 *   1. at least one bank with VAL -- something was reported, or there is
 *      nothing to have understood. Without this clause "every valid bank
 *      is clean" is true of NO banks and an empty machine check reads as
 *      corrected;
 *   2. every valid bank has UC == 0;
 *   3. no bank has PCC (processor context corrupt) or OVER (a record was
 *      overwritten, so what is there is not the whole story);
 *   4. MCG_STATUS.RIPV.
 * Anything else is uncontained. One bank's silence never outvotes
 * another's report.
 *
 * Pure, so the combinations can be table-tested without an error.
 */
#define MCI_STATUS_VAL  (1ull << 63)
#define MCI_STATUS_OVER (1ull << 62)
#define MCI_STATUS_UC   (1ull << 61)
#define MCI_STATUS_PCC  (1ull << 57)
#define MCG_STATUS_RIPV (1ull << 0)
#define MCG_STATUS_EIPV (1ull << 1)

enum arch_async_error x86_async_class(uint64_t mcg_status, const uint64_t *banks, unsigned n)
{
    bool any_valid = false, any_uc = false;
    for (unsigned i = 0; i < n; i++) {
        uint64_t st = banks[i];
        if (!(st & MCI_STATUS_VAL))
            continue;   /* this bank says nothing; it does not say "fine" */
        any_valid = true;
        if (st & (MCI_STATUS_PCC | MCI_STATUS_OVER))
            return ARCH_ASYNC_UNCONTAINED;   /* whatever RIPV claims */
        if (st & MCI_STATUS_UC)
            any_uc = true;
    }
    if (!any_valid)
        return ARCH_ASYNC_UNCONTAINED;   /* no record: nothing was understood */
    if (!(mcg_status & MCG_STATUS_RIPV))
        return ARCH_ASYNC_UNCONTAINED;   /* execution cannot continue here */
    if (!any_uc)
        return ARCH_ASYNC_CORRECTED;
    /* Uncorrected, but the machine can continue and the error is tied to
     * the instruction. Decoded for the attribution unit; this one treats
     * it as uncontained, because nothing here says which process. */
    return (mcg_status & MCG_STATUS_EIPV) ? ARCH_ASYNC_CONTAINED : ARCH_ASYNC_UNCONTAINED;
}

enum arch_async_error arch_async_error_class(const struct arch_trap_frame *frame)
{
    (void)frame;
    struct cpuid_regs r;
    cpuid(1, 0, &r);
    if (!(r.edx & (1u << 14)))
        return ARCH_ASYNC_UNCONTAINED;   /* no MCA: nothing to read */
    unsigned n = (unsigned)(rdmsr(MSR_IA32_MCG_CAP) & 0xFFu);
    if (n > X86_MCA_BANKS_MAX)
        n = X86_MCA_BANKS_MAX;
    uint64_t banks[X86_MCA_BANKS_MAX];
    for (unsigned i = 0; i < n; i++)
        banks[i] = rdmsr(MSR_IA32_MC0_STATUS + 4u * i);
    return x86_async_class(rdmsr(MSR_IA32_MCG_STATUS), banks, n);
}

const char *arch_async_error_name(enum arch_async_error c)
{
    switch (c) {
    case ARCH_ASYNC_CORRECTED:   return "corrected";
    case ARCH_ASYNC_CONTAINED:   return "contained";
    default:                     return "uncontained";
    }
}

/*
 * A machine check (invariant I-ARCH-16). Registered here rather than
 * through process.c's user-exception loop, which maps a vector to a
 * signal unconditionally and sends every kernel frame to the unhandled
 * panic: that cannot express "a corrected error returns". It also cannot
 * be allowed to, here -- an asynchronous error's frame names the context
 * interrupted at delivery, not the one that caused it, so no process is
 * blamed.
 *
 * This runs on the machine-check IST stack through the paranoid entry
 * (I-ARCH-7), which neither preempts nor delivers a kill and whose
 * handlers must not fault. So the corrected path counts and returns; it
 * does not print.
 */
static uint64_t g_async_corrected;

static void machine_check_handler(unsigned vector, struct arch_trap_frame *frame, void *arg)
{
    (void)vector;
    (void)arg;
    enum arch_async_error class = arch_async_error_class(frame);
    if (class == ARCH_ASYNC_CORRECTED) {
        __atomic_fetch_add(&g_async_corrected, 1u, __ATOMIC_RELAXED);
        return;
    }
    panic_frame(frame, "machine check: %s (MCG_STATUS 0x%llx)", arch_async_error_name(class),
                (unsigned long long)rdmsr(MSR_IA32_MCG_STATUS));
}

void arch_async_error_init(void)
{
    int rc = interrupt_register(X86_TRAP_MC, machine_check_handler, NULL, "machine-check");
    if (rc && rc != -EBUSY)
        panic("trap: cannot register the machine-check handler (%d)", rc);
}

uint64_t x86_async_corrected_count(void)
{
    return __atomic_load_n(&g_async_corrected, __ATOMIC_RELAXED);
}

/* --- arch/testhooks.h: the classifier over bank combinations this host never emits --- */

bool arch_test_async_class(const char **why)
{
    /* Every row differs from the corrected one in one bit or one bank, so
     * a dropped rule fails exactly one row and the row names it. */
    struct row {
        uint64_t mcg;
        uint64_t banks[3];
        unsigned n;
        enum arch_async_error want;
        const char *what;
    };
    static const struct row rows[] = {
        { MCG_STATUS_RIPV, { MCI_STATUS_VAL }, 1, ARCH_ASYNC_CORRECTED,
          "one valid clean bank with RIPV" },
        { MCG_STATUS_RIPV, { 0 }, 1, ARCH_ASYNC_UNCONTAINED,
          "no valid bank: 'every valid bank is clean' is vacuously true of none" },
        { MCG_STATUS_RIPV, { 0, 0, 0 }, 3, ARCH_ASYNC_UNCONTAINED,
          "three banks, none valid" },
        { MCG_STATUS_RIPV, { MCI_STATUS_VAL | MCI_STATUS_PCC }, 1, ARCH_ASYNC_UNCONTAINED,
          "PCC: processor context corrupt, whatever RIPV says" },
        { MCG_STATUS_RIPV, { MCI_STATUS_VAL | MCI_STATUS_OVER }, 1, ARCH_ASYNC_UNCONTAINED,
          "OVER: a record was overwritten, so what is there is not the whole story" },
        { 0, { MCI_STATUS_VAL }, 1, ARCH_ASYNC_UNCONTAINED,
          "RIPV clear: execution cannot continue here" },
        { MCG_STATUS_RIPV, { MCI_STATUS_VAL, MCI_STATUS_VAL | MCI_STATUS_PCC }, 2,
          ARCH_ASYNC_UNCONTAINED, "a clean bank does not outvote a later PCC: the multi-bank walk" },
        { MCG_STATUS_RIPV, { MCI_STATUS_VAL, MCI_STATUS_VAL | MCI_STATUS_UC }, 2,
          ARCH_ASYNC_UNCONTAINED, "uncorrected in a later bank, with no EIPV" },
        { MCG_STATUS_RIPV | MCG_STATUS_EIPV, { MCI_STATUS_VAL | MCI_STATUS_UC }, 1,
          ARCH_ASYNC_CONTAINED, "uncorrected, attributable, machine intact" },
    };
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        if (x86_async_class(rows[i].mcg, rows[i].banks, rows[i].n) != rows[i].want) {
            *why = rows[i].what;
            return false;
        }
    }
    kinfo("selftest: trap-async-class: %u machine-check combinations, one per rule -- VAL, PCC, OVER, "
          "RIPV and the multi-bank walk, including the empty bank set a vacuous rule calls corrected",
          (unsigned)(sizeof(rows) / sizeof(rows[0])));
    return true;
}

/*
 * x86-64 has no way to raise a real machine check from software -- `int
 * $18` reaches the vector but sets no banks, so the classifier would read
 * this machine's actual (empty) MCA state and call it uncontained, which
 * is correct and proves nothing about the policy. So this reports what
 * the host can and cannot do rather than pretending to inject.
 */
bool arch_test_async_inject(const char **why)
{
    /* The vector must be OURS. A machine check with no handler reaches
     * arch_trap_unhandled and panics -- which is what this unit removes,
     * and which nothing else here would notice, because x86-64 cannot
     * raise a real machine check from software. Registration is the one
     * part of the dispatch that IS checkable, so it is checked. */
    if (interrupt_handler_name(X86_TRAP_MC) == NULL) {
        *why = "no machine-check handler registered: vector 18 still panics through arch_trap_unhandled";
        return false;
    }
    struct cpuid_regs r;
    cpuid(1, 0, &r);
    unsigned banks = (r.edx & (1u << 14)) ? (unsigned)(rdmsr(MSR_IA32_MCG_CAP) & 0xFFu) : 0;
    kinfo("selftest: trap-async-inject: vector 18 is handled by '%s'; %u MCA banks would be read. "
          "x86-64 cannot raise a machine check from software, so the policy beyond registration is "
          "covered by trap-async-class", interrupt_handler_name(X86_TRAP_MC), banks);
    return true;
}
