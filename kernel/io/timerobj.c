/*
 * timerobj.c - a timer as a submittable I/O object (a timerfd).
 *
 * The object owns a kernel timer. Each expiry bumps a count and wakes the
 * object's wait queue; if an interval was set, the callback re-arms. A read
 * returns the count since the last read and resets it, blocking until the
 * first expiry unless the caller (or the I/O ring) asked not to. Because it
 * is a kobject with `ready`/`poll_wq`/`read`, it is submittable to the ring
 * as POLL or READ with no new op, and serves poll/select and a blocking read
 * for free.
 *
 * Lifetime: `release` marks the object dying so the callback cannot re-arm,
 * then `timer_cancel_sync` cancels the pending timer and waits out a callback
 * in flight before the object is freed -- the tcp-pcb-timer-free hazard
 * (docs/audit/next-subsystem-lifetime-windows.md). While an entry is parked
 * in a ring the ring holds a reference, so the object cannot be freed under
 * it (docs/audit/next-subsystem-aio-timer.md).
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/object.h>
#include <kernel/compiler.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/timerobj.h>
#include <kernel/wait.h>

#include <uapi/cosmo/syscall.h>

struct timer_obj {
    struct kobject obj;
    struct timer timer;
    struct waitqueue wq;
    spinlock_t lock;          /* count, interval_ns, deadline_ns, armed, dying */
    uint64_t count;           /* expirations since the last read */
    uint64_t interval_ns;     /* 0 = one-shot */
    uint64_t deadline_ns;     /* monotonic time of the next expiry while armed */
    bool armed;               /* a timer is pending (settime/gettime remaining) */
    bool dying;               /* release in progress: the callback must not re-arm */
    bool nonblock;            /* the object's non-blocking mode (set_nonblock) */
    bool realtime;            /* the fd's clock is the wall clock (timerfd abs settime) */
};

static struct timer_obj *timer_obj_of(struct kobject *obj)
{
    return container_of(obj, struct timer_obj, obj);
}

static void timer_obj_fired(struct timer *t, void *arg)
{
    (void)t;
    struct timer_obj *to = arg;
    arch_irq_state_t s = spin_lock_irqsave(&to->lock);
    if (to->dying) {                     /* being released: do not touch a dying object */
        spin_unlock_irqrestore(&to->lock, s);
        return;
    }
    to->count++;
    if (to->interval_ns) {
        /* Re-arm from the callback, and record the same now+interval the
         * timer is armed for, so gettime's remaining cannot drift from it. */
        to->deadline_ns = clock_now_ns() + to->interval_ns;
        timer_start(&to->timer, to->interval_ns);
    } else {
        to->armed = false;   /* a one-shot has fired: nothing pending */
    }
    spin_unlock_irqrestore(&to->lock, s);
    waitqueue_wake_all(&to->wq);
}

static unsigned timer_obj_ready(struct kobject *obj)
{
    struct timer_obj *to = timer_obj_of(obj);
    arch_irq_state_t s = spin_lock_irqsave(&to->lock);
    unsigned r = to->count != 0 ? COSMO_IO_READABLE : 0;
    spin_unlock_irqrestore(&to->lock, s);
    return r;
}

static struct waitqueue *timer_obj_poll_wq(struct kobject *obj, unsigned events)
{
    (void)events;   /* a timer only ever becomes readable */
    return &timer_obj_of(obj)->wq;
}

static int64_t timer_obj_read(struct kobject *obj, void *buf, size_t len)
{
    struct timer_obj *to = timer_obj_of(obj);
    if (len < sizeof(uint64_t))
        return -EINVAL;
    bool nb = io_nonblocking(to->nonblock);
    for (;;) {
        arch_irq_state_t s = spin_lock_irqsave(&to->lock);
        uint64_t c = to->count;
        to->count = 0;
        spin_unlock_irqrestore(&to->lock, s);
        if (c != 0) {
            memcpy(buf, &c, sizeof(c));
            return (int64_t)sizeof(c);
        }
        if (nb)
            return -EAGAIN;
        int rc = wait_event_killable(&to->wq, to->count != 0);
        if (rc)
            return rc;
    }
}

static int timer_obj_set_nonblock(struct kobject *obj, int on)
{
    struct timer_obj *to = timer_obj_of(obj);
    arch_irq_state_t s = spin_lock_irqsave(&to->lock);
    int prev = to->nonblock ? 1 : 0;
    if (on == 0 || on == 1)
        to->nonblock = on;
    spin_unlock_irqrestore(&to->lock, s);
    return prev;
}

