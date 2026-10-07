/*
 * epoll.c - an interest set of I/O objects waited on together (epoll).
 *
 * The object is a kobject holding a list of (fd, object) registrations. Each
 * carries a wanted COSMO_IO_* mask, opaque personality tokens (events, data),
 * and one-shot and edge flags. Readiness reaches the set by callback, as
 * Linux's does: every item owns a callback wait entry on each queue its
 * member's requested directions wake (one, or two for an O_RDWR FIFO),
 * registered for the item's whole life. A member's wake runs the callback
 * under the member queue's lock; it links the item onto the set's ready list
 * (under the set's ready-list spinlock) and wakes the set's own queue. A
 * waiter therefore sleeps on the set's queue alone and walks only the ready
 * list: the cost of a wait is the number of ready members, not the number
 * registered, and a set can be a member of another set because its wake
 * reaches the outer set's queue (docs/kernel/io/design.md, "epoll").
 *
 * Level- and edge-triggered: a level item that was reported goes back on the
 * ready list, so the next wait re-evaluates it (Linux re-queues level items);
 * an edge item comes back only through its callback, i.e. a new wake of its
 * member -- that wake is the edge, whether or not readiness dipped between two
 * looks. A one-shot item is disabled on report until MOD re-arms it. An item
 * found not ready when the ready list is walked is dropped from it (a drain's
 * wake put it there); its next wake brings it back.
 *
 * Lifetime: each registration holds a reference to its member object, dropped
 * on EPOLL_CTL_DEL, when the set is released, and when the member's last
 * handle-table slot anywhere is closed (epoll_last_handle_closed, from
 * handle_close): a registration lives exactly as long as some descriptor to
 * its member (A9). Before the item is freed its callback entries leave their
 * queues under those queues' locks, so a callback in flight on another CPU
 * has finished and none can start: the entries are the item's and the queues
 * are the member's, which the item's reference keeps alive. A queue whose
 * owner is not the member (a signalfd polls its process's queue) detaches
 * every callback entry before it dies (waitqueue_detach_callbacks) and frees
 * after a grace period; the unhook reads the queue pointer inside a read-side
 * section. Every item is also on its object's `watchers` list, which is how
 * the last close finds the sets to remove it from; one global mutex
 * (g_watch_lock) guards those lists and the subset lists the loop check
 * walks, and is taken outside ep->lock.
 *
 * Locks, inner to outer: a member queue's spinlock -> ep->rlock (ready list)
 * -> ep->wait's spinlock. A set woken from inside another set's wake takes
 * its two spinlocks with a lockdep subclass equal to the chain depth below
 * it (Linux's ep_poll_safewake): the inner set records the depth in `nests`
 * under its queue's lock while it wakes, and the forwarding callback reads
 * it there. EPOLL_MAX_NESTS bounds the chain so the subclass stays within
 * lockdep's four. ep->lock (mutex) serialises ctl against the walk of the
 * ready list and is never held when a spinlock above is taken by a waker;
 * the loop check holds g_watch_lock and no ep->lock.
 */

#include <kernel/compiler.h>
#include <kernel/epoll.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/list.h>
#include <kernel/mutex.h>
#include <kernel/object.h>
#include <kernel/panic.h>
#include <kernel/process.h>
#include <kernel/quiesce.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/wait.h>

#include <uapi/cosmo/syscall.h>

#define EPOLL_WANT_ALL(want) ((want) | COSMO_IO_HANGUP | COSMO_IO_ERROR)

struct epoll_obj;
struct epoll_item;

/* One callback entry on one member queue. */
struct epoll_hook {
    struct wait_entry we;
    struct waitqueue *wq;     /* the queue the entry is on; NULL when unhooked, or when the queue's
                               * owner detached it (WAIT_CB_FREED). Written under the queue's lock,
                               * read by the unhook inside a read-side section. */
    struct epoll_item *it;
};

