# NEXT SUBSYSTEM — a sampler's bound is a count of waits, not a stopwatch

> **BUILT.** This is the report as written, with an as-built banner.
> What the build changed, and what it found:
>
> 1. **Per call, not global** (found in review). §1 and §2 had a global
>    counter the test read around its sample, and a global knob,
>    `lockup_test_ipi_only`. Review found both wrong: the knob, while
>    set, would have made a *real* report send IPIs and miss a masked
>    CPU on x86-64; and the counter's first read came before the test
>    held the exclusive slot, so another sample in between would have
>    failed it. As built, `lockup_sample_all_info(self, timeout, flags,
>    &answered, &info)` takes `LOCKUP_SAMPLE_IPI_ONLY` for that call
>    alone and reports what that sample did: the slot `claims` it
>    attempted, the `waits` it armed, and the interval `wait_ns` it armed,
>    read back from the deadline. `lockup_sample_all` is it with no
>    flags. `samples_waits` stays, as a machine-wide tally the test does
>    not read.
> 2. **Two checks the count alone missed** (found in review): a loser
>    that *retries* the claim for a while and then refuses passes the
>    order check, and one wait armed for ten times the timeout passes a
>    count of one. As built the loser must have made exactly one claim,
>    and the wait must have been armed for exactly the timeout asked.
> 3. **With the IPI-only flag, x86-64 waits out the timeout for the
>    first time**: about 5 ms for two targets that could not answer,
>    where it had taken microseconds because the NMI answered.
> 4. **The loser's order needed its own mutation.** A loser that waits
>    for the slot and takes it is caught first by "exactly one winner";
>    one that waits for the slot to free and is refused *late* is the
>    order check's alone (row 4).
> 5. **Under the same host load that broke the old bound** (12 busy
>    loops, load average 143 and 186), two aarch64 boots of the first
>    build passed. Two boots are not a rate; the claim is structural --
>    nothing left is broken by elapsed time short of the 1 s guards.
>
> **The mutations**, each applied alone to the build as merged, each
> boot confirmed booted:
>
> | # | mutation | x86-64 | aarch64 |
> | --- | --- | --- | --- |
> | 1 | a wait per target (one deadline per target) | `info.waits == 1` | same |
> | 2 | the IPI-only flag ignored (NMI sent anyway) | neither-answered: both answered by NMI | **equivalent**: no NMI to send |
> | 3 | the loser waits for the slot and takes it | "exactly one winner" | same (first build) |
> | 4 | the loser waits for the slot to free, then is refused | the order (`saw_loser`) | same |
> | 5 | the loser retries the claim for 300 ms, then refuses | `loser->info.claims == 1` | same |
> | 6 | the deadline armed for ten times the timeout | `info.wait_ns == LOCKUP_SAMPLE_TIMEOUT_NS` | same |

## Problem

`lockup-sample-busy` (kernel/core/lockuptest.c) tests the lockup
detector's sampler, `lockup_sample_all` (kernel/core/lockup.c): the
function that asks every other CPU for its program counter and waits, up
to `LOCKUP_SAMPLE_TIMEOUT_NS` (5 ms), for the answers. Its three-CPU part
spins two target CPUs with interrupts masked, samples once, and
requires

```c
CHECK(el < LOCKUP_SAMPLE_TIMEOUT_NS + 2 * 1000 * 1000);   /* :478 */
```

where `el` is the wall-clock time of the call. The comment says what it
is for: "The bound is total, not per target: two CPUs that cannot answer
cost one timeout together, not one each." A per-target wait would cost
10 ms, so 5 ms plus 2 ms of slack separates the two.

The failures do not look like that. On CI, `el` was 88 and 101 ms; on
this developer's machine, 241 ms. A per-target wait would be ten. The
sampler's wait loop runs with preemption disabled, arms one deadline
from the clock, and leaves at the first read past it -- so a call that
takes 100 ms is not a sampler waiting twice. It is a CPU that did not
run.

The two-CPU part of the same test has two more wall-clock bounds of the
same kind: the winner's sample within `LOCKUP_SAMPLE_TIMEOUT_NS + 2 ms`
(the `:399` sightings, before a renumbering) and the loser "refused at
once" within 1 ms.

And on x86-64 the three-CPU claim is not exercised at all: the sampler
sends an NMI, which a CPU answers even with interrupts masked, so the
two "masked" targets answer in microseconds and no timeout is ever
waited.

### Measured

`tools/lockup-busy-probe.py` adds, for the probe only, a record in
`lockup_sample_all`'s wait loop of the largest gap between two
consecutive clock reads, and of all the time lost in gaps over 1 ms; and
replaces the three-CPU part's single sample with 200 samples of the same
shape. With preemption off, a gap is time the CPU did not run -- the
host descheduling the virtual CPU. `ran` is `el` less the time lost in
gaps over 1 ms. The adversary is host load: the probe's `load N`
command runs N busy loops on the host beside the boot (here 12, on an
8-core host; the load average reached 120 to 200).

