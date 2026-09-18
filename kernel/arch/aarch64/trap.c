/*
 * trap.c - Exception classification, the trap contract, the syscall entry
 * (docs/kernel/arch/aarch64/design.md, "Exceptions and interrupts").
 *
 * Vector numbering (arch/trap.h contract on this architecture):
 *   0..1019      GIC INTIDs, 1020 spurious
 *   1024..1029   synchronous exception kinds (enum arch_trap_kind)
 *   1056..1311   dynamic software vectors routed to INTIDs by gic.c
 */

#include <kernel/extable.h>
#include <kernel/interrupt.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/process.h>
#include <kernel/quiesce.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>
#include <arch/irq.h>
#include <arch/irqc.h>
#include <arch/el2.h>
#include <arch/testhooks.h>
#include <arch/trap.h>
#include <arch/user.h>
#include <aarch64/platform.h>
#include <aarch64/sysreg.h>
#include <aarch64/trapframe.h>

static const char *const kind_names[ARCH_TRAP_KIND_COUNT] = {
    [ARCH_TRAP_BREAKPOINT] = "breakpoint",
    [ARCH_TRAP_DEBUG] = "debug (software step)",
    [ARCH_TRAP_DIVIDE_ERROR] = "divide error (never raised on AArch64)",
    [ARCH_TRAP_INVALID_OPCODE] = "undefined instruction",
    [ARCH_TRAP_GENERAL_PROTECTION] = "synchronous exception",
    [ARCH_TRAP_PAGE_FAULT] = "page fault",
    [ARCH_TRAP_ASYNC_ERROR] = "SError (asynchronous abort)",
};

static uint64_t g_async_corrected;

static uint64_t g_unhandled;

static enum arch_trap_kind classify(uint64_t esr)
{
    switch (ESR_EC(esr)) {
    case ESR_EC_IABT_LOWER:
    case ESR_EC_IABT_CUR:
    case ESR_EC_DABT_LOWER:
    case ESR_EC_DABT_CUR:
        return ARCH_TRAP_PAGE_FAULT;
    case ESR_EC_BRK64:
    case ESR_EC_BREAKPT_LOWER:
    case ESR_EC_BREAKPT_CUR:
    case ESR_EC_WATCHPT_LOWER:
    case ESR_EC_WATCHPT_CUR:
        return ARCH_TRAP_BREAKPOINT;
    case ESR_EC_SSTEP_LOWER:
    case ESR_EC_SSTEP_CUR:
        return ARCH_TRAP_DEBUG;
    case ESR_EC_UNKNOWN:
    case ESR_EC_ILLEGAL:
    case ESR_EC_FP_ACCESS:
    case ESR_EC_SYSREG:
        return ARCH_TRAP_INVALID_OPCODE;
    default:
        return ARCH_TRAP_GENERAL_PROTECTION;
    }
}

static void return_to_user_check(struct arch_trap_frame *frame)
{
    struct percpu *pc = this_cpu();
    if (arch_trap_frame_is_user(frame) && pc->irq_depth == 0 && pc->preempt_count == 0)
        process_return_to_user(frame);
}

static void handle_syscall(struct arch_trap_frame *frame)
{
    uint64_t args[6] = { frame->x[0], frame->x[1], frame->x[2], frame->x[3], frame->x[4], frame->x[5] };
    arch_irq_enable();
    frame->x[0] = (uint64_t)syscall_dispatch(frame->x[8], args, frame);
    arch_irq_disable();
}

static void handle_sync(struct arch_trap_frame *frame, bool from_user)
{
    unsigned ec = ESR_EC(frame->esr);
    if (ec == ESR_EC_SVC64) {
        if (!from_user)
            panic_frame(frame, "SVC from the kernel");
        frame->vector = VEC_SYNC_BASE + ARCH_TRAP_KIND_COUNT;   /* not a dispatched vector */
        handle_syscall(frame);
        return_to_user_check(frame);
        return;
    }
    enum arch_trap_kind kind = classify(frame->esr);
    frame->vector = VEC_SYNC_BASE + (unsigned)kind;
    uint64_t elr = frame->elr;
    interrupt_dispatch((unsigned)frame->vector, frame);
    /* BRK is a fault-class exception: ELR points at the instruction. The
     * contract (x86's int3 is a trap) is that execution resumes after the
     * breakpoint unless the handler moved the PC itself. */
    if (ec == ESR_EC_BRK64 && frame->elr == elr)
        frame->elr += 4;
    return_to_user_check(frame);
}

