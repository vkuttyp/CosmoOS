/*
 * sched.c - Scheduler mechanism: run queues, the switch point, wake and
 * block, preemption, idle.
 *
 * Every state transition of a thread happens under the run-queue lock of
 * the CPU the thread belongs to. schedule() holds that lock across the
 * context switch; whoever runs next releases it in sched_finish_switch().
 */

#include <kernel/ipi.h>
#include <kernel/lockdep.h>
#include <kernel/lockup.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/printf.h>
#include <kernel/quiesce.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/timer.h>
#include <kernel/vmm.h>

#include <arch/context.h>
#include <arch/cpu.h>
#include <arch/irq.h>

#include "sched_internal.h"

static struct runqueue g_rqs[CONFIG_MAX_CPUS];
static const struct sched_policy *g_policy = &sched_policy_rr;
static bool g_initialized;
static uint64_t g_migrations;

/*
 * One lockdep class per run queue. lockdep keys a class by the name
 * pointer, so every queue initialised from the one literal "runqueue"
 * was one class, and the order of two of them -- increasing CPU id,
 * S24 -- was a rule it could not check: a second acquisition read as
 * recursion. A name per instance, in storage that lives as long as the
 * queue, makes a reversed pair a cycle it reports. */
static char g_rq_lock_names[CONFIG_MAX_CPUS][16];

static void rq_init(struct runqueue *rq, unsigned cpu)
{
    memset(rq, 0, sizeof(*rq));
    ksnprintf(g_rq_lock_names[cpu], sizeof(g_rq_lock_names[cpu]), "runqueue%u", cpu);
    spinlock_init(&rq->lock, g_rq_lock_names[cpu]);
    for (int p = 0; p < SCHED_PRIO_COUNT; p++)
        list_init(&rq->ready[p]);
    rq->cpu = cpu;
}

struct runqueue *sched_runqueue(unsigned cpu)
{
    return cpu < CONFIG_MAX_CPUS ? &g_rqs[cpu] : NULL;
}

/* --- idle --- */

static void idle_main(void *arg)
{
    (void)arg;

    /* An AP's bootstrap stack is unused once its idle thread runs on its
     * own stack; return it. The boot CPU has none (thread 0 owns the
     * static boot stack). */
    struct percpu *pc = this_cpu();
    if (pc->boot_stack != 0) {
        vaddr_t stack = pc->boot_stack;
        pc->boot_stack = 0;
        vm_kernel_free(stack);
    }

    for (;;) {
        /* Quiescent: no thread work, no read section (docs/kernel/quiesce/).
         * While halted, the next interrupt's return publishes again.
         *
         * The waking form, because on an idle machine THIS is the publish
         * that finishes a grace period: the other CPUs are here, and
         * their next trap return is up to a tick away -- further off than
         * the waiter's own deadline, so a wake only at trap returns never
         * arrives in time (invariant Q-W). Safe from here for the same
         * reason the trap return is: nothing is held, and schedule() is
         * called two lines down. */
        quiesce_note_quiescent_preemptible();
        if (this_cpu()->need_resched)
            schedule();
        else
            arch_cpu_wait_for_interrupt();
    }
}

static struct thread *create_idle(unsigned cpu)
{
    struct thread *idle = thread_prepare(idle_main, NULL, "idle", SCHED_PRIO_LOWEST, THREAD_FLAG_IDLE);
    if (idle == NULL)
        panic("sched: cannot create idle thread for CPU %u", cpu);
    idle->cpu = (int)cpu;
    idle->affinity = CPUMASK_OF(cpu);
    idle->state = THREAD_READY;
    idle->refcount = 1;
    return idle;
}

/* --- init --- */

void sched_init(void)
{
    KASSERT(!g_initialized);
    struct percpu *pc = this_cpu();
    KASSERT(pc->cpu_id == 0);

    thread_init_subsystem();

    struct runqueue *rq = &g_rqs[0];
    rq_init(rq, 0);
    pc->rq = rq;

    /* The running boot context becomes thread 0. */
    struct thread *boot = thread_alloc("kmain", SCHED_PRIO_DEFAULT, THREAD_FLAG_BOOT);
    if (boot == NULL)
        panic("sched: cannot allocate thread 0");
    arch_boot_stack(&boot->stack_base, &boot->stack_size);
    boot->state = THREAD_RUNNING;
    boot->cpu = 0;
    boot->refcount = 1;
    boot->last_start_ns = clock_now_ns();
    g_policy->slice_new(boot);
    rq->current = boot;
    pc->current = boot;

    rq->idle = create_idle(0);
    pc->idle = rq->idle;

    timer_set_tick_hook(sched_tick);
    g_initialized = true;
    thread_reaper_start();
    kinfo("sched: policy '%s', slice %u ms, tick %u Hz", g_policy->name, SCHED_SLICE_NS / 1000000, CONFIG_HZ);
}

