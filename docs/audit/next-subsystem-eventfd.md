# NEXT SUBSYSTEM — the Linux personality has no eventfd

> Constitution §68 report. This PR adds the report and the probe
> (`tools/eventfd-probe.py`); the eventfd kobject, the `LX_eventfd2` door and
> the `lxtest` checks described under "Design" and the edits in "Affected
> files" are planned work that lands in the implementation PR that follows,
> gated on CI. As the report was committed (before PR #285), `eventfd2`
> returned `-ENOSYS`.
>
> **Built in PR #285.** The implementation landed `kernel/io/eventfd.c`, wired
> `eventfd`/`eventfd2` through `do_eventfd` in `compat/linux/syscalls.c`, and
> added the `lxtest` checks; both arches boot PASS and `host-test` passes.
> Every present-tense statement below describes the state the report
> measured, before this implementation; `eventfd2` now returns a descriptor.

## Problem

`eventfd2(2)` (x86-64 290, AArch64 19) is unimplemented: its slot in
`linux_table` is NULL, so the dispatcher routes it to `lx_unknown` and it
returns `-ENOSYS`. `eventfd` is the counter-and-readiness file descriptor that
async runtimes and event loops use to wake one fd-based wait from another — a
8-byte counter a program `write`s to add to and `read`s to drain, that
`poll`/`select` report readable while it is non-zero. Without it, a Linux
program that creates an eventfd for cross-fd wakeups dies at creation.

The kernel already has the exact shape eventfd needs. The aio-timer unit
(`docs/audit/next-subsystem-aio-timer.md`) built a timer as a **kobject with a
readiness operation** — a `struct kobject_io_type` carrying `read`, `ready`,
`poll_wq` and `set_nonblock` (`kernel/io/timerobj.c`) — so it rides the I/O
ring's POLL/READ path and serves `poll`/`select` and a blocking read with no
new mechanism. eventfd is that same template with a counter in place of a
timer, and it additionally uses the `write` operation the io-type already
defines (`kernel/include/kernel/object.h`). What is missing is the object
itself and the one Linux syscall that creates it.

Prompt #2 §30 / the inventory's Linux-compat row names it:
`docs/audit/2026-09-deferred-work-inventory.md` §2.6, "missing (re-checked
2026-09-21): … `eventfd`/`timerfd`/`signalfd` …".

### Measured

`tools/eventfd-probe.py` adds `LX_eventfd2` to both syscall-number headers and
a check to the Linux raw-ABI test `tests/linux/lxtest.c`, which runs in the
standard boot (one debug boot, x86-64):

```
LXEVENTFD: eventfd2 unimplemented -> -ENOSYS; no eventfd object
```

The check asserts `eventfd2(0, 0)` returned `-ENOSYS` at the report commit,
before PR #285.

## Why it matters

