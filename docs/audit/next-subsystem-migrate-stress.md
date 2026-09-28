# NEXT SUBSYSTEM — sched-migrate-stress asserts a latency the scheduler does not promise

> Constitution §68 report. Takes up `sched-migrate-stress`'s second CI
> sighting (run 36387945129, 2026-09-28, the GIC boot of a branch that
> changed vGIC tests). The first was 2026-09-24. Both are aarch64, and
> both read only "a worker made no progress under migration". The probe
> built here names the worker. In every failure it was a thread that had
> not yet run when the 200 ms window closed, not one that was lost. The
> report finds two mechanisms, rejects a scheduler change that would have
> hidden one of them, and proposes a test that asserts what migration
> actually promises: no thread is stranded.

## Problem

```
SELFTEST: sched-migrate-stress ... FAIL: a worker made no progress under migration (268 ms)
```

The test (`kernel/scheduler/smptest.c`) starts 26 workers and a migrator:
- 8 spinners that yield each round;
- 8 sleepers (1 ms);
- 4 ping-pong pairs on completions (8 threads);
- 2 mutex contenders;
- a migrator calling `sched_migrate_from` between random CPUs in a tight
  loop.

It sleeps 200 ms, sets `stop`, joins everything, and requires every worker
to have finished at least one round. It also requires no worker to have
run outside its affinity, and at least 100 migrations. The failure says a
worker had zero rounds, and nothing else.

### Measured

**CI.** Two sightings, both aarch64, neither on a scheduler change:
- run 36002630250, 2026-09-24, the harness-retry boot;
- run 36387945129, 2026-09-28, the GIC boot.

**The probe.** `tools/migrate-stress-probe.py` instruments the test and
registers it 40 times per boot:
- each worker records when its thread first entered and when it finished
  its first round;
- at the window's close the probe records every thread's CPU and state,
  and each CPU's load;
- the migrator attributes each move to the worker it moved, and counts the
  moves made before that worker's first round;
- each repetition logs the minimum rounds per kind and the migration
  count, and every zero-round worker is logged by name.

Every boot below was confirmed to have booted.

| set | repetitions | failures |
| --- | --- | --- |
| aarch64, quiet | 40 | 0 |
| x86-64, quiet | 40 | 0 |
| aarch64, loaded (three or four boots side by side) | 440 | **7** (1.6 %) |

**Every zero-round worker had simply not run yet.** Each was READY on a
queue when the window closed, and each first entered its function after
the window had closed:

```
MSPROBE zero: spin 4    entered 308174 us  ...  cpu 2 state 0 (READY)
MSPROBE zero: mutex 0   entered 322674 us  ...
MSPROBE zero: pingpong 7 entered 263294 us ... load 8; migrated 36 times, 36 before its first round
MSPROBE zero: mutex 1   entered 263202 us  ... load 8; migrated 42 times, 42 before its first round
MSPROBE zero: mutex 0   entered 243054 us  ... load 8; migrated 250 times, 250 before its first round
```

None was lost: each ran, just after the test had stopped looking.

**The margin is thin everywhere.** The minimum rounds for a spinner or
sleeper in a repetition is 1 or 2, quiet or loaded, on both architectures.
The window measures scheduling latency, and the host's load decides it.

### Two mechanisms

1. **A thread just moved is the next one moved.** `rr_pick_migratable`
   takes the thread nearest the tail, the one that has waited least (it
   walks the ready list in reverse). `migrate_locked` enqueues the moved
   thread at the destination's tail (`g_policy->enqueue(rqt, t, false)`).
   So the thread just moved is its new queue's first candidate, and a
   migrator that returns to that CPU moves it again. That is how a
   waiting worker was moved 250 times before its first round and never
   reached the head of any queue.
2. **Queues pile up.** The migrator piles threads onto a CPU. The maximum
   load at the window's close ranged from 6 to 17, and at 10 ms slices a
   queue of 13 is 130 ms before its tail runs, before any host stall.

