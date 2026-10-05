# Lockdep callback classes: synchronous timer waits in the lock graph

Date: 2026-10-05. Branch `lockdep-callback-irq` from `main` at `93f2268b`.
Scope: the first half of `docs/plan.md` §1 "Implementation — callback-wait
dependencies", from the plan's third suggested increment. The other half,
raw IRQ save/restore pairing, is a separate follow-up PR. It needs per-thread
state across context switches (below) and touches the same lockdep core, so
each change gets its own validation and review.

## Summary

Before this change, a `timer_cancel_sync` that might deadlock against its
callback was caught only by a **per-object profile**: the exact lock
objects taken by the callback execution currently in progress on the very
timer being cancelled. Three deadlock shapes were therefore invisible:

1. a wait on a timer whose callback is not running at that moment, even if
   a callback of the same function has taken the held lock before;
2. a held lock the callback never takes directly but reaches through a
   chain recorded elsewhere;
3. a wait recorded first, followed later by a callback that takes the
   waited-across lock.

Each timer callback **function** now has a lockdep pseudo-class
(`LOCKDEP_KIND_CALLBACK`). A callback holds its class while it runs, so
every lock it takes records `callback → lock`. `timer_cancel_sync`
acquires the class without holding it on every call, recording `held →
callback`. The existing cycle check then reports all three shapes,
persistently across executions and transitively. Applied to the existing
suite, it reported **no** deadlock in the current tree: no lock held across
a timer wait anywhere in it closes a cycle with any callback.

## Coverage of synchronous callback waits

The kernel's synchronous waits for a callback or handler were enumerated
from the source:

| Wait | Before | After |
|---|---|---|
| `timer_cancel_sync` | per-object profile of the running execution (L15) | callback classes (L19), profile kept |
| `synchronize_irq`, `interrupt_unregister_sync`, `interrupt_unregister_vector_sync` | not modelled | not modelled, and argued unnecessary: they wait through `synchronize_quiesce`, which calls `might_sleep()`, so the waiter holds no spinlock, and a handler can take no mutex; no lock cycle through the wait exists |
| module teardown | not modelled | same argument (it waits through grace periods) |
| `wait_for_completion` on a completion a callback signals | not modelled | not modelled: no function identity to key a class on |

## Design decisions

- **Class per function, not per timer object.** Two timers sharing a
  function share the class. That is what makes a wait on one timer
  answerable from what another timer's callback did, and it is the usual
  class-level conservatism of lockdep. A report can name a lock object
  other than the one a particular callback would take.
- **The wait is recorded on every call**, not only when the callback is
  running, because a synchronous cancel may wait whenever it is called.
- **A report returns without waiting**, as the profile check already did,
  so a self-test can consume the report and continue.
- **The class has no interrupt-usage labels.** `check_usage` labels
  spinlock classes only. The locks taken inside the callback carry its
  interrupt context as before.
- **Capacity.** The class table holds callback classes too, and a debug
  boot of `main` already reached 307 of 320 classes. The first boot of the
  change overflowed the table: about a dozen callback classes on top of a
  nearly full pool. `LOCKDEP_MAX_CLASSES` is now 384. A full x86-64 boot
  reaches 324 classes, so the callback classes number 17. The graph grows
  from 200 KiB to 288 KiB, in LOCKDEP builds only.
- **No string formatting per tick.** A cached class takes its name from
  the graph's own copy. Only the first callback of a function formats
  `callback <address>`.

## Tests

`lockdep-callback` (`kernel/core/lockdeptest.c`, pinned, so the report in
case 3 lands on the expecting CPU):

1. callback A takes `a` once; the thread holds `a` and cancels a **different
   timer** with the same function that has never run: `LOCKDEP_R_CALLBACK`,
   and the cancel returns without waiting. An unrelated lock across the
   same cancel reports nothing;
2. callback B takes `b1`, and `b1 → b2` is recorded elsewhere; holding `b2`
   across a B cancel is reported (`b2 → B → b1 → b2`). The callback never
   took `b2`;
3. a cancel of a never-run C timer made while holding `c` reports nothing,
   since nothing is known yet. C's first callback then takes `c` under its
   class and is reported as an inversion.

**Negative controls** (`tools/lockdep-callback-probe.py`, throwaway
worktrees, both architectures):

| Mode | Removes | Result |
|---|---|---|
| `no-wait` | the wait check (only the per-object profile remains) | `lockdep-callback` fails at case 1's `hits == 1`: the old mechanism does not see it |
| `no-class` | the callback's class (enter/exit no-ops) | fails at the same check |

The existing `timer-cancel-sync` self-test still gets exactly one
`LOCKDEP_R_CALLBACK`. Its case is now caught by the class check at the
start of the cancel, which returns before the profile check would report
it again.

## A sighting this branch did not explain

One AArch64 boot failed `prio-inversion` ("the medium thread finished
before the high thread acquired"). That test had passed in about 45 AArch64
boots of `main` that day. A probe running the scenario 300 times in one
boot saw 0 failures with the change and 0 on `main`. The obvious window,
`waitqueue_prepare` marking the high thread BLOCKED before it donates
priority, is closed by `schedule_internal` re-queueing a preempted BLOCKED
thread as READY. Recorded in `docs/testing/flakes.md` as a first sighting,
not attributed. The change makes timer callbacks heavier and so moves
timing windows: `prio-inversion` takes 53–58 ms on this branch against
47–50 ms on `main`.

## Validation

On `e23b075b`, one chain, every row run (`out/cbfinal/*.log`):

| Step | Result |
|---|---|
| `make host-test` (lockdep host models at 384 classes, 1536 nodes) | PASS |
| `make host-test-lockdep-tsan` | PASS |
| x86-64 debug, 4 / 2 / 1 CPUs | PASS 139.5 / 128.5 / 115.5 s, 424 self-tests each |
| AArch64 debug, 4 / 2 / 1 CPUs | PASS 137.2 / 133.8 / 118.6 s, 424 self-tests each |
| `test-chaos`, both | PASS 132.6 s, 131.1 s |
| release build + boot, both | PASS 16.9 s, 20.1 s |
| `make analyze`, both | "static analysis: clean" |
| `tools/lockdep-callback-probe.py`, `no-wait` and `no-class`, both architectures | 4 of 4 required failures |

`lockdep-callback`, `timer-cancel-sync` and `prio-inversion` passed in all
six debug boots. Counting the boots made while diagnosing, `prio-inversion`
passed in 11 of 12 boots of the branch.

**Overhead.** x86-64, four CPUs, `main` and the branch booted back to back,
alternating. Total self-test time:

| Run | `main` | branch |
|---|---|---|
| 1 | 106.3 s | 108.1 s |
| 2 | 110.1 s | 110.2 s |

The difference is within `main`'s own run-to-run spread (3.8 s), so the
cost is not measurable at boot level. An earlier single-sample comparison
had suggested +10 s. That was host variation (different times on a shared
host), together with the per-tick name formatting that was then removed.

## Remaining

- Raw `arch_irq_save`/`arch_irq_restore` pairing: the follow-up PR. A
  correct validator needs per-thread state, because a raw save can be
  outstanding across a context switch, so a per-CPU stack would pair one
  thread's restore with another's save.
- A callback path that never executes contributes no edges; completion
  waits stay outside the graph.
