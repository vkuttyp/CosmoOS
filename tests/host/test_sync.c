/*
 * test_sync.c - Wait queues, completions, semaphores and mutexes on a
 * deterministic one-CPU scheduler (tests/host/sync_sched.c; roadmap 2.0 SA3).
 *
 * Each workload runs under many seeds, each seed one interleaving of fake
 * threads, and checks its invariants as it goes (mutual exclusion, the
 * semaphore's bound, priority inheritance's boosts and their undoing, every
 * waiter released). Every lock acquisition and release, wake, block, timer
 * and priority change is a line of the trace; the lines' FNV-1a digest is
 * recorded below. An implementation of these primitives in another language
 * must give the same digest before the C is deleted (SA3), as the handle
 * layer did (SA2). SYNC_TRACE_DUMP=1 prints the lines, to find the first
 * that differs.
 */

#include <stdio.h>

#include <kernel/completion.h>
#include <kernel/mutex.h>
#include <kernel/sched.h>
#include <kernel/semaphore.h>
#include <kernel/wait.h>

#include "harness.h"
#include "sync_baton.h"
#include "sync_sched.h"

#define TRACE_DIGEST 0xc4ef20be17b35afaull   /* recorded from the C implementation, 2026-10-30 */
#define SEEDS 300

/* --- mutex contention: mutual exclusion and every thread's turns --- */

static struct mutex g_mutex;
static int g_inside, g_turns[SYNC_MAX_THREADS];

static void contend(int id)
{
    for (int i = 0; i < 4; i++) {
        if (sync_rand(2) == 0) {
            mutex_lock(&g_mutex);
        } else if (!mutex_trylock(&g_mutex)) {
            sync_trace("T%d trylock fails", id);
            mutex_lock(&g_mutex);
        }
        EXPECT(++g_inside == 1);
        g_turns[id]++;
        for (uint64_t y = sync_rand(3); y > 0; y--)
            sync_yield();   /* others contend while it is held */
        EXPECT(mutex_is_locked(&g_mutex));
        g_inside--;
        mutex_unlock(&g_mutex);
        if (sync_rand(2) == 0)
            sync_yield();
    }
}

static void test_mutex_contention(void)
{
    static const int prio[] = { 20, 20, 30, 10 };
    for (uint64_t seed = 0; seed < SEEDS; seed++) {
        mutex_init(&g_mutex, "contended");
        g_inside = 0;
        for (int i = 0; i < 4; i++)
            g_turns[i] = 0;
        sync_trace("mutex-contention seed %llu", (unsigned long long)seed);
        EXPECT(sync_run(seed, 4, prio, contend));
        EXPECT(!mutex_is_locked(&g_mutex));
        for (int i = 0; i < 4; i++)
            EXPECT(g_turns[i] == 4);
    }
}

/* --- priority inheritance along a chain: L holds A; M holds B and waits on A; H waits on B --- */

static struct mutex g_a, g_b;
static int g_stage;

static void chain(int id)
{
    switch (id) {
    case 0:   /* L, priority 50 */
        mutex_lock(&g_a);
        g_stage = 1;
        while (g_stage < 3)
            sync_yield();   /* M and H queue behind it */
        EXPECT(sync_priority(0) == 10);   /* H's priority, through M */
        mutex_unlock(&g_a);
        EXPECT(sync_priority(0) == 50);
        break;
    case 1:   /* M, priority 30 */
        while (g_stage < 1)
            sync_yield();
        mutex_lock(&g_b);
        g_stage = 2;
        mutex_lock(&g_a);
        mutex_unlock(&g_a);
        mutex_unlock(&g_b);
        EXPECT(sync_priority(1) == 30);
        break;
    case 2:   /* H, priority 10 */
        while (g_stage < 2)
            sync_yield();
        g_stage = 3;
        mutex_lock(&g_b);
        mutex_unlock(&g_b);
        break;
    }
}

static void test_mutex_pi_chain(void)
{
    static const int prio[] = { 50, 30, 10 };
    for (uint64_t seed = 0; seed < SEEDS; seed++) {
        mutex_init(&g_a, "pi-a");
        mutex_init(&g_b, "pi-b");
        g_stage = 0;
        sync_trace("mutex-pi-chain seed %llu", (unsigned long long)seed);
        EXPECT(sync_run(seed, 3, prio, chain));
        EXPECT(!mutex_is_locked(&g_a) && !mutex_is_locked(&g_b));
    }
}

/* --- completions: waiters released once, and a timed wait --- */

static struct completion g_done, g_never;
static int g_released;

static void complete_body(int id)
{
    if (id == 0) {
        for (uint64_t y = sync_rand(4); y > 0; y--)
            sync_yield();
        complete(&g_done);
        EXPECT(!wait_for_completion_timeout(&g_never, 1000000));   /* nobody completes it: the fake clock runs out */
        return;
    }
    if (id == 3) {
        EXPECT(wait_for_completion_timeout(&g_done, 50000000));   /* completed long before the deadline */
    } else {
        wait_for_completion(&g_done);
    }
    EXPECT(completion_done(&g_done));
    g_released++;
}