static void handle_irq(struct arch_trap_frame *frame)
{
    struct percpu *pc = this_cpu();
    pc->irq_depth++;
    pc->irq_count++;
    gic_irq_dispatch(frame);
    pc->irq_depth--;
    /* Cleared unconditionally, whether or not this return publishes:
     * the flag must not outlive the trap that set it, or a later
     * unrelated return would claim a publish the kick did not cause
     * (docs/kernel/quiesce/invariants.md, Q19). */
    bool kicked = pc->quiesce_kicked;
    pc->quiesce_kicked = false;
    /* Quiescent point and preemption point (docs/kernel/quiesce/): the
     * interrupted context holds no spinlock, is not an interrupt, and had
     * interrupts enabled. */
    if (pc->irq_depth == 0 && pc->preempt_count == 0 && (frame->spsr & DAIF_I) == 0) {
        /* Attributed only when the publish ADVANCED this CPU's epoch:
         * a redundant publish tells no waiter anything, so counting it
         * would say the kick worked when it did not (Q19). */
        if (quiesce_note_quiescent_preemptible() && kicked)
            quiesce_note_kick_published();
        if (pc->need_resched)
            sched_preempt();
    }
    return_to_user_check(frame);
}

/*
 * An asynchronous abort. The frame names the context that was interrupted
 * when the error was DELIVERED, not the one that caused it, so nothing
 * here blames a process -- attribution needs the RAS error records this
 * unit does not read (invariant I-ARCH-16). The only survivable class is one the
 * hardware says it corrected; everything else stops the machine, at either
 * exception level, and says what it was.
 */
static void handle_async_error(struct arch_trap_frame *frame)
{
    frame->vector = VEC_SYNC_BASE + ARCH_TRAP_ASYNC_ERROR;
    enum arch_async_error class = arch_async_error_class(frame);
    if (class == ARCH_ASYNC_CORRECTED) {
        /* Counted rather than printed: this runs in whatever context the
         * error interrupted, including one holding a run-queue lock. The
         * count is read by the self-test and by anyone asking later. */
        __atomic_fetch_add(&g_async_corrected, 1u, __ATOMIC_RELAXED);
        return;
    }
    panic_frame(frame, "SError: %s (ESR 0x%llx)", arch_async_error_name(class),
                (unsigned long long)frame->esr);
}

uint64_t aarch64_async_corrected_count(void)
{
    return __atomic_load_n(&g_async_corrected, __ATOMIC_RELAXED);
}

void aarch64_trap_entry(struct arch_trap_frame *frame)
{
    switch (frame->kind) {
    case AARCH64_ENTRY_EL1_SYNC:
        handle_sync(frame, false);
        return;
    case AARCH64_ENTRY_EL0_SYNC:
        handle_sync(frame, true);
        return;
    case AARCH64_ENTRY_EL1_IRQ:
    case AARCH64_ENTRY_EL0_IRQ:
        frame->vector = VEC_SPURIOUS;
        handle_irq(frame);
        return;
    case AARCH64_ENTRY_EL1_SERROR:
    case AARCH64_ENTRY_EL0_SERROR:
        handle_async_error(frame);
        return;
    default:
        frame->vector = VEC_SYNC_BASE + ARCH_TRAP_GENERAL_PROTECTION;
        panic_frame(frame, "exception in an unsupported vector slot %llu (EC 0x%x)",
                    (unsigned long long)(frame->kind - AARCH64_ENTRY_BAD_BASE), ESR_EC(frame->esr));
    }
}

unsigned arch_trap_vector_count(void)
{
    return VEC_COUNT;
}

int arch_trap_vector(enum arch_trap_kind kind)
{
    if ((unsigned)kind >= ARCH_TRAP_KIND_COUNT)
        return -1;
    return (int)(VEC_SYNC_BASE + (unsigned)kind);
}

bool arch_trap_is_exception(unsigned vector)
{
    return vector >= VEC_SYNC_BASE && vector < VEC_SYNC_BASE + ARCH_TRAP_KIND_COUNT;
}