void sched_start_cpu(void)
{
    struct percpu *pc = this_cpu();
    struct runqueue *rq = &g_rqs[pc->cpu_id];
    rq_init(rq, pc->cpu_id);
    pc->rq = rq;
    rq->idle = create_idle(pc->cpu_id);
    pc->idle = rq->idle;

    /* Become the idle thread: switch into it from a context that is
     * never resumed. */
    struct arch_context dead;
    arch_irq_disable();
    spin_lock(&rq->lock);
    rq->current = rq->idle;
    pc->current = rq->idle;
    rq->idle->state = THREAD_RUNNING;
    rq->idle->last_start_ns = clock_now_ns();
    quiesce_note_quiescent();   /* a CPU coming online holds no reference from before */
    __atomic_store_n(&pc->online, true, __ATOMIC_RELEASE);
    arch_thread_switch_prepare(NULL, rq->idle);
    arch_context_switch(&dead, &rq->idle->ctx);
    panic("sched: AP bootstrap context resumed");
}

/* --- placement --- */

/*
 * Where a new thread goes: the least loaded CPU its affinity allows,
 * with ties rotating rather than always falling to the lowest-numbered.
 *
 * The rotation is the whole of this change and it matters more than it
 * looks. `nr_running` counts what is *runnable now*, and a kernel thread
 * spends almost all of its life blocked on a waitqueue -- so between any
 * two creations the queues have usually drained back to zero, every CPU
 * ties, and a scan that keeps the first winner gives every thread to
 * CPU 0. Measured before this changed: 8 of 14 threads there, and 94% of
 * the context switches (docs/audit/next-subsystem-thread-migration.md).
 *
 * Threads created back-to-back *without* blocking already spread, because
 * each one raises its target's count and the next scan sees it. That is
 * why the test for this creates threads that block first: it is the only
 * shape that distinguishes the rotation from what was here before.
 */
static unsigned g_pick_rotor;

/*
 * What a CPU is carrying: the threads queued on it plus the one it is
 * running, unless that one is its idle thread, which is not work.
 *
 * `nr_running` alone cannot answer this. It counts the ready list, and
 * `schedule` dequeues the thread it runs -- so a CPU spinning flat out
 * on a single compute-bound thread reports zero, and so does a CPU
 * asleep in `idle_main`. Every reader that wants "which CPU has the
 * least to do" was reading a number that cannot tell those two apart
 * (docs/audit/next-subsystem-load-balancer.md).
 *
 * **A hint, deliberately.** For another CPU's queue this reads two
 * fields without that queue's lock, so the answer can be stale before
 * it is used. That is sound for the two callers -- placement, which is
 * choosing between roughly-equal CPUs anyway, and the balancer, whose
 * move re-decides everything under both locks -- and it is why the
 * comparison against `idle` is an identity test on a pointer and never
 * a dereference: `rq->current` belongs to another CPU and may name a
 * thread that exits a moment later.
 */
unsigned sched_cpu_load(unsigned cpu)
{
    if (cpu >= CONFIG_MAX_CPUS)
        return 0;
    const struct runqueue *rq = &g_rqs[cpu];
    unsigned queued = __atomic_load_n(&rq->nr_running, __ATOMIC_RELAXED);
    const struct thread *cur = __atomic_load_n(&rq->current, __ATOMIC_RELAXED);
    return queued + (cur != NULL && cur != rq->idle ? 1u : 0u);
}

static unsigned pick_cpu(const struct thread *t)
{
    unsigned n = cpu_count();
    unsigned start = n ? __atomic_fetch_add(&g_pick_rotor, 1u, __ATOMIC_RELAXED) % n : 0;
    unsigned best = raw_cpu_id();   /* a default only: any online CPU in the mask overrides it below */
    unsigned best_load = ~0u;
    for (unsigned i = 0; i < n; i++) {
        unsigned c = (start + i) % n;
        if (!(t->affinity & CPUMASK_OF(c)) || !cpu_online(c))
            continue;
        unsigned load = sched_cpu_load(c);
        if (load < best_load) {
            best_load = load;
            best = c;
        }
    }
    return best;
}


/*
 * Balancing lived here and was removed before this branch shipped.
 *
 * It worked -- threads on CPU 0 went from 8 of 14 to 6 of 14 over a
 * self-test boot, and CPU 2 did ten times the context switches -- and it
 * made three of four aarch64 boots fail, once with seven tests at once
 * (`smp-wake`, `lockup-soft`, `quiesce-straggler`, `quiesce-grace`,
 * `irq-sync`, `timer-cancel-sync`, `lockdep-contention`). The same tree
 * with the balancer disabled did not. Seven concurrency tests failing
 * together is not timing sensitivity; something is being corrupted, and
 * the cause was not found. What was ruled out is written down in
 * `docs/audit/next-subsystem-thread-migration.md` so the next attempt
 * starts past it.
 *
 * What stays is the placement half: `pick_cpu` rotates its ties, so a
 * thread created on an idle machine is no longer always born on CPU 0.
 */

