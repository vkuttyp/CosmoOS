/*
 * timer.c - Clock, tick, and per-CPU timer queues.
 */

#include <kernel/errno.h>
#include <kernel/interrupt.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/string.h>
#include <kernel/timer.h>

#include <arch/cpu.h>
#include <arch/irq.h>
#include <arch/timer.h>

static struct timer_queue g_queues[CONFIG_MAX_CPUS];
static uint64_t g_clock_base;
static uint64_t g_clock_hz;
static uint64_t g_ns_mult;   /* (1e9 << CLOCK_SHIFT) / hz */
static timer_tick_hook_fn g_tick_hook;
static bool g_initialized;

#define CLOCK_SHIFT 32

/* --- clock --- */

uint64_t clock_now_ns(void)
{
    if (!g_initialized)
        return 0;
    uint64_t delta = arch_clock_read() - g_clock_base;
    /* Fixed-point: ns = delta * (1e9 / hz). A 128-bit multiply and shift
     * compiles inline; a 128-bit divide would need a runtime library the
     * kernel does not link. Relative error is below 1e-9. */
    unsigned __int128 ns = (unsigned __int128)delta * g_ns_mult;
    return (uint64_t)(ns >> CLOCK_SHIFT);
}

uint64_t clock_hz(void)
{
    return g_clock_hz;
}

const char *clock_name(void)
{
    return arch_clock_name();
}

void ndelay(uint64_t ns)
{
    uint64_t end = clock_now_ns() + ns;
    while (clock_now_ns() < end)
        arch_cpu_relax();
}

void udelay(uint64_t us)
{
    ndelay(us * 1000);
}

/* --- timer queue --- */

static struct timer_queue *local_queue(void)
{
    return this_cpu()->timers;
}

void timer_setup(struct timer *t, timer_fn fn, void *arg)
{
    list_init(&t->link);
    t->expires_ns = 0;
    t->fn = fn;
    t->arg = arg;
    t->cpu = 0;
    t->state = TIMER_IDLE;
}

void timer_start(struct timer *t, uint64_t delay_ns)
{
    KASSERT(g_initialized);
    KASSERT(t->fn != NULL);

    arch_irq_state_t s = arch_irq_save();
    struct timer_queue *q = local_queue();
    spin_lock(&q->lock);

    /* IDLE is the normal case. RUNNING means the callback is executing
     * and is re-arming its own timer, which is allowed: run_expired
     * leaves a timer alone after the callback when it is no longer
     * RUNNING. PENDING is a double start and a bug. */
    if (t->state == TIMER_PENDING)
        panic("timer_start: timer %p is already pending", (void *)t);

    /* A zero delay would expire at "now", which can equal the time
     * run_expired captured for the current pass; a callback re-arming
     * with 0 would then be popped again inside the same pass, forever.
     * One nanosecond puts every re-arm into a later pass. */
    t->expires_ns = clock_now_ns() + (delay_ns == 0 ? 1 : delay_ns);
    t->cpu = arch_cpu_id();
    t->state = TIMER_PENDING;

    /* Sorted insert, ascending expiry; ties keep FIFO order. */
    struct timer *it;
    struct list_node *pos = &q->pending;
    list_for_each_entry(it, &q->pending, link) {
        if (it->expires_ns > t->expires_ns) {
            pos = &it->link;
            break;
        }
    }
    list_insert_before(pos, &t->link);
    q->count++;

    spin_unlock(&q->lock);
    arch_irq_restore(s);
}

static bool cancel_locked(struct timer_queue *q, struct timer *t)
{
    bool was_pending = t->state == TIMER_PENDING;
    if (was_pending) {
        list_remove(&t->link);
        q->count--;
        t->state = TIMER_IDLE;
    }
    return was_pending;
}

bool timer_cancel(struct timer *t)
{
    if (t->cpu >= CONFIG_MAX_CPUS)
        return false;
    struct timer_queue *q = &g_queues[t->cpu];

    arch_irq_state_t s = spin_lock_irqsave(&q->lock);
    bool was_pending = cancel_locked(q, t);
    spin_unlock_irqrestore(&q->lock, s);
    return was_pending;
}

void quiesce_count_timer_wait(void);   /* quiesce.c statistics */

