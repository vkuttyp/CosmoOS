# NEXT SUBSYSTEM — three scheduler tests assume a spinner is running when it has only been placed

> **Status: proposed.** Report and probe (`tools/placed-running-probe.py`)
> only; nothing in the kernel changes in this PR.

## Problem

Three tests in `kernel/scheduler/smptest.c` start a spinner pinned to one
CPU (`mig_spinner_main`) and then rely on it **running** there. None of
them waits for that:

| test | what it waits for | what it then assumes |
|---|---|---|
| `sched-load` | `sched_cpu_load(busy) != 0` | the spinner is the running thread ("not merely placed", says its own comment) |
| `sched-migrate` | the worker `READY` on the spinner's CPU (`wait_ready_on`) | the spinner holds the CPU, so the worker stays queued until it is moved |
| `sched-migrate-refuses` | the same | the spinner is refused as running (`SCHED_MIGRATE_NOT_READY`) |

A spinner that has been created is on its CPU's queue: `READY`, counted in
`sched_cpu_load`, and a worker created after it is `READY` too. Every
one of those waits is satisfied **before the spinner has run at all**.

Two of the three have failed that way, twice each (`docs/testing/flakes.md`):

```
SELFTEST: sched-load       ... FAIL: a CPU running a compute-bound thread reported no load (0 ms)
[ERROR] selftest: sched-migrate-refuses: the running spinner: affinity
```

- **`sched-migrate-refuses`:** a spinner still queued is a pinned thread
  that is `READY`, and `AFFINITY` is the right answer for that.
- **`sched-load`:** the wait returns on the queued spinner's load of 1.
  The next read lands in the idle CPU's switch to it, and returns 0.
  `schedule_internal` dequeues the next thread (`nr_running--`,
  `sched.c:367`) before it sets `rq->current = next` (`:390`), and the
  idle thread is never queued. Between the two, an unlocked reader sees
  nothing queued and the idle thread current: a load of 0. That is
  consistent with the 0 ms duration: the loop exited on its first read.
  `sched_cpu_load` is documented as a hint read without the lock, so the
  kernel is not wrong here. The test is wrong to treat a hint as exact
  at the one instant it is least settled.

### Measured

`tools/placed-running-probe.py` gives the spinner an `entered` flag it
sets as its first act, and logs, at each test's check, whether the
spinner had entered and its state (`READY` 0, `RUNNING` 1). Each test is
registered 100 times per boot. Debug builds, one boot per architecture,
each confirmed.

**Not yet running at the check** (of 100):

| test | aarch64 | x86-64 |
|---|---|---|
| `sched-load` | **98** (96 still queued, 2 switched in but not yet at their first instruction) | **99** (all still queued) |
| `sched-migrate` | 1 (queued) | **14** (11 queued, 3 switched in) |
| `sched-migrate-refuses` | 1 (switched in) | **7** (4 queued, 3 switched in) |

`sched-load` almost never checks a running spinner. It passes because a
queued spinner already counts as load, and fails only when a read lands
in the switch. The two migrate tests check before the spinner has run
1–14% of the time.

**The candidate** (`--fix`): each test waits for the spinner's own
`entered` flag before its check. **All 600 checks** (100 per test per
architecture) found the spinner running (`entered 1`, `RUNNING`), and
both boots passed.

### What remains after the wait

After the spinner has entered, a thread of higher priority can still
preempt it on its CPU. The spinner runs at `SCHED_PRIO_DEFAULT - 1`
(31), and two kernel service threads run above it: the reaper (24) and
`quiesce` (28). Since P34 (the exit-space unit) the reaper does every
process's teardown, so it runs more than it did. The three tests differ
in what a preempted spinner does to them:

- **`sched-load`**: still passes. The preempting thread is current, so
  the load is at least 1.
- **`sched-migrate`**: still passes. The worker stays queued and is moved.
- **`sched-migrate-refuses`**: fails. The spinner is queued, and
  `AFFINITY` is correct.

Not seen in 600 checks, but possible, and only in the last test.

### Also seen: one DNS timeout took eight tests with it

In the first aarch64 probe boot, `net-dns` failed a timing check
(`s1.dns_pending == 0 && s1.dns_expired > s0.dns_expired`, line 4817;
its first sighting). Then seven later network tests failed to open
`/dev/net/tap`, one failed on `svc != NULL`, and `process-user` failed
(`status == 0`). The shape is P33's before it blamed once: one test's
early return leaves a shared resource, here a tap from the pool of
eight, and the tests after it fail for it. It is recorded here and not
taken up: it is a unit of its own (the network tests' cleanup on
failure).

