/*
 * wait_floor.c - The C under kernel/scheduler/wait.nova (roadmap 2.0 SA3).
 *
 * The wait queues and their types are Nova (wait.nova, generated into
 * wait.c and kernel/include/kernel/wait_types.h). What stays here reads
 * fields of struct thread and struct percpu, whose headers include wait.h
 * (so wait.nova cannot name them: the generated header would include them
 * back), formats panic's message, and keeps the two timed sleeps, which
 * are C over the wait_event macros. Each is a few lines and no decision.
 */

#include <kernel/lockdep.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/sched.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/wait.h>


/* Declared for wait.nova only; not part of wait.h's interface. */
struct thread *wait_floor_enter_sleep(uintptr_t ip);
void wait_floor_set_waiting_on(struct thread *t, struct waitqueue *wq);
void wait_floor_fatal(const char *what);

/* waitqueue_prepare's entry: never in an interrupt, never where the caller
 * may not sleep (might_sleep, with the wait's call site, which
 * waitqueue_prepare passes as its caller's address), and the thread. */
struct thread *wait_floor_enter_sleep(uintptr_t ip)
{
    struct percpu *pc = raw_this_cpu();   /* identity: irq_depth and the thread are the same wherever it runs */
    if (pc->irq_depth != 0)
        panic("wait_event in interrupt context");
    if (pc->preempt_count != 0 || pc->irq_depth != 0) {   /* might_sleep, with the caller's address */
        if (CONFIG_LOCKDEP)
            lockdep_might_sleep(ip);
        else
            panic("sleeping call with preemption disabled (count %d) or in interrupt (depth %u)", pc->preempt_count,
                  pc->irq_depth);
    }
    return pc->current;
}

void wait_floor_set_waiting_on(struct thread *t, struct waitqueue *wq)
{
    t->waiting_on = wq;
}

void wait_floor_fatal(const char *what)
{
    panic("%s", what);
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
#if CONFIG_DEBUG
    uint64_t t0 = clock_now_ns();
#endif
    timer_start(&t, ns);
    wait_event(&s.wq, __atomic_load_n(&s.done, __ATOMIC_ACQUIRE));
    /* The callback has run (done is set after nothing else), the timer
     * queue does not touch the timer after its callback (run_expired),
     * so the stack objects may go -- even though this thread may be
     * running on another CPU than the tick that fired it by now. */
#if CONFIG_DEBUG
    /* The overshoot detector: a sleep that returns more than a second
     * late is named, with the CPU whose queue held its timer and the CPU
     * it woke on (a diagnostic: both reads raw). */
    uint64_t took = clock_since_ns(t0);
    if (took > ns + NS_PER_SEC)
        kwarn("sched: sleep of %llu ms took %llu ms: '%s' armed on cpu %u, woke on cpu %u (t->cpu %d)",
              (unsigned long long)(ns / 1000000), (unsigned long long)(took / 1000000), thread_current()->name,
              t.cpu, raw_cpu_id(), thread_current()->cpu);
#endif
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
