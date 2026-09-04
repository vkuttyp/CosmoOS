# Timer Subsystem: Architecture

## 1. Purpose

Provide time to the kernel: a monotonic nanosecond clock, a periodic
per-CPU tick that drives scheduling, and one-shot timers with callbacks
for sleeps and timeouts. Wall-clock time (RTC) is not part of this
phase.

## 2. Where it sits

```text
   scheduler (slices, sleep)   drivers (timeouts)
              │ timer_start / clock_now_ns / thread_sleep_ns
              ▼
   kernel/timer/      clock source registry, per-CPU timer queue, tick
              │ arch_timer_*   ▲ tick interrupt
              ▼
   kernel/arch/x86_64/timer.c  LAPIC timer (per CPU), TSC clock,
                               PIT for one-time calibration
```

The timer depends on the interrupt subsystem (its tick is an interrupt)
and on per-CPU data. The scheduler depends on the timer; the timer never
calls into the scheduler except through the policy tick hook registered
by `sched_init`.

## 3. Responsibilities

- `clock_now_ns()`: monotonic, starts near 0 at boot, resolution of the
  clock source (TSC: tens of ns).
- Per-CPU tick at `CONFIG_HZ` (250 Hz): advances `percpu->ticks`, expires
  local timers, calls the scheduler tick hook.
- `struct timer`: one-shot, armed on the calling CPU, cancellable,
  callback runs in interrupt context on that CPU.
- Calibration at boot: measure the LAPIC timer and the TSC against the
  PIT once; APs reuse the result.
- `udelay`/`ndelay` busy-waits on the clock for driver bring-up code.

## 4. Non-responsibilities (later)

- Wall clock, RTC, NTP-style adjustment.
- Tickless idle, high-resolution timers beyond the tick, timer wheels.
- HPET as a clock source (the `struct clock_source` interface admits it).
- TSC synchronisation checks across sockets.

## 5. Interfaces

| Header | Provides |
|---|---|
| `kernel/timer.h` | `timer_init`, `clock_now_ns`, `struct timer`, `timer_start`, `timer_cancel`, `udelay`, `ndelay`, `timer_tick_hook` |
| `arch/timer.h` | `arch_timer_calibrate`, `arch_timer_start_tick`, `arch_clock_read`, `arch_clock_hz`, `arch_clock_name` |

## 6. Data structures

`struct clock_source { name, hz, read }` (one active, TSC), `struct timer`
(intrusive list node, expiry in ns, callback, argument, owning CPU,
state), per-CPU `struct timer_queue { spinlock, sorted list, count }`.

## 7. Concurrency

Each CPU's timer queue has its own spinlock (irqsave). A timer is always
on the queue of the CPU that armed it; cancellation from another CPU
takes that queue's lock. Callbacks run with the queue lock released and
interrupts disabled. Lock order: `timer_queue.lock` is a leaf, except
that callbacks may take scheduler locks after it is released.

## 8. Memory ownership

Timers are caller-owned embedded objects; the queue holds an intrusive
link only. A timer must be cancelled before its memory is freed; the
state field makes a double start or a free-while-armed detectable.

## 9. Error handling

`timer_start` on an already armed timer panics. `timer_cancel` returns
whether it was pending. Calibration that yields an implausible frequency
panics: a wrong tick would make every sleep wrong.

## 10. Performance

Insert is O(n) in queued timers on the CPU; expiry checks the head only.
Adequate for the numbers of timers a kernel of this size has; a wheel
replaces it behind the same interface when measurements demand.

## 11. Security

Nothing user-reachable. Callbacks are trusted kernel code.

## 12. Testing

Self-tests: clock monotonicity and rate sanity against tick counts,
timer expiry order and cancellation, sleep bounds, callback context. See
`testing.md`.

## 13. Future

HPET/ACPI PM timer clock sources, tickless idle, hrtimers, TSC deadline
mode, AArch64 generic timer behind `arch/timer.h`.