const char *arch_trap_name(unsigned vector)
{
    if (arch_trap_is_exception(vector))
        return kind_names[vector - VEC_SYNC_BASE];
    if (vector == VEC_SPURIOUS)
        return "spurious interrupt";
    if (vector < GIC_SGI_COUNT)
        return "SGI";
    if (vector < GIC_SPI_BASE)
        return "PPI";
    if (vector < GIC_INTID_COUNT)
        return "SPI";
    return "interrupt";
}

uintptr_t arch_trap_frame_pc(const struct arch_trap_frame *frame) { return (uintptr_t)frame->elr; }
uint64_t arch_trap_frame_detail(const struct arch_trap_frame *frame) { return frame->esr; }
uintptr_t arch_trap_frame_sp(const struct arch_trap_frame *frame) { return (uintptr_t)frame->sp; }
uintptr_t arch_trap_frame_fp(const struct arch_trap_frame *frame) { return (uintptr_t)frame->x[29]; }

void arch_trap_frame_dump(const struct arch_trap_frame *f)
{
    kprintf("trap %llu (%s) ESR=0x%016llx EC=0x%02x FAR=0x%016llx\n", (unsigned long long)f->vector,
            arch_trap_name((unsigned)f->vector), (unsigned long long)f->esr, ESR_EC(f->esr),
            (unsigned long long)f->far);
    kprintf("ELR=%016llx SPSR=%08llx SP=%016llx %s\n", (unsigned long long)f->elr, (unsigned long long)f->spsr,
            (unsigned long long)f->sp, arch_trap_frame_is_user(f) ? "EL0" : "EL1");
    for (unsigned i = 0; i < 31; i += 4) {
        kprintf("X%-2u=%016llx X%-2u=%016llx", i, (unsigned long long)f->x[i], i + 1,
                (unsigned long long)f->x[i + 1]);
        if (i + 2 < 31)
            kprintf(" X%-2u=%016llx", i + 2, (unsigned long long)f->x[i + 2]);
        if (i + 3 < 31)
            kprintf(" X%-2u=%016llx", i + 3, (unsigned long long)f->x[i + 3]);
        kprintf("\n");
    }
    kprintf("SCTLR=%016llx TCR=%016llx TTBR0=%016llx TTBR1=%016llx\n",
            (unsigned long long)READ_SYSREG(sctlr_el1), (unsigned long long)READ_SYSREG(tcr_el1),
            (unsigned long long)READ_SYSREG(ttbr0_el1), (unsigned long long)READ_SYSREG(ttbr1_el1));
    if (f->vector == VEC_SYNC_BASE + ARCH_TRAP_PAGE_FAULT) {
        unsigned fl = arch_trap_fault_flags(f);
        kprintf("FAR=%016llx (%s %s %s%s%s)\n", (unsigned long long)f->far,
                (fl & ARCH_FAULT_PRESENT) ? "protection" : "not-present",
                (fl & ARCH_FAULT_WRITE) ? "write" : "read", (fl & ARCH_FAULT_USER) ? "user" : "kernel",
                (fl & ARCH_FAULT_RESERVED) ? " reserved-bit" : "", (fl & ARCH_FAULT_EXEC) ? " instruction-fetch" : "");
    }
}

void arch_trap_unhandled(unsigned vector, struct arch_trap_frame *frame)
{
    if (arch_trap_is_exception(vector))
        panic_frame(frame, "unhandled exception %u (%s)", vector, arch_trap_name(vector));
    g_unhandled++;
    kwarn("aarch64: unhandled interrupt vector %u (%llu total unhandled)", vector, (unsigned long long)g_unhandled);
}

void arch_debug_break(void)
{
    __asm__ volatile("brk #0" ::: "memory");
}

bool arch_trap_fixup(struct arch_trap_frame *frame)
{
    uintptr_t fixup = extable_fixup((uintptr_t)frame->elr);
    if (fixup == 0)
        return false;
    frame->elr = fixup;
    return true;
}

uintptr_t arch_trap_fault_address(const struct arch_trap_frame *frame)
{
    KASSERT(frame->vector == VEC_SYNC_BASE + ARCH_TRAP_PAGE_FAULT);
    return (uintptr_t)frame->far;
}

