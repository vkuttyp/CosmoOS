# Timer Subsystem: Design

## 1. Clock source

```c
struct clock_source {
    const char *name;
    uint64_t hz;                 /* counter frequency */
    uint64_t (*read)(void);      /* raw counter */
};
```

`clock_now_ns()` = `mul_u64_u64_shr((read() - base), ns_per_tick_scaled)`
using 128-bit intermediate arithmetic (`unsigned __int128`) so a
3 GHz counter does not overflow for centuries. `base` is captured in
`timer_init` so boot time reads as 0.

x86-64 provides the TSC: `rdtsc` with `lfence` ordering. Its frequency is
measured in `arch_timer_calibrate()`:

1. Program PIT channel 2 for a one-shot count of 11932 ticks (10 ms at
   1.193182 MHz), gate on via port 0x61.
2. Record TSC and LAPIC current-count (LAPIC timer set to divide 16,
   initial count 0xFFFFFFFF, one-shot).
3. Poll the PIT output bit until it flips.
4. Record TSC and LAPIC counts; `tsc_hz = delta_tsc * 100`,
   `lapic_hz = delta_lapic * 100 * 16` (bus ticks per second before
   divide).
5. Sanity: TSC between 100 MHz and 10 GHz, LAPIC between 1 MHz and
   10 GHz, else panic.

CPUID leaf 0x15/0x16 are not used yet: TCG reports nothing useful there
and the PIT method works on every machine this project targets.

## 2. Tick

`CONFIG_HZ` = 250 → period 4 000 000 ns. `arch_timer_start_tick(hz)`
programs the LAPIC timer in periodic mode with
`initial = lapic_hz / 16 / hz` and unmasks the timer LVT on vector
`X86_VECTOR_TIMER` (allocated from the dynamic range at init). Each CPU
starts its own timer (BSP in `timer_init`, APs in the SMP PR).

`timer_tick_isr` (registered on the timer vector):
1. `this_cpu()->ticks++`.
2. `timer_run_expired(this_cpu queue, clock_now_ns())`.
3. `timer_tick_hook(now)` if one is registered (the scheduler registers
   `sched_tick`).
4. EOI is done by the arch dispatch tail, not here.

## 3. Timer queue

```c
enum timer_state { TIMER_IDLE, TIMER_PENDING, TIMER_RUNNING };

struct timer {
    struct list_node link;
    uint64_t expires_ns;
    void (*fn)(struct timer *t, void *arg);
    void *arg;
    unsigned cpu;
    enum timer_state state;
};

struct timer_queue { spinlock_t lock; struct list_node pending; unsigned count; };
```

`timer_start(t, delay_ns)`: panics if state != IDLE; sets expiry =
now + delay; inserts sorted (ascending) into the local CPU's queue;
state PENDING. Runs with interrupts disabled around the queue lock and
records `t->cpu`.

`timer_cancel(t)`: locks the queue of `t->cpu`; if PENDING, unlinks and
returns true; if RUNNING (its callback is executing on another CPU) it
returns false and the caller must not free the timer until the callback
finishes (callbacks are short; a `timer_cancel_sync` that spins on
RUNNING arrives with the SMP PR).

`timer_run_expired(q, now)`: under the lock, pop entries while
`head.expires_ns <= now`, mark RUNNING, release the lock, call `fn`,
mark IDLE, re-take the lock. A callback may re-arm its own timer.

## 4. Sleeping and delays

`thread_sleep_ns(ns)`: embeds a timer in the caller's stack frame, callback
sets a flag and `sched_wake`s the thread, then `wait_event` on the flag.
Deviation is bounded by one tick period plus scheduling latency.

`udelay(us)` / `ndelay(ns)`: spin on `clock_now_ns()` with
`arch_cpu_relax()`. Usable before the scheduler exists and in interrupt
context; never for more than a few milliseconds by convention.

## 5. Arch interface

```c
void     arch_timer_calibrate(void);         /* BSP once; fills tsc_hz, lapic_hz */
void     arch_timer_start_tick(unsigned hz); /* this CPU */
void     arch_timer_stop_tick(void);
uint64_t arch_clock_read(void);              /* TSC */
uint64_t arch_clock_hz(void);
const char *arch_clock_name(void);
unsigned arch_timer_vector(void);            /* vector the tick arrives on */
```

## 6. Failure modes

| Condition | Behaviour |
|---|---|
| PIT never flips within ~1 s of TSC spinning | panic "timer calibration timed out" |
| implausible frequencies | panic |
| timer_start on PENDING/RUNNING timer | panic |
| timer freed while PENDING | undetectable; convention + KASSERT on state in debug |
| tick before timer_init | vector has no handler → arch_trap_unhandled logs spurious |
