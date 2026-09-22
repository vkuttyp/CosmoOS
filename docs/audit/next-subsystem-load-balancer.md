# NEXT SUBSYSTEM — a balancer that pulls, and a load that can see the thread already running

Constitution §68 report. The scheduler's open row in
`docs/audit/2026-09-deferred-work-inventory.md` §2.3 reads, since the
previous unit closed the rest of it:

> **No balancer moves threads on its own yet**: that is the next unit, on
> a tree that has already survived migration.

This is that unit. It is a policy on a mechanism that already exists,
and it is deliberately small: a load that counts the thread a CPU is
running, two moments at which a CPU looks for work to take, a rule for
when the difference is worth a move, and the diagnostics to see whether
it was.

## What is established (before this unit)

- **A migration primitive.** `sched_migrate(t, cpu)` and
  `sched_migrate_from(from, to, &moved)` move one READY thread between
  run queues under both locks, taken in increasing CPU-id order (S24).
  Each run queue's lock is its own lockdep class, so the order is
  checked rather than asserted. `enum sched_migrate_result` names the
  check that refused. Neither entry may be called with a run-queue lock
  held; `sched_migrate_from` selects *under* both locks through the
  policy's `pick_migratable`, so a caller that knows only the queue has
  no stale selection to revalidate
  (`docs/audit/next-subsystem-percpu-migration.md`).
- **A rule for what may move**, and the flag that enforces it: READY,
  not its queue's `current`, not `THREAD_FLAG_PREEMPTED`, affinity
  admitting the destination (S26).
- **A rule for per-CPU data** (S25) and debug accessors that enforce it,
  so a new caller that reads "my CPU" while preemptible is a panic with
  the site named, not a corruption three subsystems away.
- **An adversary.** `make test-chaos` boots the whole suite with a
  migrator in the tick that moves a thread off every CPU every fourth
  tick for no reason at all. CI runs it on both architectures. The
  balancer proposed here moves strictly less than that, from the same
  context, through the same primitive.
- **Placement that rotates its ties.** `pick_cpu` scans for the least
  loaded CPU the thread's affinity allows and rotates equal answers, so
  threads created on an idle machine no longer all land on CPU 0
  (`docs/audit/next-subsystem-thread-migration.md`).

What does not exist is anything that moves a thread because moving it
would be better.

## The problem

### Placement is a guess made before the thread has done any work

`sched_enqueue_new` calls `pick_cpu` once, at creation. The thread is
then on that CPU for the rest of its life: `sched_wake` enqueues a woken
thread on `t->cpu`, the CPU it was created on, whatever has happened
since and whatever else is there now.

The guess is made at the one moment when the least is known. A kernel
thread is created, blocks, and does its work minutes later in response
to something that has not happened yet. `pick_cpu` cannot know which of
the threads it is placing will turn out to be the busy one, and nothing
looks again.

### And the load it reads cannot see the thread that is running

`rq->nr_running` is documented as "threads queued (not current)", and
that is exactly what it counts: `schedule` dequeues the thread it runs.
So a CPU spinning flat out on one CPU-bound thread reports
`nr_running == 0`, and so does a CPU that is asleep in its idle loop.
They are indistinguishable to every reader of that field, `pick_cpu`
included.

This is the second half of the same defect. The first half places a
thread before its load is known; the second half means that even a
reader who looked again would be looking at a number that cannot tell a
busy CPU from an empty one.

### A found gap: affinity cannot name a CPU the thread is not on

`thread_set_affinity_self` and `thread_set_affinity` assert that the new
mask admits the CPU the thread is currently on
(`set_affinity_locked`, `kernel/scheduler/thread.c`). So there is no way
to say "run on CPU 2 from now on" to a thread that is on CPU 1. The
order cannot be reversed either: `sched_migrate` moves a READY thread to
a chosen CPU, but the affinity that would *keep* it there cannot be set
until it is already there, and a thread that is running is not READY.
The only way to start a thread on a chosen CPU is to create it there
with `thread_create_on`. The measurement below hit this — its pinned
control had to create its workers pinned rather than pin them after the
fact — and it is the same absence seen from another side: nothing moves
a thread because something asked for it to be elsewhere. This unit does
not close it (below), but it is the first caller that could.

