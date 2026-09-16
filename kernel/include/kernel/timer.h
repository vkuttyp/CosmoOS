/*
 * timer.h - Monotonic clock, per-CPU tick, one-shot timers, delays.
 *
 * Contracts (docs/kernel/timer/): clock_now_ns is monotonic and lock-free;
 * timers are caller-owned objects armed on the calling CPU whose callback
 * runs in interrupt context on that CPU; timer_start on an armed timer
 * panics; udelay/ndelay spin and are usable anywhere.
 */

#ifndef KERNEL_TIMER_H
#define KERNEL_TIMER_H

#include <kernel/compiler.h>
#include <kernel/list.h>
#include <kernel/spinlock.h>

#define CONFIG_HZ 250u
#define NS_PER_SEC 1000000000ULL
#define TICK_NS (NS_PER_SEC / CONFIG_HZ)

enum timer_state { TIMER_IDLE, TIMER_PENDING, TIMER_RUNNING };

struct timer;
typedef void (*timer_fn)(struct timer *t, void *arg);

struct timer {
    struct list_node link;
    uint64_t expires_ns;
    timer_fn fn;
    void *arg;
    unsigned cpu;
    enum timer_state state;
};

struct timer_queue {
    spinlock_t lock;
    struct list_node pending;
    unsigned count;
    struct timer *running;   /* callback executing now on this queue's CPU, else NULL */
};

/* Calibrate, register the clock source, arm the boot CPU's tick, and
 * install the tick handler. Requires irq_init. */
void timer_init(void);

/* Start the tick on the calling CPU (APs during bring-up). */
void timer_init_cpu(void);

/*
 * Monotonic nanoseconds since boot.
 *
 * **Comparable across CPUs**: two values read on any two CPUs may be
 * subtracted, and the difference is the elapsed time between them to
 * within the residual skew the boot measured and printed
 * (`clock_worst_offset_ns`). Never negative, never wrapped.
 *
 * On AArch64 `cntpct_el0` is the system counter, common to every PE, so
 * there is nothing to correct and what remains is the cost of two reads
 * at two instants. On x86-64 the counter is per-CPU and the correction
 * is real (docs/audit/next-subsystem-cpu-clock.md).
 */
uint64_t clock_now_ns(void);

/*
 * Elapsed nanoseconds since `stamp`, **saturating at zero**.
 *
 * For any stamp that may have been taken on another CPU. Residual skew
 * can make such a stamp look like the future, and a plain unsigned
 * subtraction turns that into an interval of 584 years -- which is not a
 * slightly wrong measurement but a timeout that fires immediately, or a
 * diagnostic that sends its reader somewhere. Use this wherever the
 * stamp's CPU is not certainly this one; it costs a compare.
 */
uint64_t clock_since_ns(uint64_t stamp);

/*
 * The same saturating difference for a caller that already has a `now`.
 *
 * Several do, and they need it: a loop comparing many stamps against one
 * instant must not re-read the clock per item -- it would be a clock
 * read per iteration, sometimes under a lock, against a `now` that moves
 * underneath the comparison. `clock_since_ns` is this with a fresh read.
 */
static inline uint64_t clock_delta_ns(uint64_t now, uint64_t stamp)
{
    return now > stamp ? now - stamp : 0;
}

/*
 * The worst residual skew the boot measured, as a **magnitude**: the
 * contract's bound, the boot line's number, and what the cross-CPU
 * tests check themselves against. Unsigned, so that a comparison
 * against it cannot quietly be one-sided while an offset the other way
 * sails through.
 */
uint64_t clock_worst_offset_ns(void);

/*
 * `clock_worst_offset_ns()` when this machine's counter is not a clock
 * two CPUs may compare at all -- an x86-64 whose TSC is not invariant.
 * The kernel keeps using the counter (there is nothing else here to use)
 * and stops promising: a difference between two CPUs' readings is not an
 * interval, and no bound is claimed for it. Distinct from a large
 * measured offset, which is a number.
 */
