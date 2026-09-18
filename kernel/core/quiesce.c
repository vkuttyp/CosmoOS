/*
 * quiesce.c - Grace periods over the online CPUs and deferred reclamation
 * (docs/kernel/quiesce/design.md).
 *
 * The algorithm is quiesce_core.h; this file supplies the CPU registry,
 * the sleeping wait with straggler kicks, the callback worker and the
 * debug counters.
 */

#include <kernel/ipi.h>
#include <kernel/lockdep.h>
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
/*
 * Publishes that happened in a straggler kick's own trap return, per
 * CPU that published.
 *
 * Per-CPU and not per-waiter, because a per-waiter figure is not
 * available: `quiesce_test_sync_kicks` can be returned on the stack
 * because the SENDER increments it, but a publish is incremented on the
 * TARGET, asynchronously, and the IPI carries no payload naming the
 * waiter -- so two waiters kicking one pending CPU are satisfied by a
 * single publish and neither can claim it. A test reads the counter of
 * the CPU it pinned its adversary to, and the claim is carried by the
 * pair: that counter rising, and the spinner's not
 * (docs/audit/next-subsystem-straggler-kick.md).
 */
static uint64_t g_kick_publishes[CONFIG_MAX_CPUS];

/* Deferred callbacks: one list, one worker. */
static spinlock_t g_cb_lock = SPINLOCK_INIT("quiesce-cb");
static struct quiesce_head *g_cb_head;
static unsigned g_cb_pending;
static struct waitqueue g_worker_wq = WAITQUEUE_INIT(g_worker_wq);
/* Waiters for a grace period. Woken by a quiescent point taken in a
 * context that holds nothing -- the trap returns and the idle loop, not
 * the scheduler's own publishes (invariant Q-W). */
static struct waitqueue g_gp_wq = WAITQUEUE_INIT(g_gp_wq);

/* --- quiescent points ------------------------------------------------------ */

void quiesce_note_quiescent(void)
{
    (void)quiesce_core_publish(&g_state, arch_cpu_id());
    /* No wake here: see quiesce_note_quiescent_preemptible. This is
     * called from inside the scheduler, including the AP bring-up path
     * that holds a run-queue lock with interrupts off (sched.c), and a
     * wake from there re-enters the scheduler. */
}

/*
 * The same publish, plus the wake -- for callers that hold nothing.
 *
 * The wake cannot live in quiesce_note_quiescent, and that is the one
 * thing the report got wrong about this design. That function is called
 * from inside the scheduler: sched.c's AP bring-up publishes while
 * holding a run-queue lock with interrupts disabled, and waking from
 * there reaches schedule_internal, which asserts it is not called with a
 * spinlock held. The machine dies five seconds into boot.
 *
 * Both trap returns and the idle loop call this one instead. The trap
 * returns do so from the block guarded by `irq_depth == 0 &&
 * preempt_count == 0 && interrupts were enabled`, which goes on to call
 * sched_preempt() two lines later; the idle loop calls schedule() two
 * lines later. Either can certainly do the lesser thing of waking a
 * queue.
 *
 * g_ready as well: before quiesce_init there is no scheduler to wake
 * into, and nothing can be queued either, because sync_quiesce_counting
 * returns without queueing while !g_ready. So skipping the wake then
 * cannot lose one.
 */
/*
 * This CPU published inside the trap return of a straggler kick.
 *
 * Called from the architecture trap tails, which read and clear the
 * kick flag unconditionally before testing whether they may publish --
 * so this runs only for a publish in the kick's own return, never for a
 * later one (invariant Q19).
 */
void quiesce_note_kick_published(void)
{
    __atomic_fetch_add(&g_kick_publishes[arch_cpu_id()], 1u, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_stats.kick_publishes, 1u, __ATOMIC_RELAXED);
}

uint64_t quiesce_kick_publishes(unsigned cpu)
{
    if (cpu >= CONFIG_MAX_CPUS)
        return 0;
    return __atomic_load_n(&g_kick_publishes[cpu], __ATOMIC_RELAXED);
}

