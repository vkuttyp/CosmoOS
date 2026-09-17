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

bool selftest_smp_ipi_storm(const char **reason)
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
    for (unsigned c = 1; c < cpu_count(); c++) {
        if (!cpu_online(c))
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
        for (unsigned c = 1; c < cpu_count(); c++) {
            if (cpu_online(c))
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
    *out = arch_cpu_id();
    /* Stay a little so placement is not just a first instruction. */
    thread_sleep_ms(2);
    *out = *out * 100 + arch_cpu_id();
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

bool selftest_smp_wake(const char **reason)
{
    unsigned before = thread_count();
    if (online_count() < 2) {
        kinfo("selftest: one CPU; cross-CPU wake not exercised");
        return true;
    }

    unsigned target = 1;
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
    p->cpu = arch_cpu_id();
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
