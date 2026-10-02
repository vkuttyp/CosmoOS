# NEXT SUBSYSTEM — epoll is level-triggered only, EPOLLET is refused

> Constitution §68 report. This PR adds the report and the probe
> (`tools/epollet-probe.py`); the per-item edge state, the `collect` gating, the
> re-arm in `epoll_obj_wait`, the `epoll_obj_add`/`epoll_obj_mod` signature
> change, the door change in `compat/linux/syscalls.c` and the tests described
> under "Design" and listed in "Affected files" are planned work that lands in
> the implementation PR that follows, gated on CI. As the report was committed,
> `lx_epoll_ctl` rejected any `events` with `EPOLLET` set with `-EINVAL`
> (`compat/linux/syscalls.c:943`), so an edge-triggered registration could not
> be made at all.
>
> **Built in PR #301.** The implementation added `edge`/`armed` to
> `struct epoll_item`, the `collect` gating, the §3a sleep-decision gating, the
> `edge` parameter on `epoll_obj_add`/`epoll_obj_mod`, the `epoll_obj_rearm`
> extension for an undelivered edge, and the door change — as described below.
> The tests landed in `tests/linux/lxtest.c`. Both arches boot PASS and
> `host-test` passes. Every present-tense statement below describes the state
> the report measured, before this implementation.
>
> **One change from the planned re-arm (§3).** The plan re-armed an edge member
> on an observed drain or a coarse per-wait wake. Review found both lose edges: a
> drain+refill between two non-blocking `epoll_wait(0)` calls never lets
> `collect` observe the not-ready trough, and a member wake that races the
> deadline is indistinguishable from a bare timeout. The built re-arm instead
> drives off the **member's own wake**: `struct waitqueue` gained a wake
> generation (bumped on every `waitqueue_wake_*`, even with no waiter), each edge
> member records its poll queue's generation, and `collect` re-arms it whenever
> that generation has advanced — a drain+refill, a new event on a still-ready
> member, or a deadline-racing wake all advance it; a bare timeout (not a wake)
> does not. The §3a sleep decision re-reads the generation after the wait entries
> are armed, closing the collect-to-arm window. This removes the `!alarm.fired`
> bookkeeping the plan implied.

## Problem