static void test_completion(void)
{
    static const int prio[] = { 20, 20, 20, 20 };
    for (uint64_t seed = 0; seed < SEEDS; seed++) {
        completion_init(&g_done, "done");
        completion_init(&g_never, "never");
        g_released = 0;
        sync_trace("completion seed %llu", (unsigned long long)seed);
        EXPECT(sync_run(seed, 4, prio, complete_body));
        EXPECT(g_released == 3 && !completion_done(&g_never));
    }
}

/* --- a counting semaphore: never more than its count inside --- */

static struct semaphore g_sem;
static int g_holders, g_most;

static void sem_body(int id)
{
    for (int i = 0; i < 3; i++) {
        if (!semaphore_trydown(&g_sem))
            semaphore_down(&g_sem);
        g_holders++;
        if (g_holders > g_most)
            g_most = g_holders;
        sync_trace("T%d holds (%d inside)", id, g_holders);
        for (uint64_t y = sync_rand(3); y > 0; y--)
            sync_yield();
        g_holders--;
        semaphore_up(&g_sem);
    }
}

static void test_semaphore(void)
{
    static const int prio[] = { 20, 20, 20, 20, 20 };
    for (uint64_t seed = 0; seed < SEEDS; seed++) {
        semaphore_init(&g_sem, 2, "sem");
        g_holders = g_most = 0;
        sync_trace("semaphore seed %llu", (unsigned long long)seed);
        EXPECT(sync_run(seed, 5, prio, sem_body));
        EXPECT(g_most <= 2 && semaphore_count(&g_sem) == 2);
    }
}

/* --- wait-queue callbacks beside sleeping threads --- */

static struct waitqueue g_wq;
static struct wait_entry g_cb[2];
static int g_cb_runs[2], g_cb_freed[2];
static bool g_flag[SYNC_MAX_THREADS];

static void callback(struct wait_entry *e, unsigned flags)
{
    int k = (int)(e - g_cb);
    if (flags & WAIT_CB_FREED)
        g_cb_freed[k]++;
    else
        g_cb_runs[k]++;
    sync_trace("callback %d flags %u", k, flags);
}

static void wq_body(int id)
{
    if (id == 0) {
        while (waitqueue_waiting(&g_wq) < 2)
            sync_yield();
        g_flag[1] = g_flag[2] = true;
        unsigned one = waitqueue_wake_one(&g_wq);   /* both callbacks, then one thread */
        sync_trace("wake_one woke %u", one);
        EXPECT(one == 1);
        unsigned rest = waitqueue_wake_all(&g_wq);
        sync_trace("wake_all woke %u", rest);
        EXPECT(rest <= 1);
        while (waitqueue_waiting(&g_wq) > 0)
            sync_yield();
        EXPECT(waitqueue_detach_callbacks(&g_wq) == 2);
        EXPECT(waitqueue_empty(&g_wq));
        return;
    }
    wait_event(&g_wq, g_flag[id]);
    sync_trace("T%d leaves the queue", id);
}

static void test_waitqueue_callbacks(void)
{
    static const int prio[] = { 20, 20, 20 };
    for (uint64_t seed = 0; seed < SEEDS; seed++) {
        waitqueue_init(&g_wq, "callbacks");
        for (int k = 0; k < 2; k++) {
            wait_entry_init(&g_cb[k]);
            waitqueue_add_callback(&g_wq, &g_cb[k], callback);
            g_cb_runs[k] = g_cb_freed[k] = 0;
        }
        g_flag[1] = g_flag[2] = false;
        sync_trace("waitqueue-callbacks seed %llu", (unsigned long long)seed);
        EXPECT(sync_run(seed, 3, prio, wq_body));
        for (int k = 0; k < 2; k++)
            EXPECT(g_cb_runs[k] == 2 && g_cb_freed[k] == 1);
    }
}

/* --- the recorded trace --- */

static void test_trace_digest(void)
{
    uint64_t digest = sync_trace_digest();
    printf("sync trace: %llu lines, digest 0x%016llx (recorded 0x%016llx)\n", (unsigned long long)sync_trace_lines(),
           (unsigned long long)digest, (unsigned long long)TRACE_DIGEST);
    EXPECT(digest == TRACE_DIGEST);
}

int main(void)
{
    sync_trace_dump(baton_env_on("SYNC_TRACE_DUMP"));
    static const struct host_test tests[] = {
        { "mutex-contention", test_mutex_contention },
        { "mutex-pi-chain", test_mutex_pi_chain },
        { "completion", test_completion },
        { "semaphore", test_semaphore },
        { "waitqueue-callbacks", test_waitqueue_callbacks },
        { "sync-trace", test_trace_digest },
    };
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
