# NEXT SUBSYSTEM — what the straggler kick is worth

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. **This report is as built**
(PR #179), and the banner below records where the build differed from
it.

**THE ANSWER: the kick works, rarely — and the report expected the
opposite.** Over six boots, three per architecture: **161 kicks sent, 7
publishes attributed, about four per cent.** The decision rule fixed
below before the measurement says a counter that rises anywhere means
the kick works and the follow-up bounds it. It rose on both
architectures, reproducibly, so **deletion is off the table** — which
this report called "the likely outcome" three times.

**What the build changed, each found by building rather than reading:**

1. **The population this report named is not the reason the kick
   works.** The adversary was built exactly as designed, phase-locking a
   short read-side section over the target CPU's own tick, and it
   showed the opposite of what it was built to show: **that population
   publishes without any kick.** `schedule()` publishes at entry
   (`sched.c`), and the covered tick still sets `need_resched`, so the
   `preempt_enable` ending the very section that hid the tick runs
   `sched_preempt` and publishes a moment later. **The publish is not
   confined to the trap return**, which is the premise the whole
   "helpable population" story rested on — including the code comment
   this unit was written to settle. The test is kept as
   `quiesce-kick-population` and asserts what is true: the grace period
   completes while ticks are being covered.
2. **So the report's central prediction was wrong twice over**: the
   adversary *was* buildable (the inventory said no deterministic test
   could arrange the coincidence), and arranging it disproved rather
   than proved the mechanism. What the four per cent comes from is not
   identified here: it is some other population, and naming it is not
   this unit's job now that the counter exists to find it.
3. **`quiesce-kick-attributed` is not a test in the tree.** The report's
   test table named it as the positive case. Since the arranged
   population publishes without a kick, a test asserting an attributed
   publish would be asserting a coincidence and would flake. The
   positive evidence is the whole-boot counter on the
   `quiesce: straggler kicks sent N, publishes attributed M` line
   instead, and the test that replaced it asserts the mechanism it
   actually demonstrated.
4. **Two docs were already stale before this unit touched them.**
   `docs/kernel/interrupt/controllers.md` and
   `docs/kernel/smp/architecture.md` both enumerate the IPI kinds and
   both omitted `IPI_SAMPLE`, which predates this work. Fixed to match
   the header rather than merely appended to.

**Subsystem: a straggler kick that can be shown to work, or deleted.**
After eight milliseconds of waiting, `synchronize_quiesce` sends
`IPI_RESCHEDULE` to every CPU still pending, up to eight times
(`kernel/core/quiesce.c:142-171`). The mechanism has been in the tree
since the quiesce work began. **Nothing counts a kick that worked**, no
test arranges the one population it can help, and the code says so
itself: "what this kick is worth is an open question rather than a
measured fact". The inventory agrees and names the shapes —
"deleting it, bounding it, or proving it are three different units"
(`docs/audit/2026-09-deferred-work-inventory.md` §4). This unit does the
measurement that decides which of the three the next one is.

## Problem

The kick's only effect is to make the target take an interrupt. Whether
that interrupt publishes is decided by the trap return, which requires
all three of:

```c
if (pc->irq_depth == 0 && pc->preempt_count == 0 && (frame->rflags & RFLAGS_IF)) {
    quiesce_note_quiescent_preemptible();
```

(`kernel/arch/x86_64/trap.c:93`, and the AArch64 twin at
`kernel/arch/aarch64/trap.c:120` testing `DAIF_I`). The handler itself
does nothing at all but count (`kernel/interrupt/ipi.c:36-42`).

Four things follow, and the fourth is a latent defect rather than a gap:

1. **A CPU spinning with preemption off cannot be helped**, because
   `preempt_count != 0` in its trap return. `quiesce-straggler` asserts
   exactly this and is the only thing any test says about the kick: it
   proves the kick does *not* help the case it was once believed to.
2. **The population it can help has never been observed.** A CPU whose
   *periodic tick* keeps landing inside a short disabled region is
   helped by an interrupt at an unrelated phase, which lands outside one
   and publishes. That is a coherent story. It is also, at present,
   only a story.
3. **`straggler_ipis` counts kicks sent, never kicks that worked.**
   There is no counter anywhere whose value would change if the kick
   were replaced by a no-op. The mechanism is unfalsifiable as it
   stands, which is the property this unit removes.
4. **`IPI_RESCHEDULE` has two callers with different intents and one
   documented contract.** The enum says "target re-evaluates
   `need_resched` on interrupt return" (`kernel/include/kernel/ipi.h:16`)
   and the handler's comment says "the sender set `need_resched` under
   our run-queue lock". The scheduler's caller sets the flag before it sends
   (`request_resched`, `kernel/scheduler/sched.c:212-220`). **The quiesce caller sets no
   `need_resched` and holds no run-queue lock** — it wants only the trap
   return. Today the two coincide because the handler is empty.
   **The hazard is on the send side, not in the handler**, and this
   report said otherwise in its first revision. A handler that returned
   early on `!need_resched` would *not* neuter the kick: the
   architecture's trap tail evaluates
   `quiesce_note_quiescent_preemptible` independently of what the
   handler did, and the kick needs only delivery and that tail. What
   would neuter it is a **send suppressed because the target's
   `need_resched` is clear** — a guard of the shape "nothing to
   reschedule there, do not interrupt it", which is the natural
   optimisation for an IPI whose documented purpose is re-evaluating
   that flag. The quiesce caller never sets it, so **every kick would
   be suppressed** and no test in the tree would notice.

   Stated that precisely because the looser forms do *not* follow, and
   this report gave two of them in its second revision: skipping when
   the flag is **already set** would still send for the quiesce caller,
   which leaves it clear; and coalescing repeated sends preserves the
   first interrupt, which is the one that matters. The hazard needs the
   guard pointing the other way — and that is exactly the guard a
   reader of the enum's contract would think safe to add. Fixing it is
   worth doing whatever the measurement says.

## Current implementation

```c
uint64_t waited = clock_since_ns(start);
if (waited > 2 * TICK_NS && kicks < 8) {
    for (unsigned c = 0; c < cpu_count(); c++) {
        if ((pending & CPUMASK_OF(c)) && c != pc->cpu_id && cpu_online(c))
            ipi_send(c, IPI_RESCHEDULE);
    }
    kicks++;
    g_stats.straggler_ipis++;
}
```

`TICK_NS` is 4 ms at `CONFIG_HZ` 250 (`kernel/include/kernel/timer.h:17,19`),
so the threshold is 8 ms. The loop re-arms on a `TICK_NS / 2` deadline,
so the eight kicks are spent over roughly the next sixteen milliseconds,
each one an IPI to *every* pending CPU.

`quiesce_test_sync_kicks` already hands a waiter the count of the kicks
its own call sent, on the stack, because the machine-wide counter cannot
carry a per-waiter claim. That works because the **sender** increments
it; design point 3 records why a publish, which the *target* increments,
cannot be attributed the same way.

## Why it matters

- **It runs on the paths that matter.** Every synchronous caller reaches
  it: `interrupt_unregister`, module unload, `netif_unregister`, the
  receive-hook removal, and the `call_quiesce` batch worker.
- **The cost is real and bounded only by a magic number.** Up to eight
  rounds times the pending count, in IPIs, on a machine already slow
  enough to have waited 8 ms.
- **It is the last unmeasured piece of a wait that was just measured.**
  The quiesce-wake unit (PR #175) removed the polling overshoot and put
  numbers on the rest — 3.73–3.81 ms a grace period against 4.29–7.55.
  The kick is what remains of that wait with no number against it.
- **An unfalsifiable mechanism is the thing this project keeps finding
  under other names**: a rule stated in one place and enforced nowhere,
  a claim that outlives what made it true. Here the claim is a comment,
  and it has outlived two units that touched the function around it.

## Design

The rule: **a kick that worked is a publish that happened in that
kick's own trap return. Count that, and the question answers itself.**

1. **A distinct IPI kind, `IPI_QUIESCE_KICK`.** It separates the two
   intents so `IPI_RESCHEDULE`'s contract covers only the scheduler's
   use, and it closes problem 4: a future change to the reschedule IPI
   cannot silently disable the kick, because the kick no longer uses it.
   Its handler sets a per-CPU flag and counts; like the reschedule
   handler it does no work of its own.
2. **Attribution in the trap tail, not in the publish.** The flag is
   read **and cleared unconditionally in the architecture trap tail**,
   beside the three-condition test, and the publish is counted as a
   `kick_publishes` only when that same return also published.

   Clearing it inside `quiesce_note_quiescent_preemptible` — which this
   report proposed first — is wrong, and wrong in the direction that
   destroys the measurement. A kick that arrives while
   `preempt_count != 0` does not publish, so the flag would **survive
   its own trap**; the next unrelated interrupt return, or an
   `idle_main` iteration, would then clear it and count a publish the
   kick did not cause. That falsely attributes in exactly the case the
   kick is known *not* to help, so it would corrupt the positive
   measurement and silently break the negative control, which is the
   one test that would otherwise catch it. Clearing unconditionally in
   the tail bounds the flag's life to the single trap that set it.
3. **A per-CPU count, and no per-waiter claim — because none is
   available.** `kick_publishes` is per-CPU, indexed by the CPU that
   published, and a test reads the counter of the CPU it pinned its
   adversary to.

   The stack-returned count that works for `quiesce_test_sync_kicks`
   does **not** generalise here, and this report claimed it would. That
   count is local because the *sender* increments it as it sends. A
   publish is incremented on the **target** CPU, asynchronously, and the
   IPI carries no payload identifying the waiter that sent it, so two
   waiters kicking the same pending CPU are satisfied by one publish and
   neither can claim it. A per-waiter figure is therefore not
   implementable as specified, and asserting one would be a claim the
   mechanism cannot support.

   What replaces it is the *pair*: the adversary's CPU counter rising,
   and the negative control's not. Neither number alone carries the
   claim; the difference between them does.
4. **A deterministic adversary for the positive case.** The helpable
   population is a CPU whose tick keeps landing inside a short disabled
   region, and the inventory calls that "a phase coincidence no
   deterministic test can arrange". That is the claim this unit tests
   rather than accepts: the adversary is built from the mechanism, not
   from a stopwatch — a thread pinned to the target CPU that reads that
   CPU's tick, then phase-locks a short `preempt_disable` region to
   arrive just before each expected tick, so the tick lands inside and
   the kick, at an unrelated phase, lands outside.
5. **The decision rule, written down before the measurement.** The
   counter decides, and only the counter:
   - `kick_publishes` rises anywhere — the adversary, or ordinary
     operation on either architecture — and the kick **works**; the
     follow-up unit bounds it.
   - `kick_publishes` is **identically zero** across the whole suite on
     both architectures, and the kick is dead weight; the follow-up
     deletes it.

   The adversary's job is to *try to produce* a non-zero, so failing to
   build it is not a third outcome and not independent evidence — it
   only leaves the count standing alone. **Both outcomes are a result.**
   Stating this now is what stops the number being read to suit
   whichever answer arrives.

## Affected files

| file | change |
| --- | --- |
| `kernel/include/kernel/ipi.h` | `IPI_QUIESCE_KICK` before `IPI_KIND_COUNT`; the comment says it wants the trap return, not `need_resched` |
| `kernel/interrupt/ipi.c` | handler and table entry; sets the per-CPU flag |
| `kernel/include/kernel/percpu.h` | the flag |
| `kernel/core/quiesce.c` | send the new kind; clear-and-count in the preemptible publish; `kick_publishes` in the stats |
| `kernel/include/kernel/quiesce.h` | per-CPU `kick_publishes` and its accessor |
| `kernel/core/quiescetest.c` | the adversary and the negative control |
| `docs/kernel/smp/design.md`, `api.md`, `architecture.md` | each enumerates the IPI kinds; all three gain `IPI_QUIESCE_KICK` and the reason it is not `IPI_RESCHEDULE` |
| `docs/kernel/interrupt/controllers.md` | names the kinds where vectors are allocated |
| `docs/kernel/quiesce/invariants.md` | **Q19**: what a kick can and cannot do, what is counted, and that the flag lives for one trap |
| `README.md` | the Status entry |

## New APIs

`IPI_QUIESCE_KICK` (internal), a per-CPU `kick_publishes` counter with a
read accessor taking a CPU id (debug builds, as `quiesce_test_sync_kicks`
is), and the machine-wide total in `struct quiesce_stats` for the boot
line. **No per-waiter accessor**, for the reason design point 3 gives.
No syscall, no user-visible change.

## Migration plan

One commit. The new IPI kind is additive and both architectures
allocate vectors from the same enum, so nothing else moves. The kick's
behaviour is unchanged by construction — the same interrupt at the same
moment, under a different number — which is what makes the measurement
a measurement rather than a change.

## Tests

| test | asserts |
| --- | --- |
| `quiesce-kick-attributed` | the adversary: `kick_publishes` for **the CPU the adversary is pinned to** rises by at least one |
| `quiesce-kick-spinner` | the negative control, on `quiesce-straggler`'s shape: kicks are sent and `kick_publishes` does **not** rise, because a CPU with `preempt_count != 0` cannot publish in a trap return |
| `quiesce-kick-ipi-kind` | the kick sends `IPI_QUIESCE_KICK` and not `IPI_RESCHEDULE`, so the scheduler's IPI can change without disabling it |
| existing `quiesce-straggler`, `-system`, `-idle` | unchanged and still passing |

**The bug-proof, run.** With attribution wired and the *send* replaced
by a no-op, a whole boot recorded **`straggler kicks sent 27, publishes
attributed 0`** — against 1 to 2 with the send in place. The kick's
bookkeeping still ran, so only the IPI was suppressed and the variable
is isolated. `quiesce-kick-spinner` failed in the same run, at
`kick_ipis_after > kick_ipis_before`: the target took no kick IPIs
because none were sent, which is the test noticing exactly what was
broken. Neither the counter nor the test is vacuous.

The original statement of this proof said: A version that
counts a publish the kick did not cause would pass the positive test
and prove nothing, which is the failure mode this project has a name
for — and the negative control is the second half of it, because a
counter that only ever goes up is not attribution.

**What the tests must not assert** is a duration. "A grace period was
faster with the kick" is the claim a stopwatch would make, and
`docs/testing/flakes.md` has the tally of what that costs. The claim
here is that a *specific publish* happened in a kick's return.

## Benchmarks

None proposed, and deliberately. The unit's output is a count, not a
time. If the count is non-zero, the follow-up unit that bounds the kick
is where a time belongs, measured against a population that is by then
known to exist.

## Risks

- **The adversary may not be buildable.** Phase-locking to another
  CPU's tick may prove too coarse. That does not by itself decide
  anything: failing to *arrange* a benefit is not evidence that none
  exists, so the decision falls to the counter, exactly as design point
  5 states it — deletion only if `kick_publishes` is identically zero
  across the suite on both architectures. An unbuildable adversary
  leaves that count as the only evidence rather than supplying a second
  one.
- **It may be flaky if written as a timing assertion.** Mitigated by
  design point 5 and the Tests section: the assertion is a counter
  for the adversary's own CPU, not a duration, and the negative control
  must stay at zero — the pair is the claim, since neither number alone
  can be attributed to one waiter.
- **Adding an IPI kind touches vector allocation on both
  architectures.** Small, but it is the one place this unit can break
  something that has nothing to do with quiesce; both arches' boots are
  the check.

## Alternatives considered

- **Just delete it.** Cheapest, and the argument is decent: the only
  test that says anything says it does not help. Rejected as the *first*
  step, because it deletes an unmeasured mechanism on the strength of an
  argument, and if the helpable population is real this lengthens grace
  periods on exactly the machines already slow enough to need them.
  Deletion stays the likely outcome — this unit is what makes it a
  decision instead of a guess.
- **Measure time-to-publish after a kick.** Cannot separate the kick
  from the tick that was coming anyway, which is the whole difficulty.
- **Count it machine-wide and read the total.** `straggler_ipis` already
  shows why that is not enough: another waiter's kicks land in it, and
  the callback worker is unpinned.
- **Leave the comment.** It has been carried since PR #154 and survived
  two units that edited the function around it. A comment is a record of
  a question, not a plan to answer one.
