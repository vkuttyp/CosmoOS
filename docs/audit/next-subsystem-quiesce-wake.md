# NEXT SUBSYSTEM — a grace period that ends when it ends, not at the next tick

Date: 2026-09-18. Tree: `main` at d46875f (after PR #173, the
asynchronous-error classifier). Chosen from
`docs/audit/2026-09-deferred-work-inventory.md` §4.

**Subsystem: `synchronize_quiesce`'s wait. Every grace period in this
kernel finishes when a sleeping waiter next wakes up, not when the last
CPU publishes — so a grace period that is over in microseconds is
reported as over milliseconds later.**

Takes up §4's first item, the last of the quiesce report's risks still
standing: *"grace-period latency is tick-bound (a 4-8 ms floor);
`synchronize_quiesce` polls; the wake-on-publish design that would remove
the floor was not built."* The design was named in
`docs/audit/2026-09-lifetime-quiesce-report.md` and never built. This
unit builds it.

## Problem

### 1. The wait is a poll

```c
/* kernel/core/quiesce.c, sync_quiesce_counting */
for (;;) {
    cpumask_t pending = quiesce_core_pending(&g_state, target, online) & cpu_online_mask();
    if (pending == 0)
        break;
    ... straggler kicks after 2 ticks, a warning at 1 s, a debug panic at 10 s ...
    thread_sleep_ns(TICK_NS / 2);
}
```

`CONFIG_HZ` is 250 (`timer.h:17`), so `TICK_NS` is 4 ms and the sleep is
**2 ms**, rounded up to the next tick that services it. The loop checks,
sleeps, checks again. Nothing tells it that the last CPU published one
microsecond after it went to sleep.

So the *cost* of a grace period is not what it takes; it is what the
sleep takes. On an idle machine — every other CPU in the idle loop,
publishing at every tick and every exception return — the work is over
almost immediately and the waiter still pays a sleep.

### 2. Every teardown path in the tree pays it, synchronously

`synchronize_quiesce` has five production callers:

| caller | what is waiting |
| --- | --- |
| `interrupt.c:135` | `interrupt_unregister` — proving no CPU is inside the handler being removed |
| `module.c:511` | module unload |
| `netif.c:260`, `netif.c:569` | interface unregister, twice |

And the asynchronous alternative is unused: `call_quiesce`, the deferred
form that would let a caller not block at all, has **no callers outside
`quiesce.c` and its own tests**. Every reclamation in this tree takes the
synchronous floor. That is not an accident to fix by converting callers —
a device unregister genuinely wants to wait — it is the reason the floor
is worth removing rather than routing around.

### 3. The mechanism to end the wait exactly is already there

Nothing needs to be invented. The publish side is one store:

```c
/* quiesce_core.h, quiesce_core_publish */
uint64_t e = __atomic_load_n(&st->epoch, __ATOMIC_ACQUIRE);
__atomic_store_n(&st->cpus[cpu].seen_epoch, e, __ATOMIC_RELEASE);
```

called from five places that already run at every quiescent point — the
scheduler (`sched.c:65`, `:139`, `:267`) and both architectures' trap
returns (`aarch64/trap.c:120`, `x86_64/trap.c:93`). The CPU that makes
`quiesce_core_pending` go to zero is executing one of those. It simply
does not tell anyone.

And the waiter's side has a queue mechanism to be told on:
`waitqueue_wake_all` is documented **interrupt-safe** (`wait.h:11`,
*"Wakers are interrupt-safe"*), which is exactly the context a trap-return
publish runs in.

## Why it matters

- **It is the last of the quiesce report's named risks**, and the only
  one of its list still standing. The other three were closed by the
  wake-preempt and lockup units.
- **The floor is paid on paths that are already slow and already
  observed.** `device-remove-busy`, `blk-unregister-drain` and
  `net-netif-lifetime` all wait on grace periods; so does every module
  unload.
- **It is a latency bug with a correctness-shaped fix.** The wake does
  not change what a grace period *means* — `quiesce_core_pending`
  returning zero is still the whole condition — it changes when the
  waiter learns it. That is a small, checkable change to a mechanism the
  rest of the kernel's lifetime rules rest on, which is the reason to be
  careful rather than the reason to skip it.
- **The tree already measures it.** `struct quiesce_stats` carries
  `synchronizes` and `max_wait_ns` (`quiesce.h:74`, `:76`), so the
  before-and-after is not a claim, it is a counter that already exists.