static void request_resched(struct runqueue *rq)
{
    struct percpu *pc = percpu_get(rq->cpu);
    pc->need_resched = true;
    /* Another CPU may be idle in hlt or running lower priority work:
     * interrupt it so its interrupt-return path sees the flag. */
    if (rq->cpu != arch_cpu_id() && cpu_online(rq->cpu))
        ipi_send(rq->cpu, IPI_RESCHEDULE);
}

void sched_enqueue_new(struct thread *t)
{
    KASSERT(g_initialized);
    unsigned cpu = pick_cpu(t);
    struct runqueue *rq = &g_rqs[cpu];

    arch_irq_state_t s = spin_lock_irqsave(&rq->lock);
    t->cpu = (int)cpu;
    t->state = THREAD_READY;
    g_policy->slice_new(t);
    g_policy->enqueue(rq, t, false);
    if (rq->current == NULL || t->priority < rq->current->priority)
        request_resched(rq);
    spin_unlock_irqrestore(&rq->lock, s);
}

/* --- the switch --- */

void sched_finish_switch(void)
{
    struct runqueue *rq = this_cpu()->rq;
    struct thread *exited = rq->prev_exited;
    rq->prev_exited = NULL;
    spin_unlock(&rq->lock);
    /* Interrupts may still be disabled here (resumed inside a trap
     * handler); freeing a stack needs a TLB shootdown, so defer. */
    if (exited != NULL)
        thread_reap_later(exited);
}

/*
 * `preempt` distinguishes an involuntary switch (interrupt return,
 * preempt_enable) from a voluntary one (block, yield, exit). The
 * difference matters for a thread that has marked itself BLOCKED in
 * waitqueue_prepare but has not yet evaluated its condition: a
 * preemption in that window must keep it runnable, otherwise it is
 * switched out on no queue and no wait list and is lost. When it runs
 * again its wait loop sees state RUNNING, yields once, re-prepares, and
 * re-checks the condition, so no wakeup is missed.
 */
static void schedule_internal(bool preempt)
{
    KASSERT(g_initialized);
    {
        /* Identity reads: both counts are zero on any CPU a caller that
         * may switch is running on, and non-zero only where it cannot move. */
        struct percpu *chk = raw_this_cpu();
        if (chk->irq_depth != 0)
            panic("schedule() called from interrupt context (depth %u)", chk->irq_depth);
        if (chk->preempt_count != 0)
            panic("schedule() called with preemption disabled (count %d), a spinlock is held",
                  chk->preempt_count);
    }

    /* Interrupts off before the per-CPU block is read: a caller arrives
     * here preemptible, and a tick between reading `pc` and taking its
     * run-queue lock could move this thread to another CPU, which would
     * then switch on the state of the CPU it left (S25; the corruption
     * that removed the first balancer, docs/audit/next-subsystem-percpu-
     * migration.md). The lock below is the same irqsave lock, split. */
    arch_irq_state_t s = arch_irq_save();
    struct percpu *pc = this_cpu();

    /* Quiescent: preempt_count is 0 here (asserted above), so no read-side
     * section is open on this CPU (docs/kernel/quiesce/design.md). */
    quiesce_note_quiescent();

    struct runqueue *rq = pc->rq;
    spin_lock(&rq->lock);

    struct thread *prev = rq->current;
    uint64_t now = clock_now_ns();
    /* `last_start_ns` was stamped by whichever CPU last ran this thread,
     * which need not be this one. */
    prev->run_time_ns += clock_delta_ns(now, prev->last_start_ns);

    /*
     * A thread switched out by preemption stopped at a point it did not
     * choose: it may hold the pointer to this CPU's block and be about
     * to read or write a field of it -- `preempt_disable` itself is the
     * pointer, then the count. Such a thread must resume here, so it is
     * marked and no migrator moves it until it has run again (S26). A
     * thread that yields or blocks stopped at a call of its own, with no
     * such access in flight, and may move.
     */
    if (preempt && prev != rq->idle)
        prev->flags |= THREAD_FLAG_PREEMPTED;

    if (prev->state == THREAD_EXITED) {
        KASSERT(!preempt);
        rq->prev_exited = prev;
    } else if (prev->state == THREAD_READY) {
        /* Woken between blocking and reaching here: already queued. */
    } else if (preempt || prev->state == THREAD_RUNNING) {
        prev->state = THREAD_READY;
        if (prev != rq->idle)
            g_policy->enqueue(rq, prev, prev->slice_left_ns > 0);
    } else {
        /* THREAD_BLOCKED, voluntary: the wait queue owns it now. */
    }

    struct thread *next = g_policy->pick_next(rq);
    if (next != NULL)
        g_policy->dequeue(rq, next);
    else
        next = rq->idle;

    pc->need_resched = false;

    if (next == prev) {
        prev->state = THREAD_RUNNING;
        prev->flags &= ~THREAD_FLAG_PREEMPTED;
        prev->last_start_ns = now;
        spin_unlock(&rq->lock);
        arch_irq_restore(s);
        return;
    }

    next->state = THREAD_RUNNING;
    next->flags &= ~THREAD_FLAG_PREEMPTED;   /* it runs again: whatever it had in flight completes here */
    next->cpu = (int)rq->cpu;
    next->last_start_ns = now;
    next->switches++;
    if (next->slice_left_ns == 0)
        g_policy->slice_new(next);
    rq->current = next;
    pc->current = next;
    rq->switches++;

    arch_thread_switch_prepare(prev, next);
    arch_context_switch(&prev->ctx, &next->ctx);

    /* Resumed as `prev`, holding the run-queue lock taken by whoever
     * switched to us. */
    sched_finish_switch();
    arch_irq_restore(s);
}

