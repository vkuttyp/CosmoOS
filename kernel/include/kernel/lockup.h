/*
 * lockup.h - A CPU's program counter, seen from another CPU.
 *
 * Every tick records the interrupted PC and the time (two stores). On
 * request, every other online CPU records its own interrupted frame and
 * stack into its own per-CPU buffer, in its own handler, with no lock
 * and no printing: an NMI on x86-64, an ordinary IPI on AArch64. The
 * asker reads the buffers and prints. Two detectors run from the tick:
 * a soft lockup (this CPU has not switched while something is runnable)
 * and a hard lockup (the watched CPU has stopped ticking). The
 * scheduler dump and the self-test watchdog print the samples.
 * docs/kernel/diagnostics/design.md, "Lockups".
 */

#ifndef KERNEL_LOCKUP_H
#define KERNEL_LOCKUP_H

#include <kernel/compiler.h>
#include <kernel/lockup_core.h>
#include <kernel/percpu.h>

struct arch_trap_frame;

/* Start the detectors. Called once every CPU is up and ticking. */
void lockup_init(void);

/* Ask every online CPU but the caller for its frame and wait at most
 * `timeout_ns` in total for the answers; the caller's own sample comes
 * from `self` (its trap frame in a handler, NULL in a thread). Never
 * sleeps; safe from the tick. Returns false at once, having sent
 * nothing, when another CPU's report is in progress; true with the mask
 * of CPUs whose sample is current (the caller's own included), and the
 * reporter slot held until the paired lockup_print_samples returns. */
bool lockup_sample_all(const struct arch_trap_frame *self, uint64_t timeout_ns, cpumask_t *answered);

/* Handler side: record this CPU's frame if a request is pending for it.
 * Returns whether it did. No locks, no printing; NMI-safe. */
bool lockup_answer(struct arch_trap_frame *frame, bool nmi);

/* Print the samples of `answered` and, for the other online CPUs, the
 * tick-sampled last pc and its age; then release the reporter slot. */
void lockup_print_samples(cpumask_t answered);

/* One CPU's frame, now: claims the slot, asks `cpu` alone, copies its
 * answer into `out` and releases. False when the slot is taken or the
 * CPU did not answer within the bound. */
bool lockup_sample_cpu(unsigned cpu, uint64_t timeout_ns, struct cpu_sample *out);

/* `n` samples of `cpu`, `gap_ns` apart, one line each (the top three
 * frames): what a CPU that is not stalled in one place is cycling
 * through. Never sleeps (udelay); for the watchdog and the hard-lockup
 * report, never for the caller's own CPU. */
void lockup_profile(unsigned cpu, unsigned n, uint64_t gap_ns);

/* The CPU holding the reporter slot, or -1. */
int lockup_reporter(void);

/* The detectors' per-tick step; called by sched_tick with the tick's
 * frame. */
void lockup_tick(struct arch_trap_frame *frame, uint64_t now_ns);

/* Diagnostics: what the detectors have reported. Counters only grow. */
struct lockup_stats {
    uint64_t soft_reports;
    uint64_t hard_reports;
    unsigned soft_cpu;              /* the last soft report's CPU */
    unsigned soft_runnable;         /* ... and how many threads waited on it */
    uintptr_t soft_pc;              /* ... and its own tick PC */
    unsigned hard_cpu;              /* the last hard report's watcher */
    unsigned hard_target;           /* ... and the CPU that stopped ticking */
    uint64_t hard_stall_ms;         /* ... for how long */
    uint64_t hard_tick_age_ms;      /* ... and how old its tick sample was */
    cpumask_t hard_answered;        /* ... and who answered the sample it took (0: slot busy) */
    uint64_t samples;               /* requests that got the slot */
    uint64_t samples_busy;          /* requests refused because a report was in progress */
    uint64_t samples_waits;         /* deadlines lockup_sample_all armed: one per sample, however many CPUs fail to answer */
};
void lockup_get_stats(struct lockup_stats *out);

/* Test hook (debug builds): the thresholds, and whether a report is
 * expected -- an expected report's line says so, so the boot harness's
 * forbidden marker for a real one does not match it. Zero restores a
 * default. */
void lockup_set_thresholds(uint64_t soft_ns, uint64_t hard_ns, bool expected);

/* Test hook: sample with the ordinary interrupt instead of the NMI, so a
 * CPU with interrupts masked cannot answer on any architecture (x86-64
 * answers an NMI even masked, which left the sampler's timeout untested
 * there; docs/audit/next-subsystem-lockup-bound.md). Set around one
 * sample and cleared after it. */
void lockup_test_ipi_only(bool on);

#define LOCKUP_SOFT_NS_DEFAULT (10ull * 1000 * 1000 * 1000)
#define LOCKUP_HARD_NS_DEFAULT (10ull * 1000 * 1000 * 1000)
#define LOCKUP_SAMPLE_TIMEOUT_NS (5ull * 1000 * 1000)

#endif /* KERNEL_LOCKUP_H */
