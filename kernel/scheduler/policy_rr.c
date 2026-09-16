/*
 * policy_rr.c - Fixed-priority round-robin scheduling policy.
 *
 * 64 priority levels, lower number wins. Threads of equal priority take
 * turns in FIFO order with a fixed slice. A thread whose slice expires
 * yields only if someone else is ready; otherwise the slice refills and
 * it continues. This is the whole policy; the mechanism in sched.c does
 * the rest.
 */

#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/sched.h>

static void rr_enqueue(struct runqueue *rq, struct thread *t, bool at_head)
{
    KASSERT(list_empty(&t->rq_link));
    KASSERT(t->priority >= 0 && t->priority < SCHED_PRIO_COUNT);
    if (at_head)
        list_push_front(&rq->ready[t->priority], &t->rq_link);
    else
        list_push_back(&rq->ready[t->priority], &t->rq_link);
    rq->bitmap |= (uint64_t)1 << t->priority;
    rq->nr_running++;
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
 * The thread this queue would miss least: the lowest priority it has,
 * and within that level the one that would have run last.
 *
 * Lowest priority because the high-priority threads are the reason this
 * CPU is busy and the ones it is about to run; last-in because it is
 * furthest from running here, so moving it disturbs the least work. A
 * thread whose affinity excludes the destination is skipped rather than
 * refused, so a queue full of pinned threads simply offers the first one
 * that may travel.
 */
static struct thread *rr_pick_migratable(struct runqueue *rq, unsigned dst)
{
    cpumask_t want = CPUMASK_OF(dst);
    for (int prio = SCHED_PRIO_COUNT - 1; prio >= 0; prio--) {
        if (!(rq->bitmap & ((uint64_t)1 << prio)))
            continue;
        struct thread *t;
        list_for_each_entry_reverse(t, &rq->ready[prio], rq_link) {
            /*
             * Not the thread this CPU is running, which *can* be in a
             * ready list: a thread that blocks and is woken before it
             * stops running is left queued, and takes itself off in
             * `sched_set_running_current`. In that window it is both
             * `rq->current` and a list entry, and migrating it would
             * move a thread whose context is live on another CPU's
             * stack. An assertion in `sched_balance` caught exactly this
             * on the first boot of the balancer.
             */
            if (t == rq->current)
                continue;
            if (t->affinity & want)
                return t;
        }
    }
    return NULL;
}

static void rr_slice_new(struct thread *t)
{
    t->slice_left_ns = SCHED_SLICE_NS;
}

static void rr_tick(struct runqueue *rq, struct thread *current, uint64_t elapsed_ns)
{
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
};