### The scheduler change it is tempting to make, measured

*A thread is migrated at most once between two runs.* A flag is set by
`migrate_locked`, cleared at switch-in and skipped by
`pick_migratable`. It would remove mechanism 1. The probe applies it
(`--once`) beside the instrumentation, and twelve loaded aarch64 boots
(480 repetitions) gave:

| | without `--once` | with `--once` |
| --- | --- | --- |
| migrations per 200 ms | 798 – 20 536 | **2 – 130** (median 64) |
| "fewer than 100 migrations" | 0 | **445 of 480** |
| a worker with zero rounds | 7 of 440 | 3 of 480, each moved **once** |
| other tests | pass | `sched-migrate-refuses` sees a refusal under the wrong name (the flag changes which refusal a move gets); `smp-ticks`, `quiesce-straggler` and `lockup-sample-irqoff` also failed in those loaded boots, load-sensitive tests not attributed to the flag |

It does not help:
- it starves the migrator rather than the workers, so the test stops
  stressing what it exists to stress;
- workers still went unrun for 200 ms behind queues of 11 to 13 threads
  (mechanism 2);
- it changes which refusal a targeted migration reports.

**Rejected.** The tail re-pick is recorded as a property of this
scheduler, not changed. The real migrators do not drive it the way the
test's does:
- the balancer pulls one thread, only when the busiest CPU is at least
  two ahead;
- the chaos migrator moves at most one thread per CPU every 16 ms, longer
  than a slice.

### Why it matters

- **The test's claim is not the scheduler's.** A 200 ms deadline for a
  random migrator's victims is a latency bound, and the host's load sets
  it: the "N things after a fixed settle" shape again.
- **Two sightings, each a diagnosis.** The failure message named no
  worker, so each was re-run rather than read.
- **The claim worth keeping is a real one.** Migration must never strand
  a thread: dequeued and not enqueued, or enqueued where nothing runs it.
  That is what the test exists to catch, and a latency miss hides it
  behind the same message.

## Current implementation

- The test (above): one 200 ms sleep, then `stop`, then the progress,
  affinity and migration-count checks. The failure names no worker.
- `migrate_locked` (`kernel/scheduler/sched.c`) enqueues at the tail;
  `rr_pick_migratable` (`kernel/scheduler/policy_rr.c`) scans from the
  tail.
- S26 (`docs/kernel/scheduler/invariants.md`) states what may be migrated,
  and nothing about how often.

## Design

### 1. Assert that migration strands nothing

The test gets three phases:

1. **Stress, 200 ms**, as now. The migration count (at least 100) is
   taken over this window alone.
2. **Stop the migrator and join it.** The migrator gets a stop flag of its
   own (`mig_stop`); today it shares `stop` with every worker, and setting
   that would stop the workers too, so their rounds could not advance in
   phase 3. With nothing re-queueing threads, every READY thread reaches
   the head of its queue in time proportional to the queue ahead of it.
3. **Every worker runs again.** A snapshot of each worker's rounds is
   taken as the migrator stops. The test then waits for each worker's
   rounds to pass its snapshot, counting its waits (`thread_sleep_ms(1)`,
   bounded at 5 000 as a hang guard, not a latency claim). A thread a
   migration stranded never runs again, and the guard names it. Then the
   workers' `stop`, every ping-pong completion completed (as the test does
   today: a half blocked on its partner cannot see `stop`), and join.

The affinity check stays as it is.

### 2. A late starter, every boot

One more worker, a spinner whose entry sleeps 300 ms before its first
round, so it has no rounds when the 200 ms window closes. Phase 3 must
accept it. Reintroducing the window's assertion then fails on every boot,
not once a week on CI (the irq-order unit's lesson: make the other case
certain).

### 3. The failure names the worker, and a stranded one is isolated

