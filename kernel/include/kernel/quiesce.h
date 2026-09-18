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
/* The same, plus waking anyone waiting for a grace period. Only from a
 * context holding no lock and able to schedule -- the trap returns. The
 * plain form is what the scheduler's own quiescent points use, because a
 * wake from inside the scheduler re-enters it (invariant Q-W). */
void quiesce_note_quiescent_preemptible(void);
/* This CPU published inside a straggler kick's own trap return. Called
 * from the architecture trap tails only (invariant Q19). */
void quiesce_note_kick_published(void);
/* Publishes attributed to a kick on `cpu`. Per-CPU, not per-waiter:
 * see the definition for why no per-waiter figure is available. */
uint64_t quiesce_kick_publishes(unsigned cpu);

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
    uint64_t straggler_ipis;     /* straggler kicks sent to slow CPUs */
    /* Kicks that WORKED, machine-wide: a publish that happened in a
     * kick's own trap return. `straggler_ipis` counts kicks sent, so
     * before this counter existed no number in the tree would have
     * changed if the kick were replaced by a no-op
     * (docs/audit/next-subsystem-straggler-kick.md). */
    uint64_t kick_publishes;
    /* How a waiter's block ENDED, not that it happened: on more than one
     * CPU the waiter always blocks, because the epoch it waits for was
     * bumped a moment earlier and nobody has published it yet. With the
     * wake, an idle machine's grace period ends by being woken and this
     * does not move; without it, every block ends at its deadline
     * (invariant Q-W). */
    uint64_t gp_timeouts;        /* grace-period waits that reached their deadline */
    /* Wakes actually sent to a queued grace-period waiter. Zero unless
     * the wake path runs at all, which is what makes it the observable:
     * how *long* a grace period takes depends on where the other CPUs'
     * ticks fall, but whether the mechanism fires does not (Q18). */
    uint64_t gp_wakes;
    uint64_t irq_syncs;          /* synchronize_irq calls */
    uint64_t timer_sync_waits;   /* timer_cancel_sync calls that waited for a running callback */
};
void quiesce_get_stats(struct quiesce_stats *out);
#if CONFIG_DEBUG
/* A grace period that hands back the straggler kicks *it* sent.
 * `straggler_ipis` in the stats above is machine-wide, so a per-waiter
 * claim cannot be made against it -- nor against a per-CPU slot, which
 * the unpinned callback worker can overwrite between the call and the
 * read. The count comes back on the stack
 * (docs/audit/next-subsystem-lifetime-windows.md). */
unsigned quiesce_test_sync_kicks(void);
/* One grace period, returning how many of its own blocks ended at a
 * deadline rather than by being woken -- this call's, not the machine's. */
unsigned quiesce_test_sync_timeouts(void);
#endif
/* Per-CPU diagnostics (debug builds): read depth and transitions. */
uint32_t quiesce_cpu_depth(unsigned cpu);
uint64_t quiesce_cpu_transitions(unsigned cpu);
void quiesce_count_timer_wait(void);   /* timer.c */

#endif /* KERNEL_QUIESCE_H */
