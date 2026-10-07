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
 * cannot free a member whose queue the waiter is parked on). Level- and
 * edge-triggered (EPOLLET, docs/audit/next-subsystem-epollet.md); one-shot
 * supported. See docs/audit/next-subsystem-epoll.md.
 *
 * Lifetime: each registration holds a reference to its member object, dropped
 * on EPOLL_CTL_DEL, when the epoll is released, and when the member's last
 * handle-table slot anywhere is closed (epoll_last_handle_closed, called by
 * handle_close): Linux's open file description is the kobject here, and a
 * registration lives exactly as long as some descriptor to it -- a dup'd or
 * inherited descriptor keeps it, closing one of several does not remove it,
 * the last close does, in whatever epoll and process it was made. Every item
 * is also linked on its object's `watchers` list, which is how the last close
 * finds the sets to remove it from. One global mutex (g_watch_lock) guards
 * every watchers list and is taken outside ep->lock; epoll_obj_wait takes
 * ep->lock alone, so a waiter is never in the order. A waiter asleep with the
 * member pinned (its snapshot reference) is woken by the removal and drops the
 * pin on its next pass; the member's release, and so a socket's FIN, follows
 * that drop, never the waiter's next event. Nesting an epoll in an epoll stays
 * refused: a member's events wake the member's queue, not the set's, so an
 * outer set sleeping on an inner set's queue would sleep through them
 * (docs/kernel/io/design.md, "epoll").
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

struct epoll_obj;

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
    bool edge;                /* EPOLLET: report only on a transition into readiness */
    bool armed;               /* edge: eligible to report an edge now (distinct from !disabled) */
    uint64_t edge_gen;        /* edge: the member queue's wake generation last observed; a change
                               * means the member's source fired (an event), so re-arm */
    struct list_node link;
    struct epoll_obj *ep;     /* the set this item is in, for the last-close removal */
    struct epoll_item *obj_next;   /* the member object's watchers list (g_watch_lock) */
};

/* A member captured for one wait: its queue(s) and a held reference, so the
 * sleep is immune to a concurrent DEL/MOD of the live list. A member's read and
 * write readiness can live on different queues (an O_RDWR FIFO), so up to two. */