### Nothing ever revisits either decision

There is no balancer, no re-pick at wake-up, and no evacuation. A
misplacement is permanent. On a machine with four CPUs and four
runnable threads, the difference between the best and the worst
arrangement is the whole machine, and which one the kernel gets is
decided by the order the threads happened to be created in.

### Measured

`tools/sched-balance-probe.py` applies two measurements to a checkout
and removes them again. Both were run on this machine (QEMU TCG, four
CPUs, debug builds) on `e2f3d2b6`.

**The bench.** It creates twice as many threads as there are CPUs, one
at a time, each blocking before the next is created — the shape
`sched-spread` uses, which spreads them round-robin, one per CPU, two
passes. It then releases **every other one**, so the threads that are
actually runnable are the ones creation order happened to put on every
other CPU, and they spin counting iterations for 500 ms. The control
releases the same number of threads pinned one per CPU: same count, same
work, same 500 ms, spread by construction.

| round | x86-64 placement | x86-64 iterations | AArch64 placement | AArch64 iterations |
| --- | --- | --- | --- | --- |
| four runners, as placed | 1/1/1/1 | 6,759,353 | 1/1/1/1 | 8,732,514 |
| four runners, pinned one per CPU | 1/1/1/1 | 6,941,566 | 1/1/1/1 | 8,951,647 |
| **eight created, four run, as placed** | **0/2/0/2** | **3,699,171** | **2/0/2/0** | **4,756,452** |
| eight created, four run, pinned one per CPU | 1/1/1/1 | 6,594,128 | 1/1/1/1 | 8,947,749 |

Four runnable threads, four idle CPUs, and **two of the four CPUs never
run any of them**. The machine does 56% of the work it could on x86-64
and 53% on AArch64 — a 44% and a 47% loss — and it stays that way for
the whole run, because nothing moves.

The simple case is fine, and that matters: when every created thread
runs, the rotation spreads them and the two rows agree within 3%. The
defect is not that placement is bad. It is that placement is a decision
about which threads exist, and the question is which threads *run*.

**What random movement already recovers.** The same bench under the
chaos migrator, which moves one thread off every CPU every fourth tick
with no policy whatsoever:

| round (x86-64, same boot) | iterations | of that boot's ideal |
| --- | --- | --- |
| eight created, four run, as placed | 5,214,600 | 88% |
| eight created, four run, pinned one per CPU | 5,914,755 | (the ideal) |

Against 56% of ideal with nothing moving. **Random migration recovers
more than half of the loss**, which is the strongest available argument
that a directed one recovers most of the rest: the threads are
interchangeable and the destinations are idle, so almost any movement is
an improvement.

**And what movement costs.** In that same chaos boot the two rounds that
were *already* balanced lost 8% and 14% against the plain boot. The
pinned round's own threads cannot have moved, so most of that is the
migrator's per-tick work and the movement of every *other* thread in the
system. It is an upper bound on the cost of balancing without
hysteresis, it is not small, and it is why the design below has a
difference threshold and a period rather than a rule that fires whenever
two queues are unequal.

**The standing opportunity, on the suite itself.** The probe counts, per
CPU per tick, ticks where this CPU was idle with an empty queue while
another CPU had a thread queued behind the one it was running:

| | cpu 0 | cpu 1 | cpu 2 | cpu 3 |
| --- | --- | --- | --- | --- |
| x86-64, share of ticks | 3.3% | 4.5% | 8.2% | 4.8% |
| AArch64, share of ticks | 6.1% | 3.6% | 5.7% | 7.4% |

A few percent, not a few tenths — and this is the self-test suite, which
is mostly one thread deep by construction. The suite is not the
workload; it is the floor.

## Current implementation

`kernel/scheduler/sched.c`:

- `pick_cpu(t)` — least `nr_running` among online CPUs the affinity
  allows, ties rotating on a global counter. Called once, from
  `sched_enqueue_new`.
