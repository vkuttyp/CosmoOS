/*
 * policy_rr.c - Fixed-priority scheduling: round-robin, and a weighted
 * fair share at the default level.
 *
 * 64 priority levels, lower number wins. At every level but
 * SCHED_PRIO_DEFAULT, threads of equal priority take turns in FIFO order
 * with a fixed slice; a thread whose slice expires yields only if someone
 * else is ready, otherwise the slice refills and it continues.
 *
 * SCHED_PRIO_DEFAULT, where every user thread and most kernel workers run,
 * is the fair level (docs/kernel/scheduler/design.md, "The fair level";
 * roadmap 1.2 A5): its list is kept in virtual-run-time order, a thread's
 * run time counting 1024 / its nice value's weight, so each receives CPU
 * time in proportion to its weight. The arithmetic is CeeSafe's
 * (kernel/include/kernel/fair.h, kernel/scheduler/fair.c, generated); the
 * lists are here. This is the whole policy; the mechanism in sched.c does
 * the rest.
 */

#include <kernel/fair.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/sched.h>
#include <kernel/timer.h>

#define FAIR_LEVEL SCHED_PRIO_DEFAULT

static uint64_t fair_vruntime(const struct thread *t)
{
    return t->vruntime;
}

/*
 * Into the fair level's list in vruntime order, after its equals. A thread
 * that yielded goes behind everyone there, its vruntime raised to the last
 * one's; any other is placed no earlier than min_vruntime less the wake
 * bonus -- a no-op for the thread just switched out, which is at or past
 * the minimum. The walk starts at the tail: a thread switched out is
 * usually the furthest ahead.
 */
static void fair_enqueue(struct runqueue *rq, struct thread *t)
{
    struct list_node *head = &rq->ready[FAIR_LEVEL];
    if (t->yield_pending) {
        if (!list_empty(head)) {
            uint64_t last = fair_vruntime(list_entry(head->prev, struct thread, rq_link));
            if (fair_before(t->vruntime, last))
                t->vruntime = last;
        }
        list_push_back(head, &t->rq_link);
        return;
    }
    t->vruntime = fair_place(t->vruntime, rq->fair_min, FAIR_WAKE_BONUS_NS);
    struct list_node *at = head->prev;
    while (at != head && fair_before(t->vruntime, fair_vruntime(list_entry(at, struct thread, rq_link))))
        at = at->prev;
    list_insert_after(at, &t->rq_link);
}

static void rr_enqueue(struct runqueue *rq, struct thread *t, bool at_head)
{
    KASSERT(list_empty(&t->rq_link));
    KASSERT(t->priority >= 0 && t->priority < SCHED_PRIO_COUNT);
    if (t->priority == FAIR_LEVEL)
        fair_enqueue(rq, t);
    else if (at_head)
        list_push_front(&rq->ready[t->priority], &t->rq_link);
    else
        list_push_back(&rq->ready[t->priority], &t->rq_link);
    t->yield_pending = false;
    rq->bitmap |= (uint64_t)1 << t->priority;
    rq->nr_running++;
#if CONFIG_DEBUG
    t->ready_since_ns = clock_now_ns();
#endif
}

static void rr_dequeue(struct runqueue *rq, struct thread *t)
{
    KASSERT(!list_empty(&t->rq_link));
    list_remove(&t->rq_link);
    if (list_empty(&rq->ready[t->priority]))
        rq->bitmap &= ~((uint64_t)1 << t->priority);
    KASSERT(rq->nr_running > 0);
    rq->nr_running--;
}

static struct thread *rr_pick_next(struct runqueue *rq)
{
    if (rq->bitmap == 0)
        return NULL;
    unsigned prio = (unsigned)__builtin_ctzll(rq->bitmap);
    return list_first_entry(&rq->ready[prio], struct thread, rq_link);
}

/*
 * The thread this queue would run last: the lowest priority level that
 * has one, and the tail of its list. `rq->current` can be in a list --
 * woken between blocking and stopping, until sched_set_running_current
 * takes it out -- and must never be offered, nor may a thread that was
 * preempted, which may be mid-way through a per-CPU access (S26).
 */
