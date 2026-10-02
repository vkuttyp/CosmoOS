# NEXT SUBSYSTEM — the Linux personality has no signalfd

> Constitution §68 report. This PR adds the report and the probe
> (`tools/signalfd-probe.py`); the `LX_signalfd`/`LX_signalfd4` numbers, the
> `kernel/io/signalfd.c` object, the two signal-core helpers, the Linux doors
> and the tests described under "Design" and listed in "Affected files" are
> planned work that lands in the implementation PR that follows, gated on CI.
> As the report was committed, the calls were unlisted in `linux_table`, so the
> dispatcher's `lx_unknown` returned `-ENOSYS`.

## Problem

`signalfd(2)` / `signalfd4(2)` (x86-64 282 / 289, AArch64 `signalfd4` 74) have
no syscall number defined in `compat/linux/nr_*.h` and no `linux_table` entry,
so the dispatcher's `lx_unknown` returns `-ENOSYS`. `signalfd` is the file
descriptor a program reads pending signals from instead of taking an
asynchronous handler: it blocks the signals it cares about, creates a
`signalfd` for them, and `read()`s a `struct signalfd_siginfo` per delivered
signal — so a signal becomes an ordinary readable event that `poll`/`select`/
`epoll` can wait on in one loop with its other file descriptors.

It is the fourth and last of the fd-based event sources Linux event loops
expect, and the other three are built: **eventfd** (counter), **timerfd**
(timer) and **epoll** (the aggregator). A program that blocks `SIGCHLD`/
`SIGTERM` and folds them into an `epoll` loop — the standard shape for a
supervisor or server — cannot today, because the signal leg returns `-ENOSYS`.

The native signal subsystem already has the state `signalfd` needs. Pending
signals are a per-thread and a per-process bitmask with a `struct signal_info`
slot per signal number (`thread.h`, `process.h`, `kernel/process/signal.c`);
the blocked set is `sig_blocked` with `signal_blocked`/`signal_set_blocked`
(`kernel/process/signal.c`); the
pending set is observable without consuming via `signal_pending_set`
(`kernel/process/signal.c:143`); and `struct signal_info` is already marshalled to the Linux
`struct lx_siginfo` by `fill_siginfo` (`compat/linux/signal.c:60`). The
readiness-kobject pattern eventfd and timerfd use — `struct kobject_io_type`
with `.read`/`.ready`/`.poll_wq`/`.set_nonblock` and a wait queue woken on an
external event — is exactly `signalfd`'s shape, and `timerobj`'s
`waitqueue_wake_all` on the timer callback (`kernel/io/timerobj.c`) is the
analog of the wake `signalfd` needs from the signal path.

## Probe

`tools/signalfd-probe.py` adds the `LX_signalfd`/`LX_signalfd4` numbers to both
`nr_*.h` headers (so a raw call compiles on each arch) and one check to
`tests/linux/lxtest.c` (which runs in the standard boot):

```
LXSIGFD: signalfd unimplemented -> -ENOSYS; no fd-based signal delivery
```

The check calls `signalfd4(-1, &mask, 8, 0)` and asserts it returned `-ENOSYS`
at the report commit, before the implementation. The marker prints only when
the syscall really returned `-ENOSYS`, so grepping it cannot show a false
result after a failed check.

## Why it matters

- **Signals in the event loop.** `signalfd` turns a signal into a readable fd,
  the only way a `poll`/`epoll`-driven program can handle `SIGCHLD`/`SIGTERM`
  without the re-entrancy of an async handler. With eventfd, timerfd and epoll
  built, this is the missing leg.
- **In pattern; the state exists.** It is a readiness kobject like eventfd and
  timerfd, over the signal bitmask the kernel already keeps and the
  `signal_info`→`lx_siginfo` mapping already written. The implementation adds
  the object and two small signal-core helpers, not a new signal mechanism.

## The implementation before this unit