| image | host | samples | over the bound | `el` min / median / max | targets answered by NMI |
| --- | --- | --- | --- | --- | --- |
| aarch64 | quiet | 400 (two boots) | 0 | 5007 / 5012-5015 / 5116 us | 0 |
| x86-64 | quiet | 200 | 0 | 8 / 19 / 65 us | 200 |
| aarch64 | loaded | 400 (two boots) | 16 and 14 | 5014 / 5032 / 125512 us | 0 |
| x86-64 | loaded | 200 | 0 | 11 / 96 / 1542 us | 200 |

Quiet, the sampler is exactly what the comment says: one 5 ms timeout
for two targets that cannot answer, never more than 5.12 ms. Loaded,
aarch64 reproduces the CI failures, and the second loaded boot's
fourteen samples over the bound say where the time went:

```
LBPROBE over: el 66789 us, largest gap 66011 us, lost in gaps over 1 ms 66011 us, ran 778 us
LBPROBE over: el 81493 us, largest gap 80847 us, lost in gaps over 1 ms 80847 us, ran 646 us
LBPROBE over: el 86826 us, largest gap 82808 us, lost in gaps over 1 ms 82808 us, ran 4017 us
LBPROBE over: el 14090 us, largest gap 358 us, lost in gaps over 1 ms 0 us, ran 14090 us
...
```

In eleven of the fourteen, one gap holds nearly all of it: the CPU
stopped for 8 to 83 ms between two reads of the clock, and the sampler
itself ran for under 5 ms (the deadline passed while it was stopped). In
the other three no single gap reached 1 ms, yet `el` was 7.8 to 14 ms:
the host taking the CPU in slices finer than the probe's threshold --
the same thing, finer. x86-64 never went over, loaded or not, because
its targets answered by NMI within microseconds: there was nothing to
wait for.

So the bound measures three things at once -- the sampler's structure
(one wait or two), the sampler's correctness (that it stops at its
deadline), and the host's scheduling -- and only the last one varies.

### Why it matters

- **It is the most frequent failure left.** Nine sightings in eight
  days, every one a re-run, on branches that did not touch the
  detector -- and a tenth on this report's own CI run (101 ms, aarch64).
- **The property it guards is real and has no other check.** A
  per-target wait would make the hard-lockup report, which samples every
  CPU from an NMI or a tick, take one timeout per wedged CPU. Loosening
  the bound until the host cannot break it (a second, say) would leave
  that regression undetected; keeping it tight leaves the host in charge
  of the verdict. Neither is a test.
- **Half the claim is untested.** On x86-64 the masked targets answer by
  NMI, so "two targets that cannot answer" never happens there.

## Current implementation

**The sampler.** `lockup_sample_all(self, timeout_ns, &answered)`: take
the one reporting slot (`g_reporter`) or return false; stamp a new
sequence number; for each other online CPU, set its `sample.want` and
send an NMI, falling back to an IPI where there is none
(`arch_ipi_send_nmi`: x86-64 always has one, aarch64 none); record this
CPU's own sample; then **one** loop with **one** deadline, collecting
answers until all have come or the deadline passes. The slot is released
by `lockup_print_samples`.

**The test's two-CPU part.** Two racers on CPUs 0 and 1 start together;
exactly one gets the slot and holds it 2 ms before printing; the other is
refused. Checks: one winner, the loser's mask empty, the loser's call
under 1 ms, the winner's under 5 + 2 ms, and one busy refusal counted.

**The three-CPU part.** Two spinners with interrupts masked on the other
two CPUs; one sample; the `:478` bound.

## Design

### 1. The sampler says how many waits it armed

