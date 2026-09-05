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
static inline void quiesce_core_publish(struct quiesce_state *st, unsigned cpu)
{
    uint64_t e = __atomic_load_n(&st->epoch, __ATOMIC_ACQUIRE);
    __atomic_store_n(&st->cpus[cpu].seen_epoch, e, __ATOMIC_RELEASE);
    st->cpus[cpu].transitions++;
}

/* W1: begin a grace period. Sequentially consistent so the caller's
 * unlink (before this call) is ordered before the new epoch is visible to
 * any CPU. Returns the epoch every online CPU must publish. */
static inline uint64_t quiesce_core_begin(struct quiesce_state *st)
{
    return __atomic_add_fetch(&st->epoch, 1u, __ATOMIC_SEQ_CST);
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
        if (__atomic_load_n(&st->cpus[c].seen_epoch, __ATOMIC_ACQUIRE) < target)
            pending |= (uint64_t)1 << c;
    }
    return pending;
}

#endif /* KERNEL_QUIESCE_CORE_H */