#define CLOCK_OFFSET_UNBOUNDED UINT64_MAX

/* False when the offset is unbounded, as above. The boot says which. */
bool clock_is_common(void);

/*
 * Deadlines are timestamps, and the same rule governs them.
 *
 * `uint64_t d = clock_now_ns() + delay;` followed later by
 * `while (clock_now_ns() < d)` is a cross-CPU comparison whenever the
 * thread can be descheduled in between -- the deadline was computed
 * against one CPU's counter and is tested against another's. Saturating
 * subtraction does not help here: the comparison is an ordering, not a
 * difference, and `clock_since_ns` has nothing to saturate.
 *
 * When `clock_is_common()`, such a wait is wrong by at most
 * `clock_worst_offset_ns()`, which is the bound the boot advertised and
 * is the same tolerance every other cross-CPU user accepts. When it is
 * false the wait may expire early or late by an unbounded amount, and
 * the kernel has no cross-CPU time source to offer instead -- a shared
 * tick counter would be one, and this tree does not have a machine-wide
 * one (`timer_ticks()` is per-CPU).
 *
 * So: a deadline loop whose *correctness* depends on the duration needs
 * an age that no clock can distort. The block layer's timeout is the one
 * place in this tree where that mattered enough to build -- it counts
 * scans of its own thread beside the timestamp (`bio->scans`,
 * `kernel/block/blk.c`) so a stalled device still enters recovery on a
 * machine whose counter is not common.
 *
 * Every other deadline in the kernel goes through the two calls below.
 * They are not magic: on a machine where the offset is unbounded they
 * are exactly as wrong as the open-coded arithmetic they replaced, and
 * saying otherwise would be the sort of claim this unit exists to stop.
 * What they buy is that the hazard has **one** address instead of
 * sixteen -- the day this tree grows a machine-wide counter, a sound
 * deadline is two function bodies away rather than a sweep of every
 * driver poll. They also fix something real today: `clock_now_ns() +
 * budget` wraps into the past for a large budget and expires at once,
 * which `sys_futex_wait` guards against at its own call site and
 * nothing else did.
 */

/*
 * A deadline `budget_ns` from now, saturating at UINT64_MAX rather than
 * wrapping, and the test for it. Prefer these to `clock_now_ns() + x`
 * and a bare `<`.
 *
 * **These are safe across a migration and a hand-written deadline is
 * not.** They measure against the corrected clock when every CPU agrees
 * on it, and against the machine-wide tick when they do not -- one
 * counter, advanced by a designated CPU, with ownership taken over by
 * another when its owner stops ticking. Both calls read the same
 * quantity, so where the deadline was built and where it is tested no
 * longer matters. `kernel/timer/timer.c` has the design, including the
 * two shapes that were tried first and are wrong.
 *
 * `clock_now_ns() + x` compared with a bare `<` has none of that, which
 * is why every deadline loop in this tree uses these instead. Build with
 * `clock_deadline_ns`, test with `clock_deadline_passed`, always both or
 * neither -- and note the resolution: on a machine whose counter is not
 * common the tick is CONFIG_HZ-grained (4 ms), against a shortest budget
 * in this tree of 200 ms. `ndelay` and the lockup sampler are below that
 * and stay on the raw clock, each saying so.
 */
uint64_t clock_deadline_ns(uint64_t budget_ns);
bool clock_deadline_passed(uint64_t deadline);

/* This CPU's counter with no cross-CPU correction applied. For the
 * measurement that produces the correction, and for nothing else. */
uint64_t clock_raw_ns(void);

/* Measure every AP's offset against CPU 0 and, when the counter is one
 * this kernel may trust across CPUs, apply it. Called once after SMP
 * bring-up, from thread context on CPU 0. Reports what it measured
 * whether or not it applies it. */
void clock_measure_offsets(void);

