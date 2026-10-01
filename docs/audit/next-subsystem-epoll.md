# NEXT SUBSYSTEM — the Linux personality has no epoll

> Constitution §68 report. This PR adds the report and the probe
> (`tools/epoll-probe.py`); the epoll kobject, the `epoll_create1`/`epoll_ctl`/
> `epoll_wait`/`epoll_pwait` doors and the `lxtest` checks described under
> "Design" and the edits in "Affected files" are planned work that lands in the
> implementation PR that follows, gated on CI. As committed here,
> `epoll_create1` returns `-ENOSYS`.

## Problem

The `epoll` family — `epoll_create`/`epoll_create1`, `epoll_ctl`, `epoll_wait`/
`epoll_pwait` — is unimplemented: none has a number in `compat/linux/nr_*.h`,
so none is in `linux_table` and the dispatcher's `lx_unknown` returns
`-ENOSYS`. `epoll` is the scalable readiness interface every modern Linux event
loop is built on (nginx, Redis, libuv, Go's netpoller, systemd): a kernel
object that holds an interest set of fds and, in one `epoll_wait`, reports which
are ready. `poll`/`select` exist here, but a program written for `epoll` — most
networked Linux software — dies at `epoll_create1`.

The kernel already has every piece. Readiness is a first-class kobject
operation: `struct kobject_io_type` carries `ready` (the current
`COSMO_IO_READABLE`/`WRITABLE`/`HANGUP`/`ERROR` mask, never blocks) and
`poll_wq` (the wait queue woken when that mask may change) — `object.h`, used by
eventfd, timerfd and the waitable devices. `poll`/`select` (`kernel/io/poll.c`)
already turn a fixed array of those into a readiness scan, and the async I/O
ring (`kernel/io/aio.c`, `aio_wait`) already sleeps on **many** objects'
`poll_wq` at once — preparing a wait entry on each, evaluating readiness, and
re-arming on every wake. epoll is that same machinery with a **persistent,
dynamic** interest set in place of a per-call array.

## Probe

`tools/epoll-probe.py` adds `LX_epoll_create1` to both syscall-number headers
and one check to `tests/linux/lxtest.c` (which runs in the standard boot):

```
LXEPOLL: epoll_create1 unimplemented -> -ENOSYS; no scalable readiness
```

The check asserts `epoll_create1(0)` returned `-ENOSYS` at the report commit,
before the implementation PR. The marker prints only when the syscall really
returned `-ENOSYS`, so grepping it cannot show a false result after a failed
check.

## Why it matters

- **The interface networked Linux software assumes.** Almost every server and
  async runtime uses `epoll`; without it they fail at startup. `poll`/`select`
  are not substitutes — the programs call `epoll_create1` directly.
- **It is in pattern and the mechanism exists.** The readiness kobject
  (`ready`/`poll_wq`), poll.c's readiness `evaluate`, and aio_wait's dynamic
  multi-wait sleep are built and proven. epoll composes them; it needs no new
  kernel mechanism, only an interest-set object and the doors.

## The implementation before this unit

| piece | where | what it does |
|---|---|---|
| readiness kobject | `struct kobject_io_type` `.ready`/`.poll_wq` (`object.h`), `kobject_ready`/`kobject_poll_wq` | the current readiness mask, and the wait queue woken when it may change (NULL = never) |
| readiness scan | `evaluate` / the wq array in `kernel/io/poll.c` | `revents = (ready & events) | (ready & (HANGUP|ERROR))` across an array |
| dynamic multi-wait sleep | `aio_wait` (`kernel/io/aio.c`) | prepare a wait entry on every member `poll_wq`, evaluate, sleep if none ready, finish all, re-arm on wake — the exact loop epoll_wait needs |
| fd → referenced object | `handle_lookup(ht, fd, rights)` (returns a referenced pointer; `kobject_get`/`kobject_put`) | pins a member object in the interest set across `epoll_wait` calls |
| poll bit mapping | `poll_events_to_io` / `poll_events_from_io` (`compat/linux/syscalls.c`) | `POLLIN↔READABLE`, `POLLOUT↔WRITABLE`, `HANGUP→POLLHUP`, `ERROR→POLLERR` |
| dispatch | `linux_table[LX_NR_MAX]`; unlisted → `lx_unknown` | the epoll calls are unlisted → `-ENOSYS` |

## Design

### 1. The epoll kobject and interest set

`kernel/io/epoll.c` defines an epoll object as a `kobject_io_type` (like the aio
ring). It holds a mutex, a wait queue of its own, and a list of interest
entries, each — structurally the aio `aio_req`:

```
struct epoll_item {
    struct kobject *obj;      /* the member, referenced (handle_lookup) */
    int fd;                   /* the key: (fd, obj) identifies the registration */
    uint32_t events;          /* requested EPOLL* -> COSMO_IO_* plus implicit ERR/HUP */
    uint64_t data;            /* opaque epoll_data, echoed back */
    bool oneshot;             /* EPOLLONESHOT: disabled after one report until re-armed */
    bool disabled;            /* a fired one-shot, until EPOLL_CTL_MOD re-arms it */
    struct wait_entry we;     /* queued on obj's poll_wq while waiting */
    struct waitqueue *wq;     /* kobject_poll_wq(obj, want) */
    struct list_node link;
};
```

- **`epoll_ctl(ADD)`** — look up `fd` (which returns a referenced object),
  reject adding the epoll to itself or another epoll (`-EINVAL`, deferring
  nested-epoll loop detection), reject a duplicate `(fd,obj)` (`-EEXIST`),
  translate the `EPOLL*` event mask to `COSMO_IO_*` (`poll_events_to_io`'s
  shape), store the entry holding the reference.
- **`epoll_ctl(MOD)`** — update an existing entry's events/data and re-arm a
  one-shot; `-ENOENT` if not registered.
- **`epoll_ctl(DEL)`** — remove the entry and `kobject_put` its reference;
  `-ENOENT` if not registered.
- **`epoll_wait(epfd, events, maxevents, timeout_ms)`** — aio_wait's loop:
  prepare a wait entry on the epoll object's **own** wait queue (so a
  concurrent `epoll_ctl` that adds a ready member wakes the sleeper, as
  `aio_wait` prepares on the ring's own `wait`) **and** on every enabled
  entry's `poll_wq`; evaluate each with poll.c's rule (`kobject_ready(obj) &
  (want | HANGUP | ERROR)`), and if none is ready and the deadline has not
  passed, `sched_block_current`; on wake, finish all and re-evaluate. Fill up
  to `maxevents` ready entries into the user array (translating `COSMO_IO_*`
  back to `EPOLL*`), disabling one-shots that fired. `timeout_ms` of `-1`
  waits forever, `0` polls; a finite timeout arms a timer whose callback
  records the deadline as expired and **wakes the waiting thread directly**
  (`sched_wake` on the thread it recorded, plus a `fired` flag the loop tests
  under the lock) — exactly `aio_wait`'s `alarm`, not a wake of the epoll
  queue, so the wait cannot miss its own deadline when no member is on that
  queue. The timer is cancelled with `timer_cancel_sync` on the way out.
- **`ready`/`poll_wq`** on the epoll object itself: readable when any entry is
  ready, its own wait queue — so an epoll fd can be polled or (later) nested.

### 2. The Linux doors

- **`epoll_create1(flags)`** — `flags` a subset of `EPOLL_CLOEXEC` (`-EINVAL`
  otherwise; `CLOEXEC` a no-op under the spawn model). Create the object,
  install it as a read handle.
- **`epoll_create(size)`** — the legacy call (x86-64 only); `size` is ignored
  beyond the historical `size > 0`, and it is `epoll_create1(0)`.
- **`epoll_ctl(epfd, op, fd, event)`** — resolve `epfd` to the epoll object
  (type-checked, `-EINVAL` otherwise), copy the `epoll_event` in for ADD/MOD,
  dispatch on `op`.
- **`epoll_wait(epfd, events, maxevents, timeout)`** (x86-64) and
  **`epoll_pwait(epfd, events, maxevents, timeout, sigmask, sigsetsize)`** (both
  arches) share one handler; `epoll_pwait`'s signal mask is validated and, with
  the limited signal model here, applied only as far as `epoll_pwait` already
  can (a NULL mask is the common path). `maxevents <= 0` is `-EINVAL`.

### 3. Lifetime

Each interest entry holds a reference to its member object (the one
`handle_lookup` returned), so the object stays alive while registered, and the
entry's reference is dropped on `EPOLL_CTL_DEL` and on closing the epoll fd
(`release` walks the list, `kobject_put`s each and frees the entries — the aio
ring's `aio_release` pattern). A blocked `epoll_wait` holds the epoll object by
its own handle reference. No fire callback and no new lifetime rule beyond the
kobject refcount.

### 4. The `epoll_event` ABI

`struct epoll_event { uint32_t events; epoll_data_t data; }` where `data` is an
8-byte opaque union the kernel only stores and echoes. On **x86-64** the struct
is `__attribute__((packed))` — 12 bytes, `data` unaligned at offset 4 — to
match the 32-bit ABI; on **AArch64** it is unpacked — 16 bytes, `data` at offset
8. The kernel copies the array with the arch-correct layout (the packed
attribute guarded on `__x86_64__`), so a program's `struct epoll_event` and the
kernel's agree on every byte.

## Affected files

| file | change |
|---|---|
| `kernel/io/epoll.c`, a header, `kernel/kernel.mk` | the epoll kobject and interest set |
| `compat/linux/nr_x86_64.h`, `nr_aarch64.h` | `LX_epoll_create1` (291 / 20), `LX_epoll_ctl` (233 / 21), `LX_epoll_pwait` (281 / 22), and x86-64 `LX_epoll_create` (213) / `LX_epoll_wait` (232) |
| `compat/linux/linux_abi.h` | `struct lx_epoll_event` (arch-packed), `EPOLL_CTL_*`, `EPOLLIN`/`OUT`/`ERR`/`HUP`/`ONESHOT`, `EPOLL_CLOEXEC` |
| `compat/linux/syscalls.c` | `lx_epoll_create1`/`_create`/`_ctl`/`_wait`/`_pwait` + the table entries |
| `tests/linux/lxtest.c` | the epoll checks |
| `README.md` | Status entry |

## APIs

The Linux `epoll_create1(2)`, `epoll_create(2)`, `epoll_ctl(2)`,
`epoll_wait(2)` and `epoll_pwait(2)` system calls, **level-triggered**. No
native ABI change: epoll is a kobject over the existing readiness operations and
could gain a native door later; the gap this closes is the Linux one.

## Tests

Planned for the implementation (`tests/linux/lxtest.c`, which runs in the
standard boot).

| test | proves |
|---|---|
| `epoll` basic | `epoll_create1(EPOLL_CLOEXEC)` returns an fd; add an eventfd for `EPOLLIN`; `epoll_wait` with a 0 timeout returns 0; write the eventfd; `epoll_wait` returns 1 with that entry's `data`; drain it; `epoll_wait` returns 0 |
| `epoll` multi | add an eventfd and a timerfd; arm only the timerfd; a bounded `epoll_wait` returns exactly the timerfd entry; then the eventfd too once written |
| `epoll` timeout | with a member registered but not ready, `epoll_wait` with a finite timeout (say 50 ms) returns `0` at the deadline rather than blocking forever — the direct-thread timeout wake |
| `epoll` MOD/DEL | `MOD` an entry from `EPOLLIN` to `0` (no longer reported); `DEL` an entry (no longer reported, and a second `DEL` is `-ENOENT`) |
| `epoll` oneshot | an `EPOLLONESHOT` entry is reported once, then not again until `MOD` re-arms it |
| `epoll` errors | `ADD` of an already-registered fd is `-EEXIST`; `MOD`/`DEL` of an unregistered fd is `-ENOENT`; adding an epoll fd to itself is `-EINVAL`; `maxevents <= 0` is `-EINVAL` |

**Planned mutations** (each alone, boot confirmed):
- `epoll_wait` ignoring an entry's `events` mask (reporting any readiness): the
  `MOD`-to-`0` entry is reported, failing that check.
- the ready evaluation dropping the `want` filter: an eventfd registered for
  `EPOLLIN` is reported before it is written.
- `EPOLLONESHOT` not disabling the entry: the one-shot is reported twice.

## Benchmarks

None.

## Risks

- **Close-while-registered is not auto-removed (v1).** Linux drops an fd from
  the interest set when its last descriptor is closed; this kernel has no close
  hook for that. v1 keeps the member's reference, so a registered fd must be
  removed with `EPOLL_CTL_DEL` — a closed-but-still-registered entry keeps its
  object alive and keeps being evaluated. This is the one real deviation; it is
  documented, and auto-remove-on-close is a follow-up that needs a handle-table
  notification epoll can subscribe to.
- **Level-triggered only.** `EPOLLET` (edge-triggered) needs per-entry
  last-reported state and transition bookkeeping; it is a separate unit. Until
  then an `EPOLLET` bit is rejected (`-EINVAL`) rather than silently treated as
  level, so a program that depends on edge semantics fails loudly.
- **Nested epoll is refused.** Adding an epoll fd to an epoll is `-EINVAL` in
  v1; loop detection across nested epolls is deferred with it.
- **The packed `epoll_event`.** The x86-64 packed layout (§4) is the easy thing
  to get wrong; the test reads back `data` to prove the array marshals
  byte-for-byte on both arches.

## Alternatives considered

- **Building on the aio ring instead of a new object.** The ring is a
  submission/completion queue, not an interest set; epoll's ADD/MOD/DEL and
  repeated level-triggered `wait` over a persistent set are a different shape. A
  dedicated object reusing `aio_wait`'s sleep loop is cleaner than overloading
  the ring.
- **Edge-triggered in v1.** Deferred: `EPOLLET` roughly doubles the state and
  the test surface (per-entry last state, transition-only reporting). The
  level-triggered core is what most programs use and what the existing
  `evaluate` gives directly.
- **`signalfd`/`mremap` instead.** `signalfd` needs core signal surgery
  (pending signals are a bitmask, not a readable queue) and `mremap` needs VM
  resize/move primitives the VM layer lacks; epoll reuses mature readiness
  infrastructure with no core change, so it is the better-bounded pick.
