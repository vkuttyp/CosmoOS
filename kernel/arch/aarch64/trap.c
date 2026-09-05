/*
 * trap.c - Exception classification, the trap contract, the syscall entry
 * (docs/kernel/arch/aarch64/design.md, "Exceptions and interrupts").
 *
 * Vector numbering (arch/trap.h contract on this architecture):
 *   0..1019      GIC INTIDs, 1020 spurious
 *   1024..1029   synchronous exception kinds (enum arch_trap_kind)
 *   1056..1311   dynamic software vectors routed to INTIDs by gic.c
 */

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
};

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
        process_return_to_user();
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
    /* Quiescent point and preemption point (docs/kernel/quiesce/): the
     * interrupted context holds no spinlock, is not an interrupt, and had
     * interrupts enabled. */
    if (pc->irq_depth == 0 && pc->preempt_count == 0 && (frame->spsr & DAIF_I) == 0) {
        quiesce_note_quiescent();
        if (pc->need_resched)
            sched_preempt();
    }
    return_to_user_check(frame);
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