enum { R_IDLE = 0, R_READY, R_TX };   /* off the ready list; on it; on a walker's transfer list */

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
    bool edge;                /* EPOLLET: report a wake once; a level item is re-queued after a report */
    bool member_set;          /* the member is an epoll set: its wakes forward with their depth */
    struct list_node link;    /* ep->items (ep->lock) */
    struct list_node set_link;   /* ep->subsets, when member_set (g_watch_lock) */
    struct epoll_obj *ep;     /* the set this item is in */
    struct epoll_item *obj_next;   /* the member object's watchers list (g_watch_lock) */
    struct epoll_hook hook[2];     /* the member queues' callback entries (ep->lock; each entry under its queue's lock) */
    unsigned nhooks;
    struct list_node rdllink; /* ep->rdllist (ep->rlock) */
    uint8_t rstate;           /* R_* (ep->rlock) */
    bool rewake;              /* a wake arrived while on a walker's transfer list (ep->rlock) */
    bool requeue;             /* the walker's own verdict for an item it holds (the walker only) */
};

struct epoll_obj {
    struct kobject obj;
    struct mutex lock;        /* the item list, the ready-list walk, the hooks */
    struct waitqueue wait;    /* the set's own queue: callbacks and ctl wake it, wait sleeps on it */
    spinlock_t rlock;         /* the ready list and every item's rstate */
    struct list_node items;
    struct list_node rdllist;
    struct list_node subsets; /* the items whose member is a set (g_watch_lock): the loop check's edges */
    unsigned nr;
    uint64_t next_id;         /* assigns each arm (add or MOD re-arm) a unique id */
    unsigned nests;           /* while this set's queue is being woken, under wait's lock: the lockdep
                               * subclass the next set up the chain must use (ep_poll_safewake) */
    /* The loop check's memo (g_watch_lock): each set is visited once per
     * check, so a layered graph of sets costs its size, not its paths
     * (Linux's loop_check_gen). */
    uint64_t visit_gen;       /* reaches: explored in this check */
    uint64_t down_gen, up_gen;
    unsigned down_depth, up_depth;
};

static uint64_t g_check_gen;  /* g_watch_lock: the current nesting check */

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

/* Every object's watchers list, every set's subsets list, and the
 * handle-count check an add makes against a concurrent last close. Taken
 * outside any ep->lock (add, del, release, the last-close removal, the loop
 * check); epoll_obj_wait never takes it. */
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
    spinlock_init(&ep->rlock, "epoll-ready");
    list_init(&ep->items);
    list_init(&ep->rdllist);
    list_init(&ep->subsets);
    *out = &ep->obj;
    return 0;
}

/* --- the callback ----------------------------------------------------------- */

/* Wake the set's queue from inside another queue's wake, at chain depth
 * `sub`, recording `sub + 1` for a set above us while our callbacks run. */
static void set_wake_nested(struct epoll_obj *ep, unsigned sub)
{
    arch_irq_state_t s = waitqueue_lock_nested(&ep->wait, sub);
    ep->nests = sub + 1;
    waitqueue_wake_all_locked(&ep->wait);
    ep->nests = 0;
    waitqueue_unlock(&ep->wait, s);
}

/* The member's queue was woken: the member's readiness may have changed.
 * Under the member queue's lock, in the waker's context (an interrupt, a
 * timer, another CPU's thread). Links the item onto the ready list unless it
 * is there, notes the wake if a walker has it, and wakes the set's waiters. */
static void hook_wake(struct wait_entry *e, unsigned flags)
{
    struct epoll_hook *h = container_of(e, struct epoll_hook, we);
    if (flags & WAIT_CB_FREED) {
        /* The queue's owner is freeing it and has unlinked us: never touch
         * that queue again (hook_unhook reads this). */
        __atomic_store_n(&h->wq, NULL, __ATOMIC_RELEASE);
        return;
    }
    struct epoll_item *it = h->it;
    struct epoll_obj *ep = it->ep;
    /* A wake that came up from a member set carries the chain depth below
     * it; a plain member's wake is depth 0. The subclass keeps lockdep's
     * order check honest about `epoll-ready` and `epoll` nested in themselves. */
    unsigned sub = it->member_set ? epoll_of(it->obj)->nests : 0;
    arch_irq_state_t s = spin_lock_irqsave_nested(&ep->rlock, sub);
    /* A fired one-shot hears nothing until MOD re-arms it (Linux's callback
     * returns for an item whose events are cleared): no link, no wake, so
     * the set does not read readable for an item a wait cannot report. MOD
     * and rearm clear the flag under ep->lock and then poll the member
     * themselves, so a wake skipped here is not an event lost. */
    if (__atomic_load_n(&it->disabled, __ATOMIC_ACQUIRE)) {
        spin_unlock_irqrestore(&ep->rlock, s);
        return;
    }
    if (it->rstate == R_IDLE) {
        list_push_back(&ep->rdllist, &it->rdllink);
        it->rstate = R_READY;
    } else if (it->rstate == R_TX) {
        it->rewake = true;   /* the walker re-queues it: this wake is not lost */
    }
    spin_unlock_irqrestore(&ep->rlock, s);
    set_wake_nested(ep, sub);
}

