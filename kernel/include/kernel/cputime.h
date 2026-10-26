/*
 * cputime.h - CPU time split into user and system time, and the CPU-time
 * interval timers ITIMER_VIRTUAL and ITIMER_PROF (docs/kernel/process/design.md,
 * "CPU time").
 *
 * The scheduler measures how long a thread runs to the nanosecond
 * (run_time_ns), but not in which mode. The periodic tick samples that: each
 * tick charges TICK_NS to the interrupted thread and its process, as user
 * time when it interrupted user mode and as system time otherwise. A report
 * splits the measured run time in the samples' proportion (cputime_adjust),
 * so user plus system is the measured total, and never goes below an earlier
 * report. The CPU-time timers run on the samples: ITIMER_VIRTUAL on user
 * time, ITIMER_PROF on both.
 *
 * The functions are Nova's (kernel/process/cputime.c, generated); callers
 * hold the lock that guards the structs (a process's `lock`).
 */

#ifndef KERNEL_CPUTIME_H
#define KERNEL_CPUTIME_H

#include <kernel/types.h>

/* A user/system pair of nanosecond counts. */
struct cputime_pair {
    uint64_t user_ns;
    uint64_t system_ns;
};

/* One CPU-time interval timer, on its clock (user time, or user plus
 * system time). */
struct cputime_itimer {
    uint64_t expires_ns;     /* the clock reading at which it fires; 0: disarmed */
    uint64_t interval_ns;    /* the reload at expiry; 0: one shot */
};

/* A process's CPU time, under its `lock`. A fork starts zeroed; exec keeps
 * it all, the timers included, as POSIX does. */
struct cputime {
    struct cputime_pair sample;     /* the tick samples, every thread's */
    struct cputime_pair reported;   /* the last split reported (cputime_adjust) */
    uint64_t gone_run_ns;           /* the run time of threads that have left the process */
    struct cputime_pair children;   /* waited-for children's split, with their own children's */
    struct cputime_itimer virt;     /* ITIMER_VIRTUAL: SIGVTALRM, on sample.user_ns */
    struct cputime_itimer prof;     /* ITIMER_PROF: SIGPROF, on sample.user_ns + sample.system_ns */
};

/* cputime_tick's result: the signals now due. */
#define CPUTIME_DUE_VIRT 1u
#define CPUTIME_DUE_PROF 2u

/* setitimer(2)'s `which` for the two timers here. */
#define CPUTIME_ITIMER_VIRTUAL 1u
#define CPUTIME_ITIMER_PROF 2u

/* One tick's sample: `tick_ns` of user time if `user`, else of system
 * time. Fires the timers whose clock reached their expiry, reloading
 * periodic ones by their interval, and returns which signals are due
 * (CPUTIME_DUE_*), for the caller to send after dropping the lock. */
unsigned cputime_tick(struct cputime *ct, bool user, uint64_t tick_ns);

/* setitimer for CPUTIME_ITIMER_VIRTUAL or _PROF (`which`; false for any
 * other). A `value` of 0 disarms, clearing the interval too; otherwise the
 * timer fires once its clock has advanced by `value` plus one tick, the
 * sample's granularity, as Linux does. The previous remaining time and
 * interval come back in `old_value` and `old_interval`, which may be NULL. */
bool cputime_itimer_set(struct cputime *ct, unsigned which, uint64_t value_ns, uint64_t interval_ns,
                        uint64_t tick_ns, uint64_t *old_value, uint64_t *old_interval);

/* getitimer: the remaining time (0 when disarmed, never 0 when armed) and
 * the interval. */
bool cputime_itimer_get(const struct cputime *ct, unsigned which, uint64_t tick_ns, uint64_t *value,
                        uint64_t *interval);

/* The measured run time `run_ns` split in the proportion of `sample`, no
 * part below `*reported`, which becomes the result: Linux's cputime_adjust.
 * The result's parts add up to run_ns unless an earlier report already
 * exceeds it, when the earlier report is repeated. */
void cputime_adjust(uint64_t run_ns, const struct cputime_pair *sample, struct cputime_pair *reported);

/* ⌊a·b/c⌋ for a ≤ c and c > 0, exactly (it is at most b). */
uint64_t cputime_scale(uint64_t a, uint64_t b, uint64_t c);

#endif
