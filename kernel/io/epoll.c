/*
 * epoll.c - an interest set of I/O objects waited on together (epoll).
 *
 * The object is a kobject holding a list of (fd, object) registrations. Each
 * carries a wanted COSMO_IO_* mask and an opaque data token. epoll_obj_wait is
 * the aio ring's multi-wait (kernel/io/aio.c): it arms a wait entry on every
 * member's poll_wq and on the set's own queue, evaluates each member's
 * readiness (poll.c's rule), and sleeps only if none is ready and the deadline
 * has not passed; a finite timeout wakes the waiting thread directly. A ctl
 * that adds or re-arms a member wakes the set's queue so a concurrent waiter
 * re-evaluates. Level-triggered; one-shot supported. See
 * docs/audit/next-subsystem-epoll.md.
 *
 * Lifetime: each registration holds a reference to its member object, dropped
 * on EPOLL_CTL_DEL and when the epoll is released (which walks the list). v1
 * has no auto-remove on the member's close: a registered fd must be removed
 * with EPOLL_CTL_DEL.
 */

#include <kernel/compiler.h>
#include <kernel/epoll.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/list.h>
#include <kernel/mutex.h>
#include <kernel/object.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/wait.h>

#include <uapi/cosmo/syscall.h>

#define EPOLL_WANT_ALL(want) ((want) | COSMO_IO_HANGUP | COSMO_IO_ERROR)

struct epoll_item {
    struct kobject *obj;      /* the member, referenced */
    int fd;                   /* the key */
    unsigned want;            /* COSMO_IO_* requested */
    uint64_t data;            /* opaque token, echoed to the waiter */
    bool oneshot;             /* disable after one report, until MOD re-arms */
    bool disabled;            /* a fired one-shot, until MOD */
    struct wait_entry we;     /* queued on `wq` while a waiter sleeps */
    struct waitqueue *wq;     /* kobject_poll_wq(obj, want|HANGUP|ERROR), or NULL */
    bool prepared;            /* `we` is queued on `wq` right now */
    struct list_node link;
};

struct epoll_obj {
    struct kobject obj;
    struct mutex lock;        /* the item list */
    struct waitqueue wait;    /* the set's own queue: ctl wakes it, wait sleeps on it */
    struct list_node items;
};

static void epoll_release(struct kobject *obj);
static unsigned epoll_ready(struct kobject *obj);
static struct waitqueue *epoll_poll_wq(struct kobject *obj, unsigned events);

static const struct kobject_io_type epoll_type = {
    .base = { .name = "epoll", .release = epoll_release, .flags = KOBJECT_TYPE_IO },
    .ready = epoll_ready,
    .poll_wq = epoll_poll_wq,
};

static struct epoll_obj *epoll_of(struct kobject *obj)
{
    return container_of(obj, struct epoll_obj, obj);
}

struct kobject *epoll_obj_from_kobject(struct kobject *obj)
{
    return (obj != NULL && obj->type == &epoll_type.base) ? obj : NULL;
}

int epoll_obj_create(struct kobject **out)
{
    struct epoll_obj *ep = kzalloc(sizeof(*ep));
    if (ep == NULL)
        return -ENOMEM;
    kobject_init(&ep->obj, &epoll_type.base);
    mutex_init(&ep->lock, "epoll");
    waitqueue_init(&ep->wait, "epoll");
    list_init(&ep->items);
    *out = &ep->obj;
    return 0;
}

static void epoll_release(struct kobject *obj)
{
    struct epoll_obj *ep = epoll_of(obj);
    struct epoll_item *it, *tmp;
    list_for_each_entry_safe(it, tmp, &ep->items, link) {
        list_remove(&it->link);
        kobject_put(it->obj);
        kfree(it);
    }
    kfree(ep);
}

/* Lock held. */
static struct epoll_item *find_item(struct epoll_obj *ep, int fd)
{
    struct epoll_item *it;
    list_for_each_entry(it, &ep->items, link)
        if (it->fd == fd)
            return it;
    return NULL;
}

/* The readiness a member would report now (0 if disabled). Lock held. */
static unsigned item_ready(const struct epoll_item *it)
{
    if (it->disabled)
        return 0;
    return kobject_ready(it->obj) & EPOLL_WANT_ALL(it->want);
}

/* Lock held. Any member ready? */
static bool any_ready(struct epoll_obj *ep)
{
    struct epoll_item *it;
    list_for_each_entry(it, &ep->items, link)
        if (item_ready(it))
            return true;
    return false;
}

/* Lock held. Fill up to `max` ready members, disabling one-shots that fire. */
static unsigned collect(struct epoll_obj *ep, struct epoll_ready *out, unsigned max)
{
    unsigned n = 0;
    struct epoll_item *it;
    list_for_each_entry(it, &ep->items, link) {
        if (n >= max)
            break;
        unsigned ev = item_ready(it);
        if (ev == 0)
            continue;
        out[n].events = ev;
        out[n].data = it->data;
        n++;
        if (it->oneshot)
            it->disabled = true;   /* reported once; re-armed by MOD */
    }
    return n;
}