void schedule(void)
{
    schedule_internal(false);
}

void sched_yield(void)
{
    thread_current()->slice_left_ns = 0;
    schedule_internal(false);
}

void sched_preempt(void)
{
    struct percpu *pc = raw_this_cpu();   /* identity: the asserted counts */
    KASSERT(pc->irq_depth == 0 && pc->preempt_count == 0);
    schedule_internal(true);
}

void sched_block_current(void)
{
    struct percpu *pc = raw_this_cpu();   /* identity: the asserted counts */
    if (pc->irq_depth != 0)
        panic("blocking in interrupt context");
    if (pc->preempt_count != 0)
        panic("blocking with preemption disabled (count %d)", pc->preempt_count);
    schedule();
}

bool sched_wake(struct thread *t)
{
    KASSERT(t->cpu >= 0);
    struct runqueue *rq = &g_rqs[t->cpu];
    bool woke = false;

    arch_irq_state_t s = spin_lock_irqsave(&rq->lock);
    if (t->state == THREAD_BLOCKED) {
        t->state = THREAD_READY;
        g_policy->enqueue(rq, t, false);
        if (rq->current == rq->idle || t->priority < rq->current->priority)
            request_resched(rq);
        woke = true;
    }
    spin_unlock_irqrestore(&rq->lock, s);
    return woke;
}

void sched_set_running_current(void)
{
    /* Interrupts off before the block is read, as in schedule_internal:
     * the queue this thread may be on is the queue of the CPU it is on
     * *now*, and a move between the read and the lock would dequeue the
     * wrong CPU's current thread from the wrong queue (S25). */
    arch_irq_state_t s = arch_irq_save();
    struct percpu *pc = this_cpu();
    struct runqueue *rq = pc->rq;
    struct thread *cur = pc->current;

    spin_lock(&rq->lock);
    if (cur->state == THREAD_READY) {
        /* Woken before we blocked: take ourselves off the queue. */
        g_policy->dequeue(rq, cur);
    }
    cur->state = THREAD_RUNNING;
    spin_unlock(&rq->lock);
    arch_irq_restore(s);
}

/* --- migration --- */

const char *sched_migrate_result_name(enum sched_migrate_result r)
{
    switch (r) {
    case SCHED_MIGRATED: return "migrated";
    case SCHED_MIGRATE_SAME_CPU: return "same-cpu";
    case SCHED_MIGRATE_NOT_READY: return "not-ready";
    case SCHED_MIGRATE_CURRENT: return "current";
    case SCHED_MIGRATE_PREEMPTED: return "preempted";
    case SCHED_MIGRATE_AFFINITY: return "affinity";
    case SCHED_MIGRATE_OFFLINE: return "offline";
    case SCHED_MIGRATE_GAP: return "gap-closed";
    case SCHED_MIGRATE_RESULT_COUNT: break;   /* not a result: the array size */
    }
    return "?";
}

/* Both run-queue locks, increasing CPU id (S24). Interrupts are off. */
static void rq_lock_pair(unsigned a, unsigned b)
{
    unsigned lo = a < b ? a : b, hi = a < b ? b : a;
    spin_lock(&g_rqs[lo].lock);
    spin_lock(&g_rqs[hi].lock);
}

static void rq_unlock_pair(unsigned a, unsigned b)
{
    unsigned lo = a < b ? a : b, hi = a < b ? b : a;
    spin_unlock(&g_rqs[hi].lock);
    spin_unlock(&g_rqs[lo].lock);
}

/* Both locks held, `t` READY on `from`'s queue and not its current. */
static void migrate_locked(struct thread *t, unsigned from, unsigned to)
{
    struct runqueue *rqf = &g_rqs[from], *rqt = &g_rqs[to];
    KASSERT(t->state == THREAD_READY && t != rqf->current && t->cpu == (int)from &&
            (t->flags & THREAD_FLAG_PREEMPTED) == 0);
    g_policy->dequeue(rqf, t);
    t->cpu = (int)to;
    g_policy->enqueue(rqt, t, false);
    if (rqt->current == NULL || rqt->current == rqt->idle || t->priority < rqt->current->priority)
        request_resched(rqt);
    __atomic_fetch_add(&g_migrations, 1u, __ATOMIC_RELAXED);
}

