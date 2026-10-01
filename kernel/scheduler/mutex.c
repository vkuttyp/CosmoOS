/*
 * mutex.c - Sleeping mutex on a wait queue.
 */

#include <kernel/lockdep.h>
#include <kernel/mutex.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/sched.h>
#include <kernel/thread.h>

void mutex_init(struct mutex *m, const char *name)
{
    spinlock_init(&m->lock, name);
    m->owner = NULL;
    m->name = name;
    m->class = 0;
    m->pi_active = 0;
    list_init(&m->pi_link);
    waitqueue_init(&m->wq, name);
}

/*
 * Priority inheritance (docs/audit/next-subsystem-priority-inheritance.md).
 *
 * One global lock serialises every priority donation and restoration. It is
 * the outermost lock of the scheme: under it we take run-queue locks (through
 * sched_reprioritize) and wait-queue locks (to read a mutex's waiters), never
 * the reverse, so it cannot deadlock against the scheduler or the wait
 * queues. A mutex is linked into its owner's `pi_held` list exactly while it
 * has waiters, and an owner's effective priority is the best (lowest number)
 * of its base and the top waiter of each mutex it holds. The `owner` field is
 * read here with an acquire load: a boost is always recorded as the mutex
 * sitting in the owner's `pi_held`, and the owner's unlock always removes it,
 * so a boost applied to a thread that releases concurrently is undone by that
 * release -- the global lock orders the two.
 */
#define PI_MAX_DEPTH 16u   /* chain bound: the lock-acquisition order is acyclic (lockdep checks it in debug) */

static spinlock_t g_pi_lock = SPINLOCK_INIT("mutex-pi");

/* The best (lowest-number) effective priority among `m`'s blocked waiters, or
 * SCHED_PRIO_COUNT when it has none. g_pi_lock held. */
static int pi_top_waiter(struct mutex *m)
{
    int best = SCHED_PRIO_COUNT;
    arch_irq_state_t s = spin_lock_irqsave(&m->wq.lock);
    struct wait_entry *e;
    list_for_each_entry(e, &m->wq.waiters, link) {
        if (e->thread != NULL && e->thread->priority < best)
            best = e->thread->priority;
    }
    spin_unlock_irqrestore(&m->wq.lock, s);
    return best;
}

/* Recompute `t`'s effective priority from its held mutexes, apply it, and
 * propagate up the chain of mutexes it is itself blocked on. g_pi_lock held. */
static void pi_apply_chain(struct thread *t)
{
    for (unsigned depth = 0; t != NULL && depth < PI_MAX_DEPTH; depth++) {
        int eff = t->base_prio;
        struct mutex *hm;
        list_for_each_entry(hm, &t->pi_held, pi_link) {
            int tw = pi_top_waiter(hm);
            if (tw < eff)
                eff = tw;
        }
        if (eff == t->priority)
            break;                       /* no change: nothing to propagate */
        sched_reprioritize(t, eff);
        struct mutex *n = t->pi_blocked_on;
        if (n == NULL)
            break;
        t = __atomic_load_n(&n->owner, __ATOMIC_ACQUIRE);   /* next owner up the chain */
    }
}

static bool try_take(struct mutex *m, struct thread *cur)
{
    arch_irq_state_t s = spin_lock_irqsave(&m->lock);
    bool got = m->owner == NULL;
    if (got)
        m->owner = cur;
    spin_unlock_irqrestore(&m->lock, s);
    return got;
}

bool mutex_trylock(struct mutex *m)
{
    struct thread *cur = thread_current();
    if (!try_take(m, cur))
        return false;
    lockdep_acquired(m, &m->class, m->name, LOCKDEP_KIND_MUTEX, 0, true, false, (uintptr_t)__builtin_return_address(0));
    return true;
}

