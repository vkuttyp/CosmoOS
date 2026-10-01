# NEXT SUBSYSTEM — the sleeping mutex has no priority inheritance

> Constitution §68 report. This PR adds the report and the probe
> (`tools/priority-inheritance-probe.py`); the priority-inheritance design
> under "Design" and the edits in "Affected files" are planned work that lands
> in the implementation PR that follows, gated on CI. As committed here, a
> mutex owner's priority is never raised to a blocked waiter's, so the
> priority inversion below is reachable.

## Problem

The sleeping mutex (`kernel/scheduler/mutex.c`) records its `owner` and parks
contending threads on a wait queue (`wait_event` on `owner == NULL`). It never
raises the owner's scheduling priority to that of a higher-priority thread
waiting for it. With a fixed-priority preemptive scheduler (`policy_rr.c`: 64
levels, lower number wins, a strictly higher-priority runnable thread always
preempts) this is the classic **unbounded priority inversion**:

- a low-priority thread **L** holds mutex **M**;
- a high-priority thread **H** blocks on **M**;
- a medium-priority thread **Mid**, holding nothing, is runnable.

`Mid` outranks `L`, so `Mid` runs; `L` cannot make progress to its
`mutex_unlock`, so `M` stays held; so `H` — the highest-priority thread in the
system — waits for `Mid`, a thread it outranks, to finish. The high-priority
thread's blocking time is bounded not by `L`'s critical section but by
`Mid`'s entire run, and by any number of medium threads that arrive. The
priority of `H` has been defeated by `Mid`.

The pieces a fix needs already exist. `struct mutex` has `owner`
(`kernel/include/kernel/mutex.h`). `struct thread` has `priority`, the value
`policy_rr` keys on, and the policy exposes `enqueue`/`dequeue`, so a ready
thread's level can be changed by dequeue + re-enqueue. What is missing is any
code that does so on behalf of a blocked waiter, and the bookkeeping to undo
it: there is no "effective vs. base" priority, no record of a mutex's waiters'
priorities, and no scheduler entry point to reprioritize a thread that may be
ready, running, or blocked — possibly on another CPU's run queue.

Constitution §20-22 and the inventory name it plainly: `docs/audit/2026-09-deferred-work-inventory.md`
§2.3, "**no priority inheritance in `mutex.c`**".

### Measured

`tools/priority-inheritance-probe.py` adds a self-test, `prio-inversion`, that
stages the three-thread inversion on one CPU and reports it (two debug boots,
x86-64, deterministic):

```
PRIOINV: a medium-priority thread ran to completion before a high-priority thread blocked on a mutex held by a low-priority one could acquire it; mutex.c has no priority inheritance
```

The race is made deterministic rather than hoped for: `L` parks **holding**
`M` (on a semaphore), `H` is created and blocks on `M`, `Mid` is created and
parks; then `Mid` and `L` are released **together** so the scheduler's
priority order alone decides who runs, and `H` records, at the instant it
acquires `M`, whether `Mid` had already finished. Without inheritance it
always has (`h_saw_mid == 1`). Every wait is bounded by a deadline, so a
regression is a failed check, not a hung boot.

## Why it matters

- **Priority means nothing under contention.** The whole point of a priority
  is that a higher-priority runnable thread runs. A mutex shared between a
  high- and a low-priority thread silently suspends that guarantee whenever a
  medium thread exists — and the kernel's own locks are shared across
  priorities (a high-priority I/O completion path and a low-priority
  housekeeping thread can take the same mutex).
- **The blocking bound is unbounded.** Without inheritance, `H`'s wait is the
  sum of every medium thread's run, not `L`'s critical section. Inheritance is
  what makes the bound the critical section, which is the property real-time
  and latency-sensitive work depends on.
- **It is the named gap.** §2.3 lists it; this is the unit that closes it.

## The implementation before this unit

| piece | where | state |
|---|---|---|
| mutex owner | `struct mutex::owner` (`mutex.h`) | tracked; set in `try_take`, cleared in `mutex_unlock` |
| contended wait | `lock_common` → `wait_event(&m->wq, owner == NULL)` (`mutex.c`) | parks the waiter; no donation to the owner |
| scheduling priority | `struct thread::priority` (`thread.h`) | the single value `policy_rr` keys on; set only at creation (`thread.c`) |
| ready-queue reorder | `g_policy->enqueue`/`dequeue` (`policy_rr.c`) | a level change is a dequeue + re-enqueue; no caller does it after creation |
| cross-rq lock order | `sched_migrate` under both rq locks in CPU-id order, each rq lock its own lockdep class (S24) | exists for migration; a reprioritize of a thread on another CPU reuses it |

## Design

### 1. Effective vs. base priority

