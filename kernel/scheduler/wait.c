/*
 * wait.c - Wait queues and timed sleep.
 */

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
    struct percpu *pc = this_cpu();
    if (pc->irq_depth != 0)
        panic("wait_event in interrupt context");
    if (pc->preempt_count != 0)
        panic("wait_event with preemption disabled (count %d)", pc->preempt_count);

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

static unsigned wake(struct waitqueue *wq, bool all)
{
    unsigned n = 0;
    arch_irq_state_t s = spin_lock_irqsave(&wq->lock);
    struct wait_entry *e, *tmp;
    list_for_each_entry_safe(e, tmp, &wq->waiters, link) {
        sched_wake(e->thread);
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