The probe's instrument becomes permanent. A worker that fails phase 3 is
reported by kind, index, when it entered, its first round, and its thread's
CPU and state. The test does not join a stranded thread, which would hang
the boot; it reports it and fails.

**On a failure, only the stranded are left unjoined.** A worker can fail
phase 3 without being stranded. The partner of a stranded ping-pong half
is blocked in `wait_for_completion` on a signal that never comes, and it
cannot see a stop flag there. So the failure path does what the success
path does before joining anything: it sets every stop flag and completes
every ping-pong completion, which releases any half still waiting. It then
waits, with the same count-based guard, for each worker's own `exited`
flag, which the worker sets as its last action. Every worker that sets it
is joined; one that does not is the stranded one. It is reported, and it
is the only one left unjoined. The join never blocks on a thread that has
not already said it is leaving.

A stranded thread may yet run, and a failed self-test does not stop the
tests after it. So the run's shared state, its workers and their
completions and mutex are allocated per run (`kzalloc`), not static as
now. After a clean join they are freed. On a failure with a worker
unjoined, the test sets every stop flag and leaves the block allocated:
the stranded thread, if it ever runs, finds its own storage intact and
its stop set, and exits. A later run of the test gets fresh storage and
never resets the memory that thread still holds.

### 4. The scheduler's property, written down

The scheduler documents gain, under S26: a migrated thread is enqueued at
the destination's tail, and `pick_migratable` takes the tail first. So a
migrator that returns to a CPU re-moves the thread it just moved, and
repeated migration can delay a waiting thread without bound. The balancer
(a pull only when two ahead) and chaos (one move per CPU per 16 ms) do not
return that fast. The measured rejection of "at most once between runs" is
recorded with it.

### 5. The §70 gate

**Correctness.** The test asserts that no thread is stranded, which a
latency miss can no longer hide.

**Concurrency.** Unchanged in the kernel. The test's phases are ordered
by joins and counts, not by time.

**Ownership and lifetime.** A worker reported stranded is not joined,
and its run's storage (allocated per run) is left allocated for it, so no
later run can reset memory a live thread holds. A clean run frees it.

**Security.** None.

**Failure.** Named, as above.

**Performance.** Phase 3 usually costs a few ms, and 300 ms for the late
starter.

## Affected files

| file | change |
| --- | --- |
| kernel/scheduler/smptest.c | the three phases, the late starter, the named failure |
| docs/kernel/scheduler/invariants.md, testing.md | the tail re-pick under S26; the test's description |
| docs/testing/flakes.md | both sightings, and the measurement |
| README Status | the unit |

## APIs

None.

## Migration plan

One PR.

## Tests

| test | checks | mutation it must catch |
| --- | --- | --- |
| `sched-migrate-stress` | at least 100 migrations in the window; no worker outside its mask; every worker, the late starter included, runs again after the migrator stops | `migrate_locked` dropping the thread (dequeue without enqueue): phase 3 names it and fails, without hanging the boot; the 200 ms progress assertion restored: the late starter fails it on every boot |

## Benchmarks

None.

## Risks

- **A stranded thread is not joined.** It is reported and left with its
  storage and its stop flag set, and the boot fails on it. Joining it would
  hang the boot instead of naming it. The storage it keeps is leaked,
  once, on a boot that has already failed.
- **The hang guard is time.** 5 000 waits of 1 ms is 5 s of the
  test's own sleeps, far above any queue's drain. It says "stuck", not
  "slow".

## Alternatives considered

- **Widen the window.** That moves the cliff; host load still decides
  it, and the lost-thread case stays behind the same message.
- **A thread migrated at most once between runs.** Measured above: it
  throttles the migrator a hundredfold, still leaves workers unrun behind
  piled queues, and breaks named refusals.
- **Enqueue a migrated thread at the head.** It jumps the destination's
  waiters, and it would be the migrator's next pick by position only if
  the scan changed too. That is a fairness decision for a scheduler unit,
  not a fix for a test.
