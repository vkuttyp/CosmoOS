/*
 * smptest.c - Boot-time self-tests for multiprocessor operation.
 *
 * Run from thread 0 on CPU 0 after smp_init. When only one CPU came up
 * (QEMU -smp 1, or firmware without APs) the tests that need a second
 * CPU report "ok" after checking the single-CPU behaviour, so the suite
 * stays meaningful in both configurations.
 */

#include <kernel/completion.h>
#include <kernel/ipi.h>
#include <kernel/log.h>
#include <kernel/mutex.h>
#include <kernel/page.h>
#include <kernel/percpu.h>
#include <kernel/pmm.h>
#include <kernel/sched.h>
#include <kernel/selftest.h>
#include <kernel/semaphore.h>
#include <kernel/smp.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/vmm.h>
#include <kernel/wait.h>
#include <kernel/acpi.h>

#include <arch/cpu.h>
#include <arch/mmu.h>

#define STR_(x) #x
#define STR(x)  STR_(x)
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            *reason = "check failed: " #cond " at line " STR(__LINE__);        \
            return false;                                                      \
        }                                                                      \
    } while (0)

#define MS(n) ((uint64_t)(n) * 1000000ULL)

static unsigned online_count(void)
{
    return (unsigned)__builtin_popcountll(cpu_online_mask());
}

/* Exited threads are freed by the reaper after join returns; wait for
 * the count to settle. */
static bool threads_settle(unsigned expected)
{
    uint64_t deadline = clock_now_ns() + MS(200);
    while (thread_count() != expected) {
        if (clock_now_ns() > deadline)
            return false;
        sched_yield();
    }
    return true;
}

/* --- an IPI storm: cross calls while this CPU's tick sends wake IPIs ---
 *
 * Prompt #3, 3.4: the xAPIC ICR is written as two registers. A timer
 * callback on the calling CPU that wakes threads pinned elsewhere sends
 * IPI_RESCHEDULE from interrupt context, exactly what could land between
 * the two writes of a cross call in progress and redirect it. With the
 * ICR pair written under local interrupt masking every cross call below
 * reaches its CPU; without it the caller panics after one second. */

static struct waitqueue g_storm_wq = WAITQUEUE_INIT(g_storm_wq);
static volatile unsigned g_storm_gen;
static volatile bool g_storm_stop;
static struct timer g_storm_timer;

static void storm_timer(struct timer *t, void *arg)
{
    (void)arg;
    g_storm_gen++;
    waitqueue_wake_all(&g_storm_wq);   /* interrupt context: wake IPIs to the waiters' CPUs */
    if (!g_storm_stop)
        timer_start(t, 100000);        /* every tick, effectively */
}

static void storm_waiter(void *arg)
{
    (void)arg;
    unsigned seen = g_storm_gen;
    while (!g_storm_stop) {
        wait_event(&g_storm_wq, g_storm_gen != seen || g_storm_stop);
        seen = g_storm_gen;
    }
}

static void storm_call(void *arg)
{
    __atomic_fetch_add((unsigned *)arg, 1u, __ATOMIC_RELAXED);
}

static bool selftest_smp_ipi_storm_pinned(const char **reason)
{
    unsigned online = online_count();
    if (online < 2) {
        kinfo("selftest: smp-ipi-storm: one CPU, no cross-CPU traffic to race");
        return true;
    }
    unsigned threads0 = thread_count();
    struct thread *w[CONFIG_MAX_CPUS];
    unsigned nw = 0;
    g_storm_stop = false;
    g_storm_gen = 0;
    unsigned here = arch_cpu_id();   /* pinned by the wrapper: the waiters and the calls go to the others */
    for (unsigned c = 0; c < cpu_count(); c++) {
        if (c == here || !cpu_online(c))
            continue;
        w[nw] = thread_create_on(storm_waiter, NULL, "storm", SCHED_PRIO_DEFAULT, CPUMASK_OF(c));
        CHECK(w[nw] != NULL);
        nw++;
    }
    timer_setup(&g_storm_timer, storm_timer, NULL);
    timer_start(&g_storm_timer, 100000);

    unsigned calls = 0, rounds = 0;
    uint64_t end = clock_now_ns() + MS(300);
    while (clock_now_ns() < end) {
        for (unsigned c = 0; c < cpu_count(); c++) {
            if (c != here && cpu_online(c))
                smp_call_function_single(c, storm_call, &calls);
        }
        rounds++;
    }

    g_storm_stop = true;
    timer_cancel(&g_storm_timer);   /* the callback runs on this CPU: it either re-armed before the stop
                                       flag (cancelled here) or saw the flag and did not */
    waitqueue_wake_all(&g_storm_wq);
    for (unsigned i = 0; i < nw; i++)
        thread_join(w[i]);
    CHECK(calls == rounds * (online - 1));
    CHECK(g_storm_gen > 0);
    CHECK(threads_settle(threads0));
    kinfo("selftest: smp-ipi-storm: %u cross calls in %u rounds against %u wake ticks", calls, rounds,
          g_storm_gen);
    return true;
}

/* Pinned for the whole test: "another CPU than mine" is a claim about
 * this thread's CPU that must outlive its sleeps (S25); a holder or a
 * spinner parked on that other CPU must never find this thread queued
 * behind it. */
bool selftest_smp_ipi_storm(const char **reason)
{
    cpumask_t saved = thread_pin_self();
    bool r = selftest_smp_ipi_storm_pinned(reason);
    thread_set_affinity_self(saved);
    return r;
}


/* --- every reported CPU is online --- */

bool selftest_smp_online(const char **reason)
{
    const struct acpi_madt_cpu *cpus;
    size_t reported = acpi_madt_cpus(&cpus);
    unsigned online = online_count();

    CHECK(online >= 1);
    CHECK(cpu_online(0));
    CHECK(online == reported || reported > CONFIG_MAX_CPUS);
    for (unsigned c = 0; c < cpu_count(); c++) {
        struct percpu *pc = percpu_get(c);
        CHECK(pc != NULL);
        CHECK(pc->cpu_id == c);
        CHECK(pc->rq != NULL && pc->idle != NULL && pc->timers != NULL);
        CHECK(pc->boot_stack == 0 || c == 0); /* APs freed theirs in idle */
    }
    return true;
}

/* --- pinned threads run where they are pinned --- */