## Design

### 1. Wait on a queue, with the poll as the backstop

One waitqueue for grace periods. The loop keeps every escalation it has
and changes only how it waits:

```c
for (;;) {
    pending = quiesce_core_pending(...) & cpu_online_mask();
    if (pending == 0)
        break;
    ... kicks, warning, debug panic: unchanged ...
    wait_event_timeout(&g_gp_wq, quiesce_core_pending(...) == 0, TICK_NS / 2);
}
```

**The timeout is not a fallback nobody expects to hit; it is the
correctness argument.** A wake that is missed — because the publisher ran
before the waiter queued, or because a CPU published from a context that
did not wake — costs the waiter up to 2 ms and nothing else. The loop
re-checks the same condition it checks today. So the wake can only make
the wait shorter, never wrong, and a bug in it is a latency regression
rather than a hang. That property is why this design is worth building
and the reason to keep the poll rather than replace it.

### 2. Who wakes, and from where

The publisher cannot cheaply know whether it was the last: computing that
means reading every CPU's `seen_epoch`, at every quiescent point, on
every CPU — the hot path this mechanism exists to keep cheap.

So the wake is **unconditional and cheap, and only when someone is
waiting**:

```c
/* quiesce_note_quiescent, after the publish */
if (!waitqueue_empty(&g_gp_wq))
    waitqueue_wake_all(&g_gp_wq);
```

`waitqueue_empty` is a list check; on the overwhelmingly common path
there is no grace period in flight, the queue is empty, and the cost is
one load. When a waiter *is* queued, every publish wakes it and the
waiter re-evaluates `quiesce_core_pending` — the condition that was
always the truth of the matter. A spurious wake costs one re-check.

The alternative — a per-generation count of CPUs still to publish,
decremented by the publisher — makes the publish path do arithmetic on a
shared line at every quiescent point. That is the wrong trade for a
mechanism whose read side is meant to be nearly free, and it is written
down here so the next reader does not have to re-derive why.

### 3. `wait_event_timeout` does not exist yet

`wait.h` has `wait_event` and `wait_event_killable` and no timed form.
This unit adds one, because the escalation above requires waking to send
straggler IPIs and to warn whether or not anything published.

That is a new primitive in a core header, so it is built to the shape of
the two beside it: prepare, test the condition, sleep with a deadline,
finish; return whether the condition became true. It gets its own test
rather than being covered only through quiesce — a timed wait that
returns early, one that times out, and one whose condition is already
true and never sleeps at all.

### 4. What this unit does not do

- **It does not convert callers to `call_quiesce`.** They want to wait.
- **It does not remove the straggler IPI**, whose value is still an open
  question (`next-subsystem-lifetime-windows.md`); it keeps it exactly as
  it is, on the same schedule, so this unit changes one thing.
- **It does not touch `quiesce_core_*`.** The epoch algebra is unchanged;
  this is about when a waiter is told.
- **It does not lower `TICK_NS`.** The floor is the sleep, not the tick.

## Tests

The assertion must not be a stopwatch. `docs/testing/flakes.md` is
explicit that "N things after a fixed settle" is the family that fails on
a loaded host, and its rule is to wait for a counter instead. So the
observable is a **count of sleeps**, not a duration:

`struct quiesce_stats` gains `gp_sleeps` — the number of times a waiter
actually slept. With wake-on-publish, a grace period on an otherwise
idle machine completes with **zero** sleeps; without it, every grace
period sleeps at least once, because the first check is made before any
CPU can have published.

| test | claim | how it fails if the change is reverted |
| --- | --- | --- |
| `quiesce-wake` | a grace period on an idle machine completes with **no** sleep: `gp_sleeps` is unchanged across a `synchronize_quiesce` | reverted to the poll, the same call sleeps at least once and the counter moves — an observable, not a time |
| `quiesce-wake-straggler` | a grace period *does* sleep when a CPU is genuinely slow to publish, and the existing straggler escalation still fires | the wake must not make the loop exit early: this is the existing `quiesce-straggler` spinner, asserting the kicks still happen |
| `wait-timeout` | `wait_event_timeout` returns true without sleeping when the condition already holds, true when woken, and false at the deadline | the three arms of a new primitive, tested where it lives rather than only through its first caller |
| the existing suite | `quiesce-straggler`, `-system`, `-idle`, `blk-submit-unregister`, `blk-unregister-drain`, `tcp-pcb-timer-free`, `device-remove-busy` unchanged | they are the correctness of the mechanism this unit speeds up; if any of them moves, the change was not what this report says it is |