struct epoll_snap {
    struct kobject *obj;      /* referenced for the duration of the wait */
    struct waitqueue *wq[2];  /* the requested directions' wake queues, de-duplicated */
    unsigned nwq;
    unsigned want;
    bool edge;                /* captured so the sleep decision gates like collect */
    bool armed;               /* a disarmed edge member is not "ready" for the sleep check */
    uint64_t edge_gen;        /* the member's wake generation collect last acted on: a change
                               * seen after the wait entries are armed is a fresh edge, so do not sleep */
    struct wait_entry we[2];
    bool prepared[2];
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

/* Every object's watchers list, and the handle-count check an add makes
 * against a concurrent last close. Taken outside any ep->lock (add, del,
 * release, the last-close removal); epoll_obj_wait never takes it. */
static struct mutex g_watch_lock;

void epoll_init(void)
{
    mutex_init(&g_watch_lock, "epoll-watch");
}

/* g_watch_lock held; the list head is read and written under it only. */
static void watch_link(struct epoll_item *it)
{
    it->obj_next = it->obj->watchers;
    it->obj->watchers = it;
}

/* g_watch_lock held. The item is on its object's list exactly once. */
static void watch_unlink(struct epoll_item *it)
{
    struct kobject *obj = it->obj;
    if (obj->watchers == it) {
        obj->watchers = it->obj_next;
    } else {
        struct epoll_item *prev = obj->watchers;
        while (prev->obj_next != it)
            prev = prev->obj_next;
        prev->obj_next = it->obj_next;
    }
    it->obj_next = NULL;
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
    /* Nobody holds the set any more (this is its last reference), so its
     * list is ours without ep->lock; the items must still leave their
     * objects' watchers lists under the watch lock, or a member's later
     * last close would walk into freed items. */
    mutex_lock(&g_watch_lock);
    list_for_each_entry(it, &ep->items, link)
        watch_unlink(it);
    mutex_unlock(&g_watch_lock);
    list_for_each_entry_safe(it, tmp, &ep->items, link) {
        list_remove(&it->link);
        kobject_put(it->obj);
        kfree(it);
    }
    kfree(ep);
}

void epoll_last_handle_closed(struct kobject *obj)
{
    /* The lock first, even for the common object that was never
     * registered: an add that passed its handle-count check under this
     * lock and has not yet linked its item would be invisible to an
     * unlocked look at `watchers`, and its registration would outlive the
     * last descriptor (review of PR #324). The decision is made under the
     * lock the add publishes under; the uncontended mutex is the cost. */
    mutex_lock(&g_watch_lock);
    /* A descriptor can have reappeared: a handle riding in a unix message
     * is installed at the receiver (handle_install raises the count) and
     * may have landed between our caller's decrement and this lock. Then
     * the object has a descriptor again and its registrations stand. */
    if (__atomic_load_n(&obj->handles, __ATOMIC_ACQUIRE) != 0) {
        mutex_unlock(&g_watch_lock);
        return;
    }
    struct epoll_item *gone = obj->watchers;
    obj->watchers = NULL;
    for (struct epoll_item *it = gone; it != NULL; it = it->obj_next) {
        struct epoll_obj *ep = it->ep;   /* alive: its items leave this list in epoll_release */
        mutex_lock(&ep->lock);
        list_remove(&it->link);
        ep->nr--;
        /* A waiter asleep on this member holds its own pin (snapshot) and
         * parks its own wait entry on the member's queue; wake it so it
         * finishes and drops the pin now rather than on the member's next
         * event -- the object's release (a socket's FIN) waits on that. */
        waitqueue_wake_all(&ep->wait);
        mutex_unlock(&ep->lock);
    }
    mutex_unlock(&g_watch_lock);
    /* The registrations' references, outside both locks (a release may
     * block; the rule is epoll_obj_del's). None is the object's last: our
     * caller, handle_close, still holds the slot's and puts it after. */
    while (gone != NULL) {
        struct epoll_item *next = gone->obj_next;
        kobject_put(gone->obj);
        kfree(gone);
        gone = next;
    }
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

/* Resolve the wake queue(s) the member's requested directions sleep on. A
 * member's read and write readiness can live on different queues (an O_RDWR
 * FIFO wakes rd_wq on a read and wr_wq on a write), so fill up to two, one per
 * requested direction, de-duplicated. Returns the count. Lock held. */
static unsigned member_wqs(struct kobject *obj, unsigned want, struct waitqueue *out[2])
{
    unsigned n = 0;
    struct waitqueue *rd = (want & COSMO_IO_READABLE)
        ? kobject_poll_wq(obj, COSMO_IO_READABLE | COSMO_IO_HANGUP | COSMO_IO_ERROR) : NULL;
    struct waitqueue *wr = (want & COSMO_IO_WRITABLE)
        ? kobject_poll_wq(obj, COSMO_IO_WRITABLE | COSMO_IO_HANGUP | COSMO_IO_ERROR) : NULL;
    if (rd)
        out[n++] = rd;
    if (wr && wr != rd)
        out[n++] = wr;
    if (n == 0) {
        /* Neither direction requested (e.g. a hangup-only watch): the queue for
         * the full mask. */
        struct waitqueue *w = kobject_poll_wq(obj, EPOLL_WANT_ALL(want));
        if (w)
            out[n++] = w;
    }
    return n;
}

/* The combined wake generation across `n` queues: any one advancing advances
 * the sum (both are monotonic), so a change means a watched direction fired. */
static uint64_t wqs_gen(struct waitqueue *const *wq, unsigned n)
{
    uint64_t g = 0;
    for (unsigned i = 0; i < n; i++)
        g += waitqueue_wake_gen(wq[i]);
    return g;
}

/* The member's combined wake generation over its requested directions. A member
 * with no poll queue (always ready, never changes) has no event to track, so
 * its stored value is returned, which never looks changed. Lock held. */
static uint64_t item_wq_gen(const struct epoll_item *it)
{
    struct waitqueue *wqs[2];
    unsigned n = member_wqs(it->obj, it->want, wqs);
    return n ? wqs_gen(wqs, n) : it->edge_gen;
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
        /* An edge member re-arms when its poll queue has been woken since we
         * last looked -- its source fired (an event), whether or not a wait was
         * blocked for it, whether or not its readiness ever dipped to 0 between
         * our looks. This is the edge: a drain-then-refill, or a new event on a
         * still-ready member, both advance the generation. */
        if (it->edge) {
            uint64_t gen = item_wq_gen(it);
            if (gen != it->edge_gen) {
                it->armed = true;
                it->edge_gen = gen;
            }
        }
        /* An edge member reports only on a transition -- only while armed. A
         * level member reports whenever ready, as before. */
        if (io != 0 && !(it->edge && !it->armed)) {
            out[n].fd = it->fd;
            out[n].id = it->id;
            out[n].io = io;
            out[n].events = it->events;
            out[n].data = it->data;
            out[n].oneshot = it->oneshot;
            out[n].edge = it->edge;
            n++;
            if (it->oneshot)
                it->disabled = true;
            if (it->edge)
                it->armed = false;         /* disarm until a later wake re-arms it */
            list_remove(&it->link);        /* round-robin: reported goes to the tail */
            list_push_back(&ep->items, &it->link);
        }
        cur = next;
    }
    return n;
}

int epoll_obj_add(struct kobject *epobj, int fd, struct kobject *target,
                  unsigned want, uint32_t events, uint64_t data, bool oneshot, bool edge)
{
    struct epoll_obj *ep = epoll_of(epobj);
    struct epoll_item *it = kzalloc(sizeof(*it));
    if (it == NULL)
        return -ENOMEM;
    mutex_lock(&g_watch_lock);
    /* The caller looked `fd` up, so the object had a handle then; its last
     * close can have run since (another thread), and that removal found no
     * item. Refuse rather than register what nothing would ever remove. */
    if (__atomic_load_n(&target->handles, __ATOMIC_ACQUIRE) == 0) {
        mutex_unlock(&g_watch_lock);
        kfree(it);
        return -EBADF;
    }
    mutex_lock(&ep->lock);
    if (find_item(ep, fd) != NULL) {
        mutex_unlock(&ep->lock);
        mutex_unlock(&g_watch_lock);
        kfree(it);
        return -EEXIST;
    }
    it->obj = target;          /* takes ownership of the caller's reference */
    it->ep = ep;
    watch_link(it);
    it->fd = fd;
    it->id = ep->next_id++;
    it->want = want;
    it->events = events;
    it->data = data;
    it->oneshot = oneshot;
    it->disabled = false;
    it->edge = edge;
    it->armed = true;          /* a fresh arm is eligible to report an edge */
    it->edge_gen = edge ? item_wq_gen(it) : 0;   /* only a later wake re-arms past this */
    list_push_back(&ep->items, &it->link);
    ep->nr++;
    /* A concurrent epoll_wait must re-evaluate the new member (as aio_submit
     * wakes the ring's queue after parking an entry). */
    waitqueue_wake_all(&ep->wait);
    mutex_unlock(&ep->lock);
    mutex_unlock(&g_watch_lock);
    return 0;
}

int epoll_obj_mod(struct kobject *epobj, int fd, unsigned want, uint32_t events, uint64_t data, bool oneshot, bool edge)
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
    it->edge = edge;
    it->armed = true;          /* ... and re-arms the edge */
    it->edge_gen = edge ? item_wq_gen(it) : 0;   /* a fresh arm: only a later wake re-arms past this */
    it->id = ep->next_id++;    /* a fresh arm: a copy-failure re-arm of the previous arm must not match */
    waitqueue_wake_all(&ep->wait);   /* a widened mask or re-arm can make it reportable */
    mutex_unlock(&ep->lock);
    return 0;
}