static void report_cpu(void *arg)
{
    unsigned *out = arg;
    *out = raw_cpu_id();   /* recorded: where it was placed, and where it woke */
    /* Stay a little so placement is not just a first instruction. */
    thread_sleep_ms(2);
    *out = *out * 100 + raw_cpu_id();
}

bool selftest_smp_affinity(const char **reason)
{
    unsigned before = thread_count();
    unsigned seen[CONFIG_MAX_CPUS];
    struct thread *t[CONFIG_MAX_CPUS];
    unsigned n = cpu_count();

    for (unsigned c = 0; c < n; c++) {
        seen[c] = 999;
        t[c] = thread_create_on(report_cpu, &seen[c], "pinned", SCHED_PRIO_DEFAULT, CPUMASK_OF(c));
        CHECK(t[c] != NULL);
        CHECK(t[c]->cpu == (int)c);
    }
    for (unsigned c = 0; c < n; c++) {
        thread_join(t[c]);
        CHECK(seen[c] == c * 100 + c);
    }
    CHECK(thread_create_on(report_cpu, &seen[0], "nowhere", SCHED_PRIO_DEFAULT, 0) == NULL);
    CHECK(threads_settle(before));
    return true;
}

/* --- CPUs make progress concurrently --- */

struct spin_work {
    uint64_t iterations;        /* one writer (the spinner); read by the observer while it runs, so atomic on both sides */
    volatile int stop;
    unsigned cpu;               /* where it is pinned */
    volatile unsigned strays;   /* iterations that ran anywhere else; read only after the join */
};

static void spin_worker(void *arg)
{
    struct spin_work *w = arg;
    uint64_t n = 0;
    while (!__atomic_load_n(&w->stop, __ATOMIC_ACQUIRE)) {
        /* A store, not a fetch-add: this is the counter's only writer,
         * and what the observer needs is a whole value that advances. */
        __atomic_store_n(&w->iterations, ++n, __ATOMIC_RELAXED);
        if (arch_cpu_id() != w->cpu)
            w->strays++;
        arch_cpu_relax();
    }
}

/*
 * The observer of parallelism, pinned to CPU 0: it holds CPU 0 (or is
 * preempted on it -- either way CPU 0 is busy) and watches the CPU 1
 * spinner's counter. The counter can only advance under its eyes if CPU
 * 1 is executing *at the same time*, which is the property. The deadline
 * is a hang guard, not a measurement; on a quiet host the advance is seen
 * in microseconds.
 */
struct spin_observer {
    struct spin_work *watched;
    volatile bool advanced;
};

static void spin_observe(void *arg)
{
    struct spin_observer *o = arg;
    uint64_t seen = __atomic_load_n(&o->watched->iterations, __ATOMIC_RELAXED);
    uint64_t deadline = clock_now_ns() + MS(1000);
    while (clock_now_ns() < deadline) {
        if (__atomic_load_n(&o->watched->iterations, __ATOMIC_RELAXED) > seen) {
            o->advanced = true;
            return;
        }
        arch_cpu_relax();
    }
}

bool selftest_smp_parallel(const char **reason)
{
    unsigned before = thread_count();
    unsigned n = cpu_count();
    struct spin_work work[CONFIG_MAX_CPUS];
    struct thread *t[CONFIG_MAX_CPUS];

    for (unsigned c = 0; c < n; c++) {
        work[c].iterations = 0;
        work[c].stop = 0;
        work[c].cpu = c;
        work[c].strays = 0;
        t[c] = thread_create_on(spin_worker, &work[c], "spin", SCHED_PRIO_DEFAULT, CPUMASK_OF(c));
        CHECK(t[c] != NULL);
    }
    thread_sleep_ms(50);

    /* Parallelism, observed rather than inferred. This used to be a ratio,
     * `work[1].iterations > work[0].iterations / 4` ("an AP spinner had
     * the whole 50 ms"), which is a statement about how the host shared
     * its own CPUs between two vCPUs, and on a loaded host it failed on a
     * correct kernel (docs/testing/flakes.md). What the ratio stood for is
     * that CPU 1 ran its spinner *while* CPU 0 was busy, and that can be
     * watched directly: an observer pinned to CPU 0 sees CPU 1's counter
     * move. With both pinned and neither straying, an advance seen from
     * CPU 0 is two CPUs executing at once. */
    struct spin_observer obs = { .watched = &work[1], .advanced = false };
    if (n > 1) {
        struct thread *ob = thread_create_on(spin_observe, &obs, "spin-observer", SCHED_PRIO_DEFAULT,
                                             CPUMASK_OF(0));
        CHECK(ob != NULL);
        thread_join(ob);
    }
    for (unsigned c = 0; c < n; c++)
        __atomic_store_n(&work[c].stop, 1, __ATOMIC_RELEASE);
    for (unsigned c = 0; c < n; c++)
        thread_join(t[c]);

    /* Every CPU ran its spinner, where it was pinned, including CPU 0
     * alongside this thread (slice preemption). */
    for (unsigned c = 0; c < n; c++) {
        CHECK(work[c].iterations > 0);
        CHECK(work[c].strays == 0);
    }
    if (n > 1)
        CHECK(obs.advanced);
    CHECK(threads_settle(before));
    return true;
}

/* --- cross-CPU function call lands on the target --- */

static void record_cpu(void *arg)
{
    unsigned *out = arg;
    *out = arch_cpu_id();
}

bool selftest_smp_call(const char **reason)
{
    unsigned n = cpu_count();
    for (unsigned c = 0; c < n; c++) {
        if (!cpu_online(c))
            continue;
        unsigned got = 999;
        smp_call_function_single(c, record_cpu, &got);
        CHECK(got == c);
        /* No bound on the round trip here: the call has its own, one
         * second, and panics past it (ipi.c), so "the target answered" is
         * asserted by the return. A `< 100 ms` that used to follow could
         * distinguish nothing a correct kernel does from what a broken one
         * does -- a tick-driven reply would be 4 ms -- and could fail only
         * when the host held the target vCPU. */
    }
    if (n > 1)
        CHECK(ipi_count(IPI_CALL) == 0); /* CPU 0 called itself directly */
    return true;
}

/* --- shootdown reaches every CPU --- */

static void touch_page(void *arg)
{
    volatile uint32_t *p = arg;
    (void)*p; /* pull the translation into this CPU's TLB */
}

