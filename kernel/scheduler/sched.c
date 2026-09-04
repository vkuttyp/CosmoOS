/*
 * sched.c - Scheduler mechanism: run queues, the switch point, wake and
 * block, preemption, idle.
 *
 * Every state transition of a thread happens under the run-queue lock of
 * the CPU the thread belongs to. schedule() holds that lock across the
 * context switch; whoever runs next releases it in sched_finish_switch().
 */

#include <kernel/ipi.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
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
    __atomic_store_n(&pc->online, true, __ATOMIC_RELEASE);
    arch_thread_switch_prepare(rq->idle);
    arch_context_switch(&dead, &rq->idle->ctx);
    panic("sched: AP bootstrap context resumed");
}

/* --- placement --- */

static unsigned pick_cpu(const struct thread *t)
{
    unsigned best = this_cpu()->cpu_id;
    unsigned best_load = ~0u;
    for (unsigned c = 0; c < cpu_count(); c++) {
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

    struct runqueue *rq = pc->rq;
    arch_irq_state_t s = spin_lock_irqsave(&rq->lock);

    struct thread *prev = rq->current;
    uint64_t now = clock_now_ns();
    prev->run_time_ns += now - prev->last_start_ns;

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

    arch_thread_switch_prepare(next);
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

static void watchdog_check(uint64_t now)
{
    uint64_t timeout = __atomic_load_n(&g_watchdog_timeout, __ATOMIC_ACQUIRE);
    if (timeout == 0 || g_watchdog_fired || now - g_watchdog_last_kick < timeout)
        return;
    g_watchdog_fired = true;
    kprintf("\n[WATCHDOG] no progress for %llu ms; scheduler state:\n",
            (unsigned long long)((now - g_watchdog_last_kick) / 1000000));
    sched_dump();
}

void sched_tick(uint64_t now_ns)
{
    struct percpu *pc = this_cpu();
    struct runqueue *rq = pc->rq;
    if (rq == NULL)
        return;
    if (pc->cpu_id == 0)
        watchdog_check(now_ns);

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

void sched_dump(void)
{
    for (unsigned c = 0; c < cpu_count(); c++) {
        struct runqueue *rq = &g_rqs[c];
        struct percpu *pc = percpu_get(c);
        kprintf("cpu %u: %s current '%s' queued %u switches %llu bitmap 0x%llx need_resched %d preempt %d irq_depth %u ticks %llu\n",
                c, pc && pc->online ? "online" : "offline", rq->current ? rq->current->name : "-",
                rq->nr_running, (unsigned long long)rq->switches, (unsigned long long)rq->bitmap,
                pc ? pc->need_resched : 0, pc ? pc->preempt_count : 0, pc ? pc->irq_depth : 0,
                (unsigned long long)(pc ? pc->ticks : 0));
    }
    thread_dump_all();
}

/* Module ABI v1 exports (docs/kernel/module/api.md). */
#include <kernel/module.h>
EXPORT_SYMBOL(sched_yield);
