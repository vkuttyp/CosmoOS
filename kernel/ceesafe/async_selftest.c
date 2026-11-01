/*
 * async_selftest.c - The self-test cee-async, and the environment of
 * CeeSafe's async executor in the kernel (CeeSafe ADR 0039, its profiles.md
 * §4.2): the four ceesafe_env_* hooks over the kernel's clock, a wait queue
 * and a wake flag, the allocator its tasks' frames come from, and a timer
 * whose callback raises the test's AsyncSignal in interrupt context.
 *
 * The checks are CeeSafe (kernel/ceesafe/async_test.cee, generated into
 * async_test.c by `make cee`); this names the one that failed.
 *
 * One executor runs at a time: the slot is this file's, not a thread's,
 * which is enough for the self-test. A driver that runs tasks on its own
 * thread brings a per-thread slot (a field of struct thread) with it.
 */

#include <stddef.h>
#include <stdint.h>

#include "ceesafe/rt.h"
#include "kernel/kmalloc.h"
#include "kernel/log.h"
#include "kernel/panic.h"
#include "kernel/sched.h"
#include "kernel/selftest.h"
#include "kernel/thread.h"
#include "kernel/timer.h"
#include "kernel/wait.h"

uint32_t kasync_cee_selftest(void);
void kasync_raise(void);
void *kasync_alloc(size_t size, size_t align);
void kasync_free(void *p);
void kasync_arm_timer(uint32_t ms);

static struct waitqueue kasync_wq = WAITQUEUE_INIT(kasync_wq);
static uint32_t kasync_pending; /* a wake not yet seen by an idle: kept, so none is lost */
static void *kasync_slot;
static struct thread *kasync_owner;
static struct timer kasync_timer;

uint64_t ceesafe_env_now_ms(void)
{
    return clock_now_ns() / 1000000u;
}

void ceesafe_env_idle(void *executor, uint64_t deadline_ms)
{
    (void)executor;
    if (deadline_ms == UINT64_MAX) {
        wait_event(&kasync_wq, __atomic_load_n(&kasync_pending, __ATOMIC_ACQUIRE) != 0);
    } else {
        uint64_t now = ceesafe_env_now_ms();
        if (deadline_ms > now)
            (void)wait_event_timeout(&kasync_wq, __atomic_load_n(&kasync_pending, __ATOMIC_ACQUIRE) != 0,
                                     (deadline_ms - now) * 1000000u);
    }
    __atomic_store_n(&kasync_pending, 0u, __ATOMIC_RELEASE);
}

/* Any context: a wait queue's wakers are safe in interrupt context (wait.h). */
void ceesafe_env_wake(void *executor)
{
    (void)executor;
    __atomic_store_n(&kasync_pending, 1u, __ATOMIC_RELEASE);
    (void)waitqueue_wake_all(&kasync_wq);
}

void **ceesafe_env_slot(void)
{
    struct thread *me = thread_current();

    if (kasync_slot != NULL && kasync_owner != me)
        panic("cee-async: a second thread runs CeeSafe tasks while one does (one slot, async_selftest.c)");
    kasync_owner = me;
    return &kasync_slot;
}

void *kasync_alloc(size_t size, size_t align)
{
    void *p = align <= KMALLOC_MIN_ALIGN ? kmalloc(size, 0) : NULL;
    if (p == NULL)
        kprintf("cee-async: allocation of %zu bytes aligned %zu failed\n", size, align);
    return p;
}

void kasync_free(void *p)
{
    kfree(p);
}

static void kasync_fired(struct timer *t, void *arg)
{
    (void)t;
    (void)arg;
    kasync_raise(); /* in interrupt context */
}

void kasync_arm_timer(uint32_t ms)
{
    timer_start(&kasync_timer, (uint64_t)ms * 1000000u);
}

static const char *const kasync_failures[] = {
    [1] = "a scope's two children did not both return their values",
    [2] = "a race did not return the faster child",
    [3] = "a sibling's error did not cancel a sleeping child",
    [4] = "a task returned an error (cancelled or timed out)",
    [5] = "a scope's child frame could not be allocated (kmalloc)",
    [6] = "a scope's child returned an error",
    [7] = "the wait on the timer's AsyncSignal returned an error",
};

bool selftest_cee_async(const char **reason)
{
    timer_setup(&kasync_timer, kasync_fired, NULL);
    uint32_t failed = kasync_cee_selftest();

    (void)timer_cancel_sync(&kasync_timer);
    if (failed == 0)
        return true;
    *reason = failed < sizeof(kasync_failures) / sizeof(kasync_failures[0]) && kasync_failures[failed]
                  ? kasync_failures[failed]
                  : "an unknown check failed";
    return false;
}
