/*
 * quiesce.c - Grace periods over the online CPUs and deferred reclamation
 * (docs/kernel/quiesce/design.md).
 *
 * The algorithm is quiesce_core.h; this file supplies the CPU registry,
 * the sleeping wait with straggler kicks, the callback worker and the
 * debug counters.
 */

#include <kernel/ipi.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/quiesce.h>
#include <kernel/quiesce_core.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/wait.h>

#include <arch/cpu.h>

STATIC_ASSERT(QUIESCE_MAX_CPUS >= CONFIG_MAX_CPUS, "quiesce state covers every CPU");

static struct quiesce_state g_state;
static struct quiesce_stats g_stats;
static bool g_ready;

/* Deferred callbacks: one list, one worker. */
static spinlock_t g_cb_lock = SPINLOCK_INIT("quiesce-cb");
static struct quiesce_head *g_cb_head;
static unsigned g_cb_pending;
static struct waitqueue g_worker_wq = WAITQUEUE_INIT(g_worker_wq);

/* --- quiescent points ------------------------------------------------------ */

void quiesce_note_quiescent(void)
{
    quiesce_core_publish(&g_state, arch_cpu_id());
}

void quiesce_read_lock_debug(void)
{
#if CONFIG_DEBUG
    g_state.cpus[arch_cpu_id()].depth++;
#endif
}

void quiesce_read_unlock_debug(void)
{
#if CONFIG_DEBUG
    struct quiesce_cpu *c = &g_state.cpus[arch_cpu_id()];
    if (c->depth == 0)
        panic("quiesce_read_unlock without a matching lock on CPU %u", arch_cpu_id());
    c->depth--;
#endif
}

/* --- grace periods ------------------------------------------------------- */

void synchronize_quiesce(void)
{
    struct percpu *pc = this_cpu();
    if (pc->irq_depth != 0)
        panic("synchronize_quiesce in interrupt context");
    if (pc->preempt_count != 0)
        panic("synchronize_quiesce with preemption disabled (count %d): a spinlock is held", pc->preempt_count);

    cpumask_t online = cpu_online_mask();
    uint64_t target = quiesce_core_begin(&g_state);
    quiesce_note_quiescent();   /* this CPU: thread context, no read section open */

    if (!g_ready) {
        /* Before the scheduler can sleep, every other CPU is still in
         * its bootstrap: nothing can hold a reference. */
        return;
    }

    uint64_t start = clock_now_ns();
    unsigned kicks = 0;
    bool warned = false;
    for (;;) {
        cpumask_t pending = (cpumask_t)quiesce_core_pending(&g_state, target, online);
        /* A CPU that went offline holds nothing any more. */
        pending &= cpu_online_mask();
        if (pending == 0)
            break;

        uint64_t waited = clock_now_ns() - start;
        if (waited > 2 * TICK_NS && kicks < 8) {
            /* A straggler is in a preempt-disabled region across its
             * ticks, or its tick keeps landing inside one: an extra
             * interrupt gives its return path another chance. */
            for (unsigned c = 0; c < cpu_count(); c++) {
                if ((pending & CPUMASK_OF(c)) && c != pc->cpu_id && cpu_online(c))
                    ipi_send(c, IPI_RESCHEDULE);
            }
            kicks++;
            g_stats.straggler_ipis++;
        }
        if (waited > NS_PER_SEC && !warned) {
            kwarn("quiesce: grace period %llu waiting %llu ms for CPU mask 0x%llx", (unsigned long long)target,
                  (unsigned long long)(waited / 1000000), (unsigned long long)pending);
            warned = true;
        }
#if CONFIG_DEBUG
        if (waited > 10 * NS_PER_SEC)
            panic("quiesce: CPU mask 0x%llx has not reached a quiescent state in 10 s", (unsigned long long)pending);
#endif
        thread_sleep_ns(TICK_NS / 2);
    }

    uint64_t waited = clock_now_ns() - start;
    g_stats.synchronizes++;
    if (waited > g_stats.max_wait_ns)
        g_stats.max_wait_ns = waited;
}

/* --- deferred callbacks --------------------------------------------------- */

void call_quiesce(struct quiesce_head *h, void (*fn)(struct quiesce_head *h))
{
    KASSERT(fn != NULL);
    h->fn = fn;
    arch_irq_state_t s = spin_lock_irqsave(&g_cb_lock);
    h->next = g_cb_head;
    g_cb_head = h;
    g_cb_pending++;
    spin_unlock_irqrestore(&g_cb_lock, s);
    waitqueue_wake_one(&g_worker_wq);
}

static bool callbacks_pending(void)
{
    return __atomic_load_n(&g_cb_pending, __ATOMIC_ACQUIRE) != 0;
}

static void worker_main(void *arg)
{
    (void)arg;
    for (;;) {
        wait_event(&g_worker_wq, callbacks_pending());

        arch_irq_state_t s = spin_lock_irqsave(&g_cb_lock);
        struct quiesce_head *batch = g_cb_head;
        g_cb_head = NULL;
        g_cb_pending = 0;
        spin_unlock_irqrestore(&g_cb_lock, s);

        /* One grace period for the whole batch: every head was queued
         * (after its unlink) before this call began. */
        synchronize_quiesce();

        /* The list is LIFO; run in submission order. */
        struct quiesce_head *ordered = NULL;
        while (batch) {
            struct quiesce_head *n = batch->next;
            batch->next = ordered;
            ordered = batch;
            batch = n;
        }
        while (ordered) {
            struct quiesce_head *n = ordered->next;
            ordered->fn(ordered);
            g_stats.callbacks++;
            ordered = n;
        }
    }
}

void quiesce_init(void)
{
    KASSERT(!g_ready);
    struct thread *w = thread_create(worker_main, NULL, "quiesce", SCHED_PRIO_DEFAULT - 4);
    if (w == NULL)
        panic("quiesce: cannot create the worker thread");
    thread_put(w);   /* detached */
    g_ready = true;
    kinfo("quiesce: epoch-based reclamation ready (%u CPU slots)", (unsigned)CONFIG_MAX_CPUS);
}

/* --- diagnostics --------------------------------------------------------- */

void quiesce_get_stats(struct quiesce_stats *out)
{
    *out = g_stats;
    out->epoch = __atomic_load_n(&g_state.epoch, __ATOMIC_ACQUIRE);
}

uint32_t quiesce_cpu_depth(unsigned cpu)
{
    return cpu < CONFIG_MAX_CPUS ? g_state.cpus[cpu].depth : 0;
}

uint64_t quiesce_cpu_transitions(unsigned cpu)
{
    return cpu < CONFIG_MAX_CPUS ? g_state.cpus[cpu].transitions : 0;
}

void quiesce_count_timer_wait(void)
{
    __atomic_fetch_add(&g_stats.timer_sync_waits, 1u, __ATOMIC_RELAXED);
}

void quiesce_count_irq_sync(void);
void quiesce_count_irq_sync(void)
{
    g_stats.irq_syncs++;
}

/* Module ABI exports (docs/kernel/module/api.md). */
#include <kernel/module.h>
EXPORT_SYMBOL(synchronize_quiesce);
EXPORT_SYMBOL(call_quiesce);
EXPORT_SYMBOL(quiesce_read_lock_debug);
EXPORT_SYMBOL(quiesce_read_unlock_debug);