bool selftest_smp_shootdown(const char **reason)
{
    unsigned n = online_count();
    struct arch_mmu_shootdown_stats s0, s1;

    struct page *pg = pmm_alloc_page(PMM_FLAGS_ZERO);
    CHECK(pg != NULL);
    vaddr_t win = vm_map_phys(page_to_phys(pg), PAGE_SIZE, VM_PROT_RW, VM_CACHE_WB);
    CHECK(win != 0);

    for (unsigned c = 0; c < cpu_count(); c++) {
        if (cpu_online(c))
            smp_call_function_single(c, touch_page, (void *)win);
    }

    arch_mmu_shootdown_stats(&s0);
    vm_unmap_phys(win); /* one chunk: exactly one shootdown */
    arch_mmu_shootdown_stats(&s1);
    pmm_free_page(pg);

    if (n > 1) {
        CHECK(s1.initiated == s0.initiated + 1);
        CHECK(s1.acks_received == s0.acks_received + (n - 1));
    } else {
        CHECK(s1.initiated == s0.initiated); /* local-only path */
    }
    CHECK(!vm_query(win, NULL, NULL, NULL, NULL));
    return true;
}

/* --- a wake from another CPU runs the target promptly (IPI_RESCHEDULE) --- */

struct cross_wake {
    struct semaphore sem;
    volatile uint64_t woke_at;
    volatile unsigned on_cpu;
    /* IPI_RESCHEDULE handled on the target, read on the target (the
     * waiter is pinned there and ipi_count is this-CPU's), either side
     * of the block. */
    volatile uint64_t ipis_before, ipis_after;
};

static void cross_waiter(void *arg)
{
    struct cross_wake *cw = arg;
    cw->ipis_before = ipi_count(IPI_RESCHEDULE);
    semaphore_down(&cw->sem);
    cw->ipis_after = ipi_count(IPI_RESCHEDULE);
    cw->woke_at = clock_now_ns();
    cw->on_cpu = arch_cpu_id();
}

static bool selftest_smp_wake_pinned(const char **reason)
{
    unsigned before = thread_count();
    if (online_count() < 2) {
        kinfo("selftest: one CPU; cross-CPU wake not exercised");
        return true;
    }

    /* A CPU other than this one, which the wrapper pins: the wake must
     * cross CPUs for the reschedule IPI it counts to be sent at all. */
    unsigned here = arch_cpu_id(), target = here;
    for (unsigned i = 1; i < cpu_count(); i++)
        if (cpu_online((here + i) % cpu_count())) {
            target = (here + i) % cpu_count();
            break;
        }
    CHECK(target != here);
    struct cross_wake cw;
    semaphore_init(&cw.sem, 0, "cross-wake");
    cw.woke_at = 0;
    cw.on_cpu = 999;

    struct thread *t = thread_create_on(cross_waiter, &cw, "cross-waiter", SCHED_PRIO_DEFAULT,
                                        CPUMASK_OF(target));
    CHECK(t != NULL);
    /* Wait for it to *be* blocked rather than sleeping and assuming: a
     * post that finds no waiter yet takes the fast path and sends no
     * IPI, and the check below would fail for a reason that is not the
     * kernel's. BLOCKED is set under the wait-queue lock after the entry
     * is linked (wait.c), so once seen, the post will find the waiter.
     * The kernel writes `state` as a plain store under a lock this test
     * does not hold; this is a one-word read outside it, used to decide
     * when to post and never as a claim. */
    uint64_t deadline = clock_now_ns() + MS(1000);
    while (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) != THREAD_BLOCKED) {
        CHECK(clock_now_ns() < deadline);
        thread_sleep_ms(1);
    }

    uint64_t sent = clock_now_ns();
    semaphore_up(&cw.sem); /* from CPU 0: wake + IPI to CPU 1 */
    thread_join(t);

    CHECK(cw.on_cpu == target);
    CHECK(cw.woke_at >= sent);
    /* The IPI woke the idle CPU: it handled a reschedule IPI between
     * blocking and running again (sched.c, request_resched). This was
     * `woke_at - sent < 2 ms` ("well under a tick"), which never proved
     * that -- a tick landing inside the 2 ms passes it too -- and which a
     * held vCPU fails on a correct kernel. Counting the IPI on the target
     * is the claim itself. */
    CHECK(cw.ipis_after > cw.ipis_before);
    kinfo("selftest: smp-wake: cross-CPU wake seen %llu us after the post",
          (unsigned long long)((cw.woke_at - sent) / 1000));
    CHECK(threads_settle(before));
    return true;
}

/* Pinned for the whole test: the target is "another CPU than mine", a
 * claim that must outlive the test's sleeps (S25). */
bool selftest_smp_wake(const char **reason)
{
    cpumask_t saved = thread_pin_self();
    bool r = selftest_smp_wake_pinned(reason);
    thread_set_affinity_self(saved);
    return r;
}


/* --- every CPU's tick advances --- */

bool selftest_smp_ticks(const char **reason)
{
    uint64_t t0[CONFIG_MAX_CPUS];
    unsigned n = cpu_count();

    for (unsigned c = 0; c < n; c++)
        t0[c] = percpu_get(c)->ticks;
    thread_sleep_ms(40);
    for (unsigned c = 0; c < n; c++) {
        if (!cpu_online(c))
            continue;
        uint64_t d = percpu_get(c)->ticks - t0[c];
        CHECK(d >= 5);   /* 40 ms at 250 Hz = 10 ticks; TCG jitter allowed */
        CHECK(d <= 40);
    }
    return true;
}

/* --- mutex under true parallel contention --- */

struct pmutex {
    struct mutex m;
    volatile uint64_t counter;
    volatile unsigned inside;
    volatile bool violated;
};

static void pmutex_worker(void *arg)
{
    struct pmutex *pm = arg;
    for (int i = 0; i < 300; i++) {
        mutex_lock(&pm->m);
        if (__atomic_fetch_add(&pm->inside, 1u, __ATOMIC_ACQ_REL) != 0)
            pm->violated = true;
        uint64_t v = pm->counter;
        for (volatile int d = 0; d < 50; d++)
            ;
        pm->counter = v + 1;
        __atomic_fetch_sub(&pm->inside, 1u, __ATOMIC_ACQ_REL);
        mutex_unlock(&pm->m);
    }
}