- **The readiness primitive async stacks are built on.** `eventfd` is how a
  worker thread, a signal handler, or a timer wakes an `epoll`/`poll` loop;
  libraries (glibc's own, libuv, many runtimes) assume it. A program that
  reaches for it gets `-ENOSYS` at the first call.
- **It is in pattern and the mechanism exists.** The aio-timer kobject proved
  the readiness-object path (`read`/`ready`/`poll_wq`/`set_nonblock`); eventfd
  reuses it and adds only the counter semantics and the `write` side.

## The implementation before this unit

| piece | where | what it does |
|---|---|---|
| readiness kobject | `struct kobject_io_type` (`object.h`): `read`, `write`, `ready`, `poll_wq`, `set_nonblock` | a kobject that serves `poll`/`select` and blocking `read`/`write`, with per-object non-blocking mode; readiness and reads also ride the I/O ring, though a ring *write* is not spin-safe for an all-or-nothing counter and the ring has no eventfd door today (see the note in `eventfd.c`) |
| the template | `timerobj.c` (`timer_obj_create`) | a counter-bearing kobject: `read` returns the count and resets, `ready` is `COSMO_IO_READABLE` while non-zero, `poll_wq` is its wait queue, `release` is synchronous |
| handle install | `handle_install(&proc->handles, obj, rights)` | wraps a fresh kobject as an fd (as `lx_openat`/`lx_dup` do) |
| dispatch | `linux_table[LX_NR_MAX]`; unlisted → `lx_unknown` | `eventfd2` is unlisted → `-ENOSYS` |

## Design

### 1. The eventfd kobject

`kernel/io/eventfd.c` defines an `eventfd_obj` on the timerobj template: a
`uint64_t count`, a `waitqueue`, a spinlock, and `semaphore`/`nonblock` flags,
wrapped in a `struct kobject_io_type`:

- **`read`** (8 bytes): if `count == 0`, block on the wait queue (or `-EAGAIN`
  in non-blocking mode). Otherwise, in the default mode return the whole count
  and reset it to zero; under `EFD_SEMAPHORE` return `1` and decrement by one.
  A read that lowers the count wakes writers (the object became writable).
- **`write`** (8 bytes): read a `uint64_t` addend; `UINT64_MAX` is rejected
  `-EINVAL` (Linux). If `count + addend` would exceed `UINT64_MAX - 1`, block
  until a read makes room (or `-EAGAIN`). Otherwise add and wake readers.
- **`ready`**: `COSMO_IO_READABLE` while `count > 0`, `COSMO_IO_WRITABLE`
  while `count < UINT64_MAX - 1` — the Linux poll semantics.
- **`poll_wq`** / **`set_nonblock`** / **`release`**: as `timerobj` (the wait
  queue; the object's shared non-blocking mode; a plain free — no timer to
  cancel).

### 2. The Linux door

`LX_eventfd2(initval, flags)` creates the object with `count = initval` and
`semaphore = (flags & EFD_SEMAPHORE)`, installs it as an fd with read and
write rights, sets the object's non-blocking mode from `EFD_NONBLOCK`, and
marks the handle close-on-exec from `EFD_CLOEXEC`. An unknown flag bit is
`-EINVAL`.

x86-64 also keeps the **older `eventfd`** (number 284), which takes only an
initial value and no flags; a program can call it directly, so it gets its own
table entry `LX_eventfd` that calls the same handler with `flags == 0`.
AArch64's asm-generic table has only `eventfd2` (19), no separate `eventfd`.
Both live numbers are therefore covered.

### 3. Lifetime

The object is a kobject: a parked I/O-ring entry or a blocked reader holds a
reference, and `release` (a plain `kfree`, no timer) runs only when the last
reference and handle are gone. No new lifetime hazard beyond what the kobject
refcount already handles.

## Affected files

| file | change |
|---|---|
| `kernel/io/eventfd.c`, a header, `kernel/kernel.mk` | the eventfd kobject |
| `compat/linux/nr_x86_64.h`, `nr_aarch64.h` | `LX_eventfd2` (290 / 19) and `LX_eventfd` (x86-64 284) |
| `compat/linux/linux_abi.h` | `EFD_SEMAPHORE`/`EFD_NONBLOCK`/`EFD_CLOEXEC` |
| `compat/linux/syscalls.c` | `lx_eventfd2` + `[LX_eventfd2]` in the table |
| `tests/linux/lxtest.c` | the eventfd checks |
| `README.md` | Status entry |

## APIs

The Linux `eventfd2(2)` system call. No native ABI change in this unit; the
object lives in `kernel/io/` and could be exposed to the native I/O ring by a
later native syscall, but the gap this closes is the Linux one. The object is
submittable to the existing I/O ring as POLL/READ for free, like the timer.

## Tests

Planned for the implementation (`tests/linux/lxtest.c`, which runs in the
standard boot).

| test | proves |
|---|---|
| `eventfd` | `eventfd2(0,0)` returns an fd; writing 5 makes it readable (`poll`), a read returns 5 and it is no longer readable; `EFD_SEMAPHORE` with initval 3 reads `1` three times then would block; `EFD_NONBLOCK` read on an empty counter is `-EAGAIN`; a write of `UINT64_MAX` is `-EINVAL` |

**Planned mutations** (each alone, boot confirmed):
- `read` not resetting the count: the "no longer readable after a read" check
  fails (and `EFD_SEMAPHORE` would return the whole count, not 1).
- `ready` not testing the count: `poll` reports readable on an empty eventfd,
  so the pre-write poll check fails.
- `EFD_SEMAPHORE` ignored: the three single reads return 3 then 0, not 1/1/1.

## Benchmarks

None.

## Risks

- **Write overflow blocking.** The one blocking-write path (`count` near
  `UINT64_MAX`) is awkward to drive deterministically; the test exercises the
  `-EINVAL` and `-EAGAIN` edges rather than a real overflow wait, and the
  blocking path mirrors the read's wait-queue protocol.
- **Non-blocking mode is per object.** `set_nonblock` sets a property shared
  by every handle to the object, matching `timerobj` and the device-readiness
  model; `EFD_NONBLOCK` sets it at creation.

## Alternatives considered

- **A fresh object type, not the timerobj template.** The io-type
  (`read`/`write`/`ready`/`poll_wq`/`set_nonblock`) is exactly eventfd's
  surface; reusing it is why this is small and gets `poll`/`select` and the
  ring's readiness/read paths for free.
- **Only `eventfd2`, ignoring the old `eventfd` number.** On x86-64 the
  older `eventfd` (284) is a distinct number a program can still call, so it
  gets its own entry routed to the same handler with `flags == 0`; `eventfd2`
  alone would leave 284 returning `-ENOSYS`.