- `sched_wake(t)` — locks `g_rqs[t->cpu]` and enqueues there. The
  unlocked read of `t->cpu` is safe because a BLOCKED thread is on no
  queue and no migrator moves one.
- `sched_tick` — per-CPU, takes its own run-queue lock for slice
  accounting, releases it, and then (under `CONFIG_SCHED_CHAOS` only)
  calls `chaos_tick`, which is where a balancer would go.
- `sched_migrate_from(from, to, &moved)` — everything a pull needs,
  already written, already tested, already run in CI under an adversary.

`kernel/scheduler/policy_rr.c`:

- `rr_pick_migratable(rq, allowed)` — the lowest-priority thread at the
  tail of the queue it would run last, skipping `current`, preempted
  threads, and affinities that do not admit the destination.

Nothing else in the tree reads or writes a CPU's load.

## Why it matters

1. **It is the reason the previous two units were built.** The
   placement unit rotated `pick_cpu`'s ties; the migration unit made a
   move safe and gave the tree a checked lock order and a declared
   per-CPU rule. Neither moves a single thread in a shipped build. This
   unit is what they were for.
2. **The loss is not marginal.** 44% and 47% of a four-CPU machine, on
   a workload with no I/O, no locks and no sharing — the easiest
   possible case for a balancer and the one where the answer is
   unambiguous.
3. **It gets worse with more CPUs, not better.** The bench's pathology
   is "the runnable threads are a subset of the created ones". The
   chance that a subset chosen by creation order is spread evenly falls
   as the machine grows.
4. **Everything above the scheduler inherits it.** A guest's vCPU
   threads, the network stack's per-CPU receive workers, a build running
   under the Linux personality: each is a set of threads created at one
   time and made busy at another, which is precisely the shape measured.
5. **The adversary is already in CI.** The expensive part of this work —
   proving the tree survives threads moving underneath it — is done and
   running on both architectures. The policy is the cheap part, and
   leaving it undone wastes the expensive part.

## Design

### 1. Load: runnable threads, the running one included

```c
/* Runnable threads on `cpu`: those queued, plus the one running if it
 * is not the idle thread. A hint -- read without the queue's lock. */
unsigned sched_cpu_load(unsigned cpu);
```

`nr_running + (rq->current != NULL && rq->current != rq->idle ? 1 : 0)`.

That is the whole definition, and it makes the two states that matter
distinguishable: an idle CPU is 0, a CPU running one thread with nothing
queued is 1, a CPU with a thread running and another waiting is 2.

**It is read without the lock, and it is a hint.** The scan compares
pointers by identity and never dereferences `rq->current`, which belongs
to another CPU and may be a thread that exits a moment later. A stale
answer sends the balancer to the wrong queue; it cannot make it do
anything wrong when it gets there, because `sched_migrate_from` re-reads
everything under both locks and selects through the policy. Asking
which, never whether.

`pick_cpu` moves to the same definition. A CPU busy with a CPU-bound
thread has stopped being the least loaded, which is what placement was
trying to ask all along.

### 2. Two moments: the idle pull and the periodic pull

Balancing is a **pull**: the CPU that takes the work is the CPU that
decides, because it is the one with time to spare and the one whose
queue the thread arrives on.

- **The idle pull.** On every tick where this CPU's queue is empty and
  its current is the idle thread, it looks. An idle CPU has nothing to
  lose, the scan is a handful of loads, and this is the moment the
  measured defect is visible: the bench's two spare CPUs were idle for
  500 ms with the answer one cache line away.
- **The periodic pull.** Every `SCHED_BALANCE_TICKS` (16 ticks, 64 ms at
  `CONFIG_HZ` 250), a CPU that is *not* idle looks as well, so an
  imbalance between two busy CPUs is corrected even when no CPU is free.

Both run from `sched_tick`, after it has released its own run-queue
lock — exactly where `chaos_tick` runs today, which is a context the
primitive has been exercised in on both architectures since the previous
unit. The balancer replaces chaos's rotation with a choice; chaos stays,
because an adversary that only makes sensible moves is not an adversary.

### 3. The difference that is worth a move

> **Pull only when the busiest CPU's load is at least two more than
> this CPU's.**

