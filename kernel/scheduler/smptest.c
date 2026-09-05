/*
 * smptest.c - Boot-time self-tests for multiprocessor operation.
 *
 * Run from thread 0 on CPU 0 after smp_init. When only one CPU came up
 * (QEMU -smp 1, or firmware without APs) the tests that need a second
 * CPU report "ok" after checking the single-CPU behaviour, so the suite
 * stays meaningful in both configurations.
 */

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
    volatile uint64_t iterations;
    volatile int stop;
};

static void spin_worker(void *arg)
{
    struct spin_work *w = arg;
    while (!__atomic_load_n(&w->stop, __ATOMIC_ACQUIRE)) {
        w->iterations++;
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
        t[c] = thread_create_on(spin_worker, &work[c], "spin", SCHED_PRIO_DEFAULT, CPUMASK_OF(c));
        CHECK(t[c] != NULL);
    }
    thread_sleep_ms(50);
    for (unsigned c = 0; c < n; c++)
        __atomic_store_n(&work[c].stop, 1, __ATOMIC_RELEASE);
    for (unsigned c = 0; c < n; c++)
        thread_join(t[c]);

    /* Every CPU ran its spinner, including CPU 0 alongside this thread
     * (slice preemption), and the APs ran unhindered. */
    for (unsigned c = 0; c < n; c++)
        CHECK(work[c].iterations > 0);
    if (n > 1) {
        /* An AP spinner had the whole 50 ms; thread 0 shared CPU 0 with
         * its spinner, so CPU 0's count is lower but still substantial. */
        CHECK(work[1].iterations > work[0].iterations / 4);
    }
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
        uint64_t t0 = clock_now_ns();
        smp_call_function_single(c, record_cpu, &got);
        CHECK(got == c);
        CHECK(clock_now_ns() - t0 < MS(100));
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
};

static void cross_waiter(void *arg)
{
    struct cross_wake *cw = arg;
    semaphore_down(&cw->sem);
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
    thread_sleep_ms(10); /* it is now blocked on CPU 1, which is idle in hlt */

    uint64_t sent = clock_now_ns();
    semaphore_up(&cw.sem); /* from CPU 0: wake + IPI to CPU 1 */
    thread_join(t);

    CHECK(cw.on_cpu == target);
    CHECK(cw.woke_at >= sent);
    /* Well under a tick: the IPI, not the tick, woke the idle CPU. */
    CHECK(cw.woke_at - sent < MS(2));
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
