# NEXT SUBSYSTEM — the Linux personality has no timerfd

> Constitution §68 report. This PR adds the report and the probe
> (`tools/timerfd-probe.py`); the `timer_obj` arm/disarm surface, the three
> `timerfd` doors and the `lxtest` checks described under "Design" and the
> edits in "Affected files" are planned work that lands in the implementation
> PR that follows, gated on CI. As the report was committed (before PR #287),
> `timerfd_create` returned `-ENOSYS`.
>
> **Built in PR #287.** The implementation landed the `timer_obj` arm/disarm
> surface in `kernel/io/timerobj.c`, wired the three doors in
> `compat/linux/syscalls.c`, and added the `lxtest` checks; both arches boot
> PASS and `host-test` passes. Every present-tense statement below describes
> the state the report measured, before this implementation.

## Problem

`timerfd_create(2)` and its `timerfd_settime(2)`/`timerfd_gettime(2)`
companions (x86-64 283/286/287, AArch64 85/86/87) are unimplemented: their
slots in `linux_table` are NULL, so the dispatcher routes them to `lx_unknown`
and they return `-ENOSYS`. `timerfd` is a timer as a file descriptor — a fd
that becomes readable when a one-shot or periodic timer has expired, whose
8-byte read returns the number of expirations since the last read. It is the
timer source `epoll`/`poll` loops arm (libevent, libuv, glibc's own timer
helpers), so a Linux program that wants a waitable timer dies at creation.

The kernel already has the exact object. The aio-timer unit
(`docs/audit/next-subsystem-aio-timer.md`) built **a timer as a readiness
kobject** — `kernel/io/timerobj.c`, whose header opens "a timer as a
submittable I/O object (a timerfd)". It carries a one-shot or periodic timer,
counts expirations, becomes `COSMO_IO_READABLE` while the count is non-zero,
and its 8-byte `read` returns the count and resets it — byte-for-byte Linux
`timerfd` read semantics. It rides `poll`/`select` and the I/O ring's
POLL/READ path already. What it lacks is the `timerfd` control surface: it is
armed once at creation (`timer_obj_create`, `initial_ns` must be non-zero,
`kernel/include/kernel/timerobj.h:17`) and offers no way to re-arm, disarm, or
read the time remaining. This unit adds that surface and the three doors.

## Probe

`tools/timerfd-probe.py` adds `LX_timerfd_create` to both syscall-number
headers and one check to `tests/linux/lxtest.c` (which runs in the standard
boot):

```
LXTIMERFD: timerfd_create unimplemented -> -ENOSYS; no waitable timer
```

The check asserts `timerfd_create(CLOCK_MONOTONIC, 0)` returned `-ENOSYS` at
the report commit, before the implementation PR. The marker prints only when
the syscall really returned `-ENOSYS`, so grepping it cannot show a false
result after a failed check.

## Why it matters

- **The waitable timer `poll` loops need.** Without `timerfd` an event loop
  has no fd-based timer; it falls back to computing `poll` timeouts by hand, or
  the program simply fails to start. Every mainstream Linux async runtime
  reaches for `timerfd`.
- **It is in pattern and the mechanism exists.** `timerobj` already is a
  timerfd in all but its control surface — periodic re-arm, an expiration
  count, readiness and the resetting 8-byte read are built and tested
  (aio-timer unit). This unit adds arm/disarm/remaining and the Linux
  marshalling, not a new object.

## The implementation before this unit

| piece | where | what it does |
|---|---|---|
| timer kobject | `kernel/io/timerobj.c` (`timer_obj_create`) | a one-shot/periodic timer as a `kobject_io_type`: `read` returns the expiration count and resets, `ready` is `COSMO_IO_READABLE` while the count is non-zero, `poll_wq` is its wait queue, `set_nonblock` is per object, `release` is synchronous (`timer_cancel_sync` before free) |
| the kernel timer | `kernel/include/kernel/timer.h` (`timer_setup`/`timer_start`/`timer_cancel_sync`) | one-shot arming against the monotonic counter; a periodic timer re-arms itself from its callback (as `timerobj` does) |
| clocks | `clock_now_ns()` (monotonic) and `clock_realtime_ns()` (monotonic + boot RTC offset); `clock_read(clk)` in `syscalls.c:1534` picks one | monotonic and wall-clock reads; `timer_start` always arms against the monotonic counter |
| timespec marshalling | `ns_from_timespec` (`syscalls.c:249`, validates and clamps at 2^62 ns), `put_timespec` (`:266`) | `struct lx_timespec` ↔ ns, reused by `nanosleep`/`clock_gettime` |
| dispatch | `linux_table[LX_NR_MAX]`; unlisted → `lx_unknown` | the three `timerfd` calls are unlisted → `-ENOSYS` |

## Design

### 1. The `timer_obj` arm/disarm surface

`timerobj.c` keeps its current create (the native `SYS_timer_create` arms at
creation) and gains three operations, all monotonic-ns at the kernel boundary
(the Linux door does any wall-clock conversion):

- **`timer_obj_create_disarmed(bool nonblock, struct kobject **out)`** — the
  object with its timer set up but not started, `count == 0`, `interval == 0`,
  not armed. `timerfd` fds start disarmed.
- **`timer_obj_settime(obj, initial_ns, interval_ns, old_remaining_ns,
  old_interval_ns)`** — cancel any pending expiry, report the previous
  remaining time and interval through the out-params (for `old_value`), reset
  the expiration count to zero (Linux resets on settime), set the new interval,
  and if `initial_ns != 0` arm the timer for it and record the deadline; if
  `initial_ns == 0` leave the object disarmed. **`initial_ns == 0` is this
  function's sole disarm signal**, so the Linux door must map "disarm" to a
  zero `initial_ns` and an armed request to a non-zero one — never let a
  computed delay collapse to zero mean disarm (see the door below). The cancel
  is
  `timer_cancel_sync` and runs **before** the state is taken under the object
  lock — the fire callback takes that same lock, so cancelling under it would
  deadlock (the `release` path already cancels outside the lock).
- **`timer_obj_gettime(obj, remaining_ns, interval_ns)`** — `remaining` is
  `max(0, deadline - clock_now_ns())` while armed, else zero; `interval` is the
  current interval. `timerfd_gettime` always reports the remaining time
  relative, even for an absolute-armed timer.

The object tracks `deadline_ns` (the next monotonic expiry) and an `armed`
flag. Both the arm and each periodic re-arm set `deadline_ns = clock_now_ns()
+ delay` immediately before `timer_start(delay)`, so the tracked deadline and
the timer the callback actually arms follow the same schedule — `gettime`
cannot drift from the real expiry. (The fire callback already re-arms with
`timer_start(interval_ns)` from the moment it runs; it now records that same
`now + interval_ns` as the deadline.) The expiration count, the resetting
read, readiness and `poll_wq` are unchanged from `timerobj`.

A `timer_obj_from_kobject(obj)` accessor (like `aio_ring_from_kobject`) returns
the `timer_obj` or NULL, so the Linux door can confirm an fd is a timerfd
before a settime/gettime and return `-EINVAL` otherwise.

### 2. The Linux doors

- **`timerfd_create(clockid, flags)`** — `clockid` must be `CLOCK_MONOTONIC`,
  `CLOCK_REALTIME`, or `CLOCK_BOOTTIME`; `CLOCK_BOOTTIME` is accepted and
  aliased to monotonic (this kernel does not suspend, so boot time and
  monotonic time are identical), and every other `clockid` returns `-EINVAL`.
  Only `CLOCK_REALTIME` reads the wall clock; the other two use the monotonic
  counter. `flags` must be a subset of
  `TFD_NONBLOCK | TFD_CLOEXEC` (`-EINVAL` otherwise). It creates a disarmed
  `timer_obj`, remembers whether the clock is the wall clock (so settime's
  absolute conversion uses the right `now`), sets the object's non-blocking
  mode from `TFD_NONBLOCK`, marks the handle close-on-exec from `TFD_CLOEXEC`,
  and installs it as a **read-only** fd (a timerfd is not writable).
- **`timerfd_settime(fd, flags, new_value, old_value)`** — `flags` must be a
  subset of `TFD_TIMER_ABSTIME` (`-EINVAL` otherwise). Copy in the
  `itimerspec`, convert `it_value` and `it_interval` to ns. Disarm (passing
  `initial_ns == 0` to `timer_obj_settime`) **iff the raw `it_value` is zero**
  — the disarm decision is the itimerspec, never the computed delay. For an
  armed request the monotonic initial delay is `it_value` directly for a
  relative timer, or `it_value - now` for `TFD_TIMER_ABSTIME` (where `now` is
  `clock_realtime_ns()` or `clock_now_ns()` by the fd's clock); an absolute
  deadline that has already passed clamps to a minimum of **1 ns, not 0**, so
  it fires immediately rather than disarming (Linux fires a past absolute timer
  at once). If `old_value` is non-NULL write the previous remaining time and
  interval back as an `itimerspec`.
- **`timerfd_gettime(fd, curr_value)`** — `timer_obj_gettime`, then write the
  remaining time and interval as an `itimerspec`.

### 3. Lifetime

Unchanged from `timerobj`: a blocked reader or a parked I/O-ring entry holds a
reference, and `release` runs `timer_cancel_sync` before the free, so no fire
callback can touch a freed object. `settime`'s re-arm cancels synchronously
before re-arming, so a stale expiry cannot survive a re-arm.

## Affected files

| file | change |
|---|---|
| `kernel/io/timerobj.c`, `kernel/include/kernel/timerobj.h` | disarmed create, `settime`/`gettime`, `timer_obj_from_kobject`, deadline/armed tracking |
| `compat/linux/nr_x86_64.h`, `nr_aarch64.h` | `LX_timerfd_create` (283 / 85), `LX_timerfd_settime` (286 / 86), `LX_timerfd_gettime` (287 / 87) |
| `compat/linux/linux_abi.h` | `struct lx_itimerspec`, `TFD_NONBLOCK`/`TFD_CLOEXEC`/`TFD_TIMER_ABSTIME` |
| `compat/linux/syscalls.c` | `lx_timerfd_create`/`_settime`/`_gettime` + the three table entries |
| `tests/linux/lxtest.c` | the timerfd checks |
| `README.md` | Status entry |

## APIs

The Linux `timerfd_create(2)`, `timerfd_settime(2)` and `timerfd_gettime(2)`
system calls. No native ABI change: the arm/disarm surface is added to the
existing `timer_obj` kobject and could back a native door later, but the gap
this closes is the Linux one. `CLOCK_MONOTONIC`, `CLOCK_REALTIME` and
`CLOCK_BOOTTIME` (aliased to monotonic) are accepted; any other `clockid` is
`-EINVAL`.

## Tests

Planned for the implementation (`tests/linux/lxtest.c`, which runs in the
standard boot).

| test | proves |
|---|---|
| `timerfd` one-shot | `timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK)` returns an fd; a relative `settime` of ~40 ms makes it readable (`poll`); the 8-byte read returns a count ≥ 1; a second read is `-EAGAIN` (drained, non-blocking) |
| `timerfd` periodic | `settime` with a ~20 ms value and ~20 ms interval, then accumulate expirations across blocking reads (or `poll`+read) **until the count reaches 2**, under a generous overall deadline (say 2 s) — a wait for the count, never "N fires after a fixed sleep", so a slow host is late, not wrong |
| `timerfd` gettime | on an armed one-shot, `timerfd_gettime` reports a remaining `it_value` in `(0, the set value]` and the interval it was set with; after a disarming `settime` (`it_value == 0`) it reports `0`/`0` and `poll` is not readable |
| `timerfd` absolute | `settime` with `TFD_TIMER_ABSTIME` at `clock_gettime(MONOTONIC) + ~40 ms` becomes readable within a bound |
| `timerfd` past absolute | `settime` with `TFD_TIMER_ABSTIME` at a deadline already in the past (`clock_gettime(MONOTONIC) − 1 s`) and a non-zero `it_value` fires at once — readable immediately, not disarmed |
| `timerfd` errors | a bad `clockid` and a settime on a non-timerfd fd are `-EINVAL` |

**Planned mutations** (each alone, boot confirmed):
- `timerfd_settime` not subtracting `now` for `TFD_TIMER_ABSTIME`: the absolute
  timer is armed ~a wall-clock-era into the future, never fires in the bound,
  and the absolute test's `poll` times out.
- a passed absolute deadline clamping to `0` rather than `1 ns`: the past-abs
  timer disarms instead of firing, so that test never becomes readable.
- `timer_obj_gettime` returning the stored deadline instead of
  `deadline - now`: the remaining-time check (`it_value <=` the set value)
  fails.
- a disarming `settime` (`it_value == 0`) not cancelling: the object stays
  armed, so the "disarmed → not readable / gettime zero" check fails.

## Benchmarks

None.

## Risks

- **Absolute `CLOCK_REALTIME` timers do not track wall-clock jumps.** The
  absolute deadline is converted once to a monotonic delay at `settime`; if the
  wall clock is stepped afterward the expiry does not move (Linux would adjust,
  and `TFD_TIMER_CANCEL_ON_SET` exists for exactly this). This kernel has no
  wall-clock stepping interface, so the case cannot arise in practice; the flag
  is rejected rather than silently accepted.
- **Timing bounds in the test.** The one-shot and periodic checks assert
  against generous millisecond bounds and count-at-least, not exact counts, to
  stay off the loaded-host timing-flake family (the rule the lockup and timer
  units learned): wait for the readiness or the count, never for "N fires after
  a fixed sleep".
- **Re-arm versus a racing expiry.** `settime` cancels synchronously before it
  re-arms and resets the count under the lock, so an in-flight fire cannot land
  against the new setting; this mirrors `release`'s cancel-before-free.

## Alternatives considered

- **A fresh timerfd object rather than extending `timerobj`.** The timer
  kobject already is a timerfd in all but the control surface — periodic
  re-arm, the expiration count, readiness and the resetting read are built and
  proven. A second object would duplicate all of that; extending it is why this
  unit is small.
- **Only `timerfd_create`, faking settime.** A timerfd is useless without
  `settime` (it starts disarmed), so all three calls ship together;
  `gettime` is cheap once the deadline is tracked and completes the trio a
  program expects.
- **Supporting every `clockid`.** `CLOCK_MONOTONIC`, `CLOCK_REALTIME` and
  `CLOCK_BOOTTIME` are what programs arm timerfds with; `BOOTTIME` is aliased
  to monotonic (no suspend). The alarm clocks (`BOOTTIME_ALARM`,
  `REALTIME_ALARM`) are wake-from-suspend features this kernel has no basis
  for, so they are rejected (`-EINVAL`) rather than aliased.