bool selftest_smp_mutex(const char **reason)
{
    unsigned before = thread_count();
    unsigned n = cpu_count();
    struct pmutex pm;
    mutex_init(&pm.m, "smp-mutex");
    pm.counter = 0;
    pm.inside = 0;
    pm.violated = false;

    struct thread *t[2 * CONFIG_MAX_CPUS];
    unsigned k = 0;
    for (unsigned c = 0; c < n; c++) {
        for (int j = 0; j < 2; j++) {
            t[k] = thread_create_on(pmutex_worker, &pm, "pmutex", SCHED_PRIO_DEFAULT, CPUMASK_OF(c));
            CHECK(t[k] != NULL);
            k++;
        }
    }
    for (unsigned i = 0; i < k; i++)
        thread_join(t[i]);

    CHECK(pm.counter == (uint64_t)k * 300);
    CHECK(!pm.violated);
    CHECK(!mutex_is_locked(&pm.m));
    CHECK(threads_settle(before));
    return true;
}

/* --- placement: where a new thread goes ---------------------------------- */

/*
 * A worker that reports the CPU it landed on and then blocks until told
 * to leave, so that its runqueue entry is gone by the time the next
 * thread is created. Blocking is the point: see selftest_sched_spread.
 */
struct placed {
    struct completion started;
    struct completion release;   /* a real block, not a poll: see below */
    volatile unsigned cpu;
};

/*
 * Report the CPU, then block until released -- on a completion, not a
 * sleep loop.
 *
 * The difference is the test. A worker that polls with
 * `thread_sleep_ms(2)` is runnable every 2 ms, so `nr_running` on its
 * CPU is not reliably zero when the next `thread_create` samples it --
 * and a non-zero count is exactly what makes the *old* CPU-0-preferring
 * scan spread threads. The bug-proof would then pass or fail on timing.
 * Blocking on a completion makes the worker stay out of its run queue
 * until cleanup, which is the condition this test needs to be about
 * placement at all.
 */
static void placed_main(void *arg)
{
    struct placed *p = arg;
    preempt_disable();
    p->cpu = arch_cpu_id();   /* the CPU this ran on, exact for the instant */
    preempt_enable();
    complete(&p->started);
    wait_for_completion(&p->release);
    thread_exit(0);
}

/*
 * Threads created on an idle machine must not all land on one CPU.
 *
 * The shape of this test is the finding. Threads created back-to-back
 * *already* spread before this unit, because each one raises its
 * target's `nr_running` and the next scan sees it -- so a test that
 * created four spinners would have passed on the broken code and proved
 * nothing. What does not spread is threads that **block**: a kernel
 * thread waits on a queue almost all of its life, `nr_running` drains
 * back to zero between creations, every CPU ties, and a scan that keeps
 * its first winner hands every one of them to CPU 0.
 *
 * So each worker here signals and then blocks, and the next is created
 * only once the previous has stopped being runnable. That is the
 * condition this kernel is actually in, and the one the measurement in
 * the report came from: 8 of 14 threads on CPU 0.
 */
bool selftest_sched_spread(const char **reason)
{
    unsigned n = cpu_count();
    if (n < 2) {
        kinfo("selftest: sched-spread: one CPU; skipping");
        return true;
    }
    unsigned before = thread_count();
    enum { N = 8 };
    static struct placed p[N];
    struct thread *t[N];

    unsigned made = 0;
    bool ok = true;
    for (unsigned i = 0; i < N && ok; i++) {
        memset(&p[i], 0, sizeof(p[i]));
        completion_init(&p[i].started, "spread");
        completion_init(&p[i].release, "spread-rel");
        t[i] = thread_create(placed_main, &p[i], "spread", SCHED_PRIO_DEFAULT);
        if (t[i] == NULL) {
            ok = false;
            break;
        }
        made++;
        wait_for_completion(&p[i].started);
        /*
         * And then until it is *observably* out of its run queue. The
         * completion above is signalled before the worker blocks, so it
         * says "running", not "blocked" -- and a fixed sleep here would
         * be the "N things after a settle" shape this tree has a whole
         * file about (docs/testing/flakes.md). Wait for the state.
         */
        for (unsigned w = 0; w < 2000 && __atomic_load_n(&t[i]->state, __ATOMIC_ACQUIRE) != THREAD_BLOCKED; w++)
            thread_sleep_ms(1);
        if (__atomic_load_n(&t[i]->state, __ATOMIC_ACQUIRE) != THREAD_BLOCKED)
            ok = false;
    }

    cpumask_t used = 0;
    unsigned on_cpu[CONFIG_MAX_CPUS] = {0};
    for (unsigned i = 0; i < made; i++) {
        used |= CPUMASK_OF(p[i].cpu);
        if (p[i].cpu < CONFIG_MAX_CPUS)
            on_cpu[p[i].cpu]++;
    }
    /* Release and join everything that was created, on every path: a
     * worker left blocked here is a kernel thread leaked into whatever
     * test runs next. */
    for (unsigned i = 0; i < made; i++)
        complete(&p[i].release);
    for (unsigned i = 0; i < made; i++)
        thread_join(t[i]);
    if (!ok) {
        *reason = "a worker could not be created, or never reached THREAD_BLOCKED";
        return false;
    }

    unsigned distinct = 0, worst = 0, worst_cpu = 0;
    for (unsigned c = 0; c < n; c++) {
        if (used & CPUMASK_OF(c))
            distinct++;
        if (on_cpu[c] > worst) {
            worst = on_cpu[c];
            worst_cpu = c;   /* the CPU that actually holds the pile */
        }
    }
    /*
     * Deliberately not `distinct == n`. Other threads in the suite may
     * be runnable while this runs, and the rule is still "least loaded
     * first" -- the rotation only decides ties -- so a CPU that happens
     * to be busy can legitimately be skipped. What the defect looked
     * like is unmissable against either bound: every one of the eight on
     * a single CPU.
     */
    if (distinct < 2 || worst > N / 2) {
        kerror("selftest: sched-spread: %u threads that block after creation used %u of %u CPUs, %u of them on CPU %u",
               made, distinct, n, worst, worst_cpu);
        *reason = "threads created on an idle machine piled onto one CPU";
        return false;
    }
    CHECK(threads_settle(before));
    kinfo("selftest: sched-spread: %u threads that block after creation used %u of %u CPUs, at most %u on any one",
          (unsigned)N, distinct, n, worst);
    return true;
}

