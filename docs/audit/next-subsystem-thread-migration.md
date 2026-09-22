# NEXT SUBSYSTEM — a thread that can never move, on a CPU chosen once

Date: 2026-09-16. Tree: `main` at f295e42 (after PR #156, the CPU
clock). Chosen from `docs/audit/2026-09-deferred-work-inventory.md`
§2.3.

**Subsystem: thread migration and load balancing — moving a runnable
thread from one CPU to another, and deciding when to.**

This report **takes up** the inventory's §2.3 row that reads "no load
balancing and no migration (a thread stays on the CPU chosen at
creation; confirmed 2026-09-14)". The row stays open until the
implementation lands; step 7 of the plan is what strikes it.

## Problem

**A thread is assigned a CPU once, by a rule that prefers CPU 0, and
then stays there for the rest of its life.**

> As of this unit the first half is fixed and the second is not: ties
> rotate, so a thread is no longer *born* on CPU 0 by default, but
> nothing moves it afterwards. The measurements below are the state
> before either change.
>
> **Since then** (`docs/audit/next-subsystem-percpu-migration.md`): the
> corruption this report could not name was `schedule_internal` itself
> reading its per-CPU block before taking the run-queue lock, with
> sixteen more sites of the same kind on x86-64 and the EL2 hand-back on
> AArch64; the rule those sites broke is S25 and the debug accessors
> check it. A migration primitive exists and the suite runs under a
> chaos migrator. The balancer this report designed is the unit after.

Two facts, each small, and together a machine that uses one of its four
CPUs.

### The assignment happens once

`pick_cpu` is called from exactly one place, `sched_enqueue_new`
(`kernel/scheduler/sched.c:176`), which runs when a thread is first made
runnable. After that, `t->cpu` is written in three places and none of
them is a migration:

| site | what it does |
| --- | --- |
| `sched.c:77`, `sched.c:104` | the idle and boot threads, at init |
| `sched.c:180` | `sched_enqueue_new` — the one assignment |
| `sched.c:265` | `next->cpu = (int)rq->cpu` in the switch — the same CPU, restated |
| `thread.c:128` | `-1` at creation, before any queue |

And a thread that blocks and wakes goes back where it was:
`sched_wake` takes `struct runqueue *rq = &g_rqs[t->cpu]`
(`sched.c:314`). There is no path, on any CPU, in any state, that puts a
thread on a different runqueue than the one it was born on. Not even
CPU offline: nothing in `sched.c` reacts to a CPU going away by moving
its threads.

### And the assignment prefers CPU 0

```c
static unsigned pick_cpu(const struct thread *t)
{
    unsigned best = this_cpu()->cpu_id;
    unsigned best_load = ~0u;
    for (unsigned c = 0; c < cpu_count(); c++) {
        if (!(t->affinity & CPUMASK_OF(c)) || !cpu_online(c))
            continue;
        unsigned load = g_rqs[c].nr_running;
        if (load < best_load) { best_load = load; best = c; }
    }
    return best;
}
```

The comparison is strictly `<`, so a tie is won by the **lowest-numbered
CPU examined**. On an idle machine every `nr_running` is 0, the first
iteration takes `best = 0`, and no later iteration can displace it.
Every thread created while the machine is not already busy goes to
CPU 0.

`nr_running` is also the wrong quantity to tie-break on, because it
counts what is *runnable now*: four worker threads that each block on
I/O leave every queue at zero, so the fifth, sixth and seventh workers
all go to CPU 0 as well.

### What that produces, measured

From `sched_dump` and `thread_dump_all` in one x86-64 boot of `main` at
f295e42, four CPUs:

| | CPU 0 | CPU 1 | CPU 2 | CPU 3 |
| --- | --- | --- | --- | --- |
| threads | **8** | 2 | 2 | 2 |
| context switches | **22197** | 986 | 256 | 294 |

Fourteen threads, 8 of them on CPU 0, and CPU 0 doing 94% of the
switching. The 2 apiece elsewhere are that CPU's `idle` plus one thread
a test pinned there deliberately with `thread_create_on`. **Essentially
every thread this kernel creates for itself runs on CPU 0**, and the
other three CPUs are available only to code that asks for them by name.

