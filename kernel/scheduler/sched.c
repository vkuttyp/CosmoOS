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

static void rq_init(struct runqueue *rq, unsigned cpu)
{
    memset(rq, 0, sizeof(*rq));
    spinlock_init(&rq->lock, "runqueue");
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

static unsigned pick_cpu(const struct thread *t)
{
    unsigned n = cpu_count();
    unsigned start = n ? __atomic_fetch_add(&g_pick_rotor, 1u, __ATOMIC_RELAXED) % n : 0;
    unsigned best = this_cpu()->cpu_id;
    unsigned best_load = ~0u;
    for (unsigned i = 0; i < n; i++) {
        unsigned c = (start + i) % n;
        if (!(t->affinity & CPUMASK_OF(c)) || !cpu_online(c))
            continue;
        unsigned load = g_rqs[c].nr_running;
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
    struct percpu *pc = this_cpu();
    KASSERT(g_initialized);
    if (pc->irq_depth != 0)
        panic("schedule() called from interrupt context (depth %u)", pc->irq_depth);
    if (pc->preempt_count != 0)
        panic("schedule() called with preemption disabled (count %d), a spinlock is held",
              pc->preempt_count);

    /* Quiescent: preempt_count is 0 here (asserted above), so no read-side
     * section is open on this CPU (docs/kernel/quiesce/design.md). */
    quiesce_note_quiescent();

    struct runqueue *rq = pc->rq;
    arch_irq_state_t s = spin_lock_irqsave(&rq->lock);

    struct thread *prev = rq->current;
    uint64_t now = clock_now_ns();
    /* `last_start_ns` was stamped by whichever CPU last ran this thread,
     * which need not be this one. */
    prev->run_time_ns += clock_delta_ns(now, prev->last_start_ns);

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
        prev->last_start_ns = now;
        spin_unlock_irqrestore(&rq->lock, s);
        return;
    }

    next->state = THREAD_RUNNING;
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
    struct percpu *pc = this_cpu();
    KASSERT(pc->irq_depth == 0 && pc->preempt_count == 0);
    schedule_internal(true);
}

void sched_block_current(void)
{
    struct percpu *pc = this_cpu();
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
    struct percpu *pc = this_cpu();
    struct runqueue *rq = pc->rq;
    struct thread *cur = pc->current;

    arch_irq_state_t s = spin_lock_irqsave(&rq->lock);
    if (cur->state == THREAD_READY) {
        /* Woken before we blocked: take ourselves off the queue. */
        g_policy->dequeue(rq, cur);
    }
    cur->state = THREAD_RUNNING;
    spin_unlock_irqrestore(&rq->lock, s);
}

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
        kprintf("cpu %u: %s current '%s' queued %u switches %llu restore-preempts %llu bitmap 0x%llx need_resched %d preempt %d irq_depth %u ticks %llu last tick %llu ms ago pc %p\n",
                c, pc && pc->online ? "online" : "offline", rq->current ? rq->current->name : "-",
                rq->nr_running, (unsigned long long)rq->switches, (unsigned long long)preempt_point_count(c),
                (unsigned long long)rq->bitmap,
                pc ? pc->need_resched : 0, pc ? pc->preempt_count : 0, pc ? pc->irq_depth : 0,
                (unsigned long long)(pc ? pc->ticks : 0),
                (unsigned long long)(pc ? clock_delta_ns(now, pc->last_tick_ns) / 1000000 : 0),
                (void *)(pc ? pc->last_tick_pc : 0));
    }
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