/* ======================================================================
 * Migration (docs/audit/next-subsystem-percpu-migration.md)
 * ====================================================================== */

#include <kernel/lockdep.h>
#include <arch/irq.h>

#if CONFIG_DEBUG
/* A thread pinned to `cpu` that reads its CPU through the checked
 * accessor: a one-CPU affinity is a declared claim (S25). */
struct claim_probe {
    unsigned cpu;
};

static void claim_probe_main(void *arg)
{
    struct claim_probe *p = arg;
    p->cpu = arch_cpu_id();   /* checked: the affinity is one CPU */
    thread_exit(0);
}
#endif

/*
 * The per-CPU claim check itself: an unpinned, preemptible read of
 * arch_cpu_id() is reported; the same read under preemption off, under
 * interrupts off, under a self-pin, and from a thread created on one
 * CPU is not.
 */
bool selftest_percpu_claim(const char **reason)
{
#if !CONFIG_DEBUG
    (void)reason;
    return true;
#else
    if (cpu_count() < 2) {
        kinfo("selftest: percpu-claim: one CPU; skipping");
        return true;
    }
    unsigned before = thread_count();
    unsigned hits = percpu_claim_expected_hits();

    /* 1. The violation, armed: counted, not fatal. */
    percpu_claim_expect();
    (void)arch_cpu_id();
    CHECK(percpu_claim_expected_hits() == hits + 1);
    percpu_claim_expect();
    (void)this_cpu();
    CHECK(percpu_claim_expected_hits() == hits + 2);

    /* 2. The quiet forms, each armed too, so the test can say which one
     * spoke rather than the boot panicking on it. */
    percpu_claim_expect();
    preempt_disable();
    (void)arch_cpu_id();
    (void)this_cpu();
    preempt_enable();
    if (percpu_claim_expected_hits() != hits + 2) {
        *reason = "a read under preempt_disable was reported";
        return false;
    }
    arch_irq_state_t s = arch_irq_save();
    (void)arch_cpu_id();
    arch_irq_restore(s);
    if (percpu_claim_expected_hits() != hits + 2) {
        *reason = "a read under arch_irq_save was reported";
        return false;
    }
    cpumask_t saved = thread_pin_self();
    unsigned here = arch_cpu_id();
    thread_set_affinity_self(saved);
    if (percpu_claim_expected_hits() != hits + 2) {
        *reason = "a read under a self-pin was reported";
        return false;
    }
    struct claim_probe p = { .cpu = ~0u };
    struct thread *t = thread_create_on(claim_probe_main, &p, "claim-probe", SCHED_PRIO_DEFAULT, CPUMASK_OF(here));
    CHECK(t != NULL);
    thread_join(t);
    CHECK(p.cpu == here);
    if (percpu_claim_expected_hits() != hits + 2) {
        *reason = "a read from a thread created on one CPU was reported";
        return false;
    }
    /* Consume the armed expectation with one more deliberate violation,
     * so it cannot swallow a real one in a later test. */
    (void)arch_cpu_id();
    CHECK(percpu_claim_expected_hits() == hits + 3);
    CHECK(threads_settle(before));
    kinfo("selftest: percpu-claim: an unpinned preemptible read is reported; preempt off, interrupts off, a self-pin and a one-CPU thread are not");
    return true;
#endif
}

/*
 * Each run queue's lock is its own lockdep class, so the increasing-CPU-id
 * order (S24) is an order lockdep checks: 0 then 1 recorded by a real
 * migration, 1 then 0 is a cycle it reports.
 */
bool selftest_lockdep_rq_order(const char **reason)
{
#if !CONFIG_LOCKDEP
    (void)reason;
    return true;
#else
    if (cpu_count() < 2 || !cpu_online(1)) {
        kinfo("selftest: lockdep-rq-order: needs CPUs 0 and 1; skipping");
        return true;
    }
    /* The recorded order: a migration attempt from 0 to 1 takes both
     * locks, 0 first, whether or not it moves anything. */
    struct thread *moved;
    (void)sched_migrate_from(0, 1, &moved);

    struct runqueue *rq0 = sched_runqueue(0), *rq1 = sched_runqueue(1);
    unsigned hits = lockdep_expected_hits();
    lockdep_expect(LOCKDEP_R_INVERSION);
    arch_irq_state_t s = arch_irq_save();
    spin_lock(&rq1->lock);
    /* The reversed pair, asked of the checker without taking the second
     * lock: reported, the expectation consumes it. Taking it for real
     * spun against a chaos tick on another CPU holding the pair in the
     * right order -- the very deadlock the report is about -- and hung
     * every CPU with interrupts off. */
    spin_lock_check_order(&rq0->lock);
    spin_unlock(&rq1->lock);
    arch_irq_restore(s);
    if (lockdep_expected_hits() != hits + 1) {
        *reason = "taking run queue 1's lock then 0's was not reported as an inversion";
        return false;
    }
    kinfo("selftest: lockdep-rq-order: runqueue1 then runqueue0 is a cycle against the recorded runqueue0 -> runqueue1");
    return true;
#endif
}

/* A worker that records the CPU it runs on, every round, until released. */
struct mig_worker {
    volatile unsigned release;
    volatile unsigned cpu;        /* the last CPU it ran on; ~0u before it ever ran */
    volatile unsigned runs;
};

static void mig_worker_main(void *arg)
{
    struct mig_worker *w = arg;
    while (!__atomic_load_n(&w->release, __ATOMIC_ACQUIRE)) {
        preempt_disable();
        w->cpu = arch_cpu_id();   /* exact for the instant */
        preempt_enable();
        w->runs++;
        arch_cpu_relax();
    }
    thread_exit(0);
}

/* A spinner at a higher priority, pinned: the CPU it holds runs nothing
 * of lower priority while it spins, so a worker queued there stays READY. */
struct mig_spinner {
    volatile unsigned stop;
};

static void mig_spinner_main(void *arg)
{
    struct mig_spinner *s = arg;
    while (!__atomic_load_n(&s->stop, __ATOMIC_ACQUIRE))
        arch_cpu_relax();
    thread_exit(0);
}

