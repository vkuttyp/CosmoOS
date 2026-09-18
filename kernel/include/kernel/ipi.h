/*
 * ipi.h - Inter-processor interrupts by symbolic kind.
 *
 * Vectors are allocated at ipi_init and never exposed. Senders may run
 * in any context including interrupt handlers (the controller send path
 * spins only on the ICR busy bit). Handlers run in interrupt context on
 * the target.
 */

#ifndef KERNEL_IPI_H
#define KERNEL_IPI_H

#include <kernel/compiler.h>

enum ipi_kind {
    IPI_RESCHEDULE,  /* target re-evaluates need_resched on interrupt return */
    IPI_CALL,        /* run the pending smp_call_function_single request */
    IPI_TLB_FLUSH,   /* invalidate the pending shootdown range */
    IPI_HALT,        /* stop forever (panic, shutdown) */
    IPI_SAMPLE,      /* record this CPU's frame for a lockup report (lockup_answer) */
    /*
     * Make the target take an interrupt so its trap tail runs, and
     * nothing else. It does NOT set need_resched and the sender holds no
     * run-queue lock, which is why it is not IPI_RESCHEDULE: that kind's
     * contract is "re-evaluates need_resched on interrupt return", and a
     * send suppressed because the target's need_resched is clear -- the
     * natural "nothing to reschedule there" optimisation -- would
     * silently stop every straggler kick (quiesce.c, and
     * docs/audit/next-subsystem-straggler-kick.md).
     */
    IPI_QUIESCE_KICK,
    IPI_KIND_COUNT
};

/* Allocate vectors and register handlers. Requires irq_init. */
void ipi_init(void);

void ipi_send(unsigned cpu, enum ipi_kind kind);
void ipi_broadcast_others(enum ipi_kind kind);

/* Diagnostics: IPIs of `kind` handled on this CPU. */
uint64_t ipi_count(enum ipi_kind kind);

#endif /* KERNEL_IPI_H */
