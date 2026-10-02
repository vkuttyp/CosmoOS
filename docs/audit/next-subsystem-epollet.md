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
bool edge;      /* EPOLLET: report only on a transition into readiness */
bool armed;     /* eligible to report an edge now (distinct from !disabled) */
```

`edge` is fixed at ADD/MOD from the `EPOLLET` bit; `armed` is the live "a new
edge may be reported" state, reset to `true` on ADD/MOD.

### 2. `collect` gates an edge member on `armed`

A level member is reported whenever `item_ready() != 0` (unchanged). An edge
member is reported only when `item_ready() != 0 && it->armed`; on report it is
disarmed (`it->armed = false`) so it is not reported again until it re-arms. The
round-robin move-to-tail and the one-shot `disabled` handling are unchanged and
orthogonal: a report both disarms the edge flag and disables a one-shot, and a
MOD resets both.

### 3. Re-arm rule in `epoll_obj_wait`

An edge member re-arms (`armed = true`) when either:

- **its readiness is observed to have dropped** — a `collect` pass finds
  `item_ready() == 0` for it (the consumer drained it); the next rise into
  readiness is then a fresh edge; or
- **the wait was woken from a sleep** — `epoll_obj_wait` returns from
  `sched_block_current`, meaning a member's `poll_wq` (or a ctl, or the timer)
  fired; every edge member is re-armed before the next `collect`, so a new event
  that arrives while the consumer is blocked produces an edge even if the member
  never went fully not-ready.

Both re-arms happen under the set lock. The second is deliberately coarse — it
re-arms all edge members on any wake, not only the one whose event fired —
because this object pulls readiness on demand rather than being pushed a
per-fd callback, so it cannot cheaply attribute a wake to one member. The cost
is that an edge member may be re-reported because a *sibling* fired; that is a
spurious edge, which Linux's `EPOLLET` contract explicitly permits (a consumer
must use non-blocking fds and drain to `EAGAIN`, so a spurious wakeup is
harmless). It never *drops* an edge for a ready, armed member.

### 3a. The sleep decision must gate on `armed` too

A disarmed edge member stays **readable** at the object level even though
`collect` will not report it. The wait's decision to sleep (`snap_any_ready`,
`epoll.c:343`) today asks only `kobject_ready(obj) & want`, which would see such
a member as ready — so a blocking `epoll_wait` on a disarmed-but-readable edge
member would neither report (collect gates it out) nor sleep (readiness seen):
it would busy-spin. `snapshot` already skips `disabled` one-shots for this
reason, but a disarmed edge member is **not** `disabled` — it must still be in
the snapshot so its `poll_wq` is armed and a drain or a new event wakes the wait
to re-arm it. So the snapshot records each member's `edge`/`armed`, and the
"any ready" check applies the same gating as `collect`: an edge member counts as
ready for the sleep decision only when `armed`. The wait then sleeps on a
disarmed edge member (waiting on its queue and the set's), and a wake re-arms and
re-reports it. This is the one spot in `epoll_obj_wait` the unit must touch
beyond the re-arm.

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
| `kernel/io/epoll.c`, `kernel/include/kernel/epoll.h` | `edge`/`armed` on `struct epoll_item`; `collect` gates an edge member on `armed`; `epoll_obj_wait` re-arms (drained-or-woken) and its sleep check (`snap_any_ready`, via the snapshot's recorded `edge`/`armed`) treats a disarmed edge member as not ready (§3a); `epoll_obj_add`/`epoll_obj_mod` gain an `edge` parameter |
| `compat/linux/syscalls.c` | drop the `EPOLLET → -EINVAL` refusal; extract `edge`; pass it through; flip the existing `-EINVAL` assertion's expectation |
| `tests/linux/lxtest.c` | edge-vs-level tests; the existing `EPOLLET → -EINVAL` error check becomes the accepted path |
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
| edge reported once | add `EPOLLIN\|EPOLLET` on an eventfd, write it, `epoll_wait(0)` returns it once; a second `epoll_wait(0)` **without** draining returns 0 (a level member would return 1) |
| level contrast | a plain `EPOLLIN` member stays reported on repeated `epoll_wait(0)` while it is ready, confirming only the edge member is suppressed |
| edge re-arms after a drain | drain the eventfd (not ready), `epoll_wait(0)` returns 0, write again, `epoll_wait(0)` returns it — a fresh transition is a new edge |
| edge re-arms on a blocking wake | an edge member drained and not reported; a write while a thread is blocked in `epoll_wait` with a finite multi-second timeout wakes it and returns the member before the deadline (a dropped wake fails at the deadline, never hangs) |
| `EPOLLET` accepted | `epoll_ctl(ADD, …, EPOLLIN\|EPOLLET)` returns 0, not `-EINVAL` (the former error case) |
| `EPOLLET\|EPOLLONESHOT` | reported once, then suppressed until MOD re-arms, regardless of the edge flag (the two suppressions are orthogonal) |

**Planned mutations** (each alone, boot confirmed):
- `collect` reports an edge member regardless of `armed` (treat it as level):
  the "reported once then 0" test fails — the member is reported twice.
- `epoll_obj_wait` never re-arms on a drain: the "re-arms after a drain" test
  fails — a drained-then-refilled edge member is never reported again.
- `epoll_obj_add`/`epoll_obj_mod` do not set `armed = true`: an edge member is
  never reported — the first `epoll_wait` returns 0.

## Benchmarks

None.

## Risks

- **Pull-based readiness vs Linux's pushed callback.** Linux `EPOLLET` is driven
  by the per-fd wakeup callback (`ep_poll_callback`) that re-adds the fd to the
  ready list on every event. This object instead pulls readiness on demand
  (`item_ready`/`kobject_ready`). The re-arm rule (§3) reproduces ET for the
  normal cycle — block, get an edge, drain to `EAGAIN`, block again — and for a
  new event arriving during a blocking wait. The one case it approximates is a
  consumer that keeps a member continuously readable (never drains it), never
  blocks, and polls with `epoll_wait(0)` expecting a fresh edge per new event
  that does not change the readiness level: that new event is coalesced into the
  outstanding one. This is precisely the usage Linux's own documentation warns
  against (drain to `EAGAIN` or risk a lost event), so it is a documented v1
  approximation, not a correctness bug for correct ET use. This is the review's
  main focus.
- **Spurious edges under a shared set.** The coarse re-arm (re-arm all edge
  members on any wake) can report an edge member because a sibling fired. Linux
  permits spurious `EPOLLET` wakeups, so a correct consumer (non-blocking, drain
  to `EAGAIN`) is unaffected; it never drops a real edge.
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
