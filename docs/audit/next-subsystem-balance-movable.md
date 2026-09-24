# NEXT SUBSYSTEM — the balancer's test asserts what the balancer promises

> **BUILT.** This is the report as written, with an as-built banner.
> What the build changed, and what it found:
>
> 1. **The S26 proof needed all of S26 removed.** The report's proof --
>    `pick_migratable` ignoring `PREEMPTED` -- never reached the new
>    test: `migrate_locked` asserts the same rule, and the boot panicked
>    at 16 s on it, in both images. That is a catch, by the assertion.
>    To show `sched-balance-pair`'s spinning half carries the claim, the
>    rule was removed in both places at once; then the spinning pair was
>    separated and `sched-balance-pair` failed, alone, in both images.
> 2. **The pair test's two halves run one after the other on the same
>    CPU**, the yielding pair first, within the test's own watchdog
>    (about a second: the spinning half waits out its bound by design).
>
> **The proofs**, each run alone, each boot confirmed booted, on x86-64:
>
> | # | proof | image | what failed |
> | --- | --- | --- | --- |
> | 1 | `SCHED_BALANCE=0` | plain | `sched-balance-pull` (4 runnable threads on 2 of 4 CPUs after 3 s) and `sched-balance-pair`'s yielding half (not separated) |
> | 2 | `pick_migratable` ignoring `PREEMPTED` | plain, chaos | the boot, at 16 s: `migrate_locked`'s S26 assertion |
> | 3 | S26 removed entirely (the policy's check and the assertion's clause) | plain, chaos | `sched-balance-pair`: the spinning pair separated -- the only failing test |
>
> Both tests pass in both images on both architectures.