`struct thread` gains `int base_prio` — the priority it was created with.
`priority` becomes the **effective** priority the scheduler uses: normally
equal to `base_prio`, and lowered (raised in urgency) while the thread holds a
mutex a higher-priority thread waits on. All existing reads of `priority`
(the policy, the preemption checks, the balancer's placement) are unchanged;
they simply see the effective value, which is the point.

### 2. What a mutex records

`struct mutex` gains the means to answer "the highest priority among my
waiters": either a cached `int top_waiter` updated as waiters arrive and
leave, or a walk of the wait queue (small and uncontended at unlock). The
owner is already tracked. `struct thread` gains an intrusive list of the
mutexes it currently holds (a node per mutex), maintained in **all** builds —
lockdep's `held_mutex[]` is debug-only and cannot be relied on — so that on
release the owner's correct priority can be recomputed from the mutexes it
still holds.

### 3. Donation on block

When `lock_common` finds the mutex owned (before it waits), the waiter donates
its priority: the owner's effective priority is lowered to
`min(owner->priority, waiter->priority)`, and if the owner is **itself**
blocked on another mutex, the donation propagates up that chain —
`owner → owner's-mutex's-owner → …` — each boosted to at least the waiter's
priority. The chain is bounded: the lock-acquisition order is a DAG (lockdep
enforces no cycle, S-class order), so propagation terminates; a depth guard is
the belt-and-braces. Each boosted thread that is **ready** is requeued at its
new level (§5); one that is **running** keeps running (it is already the one
we want to run); one that is **blocked** takes the new level when it next
wakes, and the chain carries on from it.

### 4. Restoration on unlock

`mutex_unlock` recomputes the releasing owner's effective priority as
`min(base_prio, the highest top_waiter among the mutexes it still holds)` and
reprioritizes it (§5) before waking a waiter. Dropping the just-released
mutex from the thread's held-list first means a thread that was boosted only
for `M` returns to `base_prio`; one still holding another contended mutex
keeps the boost that mutex still demands. The woken waiter acquires `M`; its
own effective priority is whatever its base and its other held mutexes make
it.

### 5. The scheduler reprioritize primitive

A new entry point — `sched_set_effective_prio(struct thread *t, int prio)` —
changes `t->priority` safely wherever `t` is:

- it takes `t`'s run-queue lock (the queue `t->cpu` names), which may be a
  remote CPU's; a donation runs in the waiter's context and the owner may be
  elsewhere, so this obeys the increasing-CPU-id two-lock order the migration
  unit established (S24), and its own rq-lock lockdep class;
- **ready** → `dequeue`, set `priority`, `enqueue`; if the new level now
  outranks that CPU's current thread, `request_resched` (an IPI to a remote
  CPU);
- **running** → set `priority`; a raise needs nothing, a drop may let a ready
  thread preempt, so `request_resched` if so;
- **blocked** → set `priority`; it enqueues at the new level on wake. The
  donation chain continues from the mutex it is blocked on.

Migration already moves a thread's `priority` with it; a thread boosted while
ready and then migrated carries its effective priority, and the balancer —
which counts threads, not priority (S29) — is unaffected.

## Affected files

| file | change |
|---|---|
| `kernel/include/kernel/thread.h` | `base_prio`; the held-mutex list head |
| `kernel/include/kernel/mutex.h` | the held-list node; the waiters' top-priority record |
| `kernel/scheduler/mutex.c` | donate on contended lock (with bounded chain propagation); recompute and restore on unlock; maintain the held-list and `top_waiter` |
| `kernel/scheduler/sched.c`, `kernel/include/kernel/sched.h` | `sched_set_effective_prio(t, prio)` — reprioritize a ready/running/blocked thread under the (possibly remote) rq lock, S24 order |
| `kernel/scheduler/thread.c` | set `base_prio` at creation beside `priority` |
| `kernel/scheduler/schedtest.c`, `kernel/include/kernel/selftest.h`, `kernel/core/selftest.c` | the `prio-inversion` test |
| `docs/kernel/scheduler/*.md`, `README.md` | the design and a Status entry |

## APIs

No user ABI change. `sched_set_effective_prio` is a kernel-internal entry
point; priority inheritance is automatic and invisible at the mutex API.
Inheritance is active in every build (not debug-only): the inversion is a real
scheduling defect, not a diagnostic.

## Tests

Planned for the implementation.

| test | proves |
|---|---|
| `prio-inversion` | with inheritance, the staged three-thread scenario resolves the other way: the high thread acquires the mutex **before** the medium thread finishes (`h_saw_mid == 0`), because the low owner was boosted and preempted the medium thread. The probe's `== 1` becomes the implementation's `== 0`. |

**Planned mutations** (each alone, boot confirmed):
- donation removed (no boost on block): the medium thread runs first again →
  `h_saw_mid == 1` → the test fails, as the probe shows today.
- restoration skipped (owner keeps the boost after unlock): a later lower-
  priority section runs at the boosted level; a second scenario that checks
  the owner is back at `base_prio` after release fails.
- the chain not propagated (boost only the direct owner): a nested case —
  `H` blocks on `M` held by `Mid2` which is blocked on `N` held by `L` — leaves
  `L` unboosted and `H` waits again.

## Benchmarks

None required; the cost is a short bounded walk on the contended path only.
A note on `mutex_lock`/`unlock` throughput on the uncontended path (which must
stay a single `try_take`) is worth recording.

## Risks

- **Cross-CPU reprioritize.** The one real hazard: lowering/raising a thread
  on another CPU's run queue. It reuses the migration unit's two-rq-lock order
  (S24) and per-rq lockdep classes rather than inventing a second scheme; a
  donation never holds the mutex's own spinlock across the rq lock in an order
  that could invert against it.
- **The chain and cycles.** Propagation follows the lock-acquisition DAG;
  lockdep already forbids a cycle, and a depth bound is the guard against a
  mistake.
- **Restoration correctness.** The subtle part is dropping exactly the right
  boost on unlock when a thread holds several contended mutexes; the held-list
  + per-mutex `top_waiter` make the recompute exact rather than a guess, which
  is why both are maintained in all builds.

## Alternatives considered

- **Priority ceiling (raise on acquire to a static ceiling).** Simpler and
  deadlock-free, but needs every mutex annotated with a ceiling and over-raises
  the common uncontended case; inheritance pays only under actual contention
  and needs no annotation.
- **One-level donation only (no chain).** Smaller, but wrong for nested locks —
  a held mutex whose owner is itself blocked leaves the real culprit
  unboosted. The chain is bounded and cheap; correctness is worth it.
- **A `base_prio`-free scheme that saves the old priority on the mutex.** Fails
  with multiple waiters or multiple held mutexes (which old value to restore?);
  the effective/base split with a held-list is the general, correct form.
