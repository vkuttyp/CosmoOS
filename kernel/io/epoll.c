/*
 * epoll.c - an interest set of I/O objects waited on together (epoll).
 *
 * The object is a kobject holding a list of (fd, object) registrations. Each
 * carries a wanted COSMO_IO_* mask, opaque personality tokens (events, data),
 * and a one-shot flag. epoll_obj_wait is the aio ring's multi-wait
 * (kernel/io/aio.c) in the shape poll.c uses: it takes a *snapshot* of the
 * members under the lock -- pinning each with a reference and recording its
 * poll_wq -- then arms its *own* per-call wait entries on those queues and the
 * set's own queue, evaluates readiness, and sleeps only if none is ready and
 * the deadline has not passed. A finite timeout wakes the waiting thread
 * directly. A ctl that adds or re-arms a member wakes the set's queue so a
 * concurrent waiter re-evaluates.
 *
 * The snapshot-and-pin is what makes concurrent ctl safe: a waiter's wait
 * entries are its own (so two waiters do not share one), it finishes on the
 * queues it armed (so a MOD that changes a member's queue cannot strand it),
 * and it holds a reference to each member across the sleep (so a DEL or close
 * cannot free a member whose queue the waiter is parked on). Level-triggered;
 * one-shot supported. See docs/audit/next-subsystem-epoll.md.
 *
 * Lifetime: each registration holds a reference to its member object, dropped
 * on EPOLL_CTL_DEL and when the epoll is released. v1 has no auto-remove on a
 * member's close: a registered fd must be removed with EPOLL_CTL_DEL.
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
    uint64_t id;              /* this ARM instance: bumped on add and on each MOD re-arm, so a
                               * stale re-arm (from a failed copy) never matches a later arm */
    unsigned want;            /* COSMO_IO_* requested, for readiness filtering */
    uint32_t events;          /* opaque personality events token, echoed to the waiter */
    uint64_t data;            /* opaque data token, echoed to the waiter */
    bool oneshot;             /* disable after one report, until MOD re-arms */
    bool disabled;            /* a fired one-shot, until MOD or rearm */
    struct list_node link;
};

/* A member captured for one wait: its queue and a held reference, so the sleep
 * is immune to a concurrent DEL/MOD of the live list. */
struct epoll_snap {
    struct kobject *obj;      /* referenced for the duration of the wait */
    struct waitqueue *wq;     /* kobject_poll_wq(obj, want|HANGUP|ERROR), or NULL */
    unsigned want;
    struct wait_entry we;
    bool prepared;
};

struct epoll_obj {
    struct kobject obj;
    struct mutex lock;        /* the item list */
    struct waitqueue wait;    /* the set's own queue: ctl wakes it, wait sleeps on it */
    struct list_node items;
    unsigned nr;
    uint64_t next_id;         /* assigns each arm (add or MOD re-arm) a unique id */
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

/* Lock held. Fill up to `max` ready members, newest fairness: each reported
 * entry is moved to the tail so a persistently-ready fd cannot hide another
 * when more are ready than fit. Disables one-shots as they are reported (the
 * caller re-arms any it cannot deliver). The walk is bounded by the item count
 * captured up front, so moving entries to the tail cannot loop. */
static unsigned collect(struct epoll_obj *ep, struct epoll_ready *out, unsigned max)
{
    unsigned n = 0, budget = ep->nr;
    struct list_node *cur = ep->items.next;
    while (cur != &ep->items && n < max && budget-- > 0) {
        struct list_node *next = cur->next;
        struct epoll_item *it = container_of(cur, struct epoll_item, link);
        unsigned io = item_ready(it);
        if (io != 0) {
            out[n].fd = it->fd;
            out[n].id = it->id;
            out[n].io = io;
            out[n].events = it->events;
            out[n].data = it->data;
            out[n].oneshot = it->oneshot;
            n++;
            if (it->oneshot)
                it->disabled = true;
            list_remove(&it->link);        /* round-robin: reported goes to the tail */
            list_push_back(&ep->items, &it->link);
        }
        cur = next;
    }
    return n;
}

int epoll_obj_add(struct kobject *epobj, int fd, struct kobject *target,
                  unsigned want, uint32_t events, uint64_t data, bool oneshot)
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
    it->id = ep->next_id++;
    it->want = want;
    it->events = events;
    it->data = data;
    it->oneshot = oneshot;
    it->disabled = false;
    list_push_back(&ep->items, &it->link);
    ep->nr++;
    /* A concurrent epoll_wait must re-evaluate the new member (as aio_submit
     * wakes the ring's queue after parking an entry). */
    waitqueue_wake_all(&ep->wait);
    mutex_unlock(&ep->lock);
    return 0;
}

