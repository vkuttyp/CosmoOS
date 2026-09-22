/*
 * wait.c - Wait queues and timed sleep.
 */

#include <kernel/lockdep.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/sched.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/wait.h>

#include "sched_internal.h"

void waitqueue_init(struct waitqueue *wq, const char *name)
{
    spinlock_init(&wq->lock, name);
    list_init(&wq->waiters);
}

void waitqueue_prepare(struct waitqueue *wq, struct wait_entry *e)
{
    struct percpu *pc = raw_this_cpu();   /* identity: irq_depth and the thread are the same wherever it runs */
    if (pc->irq_depth != 0)
        panic("wait_event in interrupt context");
    might_sleep();

    struct thread *cur = pc->current;
    e->thread = cur;

    arch_irq_state_t s = spin_lock_irqsave(&wq->lock);
    /* A woken waiter re-preparing after a false condition is still
     * linked; pushing it again would corrupt the list. */
    if (list_empty(&e->link))
        list_push_back(&wq->waiters, &e->link);
    cur->waiting_on = wq;
    /* Under the wait-queue lock so a concurrent waker sees either the
     * BLOCKED state and enqueues us, or runs after we finish. */
    cur->state = THREAD_BLOCKED;
    spin_unlock_irqrestore(&wq->lock, s);
}

void waitqueue_finish(struct waitqueue *wq, struct wait_entry *e)
{
    arch_irq_state_t s = spin_lock_irqsave(&wq->lock);
    if (!list_empty(&e->link))
        list_remove(&e->link);
    e->thread->waiting_on = NULL;
    spin_unlock_irqrestore(&wq->lock, s);

    sched_set_running_current();
}

/*
 * A waiter that was already woken stays linked until it runs and calls
 * waitqueue_finish. wake_one must not stop at such an entry: it counts
 * only waiters it actually transitioned, and keeps scanning past ones
 * that are already READY or RUNNING, so consecutive wake_one calls reach
 * consecutive blocked waiters.
 */
static unsigned wake(struct waitqueue *wq, bool all)
{
    unsigned n = 0;
    arch_irq_state_t s = spin_lock_irqsave(&wq->lock);
    struct wait_entry *e, *tmp;
    list_for_each_entry_safe(e, tmp, &wq->waiters, link) {
        if (!sched_wake(e->thread))
            continue;
        n++;
        if (!all)
            break;
    }
    spin_unlock_irqrestore(&wq->lock, s);
    return n;
}

unsigned waitqueue_wake_one(struct waitqueue *wq)
{
    return wake(wq, false);
}

unsigned waitqueue_wake_all(struct waitqueue *wq)
{
    return wake(wq, true);
}

bool waitqueue_empty(struct waitqueue *wq)
{
    arch_irq_state_t s = spin_lock_irqsave(&wq->lock);
    bool empty = list_empty(&wq->waiters);
    spin_unlock_irqrestore(&wq->lock, s);
    return empty;
}

/* --- the timed wait --- */

void wait_timeout_init(struct wait_timeout *wt, struct waitqueue *wq)
{
    wt->wq = wq;
    wt->expired = false;
}

/* Interrupt context: the timer's callback. Waking is allowed here
 * (wait.h, "Wakers are interrupt-safe"), and the waiter re-tests its own
 * condition either way -- this only ends the block, it decides nothing. */
void wait_timeout_fired(struct timer *t, void *arg)
{
    (void)t;
    struct wait_timeout *wt = arg;
    __atomic_store_n(&wt->expired, true, __ATOMIC_RELEASE);
    waitqueue_wake_all(wt->wq);
}

bool wait_timeout_expired(const struct wait_timeout *wt)
{
    return __atomic_load_n(&wt->expired, __ATOMIC_ACQUIRE);
}

/* --- sleep --- */

struct sleeper {
    struct waitqueue wq;
    bool done;
};

static void sleep_fired(struct timer *t, void *arg)
{
    (void)t;
    struct sleeper *s = arg;
    __atomic_store_n(&s->done, true, __ATOMIC_RELEASE);
    waitqueue_wake_all(&s->wq);
}

void thread_sleep_ns(uint64_t ns)
{
    struct sleeper s;
    struct timer t;

    waitqueue_init(&s.wq, "sleep");
    s.done = false;
    timer_setup(&t, sleep_fired, &s);
    timer_start(&t, ns);
    wait_event(&s.wq, __atomic_load_n(&s.done, __ATOMIC_ACQUIRE));
    /* The callback has run (done is set after nothing else), so the
     * timer is idle and the stack objects may go. */
}

int thread_sleep_ns_killable(uint64_t ns)
{
    struct sleeper s;
    struct timer t;

    waitqueue_init(&s.wq, "sleep");
    s.done = false;
    timer_setup(&t, sleep_fired, &s);
    timer_start(&t, ns);
    int rc = wait_event_killable(&s.wq, __atomic_load_n(&s.done, __ATOMIC_ACQUIRE));
    if (rc) {
        /* Woken by a kill: the timer may still be armed on our stack --
         * and plain timer_cancel only promises the callback will not
         * START (timer.h). One already running on another CPU would
         * touch `s` and `t` after this frame is gone, so the wait is the
         * synchronous one. */
        timer_cancel_sync(&t);
    }
    return rc;
}

/* Module ABI v1 exports (docs/kernel/module/api.md). */
#include <kernel/module.h>
EXPORT_SYMBOL(thread_sleep_ns);
EXPORT_SYMBOL(waitqueue_init);
EXPORT_SYMBOL(waitqueue_prepare);
EXPORT_SYMBOL(waitqueue_finish);
EXPORT_SYMBOL(waitqueue_wake_all);
