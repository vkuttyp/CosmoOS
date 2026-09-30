# NEXT SUBSYSTEM — the async I/O ring cannot submit a timer

> Constitution §68 report. This PR adds the report and the probe
> (`tools/aio-timer-probe.py`); the timer object and syscall described under
> "Design" and the edits in "Affected files" are planned work that lands in
> the implementation PR that follows, gated on CI. As committed here, a timer
> cannot be submitted to the ring.

## Problem

The asynchronous I/O ring (`kernel/io/aio.c`, `SYS_aio_create` /
`aio_submit` / `aio_wait`) drives **any object with a readiness operation**:
its ops are `COSMO_AIO_NOP`, `READ`, `WRITE`, `PREAD`, `PWRITE`, `FSYNC` and
`POLL`, each on a handle to a `kobject` that carries `io_ops` (`ready`,
`poll_wq`, read/write). Files, sockets and — since the device-readiness unit
(`docs/audit/next-subsystem-device-readiness.md`) — devices are all
submittable that way.

Prompt #2 §23 says async I/O "must work for files, sockets, devices,
**timers**, IPC, VM operations". A timer is not among the objects the ring
can drive. There is no timer object type, and no timer op: the op space
tops out at `COSMO_AIO_POLL` (6). The only timer the ring has is
`aio_wait`'s **whole-call timeout** (the `aio_alarm` in `aio.c`), which
returns a bare `0` — not a completion carrying `user_data`. So "wake me in
20 ms" cannot be submitted as one entry among others, and a program cannot
tell "the timer fired" from "nothing else was ready". The inventory row
records this as `a timer as a submittable object ... not shown by any test
-- unverified` (`docs/audit/2026-09-deferred-work-inventory.md`, the async
I/O row); it is unverified because it is unbuilt.

### Measured

`tools/aio-timer-probe.py` adds a block to init's async-I/O self-test that
parks a poll on an empty pipe and then waits 20 ms (one debug boot, x86-64):

```
AIOTIMER: op max COSMO_AIO_POLL(6), no timer op; parked poll + 20ms wait -> 0 completions (bare timeout, no user_data); no timer object to submit
```

- **The op space has no timer.** The highest op is `COSMO_AIO_POLL` (6);
  there is no `COSMO_AIO_TIMEOUT` and no handle type that represents a timer.
- **The wait timeout is bare.** A parked entry plus a 20 ms wait returns `0`
  completions when the timeout expires — the same value as "nothing was
  ready". No completion is produced, so no `user_data` distinguishes a fired
  timer from an idle interval.

## Why it matters

- **A timer cannot be multiplexed with I/O in one ring.** The whole point of
  the ring is to wait on many things at once and learn which became ready.
  A timeout is the one thing that is a property of the *call*, not an entry,
  so a program that wants "these reads, or 20 ms, whichever first, and tell
  me which" cannot express it: the 20 ms is a bare return with no identity.
- **The claim is stated and unmet.** Prompt #2 §23 lists timers among what
  async I/O must serve, and the design says the ring drives any object with
  a readiness operation — but the one object class that is purely a
  readiness source, a timer, is the one that does not exist. This is the
  same shape as a documented capability with nothing behind it.

## The implementation before this unit

| piece | where | what it does |
|---|---|---|
| the ring | `aio_create` / `aio_submit` / `aio_wait` (`kernel/io/aio.c`) | parks entries whose object is not ready, runs them as `poll_wq` wakes fire |
| submittable objects | `kobject` with `io_ops` (`ready`, `poll_wq`, read/write) | files, sockets, devices, and the ring itself |
| ops | `COSMO_AIO_*` (`uapi/cosmo/syscall.h`) | `NOP`/`READ`/`WRITE`/`PREAD`/`PWRITE`/`FSYNC`/`POLL`; max `POLL` = 6 |
| the only timer | `aio_alarm` in `aio_wait` | the whole-call timeout; wakes the waiter, returns `0`, is not an entry |

## Design

### 1. A timer as a `kobject` with a readiness operation

A new object type, `timer` (a timerfd in Linux terms), with `io_ops`:
- `ready` → `COSMO_IO_READABLE` once the timer has expired at least once;
- `poll_wq` → a wait queue the timer's callback wakes;
- read → the number of expirations since the last read, then reset to zero
  (a `uint64_t`, as `timerfd` reads).

Because it is a `kobject` with a readiness operation, it needs **no new AIO
op**: `COSMO_AIO_POLL` on its handle completes with `COSMO_IO_READABLE` when
it fires, carrying its `user_data`, and `COSMO_AIO_READ` completes with the
expiration count. It also serves `poll`/`select` and a plain blocking read,
for free, like every other io object.

