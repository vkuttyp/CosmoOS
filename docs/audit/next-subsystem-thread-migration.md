# NEXT SUBSYSTEM — a thread that can never move, on a CPU chosen once

Date: 2026-09-16. Tree: `main` at f295e42 (after PR #156, the CPU
clock). Chosen from `docs/audit/2026-09-deferred-work-inventory.md`
§2.3.

**Subsystem: thread migration and load balancing — moving a runnable
thread from one CPU to another, and deciding when to.**

This report closes the inventory's §2.3 row that reads "no load
balancing and no migration (a thread stays on the CPU chosen at
creation; confirmed 2026-09-14)".

## Problem

**A thread is assigned a CPU once, by a rule that prefers CPU 0, and
then stays there for the rest of its life.**

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
| `THREAD_BLOCKED` | **no**, and it does not matter | it is not in a runqueue; `sched_wake` will place it, and that is where the creation-time rule applies again |
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
/* How many threads this CPU has pulled, and from where, so a test can
 * assert that a migration happened rather than infer it from placement
 * that might have come from creation. */
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
| `sched-balance-pull` | N unpinned spinning threads created **while CPU 0 is already loaded** — so creation cannot spread them — end up spread, and `sched_test_migrations` shows the moves that did it | `sched_test_set_balancing(false)`: they stay where they were created, and the test reports the imbalance and zero migrations |
| `sched-migration-happens` | a thread's `t->cpu` observably changes while it is alive — the bare fact the clock unit assumed and this kernel did not have | the same disable: `t->cpu` never changes, which is the tree as of f295e42 |
| `sched-affinity-survives-balance` | a thread pinned with `thread_create_on` is never moved off its mask, however lopsided the load | make the balancer skip the affinity check: it moves, and the test names the thread, its mask and the CPU it landed on |
| `sched-running-not-stolen` | the thread currently running on a CPU is never taken by a balancer on another; only `THREAD_READY` moves | let `pick_migratable` return `rq->current`: the victim CPU's switch path finds its current thread on another queue, which the test detects as a state mismatch before it can corrupt anything |
| `sched-balance-hysteresis` | a difference of one runnable thread does not move anything, across many periods | drop the hysteresis to `> 0`: threads ping-pong, and the test counts migrations that should not have happened |
| `smp-parallel`, `smp-pinned` (existing) | unchanged | — |

**Vacuity, named in advance.** `sched-balance-pull` is the one at risk:
if creation already spread the threads, the test proves nothing about
balancing. That is why it loads CPU 0 *first* and creates the workers
from there — so creation-time placement cannot produce the spread, and
only a migration can. The test asserts the migration counter moved, not
just the final placement, for the same reason.

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

Not yet built: this report is the plan. The implementation pull request
fills this section.

### As run

Not yet run: this report is the plan. The implementation pull request
fills this section.