The epoll unit (`kernel/io/epoll.c`, PR #291) is **level-triggered only**.
`lx_epoll_ctl` refuses a registration whose `events` carries `EPOLLET`:

```c
if (ev.events & LX_EPOLLET) {   /* edge-triggered is deferred */
    kobject_put(ep);
    return -EINVAL;
}
```

So a program that asks for edge-triggered notification — the mode most
high-throughput event loops use (nginx, libev/libevent's epoll backend, Go's
netpoller, Rust/mio, tokio) — fails at `epoll_ctl` with `-EINVAL` and cannot
run. It is not a silent degradation to level-triggered; it is a hard refusal at
setup.

Edge-triggered is a well-defined variation on the level-triggered machinery the
object already has. Level-triggered (LT) reports a member on **every**
`epoll_wait` while it stays ready; edge-triggered (ET) reports it only on a
**transition** into readiness, and then stays silent until the member is drained
and becomes ready again (or a fresh event arrives). The object already tracks
each member's readiness live (`item_ready`, `collect`) and already carries
per-member one-shot state (`disabled`) that is a near-identical "report, then
suppress until re-armed" shape. ET adds a second such flag and a rule for when
it re-arms.

## Probe

`tools/epollet-probe.py` adds one check to `tests/linux/lxtest.c` (which runs in
the standard boot), inside the existing epoll block where an epoll fd and a
spare eventfd are already in scope:

```
LXEPOLLET: edge-triggered epoll rejected -> -EINVAL; level-triggered only
```

The check calls `epoll_ctl(ep, EPOLL_CTL_ADD, efd2, {EPOLLIN|EPOLLET})` (`efd2` is
the spare eventfd the surrounding block already created for its error cases) and
asserts it returned `-EINVAL` at the report commit, before the implementation.
The marker prints only when the call really returned `-EINVAL`, so grepping it
cannot show a false result after a failed check. (`tests/linux/lxtest.c` already
asserts this `-EINVAL` at line 840 as an error case; the probe adds the greppable
marker. The implementation flips both to the accepted, edge-triggered behaviour.)

## Why it matters

- **Edge-triggered is the default for scalable event loops.** The event-loop
  libraries most Linux server programs are built on request `EPOLLET` to avoid
  re-reporting a busy fd on every wait. Refusing it at `epoll_ctl` stops those
  programs at startup, so the fd-based event-source family (eventfd, timerfd,
  signalfd, epoll — all built) is still not usable by the programs that most
  want it.
- **In pattern; the state exists.** ET is the existing object plus a per-member
  "armed" flag and a re-arm rule, mirroring the one-shot `disabled` flag already
  there. No new wait protocol, no new object — the level-triggered `collect`
  gains one condition and the wait loop one re-arm step.

## The implementation before this unit

| piece | where | what it does |
|---|---|---|
| member record | `struct epoll_item` (`kernel/io/epoll.c:44`) | `fd`, referenced `obj`, arm `id`, `want` mask, opaque `events`/`data`, one-shot `oneshot`/`disabled` |
| readiness of one member | `item_ready` (`kernel/io/epoll.c:132`) | `kobject_ready(obj) & want`, or 0 if `disabled` |
| report pass | `collect` (`kernel/io/epoll.c:144`) | reports every member with `item_ready() != 0`, round-robin to the tail, disabling one-shots as it reports them |
| wait | `epoll_obj_wait` (`kernel/io/epoll.c:298`) | snapshot-and-pin multi-wait: collect; if none ready, arm the set's queue and each member's `poll_wq`, sleep, re-collect |
| one-shot re-arm | `disabled` + `epoll_obj_mod`/`epoll_obj_rearm` | a reported one-shot is `disabled` until a MOD (or a failed-delivery re-arm) clears it |
| door | `lx_epoll_ctl` (`compat/linux/syscalls.c:920`) | translates `EPOLL*`↔`COSMO_IO_*`, extracts `oneshot`; **rejects `EPOLLET` with `-EINVAL` (`:943`)** |
| events echo | `epoll_events_from_io` (`compat/linux/syscalls.c:867`) | maps the fired `COSMO_IO_*` back to `EPOLL*` for the returned event |

## Design

### 1. Per-member edge state (`kernel/io/epoll.c`)

`struct epoll_item` gains two flags beside the one-shot pair:

```c
bool edge;         /* EPOLLET: report only on a transition into readiness */
bool armed;        /* eligible to report an edge now (distinct from !disabled) */
uint64_t edge_gen; /* the member queue's wake generation last acted on (§3) */
```

`edge` is fixed at ADD/MOD from the `EPOLLET` bit; `armed` is the live "a new
edge may be reported" state, reset to `true` on ADD/MOD; `edge_gen` is seeded at
ADD/MOD to the member queue's current generation, so only a *later* wake re-arms
past the initial `armed`.

### 2. `collect` gates an edge member on `armed`

A level member is reported whenever `item_ready() != 0` (unchanged). An edge
member is reported only when `item_ready() != 0 && it->armed`; on report it is
disarmed (`it->armed = false`) so it is not reported again until it re-arms. The
round-robin move-to-tail and the one-shot `disabled` handling are unchanged and
orthogonal: a report both disarms the edge flag and disables a one-shot, and a
MOD resets both.

### 3. Re-arm rule: the member's wake generation

> This section describes the built design. An earlier draft re-armed an edge
> member from an *observed* drain (a `collect` pass seeing it not ready) or from
> a coarse per-wait wake flag. Both lose edges — a drain and refill between two
> non-blocking `epoll_wait(0)` calls never lets `collect` observe the not-ready
> trough, and a member wake that races the wait's deadline cannot be told apart
> from a bare timeout — so the re-arm instead drives off the member's own wake.

An edge member re-arms (`armed = true`) when its poll queue has been **woken**
since `collect` last acted on it — that wake is the event, whether or not a wait
was blocked for it and whether or not its readiness ever dipped to 0 in between.

`struct waitqueue` gains a `wake_gen` counter, bumped on every
`waitqueue_wake_one`/`_all` (even one that transitions no waiter), so a poller
not currently blocked can still learn an event fired while it was away — the
object pulls readiness on demand rather than being pushed a per-fd callback, and
the generation is the minimal push signal that needs. Each edge member records
the generation of its poll queue (`it->edge_gen`); `collect` reads the queue's
current generation and, when it differs, sets `armed` and stores the new value.
A drain+refill, a new event on a still-ready member, and a wake that raced a
deadline all advance the generation; a bare timeout (not a wake) advances
nothing and re-arms nothing. A member with no poll queue (always ready, never
changing) has no generation to advance, so it reports once and stays quiet —
correct for a source with no transitions.

The re-arm is naturally per-member (each reads its own queue), so there is no
coarse "re-arm all on any wake". A report leaves `edge_gen` unchanged, so an
event that arrived between the re-arm and the report is not swallowed — the next
`collect` sees the newer generation and re-arms again.

### 3a. The sleep decision must gate on `armed`, and re-read the generation

A disarmed edge member stays **readable** at the object level even though
`collect` will not report it. The wait's decision to sleep (`snap_any_ready`)
must therefore not treat such a member as ready, or a blocking `epoll_wait` on a
disarmed-but-readable edge member would neither report (collect gates it out)
nor sleep (readiness seen) — a busy-spin. `snapshot` already skips `disabled`
one-shots, but a disarmed edge member is **not** `disabled` — it must stay in
the snapshot so its `poll_wq` is armed and its next wake ends the sleep. So the
snapshot records each member's `edge`/`armed` and its `edge_gen`, and
`snap_any_ready` — called **after** the wait entries are armed — treats a
disarmed edge member as ready only if its queue's generation has advanced past
the recorded `edge_gen` (a wake raced in between `collect` and the arm, so
re-collect rather than sleep); otherwise it does not count. This re-read closes
the collect-to-arm window the same way the level path re-reads
`kobject_ready`. A wake after this check wakes the armed entry instead.

### 4. The door stops refusing EPOLLET (`compat/linux/syscalls.c`)

`lx_epoll_ctl` drops the `EPOLLET → -EINVAL` block and extracts

```c
bool edge = (ev.events & LX_EPOLLET) != 0;
```

passing it to `epoll_obj_add`/`epoll_obj_mod` (which gain an `edge`
parameter beside `oneshot`). `EPOLLET` is an input-only flag: it is not echoed
in the returned `events` (`epoll_events_from_io` already reports only the fired
readiness bits), matching Linux.

## Affected files

| file | change |
|---|---|
| `kernel/include/kernel/wait.h`, `kernel/scheduler/wait.c` | `struct waitqueue` gains `wake_gen`, bumped on every wake; `waitqueue_wake_gen()` reads it (§3) |
| `kernel/io/epoll.c`, `kernel/include/kernel/epoll.h` | `edge`/`armed`/`edge_gen` on `struct epoll_item`; `collect` re-arms an edge member when its queue generation advanced and reports it only while `armed`; the sleep check (`snap_any_ready`) treats a disarmed edge member as not ready and re-reads the generation (§3a); `epoll_obj_rearm` restores a reported edge too; `epoll_obj_add`/`epoll_obj_mod` gain an `edge` parameter |
| `compat/linux/syscalls.c` | drop the `EPOLLET → -EINVAL` refusal; extract `edge`; pass it through; `do_epoll_wait` re-arms an undelivered edge; flip the existing `-EINVAL` assertion's expectation |
| `tests/linux/lxtest.c` | edge-vs-level tests (below); the existing `EPOLLET → -EINVAL` error check becomes the accepted path |
| `README.md` | Status entry |

## APIs

Planned for the implementation. No new syscall and no ABI change: `EPOLLET`
(`linux_abi.h:417`) and the `struct epoll_event` ABI already exist; the change is
an `edge` parameter on the two kernel-internal `epoll_obj_add`/`epoll_obj_mod`
interfaces and the behaviour of an existing flag.

## Tests

Planned for the implementation (`tests/linux/lxtest.c`, the standard-boot
CHECK/CHECKV self-test). The edge-vs-level checks are deterministic with
`timeout == 0` polls — no timing. The one blocking-wake check waits with a
finite timeout (a few seconds) as a deadline, never `-1`, so a regression that
drops the wake fails the check rather than hanging the boot.

| test | proves |
|---|---|
| `EPOLLET` accepted | `epoll_ctl(ADD, …, EPOLLIN\|EPOLLET)` returns 0, not `-EINVAL` (the former error case) |
| edge reported once | add `EPOLLIN\|EPOLLET` on an eventfd, write it, `epoll_wait(0)` returns it once; a second `epoll_wait(0)` **without** draining returns 0 (a level member would return 1) |
| edge re-arms after a drain | drain the eventfd (not ready), `epoll_wait(0)` returns 0, write again, `epoll_wait(0)` returns it — a fresh transition is a new edge |
| edge re-arms with no poll between the drain and refill | drain and refill the eventfd with no `epoll_wait` in between, so no poll observes the not-ready trough; the next `epoll_wait(0)` still returns it — the re-arm came from the member's wake (§3), not from an observed drop |
| level contrast | after a MOD to level, a ready member stays reported on repeated `epoll_wait(0)`, confirming only the edge member is suppressed |
| `EPOLLET\|EPOLLONESHOT` | reported once; a fresh edge while the one-shot is disabled is **not** reported; a MOD re-arms and it reports again — the two suppressions are orthogonal |
| edge re-arms on a blocking wake | a periodic timerfd registered edge is reported and drained (disarming it); a later period fires and its wake re-arms the edge so `epoll_wait` with a finite, far-longer deadline returns it (a dropped wake fails at the deadline, never hangs). A periodic timer, not a second thread, is the waker, so the single-threaded self-test stays deterministic |

**Mutations** (each alone, boot confirmed):
- `collect` reports an edge member regardless of `armed` (treat it as level):
  the "reported once then 0" / "with no poll between" suppression checks fail —
  the member is reported twice.
- `collect` never sets `armed` when the member's generation advanced (the §3
  re-arm): every re-arm check fails — a drained-then-refilled or blocking-woken
  edge member is never reported again.