bool quiesce_note_quiescent_preemptible(void)
{
    /* Published here rather than through quiesce_note_quiescent so the
     * caller learns whether this publish ADVANCED this CPU's epoch. A
     * trap tail attributes a straggler kick only to a publish that did:
     * a redundant one is correct and cheap but tells no waiter anything,
     * and counting it would say the kick worked when it did not (Q19). */
    bool advanced = quiesce_core_publish(&g_state, arch_cpu_id());
    if (g_ready && !waitqueue_empty(&g_gp_wq)) {
        /* Atomic: this runs from every CPU's trap return and idle loop at
         * once, so a plain += loses increments -- and the test asserts on
         * this counter, so a lost one is a lost assertion, not just a
         * wrong number. */
        __atomic_fetch_add(&g_stats.gp_wakes, waitqueue_wake_all(&g_gp_wq), __ATOMIC_RELAXED);
    }
    return advanced;
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

/* The wait itself, handing back how many straggler kicks *this* call
 * sent and, through `timeouts`, how many of its blocks ended at their
 * deadline rather than by being woken. Nothing stores either: see
 * quiesce_test_sync_kicks below.
 *
 * Per call, not from the global counter, and the difference matters: a
 * test that sampled quiesce_stats.gp_timeouts before and after would be
 * reading every CPU's grace periods, not its own. The first version of
 * quiesce-wake did exactly that and failed on other threads' work. */
static unsigned sync_quiesce_counting(unsigned *timeouts)
{
    struct percpu *pc = this_cpu();
    if (pc->irq_depth != 0)
        panic("synchronize_quiesce in interrupt context");
    might_sleep();   /* a spinlock or a read-side section is held: a report with the stacks */

    cpumask_t online = cpu_online_mask();
    uint64_t target = quiesce_core_begin(&g_state);
    quiesce_note_quiescent();   /* this CPU: thread context, no read section open */

    if (!g_ready) {
        /* Before the scheduler can sleep, every other CPU is still in
         * its bootstrap: nothing can hold a reference. */
        return 0;
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

        uint64_t waited = clock_since_ns(start);
        if (waited > 2 * TICK_NS && kicks < 8) {
            /*
             * An extra interrupt gives this CPU's return path another
             * chance to publish.
             *
             * Which stragglers that can help is narrower than it looks,
             * and this comment used to claim the wrong one. A CPU
             * publishes at interrupt return only when it is outside
             * every read-side section (`preempt_count == 0`, in the trap
             * return), so a CPU *spinning* with preemption disabled
             * takes this IPI, handles it, and returns without
             * publishing: the kick cannot help it, and the grace period
             * ends when that CPU leaves its section, kick or no kick
             * (`docs/audit/2026-09-lifetime-quiesce-report.md`, risk 2:
             * "the straggler IPI helps a halted CPU, not one spinning
             * with preemption off").
             *
             * What it can help is a CPU whose *periodic tick* keeps
             * landing inside a short disabled region: an interrupt at an
             * unrelated phase lands outside one and publishes. That
             * population is real and no test in this tree arranges it,
             * That population is real and no test in this tree arranges
             * it, so what this kick is worth is no longer an open
             * question in a comment: `kick_publishes` counts the
             * publishes that happened in a kick's own trap return, per
             * CPU, and `quiesce-kick-population` and
             * `quiesce-kick-spinner` are the pair that gives the number
             * meaning (`docs/audit/next-subsystem-straggler-kick.md`,
             * invariant Q19).
             */
            unsigned sent = 0;
            for (unsigned c = 0; c < cpu_count(); c++) {
                if ((pending & CPUMASK_OF(c)) && c != pc->cpu_id && cpu_online(c)) {
                    ipi_send(c, IPI_QUIESCE_KICK);
                    sent++;
                }
            }
            if (sent != 0) {
                kicks++;   /* rounds, which is what the eight-round bound counts */
                /* IPIs, which is what "kicks sent" has to mean if it is
                 * ever a denominator: a round can kick several CPUs.
                 * Atomic because concurrent waiters both reach here and a
                 * plain ++ loses their updates. */
                __atomic_fetch_add(&g_stats.straggler_ipis, sent, __ATOMIC_RELAXED);
            }
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
        if (!wait_event_timeout(&g_gp_wq, quiesce_core_pending(&g_state, target, online) == 0,
                                TICK_NS / 2)) {
            /* Atomic for the same reason as gp_wakes: waiters are
             * concurrent, so two grace periods can reach a deadline at
             * once. */
            __atomic_fetch_add(&g_stats.gp_timeouts, 1u, __ATOMIC_RELAXED);
            if (timeouts)
                (*timeouts)++;
        }
    }

    uint64_t waited = clock_since_ns(start);
    g_stats.synchronizes++;
    if (waited > g_stats.max_wait_ns)
        g_stats.max_wait_ns = waited;
    return kicks;
}

void synchronize_quiesce(void)
{
    (void)sync_quiesce_counting(NULL);
}

#if CONFIG_DEBUG
/*
 * The same wait, handing back the kicks *this call* sent.
 *
 * Returned rather than stored anywhere, and that is the point. The
 * machine-wide `straggler_ipis` cannot carry a per-waiter claim because
 * another waiter's kicks land in it; a per-CPU slot cannot either,
 * because the callback worker is unpinned and can run a grace period on
 * this CPU between the tested call returning and a preemptible test
 * reading the slot. A value on the caller's stack belongs to the caller
 * (docs/audit/next-subsystem-lifetime-windows.md).
 */
unsigned quiesce_test_sync_kicks(void)
{
    return sync_quiesce_counting(NULL);
}

/* One grace period, reporting how many of ITS blocks reached a deadline.
 * Zero is the claim invariant Q-W makes. */
unsigned quiesce_test_sync_timeouts(void)
{
    unsigned timeouts = 0;
    (void)sync_quiesce_counting(&timeouts);
    return timeouts;
}
#endif

/* --- deferred callbacks --------------------------------------------------- */

void call_quiesce(struct quiesce_head *h, void (*fn)(struct quiesce_head *h))
{
    KASSERT(fn != NULL);
    arch_irq_state_t s = spin_lock_irqsave(&g_cb_lock);
    if (h->pending)
        panic("call_quiesce: head %p submitted twice before its callback ran", (void *)h);
    h->pending = true;
    h->fn = fn;
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
            ordered->next = NULL;
            ordered->pending = false;   /* the callback may free or resubmit the head */
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