unsigned arch_trap_fault_flags(const struct arch_trap_frame *frame)
{
    KASSERT(frame->vector == VEC_SYNC_BASE + ARCH_TRAP_PAGE_FAULT);
    unsigned ec = ESR_EC(frame->esr);
    unsigned iss = ESR_ISS(frame->esr);
    unsigned f = 0;
    if (ec == ESR_EC_IABT_LOWER || ec == ESR_EC_IABT_CUR)
        f |= ARCH_FAULT_EXEC;
    else if (iss & ESR_ISS_WNR)
        f |= ARCH_FAULT_WRITE;
    if (ec == ESR_EC_IABT_LOWER || ec == ESR_EC_DABT_LOWER)
        f |= ARCH_FAULT_USER;
    unsigned fsc = ESR_ISS_FSC(iss);
    if (FSC_PERMISSION(fsc) || FSC_ACCESS_FLAG(fsc))
        f |= ARCH_FAULT_PRESENT;
    else if (!FSC_TRANSLATION(fsc))
        f |= ARCH_FAULT_RESERVED;
    return f;
}

/* --- asynchronous errors (invariant I-ARCH-16) --------------------------------------
 *
 * An SError's syndrome says how bad the error was, never who caused it:
 * the frame names the context interrupted when the abort was *delivered*.
 * So this classifier answers severity only, and the handler that uses it
 * blames nobody (docs/audit/next-subsystem-async-error.md, Design 3).
 *
 * Pure and separate from the register reads so the encodings can be
 * table-tested, including the ones no CI CPU will ever produce.
 */
enum arch_async_error aarch64_async_class(uint64_t esr, unsigned ras)
{
    /* No FEAT_RAS: ESR carries no AET at all, so nothing is knowable and
     * the only honest answer is the conservative one. */
    if (ras == 0)
        return ARCH_ASYNC_UNCONTAINED;
    /* IDS: the rest of the syndrome is IMPLEMENTATION DEFINED. Whatever
     * it means, it does not mean what the AET table below means. */
    if (esr & ESR_SERROR_IDS)
        return ARCH_ASYNC_UNCONTAINED;
    switch (ESR_SERROR_AET(esr)) {
    case ESR_AET_CE:
        return ARCH_ASYNC_CORRECTED;
    case ESR_AET_UEO:
    case ESR_AET_UER:
        return ARCH_ASYNC_CONTAINED;   /* the machine is intact; who did it is not said */
    case ESR_AET_UC:
    case ESR_AET_UEU:
    default:
        return ARCH_ASYNC_UNCONTAINED;   /* including every reserved encoding */
    }
}

enum arch_async_error arch_async_error_class(const struct arch_trap_frame *frame)
{
    return aarch64_async_class(frame->esr, (unsigned)ID_AA64PFR0_RAS(READ_SYSREG(id_aa64pfr0_el1)));
}

const char *arch_async_error_name(enum arch_async_error c)
{
    switch (c) {
    case ARCH_ASYNC_CORRECTED:   return "corrected";
    case ARCH_ASYNC_CONTAINED:   return "contained";
    default:                     return "uncontained";
    }
}

/* --- arch/testhooks.h: the classifier over encodings this CPU never emits --- */

bool arch_test_async_class(const char **why)
{
    static const struct {
        uint64_t esr;
        unsigned ras;
        enum arch_async_error want;
        const char *what;
    } rows[] = {
        { (uint64_t)ESR_AET_CE  << 10, 1, ARCH_ASYNC_CORRECTED,   "CE: the hardware fixed it" },
        { (uint64_t)ESR_AET_UER << 10, 1, ARCH_ASYNC_CONTAINED,   "UER: intact, but unattributable" },
        { (uint64_t)ESR_AET_UEO << 10, 1, ARCH_ASYNC_CONTAINED,   "UEO: restartable" },
        { (uint64_t)ESR_AET_UC  << 10, 1, ARCH_ASYNC_UNCONTAINED, "UC" },
        { (uint64_t)ESR_AET_UEU << 10, 1, ARCH_ASYNC_UNCONTAINED, "UEU" },
        { 4ull << 10,                  1, ARCH_ASYNC_UNCONTAINED, "reserved AET 4" },
        { 5ull << 10,                  1, ARCH_ASYNC_UNCONTAINED, "reserved AET 5" },
        { 7ull << 10,                  1, ARCH_ASYNC_UNCONTAINED, "reserved AET 7" },
        { ((uint64_t)ESR_AET_CE << 10) | ESR_SERROR_IDS, 1, ARCH_ASYNC_UNCONTAINED,
          "IDS set: the syndrome is implementation-defined, whatever AET looks like" },
        { (uint64_t)ESR_AET_CE << 10, 0, ARCH_ASYNC_UNCONTAINED,
          "no FEAT_RAS: there is no AET to have read" },
    };
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        if (aarch64_async_class(rows[i].esr, rows[i].ras) != rows[i].want) {
            *why = rows[i].what;
            return false;
        }
    }
    kinfo("selftest: trap-async-class: %u SError encodings -- three reserved AETs, IDS, and a CPU "
          "without FEAT_RAS; everything not positively corrected is uncontained",
          (unsigned)(sizeof(rows) / sizeof(rows[0])));
    return true;
}