## What this unit's predecessor assumed

The CPU-clock unit merged today (PR #156) swept 66 timestamp
subtractions, and the rule it used to classify them was:

> A stamp is **foreign** iff the CPU that wrote it may differ from the
> CPU that reads it — which for a local variable means iff the thread
> can be descheduled between the two reads.

with the worked example

```c
uint64_t t0 = clock_now_ns();
thread_sleep_ms(500);
uint64_t dt = clock_now_ns() - t0;   /* t0 is a foreign stamp */
```

**That example is false in this kernel.** A thread that sleeps wakes on
the CPU it slept on, so 42 of the 66 sites that unit converted — every
one of the "local `t0` with a sleep between the reads" category — are
same-CPU today. The conversions are harmless (a saturating subtraction
costs a compare and is a no-op when the stamp is genuinely in the past)
and they are *right for the kernel this unit is about to create*. But
the stated reason is wrong, and it is stated in
`kernel/include/kernel/timer.h`, in that unit's report, and in the
README.

So this report carries a debt, and it is the first thing in the
migration plan: **either this unit lands and the claim becomes true, or
the claim gets corrected.** It does not get to stay as it is. That is
also the sharpest argument for doing this unit next — the tree currently
documents a property of its scheduler that its scheduler does not have.

And the direction of the hazard is worth naming: once threads migrate,
every one of those 42 sites becomes genuinely cross-CPU. The previous
unit's sweep stops being defensive and starts being load-bearing, which
is why step 6 of this plan re-runs it rather than trusting it.

## Current implementation

`kernel/scheduler/sched.c` holds one `struct runqueue` per CPU in
`g_rqs[]`, each with its own `lock`, its own `nr_running` and its own
bitmap, and `kernel/scheduler/policy_rr.c` is the only policy. The
design is already per-CPU in every respect that matters for migration:
the queues are separate objects with separate locks, and the switch path
takes only the local queue's lock.

What is missing is any code that holds two of those locks at once, which
is what moving a thread between them requires.

## Why it matters

- **Three quarters of the machine is idle under kernel load.** The
  measured 22197:986:256:294 is not a benchmark artefact; it is a boot
  running the self-test suite, which is the heaviest thing this kernel
  does.
- **`thread_create_on` is the only way to use another CPU**, so every
  piece of code that wants parallelism has to do its own placement, and
  several tests already do exactly that. Placement by hand at every call
  site is the thing a scheduler is for.
- **A blocked-thread pile-up is invisible until it is not.** Because
  `nr_running` ignores blocked threads, a server that creates a thread
  per connection puts all of them on CPU 0 and nothing reports it.
- **The tree documents migration it does not have** (above).
- **CPU offline strands threads.** `smp_stop_others` takes CPUs down;
  anything runnable on one of them is left on a queue nobody will
  service. No test covers this today because nothing migrates, so there
  is nowhere for such a thread to go.

## Design

> **Reverted, except where marked.** Everything from here to "As built"
> is the plan as written before implementation. **Only the placement
> change (the tie-break, "1. Spread at creation" below) shipped.** The
> balancer, `sched_balance`, `policy->pick_migratable`, the
> `SCHED_BALANCE_*` tunables, the two-lock rule as *implemented*, and
> the `sched-balance-*` and `sched-affinity-survives` tests were built,
> measured, and removed — see "As built" for the measurement that
> removed them. The affected-files table, the New APIs block, the
> migration plan and the test table below describe what was attempted,
> not what is in the tree.



### Two changes, and the small one comes first

**1. Spread at creation.** `pick_cpu`'s tie-break becomes round-robin
over the CPUs that tie, rather than always the lowest. One counter, one
modulo. On its own this removes the CPU-0 pile-up for the common case of
threads created on an idle machine, and it is independently testable and
independently revertible.

**2. Balance afterwards.** A periodic pull: an under-loaded CPU takes a
runnable thread from the busiest one. Pull rather than push, because the
CPU doing the work is the one that will run the thread, and because it
lets a CPU decline to participate simply by not asking.

### What may move, and what may not

| thread state | may it be migrated? | why |
| --- | --- | --- |
| `THREAD_READY`, in a runqueue | **yes** | it is a queue entry and a saved context; nothing else refers to its CPU |
| `THREAD_RUNNING` | **no** | it is executing on that CPU's stack with that CPU's state. Moving it is a context switch that CPU must perform itself |
| `THREAD_BLOCKED` | **no**, and it does not need to | it is not in a runqueue at all; `sched_wake` puts it back on its existing `t->cpu` (`sched.c:314`) and this unit does not change that. Re-picking a CPU at wakeup is a real option and a natural follow-up — see Alternatives — but it is not what the balancer does |
| affinity excludes the target | **no** | `thread_create_on`'s guarantee is the whole point of the call |

So the balancer moves **ready threads only**, which is the case with no
ownership question: the thread is not running anywhere, its context is
saved, and the two runqueue locks are the only things that refer to it.

### The lock order, stated before it is written

Moving a thread means holding two runqueue locks. Two CPUs balancing
towards each other at the same moment is the classic deadlock, so:

> **Runqueue locks are taken in increasing CPU-id order, always.**

A balancer that wants to move from `busy` to `me` takes
`min(me, busy)` first. This is a rule the tree does not have yet because
nothing has ever needed two of them, and it goes in
`docs/kernel/scheduler/invariants.md` next to the existing ones.

### When to balance, and when not to

The cost of moving a thread is a cold cache and a cold TLB on the
destination. A balancer that moves threads back and forth every tick is
worse than none, so:

- **Period**: every `SCHED_BALANCE_TICKS` (start at 16, 64 ms at
  `CONFIG_HZ` 250), from the tick, on each CPU, for itself.
- **Hysteresis**: pull only when the busiest queue has at least two more
  runnable threads than this one. A difference of one is the steady
  state of an odd thread count, and chasing it is thrash.
- **One at a time**: a pull moves a single thread. The next period will
  move another if the imbalance persists, and if it does not, nothing
  more moves.

### What it does not do

- **No NUMA awareness and no cache topology.** There is one node and no
  `SRAT` parsing (§2.2 of the inventory), so "the busiest queue" is the
  only signal available. When NUMA lands, the pull will need a distance
  term, and this design leaves room for one without inviting it now.
- **No change of policy.** `policy_rr.c` stays the only policy;
  fairness, deadline and real-time scheduling are a separate row of
  §2.3.
- **No CPU isolation, no cgroup-like grouping.**
- **No migration of running threads**, which is the hard half and needs
  a cross-CPU "please switch away" handshake. Filed if it is ever
  wanted; the pull covers the load problem without it.
- **No automatic evacuation on CPU offline.** It becomes *possible*
  once threads can move, and the test below proves the mechanism, but
  wiring it into the offline path is its own unit with its own
  shutdown-ordering questions.

## Affected files

| file | change |
| --- | --- |
| `kernel/scheduler/sched.c` | `pick_cpu` tie-break; `sched_balance()`; the two-lock move; the lock-order rule |
| `kernel/scheduler/policy_rr.c` | `policy->pick_migratable` — *which* thread may leave; `dequeue` already removes one |
| `kernel/include/kernel/sched.h` | `sched_balance`, the tunables, the debug hooks |
| `kernel/timer/timer.c` | the periodic call from the tick |
| `kernel/scheduler/smptest.c` | the new tests |
| `kernel/core/selftest.c`, `kernel/include/kernel/selftest.h` | registration |
| `docs/kernel/scheduler/design.md`, `invariants.md` | the balancer and the lock order |
| `kernel/include/kernel/timer.h` | the claim the clock unit made, now true (or corrected) |

## New APIs

```c
/* kernel/include/kernel/sched.h */

/*
 * Move one runnable thread to this CPU from the busiest one, if the
 * imbalance is worth the cold cache. Called from the tick every
 * SCHED_BALANCE_TICKS; safe to call at any time from any CPU for
 * itself, and a no-op when this CPU is not the least loaded.
 *
 * Only THREAD_READY threads move, and only where affinity allows.
 */
void sched_balance(void);

#if CONFIG_DEBUG
/* How many threads this CPU has pulled: a diagnostic and a benchmark
 * number, deliberately *not* what the tests assert on. A count says
 * something moved, not that the threads under test moved, and the suite
 * has other threads in it -- sched-balance-pull records each worker's
 * t->cpu instead and checks that one of those changed. */
uint64_t sched_test_migrations(unsigned cpu);
void sched_test_set_balancing(bool on);
#endif
```

## Migration plan

1. **The claim the clock unit made.** Before any behaviour changes,
   `kernel/include/kernel/timer.h`, that unit's report and the README
   say a sleeping thread can wake elsewhere. Mark each as pending on
   this unit, so that a tree between the two units does not assert
   something false. If this unit is abandoned, that edit is the whole
   correction and it stands alone.
2. **The tie-break**, alone and testable: `pick_cpu` spreads ties
   round-robin. The CPU-0 pile-up goes away for threads created on an
   idle machine, and nothing has migrated yet.
3. **The lock order**, written into `invariants.md` before the code
   that needs it exists.
4. **`policy->pick_migratable`** in `policy_rr.c`. The policy interface
   already has `dequeue(rq, t)`, so removal is solved; what is missing is
   *choosing*: a ready thread that is not `rq->current` and whose
   affinity admits the destination. Returns NULL when the queue has
   nothing that may leave, which is the common case for a queue of one.
   No caller yet.
5. **`sched_balance`**, called from the tick. The pull, the hysteresis,
   the affinity check, the two-lock move.
6. **The clock sweep re-run by grep**, because migration makes its 42
   local sites genuinely cross-CPU. Any site the previous unit missed
   becomes a real bug the moment step 5 lands, so this is a gate, not a
   courtesy.
7. Docs, README Status, the inventory row struck, as-built and as-run.

Each step boots both architectures; steps 2 and 5 run `make BUILD=release`,
and the whole CI step list (`host-test`, `fuzz`, `analyze`,
`reproducible`, `test-gic`, `test-guard`, `test-crash`, `test-wxn`) runs
before the pull request, because a scheduler change is exactly the kind
that passes `make test` and fails something else.

## Tests

| test | what it asserts | bug-proof (what makes it fail for the stated reason) |
| --- | --- | --- |
| `sched-spread-at-create` | with every queue equal, N successive `thread_create` calls land on N different CPUs rather than all on CPU 0 | restore the `<` tie-break: every thread goes to CPU 0 and the test names the count it saw there |
| `sched-balance-pull` | the imbalance is created **after** placement, so only a migration can fix it: M unpinned spinners are created first and land wherever `pick_cpu` puts them, **each one's `t->cpu` is recorded**, and only then are K spinners pinned to CPU 0 with `thread_create_on`. CPU 0 now carries its share plus K. The assertion is per-thread: at least one recorded worker's `t->cpu` differs afterwards | `sched_test_set_balancing(false)`: no worker's `t->cpu` ever changes, and the test prints the recorded and final placement side by side |
| `sched-migration-happens` | a thread's `t->cpu` observably changes while it is alive — the bare fact the clock unit assumed and this kernel did not have | the same disable: `t->cpu` never changes, which is the tree as of f295e42 |
| `sched-affinity-survives-balance` | a thread pinned with `thread_create_on` is never moved off its mask, however lopsided the load | make the balancer skip the affinity check: it moves, and the test names the thread, its mask and the CPU it landed on |
| `sched-running-not-stolen` | the thread currently running on a CPU is never taken by a balancer on another; only `THREAD_READY` moves | let `pick_migratable` return `rq->current`: the victim CPU's switch path finds its current thread on another queue, which the test detects as a state mismatch before it can corrupt anything |
| `sched-balance-hysteresis` | a difference of one runnable thread does not move anything, across many periods | drop the hysteresis to `> 0`: threads ping-pong, and the test counts migrations that should not have happened |
| `smp-parallel`, `smp-pinned` (existing) | unchanged | — |

**Vacuity, named in advance — and the first version of this paragraph
was wrong.** `sched-balance-pull` is the test at risk: if creation-time
placement produced the spread, it proves nothing about balancing. The
first draft tried to prevent that by loading CPU 0 before creating the
workers. That does not work, and review caught it: `pick_cpu` picks the
*least* loaded CPU, so a busy CPU 0 makes it place every worker
elsewhere, and the spread the test measured would have been creation's
doing entirely.

The order is inverted instead. The workers are created first, onto
whatever CPUs `pick_cpu` chooses, and **each one's `t->cpu` is recorded
at that moment**. The imbalance is manufactured *afterwards*, by pinning
K spinners to CPU 0 — legitimate load that only that CPU can carry. Any
later change to a recorded `t->cpu` therefore happened after placement,
which is migration by definition.

For the same reason the assertion is per-thread rather than a global
counter: a count says something moved, not that *these* threads moved,
and the suite has other threads in it.

## Benchmarks

- **Thread placement across a full self-test boot**, before and after:
  the table in the Problem section, recomputed. The number to move is
  "8 of 14 on CPU 0".
- **Context switches per CPU** over the same boot: 22197:986:256:294
  today.
- **The cost of a migration**, in microseconds, and the cost of the
  balancer on a balanced machine — which should be a load of
  `nr_running` per CPU per 64 ms and nothing else.

## Risks

- **Two locks is where deadlock lives.** The rule is stated above and
  goes into `invariants.md` before the code; lockdep is already in this
  tree and will see the order.
- **A thread in flight is where use-after-free lives.** The lifetime
  unit (PR #154) is the precedent: the window between taking a thread
  off one queue and putting it on another must not be observable by a
  third CPU's `sched_wake`. The move happens with both locks held, which
  makes it atomic with respect to any other queue operation.
- **Thrash is worse than imbalance.** Hysteresis and a 64 ms period are
  the defence, and `sched-balance-hysteresis` is the test that says so.
- **This makes the clock unit's contract load-bearing.** Step 6 is the
  mitigation and it is a gate.
- **A balanced machine may be slower** for workloads whose threads share
  cache. There is no measurement of that here and this report does not
  claim otherwise; the benchmark above is the honest version, and if it
  shows a regression the hysteresis is the knob.

## Alternatives considered

- **Push instead of pull**: the busiest CPU hands work away. Rejected:
  it makes the loaded CPU do more work, and it needs the target's lock
  while the target is running, which is the same two-lock problem with
  worse timing.
- **Balance only at wakeup** (re-run `pick_cpu` in `sched_wake`).
  Tempting, and one line. Rejected as the *whole* answer because it does
  nothing for a thread that never blocks — which is exactly the spinning
  worker the load problem is about — but it is a natural follow-up and
  the design does not preclude it.
- **Fix only the tie-break** and call the row closed. It is step 2 and
  it is real, but it leaves every long-lived thread wherever it started,
  and the inventory row says "no load balancing **and no migration**".
- **Full CFS-style load tracking** with per-thread weights and decayed
  averages. That is a scheduling-policy unit; this one is about whether
  a thread can move at all, and `nr_running` is enough to answer it.

### As built

**Half of this unit shipped. The half it is named for did not, and that
is the result rather than an excuse.**

#### What shipped: placement

`pick_cpu` rotates its ties. The scan is still least-loaded-first; only
the tie changes, and the tie is what mattered — `nr_running` counts what
is runnable *now*, a kernel thread is blocked almost all of its life, so
between any two creations the queues have drained to zero, every CPU
ties, and a scan that keeps its first winner gives every thread to
CPU 0.

The test for it had to be designed around a surprise: threads created
back-to-back **already** spread before this unit, because each one
raises its target's count and the next scan sees it. A test that created
four spinners would have passed on the broken code. `sched-spread`
therefore creates each worker, waits for it to signal *and block*, and
only then creates the next — the one shape that distinguishes the
rotation from what was there before. With it: 4 of 4 CPUs. Without it:
7 of 8 threads on CPU 0.

#### What did not ship: migration

The balancer was built, it worked, and it was removed before this branch
shipped.

It worked in the sense the report asked for. Over a self-test boot on
four CPUs, threads on CPU 0 went from 8 of 14 to **6 of 14** and context
switches from 22197/986/256/294 to **20678/1569/2543/1194** — CPU 2
doing ten times the work it had.

And it destabilised the kernel. Four aarch64 boots of the same tree:

| run | failing tests |
| --- | --- |
| 1 | `smp-wake`, `lockup-soft`, `quiesce-straggler`, `quiesce-grace`, `irq-sync`, `timer-cancel-sync`, `lockdep-contention` |
| 2 | `lockdep-contention` |
| 3 | `iommu` |
| 4 | none |

Three of four, and **seven concurrency tests failing together is not
timing sensitivity** — something is being corrupted. With the balancer
removed, four of four boots pass. That is the whole argument: the same
tree, one variable, 3/4 against 0/4.

**What was ruled out**, so the next attempt starts past it:

- `sched_wake` reads `t->cpu` before taking that CPU's run-queue lock,
  which looks like the race — but a thread it can wake is `THREAD_BLOCKED`
  and therefore not in any ready list, so the balancer cannot have been
  holding it.
- `list_remove` self-links, so `dequeue` followed by `enqueue` leaves the
  node genuinely empty and `rr_enqueue`'s assertion is not being skirted.
- Fault accounting in `iommu_note_fault` is global and lock-protected,
  with no per-CPU state.
- The SMMU event interrupt is bound to one CPU at request time
  (`IRQ_CPU_ANY` resolves round-robin *once*), so it does not follow the
  test thread.

**What was found**, and it is the reason this is worth writing down:

- **lockdep cannot check the two-lock order.** Both run-queue locks are
  the `runqueue` class, so the second acquisition reads as recursion and
  panics until annotated with `spin_lock_nested`; and the annotation says
  only "deliberate". S24 first claimed lockdep would catch a reversed
  order. It cannot. S24 now says so and is marked reserved.
- **`rq->current` can be in a ready list.** The report and two comments
  claimed refusing the running thread was structural, because `schedule`
  dequeues what it runs. A thread woken between blocking and stopping
  stays queued until `sched_set_running_current` removes it, and in that
  window it is both current and a list entry. An assertion caught it on
  the first boot; it was kept only because the property lived in another
  file.
- **The tree has per-CPU assumptions nothing declares.** `el2` asserts
  the hypervisor backend owns EL2 "on this CPU" from an unpinned thread,
  and `g_el2_ready[]` is filled lazily per CPU. That test was correct
  only because threads could not move. It is very unlikely to be the only
  one.

#### What this cost, in method

Four separate times I concluded from a single run: that the balancer
broke `lockup-soft`; that `el2`/`iommu` were environmental; that a
one-run bisect was "decisive"; and that a control had exonerated the
change. A change that alters scheduling timing needs a *rate*, not a
run, and this tree's own notes say so about other people's tests. The
measurement above is four runs per arm and is the only evidence here
worth anything.

### As run

**330 self-tests PASS on x86-64 and aarch64, debug and release.** 329 at
the branch point, so one new: `sched-spread`.

Four consecutive aarch64 boots pass with no failures, against three of
four failing with the balancer in place.

| bug-proof | what fails |
| --- | --- |
| ties keep the lowest-numbered CPU | `sched-spread`: 8 threads used **1 of 4** CPUs, all 8 on CPU 0 |

That proof got sharper after review, and the way it did is worth
keeping. The workers first *polled* — `thread_sleep_ms(2)` in a loop —
so they woke every 2 ms and were often runnable when the next
`thread_create` sampled `nr_running`. A non-zero count is exactly what
makes the old CPU-0-preferring scan spread threads, so the proof
reported "2 of 4 CPUs, 7 on CPU 0" instead of the defect in its pure
form, and **could have passed on the broken code** on a different run.
The workers block on a completion now and the test waits for
`THREAD_BLOCKED` rather than a fixed settle: the defect shows as 8 of 8,
and the healthy case tightened from "at most 3 on any CPU" to "at most
2".

The balancer's own bug-proofs ran and passed before it was removed — the
balancer disabled made `sched-balance-pull` report "not one of the 6
workers left where it was placed", and removing the affinity check
panicked the kernel outright rather than merely moving a pinned thread.
Both are in the history; neither ships, because the code they proved
does not.


