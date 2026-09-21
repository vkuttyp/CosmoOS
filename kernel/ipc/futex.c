/* futex.c - Wait on and wake by a user word (docs/compat/linux/design.md "futex"). */

#include <kernel/errno.h>
#include <kernel/futex.h>
#include <kernel/list.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/uaccess.h>
#include <kernel/vfs.h>
#include <kernel/vmm.h>
#include <kernel/wait.h>

#define FUTEX_BUCKETS 64

/*
 * A waiter's identity is its key (docs/audit/next-subsystem-shared-futex.md):
 * what the word maps, not which space asked. `key.held` is the one vnode
 * reference the waiter holds -- the vnode its CURRENT key names, or NULL
 * -- taken when it was classified or when a requeue moved it onto a
 * shared word, exchanged by a requeue that changes the key, and put at
 * dequeue. Every waiter on one word carries that word's key, so a requeue
 * deals with one old vnode and one new one however many it moves.
 */
struct futex_waiter {
    struct list_node link;
    struct futex_key key;       /* under the bucket lock: a requeue moves the waiter */
    struct bucket *bucket;      /* the list the link is on; changed only with both buckets locked */
    struct thread *thread;
    bool woken;
    bool timed_out;
};

struct bucket {
    spinlock_t lock;
    struct list_node waiters;
    uint64_t wake_seq;   /* bumped by every futex_wake under the lock */
    uint64_t queue_seq;  /* bumped by every change to the waiter list (enqueue, wake, requeue) */
};

static struct bucket g_buckets[FUTEX_BUCKETS];

/* Called once from kernel_main before any user process exists (and so
 * before any futex call); the buckets are never re-initialised. */
void futex_init(void)
{
    for (unsigned i = 0; i < FUTEX_BUCKETS; i++) {
        spinlock_init(&g_buckets[i].lock, "futex");
        list_init(&g_buckets[i].waiters);
    }
}

static struct bucket *bucket_of(const struct futex_key *k)
{
    uint64_t h = (uintptr_t)k->obj ^ (k->off >> 2) ^ (k->off >> 17);
    return &g_buckets[h % FUTEX_BUCKETS];
}

static bool key_eq(const struct futex_key *a, const struct futex_key *b)
{
    return a->obj == b->obj && a->off == b->off;
}

/* The reference a key holds, dropped where a put may block: never under
 * a bucket lock. */
static void key_put(struct futex_key *k)
{
    if (k->held != NULL) {
        vnode_put(k->held);
        k->held = NULL;
    }
}

static void timeout_fired(struct timer *t, void *arg)
{
    (void)t;
    struct futex_waiter *w = arg;
    __atomic_store_n(&w->timed_out, true, __ATOMIC_RELEASE);
    sched_wake(w->thread);
}