static void lock_common(struct mutex *m, unsigned subclass, uintptr_t ip)
{
    struct thread *cur = thread_current();
    if (raw_this_cpu()->irq_depth != 0)   /* identity: zero on any CPU a sleeping caller runs on */
        panic("mutex_lock('%s') in interrupt context", m->name);
    /* Checked on every acquisition, not only when the mutex is contended
     * and wait_event would notice: a sleeping lock taken under a spinlock
     * (preempt_count > 0) must fail deterministically, in the first test
     * that runs the path, not only under load (invariant S6). */
    might_sleep();
    if (m->owner == cur)
        panic("mutex_lock('%s'): recursive lock by '%s'", m->name, cur->name);
    /* The order check runs before the wait so a deadlocking acquisition
     * is reported, not hung on; the push waits for ownership. */
    lockdep_acquire_check(&m->class, m->name, LOCKDEP_KIND_MUTEX, subclass, false, ip);

    if (!try_take(m, cur)) {
        /* Contended. Enqueue and, each time the owner still holds it, donate
         * this thread's priority to the owner (and up the chain) before
         * blocking. wait_event's lost-wakeup protocol is kept: prepared and
         * BLOCKED before the owner is re-checked, so a release between the
         * donation and the block makes sched_block_current return at once. */
        struct wait_entry we;
        wait_entry_init(&we);
        for (;;) {
            waitqueue_prepare(&m->wq, &we);
            if (try_take(m, cur))
                break;
            arch_irq_state_t s = spin_lock_irqsave(&g_pi_lock);
            struct thread *o = __atomic_load_n(&m->owner, __ATOMIC_ACQUIRE);
            if (o != NULL && o != cur) {
                cur->pi_blocked_on = m;
                if (list_empty(&m->pi_link)) {
                    list_push_back(&o->pi_held, &m->pi_link);
                    m->pi_active = 1;
                }
                pi_apply_chain(o);
            }
            spin_unlock_irqrestore(&g_pi_lock, s);
            sched_block_current();
            s = spin_lock_irqsave(&g_pi_lock);
            cur->pi_blocked_on = NULL;
            spin_unlock_irqrestore(&g_pi_lock, s);
        }
        waitqueue_finish(&m->wq, &we);
        /* Acquired through the contended path. Only one waiter was woken and
         * the release unlinked the mutex from the old owner, so if others
         * are still queued this new owner must inherit them. */
        arch_irq_state_t s = spin_lock_irqsave(&g_pi_lock);
        cur->pi_blocked_on = NULL;
        if (!waitqueue_empty(&m->wq)) {
            if (list_empty(&m->pi_link)) {
                list_push_back(&cur->pi_held, &m->pi_link);
                m->pi_active = 1;
            }
            pi_apply_chain(cur);
        }
        spin_unlock_irqrestore(&g_pi_lock, s);
    }
    lockdep_acquired(m, &m->class, m->name, LOCKDEP_KIND_MUTEX, subclass, false, false, ip);
}

void mutex_lock(struct mutex *m)
{
    lock_common(m, 0, (uintptr_t)__builtin_return_address(0));
}

void mutex_lock_nested(struct mutex *m, unsigned subclass)
{
    lock_common(m, subclass, (uintptr_t)__builtin_return_address(0));
}

void mutex_unlock(struct mutex *m)
{
    struct thread *cur = thread_current();
    arch_irq_state_t s = spin_lock_irqsave(&m->lock);
    if (m->owner != cur)
        panic("mutex_unlock('%s') by '%s' but owner is '%s'", m->name, cur->name,
              m->owner ? m->owner->name : "nobody");
    lockdep_release(m, LOCKDEP_KIND_MUTEX, (uintptr_t)__builtin_return_address(0));
    __atomic_store_n(&m->owner, NULL, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&m->lock, s);

    /* Give up any priority this mutex donated. The owner is cleared above
     * (under m->lock) before g_pi_lock is taken, so a thread donating
     * concurrently reads a NULL owner and does not boost a thread that no
     * longer holds the lock; a boost that landed earlier is recorded as this
     * mutex sitting in `pi_held`, and is undone here. Taken on every unlock
     * because whether a donation landed can only be read under g_pi_lock. */
    s = spin_lock_irqsave(&g_pi_lock);
    if (!list_empty(&m->pi_link)) {
        list_remove(&m->pi_link);
        list_init(&m->pi_link);
        m->pi_active = 0;
        pi_apply_chain(cur);   /* cur loses this mutex's inherited priority */
    }
    spin_unlock_irqrestore(&g_pi_lock, s);
    waitqueue_wake_one(&m->wq);
}

bool mutex_is_locked(struct mutex *m)
{
    return __atomic_load_n(&m->owner, __ATOMIC_ACQUIRE) != NULL;
}

/* Module ABI v1 exports (docs/kernel/module/api.md). */
#include <kernel/module.h>
EXPORT_SYMBOL(mutex_init);
EXPORT_SYMBOL(mutex_lock);
EXPORT_SYMBOL(mutex_lock_nested);
EXPORT_SYMBOL(mutex_trylock);
EXPORT_SYMBOL(mutex_unlock);