int epoll_obj_add(struct kobject *epobj, int fd, struct kobject *target,
                  unsigned want, uint64_t data, bool oneshot)
{
    struct epoll_obj *ep = epoll_of(epobj);
    struct epoll_item *it = kzalloc(sizeof(*it));
    if (it == NULL)
        return -ENOMEM;
    mutex_lock(&ep->lock);
    if (find_item(ep, fd) != NULL) {
        mutex_unlock(&ep->lock);
        kfree(it);
        return -EEXIST;
    }
    it->obj = target;          /* takes ownership of the caller's reference */
    it->fd = fd;
    it->want = want;
    it->data = data;
    it->oneshot = oneshot;
    it->disabled = false;
    wait_entry_init(&it->we);
    it->wq = kobject_poll_wq(target, EPOLL_WANT_ALL(want));
    list_push_back(&ep->items, &it->link);
    /* A concurrent epoll_wait must re-evaluate the new member (as aio_submit
     * wakes the ring's queue after parking an entry). */
    waitqueue_wake_all(&ep->wait);
    mutex_unlock(&ep->lock);
    return 0;
}

int epoll_obj_mod(struct kobject *epobj, int fd, unsigned want, uint64_t data, bool oneshot)
{
    struct epoll_obj *ep = epoll_of(epobj);
    mutex_lock(&ep->lock);
    struct epoll_item *it = find_item(ep, fd);
    if (it == NULL) {
        mutex_unlock(&ep->lock);
        return -ENOENT;
    }
    it->want = want;
    it->data = data;
    it->oneshot = oneshot;
    it->disabled = false;      /* MOD re-arms a fired one-shot */
    it->wq = kobject_poll_wq(it->obj, EPOLL_WANT_ALL(want));
    waitqueue_wake_all(&ep->wait);   /* a widened mask or re-arm can make it reportable */
    mutex_unlock(&ep->lock);
    return 0;
}

int epoll_obj_del(struct kobject *epobj, int fd)
{
    struct epoll_obj *ep = epoll_of(epobj);
    mutex_lock(&ep->lock);
    struct epoll_item *it = find_item(ep, fd);
    if (it == NULL) {
        mutex_unlock(&ep->lock);
        return -ENOENT;
    }
    list_remove(&it->link);
    mutex_unlock(&ep->lock);
    kobject_put(it->obj);
    kfree(it);
    return 0;
}

struct epoll_alarm {
    struct thread *thread;
    volatile bool fired;
};

static void alarm_fired(struct timer *t, void *arg)
{
    (void)t;
    struct epoll_alarm *al = arg;
    __atomic_store_n(&al->fired, true, __ATOMIC_RELEASE);
    sched_wake(al->thread);
}

int64_t epoll_obj_wait(struct kobject *epobj, struct epoll_ready *out, unsigned max, uint64_t timeout_ns)
{
    struct epoll_obj *ep = epoll_of(epobj);
    struct epoll_alarm alarm = { .thread = thread_current(), .fired = false };
    struct timer timer;
    bool armed = false;
    if (timeout_ns == 0)
        alarm.fired = true;   /* poll */
    else if (timeout_ns != EPOLL_WAIT_FOREVER) {
        timer_setup(&timer, alarm_fired, &alarm);
        timer_start(&timer, timeout_ns);
        armed = true;
    }
    struct wait_entry ep_we;
    wait_entry_init(&ep_we);
    int rc = 0;
    unsigned n = 0;

    mutex_lock(&ep->lock);
    for (;;) {
        n = collect(ep, out, max);
        if (n > 0 || __atomic_load_n(&alarm.fired, __ATOMIC_ACQUIRE))
            break;
        if (process_kill_pending()) {
            rc = -EINTR;
            break;
        }
        /* Arm every wake source, then decide under the lock. */
        waitqueue_prepare(&ep->wait, &ep_we);
        struct epoll_item *it;
        list_for_each_entry(it, &ep->items, link) {
            if (!it->disabled && it->wq) {
                waitqueue_prepare(it->wq, &it->we);
                it->prepared = true;
            }
        }
        bool sleep = !any_ready(ep) && !__atomic_load_n(&alarm.fired, __ATOMIC_ACQUIRE) &&
                     !process_kill_pending();
        mutex_unlock(&ep->lock);
        if (sleep)
            sched_block_current();
        mutex_lock(&ep->lock);
        waitqueue_finish(&ep->wait, &ep_we);
        list_for_each_entry(it, &ep->items, link) {
            if (it->prepared) {
                waitqueue_finish(it->wq, &it->we);
                it->prepared = false;
            }
        }
    }
    mutex_unlock(&ep->lock);
    if (armed)
        timer_cancel_sync(&timer);
    return rc ? rc : (int64_t)n;
}

static unsigned epoll_ready(struct kobject *obj)
{
    struct epoll_obj *ep = epoll_of(obj);
    mutex_lock(&ep->lock);
    unsigned r = any_ready(ep) ? COSMO_IO_READABLE : 0;
    mutex_unlock(&ep->lock);
    return r;
}

static struct waitqueue *epoll_poll_wq(struct kobject *obj, unsigned events)
{
    (void)events;
    return &epoll_of(obj)->wait;
}