/* Two CPUs other than this one, online: a and b. False if there are not two. */
static bool two_other_cpus(unsigned here, unsigned *a, unsigned *b)
{
    unsigned n = cpu_count(), found = 0;
    for (unsigned c = 0; c < n && found < 2; c++) {
        if (c == here || !cpu_online(c))
            continue;
        if (found == 0)
            *a = c;
        else
            *b = c;
        found++;
    }
    return found == 2;
}

static bool wait_ready_on(struct thread *t, unsigned cpu)
{
    uint64_t deadline = clock_deadline_ns(2000000000ULL);
    while (!clock_deadline_passed(deadline)) {
        if (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) == THREAD_READY && __atomic_load_n(&t->cpu, __ATOMIC_ACQUIRE) == (int)cpu)
            return true;
        thread_sleep_ms(1);
    }
    return false;
}

/*
 * One thread, one move: a worker held READY on CPU A behind a
 * higher-priority spinner is moved to B by sched_migrate, runs next on
 * B (its first recorded CPU is B), and the migration count rose.
 */
static bool sched_migrate_pinned(const char **reason)
{
    unsigned here = arch_cpu_id();   /* pinned by the wrapper */
    unsigned a, b;
    if (!two_other_cpus(here, &a, &b)) {
        kinfo("selftest: sched-migrate: needs three online CPUs; skipping");
        return true;
    }
    unsigned before = thread_count();
    uint64_t moves = sched_migration_count();

    struct mig_spinner sp = { 0 };
    struct thread *ts = thread_create_on(mig_spinner_main, &sp, "mig-spin", SCHED_PRIO_DEFAULT - 1, CPUMASK_OF(a));
    CHECK(ts != NULL);
    struct mig_worker w = { .cpu = ~0u };
    struct thread *tw = thread_create_on(mig_worker_main, &w, "mig-worker", SCHED_PRIO_DEFAULT, CPUMASK_OF(a));
    if (tw == NULL) {
        __atomic_store_n(&sp.stop, 1u, __ATOMIC_RELEASE);
        thread_join(ts);
        *reason = "cannot create the worker";
        return false;
    }
    bool ready = wait_ready_on(tw, a);
    /* The worker may leave for b now; nothing else has touched its queue
     * entry. Under the chaos migrator the move may already have been made
     * by the tick between these two lines, which the result names. */
    thread_set_affinity(tw, CPUMASK_OF(a) | CPUMASK_OF(b));
    enum sched_migrate_result r = ready ? sched_migrate(tw, b) : SCHED_MIGRATE_NOT_READY;

    /* It runs next on b: wait for the first recorded CPU. */
    uint64_t deadline = clock_deadline_ns(2000000000ULL);
    while (w.cpu == ~0u && !clock_deadline_passed(deadline))
        thread_sleep_ms(1);
    unsigned first = w.cpu;
    __atomic_store_n(&w.release, 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&sp.stop, 1u, __ATOMIC_RELEASE);
    if (first != ~0u)
        thread_join(tw);
    thread_join(ts);
    if (!ready) {
        *reason = "the worker never became READY on the spinner's CPU";
        return false;
    }
    if (r != SCHED_MIGRATED && r != SCHED_MIGRATE_SAME_CPU) {
        kerror("selftest: sched-migrate: sched_migrate returned %s", sched_migrate_result_name(r));
        *reason = "a READY, unpinned worker was refused";
        return false;
    }
    if (first != b) {
        kerror("selftest: sched-migrate: worker's first run on cpu %u, expected %u (result %s)", first, b,
               sched_migrate_result_name(r));
        *reason = first == ~0u ? "the migrated worker never ran" : "the migrated worker ran on the wrong CPU";
        return false;
    }
    CHECK(sched_migration_count() > moves);
    CHECK(threads_settle(before));
    kinfo("selftest: sched-migrate: a worker READY on cpu %u moved to cpu %u and ran there first (%s)", a, b,
          sched_migrate_result_name(r));
    return true;
}

bool selftest_sched_migrate(const char **reason)
{
    cpumask_t saved = thread_pin_self();
    bool r = sched_migrate_pinned(reason);
    thread_set_affinity_self(saved);
    return r;
}

/* The woken-before-blocked window: prepared to wait (BLOCKED), then woken
 * (READY, queued) while still running -- held open by preemption off. */
struct window_probe {
    struct waitqueue wq;
    volatile unsigned in_window;
    volatile unsigned go;
};

static void window_main(void *arg)
{
    struct window_probe *p = arg;
    struct wait_entry e;
    wait_entry_init(&e);
    waitqueue_prepare(&p->wq, &e);   /* BLOCKED, still running */
    preempt_disable();               /* a tick must not switch us out: we stay rq->current */
    __atomic_store_n(&p->in_window, 1u, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&p->go, __ATOMIC_ACQUIRE))
        arch_cpu_relax();
    preempt_enable();
    waitqueue_finish(&p->wq, &e);    /* takes itself off the queue it was woken onto */
    thread_exit(0);
}

/*
 * Every refusal by its name: the pinned worker, the blocked one, the
 * running spinner, a CPU that is not one, the same CPU -- and the window,
 * where a thread is READY and queued and still its CPU's current.
 */
