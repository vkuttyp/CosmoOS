# NEXT SUBSYSTEM — sched-balance-hysteresis judges pulls by a hint that counts a yielding CPU twice

> **Status: proposed.** Report and probe (`tools/hysteresis-probe.py`)
> only; nothing in the kernel changes in this PR.

## Problem

`docs/testing/flakes.md` records two sightings of one failure, one on
each architecture, both in boots that change no scheduler code:

```
SELFTEST: sched-balance-hysteresis ... FAIL: the balancer moved a thread for a difference of one
```

- 2026-09-28, x86-64: an nvme-admin mutation boot.
- 2026-09-29, aarch64: the net-leftover unit's check-counting boot.

**The test.** It holds three workers allowed only on CPUs A and B: two
on A, which yield, and one on B, which does not. For 500 ms it watches
the workers' CPUs. Every ~8 ms (a 5 ms sleep on a 4 ms tick) it also
reads both CPUs' loads through `sched_cpu_load`, and the reading
decides the verdict:
- If a worker changed CPU and a reading ever showed a difference of
  two, it logs "not asserted" and passes.
- If a worker changed CPU and no reading did, it fails.
- If nothing moved, it passes.

**What the balancer can do.** It cannot pull for a difference of one.
`sched_migrate_from` re-decides every pull under both runqueue locks
(`load_locked(from) < load_locked(to) + min_gap` refuses), and the
balancer asks for `min_gap = 2`. So every move of a worker is a pull at
a real, locked difference of two or more. The test's failure message
can never be true on this kernel. What the test lacks is the
difference the balancer saw.

The probe finds two defects in the test, one feeding each outcome.

**1. A real third thread on A, between samples, is a legitimate pull
that the test calls a violation.** A thread made runnable on A for a
few milliseconds raises A to 3 against B's 1. If B's scan falls inside
that (a busy CPU scans every `SCHED_BALANCE_TICKS` = 16 ticks = 64 ms),
B pulls. With a third thread running, one of A's workers is queued
after a *yield*, so it is not `THREAD_FLAG_PREEMPTED`, and
`pick_migratable` may take it. If no sample happened to see the 3, the
test fails. That is the two sightings.

**2. The test's premise check is fooled by the hint, in nearly every
boot, so it cannot catch the defect it exists for.**
- **The double count.** `schedule_internal` re-queues a yielding
  thread (`enqueue`, `nr_running` + 1) before it dequeues the next one
  and sets `rq->current`, all under the CPU's lock. An unlocked
  `sched_cpu_load` read inside that window counts the yielder twice,
  queued and running, and reads 3 on a CPU running two threads.
- **Why it is nearly every boot.** A's workers yield in a tight loop,
  so the sampler reads "3 vs 1" in nearly every window. The test then
  treats any move as "not asserted".
- **The consequence.** Under a balancer lowered to a threshold of one,
  the defect this test was written for, it passed on aarch64 with 12
  moves.

### Measured

`tools/hysteresis-probe.py` records every pull of a `hyst` worker
inside `sched_migrate_from`, under both locks: the two loads, the
`min_gap` asked, what the source CPU was running, and what its queue
held. At the test's first broken sample, it also takes A's lock and
records what A holds. Its modes:

- **`--force`**: an intruder pinned to A runs ~3 ms right after each of
  the test's samples, and the next sample waits for the burst to end.
  So the intruder is runnable between samples and never at one. It is
  built from the mechanism, not a stopwatch.
- **`--threshold1`**: the balancer's scan and its locked re-check both
  lowered to a difference of one.
- **`--fix`**: the candidate check (§1).

**Pulls at the moment of decision.** Every forced pull (twelve boots:
the eight in the table's `--force` row, two with the first `--fix`
described below, and the two `--force --fix`) recorded the same shape,
on both architectures:

```
HYPROBE: pull 0: cpu 0 -> 2, locked loads 3 vs 1 (min_gap 2); running hyintr/32; queued [hyst/32,hyst/32]
```

Neither queued worker is marked `P` (preempted): the yielder is the
movable one.

