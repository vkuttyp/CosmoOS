/*
 * eventfd.c - an eventfd counter as an I/O object (eventfd(2)).
 *
 * A kobject carrying a uint64 counter and a wait queue. A write adds its
 * 8-byte value to the counter and wakes readers; a read returns the count and
 * resets it (or returns 1 and decrements, in semaphore mode) and wakes
 * writers; both block (or return -EAGAIN) when they cannot proceed. Because it
 * is a kobject with read/write/ready/poll_wq, it serves poll/select, blocking
 * read/write and the I/O ring with no new op -- the same template as the
 * timer object (kernel/io/timerobj.c). See docs/audit/next-subsystem-eventfd.md.
 *
 * Lifetime: a plain kobject. A parked I/O-ring entry or a blocked waiter holds
 * a reference, so `release` (a plain free -- no timer, unlike timerobj) runs
 * only once the last reference and handle are gone.
 */

#include <kernel/compiler.h>
#include <kernel/errno.h>
#include <kernel/eventfd.h>
#include <kernel/kmalloc.h>
#include <kernel/object.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/wait.h>

#include <uapi/cosmo/syscall.h>

#include <stdint.h>

#define EVENTFD_MAX (UINT64_MAX - 1)   /* the largest count a write may leave (Linux) */

struct eventfd_obj {
    struct kobject obj;
    struct waitqueue wq;
    spinlock_t lock;          /* count, nonblock */
    uint64_t count;
    bool semaphore;           /* read returns 1 and decrements, rather than the whole count */
    bool nonblock;            /* the object's non-blocking mode (set_nonblock) */
};

static struct eventfd_obj *eventfd_of(struct kobject *obj)
{
    return container_of(obj, struct eventfd_obj, obj);
}

static unsigned eventfd_ready(struct kobject *obj)
{
    struct eventfd_obj *e = eventfd_of(obj);
    arch_irq_state_t s = spin_lock_irqsave(&e->lock);
    unsigned r = 0;
    if (e->count > 0)
        r |= COSMO_IO_READABLE;
    if (e->count < EVENTFD_MAX)
        r |= COSMO_IO_WRITABLE;
    spin_unlock_irqrestore(&e->lock, s);
    return r;
}

static struct waitqueue *eventfd_poll_wq(struct kobject *obj, unsigned events)
{
    (void)events;   /* one queue for readable and writable; waiters re-check */
    return &eventfd_of(obj)->wq;
}

static int64_t eventfd_read(struct kobject *obj, void *buf, size_t len)
{
    struct eventfd_obj *e = eventfd_of(obj);
    if (len < sizeof(uint64_t))
        return -EINVAL;
    bool nb = io_nonblocking(e->nonblock);
    for (;;) {
        arch_irq_state_t s = spin_lock_irqsave(&e->lock);
        uint64_t v = 0;
        if (e->count != 0) {
            if (e->semaphore) {
                v = 1;
                e->count -= 1;
            } else {
                v = e->count;
                e->count = 0;
            }
        }
        spin_unlock_irqrestore(&e->lock, s);
        if (v != 0) {
            memcpy(buf, &v, sizeof(v));
            waitqueue_wake_all(&e->wq);   /* the count dropped: a blocked writer may now have room */
            return (int64_t)sizeof(v);
        }
        if (nb)
            return -EAGAIN;
        int rc = wait_event_killable(&e->wq, e->count != 0);
        if (rc)
            return rc;
    }
}

static int64_t eventfd_write(struct kobject *obj, const void *buf, size_t len)
{
    struct eventfd_obj *e = eventfd_of(obj);
    if (len < sizeof(uint64_t))
        return -EINVAL;
    uint64_t add;
    memcpy(&add, buf, sizeof(add));
    if (add == UINT64_MAX)
        return -EINVAL;   /* Linux reserves the all-ones value */
    bool nb = io_nonblocking(e->nonblock);
    for (;;) {
        arch_irq_state_t s = spin_lock_irqsave(&e->lock);
        if (e->count <= EVENTFD_MAX - add) {   /* room: count + add stays <= EVENTFD_MAX */
            e->count += add;
            spin_unlock_irqrestore(&e->lock, s);
            waitqueue_wake_all(&e->wq);   /* the count rose: a blocked reader may now proceed */
            return (int64_t)sizeof(add);
        }
        spin_unlock_irqrestore(&e->lock, s);
        if (nb)
            return -EAGAIN;
        int rc = wait_event_killable(&e->wq, e->count <= EVENTFD_MAX - add);
        if (rc)
            return rc;
    }
}

static int eventfd_set_nonblock(struct kobject *obj, int on)
{
    struct eventfd_obj *e = eventfd_of(obj);
    arch_irq_state_t s = spin_lock_irqsave(&e->lock);
    int prev = e->nonblock ? 1 : 0;
    if (on == 0 || on == 1)
        e->nonblock = on;
    spin_unlock_irqrestore(&e->lock, s);
    return prev;
}

static void eventfd_release(struct kobject *obj)
{
    kfree(eventfd_of(obj));   /* no timer to cancel, unlike timerobj */
}

static const struct kobject_io_type eventfd_type = {
    .base = { .name = "eventfd", .release = eventfd_release, .flags = KOBJECT_TYPE_IO },
    .read = eventfd_read,
    .write = eventfd_write,
    .ready = eventfd_ready,
    .set_nonblock = eventfd_set_nonblock,
    .poll_wq = eventfd_poll_wq,
};

int eventfd_obj_create(uint64_t initval, bool semaphore, struct kobject **out)
{
    struct eventfd_obj *e = kzalloc(sizeof(*e));
    if (e == NULL)
        return -ENOMEM;
    kobject_init(&e->obj, &eventfd_type.base);
    spinlock_init(&e->lock, "eventfd");
    waitqueue_init(&e->wq, "eventfd");
    e->count = initval;
    e->semaphore = semaphore;
    *out = &e->obj;
    return 0;
}