int epoll_obj_del(struct kobject *epobj, int fd)
{
    struct epoll_obj *ep = epoll_of(epobj);
    mutex_lock(&g_watch_lock);
    mutex_lock(&ep->lock);
    struct epoll_item *it = find_item(ep, fd);
    if (it == NULL) {
        mutex_unlock(&ep->lock);
        mutex_unlock(&g_watch_lock);
        return -ENOENT;
    }
    list_remove(&it->link);
    ep->nr--;
    watch_unlink(it);
    mutex_unlock(&ep->lock);
    mutex_unlock(&g_watch_lock);
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
     * this is a no-op -- never re-enabling a different registration. Restore
     * whichever suppression the report set: a fired one-shot's `disabled`, or a
     * reported edge's `armed`, so an event the door could not copy out is not
     * lost. */
    if (it != NULL && it->id == id) {
        bool changed = false;
        if (it->disabled) {
            it->disabled = false;
            changed = true;
        }
        if (it->edge && !it->armed) {
            it->armed = true;
            changed = true;
        }
        if (changed)
            waitqueue_wake_all(&ep->wait);   /* a waiter that slept while it was suppressed must re-evaluate */
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
        snap[n].nwq = member_wqs(it->obj, it->want, snap[n].wq);
        snap[n].want = it->want;
        snap[n].edge = it->edge;
        snap[n].armed = it->armed;
        snap[n].edge_gen = it->edge_gen;
        for (unsigned j = 0; j < 2; j++) {
            snap[n].prepared[j] = false;
            wait_entry_init(&snap[n].we[j]);
        }
        n++;
    }
    return n;
}

/* Whether the wait should stay awake rather than sleep. Called after the member
 * wait entries are armed, so it closes the window between collect and arming:
 *  - a disarmed edge member does not count as ready for its readiness (collect
 *    would not report it) -- but if its queue was woken since collect acted on
 *    it (its generation advanced), an event fired in that window and the next
 *    collect would re-arm it, so do not sleep;
 *  - any other member counts as ready when its readiness bits are set.
 * A member's queue firing after this check wakes the armed entry instead. */
static bool snap_any_ready(struct epoll_snap *snap, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        if (snap[i].edge && !snap[i].armed) {
            if (snap[i].nwq && wqs_gen(snap[i].wq, snap[i].nwq) != snap[i].edge_gen)
                return true;   /* a fresh edge raced in; re-collect rather than sleep */
            continue;
        }
        if (kobject_ready(snap[i].obj) & EPOLL_WANT_ALL(snap[i].want))
            return true;
    }
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
        /* collect re-arms each edge member from its own poll queue's wake
         * generation, so no wake bookkeeping is needed here: a member's event
         * (even one that raced the deadline, or arrived while not blocked) is
         * seen on the next collect, and a bare timeout -- not an event -- does
         * not re-arm anything. */
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
            for (unsigned j = 0; j < snap[i].nwq; j++) {
                waitqueue_prepare(snap[i].wq[j], &snap[i].we[j]);
                snap[i].prepared[j] = true;
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
            for (unsigned j = 0; j < 2; j++)
                if (snap[i].prepared[j])
                    waitqueue_finish(snap[i].wq[j], &snap[i].we[j]);
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
