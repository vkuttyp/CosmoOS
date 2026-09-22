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
    /* A ready thread on `rq` that may leave for some CPU in `allowed`:
     * not `rq->current`, its affinity admitting one of them; NULL when
     * none may. Called with `rq->lock` held. The policy chooses which of
     * the eligible it offers (round-robin: the one it would run last). */
    struct thread *(*pick_migratable)(struct runqueue *rq, cpumask_t allowed);
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
struct arch_trap_frame;
void sched_tick(uint64_t now_ns, struct arch_trap_frame *frame);

/*
 * Migration: move one ready thread from one CPU's run queue to another's
 * (docs/kernel/scheduler/design.md, "Migration"; invariants S24-S26).
 *
 * Only a THREAD_READY thread that is not its queue's `current` moves --
 * a running thread is on its CPU's stack, a blocked one is on no queue
 * and wakes on its own `t->cpu`, and a woken-before-blocked thread is
 * both current and queued until it runs `sched_set_running_current`.
 * Both run-queue locks are taken inside, in increasing CPU-id order
 * (S24), and released before the return. **Neither entry may be called
 * with a run-queue lock held.** Callable with interrupts off or from a
 * tick. The result says which check refused, so a caller asks *which*,
 * never *whether*.
 */
enum sched_migrate_result {
    SCHED_MIGRATED,              /* moved: t->cpu == cpu, queued there */
    SCHED_MIGRATE_SAME_CPU,      /* already there */
    SCHED_MIGRATE_NOT_READY,     /* RUNNING (no queue entry), BLOCKED, EXITED; or the queue offered nothing */
    SCHED_MIGRATE_CURRENT,       /* READY but rq->current: the woken-before-blocked window */
    SCHED_MIGRATE_AFFINITY,      /* cpu not in t->affinity */
    SCHED_MIGRATE_OFFLINE,       /* cpu not online, or not a CPU */
};
const char *sched_migrate_result_name(enum sched_migrate_result r);

/* Move `t` to `cpu`. */
enum sched_migrate_result sched_migrate(struct thread *t, unsigned cpu);
/* Move a thread of the policy's choosing from CPU `from`'s queue to
 * `to`'s: the selection and the move under both locks, no hand-off.
 * `*moved` names the thread on SCHED_MIGRATED, NULL otherwise. */
enum sched_migrate_result sched_migrate_from(unsigned from, unsigned to, struct thread **moved);
/* Moves made since boot (every entry, the chaos migrator included). */
uint64_t sched_migration_count(void);
#if CONFIG_SCHED_CHAOS
/* The chaos migrator's tally (debug builds with SCHED_CHAOS=1): moves
 * made from the tick, and calls that found nothing to move. */
void sched_chaos_stats(uint64_t *migrated, uint64_t *refused);
#endif

struct runqueue *sched_runqueue(unsigned cpu);

/* Diagnostics. */
void sched_dump(void);
/* A subsystem's contribution to sched_dump: printed after the thread
 * table on every dump (the watchdog's, a lockup report's, the panic's).
 * The hook runs in interrupt context with interrupts off and must take
 * no lock another CPU may hold (a hung CPU's, say): print counters. */
void sched_dump_register(const char *name, void (*fn)(void));
uint64_t sched_switch_count(unsigned cpu);

/* Hang watchdog: if sched_watchdog_kick() is not called for `timeout_ns`
 * while armed, the boot CPU's tick prints every thread and run queue
 * once. Used by the self-test runner; costs one comparison per tick. */
void sched_watchdog_arm(uint64_t timeout_ns);
void sched_watchdog_kick(void);
void sched_watchdog_disarm(void);

#endif /* KERNEL_SCHED_H */