static void timer_obj_release(struct kobject *obj)
{
    struct timer_obj *to = timer_obj_of(obj);
    /* Stop the callback re-arming, then cancel and wait out one in flight:
     * after this the timer is disarmed and its callback is not running, so
     * the object can be freed without a timer firing into freed memory. */
    arch_irq_state_t s = spin_lock_irqsave(&to->lock);
    to->dying = true;
    spin_unlock_irqrestore(&to->lock, s);
    timer_cancel_sync(&to->timer);
    kfree(to);
}

static const struct kobject_io_type timer_type = {
    .base = { .name = "timer", .release = timer_obj_release, .flags = KOBJECT_TYPE_IO },
    .read = timer_obj_read,
    .ready = timer_obj_ready,
    .set_nonblock = timer_obj_set_nonblock,
    .poll_wq = timer_obj_poll_wq,
};

int timer_obj_create(uint64_t initial_ns, uint64_t interval_ns, struct kobject **out)
{
    if (initial_ns == 0)
        return -EINVAL;   /* a timer must be armed; re-arm at create time */
    struct timer_obj *to = kzalloc(sizeof(*to));
    if (to == NULL)
        return -ENOMEM;
    kobject_init(&to->obj, &timer_type.base);
    spinlock_init(&to->lock, "timer");
    waitqueue_init(&to->wq, "timer");
    to->interval_ns = interval_ns;
    timer_setup(&to->timer, timer_obj_fired, to);
    to->deadline_ns = clock_now_ns() + initial_ns;
    to->armed = true;
    timer_start(&to->timer, initial_ns);
    *out = &to->obj;
    return 0;
}

int timer_obj_create_disarmed(bool nonblock, bool realtime, struct kobject **out)
{
    struct timer_obj *to = kzalloc(sizeof(*to));
    if (to == NULL)
        return -ENOMEM;
    kobject_init(&to->obj, &timer_type.base);
    spinlock_init(&to->lock, "timer");
    waitqueue_init(&to->wq, "timer");
    to->nonblock = nonblock;
    to->realtime = realtime;
    timer_setup(&to->timer, timer_obj_fired, to);   /* set up but not started */
    *out = &to->obj;
    return 0;
}

void timer_obj_settime(struct kobject *obj, uint64_t initial_ns, uint64_t interval_ns,
                       uint64_t *old_remaining_ns, uint64_t *old_interval_ns)
{
    struct timer_obj *to = timer_obj_of(obj);
    /* Cancel any pending expiry *before* taking the lock: the fire callback
     * takes this same lock, so cancelling under it would deadlock (the
     * release path cancels outside the lock for the same reason). After this
     * the callback is neither pending nor running, so nothing re-arms the
     * timer until the timer_start below. */
    timer_cancel_sync(&to->timer);
    arch_irq_state_t s = spin_lock_irqsave(&to->lock);
    uint64_t now = clock_now_ns();
    if (old_remaining_ns)
        *old_remaining_ns = (to->armed && to->deadline_ns > now) ? to->deadline_ns - now : 0;
    if (old_interval_ns)
        *old_interval_ns = to->interval_ns;
    to->count = 0;                 /* settime resets the expiration count (Linux) */
    if (initial_ns != 0) {
        to->interval_ns = interval_ns;
        to->deadline_ns = now + initial_ns;
        to->armed = true;
        timer_start(&to->timer, initial_ns);
    } else {
        to->interval_ns = 0;       /* disarmed: no interval pending */
        to->armed = false;
    }
    spin_unlock_irqrestore(&to->lock, s);
}

void timer_obj_gettime(struct kobject *obj, uint64_t *remaining_ns, uint64_t *interval_ns)
{
    struct timer_obj *to = timer_obj_of(obj);
    arch_irq_state_t s = spin_lock_irqsave(&to->lock);
    uint64_t now = clock_now_ns();
    *remaining_ns = (to->armed && to->deadline_ns > now) ? to->deadline_ns - now : 0;
    *interval_ns = to->interval_ns;
    spin_unlock_irqrestore(&to->lock, s);
}

bool timer_obj_is_realtime(struct kobject *obj)
{
    return timer_obj_of(obj)->realtime;
}

struct kobject *timer_obj_from_kobject(struct kobject *obj)
{
    return (obj != NULL && obj->type == &timer_type.base) ? obj : NULL;
}
