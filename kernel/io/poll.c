/*
 * poll.c - io_poll (docs/kernel/io/design.md, "Polling").
 *
 * The same shape as the AIO ring's wait (aio.c): arm a wait entry on every
 * object's poll_wq, re-evaluate readiness, sleep only if nothing is ready
 * and nothing woke us in between (waitqueue_prepare marks the thread
 * BLOCKED before the check, so a wake between the check and the block is
 * not lost), then disarm. A timer wakes the thread when the timeout ends;
 * a kill or a deliverable signal makes the wait -EINTR.
 */

#include <arch/cpu.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/object.h>
#include <kernel/poll.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/wait.h>

struct poll_alarm {
    struct thread *thread;
    bool fired;
};

#if CONFIG_SELFTEST
/* poll-timer-outlives' seam: a held callback waits, after marking the alarm
 * fired, until the test lets it go, so the poller can return on another CPU
 * meanwhile. Unarmed, the callback reads one word and nothing more. */
static int g_alarm_hold;
static unsigned g_alarm_in, g_alarm_done;

void poll_test_alarm_hold(bool on)
{
    if (on) {
        __atomic_store_n(&g_alarm_in, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_alarm_done, 0, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&g_alarm_hold, on ? 1 : 0, __ATOMIC_RELEASE);
}
unsigned poll_test_alarm_in(void) { return __atomic_load_n(&g_alarm_in, __ATOMIC_ACQUIRE); }
unsigned poll_test_alarm_done(void) { return __atomic_load_n(&g_alarm_done, __ATOMIC_ACQUIRE); }
#endif

static void alarm_fired(struct timer *t, void *arg)
{
    (void)t;
    struct poll_alarm *a = arg;
    __atomic_store_n(&a->fired, true, __ATOMIC_RELEASE);
#if CONFIG_SELFTEST
    bool held = __atomic_load_n(&g_alarm_hold, __ATOMIC_ACQUIRE) != 0;
    if (held) {
        __atomic_fetch_add(&g_alarm_in, 1u, __ATOMIC_RELEASE);
        while (__atomic_load_n(&g_alarm_hold, __ATOMIC_ACQUIRE))
            arch_cpu_relax();
    }
#endif
    sched_wake(a->thread);   /* `a` is the poller's stack: io_poll waits for this callback before it returns */
#if CONFIG_SELFTEST
    if (held)
        __atomic_fetch_add(&g_alarm_done, 1u, __ATOMIC_RELEASE);
#endif
}

/* revents for every entry; the count of entries with something set. */
static int evaluate(struct io_pollfd *fds, unsigned n)
{
    int count = 0;
    for (unsigned i = 0; i < n; i++) {
        fds[i].revents = 0;
        if (fds[i].obj == NULL)
            continue;
        unsigned ready = kobject_ready(fds[i].obj);
        fds[i].revents = (ready & fds[i].events) | (ready & (COSMO_IO_HANGUP | COSMO_IO_ERROR));
        if (fds[i].revents)
            count++;
    }
    return count;
}

int io_poll(struct io_pollfd *fds, unsigned n, uint64_t timeout_ns)
{
    int count = evaluate(fds, n);
    if (count > 0 || timeout_ns == 0)
        return count;

    struct wait_entry *we = NULL;
    struct waitqueue **wqs = NULL;
    if (n) {
        we = kmalloc(n * sizeof(*we), 0);
        wqs = kmalloc(n * sizeof(*wqs), 0);
        if (we == NULL || wqs == NULL) {
            kfree(we);
            kfree(wqs);
            return -ENOMEM;
        }
    }
    for (unsigned i = 0; i < n; i++) {
        wait_entry_init(&we[i]);
        wqs[i] = fds[i].obj ? kobject_poll_wq(fds[i].obj, fds[i].events | COSMO_IO_HANGUP | COSMO_IO_ERROR) : NULL;
    }
    struct poll_alarm alarm = { .thread = thread_current(), .fired = false };
    struct timer timer;
    bool armed = false;
    if (timeout_ns != IO_POLL_FOREVER) {
        timer_setup(&timer, alarm_fired, &alarm);
        timer_start(&timer, timeout_ns);
        armed = true;
    }
    int rc = 0;
    for (;;) {
        /* Arm every wake source, then decide. */
        for (unsigned i = 0; i < n; i++)
            if (wqs[i])
                waitqueue_prepare(wqs[i], &we[i]);
        count = evaluate(fds, n);
        bool fired = __atomic_load_n(&alarm.fired, __ATOMIC_ACQUIRE);
        bool interrupted = process_kill_pending();
        if (count == 0 && !fired && !interrupted)
            sched_block_current();
        for (unsigned i = 0; i < n; i++)
            if (wqs[i])
                waitqueue_finish(wqs[i], &we[i]);
        if (count > 0) {
            rc = count;
            break;
        }
        if (fired) {
            rc = 0;
            break;
        }
        if (interrupted) {
            rc = -EINTR;
            break;
        }
    }
    if (armed)
        timer_cancel_sync(&timer);   /* the timer and `alarm` are this frame's: wait out a callback running on
                                        the CPU that armed it before they go (T14; epoll and aio already did) */
    kfree(we);
    kfree(wqs);
    return rc;
}
