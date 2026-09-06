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

uint64_t clock_now_ns(void);
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

/* Hook called from the tick on every CPU (the scheduler registers). */
typedef void (*timer_tick_hook_fn)(uint64_t now_ns);
void timer_set_tick_hook(timer_tick_hook_fn hook);

void ndelay(uint64_t ns);
void udelay(uint64_t us);

/* Diagnostics. */
uint64_t timer_ticks(void);            /* this CPU */
unsigned timer_pending_count(void);    /* this CPU */

#endif /* KERNEL_TIMER_H */