### 2. Creating and arming it

A syscall, `SYS_timer_create`, returns a handle for a timer armed with an
initial delay and an optional interval (nanoseconds; interval 0 is
one-shot). The handle is closed like any other. Arming at create time keeps
the first unit minimal; a `settime` to re-arm is a later addition, noted and
not built here.

### 3. Lifetime

The object owns a kernel `timer`. Its `release` must cancel the timer and
be sure the callback is not still in flight before the object is freed —
the `tcp-pcb-timer-free` hazard (`docs/audit/next-subsystem-lifetime-windows.md`):
a timer armed by an object that outlives it fires into freed memory. The
callback takes only what it needs to wake the queue and bump the count, and
`release` cancels-then-quiesces.

`release`, and the timer cancel, run only when the object's *last* reference
drops. A submitted timer has **two**: the open handle's, and one the ring
takes at submit (`aio_submit` → `handle_lookup` → `kobject_get`) that the
parked entry holds and `req_free` drops. Neither alone frees it — closing the
handle leaves the ring's reference, and closing the ring (`aio_release` drops
every parked entry) leaves the handle's — so the timer is cancelled when
whichever closes *last* closes, in either order. That the ring holds a
reference while an entry is parked is exactly what makes the hazard safe: a
submitted timer cannot be freed under the ring. "Cancel at `close`" is the
un-submitted case, where the handle is the only reference.

## Affected files

| file | change |
|---|---|
| `kernel/io/timerobj.c` (new) | the timer `kobject`, its `io_ops`, the callback, `release` that cancels the timer |
| `kernel/include/uapi/cosmo/syscall.h` | `SYS_timer_create` and its arg struct (initial/interval ns) |
| `kernel/syscall/native.c` | wire `SYS_timer_create` |
| `userland/lib/…` | a `cosmo_timer_create` wrapper |
| `docs/kernel/io/*.md` | a timer is a submittable readiness object |
| `README.md` | Status entry |

## APIs

One new syscall, `SYS_timer_create(initial_ns, interval_ns) -> handle`, and
one new `kobject` type. No new AIO op — the timer rides the existing
`POLL`/`READ` path. No change to existing ABI.

## Tests

Planned for the implementation; the probe's userland block grows into it.

| test | proves |
|---|---|
| `aio-timer` (userland, in init's async-I/O suite) | a timer armed for 20 ms, submitted to the ring as `POLL`, completes with its `user_data` after it fires and not before; `READ` returns the expiration count; a periodic timer fires repeatedly; `F_NOWAIT` before it fires is `-EAGAIN`; a timer created and closed **without** submitting cancels its timer at `close` (the handle is its only reference); and with a timer submitted as a parked `POLL`, closing the ring drops the parked entry (`aio_release`) while an open handle keeps the timer, which is cancelled only once that handle is closed too — the test closes both and checks the entry was dropped |

**Planned mutations** (each alone, boot confirmed):
- the callback not waking `poll_wq`: the parked poll never completes and the
  test times out.
- `ready` not reflecting the expiration: the poll completes before the timer
  fires (or never), and the "not before" assertion fails.
- read not resetting the count: a periodic timer's second read is wrong.
- `release` not cancelling the timer: a create-and-close loop (no submit, so
  the handle is the only reference) under the poison fires a cancelled timer
  into freed memory (the `tcp-pcb-timer-free` shape).

## Benchmarks

None.

## Risks

- **The timer outliving the object** — the central hazard, handled by
  `release` cancelling and quiescing the callback (§Design 3), and bounded
  while submitted by the ring's reference. A test arms and closes without
  submitting, in a loop, to drive `release` at `close`.
- **Interval drift and coalescing** — a periodic timer that fires faster
  than it is read must accumulate a count, not queue N wakeups; the count is
  a single `uint64_t`, read-and-reset, so a slow reader loses nothing but
  the individual edges, which is `timerfd`'s contract.

## Alternatives considered

- **A `COSMO_AIO_TIMEOUT` op** (a per-entry timer, as `io_uring` has). It is
  smaller, but it serves only the ring; a timer object also answers
  `poll`/`select` and a blocking read, and matches the ring's own model —
  "any object with a readiness operation" — rather than adding a special
  case to the submission path. The object is the more general primitive.
- **Exposing `aio_wait`'s alarm as a completion.** The alarm is intrinsically
  the call's timeout; making it an entry would be a second timer mechanism
  beside a proper timer object, for no gain.
