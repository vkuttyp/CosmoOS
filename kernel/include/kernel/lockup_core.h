/*
 * lockup_core.h - The lockup unit's pure parts: the per-CPU sample
 * record and the hard-lockup watcher rule. No kernel dependencies, so
 * the host test (tests/host/test_lockup.c) includes it as is.
 * docs/kernel/diagnostics/design.md, "Lockups".
 */

#ifndef KERNEL_LOCKUP_CORE_H
#define KERNEL_LOCKUP_CORE_H

#include <stdbool.h>
#include <stdint.h>

#define LOCKUP_TRACE_MAX 16

/* What a CPU records about itself when asked. Written only by that CPU,
 * in its own handler (lockup_answer); `seq` is stored last, with
 * release, and is the claim that the rest is complete. `want` is
 * written by the asker before it sends the interrupt: a request is
 * pending for this CPU exactly when want != seq. */
struct cpu_sample {
    uint64_t want;
    uint64_t seq;
    uintptr_t pc;
    uintptr_t sp;
    uintptr_t trace[LOCKUP_TRACE_MAX];
    unsigned depth;
    bool nmi;                       /* answered from an NMI (x86-64) */
    uint64_t when_ns;
};

/* The CPU that CPU `k` watches for a hard lockup: the online CPU with
 * the next-higher id, wrapping to the lowest online id. With two or
 * more CPUs online every online CPU has exactly one watcher whatever
 * holes `online` has; a lone CPU's target is itself, and the checker
 * skips a target that is itself. */
static inline unsigned lockup_watch_target(uint64_t online, unsigned k)
{
    uint64_t above = k >= 63 ? 0 : (online & ~((((uint64_t)1) << (k + 1)) - 1));
    if (above != 0)
        return (unsigned)__builtin_ctzll(above);
    if (online != 0)
        return (unsigned)__builtin_ctzll(online);
    return k;
}

#endif /* KERNEL_LOCKUP_CORE_H */