/* Resolve the wake queue(s) the member's requested directions wake. A
 * member's read and write readiness can live on different queues (an O_RDWR
 * FIFO wakes rd_wq on a read and wr_wq on a write), so up to two, one per
 * requested direction, de-duplicated. */
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

/* ep->lock held. Put the item's callback entries on its member's queues. */
static void hook_item(struct epoll_item *it)
{
    struct waitqueue *wqs[2];
    it->nhooks = member_wqs(it->obj, it->want, wqs);
    for (unsigned i = 0; i < it->nhooks; i++) {
        it->hook[i].it = it;
        it->hook[i].wq = wqs[i];
        wait_entry_init(&it->hook[i].we);
        waitqueue_add_callback(wqs[i], &it->hook[i].we, hook_wake);
    }
}

/* ep->lock held. Take the item's callback entries off their queues: when this
 * returns no callback of the item is running or can start (the removal is
 * under each queue's lock). A queue whose owner detached us (WAIT_CB_FREED,
 * NULL here) is not touched; the pointer is read inside a read-side section
 * because that owner frees the queue after a grace period. */
static void unhook_item(struct epoll_item *it)
{
    for (unsigned i = 0; i < it->nhooks; i++) {
        quiesce_read_lock();
        struct waitqueue *wq = __atomic_load_n(&it->hook[i].wq, __ATOMIC_ACQUIRE);
        if (wq != NULL)
            waitqueue_remove_callback(wq, &it->hook[i].we);
        quiesce_read_unlock();
        it->hook[i].wq = NULL;
    }
    it->nhooks = 0;
}

/* Take the item off the ready list, whatever state it is in. ep->lock held
 * (so no walker holds it on a transfer list). */
static void unready_item(struct epoll_obj *ep, struct epoll_item *it)
{
    arch_irq_state_t s = spin_lock_irqsave(&ep->rlock);
    if (it->rstate != R_IDLE)
        list_remove(&it->rdllink);
    it->rstate = R_IDLE;
    it->rewake = false;
    spin_unlock_irqrestore(&ep->rlock, s);
}

/* Put the item on the ready list (unless it is there) and wake the set: an
 * ADD or MOD of a member that is ready now, or a re-arm. ep->lock held. */
static void ready_item(struct epoll_obj *ep, struct epoll_item *it)
{
    arch_irq_state_t s = spin_lock_irqsave(&ep->rlock);
    if (it->rstate == R_IDLE) {
        list_push_back(&ep->rdllist, &it->rdllink);
        it->rstate = R_READY;
    }
    spin_unlock_irqrestore(&ep->rlock, s);
    /* Depth 0, and `nests` recorded for a set above us, as a callback's wake
     * does: an outer set's forwarding callback runs inside this wake. */
    set_wake_nested(ep, 0);
}

/* The readiness a member would report now (0 if disabled). */
static unsigned item_ready(const struct epoll_item *it)
{
    if (it->disabled)
        return 0;
    return kobject_ready(it->obj) & EPOLL_WANT_ALL(it->want);
}

/* --- the loop check (nesting) --------------------------------------------- */

/* g_watch_lock held. The longest chain of sets from `ep` downward, counting
 * `ep` (1 when it holds no set), memoised per check: a set reached by many
 * paths is computed once. A chain longer than EPOLL_MAX_NESTS cannot exist,
 * so the recursion is at most that deep. */
static unsigned depth_below(struct epoll_obj *ep)
{
    if (ep->down_gen == g_check_gen)
        return ep->down_depth;
    unsigned best = 1;
    struct epoll_item *it;
    list_for_each_entry(it, &ep->subsets, set_link) {
        unsigned d = 1 + depth_below(epoll_of(it->obj));
        if (d > best)
            best = d;
    }
    ep->down_gen = g_check_gen;
    ep->down_depth = best;
    return best;
}

/* g_watch_lock held. The longest chain of sets from `ep` upward through the
 * sets it is a member of, counting `ep`; memoised the same way. */
