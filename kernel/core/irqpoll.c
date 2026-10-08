/*
 * irqpoll.c - Bounded completion handling for interrupt handlers: a
 * budget per call, the remainder deferred to a per-CPU worker
 * (kernel/include/kernel/irqpoll.h; docs/kernel/interrupt/design.md,
 * "Bounded completion handling").
 *
 * Locks: an irq_poll's own lock, then a worker's list lock or the
 * irq_poll's idle queue, never the other way round. Neither is held across
 * `poll`. The idle queue is woken under the irq_poll's lock: a waiter
 * reads its condition under that lock too, so it cannot see the poll idle,
 * return and let its caller free the irq_poll while a waker is still
 * inside the queue (review, PR #335).
 */

#include <kernel/irqpoll.h>
#include <kernel/lockdep.h>
#include <kernel/log.h>
#include <kernel/module.h>
#include <kernel/percpu.h>
#include <kernel/printf.h>
#include <kernel/sched.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/wait.h>

struct irq_poll_worker {
    spinlock_t lock;
    struct list_node list;    /* irq_polls handed to this CPU, oldest first */
    struct waitqueue wq;
    struct thread *thread;
    bool ready;               /* published with release once the thread exists */
    bool lowered;             /* the worker's own: at SCHED_PRIO_DEFAULT, until `boost` or idle */
    bool boosted;             /* set by `boost` when it raised the worker again */
    uint64_t busy_since;      /* the worker's own: when its current hold began (0 = idle) */
    struct timer boost;       /* raises a lowered worker after IRQ_POLL_HOLD_NS */
    char name[16];
};

static struct irq_poll_worker g_workers[CONFIG_MAX_CPUS];

void irq_poll_init(struct irq_poll *ip, irq_poll_fn poll, const char *name)
{
    ip->poll = poll;
    ip->name = name;
    ip->class_fn = (const void *)poll;
    ip->lockdep_class = 0;
    spinlock_init(&ip->lock, "irq-poll");
    waitqueue_init(&ip->idle_wq, "irq-poll-idle");
    ip->waiters = 0;
    ip->running = ip->scheduled = ip->queued = ip->again = ip->disabled = false;
    ip->cpu = 0;
    list_init(&ip->link);
    ip->runs = ip->calls = ip->deferred = 0;
    ip->max_one = 0;
}

void irq_poll_set_class(struct irq_poll *ip, const void *fn)
{
    ip->class_fn = fn;
    ip->lockdep_class = 0;
}