A difference of one is the steady state of an odd number of threads, and
chasing it moves a thread back and forth forever. Two is also exactly
the threshold the measurement asks for: in the bench, the spare CPUs
were at 0 and the loaded ones at 2 — one thread running, one waiting.
And it refuses the move that gains nothing: a CPU running a single
thread with an empty queue is at 1, so its thread is never dragged onto
an idle CPU, where it would arrive with a cold cache to do exactly the
work it was already doing.

One thread per pull. If the imbalance survives, the next period moves
another; if it does not, nothing more moves.

### 4. What may move is already decided

The pull calls `sched_migrate_from(busiest, self, &moved)` and takes
what it is given. Every rule about what may move — READY, not current,
not preempted, affinity — is the primitive's, already written and
already tested. The balancer adds no new answer to that question, which
is the point of having built the primitive first.

A refusal is not an error. `sched_migrate_from` returning
`SCHED_MIGRATE_NOT_READY` because the busiest queue's only spare thread
was preempted means this period moves nothing, and the counter for that
reason goes up.

### 5. Diagnostics, because the cost is real

The measurement above shows movement is not free, so the unit ships the
means to see it:

- `sched_balance_stats()` — scans, pulls, and a count per refusal
  reason.
- The per-CPU line in `sched_dump` gains the CPU's load, so a lockup
  report and the panic dump say what the scheduler thought the machine
  looked like.
- The boot prints one tally line after the self-tests, as the chaos
  migrator does.

### 6. What it does not do

- **No wake-time re-pick.** Placing a woken thread somewhere other than
  `t->cpu` touches the hottest path in the scheduler and would need its
  own measurement of what it costs there. The idle pull reaches the same
  threads within one tick of a CPU going idle, which the benchmark
  below will show is enough for the measured case. It is the natural
  next unit if the numbers ask for it.
- **No affinity that moves a thread.** The gap above stays open: an
  affinity change still has to admit the CPU the thread is on. Honouring
  a narrowing one means moving a *running* thread, which needs the
  cross-CPU handshake this unit also does not build, so the two belong
  together in a later unit and the assertion stays until then.
- **No push.** A loaded CPU does not hand work away. The CPU with spare
  time is the right one to spend time deciding.
- **No migration of a running thread**, which needs a cross-CPU "switch
  away now" handshake, and no evacuation on CPU offline. Both become
  possible once this exists; neither is needed to balance a queue.
- **No NUMA and no cache topology.** There is one node and no SRAT
  parsing (inventory §2.2), so "the most loaded CPU" is the only signal
  the machine offers. The design leaves room for a distance term without
  inviting one now.
- **No change of policy.** `policy_rr.c` stays the only policy;
  fairness, deadline and real-time scheduling are separate rows of §2.3.
- **No per-thread weight or utilisation tracking.** Load is a count of
  runnable threads. A tree with one policy and no priorities-by-weight
  has nothing to weigh with, and inventing a number no measurement asks
  for is how a balancer becomes untunable.

## Affected files

| file | change |
| --- | --- |
| `kernel/scheduler/sched.c` | `sched_cpu_load`, `balance_tick` and its two triggers, the pull's stats, `pick_cpu` on the new load, the `sched_dump` line |
| `kernel/include/kernel/sched.h` | `sched_cpu_load`, `sched_balance_stats`, `struct sched_balance_stats`, `SCHED_BALANCE_TICKS` |
| `kernel/scheduler/policy_rr.c` | unchanged if `rr_pick_migratable`'s choice holds up under a directed caller; the report expects it does |
| `kernel/scheduler/smptest.c` | the new tests, and the bench promoted from the probe |
| `kernel/core/selftest.c`, `kernel/include/kernel/selftest.h` | registration, and the balance tally line |
| `build/config.mk` | `CONFIG_SCHED_BALANCE` (default on), so a bug-proof can turn the balancer off without deleting it |
| `docs/kernel/scheduler/design.md` | §3b, "Balancing", after the migration section |
| `docs/kernel/scheduler/invariants.md` | S27, S28, S29 |
| `docs/kernel/scheduler/api.md` | the two new calls |
| `docs/kernel/scheduler/testing.md` | the new tests and what each proves |
| `docs/audit/2026-09-deferred-work-inventory.md` | §2.3's balancer sentence struck through |
| `README.md` | Status entry |
| `tools/sched-balance-probe.py` | shipped with this report; the bench moves into the suite when the unit is built |