| piece | where | what it does |
|---|---|---|
| pending signals | `sig_pending` + `sig_info[]` (`thread.h`), `sig_shared_pending` + `sig_shared_info[]` (`process.h`) | per-thread and per-process bitmask with one `signal_info` slot per signal number (coalesced, as Linux does for non-RT signals) |
| blocked set | `sig_blocked`, `signal_blocked`/`signal_set_blocked` (`kernel/process/signal.c`) | the mask of blocked signals (`SIGKILL`/`SIGSTOP` unblockable) |
| observe pending | `signal_pending_set(t)` (`kernel/process/signal.c:143`) | `sig_pending \| sig_shared_pending`, already feeding `rt_sigpending` |
| dequeue | `dequeue_locked` (`kernel/process/signal.c:410`, static) | removes one pending signal and returns its info, but only an **unblocked** one (`& ~sig_blocked`) and handler-priority ordered |
| raise + wake | `route_locked` in `signal_send`/`signal_send_thread` (`kernel/process/signal.c`) | sets the pending bit, fills the slot, and wakes the target with `sched_wake` — not a wait queue |
| siginfo mapping | `struct signal_info` (`signal.h:78`) → `fill_siginfo` (`compat/linux/signal.c:60`) | sig number, source, fault addr, code, sender pid/uid |
| readiness object | `struct kobject_io_type` (`object.h:91`); eventfd (`kernel/io/eventfd.c`), timerfd (`kernel/io/timerobj.c`) | `.read`/`.ready`/`.poll_wq`/`.set_nonblock` + a wait queue woken on an external event (`waitqueue_wake_all`, `timerobj.c:74`) |
| dispatch | `linux_table[LX_NR_MAX]`; unlisted → `lx_unknown` | the `signalfd` calls are unlisted → `-ENOSYS` |

## Design

### 1. A signalfd object (`kernel/io/signalfd.c`)

A readiness kobject on the eventfd/timerfd template, holding only the fd's
signal mask — not a process pointer:

```c
struct signalfd_obj {
    struct kobject base;
    uint64_t mask;             /* signals this fd reports (SIGKILL/SIGSTOP never) */
    bool nonblock;
};
```

Like Linux (whose `signalfd_ctx` holds just the mask), the object reads the
**reading** process's signals, taken from `process_current()` in `ready`/`read`
— so it owns no process reference, and a descriptor that outlives its creator
(inherited, or passed over an `AF_UNIX` socket) simply reports the signals of
whoever reads it, with nothing to dangle. The wait queue it polls on lives on
the process (§2), not the object.

- **`ready`** returns `COSMO_IO_READABLE` when `signal_pending_set(current) &
  mask` is non-empty (a matching signal is pending for the reading process or
  thread).
- **`poll_wq`** returns the reading process's `signalfd_wqh` (§2), so
  `poll`/`select`/`epoll` wait on it.
- **`read`** consumes matching pending signals of the current process into the
  caller's buffer as `struct signalfd_siginfo` records (one per signal number,
  128 bytes), blocking on `signalfd_wqh` until one is pending unless
  `nonblock`/`O_NONBLOCK` (then `-EAGAIN`); it drains as many as fit, returning
  at least one.
- **`set_nonblock`** tracks the fd's `O_NONBLOCK`.

### 2. A per-process signalfd wait queue, woken from the signal path

`route_locked` wakes a target thread with `sched_wake`, not a wait queue, so a
`signalfd`'s poll has nothing to hook. The unit adds **one** wait queue per
process — `struct waitqueue signalfd_wqh` on `struct process` — the analog of
Linux's `sighand->signalfd_wqh`, which every `signalfd` of that process polls
on. After `route_locked` (or `signal_send`) marks a signal pending, it wakes
that queue with `waitqueue_wake_all(&p->signalfd_wqh)` — as `timer_obj_fired`
wakes its waiters (`timerobj.c:74`). One shared queue, not a per-object
registry, so there is no list to keep in step with object lifetimes and no
per-object process pointer. The wake happens once the pending bit is set and is
ordered against `ready`'s read of the pending set, so a waiter either sees the
signal or is woken to re-check; a poller filters by its own `mask`. The same
queue is woken when `signalfd4` **updates** an existing fd's mask (§4), so a
signal already pending that the new mask now includes wakes a blocked poller.