static void assert_no_rq_lock_held(void)
{
#if CONFIG_LOCKDEP
    for (unsigned c = 0; c < cpu_count(); c++)
        KASSERT(!lockdep_is_held(&g_rqs[c].lock, LOCKDEP_KIND_SPIN));
#endif
}

enum sched_migrate_result sched_migrate(struct thread *t, unsigned to)
{
    KASSERT(g_initialized);
    assert_no_rq_lock_held();
    if (to >= cpu_count() || !cpu_online(to))
        return SCHED_MIGRATE_OFFLINE;
    for (;;) {
        arch_irq_state_t s = arch_irq_save();
        int from = __atomic_load_n(&t->cpu, __ATOMIC_ACQUIRE);
        KASSERT(from >= 0);
        if ((unsigned)from == to) {
            arch_irq_restore(s);
            return SCHED_MIGRATE_SAME_CPU;
        }
        rq_lock_pair((unsigned)from, to);
        if (t->cpu != from) {
            /* Moved by someone else between the read and the locks:
             * the queue it is on now is not the one locked. Again. */
            rq_unlock_pair((unsigned)from, to);
            arch_irq_restore(s);
            continue;
        }
        enum sched_migrate_result r;
        if (t->state != THREAD_READY)
            r = SCHED_MIGRATE_NOT_READY;
        else if (g_rqs[from].current == t)
            r = SCHED_MIGRATE_CURRENT;
        else if (t->flags & THREAD_FLAG_PREEMPTED)
            r = SCHED_MIGRATE_PREEMPTED;
        else if ((t->affinity & CPUMASK_OF(to)) == 0)
            r = SCHED_MIGRATE_AFFINITY;
        else if (!cpu_online(to))
            r = SCHED_MIGRATE_OFFLINE;
        else {
            migrate_locked(t, (unsigned)from, to);
            r = SCHED_MIGRATED;
        }
        rq_unlock_pair((unsigned)from, to);
        arch_irq_restore(s);
        return r;
    }
}

/* Both queues' locks are held, so these two reads are exact rather than
 * the hint `sched_cpu_load` gives an unlocked caller. */
static unsigned load_locked(const struct runqueue *rq)
{
    return rq->nr_running + (rq->current != NULL && rq->current != rq->idle ? 1u : 0u);
}

enum sched_migrate_result sched_migrate_from(unsigned from, unsigned to, unsigned min_gap, struct thread **moved)
{
    KASSERT(g_initialized);
    assert_no_rq_lock_held();
    *moved = NULL;
    if (from == to)
        return SCHED_MIGRATE_SAME_CPU;
    if (from >= cpu_count() || to >= cpu_count() || !cpu_online(from) || !cpu_online(to))
        return SCHED_MIGRATE_OFFLINE;
    arch_irq_state_t s = arch_irq_save();
    rq_lock_pair(from, to);
    enum sched_migrate_result r = SCHED_MIGRATE_NOT_READY;
    /* The caller scanned with an unlocked hint; a wake on the
     * destination since then can have closed the difference the move was
     * for, and moving anyway is the thrash the threshold exists to
     * prevent. Re-ask here, where both numbers are exact. */
    if (min_gap != 0 && load_locked(&g_rqs[from]) < load_locked(&g_rqs[to]) + min_gap) {
        rq_unlock_pair(from, to);
        arch_irq_restore(s);
        return SCHED_MIGRATE_GAP;
    }
    struct thread *t = g_policy->pick_migratable(&g_rqs[from], CPUMASK_OF(to));
    if (t != NULL) {
        migrate_locked(t, from, to);
        *moved = t;
        r = SCHED_MIGRATED;
    }
    rq_unlock_pair(from, to);
    arch_irq_restore(s);
    return r;
}

uint64_t sched_migration_count(void)
{
    return __atomic_load_n(&g_migrations, __ATOMIC_RELAXED);
}

#if CONFIG_SCHED_BALANCE
/*
 * The balancer: a CPU takes work, it never gives it away.
 *
 * **A pull, not a push.** The CPU that decides is the CPU that receives,
 * because it is the one with time to spend deciding and the one whose
 * queue the thread lands on. A push would have the busiest CPU -- the
 * one with least to spare -- scanning, and writing into a queue whose
 * owner is running.
 *
 * **Two moments.** An idle CPU looks every tick: it has nothing to lose,
 * the scan is a handful of loads, and this is the moment the defect this
 * unit was written for is visible (four runnable threads on two CPUs
 * while two sit idle for half a second;
 * docs/audit/next-subsystem-load-balancer.md). A CPU that is running
 * something looks every SCHED_BALANCE_TICKS, so an imbalance between two
 * busy CPUs is still corrected when no CPU is free.
 *
 * **A difference of two.** One is the steady state of an odd thread
 * count, and chasing it moves a thread back and forth forever. Two is
 * also the smallest difference that means a thread is *waiting*: a CPU
 * running one thread with an empty queue is at 1, so its thread is never
 * dragged to an idle CPU to arrive cold and do the work it was already
 * doing. Moving one thread shrinks the difference by two, which is why a
 * single pull per look settles rather than oscillates.
 *
 * **The scan is a hint** (`sched_cpu_load`): it reads other queues
 * without their locks. Everything it concludes is re-decided under both
 * locks by `sched_migrate_from`, which selects the thread itself through
 * the policy -- so a stale reading costs a wasted scan and can cost
 * nothing else.
 */
