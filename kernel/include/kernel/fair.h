/*
 * fair.h - The weighted fair share at the default priority level: nice
 * values' weights and the virtual-time arithmetic that orders the level
 * (docs/kernel/scheduler/design.md, "The fair level").
 *
 * The 63 other levels stay round-robin. At SCHED_PRIO_DEFAULT each thread
 * has a virtual run time, `vruntime`: its run time scaled by 1024 over its
 * weight, Linux's weight for its nice value (nice 0 = 1024, each step
 * about 1.25 times the next). The level's ready list is kept in vruntime
 * order and the leftmost runs, so over time each thread receives CPU time
 * in proportion to its weight. The tick switches the running thread out
 * once it is more than FAIR_GRANULARITY_NS of virtual time ahead of the
 * leftmost. A thread that joins the level -- new, woken, back from a boost
 * or from another CPU -- is placed no earlier than the queue's
 * `min_vruntime` minus FAIR_WAKE_BONUS_NS, so a long sleep earns a short
 * lead, not a monopoly.
 *
 * The functions are Nova's (kernel/scheduler/fair.c, generated). They keep
 * no state and cannot fail; the caller holds the run-queue lock, often in
 * the tick.
 */

#ifndef KERNEL_FAIR_H
#define KERNEL_FAIR_H

#include <kernel/types.h>

#define FAIR_NICE_0_WEIGHT 1024u
/* How far ahead of the leftmost the running thread may get: a tick of
 * virtual time. Two equal threads then take turns of 12 to 16 ms (twice
 * this and up to two ticks), about the 10 ms slice round-robin gives. */
#define FAIR_GRANULARITY_NS (4u * 1000u * 1000u)
#define FAIR_WAKE_BONUS_NS (5u * 1000u * 1000u)     /* half a slice */

/* Linux's sched_prio_to_weight for `nice`, clamped to -20..19: 88761 at
 * -20 to 15 at 19. */
uint32_t fair_weight(int32_t nice);

/* `vruntime` advanced by `delta_ns` of run time at `weight` (> 0):
 * ⌊delta_ns · 1024 / weight⌋ more, exactly, saturating at UINT64_MAX. */
uint64_t fair_charge(uint64_t vruntime, uint64_t delta_ns, uint32_t weight);

/* Where a thread joining the level starts: its own vruntime, but no
 * earlier than `min_vruntime - bonus` (not below 0). */
uint64_t fair_place(uint64_t vruntime, uint64_t min_vruntime, uint64_t bonus);

/* A vruntime from a queue whose minimum is `from_min`, carried to one whose
 * minimum is `to_min`: the same lead or lag, saturating at both ends. */
uint64_t fair_rebase(uint64_t vruntime, uint64_t from_min, uint64_t to_min);

/* Whether a thread at vruntime `a` runs before one at `b`: the level's
 * order. Ties keep arrival order, so `a` goes after an equal `b`. */
bool fair_before(uint64_t a, uint64_t b);

/* Whether the running thread, at `current`, has run far enough ahead of
 * the leftmost ready one, at `leftmost`, to give way: more than
 * `granularity` ahead. */
bool fair_should_preempt(uint64_t current, uint64_t leftmost, uint64_t granularity);

/* The queue's new min_vruntime: never below `min_vruntime`, and otherwise
 * the smaller of the running thread's and the leftmost ready thread's
 * vruntimes, among those that exist. */
uint64_t fair_min(uint64_t min_vruntime, bool has_current, uint64_t current, bool has_leftmost, uint64_t leftmost);

#endif
