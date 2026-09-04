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
    IPI_KIND_COUNT
};

/* Allocate vectors and register handlers. Requires irq_init. */
void ipi_init(void);

void ipi_send(unsigned cpu, enum ipi_kind kind);
void ipi_broadcast_others(enum ipi_kind kind);

/* Diagnostics: IPIs of `kind` handled on this CPU. */
uint64_t ipi_count(enum ipi_kind kind);

#endif /* KERNEL_IPI_H */