static struct thread *rr_pick_migratable(struct runqueue *rq, cpumask_t allowed)
{
    uint64_t bits = rq->bitmap;
    while (bits != 0) {
        unsigned prio = 63u - (unsigned)__builtin_clzll(bits);
        struct thread *t;
        list_for_each_entry_reverse(t, &rq->ready[prio], rq_link) {
            if (t != rq->current && (t->flags & THREAD_FLAG_PREEMPTED) == 0 && (t->affinity & allowed) != 0)
                return t;
        }
        bits &= ~((uint64_t)1 << prio);
    }
    return NULL;
}

static void rr_slice_new(struct thread *t)
{
    t->slice_left_ns = SCHED_SLICE_NS;
}

/* The fair level's leftmost ready thread, or NULL. */
static struct thread *fair_leftmost(struct runqueue *rq)
{
    struct list_node *head = &rq->ready[FAIR_LEVEL];
    return list_empty(head) ? NULL : list_entry(head->next, struct thread, rq_link);
}

/*
 * Run time since the later of the switch-in and the last charge, weighted,
 * for a thread at the fair level; then the queue's minimum moves up to the
 * smaller of it and the leftmost. Time spent at another level (boosted by
 * priority inheritance) is not charged: the stamp moves on regardless.
 */
static void rr_charge(struct runqueue *rq, struct thread *t, uint64_t now_ns)
{
    if (t->priority == FAIR_LEVEL) {
        uint64_t from = t->last_start_ns > t->vruntime_stamp_ns ? t->last_start_ns : t->vruntime_stamp_ns;
        int32_t nice = __atomic_load_n(&t->nice, __ATOMIC_RELAXED);
        t->vruntime = fair_charge(t->vruntime, clock_delta_ns(now_ns, from), fair_weight(nice));
    }
    t->vruntime_stamp_ns = now_ns;
    struct thread *left = fair_leftmost(rq);
    bool cur = rq->current == t && t->priority == FAIR_LEVEL;
    rq->fair_min = fair_min(rq->fair_min, cur, cur ? t->vruntime : 0, left != NULL, left ? left->vruntime : 0);
}

/* A move keeps the thread's lead or lag on the new queue's minimum. */
static void rr_moved(struct runqueue *from, struct runqueue *to, struct thread *t)
{
    t->vruntime = fair_rebase(t->vruntime, from->fair_min, to->fair_min);
}

static void rr_tick(struct runqueue *rq, struct thread *current, uint64_t elapsed_ns)
{
    if (current->priority == FAIR_LEVEL) {
        /* A better level waiting (a wake already asked), or the leftmost
         * far enough behind: give way. The slice does not count here. */
        struct thread *left = fair_leftmost(rq);
        if ((rq->bitmap != 0 && (unsigned)__builtin_ctzll(rq->bitmap) < (unsigned)FAIR_LEVEL) ||
            (left != NULL && fair_should_preempt(current->vruntime, left->vruntime, FAIR_GRANULARITY_NS)))
            percpu_get(rq->cpu)->need_resched = true;
        return;
    }
    if (current->slice_left_ns > elapsed_ns) {
        current->slice_left_ns -= elapsed_ns;
        return;
    }
    current->slice_left_ns = 0;

    /* A higher or equal priority thread is waiting: give way. Otherwise
     * keep running with a fresh slice. */
    if (rq->bitmap != 0 && (unsigned)__builtin_ctzll(rq->bitmap) <= (unsigned)current->priority)
        percpu_get(rq->cpu)->need_resched = true;
    else
        rr_slice_new(current);
}

const struct sched_policy sched_policy_rr = {
    .name = "rr",
    .enqueue = rr_enqueue,
    .dequeue = rr_dequeue,
    .pick_next = rr_pick_next,
    .tick = rr_tick,
    .slice_new = rr_slice_new,
    .pick_migratable = rr_pick_migratable,
    .charge = rr_charge,
    .moved = rr_moved,
};