This also closes a signal-core gap for `signalfd`: today a signal whose action
is ignore is **discarded at send even when blocked** (`kernel/process/signal.c:226-227`, a
recorded deviation from Linux, which keeps a blocked signal pending). A
supervisor's `signalfd` for `SIGCHLD` — whose default action is ignore — would
never see child exits. The unit makes a blocked signal that some `signalfd`
could report stay pending (Linux's rule: a blocked signal is queued regardless
of `SIG_IGN`), so it is there for `read` to consume; `SIGCHLD` is the test
case.

### 3. A consume-by-mask dequeue

`dequeue_locked` only takes **unblocked** signals (it filters `& ~sig_blocked`)
and orders by handler priority — wrong for `signalfd`, whose signals are
normally **blocked** and whose order is by signal number. The unit adds:

```c
bool signal_consume_mask(struct process *p, struct thread *t, uint64_t mask,
                         struct signal_info *out);
```

which, under the process lock, removes the lowest-numbered pending signal in
`mask` (from the thread's or the process's set, **including blocked ones**),
copies its `signal_info` to `*out`, clears the bit, and returns whether it
found one. `signalfd`'s `read` loops it.

### 4. The Linux doors

- **`signalfd4(fd, sigmask, sizemask, flags)`** — `sizemask` must be `8`
  (`sizeof(sigset)`), else `-EINVAL`; `flags` ⊆ `SFD_NONBLOCK | SFD_CLOEXEC`.
  `fd == -1` creates a new `signalfd` (masking off `SIGKILL`/`SIGSTOP`) and
  installs a handle; `fd >= 0` looks up an existing `signalfd` and replaces its
  mask, then wakes the process's `signalfd_wqh` so a poller blocked under the
  old mask re-checks against the new one. Returns the fd.
- **`signalfd(fd, sigmask, sizemask)`** (x86-64) is `signalfd4` with `flags 0`.
- The read path marshals `struct signal_info` into `struct signalfd_siginfo`
  (reusing the field extraction `fill_siginfo` already does for `lx_siginfo`).

## Affected files

| file | change |
|---|---|
| `compat/linux/nr_x86_64.h`, `compat/linux/nr_aarch64.h` | `LX_signalfd` (x86-64) / `LX_signalfd4` numbers |
| `compat/linux/linux_abi.h` | `SFD_NONBLOCK`/`SFD_CLOEXEC`, `struct lx_signalfd_siginfo` |
| `kernel/process/signal.c`, `kernel/include/kernel/signal.h` | `signal_consume_mask`, the `signalfd_wqh` wake, and keeping a blocked ignored signal pending |
| `kernel/include/kernel/process.h` | the per-process `signalfd_wqh` wait queue |
| `kernel/io/signalfd.c`, `kernel/include/kernel/signalfd.h` | the signalfd object |
| `compat/linux/syscalls.c` | `lx_signalfd`/`lx_signalfd4` + the table entries |
| `tests/linux/lxsig.c` | the signalfd tests (raise + read + poll) |
| `README.md` | Status entry |

## APIs

Planned for the implementation. The Linux `signalfd`/`signalfd4` calls, over a
new `signalfd` kobject (`kernel/io/signalfd.c`), a per-process `signalfd_wqh`
woken from the signal path, and `signal_consume_mask`; no on-disk or native
user ABI change beyond those kernel-internal interfaces.

## Tests

Planned for the implementation (`tests/linux/lxsig.c`, a standard-boot Linux
test program).