> Constitution §68 report. Takes up `sched-balance-pull`'s failures under
> the chaos migrator -- five on CI in two days, the last two in a row on
> aarch64, every one on a tree that could not have caused it
> (`docs/testing/flakes.md`, "Under the chaos migrator:
> `sched-balance-pull`"), which says the decision the balancer's owners
> hold, whether this test asserts under chaos at all, is due. It belongs
> to the deferred-work inventory's section 2.3 entry for the pull
> balancer (S26-S29, `make test-chaos`). The answer here is neither to
> skip it nor to widen it: the test asserts a race the balancer usually
> wins, and the fix is to assert the property the balancer actually
> promises.

## Problem

`sched-balance-pull` (kernel/scheduler/smptest.c) creates twice as many
workers as CPUs, one per CPU in turn, releases every other one to spin,
and requires that within 3 s the released workers are running on as many
CPUs as there are of them. It passes in every plain boot. Under the chaos
migrator (`make test-chaos`, which moves one ready thread from every CPU
to the next every fourth tick) it has failed five times on CI:

```
SELFTEST: sched-balance-pull ... FAIL: runnable threads stayed on the
CPUs creation order gave them (3016 ms)
```

The scheduler's own testing document already names the mechanism
(`docs/kernel/scheduler/testing.md`, the paragraph on the balancer's
window): a released worker is movable only until its first preemption.
Two compute-bound threads sharing a CPU alternate by preemption, so the
one in the queue always carries `THREAD_FLAG_PREEMPTED`, and S26 forbids
any migrator to move it -- a thread switched out by preemption may be
between the two instructions of a per-CPU access. So the balancer must
pull a released worker in the window before that worker's first slice
ends, and "once they are alternating, the imbalance is permanent for that
round". In a plain boot an idle CPU looks every tick and wins that race.
The chaos migrator can lose it by the rules in one move: it takes a
freshly released, not-yet-preempted worker and puts it on a CPU that is
already running one. From then on no migrator may separate the pair,
and the test can only time out.

The test therefore asserts a *race*: that the balancer wins the window.
That is not the balancer's contract. Its contract (S27, S28, with S26) is
that an idle CPU pulls a **movable** runnable thread from a CPU two or
more ahead.

### Measured

`tools/balance-chaos-probe.py` puts three measurements, for the probe
only, in place of three balance tests' bodies -- each in its own test so
each has that test's eight-second watchdog -- and runs in the chaos
image and in the plain debug image (which differ only in `SCHED_CHAOS`),
on both architectures. The probe was corrected in review (each phase in
its own test, setup failures counted apart from spreads, the user
workload confirmed formed before it is sampled, sharing read per CPU,
the pair's premise observed rather than slept for); the figures below
say which version measured them.

1. **The test's own scenario, repeated** (`BPROBE`): as many rounds as
   fit in the watchdog, each with a 1.5 s bound, and on a miss a dump of
   every released worker's last CPU, state and `PREEMPTED` flag with the
   balancer's and the migrator's counters. **Every round that spreads,
   spreads within 38 ms; a miss never spreads.** Misses: 1 in 360 chaos
   rounds over nine chaos boots, 0 in 320 plain rounds over eight plain
   boots (the corrected probe's four boots among them: no miss, no setup
   failure). The one miss, on x86-64, from the first version:

   ```
   BPROBE round 0 MISS after 1500 ms: cpus used 3 of 4
   BPROBE   worker 0: last cpu 3, state 0, preempted 1, queue cpu 3
   BPROBE   worker 6: last cpu 3, state 1, preempted 0, queue cpu 3
   BPROBE   chaos migrated +4, balancer pulls +2, refused preempted +0
   ```

   Workers 0 and 6 alternating on CPU 3, the queued one (`state 0`,
   READY) preempted; one CPU with no worker. (The first version of the
   probe counted the wrong refusal: a queue whose only spare thread is
   preempted offers nothing, so `pick_migratable` returns NULL and the
   pull reports `NOT_READY`, not `PREEMPTED`. The probe prints
   `NOT_READY` now.)

2. **The pair, made on purpose** (`PPROBE`): two workers pinned to one
   CPU; the probe waits (up to 1 s) for the pair's premise -- for a
   spinning pair, the queued one seen READY with `PREEMPTED`; for a
   yielding pair, both switched in more than once -- then widens both to
   every CPU and waits up to 1 s for them to run on two CPUs. A pair
   whose premise is not seen is reported inconclusive, not measured.
   (The first version slept 50 ms instead of observing the premise; the
   corrected one saw it in every run.) Deterministic, and the same in all
   eight boots that ran it -- both architectures, plain and chaos, both
   versions of the probe:

   ```
   PPROBE spinning pair on cpu 0: queued one preempted at widen 1; separated NO after 1003 ms; balancer pulls +0, refused not-ready +502
   PPROBE yielding pair on cpu 0: queued one preempted at widen 0; separated yes after 3 ms; balancer pulls +1, refused not-ready +0
   ```

   Spinning pairs: never separated, 455 to 502 refusals in the second.
   Yielding pairs: separated in 3 to 7 ms. An idle CPU tries every tick
   -- some five hundred refusals a second -- and
   is refused every time, by the rule that protects per-CPU accesses. A
   yielding pair is never preempted in the queue, and the first pull
   separates it. One move of a spinner onto a busy CPU is enough to make
   the test's claim unreachable; the chaos migrator makes such moves at
   random.

3. **The same question for user threads** (`UPROBE`): `init --probe
   spin:N` (added for the probe) runs one spinning native thread per
   CPU, sampled every 10 ms. The corrected probe, which first confirms
   all N threads exist and are placed, in four boots: **one sharing, in
   the aarch64 chaos boot, for 16 ms, with no preempted thread queued on
   that CPU -- and it resolved**. The first version (eight boots, 4 s
   each, the workload not confirmed) saw none. So the case where S26's
   cost would reach a real program -- two compute-bound user threads
   *stuck* on one CPU -- was not observed, and this report does not
   change S26 (see Alternatives).

   One corrected-probe boot (x86-64, chaos) failed a later test,
   `bench-balance`, on its `threads_settle` check -- the kernel's thread
   count back to its starting value within 200 ms. It ran after the
   probe had killed its user spinners, whose threads are reaped
   asynchronously; it has not been seen in any boot without the probe
   and is not attributed to the tree.

The rate, 1 in 360 rounds here against five failures in two days on CI,
is a rate and says only that CI's hosts lose the window more often; the
mechanism, which item 2 makes certain, is the same on both.

### Why it matters

- **Every unit has been paying for it.** Five CI runs in two days
  failed here -- on `main` itself and on PRs #225, #229, #232 and #233,
  none of which touched the scheduler -- and each was re-run. A test that fails for reasons its own documentation already
  states is teaching everyone to re-run on red.
- **A re-run that goes green says nothing.** The test cannot tell a
  broken balancer from a lost race, so its passes and its failures are
  both uninformative under chaos.
- **Skipping it under chaos**, as `sched-balance-hysteresis` does, would
  lose the one place the balancer runs against a migrator that moves
  things for no reason -- which is the environment it was built to
  survive.

## Current implementation

**The workers.** `bal_worker_main`: after its release, a loop that
records `arch_cpu_id()` under `preempt_disable`, spins 256 iterations,
and -- only if `yielding` is set -- calls `sched_yield`. The pull test
releases every other worker with `yielding = 0`. `sched-balance-
hysteresis` already sets `yielding` on the pair it needs movable, with
the reason written at the site: "A yielding worker is a *movable* one".

**The balancer** (`balance_tick`, sched.c): an idle CPU looks every tick,
a busy one every 16; it tries the busiest few CPUs at least two ahead, in
order, and `sched_migrate_from` takes a thread the policy offers
(`rr_pick_migratable`: READY, not `current`, not `PREEMPTED`, allowed
there). A queue whose only spare thread is preempted offers none.

**The chaos migrator** (`chaos_tick`): every fourth tick each CPU moves
one thread its queue can spare to the next online CPU in a rotation,
with no load test ("chaos asks for no gap: it moves for no reason").

## Design

### 1. The pull test's released workers yield

`sched-balance-pull` sets `yielding` on the workers it releases. A
yielding worker gives its CPU up every 256 iterations (`sched_yield`:
a switch without the preemption mark, back onto the same CPU), so a
queued one is READY by its own call and S26 lets a migrator take it. It
is not *never* preempted -- a reschedule pending when an interrupt finds
it between yields preempts it like any thread -- but it gives the CPU up
long before a slice ends, and it clears the mark the next time it runs,
so it cannot sit preempted in a queue for a round the way a spinner
does; the pair measurement found its queued worker unmarked. The runnable set is still two-deep on half the CPUs with the
other half idle, and without a balancer it still stays that way -- a
yield keeps a thread on its CPU; only a migrator moves it -- so the test
still fails with the balancer compiled out (`SCHED_BALANCE=0`), which is
how it was proved. What changes is that a chaos move can no longer make
the claim unreachable: a pair the migrator builds is movable, and the
balancer separates it.

The claim becomes the contract: **an idle CPU pulls a movable runnable
thread from a CPU two or more ahead**, within the bound, whatever the
migrator does. The window -- a compute-bound thread is movable only
until its first preemption -- stays documented where it is, as the
balancer's limit rather than a property any test asserts.

### 2. The pair, as a test

`sched-balance-pair` makes the probe's item 2 a test, both halves:

- **A yielding pair is separated.** Two yielding workers pinned to one
  CPU (not the test thread's) and released; once each has been switched
  in more than once -- observed, within a bound -- both are widened to
  every CPU, and within a bound they run on two different CPUs.
- **A spinning pair is not.** The same, except that the premise
  observed before the widen is the queued one READY with `PREEMPTED`;
  the pair must still share one CPU at the end of the same window. A
  premise not seen within its bound fails the test as a broken premise,
  distinctly from either claim. This is S26 observed from outside: were a
  migrator ever to take a preempted thread, this half would see the pair
  separated. It is also what makes the first half mean something -- the
  only difference between the two halves is the yield.

Both halves hold under the chaos migrator too (it honours S26 as the
balancer does), so the test runs in both images.

### 3. The invariant

No new invariant: the balancer's contract is S27-S28 with S26, and S26's
cost is already the scheduler's documented gap ("A thread time-slicing
with another on the same CPU is never moved"). That gap's text gains the
measurement -- five hundred refused pulls a second, deterministic -- and
`sched-balance-pair` as its check.

### 4. The §70 gate

**Correctness.** A test whose outcome under chaos was decided by a race
the rules allow the migrator to lose now asserts a property the rules
guarantee. No kernel code changes.

**Concurrency.** The workers' yields are the existing `sched_yield`; the
pair test's pinning and widening use the existing `thread_create_on` and
`thread_set_affinity`, as `sched-balance-hysteresis` does.

**Ownership and lifetime.** Every worker is stopped and joined on every
exit, as the existing helpers do.

**Security.** None: tests only.

**Failure.** The pull test fails when an idle CPU does not pull a
movable thread within the bound -- a balancer defect, and nothing else.

**Performance.** None.

## Affected files

| file | change |
| --- | --- |
| kernel/scheduler/smptest.c | `sched-balance-pull`'s released workers yield; `sched-balance-pair` |
| kernel/core/selftest.c, kernel/include/kernel/selftest.h | register `sched-balance-pair` |
| docs | the scheduler testing doc (the pull test's claim, the pair test, the window paragraph), the S26 gap's text, `docs/testing/flakes.md` (the entry resolved), the inventory's section 2.3 entry, README Status |

## APIs

None new. Relied on: `sched_yield`, `thread_create_on`,
`thread_set_affinity`, `sched_balance_stats`.

## Migration plan

One PR: the pull test's workers, the pair test, the documents.

## Tests

| test | checks | proof it must fail |
| --- | --- | --- |
| `sched-balance-pull` (changed) | as now, with the released workers yielding | `SCHED_BALANCE=0`: the workers stay two-deep on half the CPUs |
| `sched-balance-pair` (new) | a yielding pair built on one CPU is separated within a bound; a spinning pair in the same window is not, its queued one preempted at the widen | `SCHED_BALANCE=0`: the yielding pair is never separated; `pick_migratable` ignoring `PREEMPTED` (the S26 check removed): the spinning pair is separated |

Each proof run alone, the runner confirming each boot booted. The
`SCHED_BALANCE=0` proofs run in the **plain image only**: in the chaos
image the migrator can do the balancer's work and spread the workers
itself (Risks). The S26 proof -- `pick_migratable` ignoring `PREEMPTED`
-- runs in both images.

## Benchmarks

None.

## Risks

- **The chaos migrator can do the balancer's work.** With the balancer
  compiled out, a chaos boot may still spread yielding workers, because
  chaos moves them too. The proof that the pull test depends on the
  balancer is therefore made in the plain image, where nothing else
  moves them; the report says so rather than claiming it under chaos.
- **The window is no longer asserted anywhere.** It was never asserted
  as such -- the pull test won it without saying so -- and it cannot be
  asserted deterministically (it is a race by construction). It stays a
  documented limit, with the pair test showing its cause.

## Alternatives considered

- **Skip the pull test under chaos**, as `sched-balance-hysteresis`
  does. Hysteresis skips because its claim -- *no* move happens -- is
  one chaos contradicts by design. The pull claim is not: an idle CPU
  should still pull under chaos, and with movable workers it does.
- **Widen the bound.** A lost window never recovers; the probe's misses
  sat at the bound in every case. Any bound fails the same way.
- **Stop the chaos migrator from moving a thread onto a busier CPU.**
  The rotation moves a spare thread from a two-deep CPU to the next one,
  which is often running a worker; a load test would make chaos a second
  balancer and stop it exercising the placements it exists to produce.
- **Let a preempted thread move when it was preempted in user mode**,
  which cannot be mid-way through a kernel per-CPU access. That is where
  S26's cost would reach real programs, but the probe's user-thread
  measurement found no pair stuck in twelve boots (one sharing, 16 ms,
  that resolved), so there is no measured
  defect to fix; recorded in the inventory for when there is one.
