/*
 * irqpoll.h - Bounded completion handling for interrupt handlers
 * (docs/kernel/interrupt/design.md, "Bounded completion handling").
 *
 * A device's completion handler consumes what the device has finished
 * until it finds nothing more. A device that keeps finishing -- refilled
 * by a submitter on another CPU, or by the handler itself, since
 * bio_complete hands the next waiting bio to the driver -- keeps that
 * handler running, with interrupts off, for as long as the refilling
 * lasts (docs/testing/flakes.md, "held for 184 s"). An irq_poll bounds it:
 *
 *   - The handler calls irq_poll_sched(). The driver's `poll` runs there,
 *     consuming at most IRQ_POLL_BUDGET completions.
 *   - If it used the whole budget, the rest is deferred to this CPU's
 *     "irqpoll/N" worker, a kernel thread that calls `poll` again, a
 *     budget at a time, yielding between batches, until a call returns
 *     less than the budget.
 *   - One consumer at a time: a handler that finds `poll` running (on
 *     another CPU, or in the worker) or queued only notes that there is
 *     more, and the running one takes it before going idle. Completions
 *     are therefore consumed in the device's order, as before.
 *
 * The deferred remainder always runs: the worker is pinned to an online
 * CPU, runnable from the moment it is queued, at the default priority, and
 * it re-queues itself while `poll` keeps using its budget. Until the
 * workers are started (boot, before drivers load) a deferral is not
 * possible and the handler polls to the end, as before.
 *
 * irq_poll_disable() is the teardown's half: on return no `poll` is
 * running or queued, and none will start until irq_poll_enable(). A
 * driver calls it after its interrupt is released and before it frees
 * what `poll` reads -- the deferred half of synchronize_irq.
 */
#ifndef KERNEL_IRQPOLL_H
#define KERNEL_IRQPOLL_H

#include <stdbool.h>
#include <stdint.h>

#include <kernel/list.h>
#include <kernel/spinlock.h>

#ifndef IRQ_POLL_BUDGET   /* overridable for a probe: tools/irq-budget-probe.py --old */
#define IRQ_POLL_BUDGET 32u
#endif

struct irq_poll;
/* Consume at most `budget` completions; return how many were consumed.
 * Fewer than `budget` means the device had nothing more when it looked.
 * Called with interrupts off from irq_poll_sched, with them on from the
 * worker, never on two CPUs at once for one irq_poll. */
typedef unsigned (*irq_poll_fn)(struct irq_poll *ip, unsigned budget);

struct irq_poll {
    irq_poll_fn poll;
    const char *name;
    spinlock_t lock;
    bool running;       /* `poll` is executing (handler or worker) */
    bool scheduled;     /* handed to a worker, not yet started there */
    bool queued;        /* on the worker's list (that worker's lock) */
    bool again;         /* an interrupt came while running or scheduled */
    bool disabled;
    unsigned cpu;       /* the worker it was handed to */
    struct list_node link;
    uint64_t runs;      /* calls of `poll` finished: irq_poll_synchronize's generation */
    uint64_t calls;     /* irq_poll_sched calls that ran `poll` */
    uint64_t deferred;  /* hand-offs to a worker */
    uint32_t max_one;   /* most consumed by one call of `poll` */
};

void irq_poll_init(struct irq_poll *ip, irq_poll_fn poll, const char *name);
/* From the interrupt handler (interrupts off). */
void irq_poll_sched(struct irq_poll *ip);
/* Thread context. Sleeps (yields) until no `poll` runs or is queued. */
void irq_poll_disable(struct irq_poll *ip);
void irq_poll_enable(struct irq_poll *ip);
/* Thread context: wait until a call of `poll` running now has returned
 * (one that starts later is not waited for). What a handler's caller had
 * from synchronize_irq -- "whatever the handler was doing is done" --
 * for the half a worker may be running. */
void irq_poll_synchronize(struct irq_poll *ip);

/* kernel_main, once every CPU is online. */
void irq_poll_start_workers(void);

#endif /* KERNEL_IRQPOLL_H */