static void note_one(struct irq_poll *ip, unsigned n)
{
    uint32_t max = __atomic_load_n(&ip->max_one, __ATOMIC_RELAXED);
    while (n > max && !__atomic_compare_exchange_n(&ip->max_one, &max, n, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        ;
}

/* One call of `poll`, inside its callback class (lockdep design.md,
 * "Callback classes"): every lock it takes is recorded as taken under the
 * class, wherever it runs, so a teardown's wait is checked against it.
 * Exit names only the function, as a timer's does. */
static unsigned run_poll(struct irq_poll *ip)
{
    const void *fn = ip->class_fn;
    lockdep_callback_enter(fn, &ip->lockdep_class);
    unsigned n = ip->poll(ip, IRQ_POLL_BUDGET);
    lockdep_callback_exit(fn);
    note_one(ip, n);
    return n;
}

/* A worker that can take a deferral: this CPU's, else any started one. */
static struct irq_poll_worker *worker_for(unsigned cpu)
{
    if (cpu < CONFIG_MAX_CPUS && __atomic_load_n(&g_workers[cpu].ready, __ATOMIC_ACQUIRE))
        return &g_workers[cpu];
    for (unsigned c = 0; c < CONFIG_MAX_CPUS; c++)
        if (__atomic_load_n(&g_workers[c].ready, __ATOMIC_ACQUIRE))
            return &g_workers[c];
    return NULL;
}

/* ip->lock held, `w` started. */
static void queue_locked(struct irq_poll *ip, struct irq_poll_worker *w)
{
    ip->scheduled = true;
    ip->cpu = (unsigned)(w - g_workers);
    ip->deferred++;
    spin_lock(&w->lock);
    ip->queued = true;
    list_push_back(&w->list, &ip->link);
    spin_unlock(&w->lock);
}

/* After a `poll` that consumed `n`: idle, or queued for more. Returns the
 * worker to wake, or NULL. ip->lock held. */
static struct irq_poll_worker *finish_locked(struct irq_poll *ip, unsigned n, unsigned cpu)
{
    ip->running = false;
    ip->runs++;
    bool more = n >= IRQ_POLL_BUDGET || ip->again;
    ip->again = false;
    if (!more || ip->disabled)
        return NULL;
    struct irq_poll_worker *w = worker_for(cpu);
    if (w == NULL)
        return NULL;   /* unreachable once workers start; the caller polls on */
    queue_locked(ip, w);
    return w;
}

void irq_poll_sched(struct irq_poll *ip)
{
    arch_irq_state_t s = spin_lock_irqsave(&ip->lock);
    if (ip->disabled) {
        spin_unlock_irqrestore(&ip->lock, s);
        return;
    }
    if (ip->running || ip->scheduled) {
        ip->again = true;   /* the one consuming now takes it before going idle */
        spin_unlock_irqrestore(&ip->lock, s);
        return;
    }
    ip->running = true;
    ip->calls++;
    spin_unlock_irqrestore(&ip->lock, s);
    unsigned cpu = raw_cpu_id();   /* interrupts are off: this is the CPU the worker should be */
    for (;;) {
        unsigned n = run_poll(ip);
        s = spin_lock_irqsave(&ip->lock);
        bool early = n >= IRQ_POLL_BUDGET && worker_for(cpu) == NULL && !ip->disabled;
        struct irq_poll_worker *w = early ? NULL : finish_locked(ip, n, cpu);
        if (!early && ip->waiters != 0)
            waitqueue_wake_all(&ip->idle_wq);
        spin_unlock_irqrestore(&ip->lock, s);
        if (w != NULL)
            waitqueue_wake_one(&w->wq);
        if (!early)
            return;
        /* Boot, before the workers: nowhere to defer to, so on, as the
         * handlers did before the budget. */
    }
}

/* The wait's lockdep half, then might_sleep: a lock that the poll takes
 * held across the wait is the deadlock itself and is reported as one
 * (LOCKDEP_R_CALLBACK, naming the lock and the class), which the plain
 * sleep check behind it would only call a sleep in atomic context. False
 * after a report: the caller must not wait. */
static bool may_wait(struct irq_poll *ip, uintptr_t ip_caller)
{
    if (!lockdep_callback_wait(ip->class_fn, &ip->lockdep_class, ip_caller))
        return false;
    might_sleep();
    return true;
}

static bool idle_now(struct irq_poll *ip)
{
    arch_irq_state_t s = spin_lock_irqsave(&ip->lock);
    bool idle = !ip->running && !ip->scheduled;
    spin_unlock_irqrestore(&ip->lock, s);
    return idle;
}

static bool run_done(struct irq_poll *ip, uint64_t gen)
{
    arch_irq_state_t s = spin_lock_irqsave(&ip->lock);
    bool done = !ip->running || ip->runs != gen;
    spin_unlock_irqrestore(&ip->lock, s);
    return done;
}

/* `waiters` is raised under the lock before the condition is first read
 * and every finish reads it under the same lock, so a finish after the
 * raise wakes, and one before it is seen by the condition. */
static void waiters_add(struct irq_poll *ip, int d)
{
    arch_irq_state_t s = spin_lock_irqsave(&ip->lock);
    ip->waiters += (unsigned)d;
    spin_unlock_irqrestore(&ip->lock, s);
}

void irq_poll_disable(struct irq_poll *ip)
{
    /* Whether or not a poll runs now, this call may wait for one: checked
     * against every poll of the function observed. After a report the
     * irq_poll is still disabled and unqueued; only the wait is skipped. */
    bool wait = may_wait(ip, (uintptr_t)__builtin_return_address(0));
    arch_irq_state_t s = spin_lock_irqsave(&ip->lock);
    ip->disabled = true;
    if (ip->scheduled) {
        struct irq_poll_worker *w = &g_workers[ip->cpu];
        spin_lock(&w->lock);
        if (ip->queued) {
            list_remove(&ip->link);
            list_init(&ip->link);
            ip->queued = false;
            ip->scheduled = false;   /* else the worker has popped it and clears this itself */
        }
        spin_unlock(&w->lock);
    }
    bool busy = ip->running || ip->scheduled;
    if (busy && wait)
        ip->waiters++;
    spin_unlock_irqrestore(&ip->lock, s);
    if (!busy || !wait)
        return;
    wait_event(&ip->idle_wq, idle_now(ip));
    waiters_add(ip, -1);
}

void irq_poll_synchronize(struct irq_poll *ip)
{
    if (!may_wait(ip, (uintptr_t)__builtin_return_address(0)))
        return;
    arch_irq_state_t s = spin_lock_irqsave(&ip->lock);
    bool busy = ip->running;
    uint64_t gen = ip->runs;
    if (busy)
        ip->waiters++;
    spin_unlock_irqrestore(&ip->lock, s);
    if (!busy)
        return;
    wait_event(&ip->idle_wq, run_done(ip, gen));
    waiters_add(ip, -1);
}

void irq_poll_enable(struct irq_poll *ip)
{
    arch_irq_state_t s = spin_lock_irqsave(&ip->lock);
    ip->disabled = false;
    ip->again = false;
    spin_unlock_irqrestore(&ip->lock, s);
}

/* A lowered worker is raised again after one hold: it cannot rely on
 * running to raise itself, since a busier thread above the default
 * priority may be what keeps it off the CPU (review, PR #335). */
static void boost_fn(struct timer *t, void *arg)
{
    (void)t;
    struct irq_poll_worker *w = arg;
    __atomic_store_n(&w->boosted, true, __ATOMIC_RELEASE);
    sched_reprioritize(w->thread, SCHED_PRIO_HIGHEST);
}

static void worker_main(void *arg)
{
    struct irq_poll_worker *w = arg;
    unsigned cpu = (unsigned)(w - g_workers);
    struct thread *self = thread_current();
    for (;;) {
        /* Idle again: the next deferral starts at the highest priority. */
        arch_irq_state_t ls = spin_lock_irqsave(&w->lock);
        bool idle = list_empty(&w->list);
        spin_unlock_irqrestore(&w->lock, ls);
        if (idle) {
            if (w->lowered) {
                (void)timer_cancel_sync(&w->boost);
                sched_reprioritize(self, SCHED_PRIO_HIGHEST);
                w->lowered = false;
            }
            __atomic_store_n(&w->boosted, false, __ATOMIC_RELAXED);
            w->busy_since = 0;
        }
        wait_event(&w->wq, !list_empty(&w->list));
        if (w->busy_since == 0)
            w->busy_since = clock_now_ns();
        arch_irq_state_t s = spin_lock_irqsave(&w->lock);
        struct list_node *node = list_empty(&w->list) ? NULL : list_pop_front(&w->list);
        struct irq_poll *ip = NULL;
        if (node != NULL) {
            ip = container_of(node, struct irq_poll, link);
            list_init(&ip->link);
            ip->queued = false;
        }
        spin_unlock_irqrestore(&w->lock, s);
        if (ip == NULL)
            continue;
        s = spin_lock_irqsave(&ip->lock);
        ip->scheduled = false;
        if (ip->disabled) {
            if (ip->waiters != 0)
                waitqueue_wake_all(&ip->idle_wq);   /* irq_poll_disable is waiting for exactly this */
            spin_unlock_irqrestore(&ip->lock, s);
            continue;
        }
        ip->running = true;
        ip->again = false;
        spin_unlock_irqrestore(&ip->lock, s);
        /* Preemption off across the call: `poll` never sleeps (it runs in
         * interrupt context too), and a sleep inside it is then reported
         * here as it would be in the handler. Interrupts stay on; the
         * call's callback class is this thread's (lockdep_acquired), so an
         * interrupt taken meanwhile does not see it. */
        preempt_disable();
        unsigned n = run_poll(ip);
        preempt_enable();
        s = spin_lock_irqsave(&ip->lock);
        (void)finish_locked(ip, n, cpu);   /* back on this worker's own list if there is more */
        if (ip->waiters != 0)
            waitqueue_wake_all(&ip->idle_wq);
        spin_unlock_irqrestore(&ip->lock, s);
        /* A batch at a time, with the tick, interrupts and anything else at
         * this priority between two. At the highest priority for
         * IRQ_POLL_HOLD_NS of one backlog, then at the default until the
         * backlog ends: a device that never runs dry would otherwise keep
         * this CPU's threads off it for as long as it lasts -- among them,
         * in virtio-remove-inflight, the test thread that was to stop the
         * submitter refilling it (a livelock: 36 s on two CPUs, the boot's
         * whole budget under chaos). Past the hold it time-slices with
         * the CPU's other threads, as ksoftirqd does in Linux. Lowered,
         * it is raised again by `boost` after another IRQ_POLL_HOLD_NS, so
         * no thread at any priority holds the remainder off for longer than
         * that either: half of a long backlog at the highest priority, half
         * shared. */
        if (__atomic_exchange_n(&w->boosted, false, __ATOMIC_ACQ_REL)) {
            w->lowered = false;
            w->busy_since = clock_now_ns();
        }
        if (!w->lowered && clock_now_ns() - w->busy_since > IRQ_POLL_HOLD_NS) {
            sched_reprioritize(self, SCHED_PRIO_DEFAULT);
            w->lowered = true;
            timer_start(&w->boost, IRQ_POLL_HOLD_NS);
        }
        sched_yield();
    }
}

void irq_poll_start_workers(void)
{
    for (unsigned c = 0; c < cpu_count() && c < CONFIG_MAX_CPUS; c++) {
        if (!cpu_online(c))
            continue;
        struct irq_poll_worker *w = &g_workers[c];
        spinlock_init(&w->lock, "irq-poll-cpu");
        list_init(&w->list);
        waitqueue_init(&w->wq, "irqpoll");
        timer_setup(&w->boost, boost_fn, w);
        ksnprintf(w->name, sizeof(w->name), "irqpoll/%u", c);
        /* The highest priority: what the worker runs is what the handler
         * ran before the budget, above every thread. At the default it
         * waited behind any busier thread on its CPU -- quiesce, the
         * reaper, a pinned self-test spinner -- and the queue's completions
         * with it, for as long as that thread ran (review, PR #334). */
        w->thread = thread_create_on(worker_main, w, w->name, SCHED_PRIO_HIGHEST, CPUMASK_OF(c));
        if (w->thread == NULL) {
            kwarn("irqpoll: no worker for cpu %u; its deferrals go to another CPU's", c);
            continue;
        }
        __atomic_store_n(&w->ready, true, __ATOMIC_RELEASE);
    }
}

EXPORT_SYMBOL(irq_poll_init);
EXPORT_SYMBOL(irq_poll_set_class);
EXPORT_SYMBOL(irq_poll_sched);
EXPORT_SYMBOL(irq_poll_disable);
EXPORT_SYMBOL(irq_poll_enable);
EXPORT_SYMBOL(irq_poll_synchronize);
