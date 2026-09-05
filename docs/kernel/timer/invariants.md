# Timer Subsystem: Invariants

Check mechanism per item: **assert**, **test**
(`docs/kernel/timer/testing.md`), or **review**.

**T1. `clock_now_ns` is monotonic and lock-free.** It reads one counter
(`lfence; rdtsc`) and multiplies; the base is captured once in
`timer_init`. Check: test `timer` (1000 consecutive reads never
decrease); review (single-socket TSC; cross-socket synchronisation is a
documented gap).

**T2. The nanosecond conversion uses a fixed-point multiply with a
128-bit intermediate and no 128-bit division.** `g_ns_mult =
(1e9 << 32) / hz` is computed once with 64-bit arithmetic;
`clock_now_ns` computes `(delta * g_ns_mult) >> 32` as
`unsigned __int128`. Found during bring-up: `(delta * 1e9) / hz` on a
128-bit value emitted a call to `__udivti3`, which the freestanding
kernel does not link (no compiler-rt), and the link failed. The kernel
must never rely on a 128-bit divide. Check: link (an undefined
`__udivti3` fails the build); review.

**T3. A timer is armed on the calling CPU and lives on that CPU's queue
until it fires or is cancelled.** `timer_start` records `t->cpu`;
`timer_cancel` locks `g_queues[t->cpu]`. Callbacks run on the arming
CPU. Check: review; test `timer`.

**T4. `timer_start` on a `TIMER_PENDING` timer panics.** A double arm
would link the node twice. IDLE is the normal case; RUNNING is accepted
so a callback can re-arm its own timer (T13). Check: assert.

**T5. A timer's memory is owned by the caller and must outlive any
PENDING or RUNNING state.** `timer_cancel` returning false for a
RUNNING timer means the callback may still be executing. Check:
review; `thread_sleep_ns` waits for the callback's flag before its
stack timer goes out of scope (test `sleep`).

**T6. Callbacks run in interrupt context with the queue lock released
and interrupts disabled.** `run_expired` unlocks around each `fn`, marks
the timer RUNNING before and IDLE after unless the callback re-armed it.
Callbacks may only call interrupt-safe functions. Check: review; test
`timer` (callback runs, state returns to IDLE), test `sleep` (callback
performs `waitqueue_wake_all`).

**T7. The queue is sorted by expiry; expiry processing stops at the
first future timer.** Sorted insert in `timer_start`, head check in
`run_expired`. Check: test `timer` (timers armed 30/10/20 ms fire in
order 10, 20, 30).

**T8. The tick period is `TICK_NS` = 1e9 / `CONFIG_HZ` and the tick
runs on every CPU that called `timer_init_cpu`.** The LAPIC timer is
periodic with `lapic_hz / CONFIG_HZ` counts. Check: test `timer` (tick
count over 40 ms matches `elapsed / TICK_NS` within ±2).

**T9. Calibration results are bounded or the kernel refuses to
continue.** TSC in [100 MHz, 10 GHz], LAPIC timer in [1 MHz, 10 GHz],
PIT terminal count reached within a bounded spin. Check: assert.

**T10. The tick handler does bounded work.** Increment, expire the
head run, one hook call. No allocation, no logging on the hot path.
Check: review.

**T11. Exactly one tick hook, set before the tick can observe it.**
`sched_init` sets `sched_tick` after `timer_init`; the hook pointer is
checked for NULL on every tick. Check: review.

**T12. Delays are busy-waits with no side effects.** `ndelay`/`udelay`
never block or take locks, so they are usable before the scheduler and
in interrupt context. Check: review; used by the `timer` and
`irq-route` tests inside thread 0 before any sleep facility is proven.

**T13. A callback may re-arm its own timer.** `timer_start` accepts a
timer in state IDLE or RUNNING (RUNNING means "its callback is
executing"); only PENDING is a double start and panics. After the
callback returns, `run_expired` sets IDLE only if the state is still
RUNNING, so a re-armed (PENDING) timer is left on the queue. An earlier
draft accepted only IDLE, which made the documented periodic pattern
panic (found in review, PR #3). Check: test `timer`, self-rearming
callback fires exactly four times and ends IDLE.

## Gaps

- No cross-CPU TSC synchronisation check (SMP PR, or a switch to the
  HPET/ACPI PM timer as a fallback clock source).
- (closed by the lifetime pass) `timer_cancel_sync` waits for a RUNNING callback; the old gap read: `timer_cancel` cannot wait for a RUNNING callback (`timer_cancel_sync`
  planned).
- The queue is a sorted list; insertion is O(n).
