/*
 * quiesce_core.h - The epoch algorithm of the quiescence subsystem, as
 * pure inline functions over a state block (docs/kernel/quiesce/design.md,
 * "The epoch algorithm and its memory ordering").
 *
 * Kept free of kernel dependencies so tests/host/test_quiesce.c can drive
 * it with host threads under the sanitizers. kernel/core/quiesce.c wraps
 * it with the scheduler, the per-CPU registry and the callback worker.
 */

#ifndef KERNEL_QUIESCE_CORE_H
#define KERNEL_QUIESCE_CORE_H

#include <stdbool.h>
#include <stdint.h>

#ifndef QUIESCE_MAX_CPUS
#define QUIESCE_MAX_CPUS 64u
#endif

/*
 * The memory orders, by the names design.md gives them. Only the host's
 * negative-control builds override one (tests/host/host.mk,
 * host-test-quiesce-tsan), to show the TSan model fails when that order
 * is weakened; the kernel always uses these defaults. The litmus tests in
 * tests/litmus/quiesce/ check the same orders against the C11 model.
 */
#ifndef QUIESCE_MO_Q1
#define QUIESCE_MO_Q1 __ATOMIC_ACQUIRE     /* publisher: load the epoch */
#endif
#ifndef QUIESCE_MO_Q2
#define QUIESCE_MO_Q2 __ATOMIC_ACQ_REL     /* publisher: exchange its seen epoch */
#endif
#ifndef QUIESCE_MO_W1
#define QUIESCE_MO_W1 __ATOMIC_SEQ_CST     /* waiter: advance the epoch */
#endif
#ifndef QUIESCE_MO_W2
#define QUIESCE_MO_W2 __ATOMIC_ACQUIRE     /* waiter: load a CPU's seen epoch */
#endif
#ifndef QUIESCE_MO_FENCE
#define QUIESCE_MO_FENCE __ATOMIC_SEQ_CST  /* W1b and Q0, the two online fences below */
#endif

/* Per-CPU record, one cache line each so publishing never shares a line
 * with another CPU's record or with the global epoch. */
struct quiesce_cpu {
    uint64_t seen_epoch;      /* last epoch published at a quiescent point (release store) */
    uint32_t depth;           /* debug: nested read-side sections on this CPU */
    uint32_t pad;
    uint64_t transitions;     /* debug: quiescent points passed */
} __attribute__((aligned(64)));

struct quiesce_state {
    uint64_t epoch __attribute__((aligned(64)));   /* advanced by waiters (seq_cst RMW) */
    struct quiesce_cpu cpus[QUIESCE_MAX_CPUS];
};

/* Q1/Q2: a CPU passes a quiescent point. Acquire the epoch so every later
 * read section sees the unlinks that preceded the epoch advance; release
 * the publication so every access in the read sections before this point
 * is ordered before the value the waiter will acquire. */
/* Returns true when this publish MOVED this CPU's seen epoch, i.e. when
 * it told a waiter something it did not already know. A publish by a CPU
 * that has already published the current epoch is correct and cheap, but
 * it advances nothing -- and attributing one of those to a straggler
 * kick would count the kick as having worked when it did not
 * (docs/kernel/quiesce/invariants.md, Q19). */
static inline bool quiesce_core_publish(struct quiesce_state *st, unsigned cpu)
{
    uint64_t e = __atomic_load_n(&st->epoch, QUIESCE_MO_Q1);
    uint64_t prev = __atomic_exchange_n(&st->cpus[cpu].seen_epoch, e, QUIESCE_MO_Q2);
    st->cpus[cpu].transitions++;
    return prev != e;
}

/* W1: begin a grace period. Sequentially consistent so the caller's
 * unlink (before this call) is ordered before the new epoch is visible to
 * any CPU. Returns the epoch every online CPU must publish. */
static inline uint64_t quiesce_core_begin(struct quiesce_state *st)
{
    return __atomic_add_fetch(&st->epoch, 1u, QUIESCE_MO_W1);
}

/*
 * W1b and Q0: the two halves of "which CPUs does a grace period wait
 * for" (docs/kernel/quiesce/design.md, "CPUs coming online").
 *
 * A waiter reads the online CPUs AFTER W1, behind W1b; a CPU coming
 * online marks itself online, then Q0, then publishes. That is the
 * store-buffering shape -- waiter: unlink, W1, read online; new CPU:
 * write online, read the epoch -- and with a sequentially consistent
 * fence on both sides at least one of them sees the other: either the
 * waiter's snapshot includes the new CPU and it waits for that CPU's
 * next publish, or the new CPU's publish reads the advanced epoch and
 * (Q1 against W1) every read section it enters afterwards sees the
 * unlink. Release/acquire alone permits both to miss, which is what the
 * order before these fences did (tests/litmus/quiesce/online-*.litmus).
 */
static inline void quiesce_core_after_begin(void)
{
    __atomic_thread_fence(QUIESCE_MO_FENCE);   /* W1b */
}

static inline void quiesce_core_after_online(void)
{
    __atomic_thread_fence(QUIESCE_MO_FENCE);   /* Q0 */
}

/* W2: the CPUs in `online` that have not yet published `target` (or a
 * later epoch: two waiters may advance twice before one point). Acquire
 * loads pair with the publishers' release stores, so when this returns 0
 * every read-side access those CPUs made before their quiescent points
 * happens-before the caller's next instruction. */
static inline uint64_t quiesce_core_pending(const struct quiesce_state *st, uint64_t target, uint64_t online)
{
    uint64_t pending = 0;
    for (unsigned c = 0; c < QUIESCE_MAX_CPUS && (online >> c) != 0; c++) {
        if (!((online >> c) & 1u))
            continue;
        if (__atomic_load_n(&st->cpus[c].seen_epoch, QUIESCE_MO_W2) < target)
            pending |= (uint64_t)1 << c;
    }
    return pending;
}

#endif /* KERNEL_QUIESCE_CORE_H */
