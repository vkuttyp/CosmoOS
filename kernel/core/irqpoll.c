/*
 * irqpoll.c - Bounded completion handling for interrupt handlers: a
 * budget per call, the remainder deferred to a per-CPU worker
 * (kernel/include/kernel/irqpoll.h; docs/kernel/interrupt/design.md,
 * "Bounded completion handling").
 *
 * Locks: an irq_poll's own lock, then a worker's list lock, in that order
 * and never the other way round. Neither is held across `poll`.
 */

#include <kernel/irqpoll.h>
#include <kernel/log.h>
#include <kernel/module.h>
#include <kernel/percpu.h>
#include <kernel/printf.h>
#include <kernel/sched.h>
#include <kernel/thread.h>
#include <kernel/wait.h>

struct irq_poll_worker {
    spinlock_t lock;
    struct list_node list;    /* irq_polls handed to this CPU, oldest first */
    struct waitqueue wq;
    struct thread *thread;
    bool ready;               /* published with release once the thread exists */
    char name[16];
};

static struct irq_poll_worker g_workers[CONFIG_MAX_CPUS];

void irq_poll_init(struct irq_poll *ip, irq_poll_fn poll, const char *name)
{
    ip->poll = poll;
    ip->name = name;
    spinlock_init(&ip->lock, "irq-poll");
    ip->running = ip->scheduled = ip->queued = ip->again = ip->disabled = false;
    ip->cpu = 0;
    list_init(&ip->link);
    ip->runs = ip->calls = ip->deferred = 0;
    ip->max_one = 0;
}

static void note_one(struct irq_poll *ip, unsigned n)
{
    uint32_t max = __atomic_load_n(&ip->max_one, __ATOMIC_RELAXED);
    while (n > max && !__atomic_compare_exchange_n(&ip->max_one, &max, n, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        ;
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
        unsigned n = ip->poll(ip, IRQ_POLL_BUDGET);
        note_one(ip, n);
        s = spin_lock_irqsave(&ip->lock);
        bool early = n >= IRQ_POLL_BUDGET && worker_for(cpu) == NULL && !ip->disabled;
        struct irq_poll_worker *w = early ? NULL : finish_locked(ip, n, cpu);
        spin_unlock_irqrestore(&ip->lock, s);
        if (w != NULL)
            waitqueue_wake_one(&w->wq);
        if (!early)
            return;
        /* Boot, before the workers: nowhere to defer to, so on, as the
         * handlers did before the budget. */
    }
}

void irq_poll_disable(struct irq_poll *ip)
{
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
    spin_unlock_irqrestore(&ip->lock, s);
    for (;;) {
        s = spin_lock_irqsave(&ip->lock);
        bool busy = ip->running || ip->scheduled;
        spin_unlock_irqrestore(&ip->lock, s);
        if (!busy)
            return;
        sched_yield();
    }
}

void irq_poll_synchronize(struct irq_poll *ip)
{
    arch_irq_state_t s = spin_lock_irqsave(&ip->lock);
    bool busy = ip->running;
    uint64_t gen = ip->runs;
    spin_unlock_irqrestore(&ip->lock, s);
    while (busy) {
        sched_yield();
        s = spin_lock_irqsave(&ip->lock);
        busy = ip->running && ip->runs == gen;
        spin_unlock_irqrestore(&ip->lock, s);
    }
}

void irq_poll_enable(struct irq_poll *ip)
{
    arch_irq_state_t s = spin_lock_irqsave(&ip->lock);
    ip->disabled = false;
    ip->again = false;
    spin_unlock_irqrestore(&ip->lock, s);
}

static void worker_main(void *arg)
{
    struct irq_poll_worker *w = arg;
    unsigned cpu = (unsigned)(w - g_workers);
    for (;;) {
        wait_event(&w->wq, !list_empty(&w->list));
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
            spin_unlock_irqrestore(&ip->lock, s);
            continue;   /* irq_poll_disable is waiting for exactly this */
        }
        ip->running = true;
        ip->again = false;
        spin_unlock_irqrestore(&ip->lock, s);
        unsigned n = ip->poll(ip, IRQ_POLL_BUDGET);
        note_one(ip, n);
        s = spin_lock_irqsave(&ip->lock);
        (void)finish_locked(ip, n, cpu);   /* back on this worker's own list if there is more */
        spin_unlock_irqrestore(&ip->lock, s);
        /* A batch at a time: whatever else is runnable here goes between
         * two, so a device that never runs dry costs this CPU a share of
         * its time rather than all of it. */
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
        ksnprintf(w->name, sizeof(w->name), "irqpoll/%u", c);
        w->thread = thread_create_on(worker_main, w, w->name, SCHED_PRIO_DEFAULT, CPUMASK_OF(c));
        if (w->thread == NULL) {
            kwarn("irqpoll: no worker for cpu %u; its deferrals go to another CPU's", c);
            continue;
        }
        __atomic_store_n(&w->ready, true, __ATOMIC_RELEASE);
    }
}

EXPORT_SYMBOL(irq_poll_init);
EXPORT_SYMBOL(irq_poll_sched);
EXPORT_SYMBOL(irq_poll_disable);
EXPORT_SYMBOL(irq_poll_enable);
EXPORT_SYMBOL(irq_poll_synchronize);