static unsigned depth_above(struct epoll_obj *ep)
{
    if (ep->up_gen == g_check_gen)
        return ep->up_depth;
    unsigned best = 1;
    for (struct epoll_item *it = ep->obj.watchers; it != NULL; it = it->obj_next) {
        unsigned d = 1 + depth_above(it->ep);
        if (d > best)
            best = d;
    }
    ep->up_gen = g_check_gen;
    ep->up_depth = best;
    return best;
}

/* g_watch_lock held. Whether `target` is `inner` or reachable below it; a
 * set explored in this check is not explored again. */
static bool reaches(struct epoll_obj *inner, struct epoll_obj *target)
{
    if (inner == target)
        return true;
    if (inner->visit_gen == g_check_gen)
        return false;
    inner->visit_gen = g_check_gen;
    struct epoll_item *it;
    list_for_each_entry(it, &inner->subsets, set_link)
        if (reaches(epoll_of(it->obj), target))
            return true;
    return false;
}

/* g_watch_lock held. Whether `inner` may become a member of `outer`: not when
 * `outer` is reachable from `inner` (a loop), and not when the chain through
 * the new edge would exceed EPOLL_MAX_NESTS sets. -ELOOP for both, as Linux.
 * One check generation: every set is visited at most once by each walk, so
 * the cost is the sets and edges reachable, however many paths join them
 * (review of PR #325: a layered graph made the path count exponential, all
 * under the global lock). */
static int nesting_allowed(struct epoll_obj *outer, struct epoll_obj *inner)
{
    g_check_gen++;
    if (reaches(inner, outer))
        return -ELOOP;
    if (depth_above(outer) + depth_below(inner) > EPOLL_MAX_NESTS)
        return -ELOOP;
    return 0;
}

/* --- the object ------------------------------------------------------------- */

static void epoll_release(struct kobject *obj)
{
    struct epoll_obj *ep = epoll_of(obj);
    struct epoll_item *it, *tmp;
    /* Nobody holds the set any more (this is its last reference), so its
     * lists are ours without ep->lock; the items must still leave their
     * objects' watchers lists and our subsets list under the watch lock, or
     * a member's later last close, or a loop check, would walk freed items. */
    mutex_lock(&g_watch_lock);
    list_for_each_entry(it, &ep->items, link) {
        watch_unlink(it);
        if (it->member_set)
            list_remove(&it->set_link);
    }
    mutex_unlock(&g_watch_lock);
    list_for_each_entry_safe(it, tmp, &ep->items, link) {
        list_remove(&it->link);
        unhook_item(it);          /* before the put: the queues are the member's */
        kobject_put(it->obj);
        kfree(it);
    }
    kfree(ep);
}