bool timer_cancel_sync(struct timer *t)
{
    if (t->cpu >= CONFIG_MAX_CPUS)
        return false;
    struct timer_queue *q = &g_queues[t->cpu];
    bool was_pending = false;
    bool waited = false;

    /* The callback runs under q->running with the queue lock dropped and
     * takes the lock again when it returns. Holding the lock while
     * q->running != t therefore means the callback is not executing;
     * seeing q->running == t means it is, on another CPU (on this CPU it
     * would have to be interrupt context, which cannot be pre-empted by
     * us: interrupts are masked while we hold the lock). A callback may
     * re-arm itself, so cancel again after every wait. */
    for (;;) {
        arch_irq_state_t s = spin_lock_irqsave(&q->lock);
        was_pending |= cancel_locked(q, t);
        if (q->running != t) {
            spin_unlock_irqrestore(&q->lock, s);
            break;
        }
        if (t->cpu == arch_cpu_id())
            panic("timer_cancel_sync: timer %p cancelled from its own callback", (void *)t);
        spin_unlock_irqrestore(&q->lock, s);
        waited = true;
        arch_cpu_relax();
    }
    if (waited)
        quiesce_count_timer_wait();
    return was_pending;
}

static void run_expired(struct timer_queue *q, uint64_t now)
{
    spin_lock(&q->lock);
    while (!list_empty(&q->pending)) {
        struct timer *t = list_first_entry(&q->pending, struct timer, link);
        if (t->expires_ns > now)
            break;
        list_remove(&t->link);
        q->count--;
        t->state = TIMER_RUNNING;
        q->running = t;
        spin_unlock(&q->lock);

        t->fn(t, t->arg);

        spin_lock(&q->lock);
        q->running = NULL;
        if (t->state == TIMER_RUNNING)
            t->state = TIMER_IDLE; /* unless the callback re-armed it */
    }
    spin_unlock(&q->lock);
}

/* --- tick --- */

static void tick_isr(unsigned vector, struct arch_trap_frame *frame, void *arg)
{
    (void)vector;
    (void)frame;
    (void)arg;

    struct percpu *pc = this_cpu();
    pc->ticks++;

    uint64_t now = clock_now_ns();
    run_expired(pc->timers, now);
    if (g_tick_hook)
        g_tick_hook(now);
}

void timer_set_tick_hook(timer_tick_hook_fn hook)
{
    g_tick_hook = hook;
}

void timer_init_cpu(void)
{
    struct percpu *pc = this_cpu();
    struct timer_queue *q = &g_queues[pc->cpu_id];
    spinlock_init(&q->lock, "timer_queue");
    list_init(&q->pending);
    q->count = 0;
    q->running = NULL;
    pc->timers = q;
    arch_timer_start_tick(CONFIG_HZ);
}

void timer_init(void)
{
    KASSERT(!g_initialized);

    arch_timer_calibrate();
    g_clock_hz = arch_clock_hz();
    KASSERT(g_clock_hz > 0);
    g_ns_mult = ((uint64_t)NS_PER_SEC << CLOCK_SHIFT) / g_clock_hz;
    g_clock_base = arch_clock_read();
    g_initialized = true;

    int rc = interrupt_register(arch_timer_vector(), tick_isr, NULL, "timer-tick");
    if (rc)
        panic("timer: cannot register tick handler (%d)", rc);

    timer_init_cpu();

    kinfo("timer: %s at %llu.%03llu MHz, tick %u Hz", arch_clock_name(),
          (unsigned long long)(g_clock_hz / 1000000), (unsigned long long)((g_clock_hz / 1000) % 1000),
          CONFIG_HZ);
}

uint64_t timer_ticks(void)
{
    return this_cpu()->ticks;
}

unsigned timer_pending_count(void)
{
    struct timer_queue *q = local_queue();
    arch_irq_state_t s = spin_lock_irqsave(&q->lock);
    unsigned n = q->count;
    spin_unlock_irqrestore(&q->lock, s);
    return n;
}

/* Module ABI v1 exports (docs/kernel/module/api.md). */
#include <kernel/module.h>
EXPORT_SYMBOL(clock_now_ns);
EXPORT_SYMBOL(timer_setup);
EXPORT_SYMBOL(timer_start);
EXPORT_SYMBOL(timer_cancel);
EXPORT_SYMBOL(timer_cancel_sync);
EXPORT_SYMBOL(ndelay);
EXPORT_SYMBOL(udelay);