| test | proves |
|---|---|
| read a pending signal | block `SIGUSR1`, `signalfd4` for it, `tgkill` self, `read` returns one `signalfd_siginfo` with `ssi_signo == SIGUSR1` and the sender pid/uid |
| readiness / poll | the signalfd `poll`s readable only after the signal is raised; with `SFD_NONBLOCK`, `read` before any signal is `-EAGAIN` |
| blocked ignored signal (`SIGCHLD`) | block `SIGCHLD`, `signalfd4` for it, spawn a child that exits: the `signalfd` reads the `SIGCHLD` — i.e. a blocked signal whose default action is ignore is kept pending for the fd rather than discarded |
| mask scope | a signal **not** in the fd's mask leaves `read` blocked/`-EAGAIN` and stays pending for ordinary delivery |
| mask update wakes a poller | a thread blocked in `poll` on the signalfd for mask A, with a signal in B\A already pending; `signalfd4(fd, B, 8, 0)` makes the `poll` return readable |
| fd outlives creator | a `signalfd` fd read after the mask was set reports the **reader's** signals and never dereferences a gone process (no process pointer in the object) |
| errors | `sizemask != 8` is `-EINVAL`; a bad `flags` bit is `-EINVAL`; `SIGKILL`/`SIGSTOP` in the mask are silently ignored, not reported |

**Planned mutations** (each alone, boot confirmed):
- `ready`/`read` ignore the mask (report any pending signal): the mask-scope
  test fails (a masked-out signal is read).
- the `signalfd_wqh` wake is omitted from the signal path: the poll test
  hangs/times out waiting for a pending signal (readiness never fires) — caught
  by a bounded wait.
- a blocked ignored signal is still discarded at send: the `SIGCHLD` test fails
  (the fd never becomes readable).
- `signal_consume_mask` does not clear the pending bit: a second `read` returns
  the same signal, or the signal is still delivered after being read.

## Benchmarks

None.

## Risks

- **The wake on the signal path.** Waking `signalfd_wqh` from `route_locked` is
  the one cross-cutting change, on the hot signal-send path. It must respect the
  existing lock order (the process lock is held across `route_locked`): the
  wake is issued after the pending bit is set, and `waitqueue_wake_all` on a
  per-process queue must be safe to call there (as `timer_obj_fired` wakes its
  queue). This is the implementation's main hazard and the focus of its review.
- **Keeping a blocked ignored signal pending.** For `signalfd(SIGCHLD)` to work,
  a blocked signal whose action is ignore must stay pending rather than be
  discarded at send (`kernel/process/signal.c:226-227` currently discards it, a
  recorded deviation from Linux). The unit restores Linux's rule for a blocked
  signal; an *unblocked* ignored signal is still discarded as before, so
  nothing else changes.
- **Descriptor lifetime.** The object holds no process reference and reads
  `process_current()`, so an inherited or `SCM_RIGHTS`-passed `signalfd`
  reports the reader's signals and cannot outlive and dereference a gone
  process — matching Linux, where the read side uses `current`.
- **Consuming blocked signals.** `signalfd` deliberately dequeues blocked
  signals (that is the point), so `signal_consume_mask` drops the
  `~sig_blocked` filter — but `SIGKILL`/`SIGSTOP` must never be maskable into a
  signalfd (the door strips them), so the unblockable signals keep their
  default force.
- **Coalesced, non-RT.** The pending model is one `signal_info` per signal
  number (last writer wins), so `signalfd` reports one record per pending
  signal number, matching Linux's non-RT coalescing; real-time signal queuing
  is out of scope (the kernel has none).
- **Process-directed vs thread-directed.** v1 consumes the process-shared
  pending set and the reading thread's own pending set; it does not arbitrate
  which thread of a multi-threaded process should receive a process-directed
  signal beyond "the one that reads first", which is within Linux's latitude.

## Alternatives considered

- **`netlink`.** A whole socket address family (sockets, address binding, a
  message protocol) — far larger, and little of it is in pattern with an
  existing object.
- **`rseq`.** Restartable sequences need a new per-thread registration ABI and
  abort logic in the return-to-user path — a greenfield subsystem, not a
  composition of existing mechanism.
- **`setsockopt` expansion.** The socket stub has almost no per-socket backing
  to honor (`TCP_NODELAY`/`SO_REUSEADDR`/buffer sizes have no settable field),
  so an honest unit would mostly still answer `-ENOPROTOOPT`. `signalfd` reuses
  the readiness-object template and the existing signal core, so it is the
  better-bounded pick and completes the fd-based event-source family.