`lockup_sample_all` counts, in the lockup statistics, the deadlines it
arms: `samples_waits`, one per call today, by construction. The test
reads the count before its own sample and again **before releasing the
slot** -- the slot is exclusive, so between those two reads no other
caller can have sampled -- and requires exactly one wait for a sample
whose two targets could not answer. A per-target implementation arms one
per target and fails it; no clock is involved. (**As built, the count is
this call's own, reported by `lockup_sample_all_info`**: the first of the
two reads described here came before the test held the slot, so it was
not covered by the slot's exclusion -- banner item 1.)

### 2. The masked targets cannot answer, on both architectures

A debug-build knob (**as built: a per-call flag, `LOCKUP_SAMPLE_IPI_ONLY`, banner item 1**), `lockup_test_ipi_only(true)`, makes the sampler send
the ordinary interrupt instead of the NMI. With it set, a CPU spinning
with interrupts masked cannot answer on x86-64 either, and the
three-CPU part checks that **neither masked target answered** (their
bits clear in `answered`) -- which proves the sample waited out its
timeout -- on both architectures. The knob is set only around that
sample and cleared on every exit.

### 3. Time bounds that stay, and what they are

- **The three-CPU sample returns** within a generous bound (1 s): a
  guard that names a hang -- a wait that never stopped -- and not a
  measurement. A host stall of a second is not what CI does; the
  largest measured here, under a load average of 200, was 125 ms.
- **The loser is refused at once**, restated as an order rather than a
  time: the winner holds the slot until it sees that the loser has
  returned (bounded by the same 1 s guard) and records whether it did.
  A loser that waited for the slot would return only after the release,
  and the winner would see its guard expire first. Together with "exactly
  one winner", that is the claim, with no clock in it.
- **The winner's own sample** is covered by the same hang guard; its
  "within 5 + 2 ms" said nothing the three-CPU part does not say better.

### 4. The invariant

The lockup detector's invariants gain the sentence: a sample arms one
wait, whatever number of CPUs fail to answer, checked by the wait count
with the targets made unable to answer on both architectures. No new
invariant number: it is the existing claim of the sampler's comment,
given a check that the host cannot fail.

### 5. The §70 gate

**Correctness.** The same claims, checked by counts and orders instead
of wall-clock time, and the half that x86-64 skipped now exercised.

**Concurrency.** The wait count is read under the test's own hold of the
exclusive slot, so it is this sample's alone. The ordering check uses
two flags the racers already share.

**Ownership and lifetime.** As built there is no knob and no global
state: the flag and the report belong to one call (banner item 1).

**Security.** None.

**Failure.** A per-target wait, a sampler that ignores its deadline (the
hang guard), a masked target that answers (the knob not honoured), a
loser that waits: each fails one named check.

**Performance.** One counter increment per sample.

## Affected files

| file | change |
| --- | --- |
| kernel/core/lockup.c | count the waits armed (`samples_waits`); as built, `lockup_sample_all_info` with its flag and per-call report (banner item 1) |
| kernel/include/kernel/lockup.h | the stat and the knob |
| kernel/core/lockuptest.c | `lockup-sample-busy`'s checks as above |
| docs | the lockup detector's invariants and testing docs; `docs/testing/flakes.md` (the entry marked resolved once the build lands; until then it says re-running is still the answer); the inventory; README Status |

## APIs

### New

```c
/* struct lockup_stats */
uint64_t samples_waits;   /* deadlines armed by samples: one per sample */

/* As built, not this: a per-call flag, LOCKUP_SAMPLE_IPI_ONLY, on lockup_sample_all_info (banner item 1).
 * Proposed: sample with the ordinary interrupt, not the NMI, so a CPU
 * with interrupts masked cannot answer on any architecture (tests). */
void lockup_test_ipi_only(bool on);
```

## Migration plan

One PR: the counter and the knob, then the test's checks, then the
documents.

## Tests

| check | replaces | mutation it must catch |
| --- | --- | --- |
| three-CPU sample: `samples_waits` rose by exactly one, read before the slot is released (as built: `info.waits == 1` and `info.wait_ns` equal to the timeout, per call) | `el < 5 ms + 2 ms` (`:478`) | a wait per target: the count rises by two |
| three-CPU sample, IPI only (as built: `LOCKUP_SAMPLE_IPI_ONLY` on that call): neither masked target answered | (nothing: on x86-64 the targets answered by NMI) | the knob ignored: on x86-64 both answer |
| three-CPU sample returns within 1 s | -- (a guard) | a wait loop that ignores its deadline |
| two-CPU part: the loser returned while the winner still held the slot | `loser < 1 ms` | the loser waiting for the slot (as built: waiting *and taking* it is caught first by "exactly one winner"; waiting and then being refused is this check's -- banner item 3, rows 3 and 4) |
| two-CPU part: the winner's sample returns within 1 s | `winner < 5 ms + 2 ms` | -- (a guard) |

Each mutation run alone, on both architectures, each boot confirmed
booted; and the probe's loaded run repeated against the new checks, to
show the host adversary that broke the old bound does not break them.

## Benchmarks

None.

## Risks

- **A count can be satisfied by an implementation that is still slow**
  (one wait whose deadline is ten times the timeout). The hang guard
  catches a wait that never ends, not one that is merely long; the
  timeout's value is a constant the sampler uses directly, and the
  quiet-boot measurement above (5.00-5.12 ms, 400 samples) is recorded
  as the reference if anyone changes it.
- **The knob changes how x86-64 samples during the test.** Only around
  the one sample, cleared after it, called only by the self-test; the
  hard-lockup path that relies on the NMI is untouched and still tested
  by `lockup-sample-irqoff`.

## Alternatives considered

- **Widen the bound** (to 50 ms, or 1 s): 125 ms was measured under
  load, and CI's 101 ms is already past any bound that still separates
  one wait from two.
- **Subtract stolen time**, as the probe does: a guest cannot see time
  its host took in slices finer than its own clock reads (three of the
  fourteen loaded misses), so the subtraction is a diagnostic, not a
  check.
- **Keep it and re-run**, as the flakes entry has: nine sightings in eight
  days is a unit's worth of CI rounds, and the claim underneath is
  checkable without the clock.