**And a benchmark, reported rather than asserted**: `max_wait_ns` and
total `synchronizes` across a boot, before and after. A number in the log
is worth having and a number in an assertion is a flake.

## Affected files

| file | change |
| --- | --- |
| `kernel/include/kernel/wait.h` | `wait_event_timeout` |
| `kernel/core/wait.c` (or where the queue lives) | the timed wait's implementation |
| `kernel/core/quiesce.c` | the grace-period waitqueue; the loop waits on it; `quiesce_note_quiescent` wakes; `gp_sleeps` |
| `kernel/include/kernel/quiesce.h` | `gp_sleeps` in `struct quiesce_stats` |
| `kernel/core/quiescetest.c` | `quiesce-wake`, `quiesce-wake-straggler` |
| the wait test's home | `wait-timeout` |
| `docs/kernel/quiesce/design.md`, `invariants.md` | the wake, and why the poll stays |
| `docs/kernel/scheduler/api.md` (wait queues) | the new primitive |
| `docs/audit/2026-09-deferred-work-inventory.md` | §4's first item struck |
| `README.md` | the Status entry |

## New APIs

- `wait_event_timeout(wq, cond, ns)` — kernel, and the first timed wait
  in this tree.
- `quiesce_stats.gp_sleeps` — a counter, so the property is observable
  rather than timed.

No syscall, no uapi change, no change to `quiesce_core_*`.

## Invariant

**Q-W. A grace period ends when the last CPU publishes, and the waiter
learns no later than the next half-tick.** The wake is an optimisation
over a poll that is still there: `quiesce_core_pending` returning zero
remains the entire condition, `wait_event_timeout`'s deadline is
`TICK_NS / 2`, and a missed or spurious wake costs one re-check. So a
defect in the wake path is a latency regression and cannot be a hang or a
premature return. Check: `quiesce-wake` (no sleep on an idle machine, by
counter), `quiesce-wake-straggler` (a real straggler still sleeps and is
still kicked), and the existing quiesce and lifetime suites unchanged.
Gap: the wake fires on every publish while any waiter is queued, so a
grace period waiting on one slow CPU is woken by every other CPU's
quiescent points; that is measured in `gp_sleeps` rather than assumed to
be cheap.

## Migration plan

1. `wait_event_timeout` and its own test, before anything depends on it.
2. `gp_sleeps`, still polling — so the counter's meaning is established
   against the current behaviour and the test can be written to fail
   first.
3. The waitqueue, the wake, the loop.
4. The benchmark line, the docs, the inventory item, the README entry.

## Risks

- **A wake from the trap-return path is a scheduler operation in a hot
  place.** `wait.h` says wakers are interrupt-safe and `sock_wake` already
  does this from packet receive, but "documented safe" and "safe at every
  quiescent point on every CPU" are different claims, and step 3 is where
  that gets established rather than assumed. The `waitqueue_empty` guard
  keeps the common path to one load, which is the mitigation and also the
  thing to measure.
- **A spurious-wake storm.** Every publish wakes every waiter while one
  is queued. On an idle machine that is a handful of wakes; on a busy one
  with a slow CPU it could be many. `gp_sleeps` and a count of wakes make
  it visible, and if it is bad the answer is the per-generation counter
  Design §2 rejected — which would then be rejected on evidence instead
  of on reasoning.
- **The floor may not be where this report says.** The claim is that an
  idle-machine grace period is dominated by the sleep. Step 2 measures it
  before the wake exists, so if the sleep is not the cost, the unit says
  so and stops rather than shipping a change that buys nothing.

## Alternatives considered

- **Leave it.** The floor is small and every teardown pays it; the design
  has been named and unbuilt since the quiesce report.
- **A per-generation "CPUs still to publish" counter**, decremented by
  the publisher, waking exactly once when it reaches zero. Precise, no
  spurious wakes — and it puts an atomic decrement on a shared line into
  every quiescent point, which is the one path this mechanism keeps
  cheap. Rejected on that trade, and Design §2 records it so the next
  reader need not re-derive it. If the spurious-wake risk above turns out
  to be real, this is where to go.
- **Shorten the poll.** A 1 ms sleep quarters the floor and quadruples
  the wakeups, and still ends grace periods late. It trades the same
  thing in the same direction for less.
