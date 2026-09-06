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

static void alarm_fired(struct timer *t, void *arg)
{
    (void)t;
    struct poll_alarm *a = arg;
    __atomic_store_n(&a->fired, true, __ATOMIC_RELEASE);
    sched_wake(a->thread);
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
        timer_cancel(&timer);
    kfree(we);
    kfree(wqs);
    return rc;
}