## New APIs

```c
/* Runnable threads on `cpu`: queued, plus the running one when it is
 * not the idle thread. Read without that queue's lock -- a hint for a
 * balancer's scan, re-decided under both locks by sched_migrate_from.
 * Compares rq->current by identity and never dereferences it. */
unsigned sched_cpu_load(unsigned cpu);

struct sched_balance_stats {
    uint64_t scans;        /* times a CPU looked */
    uint64_t pulls;        /* threads taken */
    uint64_t no_candidate; /* looked, nothing was far enough ahead */
    uint64_t refused[SCHED_MIGRATE_RESULT_COUNT];  /* by the primitive's reason */
};
void sched_balance_stats(struct sched_balance_stats *out);
```

`SCHED_MIGRATE_RESULT_COUNT` is new, and goes next to
`enum sched_migrate_result` so that the array and the enum cannot drift
apart.

```c
/* Ticks between periodic pulls by a CPU that is not idle. An idle CPU
 * looks every tick. */
#define SCHED_BALANCE_TICKS 16u
```

## Migration plan

1. **`sched_cpu_load` and its use in `pick_cpu`**, alone, with the test
   that a busy CPU is no longer indistinguishable from an idle one. This
   changes placement and nothing else; boot both architectures and keep
   the bench numbers, which should not move (the bench's threads are all
   blocked when the later ones are placed, so the new load reads the
   same 0 everywhere).
2. **`balance_tick` behind `CONFIG_SCHED_BALANCE`, idle trigger only.**
   Boot, and read the bench: this is the step the 44% is expected to
   come back at.
3. **The periodic trigger**, with the difference threshold. Boot, and
   read the balanced rounds: they must not regress.
4. **Stats, the dump line and the tally.**
5. **The tests, each with its bug-proof**, and the bench promoted into
   the suite with the thresholds the numbers support.
6. **The adversary:** `make test-chaos` with the balancer *also* on, on
   both architectures. A balancer and a chaos migrator competing is the
   closest thing to a hostile scheduler this tree can build, and it is
   the same suite either way.
7. **Release builds both architectures, `gmake host-test`, every
   mutation alone.**
8. **Docs, inventory strike-through, README Status entry, and the
   report's as-built banner.**

## Tests

| test | what it proves | bug-proof: what fails without the code |
| --- | --- | --- |
| `sched-load` | a CPU running a thread reports load 1, an idle CPU 0, and placement prefers the idle one | define load as `nr_running` again: the two CPUs read equal and the test's thread lands on the busy one |
| `sched-balance-pull` | the bench's shape: eight created, every other one released, all four CPUs running one within a bounded time | `CONFIG_SCHED_BALANCE=0`: two of four CPUs, the measured 56% |
| `sched-balance-hysteresis` | one runnable thread on each of two CPUs and one idle: nothing moves, over many periods | threshold of one instead of two: the migration count climbs every period |
| `sched-balance-affinity` | a pinned thread on the busiest CPU is never pulled | the primitive's own affinity check, already proved by `sched-migrate-refuses`; this asserts the balancer does not reach around it |
| `sched-balance-idle-only` | a CPU that is *not* idle does not pull more often than the period | drop the idle/periodic distinction: the scan count rises by the tick count |
| `sched-balance-no-thrash` | a balanced workload makes no pulls at all | as for hysteresis |

Every one of them is a claim about two CPUs and is written with two
CPUs, with the machine's own counters rather than a settle-and-count
(`docs/testing/flakes.md`). Each waits for the state it needs rather
than sleeping a fixed time, and each is skipped with a printed line on a
one-CPU machine.

The whole suite also runs under `make test-chaos`, where the balancer's
decisions and the chaos migrator's random ones are both in flight.

## Benchmarks

