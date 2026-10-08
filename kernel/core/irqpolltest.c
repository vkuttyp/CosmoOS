/*
 * irqpolltest.c - irqpoll-boost: a worker lowered past its hold is raised
 * again (kernel/core/irqpoll.c; docs/kernel/interrupt/design.md, "Bounded
 * completion handling").
 *
 * A backlog keeps its irqpoll worker at the highest priority for
 * IRQ_POLL_HOLD_NS, then at the default. A thread above the default that
 * stays runnable on the worker's CPU would then hold the rest of the
 * backlog off for as long as it ran, unless something raises the worker
 * again: the worker cannot, since it is not running (review, PR #335).
 * The test pins a spinner above the default to one CPU for SPIN_MS,
 * schedules a poll there whose every call uses its whole budget for that
 * long, and holds the longest gap between two calls of the poll to a bound.
 * Without the raise, the gap is the spinner's whole run.
 */

#include <kernel/irqpoll.h>
#include <kernel/log.h>
#include <kernel/percpu.h>
#include <kernel/selftest.h>
#include <kernel/thread.h>
#include <kernel/timer.h>

#include <arch/cpu.h>
#include <arch/irq.h>

#define STR_(x) #x
#define STR(x)  STR_(x)
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            *reason = "check failed: " #cond " at line " STR(__LINE__);        \
            return false;                                                      \
        }                                                                      \
    } while (0)

#define BOOST_SPIN_MS 300u
/* LOAD-SENSITIVE (docs/testing/flakes.md, "The list"): a time bound. The
 * raise comes IRQ_POLL_HOLD_NS (10 ms) after the drop and the tick is 4 ms,
 * so a gap is about 15 ms; 100 ms is a host stall, and without the raise a
 * gap is the spinner's whole 300 ms. */
#define BOOST_GAP_MS  100u

static struct {
    struct irq_poll ip;
    volatile uint64_t until, last, max_gap;
    volatile unsigned calls, spinning, ended;
} g_bt;

static unsigned boost_poll(struct irq_poll *ip, unsigned budget)
{
    (void)ip;
    uint64_t now = clock_now_ns();
    if (g_bt.last != 0 && now - g_bt.last > g_bt.max_gap)
        g_bt.max_gap = now - g_bt.last;
    g_bt.last = now;
    __atomic_fetch_add(&g_bt.calls, 1u, __ATOMIC_RELAXED);
    if (now < g_bt.until)
        return budget;   /* a backlog for as long as the spinner runs */
    __atomic_store_n(&g_bt.ended, 1u, __ATOMIC_RELEASE);
    return 0;
}

static void boost_spinner(void *arg)
{
    (void)arg;
    __atomic_store_n(&g_bt.spinning, 1u, __ATOMIC_RELEASE);
    while (clock_now_ns() < g_bt.until)
        arch_cpu_relax();
}

static bool selftest_irqpoll_boost_pinned(const char **reason)
{
    unsigned cpu = raw_cpu_id();
    irq_poll_init(&g_bt.ip, boost_poll, "irqpoll-boost");
    g_bt.last = g_bt.max_gap = 0;
    g_bt.calls = g_bt.spinning = g_bt.ended = 0;
    g_bt.until = clock_now_ns() + (uint64_t)BOOST_SPIN_MS * 1000000ull;
    /* What a handler does, on this CPU: the first call uses its budget, so
     * the rest goes to this CPU's worker -- which preempts this thread at
     * once, keeps the CPU for its hold, then time-slices with it. */
    arch_irq_state_t s = arch_irq_save();
    irq_poll_sched(&g_bt.ip);
    arch_irq_restore(s);
    /* Then the spinner, above the default: neither this thread nor a
     * lowered worker preempts it. Created first, it ran its whole time
     * before the schedule, and the worker had no backlog to show. */
    struct thread *spinner = thread_create_on(boost_spinner, NULL, "boost-spin", SCHED_PRIO_DEFAULT - 4,
                                              CPUMASK_OF(cpu));
    if (spinner == NULL) {
        g_bt.until = 0;   /* the backlog ends at the next call */
        irq_poll_disable(&g_bt.ip);
        CHECK(spinner != NULL);
    }
    (void)thread_join(spinner);   /* this thread runs again once the spinner is done */
    /* The backlog's last call, the first after the spinner: a worker held
     * off for the spinner's whole run makes its gap there. Disabling
     * before it came would drop the call that measures the stall. */
    uint64_t end = clock_now_ns() + 2000000000ull;
    while (!__atomic_load_n(&g_bt.ended, __ATOMIC_ACQUIRE) && clock_now_ns() < end)
        thread_sleep_ms(1);
    bool ended = __atomic_load_n(&g_bt.ended, __ATOMIC_ACQUIRE) != 0;
    irq_poll_disable(&g_bt.ip);
    uint64_t gap_ms = g_bt.max_gap / 1000000ull;
    kinfo("selftest: irqpoll-boost: cpu %u, a spinner above the default for %u ms: %u calls of the poll, "
          "longest gap %llu ms (bound %u)",
          cpu, BOOST_SPIN_MS, g_bt.calls, (unsigned long long)gap_ms, BOOST_GAP_MS);
    CHECK(g_bt.spinning == 1);
    CHECK(ended);
    CHECK(g_bt.calls > 10);   /* not vacuous: the worker ran the backlog, beside the spinner */
    CHECK(gap_ms <= BOOST_GAP_MS);
    return true;
}

/* Pinned: the deferral goes to this CPU's worker, which the spinner shares. */
bool selftest_irqpoll_boost(const char **reason)
{
    cpumask_t saved = thread_pin_self();
    bool r = selftest_irqpoll_boost_pinned(reason);
    thread_set_affinity_self(saved);
    return r;
}