int futex_wait(struct vm_space *space, uint64_t uaddr, uint32_t val, uint64_t timeout_ns, bool private)
{
    if (uaddr & 3)
        return -EINVAL;
    struct futex_waiter w = { .thread = thread_current() };
    int krc = vm_user_futex_key(space, uaddr, private, &w.key);
    if (krc)
        return krc;
    struct bucket *b = bucket_of(&w.key);
    w.bucket = b;
    list_init(&w.link);

    /*
     * The compare and the enqueue must be atomic with respect to a waker,
     * or a wake between them is lost. The user copy may fault (a demand
     * fault allocates, a fatal one kills the process), so it cannot run
     * under the bucket spinlock (docs/kernel/lockdep/design.md, "futex").
     * Instead: read the bucket's wake sequence, drop the lock, copy and
     * compare, re-take the lock and enqueue only if no wake happened in
     * between. A wake that did happen may have been ours: return 0, a
     * spurious wake the futex contract permits and every user retries.
     */
    arch_irq_state_t s = spin_lock_irqsave(&b->lock);
    uint64_t seq = b->wake_seq;
    spin_unlock_irqrestore(&b->lock, s);

    uint32_t cur;
    if (copy_from_user(&cur, uaddr, sizeof(cur))) {
        key_put(&w.key);
        return -EFAULT;
    }
    if (cur != val) {
        key_put(&w.key);
        return -EAGAIN;
    }

    /*
     * The key was taken before the word was read, with the space lock
     * released in between: a thread that unmapped this address and
     * mapped something else at it (MAP_FIXED) in that window would have
     * had the word read from the new mapping and the waiter enqueued
     * under the old key, where a wake through the new mapping never
     * looks. A caller racing its own munmap against its own wait has a
     * bug, but the waiter is the one that would hang for it, so the
     * word is classified again and a changed key returns 0 -- the
     * spurious wake the contract permits (review found the window).
     */
    struct futex_key again;
    krc = vm_user_futex_key(space, uaddr, private, &again);
    if (krc) {
        key_put(&w.key);
        return krc;
    }
    bool same = key_eq(&again, &w.key);
    key_put(&again);
    if (!same) {
        key_put(&w.key);
        return 0;
    }

    s = spin_lock_irqsave(&b->lock);
    if (b->wake_seq != seq) {
        spin_unlock_irqrestore(&b->lock, s);
        key_put(&w.key);
        return 0;
    }
    list_push_back(&b->waiters, &w.link);
    b->queue_seq++;
    spin_unlock_irqrestore(&b->lock, s);

    struct timer t;
    if (timeout_ns) {
        timer_setup(&t, timeout_fired, &w);
        timer_start(&t, timeout_ns);
    }
    /* A private wait queue with this thread as the only waiter: the wake
     * side and the timer wake the thread directly. */
    struct waitqueue wq;
    waitqueue_init(&wq, "futex");
    int rc = wait_event_killable(&wq, __atomic_load_n(&w.woken, __ATOMIC_ACQUIRE) ||
                                          __atomic_load_n(&w.timed_out, __ATOMIC_ACQUIRE));
    if (timeout_ns)
        timer_cancel(&t);

    /* Leave whichever list the waiter is on now: a requeue may have moved
     * it to another bucket, changing w.bucket under both buckets' locks,
     * so the bucket read under its own lock is the one the link is on. */
    bool was_woken;
    for (;;) {
        struct bucket *cur_b = __atomic_load_n(&w.bucket, __ATOMIC_ACQUIRE);
        s = spin_lock_irqsave(&cur_b->lock);
        if (w.bucket == cur_b) {
            was_woken = w.woken;
            if (!list_empty(&w.link))
                list_remove(&w.link);
            spin_unlock_irqrestore(&cur_b->lock, s);
            break;
        }
        spin_unlock_irqrestore(&cur_b->lock, s);
    }
    /* Dequeued: the one reference the waiter holds -- to whatever vnode
     * its key names now, after any requeue -- goes here, with no lock. */
    key_put(&w.key);

    if (was_woken)
        return 0;
    if (rc)
        return rc;   /* -EINTR */
    return -ETIMEDOUT;
}

int futex_wake(struct vm_space *space, uint64_t uaddr, unsigned n, bool private)
{
    if (uaddr & 3)
        return -EINVAL;
    struct futex_key key;
    int krc = vm_user_futex_key(space, uaddr, private, &key);
    if (krc)
        return krc;
    struct bucket *b = bucket_of(&key);
    int woken = 0;
    arch_irq_state_t s = spin_lock_irqsave(&b->lock);
    b->wake_seq++;   /* a waiter between its compare and its enqueue sees this and retries */
    struct futex_waiter *w, *tmp;
    list_for_each_entry_safe(w, tmp, &b->waiters, link) {
        if ((unsigned)woken >= n)
            break;
        if (!key_eq(&w->key, &key))
            continue;
        list_remove(&w->link);
        list_init(&w->link);
        __atomic_store_n(&w->woken, true, __ATOMIC_RELEASE);
        sched_wake(w->thread);
        woken++;
    }
    if (woken)
        b->queue_seq++;
    spin_unlock_irqrestore(&b->lock, s);
    key_put(&key);   /* the call's own reference; a waiter's is its own */
    return woken;
}

