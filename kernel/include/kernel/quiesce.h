/*
 * quiesce.h - Kernel object lifetime: read-side sections, grace periods,
 * deferred reclamation (docs/kernel/quiesce/).
 *
 * The invariant this provides: an object cannot be reclaimed until every
 * CPU that could still have a reference to it has passed through a
 * provably safe quiescent state.
 *
 *   Read side     a preemption-disabled region (every spinlock is one, so
 *                 is every interrupt handler). quiesce_read_lock/unlock
 *                 name it explicitly for lock-free readers. Must not block.
 *   Quiescent     interrupt return to a context with no interrupt, no
 *                 preemption disable and interrupts enabled; schedule();
 *                 the idle loop; a CPU coming online. Nothing else.
 *   Reclaim       unlink the object from everything readers walk, then
 *                 synchronize_quiesce() (sleeps) or call_quiesce() (any
 *                 context; the callback runs after a grace period), then
 *                 free.
 *
 * Every user documents what is protected, who reads, who unlinks, who
 * synchronises, who frees, and why a reference count alone is not enough
 * (docs/kernel/quiesce/design.md, "Users").
 */

#ifndef KERNEL_QUIESCE_H
#define KERNEL_QUIESCE_H

#include <kernel/compiler.h>
#include <kernel/percpu.h>

struct quiesce_head {
    struct quiesce_head *next;
    void (*fn)(struct quiesce_head *h);
    bool pending;   /* submitted and not yet run; a second call_quiesce panics */
};

/* After sched_init: the callback worker thread. */
void quiesce_init(void);

void quiesce_read_lock_debug(void);
void quiesce_read_unlock_debug(void);

/* Read-side critical section: preemption off (plus a debug depth
 * counter). Nestable. No blocking inside. */
static inline void quiesce_read_lock(void)
{
    preempt_disable();
    quiesce_read_lock_debug();
}
static inline void quiesce_read_unlock(void)
{
    quiesce_read_unlock_debug();
    preempt_enable();
}

/* The calling CPU is quiescent here. Called by the scheduler, the idle
 * loop, the arch interrupt-return tails and CPU bring-up; not by users. */
void quiesce_note_quiescent(void);

/* Wait for one grace period over the CPUs online now: every one of them
 * passes a quiescent state after this call began. Sleeps; never with a
 * spinlock held (asserted). Returns only when done. */
void synchronize_quiesce(void);

/* Run `fn(h)` in thread context after a grace period that begins after
 * this call. Any context. `h` belongs to the subsystem from this call
 * until `fn` runs: submitting it again before then is a bug and panics
 * (the object it is embedded in is about to be freed by the first
 * callback). `h` must be zero-initialised or previously run. */
void call_quiesce(struct quiesce_head *h, void (*fn)(struct quiesce_head *h));

struct quiesce_stats {
    uint64_t epoch;
    uint64_t synchronizes;       /* synchronize_quiesce calls completed */
    uint64_t callbacks;          /* call_quiesce callbacks run */
    uint64_t max_wait_ns;        /* longest grace period observed */
    uint64_t straggler_ipis;     /* reschedule IPIs sent to slow CPUs */
    uint64_t irq_syncs;          /* synchronize_irq calls */
    uint64_t timer_sync_waits;   /* timer_cancel_sync calls that waited for a running callback */
};
void quiesce_get_stats(struct quiesce_stats *out);
/* Per-CPU diagnostics (debug builds): read depth and transitions. */
uint32_t quiesce_cpu_depth(unsigned cpu);
uint64_t quiesce_cpu_transitions(unsigned cpu);
void quiesce_count_timer_wait(void);   /* timer.c */

#endif /* KERNEL_QUIESCE_H */