| run | boots | premise "broken" (sampled 3 vs 1) | moves | pulls recorded (below a locked two) | verdict |
|---|---|---|---|---|---|
| probe, unforced | 4 (2 per arch) | **4 of 4** (and 4 of 4 in the 4 boots before the locked peek was added) | 0 | 0 | ok |
| `--force` | 8 (4 per arch, across the intruder's three forms: 5 ms bursts, 3 ms, 3 ms with the sample waiting it out) | 8 of 8 | 1 each | 1 each (0), all `3 vs 1` | ok, "not asserted" |
| `--force --fix` | 2 | x86 yes, **aarch64 no** | 1 each | 1 each (0) | ok |
| `--threshold1` | 2 | aarch64 yes, x86 no | 12 / 2 | 14 (8) / 15 (8) | **aarch64 ok ("not asserted")**, x86 FAIL |
| `--threshold1 --fix` | 2 | yes | 14 / 12 | 15 (15) / 12 (12) | FAIL, both |
| `--fix`, unforced | 2 | 2 of 2 | 0 | 0 | ok |

What the table shows:

- **The false "3".** In every unforced boot, the sampler saw a 3 on A,
  which is CPU 0 or 1. The locked peek a moment later found load 2 each
  time: two `hyst` workers, one running and one queued, and nothing
  else. That is the double count, not a thread.
- **The flake, reproduced.** In the aarch64 `--force --fix` boot, a
  worker moved on a locked 3 vs 1, and no sample ever saw the premise
  broken. That is exactly the sightings' shape: the unmodified test
  would have failed it. The candidate judged it by the locked loads and
  passed.
- **The first threshold-of-one table** (an earlier `--fix` that still
  gated on the sampler's moves) showed a second hole. With pulls going
  back and forth between samples, x86 recorded 14 pulls, 8 of them
  below two, with **no** sampled move, and passed. The candidate
  therefore fails on any pull below two, whatever the sampler saw.
  This is the version in the table above.

### Why it matters

As written, the test fails a correct kernel rarely. More often, it
passes one with the defect it exists to catch.

## Current implementation

- **`kernel/scheduler/sched.c`, `sched_cpu_load`:** an unlocked hint,
  `nr_running + (current != idle)`. It double-counts a CPU inside
  `schedule_internal` between re-queueing `prev` and setting
  `rq->current`.
- **`sched_migrate_from`:** re-decides under both locks with `min_gap`.
- **`balance_tick`:** scans with the hint, and asks for `min_gap = 2`.
- **`kernel/scheduler/smptest.c`, `sched_balance_hysteresis_pinned`:**
  the test above. `bal_worker_main` records the CPU it runs on.

## Design

### 1. The balancer records what it saw, on the thread it moved

In debug builds, `struct thread` gains two fields, written in
`sched_migrate_from` under both locks when the caller asked for a gap
(`min_gap != 0`, so the balancer and not the chaos migrator):

- `bal_pulls`: this thread's balancer pulls.
- `bal_gap_min`: the smallest locked difference among them,
  `load_locked(from) - load_locked(to)` before the move.

The test sets `bal_gap_min` to its maximum and reads `bal_pulls` before
widening each worker's affinity; no pull can happen while a worker is
pinned. After the window and the join, it reads both again:

- **Any pull with `bal_gap_min < 2` fails.** "The balancer pulled a
  thread for a locked difference below two", whether or not the sampler
  saw the worker move.
- **A move with every pull at two or more** is the rule obeyed and
  passes, logged with the gap seen.
- **No pull** passes, as before.

The sampled premise check and its "not asserted" escape are removed;
the sampler stays only to report moves in the log.

### 2. A real third thread, made certain: the intruder phase

After the quiet window, the test adds an intruder pinned to A that
spins until a worker has been pulled, bounded at 1 s: a steady 3 vs 1.
It asserts that exactly this happens: a pull, recorded at a locked
difference of two or more, and no failure. That is the sightings'
case, as a proof rather than a rerun, and it proves the record is
written.

### 3. The record

- **flakes.md:** the entry, explained by this PR, gains "fixed". This
  PR also records three sightings from the probe's boots.
- **`docs/kernel/scheduler/testing.md`:** the test's claim and why it
  is judged by the locked record.
- **`sched_cpu_load`'s comment:** it names the double count, so the
  next reader of the hint knows it reads one high on a CPU mid-switch.
- **README Status entry.**

## Affected files

The implementation's. This PR adds the probe, this report, the
explanation under the hysteresis flakes entry, and three new flakes
entries from the probe's boots.

| file | change |
|---|---|
| `kernel/include/kernel/thread.h` | `bal_pulls`, `bal_gap_min` (debug) |
| `kernel/scheduler/sched.c` | record them in `sched_migrate_from`; `sched_cpu_load`'s comment |
| `kernel/scheduler/smptest.c` | the verdict from the record; the intruder phase |
| `docs/kernel/scheduler/testing.md`, `docs/testing/flakes.md`, `README.md` | as above |

## APIs

None outside debug builds.

## Tests

| test | proves |
|---|---|
| `sched-balance-hysteresis` | no balancer pull of its workers below a locked difference of two, across a quiet window, whatever the sampler sees; then, with an intruder on A, a legitimate pull at two or more passes |

**Planned mutations** (each alone, both architectures, boot
confirmed):

| mutation | expected |
|---|---|
| `--threshold1`'s two lowerings (the scan's `mine + 2` and `min_gap`) | fails, a pull below two |
| only `min_gap` lowered to 1 | fails, a pull below two |
| the record not written | the intruder phase fails: a move with no pull recorded |
| the intruder phase's pull judged by the old sampled premise | fails in some boots; recorded as a rate, not asserted |

## Benchmarks

None. `sched_migrate_from` gains two stores in debug builds, under
locks it already holds.

## Risks

- **The fields are written under both runqueue locks and read by the
  test after its workers are joined.** No concurrent reader exists.
- **The intruder phase depends on B scanning within its bound.** B is
  busy, so it scans every 64 ms, and 1 s is about fifteen scans.

## Alternatives considered

- **Fix the double count in `schedule_internal`**, by dequeuing next
  before re-queueing prev. That changes the scheduler's core for a
  hint whose every reader already re-decides under locks. The count
  has no other known consumer that asserts on it. Noted in the comment
  instead.
- **Read the premise under locks in the test.** That still samples,
  and a transient between samples is exactly what it misses (defect 1).
- **Keep "not asserted" and sample more often.** It narrows defect 1
  and leaves defect 2 whole.
- **Move A and B away from CPU 0.** The double count is on whatever
  CPU A is, and A was CPU 1 in some boots too.