int epoll_obj_mod(struct kobject *epobj, int fd, unsigned want, uint32_t events, uint64_t data, bool oneshot)
{
    struct epoll_obj *ep = epoll_of(epobj);
    mutex_lock(&ep->lock);
    struct epoll_item *it = find_item(ep, fd);
    if (it == NULL) {
        mutex_unlock(&ep->lock);
        return -ENOENT;
    }
    it->want = want;
    it->events = events;
    it->data = data;
    it->oneshot = oneshot;
    it->disabled = false;      /* MOD re-arms a fired one-shot */
    it->id = ep->next_id++;    /* a fresh arm: a copy-failure re-arm of the previous arm must not match */
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
    ep->nr--;
    mutex_unlock(&ep->lock);
    /* Safe to free even with a waiter asleep: a waiter holds its own reference
     * to the member and parks its own wait entry on the member's queue, not
     * this item's -- the item carries no wait state. */
    kobject_put(it->obj);
    kfree(it);
    return 0;
}

void epoll_obj_rearm(struct kobject *epobj, int fd, uint64_t id)
{
    struct epoll_obj *ep = epoll_of(epobj);
    mutex_lock(&ep->lock);
    struct epoll_item *it = find_item(ep, fd);
    /* Only the exact registration that produced the undelivered event: if the
     * fd was removed, or removed and re-added, the id no longer matches and
     * this is a no-op -- never re-enabling a different registration. */
    if (it != NULL && it->id == id && it->disabled) {
        it->disabled = false;
        waitqueue_wake_all(&ep->wait);   /* a waiter that slept while it was disabled must re-evaluate */
    }
    mutex_unlock(&ep->lock);
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

/* Lock held. Capture every enabled member into `snap` (up to `cap`), pinning
 * each with a reference; returns the count. */
static unsigned snapshot(struct epoll_obj *ep, struct epoll_snap *snap, unsigned cap)
{
    unsigned n = 0;
    struct epoll_item *it;
    list_for_each_entry(it, &ep->items, link) {
        if (n >= cap)
            break;
        if (it->disabled)
            continue;
        kobject_get(it->obj);
        snap[n].obj = it->obj;
        snap[n].wq = kobject_poll_wq(it->obj, EPOLL_WANT_ALL(it->want));
        snap[n].want = it->want;
        snap[n].prepared = false;
        wait_entry_init(&snap[n].we);
        n++;
    }
    return n;
}

static bool snap_any_ready(struct epoll_snap *snap, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        if (kobject_ready(snap[i].obj) & EPOLL_WANT_ALL(snap[i].want))
            return true;
    return false;
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
    int rc = 0;
    unsigned n = 0;

    for (;;) {
        mutex_lock(&ep->lock);
        n = collect(ep, out, max);
        if (n > 0 || __atomic_load_n(&alarm.fired, __ATOMIC_ACQUIRE) || process_kill_pending()) {
            if (n == 0 && process_kill_pending())
                rc = -EINTR;
            mutex_unlock(&ep->lock);
            break;
        }
        /* Snapshot the members and pin them, and arm every wake source -- all
         * under the lock, so a concurrent ctl's wake of ep->wait cannot be lost
         * (as aio_wait arms the ring's queue under its lock). */
        unsigned cap = ep->nr;
        struct epoll_snap *snap = cap ? kmalloc(cap * sizeof(*snap), 0) : NULL;
        if (cap && snap == NULL) {
            mutex_unlock(&ep->lock);
            rc = -ENOMEM;
            break;
        }
        unsigned sn = snapshot(ep, snap, cap);
        wait_entry_init(&ep_we);
        waitqueue_prepare(&ep->wait, &ep_we);
        for (unsigned i = 0; i < sn; i++) {
            if (snap[i].wq) {
                waitqueue_prepare(snap[i].wq, &snap[i].we);
                snap[i].prepared = true;
            }
        }
        bool sleep = !snap_any_ready(snap, sn) && !__atomic_load_n(&alarm.fired, __ATOMIC_ACQUIRE) &&
                     !process_kill_pending();
        mutex_unlock(&ep->lock);
        if (sleep)
            sched_block_current();
        /* Finish without the lock: each pinned member (and so its queue) is
         * kept alive by the reference the snapshot holds, even if a concurrent
         * DEL removed and freed its item. */
        waitqueue_finish(&ep->wait, &ep_we);
        for (unsigned i = 0; i < sn; i++) {
            if (snap[i].prepared)
                waitqueue_finish(snap[i].wq, &snap[i].we);
            kobject_put(snap[i].obj);
        }
        kfree(snap);
    }
    if (armed)
        timer_cancel_sync(&timer);
    return rc ? rc : (int64_t)n;
}

static unsigned epoll_ready(struct kobject *obj)
{
    struct epoll_obj *ep = epoll_of(obj);
    mutex_lock(&ep->lock);
    struct epoll_item *it;
    unsigned r = 0;
    list_for_each_entry(it, &ep->items, link) {
        if (item_ready(it)) {
            r = COSMO_IO_READABLE;
            break;
        }
    }
    mutex_unlock(&ep->lock);
    return r;
}

static struct waitqueue *epoll_poll_wq(struct kobject *obj, unsigned events)
{
    (void)events;
    return &epoll_of(obj)->wait;
}