static bool sched_migrate_refuses_pinned(const char **reason)
{
    unsigned here = arch_cpu_id();   /* pinned by the wrapper */
    unsigned a, b;
    if (!two_other_cpus(here, &a, &b)) {
        kinfo("selftest: sched-migrate-refuses: needs three online CPUs; skipping");
        return true;
    }
    unsigned before = thread_count();
    bool ok = true;
    enum sched_migrate_result r;

    struct mig_spinner sp = { 0 };
    struct thread *ts = thread_create_on(mig_spinner_main, &sp, "mig-spin", SCHED_PRIO_DEFAULT - 1, CPUMASK_OF(a));
    CHECK(ts != NULL);
    struct mig_worker w = { .cpu = ~0u };
    struct thread *tw = thread_create_on(mig_worker_main, &w, "mig-pinned", SCHED_PRIO_DEFAULT, CPUMASK_OF(a));
    CHECK(tw != NULL);
    bool ready = wait_ready_on(tw, a);

    if (ready) {
        r = sched_migrate(tw, b);
        if (r != SCHED_MIGRATE_AFFINITY) {
            kerror("selftest: sched-migrate-refuses: a worker pinned to cpu %u: %s", a, sched_migrate_result_name(r));
            ok = false;
        }
        r = sched_migrate(tw, a);
        if (r != SCHED_MIGRATE_SAME_CPU) {
            kerror("selftest: sched-migrate-refuses: to its own CPU: %s", sched_migrate_result_name(r));
            ok = false;
        }
        r = sched_migrate(tw, cpu_count());
        if (r != SCHED_MIGRATE_OFFLINE) {
            kerror("selftest: sched-migrate-refuses: to a CPU that is not one: %s", sched_migrate_result_name(r));
            ok = false;
        }
        r = sched_migrate(ts, b);   /* the spinner: RUNNING on a */
        if (r != SCHED_MIGRATE_NOT_READY) {
            kerror("selftest: sched-migrate-refuses: the running spinner: %s", sched_migrate_result_name(r));
            ok = false;
        }
    }
    __atomic_store_n(&w.release, 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&sp.stop, 1u, __ATOMIC_RELEASE);
    thread_join(tw);
    thread_join(ts);
    if (!ready) {
        *reason = "the worker never became READY on the spinner's CPU";
        return false;
    }

    /* A preempted thread: a worker that has run on a, then a higher
     * priority spinner pinned to a takes the CPU from it. READY, queued,
     * not current -- and refused, because it stopped where the tick
     * found it, perhaps between the two instructions of a per-CPU read. */
    struct mig_worker pw = { .cpu = ~0u };
    struct thread *tp = thread_create_on(mig_worker_main, &pw, "mig-preempted", SCHED_PRIO_DEFAULT,
                                         CPUMASK_OF(a) | CPUMASK_OF(b));
    CHECK(tp != NULL);
    uint64_t deadline = clock_deadline_ns(2000000000ULL);
    while (pw.runs == 0 && !clock_deadline_passed(deadline))
        thread_sleep_ms(1);
    struct mig_spinner sp2 = { 0 };
    unsigned on = pw.cpu;   /* where it runs; the spinner goes there */
    struct thread *ts2 = on == a || on == b ? thread_create_on(mig_spinner_main, &sp2, "mig-spin2", SCHED_PRIO_DEFAULT - 1, CPUMASK_OF(on)) : NULL;
    bool preempted = ts2 != NULL && wait_ready_on(tp, on);
    if (preempted) {
        r = sched_migrate(tp, on == a ? b : a);
        if (r != SCHED_MIGRATE_PREEMPTED) {
            kerror("selftest: sched-migrate-refuses: a preempted worker: %s", sched_migrate_result_name(r));
            ok = false;
        }
    }
    __atomic_store_n(&pw.release, 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&sp2.stop, 1u, __ATOMIC_RELEASE);
    if (ts2)
        thread_join(ts2);
    thread_join(tp);
    if (!preempted) {
        *reason = "the worker never ran, or was never preempted on its CPU";
        return false;
    }

    /* A blocked thread, and then the window: the probe prepares to wait
     * (BLOCKED, still running), and is then woken (READY, queued, still
     * its CPU's current). */
    struct window_probe wp;
    waitqueue_init(&wp.wq, "mig-window");
    wp.in_window = 0;
    wp.go = 0;
    /* On a CPU that is not this thread's: the probe spins with preemption
     * off until told to go, and the teller must be able to run. */
    struct thread *tv = thread_create_on(window_main, &wp, "mig-window", SCHED_PRIO_DEFAULT,
                                         CPUMASK_OF(a) | CPUMASK_OF(b));   /* either other CPU: the current check alone refuses the window */
    CHECK(tv != NULL);
    deadline = clock_deadline_ns(2000000000ULL);
    while (!__atomic_load_n(&wp.in_window, __ATOMIC_ACQUIRE) && !clock_deadline_passed(deadline))
        thread_sleep_ms(1);
    if (!wp.in_window) {
        __atomic_store_n(&wp.go, 1u, __ATOMIC_RELEASE);
        thread_join(tv);
        *reason = "the window probe never reached its window";
        return false;
    }
    /* BLOCKED and running: not ready. */
    unsigned away = (unsigned)tv->cpu == a ? b : a;
    r = sched_migrate(tv, away);
    if (r != SCHED_MIGRATE_NOT_READY) {
        kerror("selftest: sched-migrate-refuses: a thread prepared to wait: %s", sched_migrate_result_name(r));
        ok = false;
    }
    /* Woken: READY and queued, still its CPU's current. */
    waitqueue_wake_all(&wp.wq);
    r = sched_migrate(tv, away);   /* its mask admits `away`: only the current check refuses the window */
    if (r != SCHED_MIGRATE_CURRENT) {
        kerror("selftest: sched-migrate-refuses: the woken-before-blocked window: %s (state %d)",
               sched_migrate_result_name(r), (int)tv->state);
        ok = false;
    }
    __atomic_store_n(&wp.go, 1u, __ATOMIC_RELEASE);
    thread_join(tv);
    if (!ok) {
        *reason = "a refusal came back under the wrong name";
        return false;
    }
    CHECK(threads_settle(before));
    kinfo("selftest: sched-migrate-refuses: affinity, same-cpu, offline, not-ready (running and blocked), preempted and current (the window) each refused by name");
    return true;
}

bool selftest_sched_migrate_refuses(const char **reason)
{
    cpumask_t saved = thread_pin_self();
    bool r = sched_migrate_refuses_pinned(reason);
    thread_set_affinity_self(saved);
    return r;
}

/* --- the stress --- */

struct stress_shared {
    volatile unsigned stop;
    volatile unsigned bad_affinity;   /* a worker found itself on a CPU its mask excludes */
    struct mutex mtx;
};

struct stress_worker {
    struct stress_shared *sh;
    volatile uint64_t rounds;
    struct completion *wait_on;   /* ping-pong: what I wait for */
    struct completion *signal;    /*            and what I signal */
};

static void stress_check_here(struct stress_worker *w)
{
    preempt_disable();
    if ((thread_current()->affinity & CPUMASK_OF(arch_cpu_id())) == 0)
        __atomic_store_n(&w->sh->bad_affinity, 1u, __ATOMIC_RELEASE);
    preempt_enable();
}

static void stress_spinner(void *arg)
{
    struct stress_worker *w = arg;
    while (!__atomic_load_n(&w->sh->stop, __ATOMIC_ACQUIRE)) {
        stress_check_here(w);
        w->rounds++;
        for (unsigned i = 0; i < 64; i++)
            arch_cpu_relax();
    }
    thread_exit(0);
}