## Benchmarks

None.

## Risks

- **Pull-based readiness vs Linux's pushed callback.** Linux `EPOLLET` is driven
  by the per-fd wakeup callback (`ep_poll_callback`) that re-adds the fd to the
  ready list on every event. This object pulls readiness on demand
  (`item_ready`/`kobject_ready`), and the per-queue wake generation (§3) is the
  push signal it adds: an edge is any advance of the member queue's generation,
  i.e. any `waitqueue_wake_*` on it. So ET is faithful as long as the source
  wakes its queue when it produces the event — which the readiness objects do
  (that wake is also what a blocked `poll`/`epoll` needs). The residual
  approximation is a source that changes readiness **without** waking its queue:
  such a transition is not an edge here. No current readiness object does that,
  so it is a latent property of the contract, not an observed gap.
- **Spurious edges are permitted.** A wake with no real readiness change
  advances the generation and re-arms, but `collect` still gates on actual
  readiness, so a spurious wake reports nothing unless the member is in fact
  ready — and a spurious `EPOLLET` wakeup is explicitly allowed by Linux (a
  correct consumer is non-blocking and drains to `EAGAIN`). The re-arm is
  per-member (each reads its own queue's generation), so a sibling's event does
  not re-arm an unrelated member.
- **Orthogonal to one-shot.** `armed` (edge) and `disabled` (one-shot) both gate
  a report and are both reset by MOD; an `EPOLLET|EPOLLONESHOT` member fires once
  and then stays `disabled` until MOD, independent of arming.
- **Level default unchanged.** A member without `EPOLLET` ignores `armed`
  entirely, so existing level-triggered behaviour — and every existing epoll
  test — is byte-for-byte unchanged.
- **Concurrency.** `armed` lives under the existing set mutex, so concurrent
  `ctl`/`wait` serialise on it exactly as the one-shot `disabled` flag does; the
  snapshot-and-pin wait protocol (the subtle part of the object) is untouched.

## Alternatives considered

- **`rseq`.** Restartable sequences need a new per-thread registration ABI and
  abort logic on the return-to-user path — a greenfield subsystem, not a
  composition of existing mechanism, and glibc tolerates its absence (it falls
  back when `rseq` returns `-ENOSYS`), so the impact is lower.
- **`setsockopt` expansion.** Deliberately returns `-ENOPROTOOPT` by a prior
  decision (`docs/audit/next-subsystem-socket-verdict.md`, Design 4): the socket
  has no per-option backing to honour and `-ENOPROTOOPT` is what Linux itself
  returns for an unimplemented option, which a program's error path handles.
- **`execve`.** Deferred by the native spawn model (no fork/exec); replacing a
  process image is a major subsystem and an architectural question of its own,
  not a bounded §68 unit.
- **`netlink`.** A whole socket address family — far larger, and little of it is
  in pattern with an existing object.

EPOLLET extends the just-built epoll object with one flag and one re-arm rule,
reusing the one-shot suppression shape and leaving level-triggered behaviour
untouched, so it is the better-bounded pick and lets the fd-based event-source
family serve the edge-triggered event loops that are its main consumers.