static uint64_t g_bal_scans, g_bal_pulls, g_bal_none;
static uint64_t g_bal_refused[SCHED_MIGRATE_RESULT_COUNT];

static void balance_tick(struct percpu *pc)
{
    unsigned n = cpu_count();
    if (n < 2)
        return;
    struct runqueue *rq = pc->rq;
    unsigned self = pc->cpu_id;

    /* This CPU's own fields, read in its own tick with interrupts off:
     * another CPU can still enqueue here, and a reading that is one
     * wake-up stale only decides whether to look. */
    unsigned mine = sched_cpu_load(self);
    bool idle_here = mine == 0 && rq->current == rq->idle;
    if (!idle_here && (pc->ticks % SCHED_BALANCE_TICKS) != 0)
        return;

    __atomic_fetch_add(&g_bal_scans, 1u, __ATOMIC_RELAXED);

    /*
     * The busiest CPU may have nothing it can give: its only spare
     * thread may be preempted (S26 forbids moving it), or pinned
     * elsewhere. Giving up then would leave this CPU idle while a CPU
     * of equal load two places along has a thread it could hand over,
     * and an idle CPU that keeps choosing the same unusable source is
     * idle for as long as that source stays busiest.
     *
     * So try the busiest few, in order, stopping at the first that gives
     * a thread. `BALANCE_TRIES` bounds the work: the scan is O(cpus) and
     * this repeats it at most three times, in a tick.
     */
    enum { BALANCE_TRIES = 3 };
    cpumask_t tried = 0;
    for (unsigned attempt = 0; attempt < BALANCE_TRIES; attempt++) {
        unsigned busiest = self, busiest_load = mine;
        for (unsigned c = 0; c < n; c++) {
            if (c == self || !cpu_online(c) || (tried & CPUMASK_OF(c)))
                continue;
            unsigned load = sched_cpu_load(c);
            if (load > busiest_load) {
                busiest_load = load;
                busiest = c;
            }
        }
        if (busiest == self || busiest_load < mine + 2) {
            /* Nothing left that is far enough ahead. */
            if (attempt == 0)
                __atomic_fetch_add(&g_bal_none, 1u, __ATOMIC_RELAXED);
            return;
        }
        tried |= CPUMASK_OF(busiest);

        struct thread *moved = NULL;
        enum sched_migrate_result r = sched_migrate_from(busiest, self, 2, &moved);
        if (r == SCHED_MIGRATED) {
            __atomic_fetch_add(&g_bal_pulls, 1u, __ATOMIC_RELAXED);
            return;
        }
        if ((unsigned)r < SCHED_MIGRATE_RESULT_COUNT)
            __atomic_fetch_add(&g_bal_refused[r], 1u, __ATOMIC_RELAXED);
        if (r == SCHED_MIGRATE_GAP) {
            /*
             * The difference had gone under the locks, and that has two
             * causes with different answers: this CPU got busier, in
             * which case no source is worth trying, or *that* source got
             * lighter, in which case another may still be two ahead. The
             * result alone does not say which, so re-read this CPU's own
             * load and let it say.
             */
            unsigned now = sched_cpu_load(self);
            bool busier = now > mine;
            /* Adopt the fresher reading either way. If this CPU got
             * *lighter* -- another CPU pulled from it while this look was
             * in progress -- keeping the old higher number would hold the
             * next candidate to a threshold this CPU no longer has. */
            mine = now;
            if (busier)
                return;
        }
    }
}
#endif /* CONFIG_SCHED_BALANCE */

void sched_balance_stats(struct sched_balance_stats *out)
{
    memset(out, 0, sizeof(*out));
#if CONFIG_SCHED_BALANCE
    out->scans = __atomic_load_n(&g_bal_scans, __ATOMIC_RELAXED);
    out->pulls = __atomic_load_n(&g_bal_pulls, __ATOMIC_RELAXED);
    out->no_candidate = __atomic_load_n(&g_bal_none, __ATOMIC_RELAXED);
    for (unsigned i = 0; i < SCHED_MIGRATE_RESULT_COUNT; i++)
        out->refused[i] = __atomic_load_n(&g_bal_refused[i], __ATOMIC_RELAXED);
#endif
}