/*
 * A virtual SError, delivered through EL2 (invariant I-ARCH-16). The only class
 * that returns is one the hardware calls corrected, so this test injects
 * exactly that and asserts the machine carried on -- the panic arm is not
 * injectable by definition, and is covered by the classifier's table.
 *
 * Needs FEAT_RAS: without it VSESR_EL2 does not exist, so the syndrome
 * cannot be set and the abort would classify as uncontained and stop the
 * machine. CI's cortex-a72 has no RAS and cortex-a76 (make test-guard)
 * does, so this runs on the guard boot and skips on the default one --
 * which is stated rather than hidden, because a test that silently does
 * nothing is worse than one that says it did nothing.
 */
bool arch_test_async_inject(const char **why)
{
    unsigned ras = (unsigned)ID_AA64PFR0_RAS(READ_SYSREG(id_aa64pfr0_el1));
    if (ras == 0 || !el2_available()) {
        kinfo("selftest: trap-async-inject: %s; the corrected arm is covered by trap-async-class only",
              ras == 0 ? "no FEAT_RAS on this CPU model" : "no EL2");
        return true;
    }
    uint64_t before = aarch64_async_corrected_count();
    /* EC 0x2F with IDS clear and AET = CE: the syndrome for an error the
     * hardware corrected. */
    uint64_t esr = ((uint64_t)ESR_EC_SERROR << ESR_EC_SHIFT) | ((uint64_t)ESR_AET_CE << 10);
    if (el2_call_raw(HV_EL2_CALL_VSE, esr) != 0) {
        *why = "EL2 refused the virtual SError";
        return false;
    }
    /*
     * And then unmask it, because this kernel runs EL1 with PSTATE.A set
     * from its first instruction (entry.S: `msr daifset, #0xF`, and only
     * `daifclr, #2` -- IRQ -- is ever cleared). So an asynchronous abort
     * is not taken while the kernel runs at all; it stays pending until
     * something unmasks it. User mode does not have that property: EL0
     * runs with DAIF clear, so an SError there is taken immediately, and
     * that is the path this unit's dispatch is for.
     *
     * The window is kept short and closed again by hand: while VSE is set
     * the abort is pending continuously and is re-taken on every return
     * with A clear, so the count may move by more than one before the
     * clear call takes it back. More than once is the assertion, not
     * exactly once.
     */
    __asm__ volatile("msr daifclr, #4" ::: "memory");   /* A */
    for (unsigned i = 0; i < 1000 && aarch64_async_corrected_count() == before; i++)
        __asm__ volatile("isb" ::: "memory");
    __asm__ volatile("msr daifset, #4" ::: "memory");
    (void)el2_call_raw(HV_EL2_CALL_VSE_CLEAR, 0);
    if (aarch64_async_corrected_count() <= before) {
        *why = "a corrected SError was not delivered, or did not return";
        return false;
    }
    kinfo("selftest: trap-async-inject: a corrected SError was taken at EL1 and execution continued "
          "(count %llu); before this unit the same abort stopped the machine",
          (unsigned long long)aarch64_async_corrected_count());
    return true;
}

/*
 * Nothing to register: an SError arrives through vector slots 7 and 11,
 * which aarch64_trap_entry dispatches itself, so there is no vector
 * number for interrupt_register to hold (I-ARCH-16). The function exists
 * because x86-64's #MC does need registering and main.c should not know
 * which architecture it is on.
 */
void arch_async_error_init(void)
{
}