/* One tick of the counter, in nanoseconds (rounded up, never zero). No
 * offset between two CPUs can be known more precisely than this. */
uint64_t clock_resolution_ns(void);

/* Whether any CPU's offset was actually measured. False on a counter
 * shared by construction, where a bound of zero is exact rather than
 * unmeasured. */
bool clock_offsets_measured(void);

/* The bound the measurement computed, whether or not it was applied. On
 * a machine that measures and then declines to trust the result, this is
 * the only way to check that the computation itself is sound. Zero when
 * nothing was measured. */
uint64_t clock_measured_bound_ns(void);

#if CONFIG_DEBUG
/*
 * Make this machine's counters disagree, for the cross-CPU tests.
 *
 * `clock_test_set_cpu_offset_ns` adds a signed offset to every
 * clock_now_ns() taken on `cpu`; `clock_test_set_worst_offset_ns`
 * rewrites the advertised bound. The second exists so a test cannot
 * quietly agree with whatever the boot happened to print: advertise
 * zero on a machine with injected skew and the assertion must fail.
 * Debug builds only.
 */
void clock_test_set_cpu_offset_ns(unsigned cpu, int64_t ns);
void clock_test_set_worst_offset_ns(uint64_t ns);
void clock_test_force_uncommon(bool on);

/* The machine-wide tick that deadlines use when the counter is not
 * common: its value, its current owner, and a way to point the
 * ownership at a CPU that will never tick, so the takeover path can be
 * tested rather than believed. */
uint64_t clock_test_global_ticks(void);
unsigned clock_test_tick_owner(void);
void clock_test_set_tick_owner(unsigned cpu);
#endif
/* Nanoseconds since 1970-01-01 UTC: the monotonic clock plus the offset
 * read from the real-time clock at boot (0 when the platform has none).
 * Lock-free, any context. */
uint64_t clock_realtime_ns(void);
uint64_t clock_hz(void);
const char *clock_name(void);

void timer_setup(struct timer *t, timer_fn fn, void *arg);
void timer_start(struct timer *t, uint64_t delay_ns);
/* True if the timer was pending and is now cancelled. Any context. On
 * return the callback will not START; it may still be RUNNING on the
 * timer's CPU, so the timer and its argument must stay alive. */
bool timer_cancel(struct timer *t);

/* Cancel and wait until the callback is not running anywhere: on return
 * the timer's memory may be freed. Spins (no sleep) while the callback
 * runs on another CPU, re-cancelling if the callback re-armed. Any
 * context except the timer's own callback (a panic: it would wait for
 * itself). On the timer's own CPU the callback cannot be running, since
 * callbacks run in interrupt context and interrupts are masked here, so
 * the wait is free. Returns what timer_cancel would have. */
bool timer_cancel_sync(struct timer *t);
#if CONFIG_DEBUG
/* Iterations timer_cancel_sync has spent waiting for a running callback,
 * readable *while* it waits -- which is what lets a test release a
 * callback it parked only once the cancel is demonstrably blocked
 * (docs/audit/next-subsystem-lifetime-windows.md). */
unsigned timer_test_cancel_spins(void);
void timer_test_reset_cancel_spins(void);
#endif

/* Hook called from the tick on every CPU (the scheduler registers), with
 * the tick's trap frame: the interrupted context. */
struct arch_trap_frame;
typedef void (*timer_tick_hook_fn)(uint64_t now_ns, struct arch_trap_frame *frame);
void timer_set_tick_hook(timer_tick_hook_fn hook);

/* CONFIG_SELFTEST: nanoseconds this CPU's ticks spent from entry to the
 * hook call, accumulated (lockup-tick-bench); 0 in a release build. */
uint64_t timer_tick_cost_ns(void);

void ndelay(uint64_t ns);
void udelay(uint64_t us);

/* Diagnostics. */
uint64_t timer_ticks(void);            /* this CPU */
unsigned timer_pending_count(void);    /* this CPU */

#endif /* KERNEL_TIMER_H */