#if CONFIG_SCHED_CHAOS
/*
 * The chaos migrator (SCHED_CHAOS=1, debug builds): every fourth tick,
 * each CPU sends one thread its queue can spare to the next online CPU
 * in a rotation, for no reason but to move it. The whole suite under
 * migration, from the context the balancer of the next unit will use.
 */
static uint64_t g_chaos_migrated, g_chaos_refused;
static unsigned g_chaos_rotor[CONFIG_MAX_CPUS];

static void chaos_tick(struct percpu *pc)
{
    if ((pc->ticks & 3u) != 0)
        return;
    unsigned n = cpu_count();
    if (n < 2)
        return;
    unsigned self = pc->cpu_id;
    unsigned to = self;
    for (unsigned i = 0; i < n; i++) {
        unsigned c = (self + 1 + g_chaos_rotor[self]++) % n;
        if (c != self && cpu_online(c)) {
            to = c;
            break;
        }
    }
    if (to == self)
        return;
    struct thread *moved;
    if (sched_migrate_from(self, to, 0, &moved) == SCHED_MIGRATED)   /* chaos asks for no gap: it moves for no reason */
        __atomic_fetch_add(&g_chaos_migrated, 1u, __ATOMIC_RELAXED);
    else
        __atomic_fetch_add(&g_chaos_refused, 1u, __ATOMIC_RELAXED);
}

void sched_chaos_stats(uint64_t *migrated, uint64_t *refused)
{
    *migrated = __atomic_load_n(&g_chaos_migrated, __ATOMIC_RELAXED);
    *refused = __atomic_load_n(&g_chaos_refused, __ATOMIC_RELAXED);
}
#endif

/* --- hang watchdog --- */

static uint64_t g_watchdog_timeout;
static uint64_t g_watchdog_last_kick;
static volatile bool g_watchdog_fired;

void sched_watchdog_arm(uint64_t timeout_ns)
{
    g_watchdog_last_kick = clock_now_ns();
    g_watchdog_fired = false;
    __atomic_store_n(&g_watchdog_timeout, timeout_ns, __ATOMIC_RELEASE);
}

void sched_watchdog_kick(void)
{
    g_watchdog_last_kick = clock_now_ns();
}

void sched_watchdog_disarm(void)
{
    __atomic_store_n(&g_watchdog_timeout, 0, __ATOMIC_RELEASE);
}

static void watchdog_check(uint64_t now, struct arch_trap_frame *frame)
{
    uint64_t timeout = __atomic_load_n(&g_watchdog_timeout, __ATOMIC_ACQUIRE);
    uint64_t last = __atomic_load_n(&g_watchdog_last_kick, __ATOMIC_RELAXED);
    /* A kick from another CPU between this tick's timestamp and the check
     * puts `last` ahead of `now`: progress, not a hang (the difference would
     * otherwise wrap to a huge count and fire the report). That used to be
     * a hand-rolled `now <= last ||` here; it is the saturating subtraction
     * now, which is the same test written once for the whole tree. */
    if (timeout == 0 || g_watchdog_fired || clock_delta_ns(now, last) < timeout)
        return;
    g_watchdog_fired = true;
    kprintf("\n[WATCHDOG] no progress for %llu ms; scheduler state:\n",
            (unsigned long long)(clock_delta_ns(now, last) / 1000000));
    sched_dump();
    /* Every other CPU's frame, recorded by that CPU (kernel/core/lockup.c);
     * this CPU's from the tick's own frame. */
    cpumask_t answered;
    if (lockup_sample_all(frame, LOCKUP_SAMPLE_TIMEOUT_NS, &answered))
        lockup_print_samples(answered);
    else
        kprintf("  sample in progress on cpu %d\n", lockup_reporter());
    /* A CPU that is running something and is not this one: eight more
     * samples, so a loop that is cycling rather than stuck shows its
     * shape. */
    for (unsigned c = 0; c < cpu_count(); c++) {
        struct runqueue *rq = &g_rqs[c];
        if (c != arch_cpu_id() && cpu_online(c) && rq->current != NULL && rq->current != rq->idle)
            lockup_profile(c, 8, 250 * 1000);
    }
}