static void stress_sleeper(void *arg)
{
    struct stress_worker *w = arg;
    while (!__atomic_load_n(&w->sh->stop, __ATOMIC_ACQUIRE)) {
        stress_check_here(w);
        w->rounds++;
        thread_sleep_ms(1);
    }
    thread_exit(0);
}

static void stress_pingpong(void *arg)
{
    struct stress_worker *w = arg;
    while (!__atomic_load_n(&w->sh->stop, __ATOMIC_ACQUIRE)) {
        complete(w->signal);
        wait_for_completion(w->wait_on);
        stress_check_here(w);
        w->rounds++;
    }
    complete(w->signal);   /* the partner may be waiting on us */
    thread_exit(0);
}

static void stress_mutexer(void *arg)
{
    struct stress_worker *w = arg;
    while (!__atomic_load_n(&w->sh->stop, __ATOMIC_ACQUIRE)) {
        mutex_lock(&w->sh->mtx);
        stress_check_here(w);
        w->rounds++;
        mutex_unlock(&w->sh->mtx);
        arch_cpu_relax();
    }
    thread_exit(0);
}

static uint64_t stress_rand(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    return x;
}

static void stress_migrator(void *arg)
{
    struct stress_worker *w = arg;
    uint64_t seed = 0x9E3779B97F4A7C15ull;
    unsigned n = cpu_count();
    while (!__atomic_load_n(&w->sh->stop, __ATOMIC_ACQUIRE)) {
        unsigned from = (unsigned)(stress_rand(&seed) % n), to = (unsigned)(stress_rand(&seed) % n);
        struct thread *moved;
        if (sched_migrate_from(from, to, &moved) == SCHED_MIGRATED)
            w->rounds++;
        arch_cpu_relax();
    }
    thread_exit(0);
}

/*
 * 200 ms of spinners, sleepers, ping-pong pairs and mutex contenders
 * while a migrator moves whatever it finds between random CPUs: every
 * worker made progress, no worker ever ran outside its mask, and the
 * migration count rose by at least 100.
 */
bool selftest_sched_migrate_stress(const char **reason)
{
    if (cpu_count() < 2) {
        kinfo("selftest: sched-migrate-stress: one CPU; skipping");
        return true;
    }
    unsigned before = thread_count();
    uint64_t moves = sched_migration_count();
    enum { SPIN = 8, SLEEP = 8, PAIRS = 4, MUTEX = 2 };
    static struct stress_shared sh;
    static struct stress_worker spin[SPIN], sleep[SLEEP], pp[2 * PAIRS], mx[MUTEX], mig;
    static struct completion pc[2 * PAIRS];
    struct thread *t[SPIN + SLEEP + 2 * PAIRS + MUTEX + 1];
    unsigned made = 0;

    memset(&sh, 0, sizeof(sh));
    mutex_init(&sh.mtx, "mig-stress");
    for (unsigned i = 0; i < 2 * PAIRS; i++)
        completion_init(&pc[i], "mig-pp");
    for (unsigned i = 0; i < SPIN; i++) {
        spin[i] = (struct stress_worker){ .sh = &sh };
        t[made++] = thread_create(stress_spinner, &spin[i], "mig-spin", SCHED_PRIO_DEFAULT);
    }
    for (unsigned i = 0; i < SLEEP; i++) {
        sleep[i] = (struct stress_worker){ .sh = &sh };
        t[made++] = thread_create(stress_sleeper, &sleep[i], "mig-sleep", SCHED_PRIO_DEFAULT);
    }
    for (unsigned i = 0; i < PAIRS; i++) {
        pp[2 * i] = (struct stress_worker){ .sh = &sh, .wait_on = &pc[2 * i], .signal = &pc[2 * i + 1] };
        pp[2 * i + 1] = (struct stress_worker){ .sh = &sh, .wait_on = &pc[2 * i + 1], .signal = &pc[2 * i] };
        t[made++] = thread_create(stress_pingpong, &pp[2 * i], "mig-ping", SCHED_PRIO_DEFAULT);
        t[made++] = thread_create(stress_pingpong, &pp[2 * i + 1], "mig-pong", SCHED_PRIO_DEFAULT);
    }
    for (unsigned i = 0; i < MUTEX; i++) {
        mx[i] = (struct stress_worker){ .sh = &sh };
        t[made++] = thread_create(stress_mutexer, &mx[i], "mig-mutex", SCHED_PRIO_DEFAULT);
    }
    mig = (struct stress_worker){ .sh = &sh };
    t[made++] = thread_create(stress_migrator, &mig, "mig-migrator", SCHED_PRIO_DEFAULT);

    thread_sleep_ms(200);
    __atomic_store_n(&sh.stop, 1u, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < 2 * PAIRS; i++)
        complete(&pc[i]);   /* release any half of a pair still waiting */
    bool all_made = true;
    for (unsigned i = 0; i < made; i++) {
        if (t[i] != NULL)
            thread_join(t[i]);
        else
            all_made = false;
    }
    if (!all_made) {
        *reason = "a worker could not be created";
        return false;
    }
    uint64_t moved = sched_migration_count() - moves;
    bool progress = true;
    for (unsigned i = 0; i < SPIN; i++) progress = progress && spin[i].rounds > 0;
    for (unsigned i = 0; i < SLEEP; i++) progress = progress && sleep[i].rounds > 0;
    for (unsigned i = 0; i < 2 * PAIRS; i++) progress = progress && pp[i].rounds > 0;
    for (unsigned i = 0; i < MUTEX; i++) progress = progress && mx[i].rounds > 0;
    if (!progress) {
        *reason = "a worker made no progress under migration";
        return false;
    }
    if (sh.bad_affinity) {
        *reason = "a worker ran on a CPU its affinity excludes";
        return false;
    }
    if (moved < 100) {
        kerror("selftest: sched-migrate-stress: only %llu migrations in 200 ms (the migrator made %llu)",
               (unsigned long long)moved, (unsigned long long)mig.rounds);
        *reason = "fewer than 100 migrations happened";
        return false;
    }
    CHECK(threads_settle(before));
    kinfo("selftest: sched-migrate-stress: %llu migrations in 200 ms over %u workers; every worker progressed and stayed inside its mask",
          (unsigned long long)moved, made - 1);
    return true;
}