The probe's bench becomes `bench-balance`, reported on every boot and
recorded in the report's banner as run:

| claim | before (x86-64) | target |
| --- | --- | --- |
| eight created, four run, as placed | 3,699,171 (56% of ideal) | ≥ 85% of the pinned control |
| four runners, as placed | 6,759,353 | within 5% of before: balancing an already-balanced machine costs nothing measurable |
| pulls during the balanced round | n/a | 0 |

The same four rows on AArch64, where the loss measured 47%.

The 85% target is set from what the *random* migrator already achieved
(88% of its own boot's ideal) minus the period's latency, and it is
deliberately not 100%: the threads spend the first tick or two of the
run where creation order put them, and a balancer that reached 100%
would be one that moved before it had anything to go on.

## Risks

- **A balancer destabilised this kernel once.** Three of four AArch64
  boots failed, once with seven concurrency tests at once, and the cause
  was not found at the time. It has been found since: `schedule_internal`
  read its own per-CPU block before taking the run-queue lock, so a
  thread moved in that window switched on the state of the CPU it had
  left. That, and sixteen other undeclared per-CPU claims, were fixed in
  the previous unit, and the debug accessors now panic on a new one.
  The evidence that this is different is not an argument: it is
  `make test-chaos` passing on both architectures in CI while moving
  threads far more aggressively than this balancer will.
- **Thrash.** The measured cost of unhysteresised movement is 8–14% on
  an already-balanced machine. Mitigation: the difference threshold, the
  period, one thread per pull, and a benchmark row whose target is zero
  pulls on a balanced workload — a number that fails loudly rather than
  degrading quietly.
- **The scan is racy by construction.** It reads other CPUs' queues
  without their locks. Mitigation: it is a hint, it never dereferences
  `rq->current`, and the move re-decides everything under both locks.
  The failure mode of a stale read is a wasted scan.
- **The idle pull runs every tick.** On four CPUs that is four loads and
  a compare, 250 times a second, on a CPU that is otherwise halted.
  Mitigation: the balanced-round benchmark is exactly the measurement
  that would catch it costing more than that.
- **An idle CPU pulling can wake a CPU that was about to halt** and
  start a wake/idle cycle between two nearly-idle CPUs. The threshold of
  two is what prevents it: a CPU with one thread and nothing queued is
  never a source.
- **`pick_cpu` changing under the same unit as the balancer** means two
  variables. Mitigation: step 1 of the migration plan ships and is
  measured alone, which is the lesson the previous balancer's
  removal wrote down.

## Alternatives considered

- **Wake-time placement instead of a periodic pull.** It would fix the
  measured case instantly rather than within a tick, and it is how most
  of Linux's balance actually happens. It also puts a scan of every CPU
  on the path of every wake-up, which is this scheduler's hottest path
  and one with no measurement behind it yet. The idle pull gets most of
  the benefit at a cost that is paid only by CPUs with nothing to do.
  Deferred, named, and the benchmark here is what would justify it.
- **Push instead of pull.** The overloaded CPU is by definition the one
  with no spare time, and a push writes into a queue whose owner is
  running. Pull puts the scan on the idle CPU and the enqueue on the
  queue that is about to be consumed.
- **A single global run queue**, which cannot be unbalanced. It also
  cannot scale: one lock on the switch path of every CPU, against a tree
  that has just given each run queue its own lockdep class precisely to
  keep them separate.
- **Per-thread utilisation tracking** (a decaying average of runnable
  time, as `PELT` does). It is the right answer for a scheduler with
  weights and a fairness target. This one has round-robin and a priority
  band, so the number would feed nothing, and it would have to be
  maintained on every enqueue and dequeue to be read a few times a
  second.
- **Teach the chaos migrator a policy** rather than writing a balancer.
  The chaos migrator's value is that it is *not* a policy: it is the
  adversary that proves the tree survives movement it did not ask for.
  Making it sensible would delete the only test of that.
- **A load that counts blocked threads too**, so placement could see
  which CPU has the most threads attached. It would have placed the
  bench's eight evenly and changed nothing about the four that ran: the
  question is which threads are runnable, and a blocked thread is not.