void sched_tick(uint64_t now_ns, struct arch_trap_frame *frame)
{
    struct percpu *pc = this_cpu();
    struct runqueue *rq = pc->rq;
    if (rq == NULL)
        return;
    if (pc->cpu_id == 0)
        watchdog_check(now_ns, frame);
    lockup_tick(frame, now_ns);

    spin_lock(&rq->lock);
    struct thread *cur = rq->current;
    if (cur != NULL && cur != rq->idle)
        g_policy->tick(rq, cur, TICK_NS);
    else if (cur == rq->idle && rq->bitmap != 0)
        pc->need_resched = true;
    spin_unlock(&rq->lock);

#if CONFIG_SCHED_BALANCE
    balance_tick(pc);   /* after the tick's own unlock: the pull takes both locks itself */
#endif
#if CONFIG_SCHED_CHAOS
    chaos_tick(pc);   /* after the tick's own unlock: the migrator takes both locks itself */
#endif
#if CONFIG_DEBUG
    /* The stall detector: a thread READY on this queue for over a second
     * is a scheduling stall, and the line names what ran instead. Every
     * 32 ticks, this CPU's own lists, under its own lock. */
    if ((pc->ticks & 31u) == 0) {
        /* Gathered under the lock, printed after it: the run-queue lock is
         * a leaf (S2), and a print takes the console lock, which the
         * thread this tick interrupted may hold. */
        char stalled[THREAD_NAME_MAX] = "", runner[THREAD_NAME_MAX] = "-";   /* copies: a name lives in a thread another CPU may free */
        int sprio = 0, rprio = 0;
        unsigned sflags = 0;
        uint64_t waited = 0, slice = 0;
        spin_lock(&rq->lock);
        for (int p = 0; p < SCHED_PRIO_COUNT && stalled[0] == '\0'; p++) {
            struct thread *t;
            list_for_each_entry(t, &rq->ready[p], rq_link) {
                uint64_t w = clock_delta_ns(now_ns, t->ready_since_ns);
                if (w > NS_PER_SEC && t != rq->current) {
                    strlcpy(stalled, t->name, sizeof(stalled));
                    sprio = t->priority;
                    sflags = t->flags;
                    waited = w;
                    if (rq->current) {
                        strlcpy(runner, rq->current->name, sizeof(runner));
                        rprio = rq->current->priority;
                        slice = rq->current->slice_left_ns;
                    }
                    break;
                }
            }
        }
        spin_unlock(&rq->lock);
        if (stalled[0] != '\0')
            kwarn("sched: stall: '%s' (prio %d, flags 0x%x) READY on cpu %u for %llu ms behind '%s' (prio %d, preempt %d, irq_depth %u, slice %llu us)",
                  stalled, sprio, sflags, rq->cpu, (unsigned long long)(waited / 1000000), runner, rprio,
                  pc->preempt_count, pc->irq_depth, (unsigned long long)(slice / 1000));
    }
#endif
}

uint64_t sched_switch_count(unsigned cpu)
{
    return cpu < CONFIG_MAX_CPUS ? g_rqs[cpu].switches : 0;
}

#define SCHED_DUMP_HOOKS 8
static struct { const char *name; void (*fn)(void); } g_dump_hooks[SCHED_DUMP_HOOKS];
static unsigned g_dump_hook_count;

void sched_dump(void)
{
    /* One instant for the whole dump: every "ms ago" below is an age
     * against the same `now`, so the lines can be compared with each
     * other. Reading the clock per line would not let them be. */
    uint64_t now = clock_now_ns();
    for (unsigned c = 0; c < cpu_count(); c++) {
        struct runqueue *rq = &g_rqs[c];
        struct percpu *pc = percpu_get(c);
        /* The tick sample and its age (kernel/core/lockup.c): a CPU whose
         * last tick is seconds old is not taking interrupts, and its
         * other fields are as old as that. */
        kprintf("cpu %u: %s current '%s' queued %u load %u switches %llu restore-preempts %llu bitmap 0x%llx need_resched %d preempt %d irq_depth %u ticks %llu last tick %llu ms ago pc %p\n",
                c, pc && pc->online ? "online" : "offline", rq->current ? rq->current->name : "-",
                rq->nr_running, sched_cpu_load(c), (unsigned long long)rq->switches, (unsigned long long)preempt_point_count(c),
                (unsigned long long)rq->bitmap,
                pc ? pc->need_resched : 0, pc ? pc->preempt_count : 0, pc ? pc->irq_depth : 0,
                (unsigned long long)(pc ? pc->ticks : 0),
                (unsigned long long)(pc ? clock_delta_ns(now, pc->last_tick_ns) / 1000000 : 0),
                (void *)(pc ? pc->last_tick_pc : 0));
    }
    kprintf("migrations %llu\n", (unsigned long long)sched_migration_count());
    thread_dump_all();
    for (unsigned i = 0; i < g_dump_hook_count; i++) {
        kprintf("%s:\n", g_dump_hooks[i].name);
        g_dump_hooks[i].fn();
    }
}

void sched_dump_register(const char *name, void (*fn)(void))
{
    if (g_dump_hook_count >= SCHED_DUMP_HOOKS)
        panic("sched_dump_register: no slot for '%s'", name);
    g_dump_hooks[g_dump_hook_count].name = name;
    g_dump_hooks[g_dump_hook_count].fn = fn;
    __atomic_store_n(&g_dump_hook_count, g_dump_hook_count + 1, __ATOMIC_RELEASE);
}

/* Module ABI v1 exports (docs/kernel/module/api.md). */
#include <kernel/module.h>
EXPORT_SYMBOL(sched_yield);
EXPORT_SYMBOL(sched_block_current);   /* wait_event from a module (the xhci port worker) */
