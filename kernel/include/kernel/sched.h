/*
 * sched.h - Scheduler mechanism and policy interface.
 *
 * schedule() is the single switch point. It requires: not in interrupt
 * context, preemption not disabled by the caller (the run-queue lock it
 * takes itself is exempt), interrupts in any state (saved and restored).
 * sched_wake() and the tick are interrupt-safe.
 */

#ifndef KERNEL_SCHED_H
#define KERNEL_SCHED_H

#include <kernel/list.h>
#include <kernel/spinlock.h>
#include <kernel/thread.h>

#define SCHED_SLICE_NS (10u * 1000u * 1000u) /* 10 ms */

struct runqueue {
    spinlock_t lock;
    uint64_t bitmap;                         /* bit p: ready[p] non-empty */
    struct list_node ready[SCHED_PRIO_COUNT];
    unsigned nr_running;                     /* threads queued (not current) */
    struct thread *current;
    struct thread *idle;
    struct thread *prev_exited;              /* handed to sched_finish_switch */
    uint64_t switches;
    unsigned cpu;
};

struct sched_policy {
    const char *name;
    void (*enqueue)(struct runqueue *rq, struct thread *t, bool at_head);
    void (*dequeue)(struct runqueue *rq, struct thread *t);
    struct thread *(*pick_next)(struct runqueue *rq);   /* NULL: run idle */
    void (*tick)(struct runqueue *rq, struct thread *current, uint64_t elapsed_ns);
    void (*slice_new)(struct thread *t);
};

extern const struct sched_policy sched_policy_rr;

/* Boot CPU: turn the boot context into thread 0, create the idle
 * thread, register the tick hook. Requires timer_init. */
void sched_init(void);

/* Calling AP: create its idle thread and run queue, then enter the idle
 * loop (never returns). */
void sched_start_cpu(void) __noreturn;

void schedule(void);
void sched_yield(void);

/* Called by preempt_enable and the interrupt-return path when
 * need_resched is set and the context is preemptible. */
void sched_preempt(void);

/* Make a BLOCKED thread READY. Returns true if it did; false (no-op) if
 * the thread was already READY or RUNNING, which wakers use to keep
 * looking for a waiter that actually needs waking. */
bool sched_wake(struct thread *t);

/* Caller has set current->state = THREAD_BLOCKED under a wait-queue
 * lock and released that lock; this schedules away and returns when the
 * thread is woken. */
void sched_block_current(void);

/* Tick hook: slice accounting for the current thread on this CPU. */
void sched_tick(uint64_t now_ns);

struct runqueue *sched_runqueue(unsigned cpu);

/* Diagnostics. */
void sched_dump(void);
uint64_t sched_switch_count(unsigned cpu);

/* Hang watchdog: if sched_watchdog_kick() is not called for `timeout_ns`
 * while armed, the boot CPU's tick prints every thread and run queue
 * once. Used by the self-test runner; costs one comparison per tick. */
void sched_watchdog_arm(uint64_t timeout_ns);
void sched_watchdog_kick(void);
void sched_watchdog_disarm(void);

#endif /* KERNEL_SCHED_H */