### Why it matters

- Two tests fail on a correct kernel, and the failures look like
  scheduler defects: "reported no load", "refused under the wrong name".
- All three tests pass mostly by accident. `sched-load` checks a queued
  spinner 98–99% of the time. It is testing "a placed thread counts",
  not the "running" its comment says.

## Current implementation

- `mig_spinner_main` spins until `stop`. Nothing records that it ran.
- `sched_load_pinned` waits for a non-zero `sched_cpu_load(busy)`.
  `sched_migrate_pinned` and `sched_migrate_refuses_pinned` wait with
  `wait_ready_on(tw, a)`.
- `sched_cpu_load` is `nr_running` plus one for a non-idle `current`,
  read without the queue's lock (a documented hint).

## Design

### 1. A spinner says it is running

`struct mig_spinner` gains `entered`, set by `mig_spinner_main` as its
first act. A helper, `wait_spinner_entered(&sp)` (2 s bound, 1 ms
sleeps), is called by each of the three tests before it relies on the
spinner:

- `sched-load` waits for `entered` instead of for a non-zero load. Its
  comment then says what it does.
- `sched-migrate` and `sched-migrate-refuses` wait for `entered` before
  they create the worker. The worker is then placed behind a spinner
  that already holds the CPU, which is what "stays READY" relies on.

A spinner that never enters fails the test by name ("the spinner never
ran"), not as a mistaken claim about load or refusal.

### 2. The refused spinner outranks the kernel's threads

`sched-migrate-refuses`' spinner runs at `SCHED_PRIO_DEFAULT - 16`. That
is above the reaper (24) and `quiesce` (28), so nothing but an
interrupt can preempt it for the few milliseconds it holds its CPU, and
"running" stays true from `entered` to the migrate. The worker and the
second spinner (`mig-spin2`, which must preempt the worker) keep their
priorities. `sched-load` and `sched-migrate` are unaffected by a
preemption (above), so their spinners keep `DEFAULT - 1`.

### 3. The record

`flakes.md`: both sightings of each test attributed and fixed, and the
`net-dns` cascade recorded as its first sighting. `testing.md`
(scheduler): the three tests' waits.

### 4. The §70 gate

**Correctness.** Three tests wait for the state they assert. No kernel
change.

**Concurrency.** The spinner's flag is a release store, read with
acquire. Nothing else changes.

**Ownership and lifetime.** None.

**Security.** None.

**Failure.** A spinner that never runs is reported as that.

**Performance.** One short wait per test, normally a millisecond or two.

## Affected files

| file | change |
|---|---|
| `kernel/scheduler/smptest.c` | `entered`, `wait_spinner_entered`, the three tests, the refused spinner's priority |
| `docs/kernel/scheduler/testing.md`, `docs/testing/flakes.md`, `README.md` | the record |

## APIs

None.

## Migration plan

One PR.

## Tests

Each mutation alone, with its boot confirmed:
- `sched-load` back to waiting on the load, with the probe's 100
  repetitions: its checks see a queued spinner again (logged). Its
  failure needs the switch window, so the proof is the log, not a
  failure;
- the migrate tests without the wait: the probe's log again;
- **the refused spinner back at `DEFAULT - 1`, with a thread of the
  reaper's priority made runnable on its CPU just before the migrate:**
  `sched-migrate-refuses` must fail with "affinity", and pass at
  `DEFAULT - 16`. The competing thread is built for the test, so the
  preemption happens on every run.

Also `gmake host-test`, `gmake analyze`, and debug, release, GIC and
chaos boots on both architectures.

## Benchmarks

None.

## Risks

- **A spinner at priority 16 starves its CPU's per-CPU kernel threads**
  (`netrx/N`, for example) for the few milliseconds of the test. That is
  bounded by the test, and the balancer can move a queued thread that
  isn't pinned.
- **`sched-load` stops exercising "a placed thread counts".** Today that
  is what it checks, by accident, 98% of the time. It is part of
  `sched_cpu_load`'s definition (queued plus current); a test that
  asserts it on purpose would be a separate one, and is not proposed
  here.

## Alternatives considered

- **Make the hint exact at the switch.** Setting `rq->current` before the
  dequeue would close the 0 window for this reader. But an unlocked
  reader can still see one field new and the other old somewhere else;
  a hint is a hint. A test must not assert one exactly.
- **Retry the refusal until the spinner is seen running.** "Seen
  running" is a second unlocked read of the same kind: the retry would
  hide the race, not remove it.
- **Pin the reaper away from the tests' CPUs.** That changes the kernel
  to suit a test, and `quiesce` would remain.