int futex_requeue(struct vm_space *space, uint64_t uaddr1, uint64_t uaddr2, unsigned nr_wake, unsigned nr_requeue,
                  bool cmp, uint32_t cmpval, bool private)
{
    if ((uaddr1 & 3) || (uaddr2 & 3))
        return -EINVAL;
    struct futex_key k1, k2;
    int krc = vm_user_futex_key(space, uaddr1, private, &k1);
    if (krc)
        return krc;
    krc = vm_user_futex_key(space, uaddr2, private, &k2);
    if (krc) {
        key_put(&k1);
        return krc;
    }
    struct bucket *b1 = bucket_of(&k1), *b2 = bucket_of(&k2);
    bool same_key = key_eq(&k1, &k2);
    /* Two buckets of one class: lower address first, always (no other path
     * takes two), the second annotated as nested for lockdep
     * (docs/kernel/lockdep/invariants.md, "futex"). */
    struct bucket *lo = b1 < b2 ? b1 : b2, *hi = b1 < b2 ? b2 : b1;
    arch_irq_state_t s;
    for (;;) {
        /*
         * CMP_REQUEUE's compare must be atomic with respect to the other
         * futex operations on uaddr1's bucket, as on Linux, where the word
         * is read under the bucket lock. The user copy cannot run under the
         * spinlock here (it may fault), so: note the bucket's queue
         * sequence, compare unlocked, then take the locks and act only if
         * no enqueue, wake or requeue touched the bucket in between;
         * otherwise compare again. The same shape as futex_wait's
         * compare-then-enqueue.
         */
        uint64_t seq = 0;
        if (cmp) {
            s = spin_lock_irqsave(&b1->lock);
            seq = b1->queue_seq;
            spin_unlock_irqrestore(&b1->lock, s);
            uint32_t cur;
            if (copy_from_user(&cur, uaddr1, sizeof(cur))) {
                key_put(&k1);
                key_put(&k2);
                return -EFAULT;
            }
            if (cur != cmpval) {
                key_put(&k1);
                key_put(&k2);
                return -EAGAIN;
            }
        }
        s = spin_lock_irqsave(&lo->lock);
        if (hi != lo)
            spin_lock_nested(&hi->lock, 1);
        if (!cmp || b1->queue_seq == seq)
            break;
        if (hi != lo)
            spin_unlock(&hi->lock);
        spin_unlock_irqrestore(&lo->lock, s);
    }
    /*
     * A wake or a move counts as a wake for a waiter caught between its
     * compare and its enqueue on uaddr1: it returns spuriously rather than
     * sleeping on a word whose waiters have just been moved away. A word
     * requeued onto itself with nothing to wake moves nobody, and must
     * not bump the sequence: it is the count of sleepers the native
     * thread door's tests read, and a count that woke the waiters it was
     * counting sent them round their loop and onto a mutex the counter
     * held -- CI's slower hosts hit that window every run.
     */
    if (!same_key || nr_wake)
        b1->wake_seq++;
    b1->queue_seq++;
    b2->queue_seq++;
    int woken = 0, requeued = 0, moved = 0;
    struct futex_waiter *w, *tmp;
    list_for_each_entry_safe(w, tmp, &b1->waiters, link) {
        if (!key_eq(&w->key, &k1))
            continue;
        if ((unsigned)woken < nr_wake) {
            list_remove(&w->link);
            list_init(&w->link);
            __atomic_store_n(&w->woken, true, __ATOMIC_RELEASE);
            sched_wake(w->thread);
            woken++;
        } else if ((unsigned)requeued < nr_requeue) {
            /*
             * A word requeued onto itself is counted and left where it is,
             * as on Linux. Moving it would push it to the tail of the list
             * this loop is walking, where it matches uaddr1 again and is
             * moved again: an unbounded walk with interrupts off that any
             * program could ask for. Left in place it is the one thing
             * userland cannot otherwise learn -- how many are asleep on a
             * word -- which the native thread door's tests use.
             */
            if (!same_key) {
                list_remove(&w->link);
                /*
                 * The key changes, so the reference is exchanged: the
                 * waiter now holds one to the destination's vnode --
                 * taken below, one add for all of them, before the locks
                 * drop -- and gives up the one it held, which is put
                 * after the locks are released: every waiter on uaddr1
                 * held k1's vnode, so one vnode, `moved` times.
                 */
                w->key.obj = k2.obj;
                w->key.off = k2.off;
                w->key.held = k2.held;
                __atomic_store_n(&w->bucket, b2, __ATOMIC_RELEASE);
                list_push_back(&b2->waiters, &w->link);
                moved++;
            }
            requeued++;
        } else {
            break;
        }
    }
    /*
     * The moved waiters' new references, in one add: `moved` holders of
     * one vnode were created above, and the count must say so before
     * the locks drop, because a moved waiter that wakes and dequeues
     * after the unlock puts a reference it must already hold. One
     * atomic add under the locks rather than one per waiter (review of
     * the build): the walk is bounded by the waiters present on the
     * word, and the reference traffic on the vnode's line need not
     * scale with it at all.
     */
    if (moved > 0 && k2.held != NULL)
        vnode_get_n(k2.held, (unsigned)moved);
    if (hi != lo)
        spin_unlock(&hi->lock);
    spin_unlock_irqrestore(&lo->lock, s);
    /* The moved waiters' old references, and the call's own two. */
    for (int i = 0; i < moved && k1.held != NULL; i++)
        vnode_put(k1.held);
    key_put(&k1);
    key_put(&k2);
    return woken + requeued;
}
