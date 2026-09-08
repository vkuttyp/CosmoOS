# Timer Subsystem: Testing

## Self-tests

### `timer` (`kernel/scheduler/schedtest.c`, `selftest_timer`)

Runs in thread 0 with the tick active, using `udelay` (busy-wait) rather
than sleep so the scheduler is not involved.

| Step | Proves |
|---|---|
| a probe on the test's stack re-arms itself from its callback every 2 ms; four fires within 500 ms, then `TIMER_IDLE`; on any other outcome the timer is cancelled synchronously *before* the check returns | re-arming from a callback; and that a failing test leaves no armed timer behind — an earlier version returned with the probe still armed when the fourth fire was late under host load, and the fifth fire wrote into the dead frame and panicked the kernel (found by the USB unit's chain, 2026-09-08) |
| 1000 consecutive `clock_now_ns()` reads never decrease | T1 monotonic clock |
| `udelay(40 ms)`: clock advanced ≥ 40 ms and < 80 ms | delay honours the clock; TCG slack allowed |
| ticks advanced by `elapsed / TICK_NS` ± 2 | T8: the LAPIC periodic timer runs at `CONFIG_HZ` relative to the TSC; the ±2 covers tick phase at both ends |
| timers armed for 30, 10, 20 ms fire in order 10, 20, 30 (`fired_at == {2,3,1}`) within 60 ms | T7 sorted queue; callbacks run; `timer_pending_count() >= 3` while armed |
| all three timers back to `TIMER_IDLE` | T6 state handling after the callback |
| arm 20 ms, `timer_cancel` → true, second `timer_cancel` → false, nothing fires in 30 ms | cancellation and state |
| `timer-cancel-sync` (`kernel/core/quiescetest.c`): a callback spinning 20 ms on CPU 1 is outlasted by `timer_cancel_sync` (≥ 10 ms, `timer_sync_waits` +1); a callback that re-arms every 1 ms is stopped for good (IDLE, no fire in 30 ms) | the sync form and the re-arm race (`docs/kernel/quiesce/testing.md`) |

### `sleep` (`selftest_sleep`)

`thread_sleep_ms(20)` elapsed in `[20 ms, 20 ms + 3·TICK_NS + 10 ms)`;
`thread_sleep_ns(1 ms)` never returns early. Proves the timer callback →
`waitqueue_wake_all` → `sched_wake` path and that the stack timer is idle
before the frame is discarded (T5).

### Calibration (every boot)

`timer_init` logs `timer: tsc at <MHz> MHz, tick 250 Hz` and, in debug
builds, the raw `calibrated over 10 ms: TSC <Hz>, LAPIC timer <Hz> (/16)`
line. Implausible results panic (T9), so a passing boot is itself a
calibration test. Measured on QEMU TCG: TSC 996–1060 MHz, LAPIC
62–67 MHz.

### Indirect coverage

Every `preempt`, `mutex`, `semaphore`, `completion`, and `waitqueue`
self-test depends on the tick (slices) and on sleep timers; the
`irq-route` test uses `udelay` windows to count PIT interrupts.

## Bug the build caught

The first `clock_now_ns` divided a 128-bit product by the frequency;
clang emitted `__udivti3` and the link failed. The fix (fixed-point
multiplier, T2) is verified by the link succeeding and by the tick-rate
check above, which would drift if the multiplier were wrong.

## Gaps and planned tests

- No cross-CPU cancel (SMP PR).
- Timing bounds are loose for TCG; tighten under KVM/HVF.
- A host test for the sorted queue and `run_expired` state machine is
  straightforward (`timer.c` depends on percpu, spinlock, and arch clock,
  all shimmable) and is planned.

## Running

```sh
make test          # SELFTEST: timer ... ok, SELFTEST: sleep ... ok
```

## The tick against the clock

`timer` compares the tick counter with the monotonic clock over a 40 ms
window. The tolerance is not symmetric, and the reason is worth stating:
a tick interrupt that arrives while the previous one is still pending is
coalesced -- the counter advances once -- so on a busy host emulating
this machine the tick count *lags* the clock, by more the busier the
host is. That is the platform being honest. Ticks *ahead* of the clock
would mean a tick counted without time passing, which is a real bug, so
that side keeps a bound of two.

The lag allowance is a quarter of the window (ten ticks in forty). It
was two, and the test failed about half the time in the busiest shape
the chain runs (`QEMU_KBD=hub` on AArch64, where the hub's worker and
the keyboard add interrupt work) on `main` as well as on a branch --
which is what distinguishes a tolerance that is too tight from a
regression.