void epoll_last_handle_closed(struct kobject *obj)
{
    /* The lock first: an add that passed its handle-count check under this
     * lock and has not yet linked its item would be invisible to an unlocked
     * look at `watchers` (review of PR #324). handle_close spares the common
     * object this call altogether (kobject.watched, set under this lock
     * before the add's check; the pairing is the store-buffering litmus
     * tests/litmus/epoll/watched.litmus). */
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
        if (it->member_set)
            list_remove(&it->set_link);
        mutex_lock(&ep->lock);
        list_remove(&it->link);
        ep->nr--;
        unhook_item(it);
        unready_item(ep, it);
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

/* ep->lock held. */
static struct epoll_item *find_item(struct epoll_obj *ep, int fd)
{
    struct epoll_item *it;
    list_for_each_entry(it, &ep->items, link)
        if (it->fd == fd)
            return it;
    return NULL;
}

int epoll_obj_add(struct kobject *epobj, int fd, struct kobject *target,
                  unsigned want, uint32_t events, uint64_t data, bool oneshot, bool edge)
{
    struct epoll_obj *ep = epoll_of(epobj);
    if (target == epobj)
        return -EINVAL;   /* a set in itself: Linux says EINVAL, not ELOOP */
    struct epoll_item *it = kzalloc(sizeof(*it));
    if (it == NULL)
        return -ENOMEM;
    mutex_lock(&g_watch_lock);
    /* Published before the check below, so a last close that misses this
     * add's item (it decremented after our check) sees the flag and takes
     * the lock -- where it finds the item. Store-buffering: the fence pairs
     * with handle_close's (tests/litmus/epoll/watched.litmus). */
    __atomic_store_n(&target->watched, 1u, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    /* The caller looked `fd` up, so the object had a handle then; its last
     * close can have run since (another thread), and that removal found no
     * item. Refuse rather than register what nothing would ever remove. */
    if (__atomic_load_n(&target->handles, __ATOMIC_ACQUIRE) == 0) {
        mutex_unlock(&g_watch_lock);
        kfree(it);
        return -EBADF;
    }
    bool member_set = epoll_obj_from_kobject(target) != NULL;
    if (member_set) {
        int rc = nesting_allowed(ep, epoll_of(target));
        if (rc) {
            mutex_unlock(&g_watch_lock);
            kfree(it);
            return rc;
        }
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
    it->member_set = member_set;
    watch_link(it);
    if (member_set)
        list_push_back(&ep->subsets, &it->set_link);
    it->fd = fd;
    it->id = ep->next_id++;
    it->want = want;
    it->events = events;
    it->data = data;
    it->oneshot = oneshot;
    it->disabled = false;
    it->edge = edge;
    list_init(&it->rdllink);
    it->rstate = R_IDLE;
    list_push_back(&ep->items, &it->link);
    ep->nr++;
    /* Hooked first, then polled: a wake between the poll and the hook would
     * otherwise be missed. A member ready now goes straight on the list (a
     * waiter asleep must see it, as it would a new event); one with no queue
     * at all (a plain file: always ready, never changing) lives on the list. */
    hook_item(it);
    if (item_ready(it) || it->nhooks == 0)
        ready_item(ep, it);
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
    /* The requested directions may have changed, and with them the queues
     * the item must hear: re-hook, under the set lock so no walker has the
     * item. The ready state is kept -- a wake that arrived is still an event. */
    unhook_item(it);
    it->want = want;
    it->events = events;
    it->data = data;
    it->oneshot = oneshot;
    __atomic_store_n(&it->disabled, false, __ATOMIC_RELEASE);   /* MOD re-arms a fired one-shot */
    it->edge = edge;
    it->id = ep->next_id++;    /* a fresh arm: a copy-failure re-arm of the previous arm must not match */
    hook_item(it);
    /* A widened mask or a re-arm can make it reportable now: like ADD, a
     * fresh arm reports a member that is ready (the edge model's "a fresh arm
     * is eligible to report"). */
    if (item_ready(it) || it->nhooks == 0)
        ready_item(ep, it);
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
    if (it->member_set)
        list_remove(&it->set_link);
    unhook_item(it);          /* no callback runs on this item after this */
    unready_item(ep, it);
    mutex_unlock(&ep->lock);
    mutex_unlock(&g_watch_lock);
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
     * this is a no-op -- never re-enabling a different registration. The
     * event the door could not copy out goes back on the ready list, and a
     * fired one-shot's suppression is lifted, so it is not lost. */
    if (it != NULL && it->id == id) {
        __atomic_store_n(&it->disabled, false, __ATOMIC_RELEASE);
        ready_item(ep, it);
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

/* ep->lock held. Walk the ready list: move it to a transfer list under rlock
 * (so callbacks keep linking new arrivals onto the real list, and note a
 * wake of an item we hold), evaluate each item without any spinlock, report
 * the ready ones up to `max`, and put back what belongs on the list: a
 * reported level item (Linux re-queues level items, so the next wait
 * re-evaluates it; the tail, so a persistently ready member cannot hide
 * another when more are ready than fit), every unexamined item (the list
 * was longer than `max`), and any item whose member woke while we held it.
 * A disabled one-shot, a reported edge item, and an item found not ready
 * are dropped; a new wake brings each back. */
static unsigned collect(struct epoll_obj *ep, struct epoll_ready *out, unsigned max)
{
    struct list_node tx;
    list_init(&tx);
    arch_irq_state_t s = spin_lock_irqsave(&ep->rlock);
    while (!list_empty(&ep->rdllist)) {
        struct epoll_item *it = container_of(ep->rdllist.next, struct epoll_item, rdllink);
        list_remove(&it->rdllink);
        list_push_back(&tx, &it->rdllink);
        it->rstate = R_TX;
        it->rewake = false;
    }
    spin_unlock_irqrestore(&ep->rlock, s);

    unsigned n = 0;
    struct list_node requeue;
    list_init(&requeue);
    while (!list_empty(&tx)) {
        struct epoll_item *it = container_of(tx.next, struct epoll_item, rdllink);
        list_remove(&it->rdllink);
        bool keep;
        if (n >= max) {
            keep = true;   /* unexamined: stays ready */
        } else {
            unsigned io = item_ready(it);
            if (io != 0) {
                out[n].fd = it->fd;
                out[n].id = it->id;
                out[n].io = io;
                out[n].events = it->events;
                out[n].data = it->data;
                out[n].oneshot = it->oneshot;
                out[n].edge = it->edge;
                n++;
                if (it->oneshot)
                    __atomic_store_n(&it->disabled, true, __ATOMIC_RELEASE);   /* hook_wake reads it */
                keep = !it->oneshot && !it->edge;   /* level: re-queued; edge and one-shot: wait for a wake / MOD */
            } else {
                keep = false;   /* a drain's wake, or a one-shot still disabled: off the list */
            }
        }
        list_push_back(&requeue, &it->rdllink);
        it->requeue = keep;   /* ours alone; `rewake` is the callback's, resolved with it under rlock */
    }
    s = spin_lock_irqsave(&ep->rlock);
    while (!list_empty(&requeue)) {
        struct epoll_item *it = container_of(requeue.next, struct epoll_item, rdllink);
        list_remove(&it->rdllink);
        if (it->requeue || it->rewake) {   /* kept, or a wake arrived while it was ours */
            list_push_back(&ep->rdllist, &it->rdllink);
            it->rstate = R_READY;
        } else {
            it->rstate = R_IDLE;
        }
        it->rewake = false;
    }
    bool pending = !list_empty(&ep->rdllist);
    spin_unlock_irqrestore(&ep->rlock, s);
    /* Items went back on the list: another waiter that found it empty while
     * we held them is asleep with events pending, and an outer set that
     * polled us then read not-ready -- wake, as Linux's ep_done_scan does. */
    if (pending)
        set_wake_nested(ep, 0);
    return n;
}

/* Whether the ready list is empty. */
static bool nothing_ready(struct epoll_obj *ep)
{
    arch_irq_state_t s = spin_lock_irqsave(&ep->rlock);
    bool empty = list_empty(&ep->rdllist);
    spin_unlock_irqrestore(&ep->rlock, s);
    return empty;
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

    for (;;) {
        mutex_lock(&ep->lock);
        n = collect(ep, out, max);
        mutex_unlock(&ep->lock);
        if (n > 0 || __atomic_load_n(&alarm.fired, __ATOMIC_ACQUIRE) || process_kill_pending()) {
            if (n == 0 && process_kill_pending())
                rc = -EINTR;
            break;
        }
        /* Arm the one wake source, then decide: a callback that links an item
         * after this check wakes the entry (the thread is BLOCKED on the
         * queue before the list is read), so no event is lost; one that
         * linked before it is on the list and we do not sleep. */
        waitqueue_prepare(&ep->wait, &ep_we);
        bool sleep = nothing_ready(ep) && !__atomic_load_n(&alarm.fired, __ATOMIC_ACQUIRE) &&
                     !process_kill_pending();
        if (sleep)
            sched_block_current();
        waitqueue_finish(&ep->wait, &ep_we);
    }
    if (armed)
        timer_cancel_sync(&timer);
    return rc ? rc : (int64_t)n;
}

/* The set's own readiness, for poll() on it and for a set that is a member
 * of another: readable while its ready list has entries. An entry may turn
 * out not ready when walked (a drain's wake), so this can read readable
 * where a wait would then report nothing, as Linux's epoll fd can; what it
 * never does is read unreadable with an event pending, or take a lock an
 * outer walk holds -- it is a spinlock-guarded emptiness check, so a set
 * nested in a set needs no mutex recursion to be polled. */
static unsigned epoll_ready(struct kobject *obj)
{
    return nothing_ready(epoll_of(obj)) ? 0 : COSMO_IO_READABLE;
}

static struct waitqueue *epoll_poll_wq(struct kobject *obj, unsigned events)
{
    (void)events;
    return &epoll_of(obj)->wait;
}

/* --- for the tests ----------------------------------------------------------- */

unsigned epoll_obj_max_nests(void)
{
    return EPOLL_MAX_NESTS;
}
