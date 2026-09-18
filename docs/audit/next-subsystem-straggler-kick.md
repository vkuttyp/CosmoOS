# NEXT SUBSYSTEM — what the straggler kick is worth

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This report is a design, not
an as-built.

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
   return. Today the two coincide because the handler is empty. An
   early exit when `!need_resched`, which the documented contract would
   permit, would silently turn every straggler kick into a no-op, and
   **no test in the tree would notice.**

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
carry a per-waiter claim — the pattern this unit extends.

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
2. **Attribution in the publish path.** `quiesce_note_quiescent_preemptible`
   — reached from both trap returns and from `idle_main` — tests and
   clears the flag. If it was set, the publish is counted as
   `kick_publishes`. That is the narrowest defensible meaning of "the
   kick worked", and it costs one per-CPU read on a path that already
   runs there.
3. **A per-waiter count.** `quiesce_test_sync_kick_publishes()` mirroring
   `quiesce_test_sync_kicks()`, returned on the stack for the reason the
   existing one is: another waiter's kicks land in the machine-wide
   counter, and the callback worker is unpinned, so neither a global nor
   a per-CPU slot can carry a per-waiter claim.
4. **A deterministic adversary for the positive case.** The helpable
   population is a CPU whose tick keeps landing inside a short disabled
   region, and the inventory calls that "a phase coincidence no
   deterministic test can arrange". That is the claim this unit tests
   rather than accepts: the adversary is built from the mechanism, not
   from a stopwatch — a thread pinned to the target CPU that reads that
   CPU's tick, then phase-locks a short `preempt_disable` region to
   arrive just before each expected tick, so the tick lands inside and
   the kick, at an unrelated phase, lands outside.
5. **The decision rule, written down before the measurement.** If the
   adversary can be built and `kick_publishes` rises, the kick is proved
   and the follow-up unit bounds it. If the adversary cannot be built,
   or `kick_publishes` is identically zero across the suite on both
   architectures, the kick is dead weight and the follow-up deletes it.
   **Both outcomes are a result.** Stating the rule now is what stops
   the measurement from being read to suit whichever answer arrives.

## Affected files

| file | change |
| --- | --- |
| `kernel/include/kernel/ipi.h` | `IPI_QUIESCE_KICK` before `IPI_KIND_COUNT`; the comment says it wants the trap return, not `need_resched` |
| `kernel/interrupt/ipi.c` | handler and table entry; sets the per-CPU flag |
| `kernel/include/kernel/percpu.h` | the flag |
| `kernel/core/quiesce.c` | send the new kind; clear-and-count in the preemptible publish; `kick_publishes` in the stats |
| `kernel/include/kernel/quiesce.h` | `kick_publishes`, `quiesce_test_sync_kick_publishes` |
| `kernel/core/quiescetest.c` | the adversary and the negative control |
| `docs/kernel/quiesce/invariants.md` | **Q19**: what a kick can and cannot do, and what is counted |
| `README.md` | the Status entry |

## New APIs

`IPI_QUIESCE_KICK` (internal), `quiesce_test_sync_kick_publishes(void)`
(debug builds, as `quiesce_test_sync_kicks` is), and one `uint64_t` in
`struct quiesce_stats`. No syscall, no user-visible change.

## Migration plan

One commit. The new IPI kind is additive and both architectures
allocate vectors from the same enum, so nothing else moves. The kick's
behaviour is unchanged by construction — the same interrupt at the same
moment, under a different number — which is what makes the measurement
a measurement rather than a change.

## Tests

| test | asserts |
| --- | --- |
| `quiesce-kick-attributed` | the adversary: a publish attributed to a kick, `kick_publishes` up by at least one for **this waiter** |
| `quiesce-kick-spinner` | the negative control, on `quiesce-straggler`'s shape: kicks are sent and `kick_publishes` does **not** rise, because a CPU with `preempt_count != 0` cannot publish in a trap return |
| `quiesce-kick-ipi-kind` | the kick sends `IPI_QUIESCE_KICK` and not `IPI_RESCHEDULE`, so the scheduler's IPI can change without disabling it |
| existing `quiesce-straggler`, `-system`, `-idle` | unchanged and still passing |

**The bug-proof.** Attribution wired and the *send* disabled: the
adversary must then record zero attributed publishes. A version that
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
  CPU's tick may prove too coarse. That is a result and the report says
  so up front: it moves the decision to "delete or bound" rather than
  leaving it open, which is still strictly better than a comment.
- **It may be flaky if written as a timing assertion.** Mitigated by
  design point 5 and the Tests section: the assertion is a counter
  attributable to this waiter, not a duration, and the negative control
  must stay at zero.
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
