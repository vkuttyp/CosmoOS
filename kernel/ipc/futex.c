/* futex.c - Wait on and wake by a user word (docs/compat/linux/design.md "futex"). */

#include <kernel/errno.h>
#include <kernel/futex.h>
#include <kernel/list.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/uaccess.h>
#include <kernel/wait.h>

#define FUTEX_BUCKETS 64

struct futex_waiter {
    struct list_node link;
    struct vm_space *space;
    uint64_t uaddr;
    struct thread *thread;
    bool woken;
    bool timed_out;
};

struct bucket {
    spinlock_t lock;
    struct list_node waiters;
    uint64_t wake_seq;   /* bumped by every futex_wake under the lock */
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

static struct bucket *bucket_of(struct vm_space *space, uint64_t uaddr)
{
    uint64_t h = (uintptr_t)space ^ (uaddr >> 2) ^ (uaddr >> 17);
    return &g_buckets[h % FUTEX_BUCKETS];
}

static void timeout_fired(struct timer *t, void *arg)
{
    (void)t;
    struct futex_waiter *w = arg;
    __atomic_store_n(&w->timed_out, true, __ATOMIC_RELEASE);
    sched_wake(w->thread);
}

int futex_wait(struct vm_space *space, uint64_t uaddr, uint32_t val, uint64_t timeout_ns)
{
    if (uaddr & 3)
        return -EINVAL;
    struct bucket *b = bucket_of(space, uaddr);
    struct futex_waiter w = { .space = space, .uaddr = uaddr, .thread = thread_current() };
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
    if (copy_from_user(&cur, uaddr, sizeof(cur)))
        return -EFAULT;
    if (cur != val)
        return -EAGAIN;

    s = spin_lock_irqsave(&b->lock);
    if (b->wake_seq != seq) {
        spin_unlock_irqrestore(&b->lock, s);
        return 0;
    }
    list_push_back(&b->waiters, &w.link);
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

    s = spin_lock_irqsave(&b->lock);
    bool was_woken = w.woken;
    if (!list_empty(&w.link))
        list_remove(&w.link);
    spin_unlock_irqrestore(&b->lock, s);

    if (was_woken)
        return 0;
    if (rc)
        return rc;   /* -EINTR */
    return -ETIMEDOUT;
}

int futex_wake(struct vm_space *space, uint64_t uaddr, unsigned n)
{
    if (uaddr & 3)
        return -EINVAL;
    struct bucket *b = bucket_of(space, uaddr);
    int woken = 0;
    arch_irq_state_t s = spin_lock_irqsave(&b->lock);
    b->wake_seq++;   /* a waiter between its compare and its enqueue sees this and retries */
    struct futex_waiter *w, *tmp;
    list_for_each_entry_safe(w, tmp, &b->waiters, link) {
        if ((unsigned)woken >= n)
            break;
        if (w->space != space || w->uaddr != uaddr)
            continue;
        list_remove(&w->link);
        list_init(&w->link);
        __atomic_store_n(&w->woken, true, __ATOMIC_RELEASE);
        sched_wake(w->thread);
        woken++;
    }
    spin_unlock_irqrestore(&b->lock, s);
    return woken;
}
