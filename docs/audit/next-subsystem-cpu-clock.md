# NEXT SUBSYSTEM — two timestamps and no rule about subtracting them

Date: 2026-09-16. Tree: `main` at 1818b44 (after PR #154, the lifetime
windows). Chosen from `docs/audit/2026-09-deferred-work-inventory.md`
§3.

**Subsystem: a clock two CPUs can compare, and a stated rule about which
timestamps may be subtracted from which.**

This report closes the inventory's §3 row that reads "no CPU feature
framework (`arch_cpu_has`); no errata table; **invariant TSC detected
but unused, so cross-CPU timestamps are unsynchronised**", in its third
clause. The first two are a framework and a table and are a different
unit; this one is the clause with consequences in shipping code today.

## Problem

**`clock_now_ns()` is a per-CPU clock on x86-64 that everything treats
as a machine-wide one.**

`arch_clock_read()` on x86-64 is the raw time-stamp counter
(`kernel/arch/x86_64/timer.c:116-119`):

```c
uint64_t arch_clock_read(void)
{
    return rdtsc_ordered();
}
```

and `clock_now_ns` converts it against a single machine-wide base
(`kernel/timer/timer.c:29-39`). There is no per-CPU correction anywhere,
and the one fact that would say whether the counter is even usable this
way is **detected and never read**: `g_cpu.has_invariant_tsc` is set in
`kernel/arch/x86_64/cpu.c:89` and appears in no other file in the tree.

On AArch64 the same call reads `cntpct_el0`
(`kernel/arch/aarch64/timer.c:111-115`), which is the system counter —
architecturally one counter for the whole system, not one per PE. **So
this hazard is x86-64's alone**, which is a large part of why nothing
has tripped over it: the machine where it bites is the one whose
timestamps happen to agree under QEMU.

### What subtracts a foreign CPU's timestamp today

The pattern is `now - stamp`, where `now` was read on this CPU and
`stamp` was written by another, and the type is unsigned:

- **The block layer's timeout** (`kernel/block/blk.c:215-219`). `now` is
  read by the timeout thread; `b->issued_ns` was stamped by whichever
  CPU handed the bio to the driver — and `bio->issue_cpu`, stamped on
  the very next line (`:180-181`), records which one, so the structure
  already knows what the arithmetic ignores. If the issuing CPU's
  counter runs *ahead*, `now - b->issued_ns` underflows to an enormous
  unsigned value, the comparison against `timeout_ns` succeeds, and
  **every in-flight bio on that device is declared timed out at once**.
- **The lockup detector's reports**, in three places rather than the two
  the first draft of this report found: `now - t->last_tick_ns` at
  `kernel/core/lockup.c:196` and `:241`, and `now - s->when_ns` at
  `:176`, where the sample was stamped by the *target* CPU inside its
  own NMI or IPI handler. All three print an age a skew makes wrong and
  an underflow makes absurd, in the one message an operator reads when
  the machine is already in trouble. `kernel/core/lockuptest.c:186`
  computes the same age and is swept with them.

  That the first pass over this file found two of the three is the
  reason the migration plan below ends with a grep rather than a
  reading: a sweep that depends on having read carefully enough is not a
  sweep.
- **The scheduler's dump** (`kernel/scheduler/sched.c:438`) — and this
  one is the evidence that the tree half-knows:

  ```c
  (unsigned long long)(pc && now > pc->last_tick_ns ? (now - pc->last_tick_ns) / 1000000 : 0),
  ```

  The subtraction is guarded, in that one place, by a comparison that
  exists for no other reason. Somebody saw the underflow, defended
  against it where they were standing, and there was no rule to
  generalise.

### What this unit's predecessor merged an hour ago

`blk-unregister-drain` (PR #154) asserts an *order* between two
timestamps: `g_test_left_ns`, stamped by a submitter parked on another
CPU, and `g_test_unreg_ns`, stamped by `blk_unregister` on the test's
CPU. On a machine with skewed counters that assertion is unsound in both
directions — it can pass while the order was wrong and fail while it was
right. It passes today because the counters agree today. A test that
depends on an unstated property is a test that will be debugged twice.

## Current implementation

There is none to describe for the correction, which is the point. What
exists:

- `arch_clock_read()` per architecture, with no contract about whether
  two CPUs' readings are comparable — `docs/kernel/arch/design.md`
  documents what it returns, not whose clock it is.
- `g_cpu.has_invariant_tsc`, detected at `cpu.c:89`, read nowhere. On a
  CPU without it the TSC stops being a clock at all under frequency
  scaling or deep C-states, and nothing notices.
- One guarded subtraction, in the scheduler's dump, and several
  unguarded ones.
- A boot line that names the clock and its rate
  (`timer.c:292`) and says nothing about its scope.

## Why it matters

**Correctness, and the failure is not graceful.** An unsigned underflow
in the block timeout does not produce a slightly wrong number; it
produces an immediate, total, spurious timeout of a device's in-flight
I/O. The recovery path that follows is the one the tree exercises least.

**Diagnosis.** The lockup detector exists to tell an operator what a
stuck machine was doing, and it reports a cross-CPU age. A detector that
prints nonsense in the case it was built for is worse than one that
prints nothing, because it sends the reader somewhere.

**The rule is missing, not just the correction.** Even on a machine
whose counters are perfectly synchronised, nothing in the tree says that
subtracting another CPU's timestamp is allowed. The next such
subtraction will be written the same way, and the one after that.

**And it is the last unstated assumption of its kind that the tree's own
audit names.** The two neighbouring clauses — a CPU feature framework
and an errata table — are features for later. This clause is a defect
reachable today, which the inventory's ordering rule puts first.

## Design

### What is being promised

One sentence, and the rest of the design serves it: **any two values
returned by `clock_now_ns()` on any two CPUs may be subtracted, and the
result is the elapsed time between them to within a bound the boot
measures and reports.**

The bound is the part the first draft of this sentence left out, and
leaving it out made the contract disagree with the design two pages
later: a correction narrows skew, it does not abolish it, so "the
elapsed time" is a promise no implementation can keep and a reader would
be right to hold it to. What can be kept is this, and it is what the
contract, the tests and the boot line all say:

- the difference is the elapsed interval **± the residual skew**;
- the residual skew is **measured, not assumed**, and printed once at
  boot as the worst offset seen — `clock_worst_offset_ns()`, one number,
  which the contract, the boot line and the tests all name;
- the difference is **never negative and never wraps**, which is a
  property rather than a bound and is the one the callers actually
  depend on.

**One number, checked against itself.** The boot measures the residual
skew; the tests measure it again at runtime, by a different method, and
assert that what they see is within what the boot advertised. That
relationship is the point and it is easy to lose: a runtime handshake
bracket is *not* the contract's bound — it is a second observation of
the same quantity, carrying its own round-trip noise — so a test that
asserted only "the bracket held" could pass on a machine whose skew had
drifted past what the boot promised. The assertion is therefore

```
observed apparent offset  <=  clock_worst_offset_ns() + bracket width
```

where the bracket width is the measurement's noise floor and
`clock_worst_offset_ns()` is the advertised accuracy. If the left side
exceeds the right, either the correction has drifted or the boot's
number was optimistic, and both are findings.

That third point is why `clock_since_ns` exists and why it lands first:
an interval that is wrong by a microsecond is a measurement, and an
interval that is wrong by 584 years is an outage.

### Establishing it, per architecture

**AArch64: already true, and asserted rather than assumed.**
`cntpct_el0` is the system counter and is common to every PE. The unit
does not change it; it adds the test that says so, because "no skew"
that nobody measures is a belief.

**x86-64: measured, corrected, or refused.**

1. **Is the counter a clock at all?** `has_invariant_tsc` finally gets
   read. Without it the TSC varies with frequency and halts in deep
   C-states, so it is not a time source: the boot says so plainly and
   the clock falls back to the platform timer the calibration already
   uses. (Which timer, and whether that fallback is acceptable for the
   tick, is settled in step 1 of the migration rather than assumed
   here.)
2. **Measure the offset of every application processor against CPU 0**
   at bring-up, where the two CPUs are already synchronising and no
   other work is running. The classic three-read exchange — CPU 0 reads,
   the AP reads, CPU 0 reads again — bounds the AP's offset by the
   round trip and takes the midpoint; repeated a few times, the best
   (narrowest) bound wins.
3. **Apply it in `arch_clock_read()`**, as a per-CPU addend. One load
   from the percpu block on a path that is already reading a counter and
   doing a 128-bit multiply.
4. **Report it.** The boot line gains the worst offset observed, so a
   machine with real skew says so once, in a line somebody keeps.

### The rule, and making it enforceable

A correction narrows the skew; it does not make it zero, and a
correction that is subtly wrong is worse than none. So the second half
of the unit is a subtraction that cannot underflow:

```c
/* Elapsed time since `stamp`, which may have been taken on another CPU.
 * Saturates at zero rather than wrapping: a stamp that appears to be in
 * the future is residual skew, not an interval of 584 years. */
uint64_t clock_since_ns(uint64_t stamp);
```

and every site that subtracts a foreign stamp uses it: the block
timeout, both lockup reports, the scheduler's dump — which stops being
the one place with a hand-rolled guard and becomes one of several with
the same one.

**This is the half that matters most**, and the report says so plainly.
The correction removes the skew a machine has; the helper removes the
*class* of failure, including on the machine whose correction is wrong,
including on hardware this project has never run on, and including for
the next subtraction somebody writes.

### What it does not do

- It does not build `arch_cpu_has` or an errata table. Those are the
  row's other two clauses and a different unit; this one reads a single
  feature bit it already has.
- It does not make the TSC a *wall* clock or touch `clock_realtime_ns`.
- It does not attempt sub-microsecond synchronisation. The claim is that
  subtraction is sound and the failure mode is bounded, not that the
  counters agree to the nanosecond — and the boot line reports what was
  actually achieved rather than a target.
- It does not renumber or re-scope `bio->issue_cpu`, which stays what it
  is (a locality counter). That it sits beside the timestamp is
  evidence, not an interface.

### The §70 gate

*Ownership and lifetime.* The per-CPU offset lives in the percpu block,
written once at bring-up before the CPU runs anything else, read on
every clock read thereafter. No allocation.

*Concurrency.* The offset is written by its own CPU during bring-up and
read by that CPU only; nothing else touches it. The measurement itself
runs while the AP is otherwise idle, which is the one moment it can be
taken without a lock.

*Memory.* One `int64_t` per CPU.

*Error handling.* A machine without an invariant TSC is a machine whose
TSC is not a clock: the boot says so and uses the fallback rather than
producing plausible wrong numbers. A measurement that cannot be narrowed
below a threshold is reported and the offset is left at zero — the
saturating helper is what keeps that safe, which is why it is not
optional.

*Security.* No new interface and no userland surface. `clock_now_ns` is
already readable through existing calls and its value does not change in
kind.

*Performance.* One add on the clock path, which already does a 128-bit
multiply and a shift. The measurement happens once per CPU at boot and
is bounded by a few round trips. The boot's AP bring-up already costs
10 ms per CPU (an inventory row of its own), so this is not the thing to
measure there.

*Observability.* The boot line gains the clock's scope and the worst
offset. `sysctl` exposes the per-CPU offsets, so an operator on strange
hardware can see what the kernel decided rather than infer it.

*Future extensibility.* The errata table, when it exists, is where "this
CPU's TSC lies" would live; this unit's offset is the mechanism such an
entry would set. Reading one feature bit is not that table, and the
report does not pretend it is a step toward one.

## Affected files

| file | change |
| --- | --- |
| `kernel/arch/x86_64/timer.c` | the per-CPU offset applied in `arch_clock_read`; the fallback when the TSC is not invariant |
| `kernel/arch/x86_64/smp.c` (or where APs are brought up) | the offset measurement, at the moment the AP and CPU 0 are already in step |
| `kernel/arch/x86_64/cpu.c` | `has_invariant_tsc` finally read |
| `kernel/include/kernel/percpu.h` | the offset |
| `kernel/timer/timer.c` | `clock_since_ns`; the boot line's scope and worst offset |
| `kernel/include/kernel/timer.h` | the promise, stated where `clock_now_ns` is declared |
| `kernel/block/blk.c` | the timeout's subtraction |
| `kernel/core/lockup.c` | all **three** ages: the two tick ages and the sample age at `:176`, which the first pass missed |
| `kernel/core/lockuptest.c` | the same age, computed there too |
| `kernel/scheduler/sched.c` | the dump's age, which stops being the only guarded one |
| `kernel/device/devtest.c` | `blk-unregister-drain`'s ordering, which rests on this |
| `kernel/arch/aarch64/timer.c` | nothing expected: the system counter is already common. The test is what says so |
| docs | `docs/kernel/arch/design.md`, `docs/kernel/timer/`, README Status, `docs/README.md`, the inventory row's third clause |

## New APIs

```c
/* kernel/include/kernel/timer.h */

/*
 * Monotonic nanoseconds. **Comparable across CPUs**: two values read on
 * any two CPUs may be subtracted, and the difference is the elapsed time
 * between them to within the residual skew the boot measured and
 * printed. Never negative, never wrapped -- see clock_since_ns for a
 * stamp that may be foreign.
 *
 * On x86-64 that is a per-CPU offset applied to the TSC; on AArch64 the
 * system counter is common to every PE, so the offset is zero and what
 * remains is the cost of two reads at two instants.
 */
uint64_t clock_now_ns(void);

/* Elapsed time since `stamp`, saturating at zero. For a stamp that may
 * have been taken on another CPU: residual skew makes it look like the
 * future, and an unsigned subtraction turns that into 584 years. */
uint64_t clock_since_ns(uint64_t stamp);

/*
 * The worst residual skew the boot measured: the contract's bound, the
 * boot line's number and what the cross-CPU tests check themselves
 * against. One quantity with one name, so that a test cannot quietly
 * assert something narrower than what was advertised.
 */
int64_t clock_worst_offset_ns(void);
```

## Migration plan

1. **`clock_since_ns` and the sweep**, before any correction exists.
   The saturating helper removes the failure class on today's kernel,
   and doing it first means the correction lands on a tree that is
   already safe if the correction is wrong. Its test is the block
   timeout with an injected skew.
2. **The AArch64 assertion**: the test that says the system counter is
   common, which passes before anything changes and is the control for
   step 4.
3. **`has_invariant_tsc` read**, the boot line, and the fallback. No
   offset yet; the machine either has a usable TSC or says it does not.
4. **The measurement and the offset**, applied in `arch_clock_read`.
5. **The ordering test's dependency made explicit**:
   `blk-unregister-drain` gains a line saying which property it rests
   on, now that the property is stated somewhere.
6. **The sweep, checked by grep rather than by reading.** Every
   `- .*_ns` subtraction in the tree, listed, and each one marked local
   or foreign with the reason. The first pass over `lockup.c` by reading
   found two of its three sites; a sweep that depends on having read
   carefully enough is not a sweep.
7. Docs, README Status, the inventory row's third clause struck, as-built.

Each step boots both architectures; step 1 and step 4 run
`make BUILD=release`; step 4 runs the 1-CPU and 8-CPU matrix by hand,
because an offset measured per CPU is exactly the thing a CPU count
changes.

## Tests

| test | what it asserts | bug-proof (what makes it fail for the stated reason) |
| --- | --- | --- |
| `clock-cross-cpu` | **the promise, checked against the number the boot advertised**: a timestamp on CPU A, one on CPU B ordered after it by a handshake, then one on CPU A again; the middle value lies within the bracket the outer two form, and the apparent offset it implies is within `clock_worst_offset_ns()` plus the bracket's own width. Over many rounds and every online pair, reporting the widest apparent offset seen | inject a per-CPU offset wider than the boot's advertised bound: the assertion fails naming the pair, the offset and the bound it exceeded. And the converse: report a boot bound of zero on a machine with skew, and the same assertion fails -- which is what stops the test from agreeing with whatever the boot happened to print |
| `clock-since-saturates` | `clock_since_ns` of a stamp from the future returns 0, and of a stamp in the past returns the interval | make it a plain subtraction: the future stamp returns an age near `UINT64_MAX`, which is the number the block timeout would have compared against |
| `blk-timeout-skew` | with a skew injected so an issuing CPU's clock runs ahead, a bio issued on that CPU is **not** timed out early, and one genuinely overdue still is | revert the timeout's subtraction to `now - issued_ns`: every in-flight bio on the device times out at once, which is the defect this unit is named for |
| `lockup-report-skew` | the lockup report's "last tick N ms ago" for a CPU whose clock runs ahead reads 0 rather than 584 years | the same revert, in `lockup.c`: the operator's one diagnostic prints an absurd number |
| `clock-scope-aarch64` | on AArch64 the reading taken on CPU B falls **inside the bracket** of readings taken on CPU A either side of it, for every online pair, and the boot's advertised bound is **zero** because the system counter is common. Not "the measured offset is zero": two CPUs read a shared, advancing counter at different instants, so the delta is bounded by the round trip and never exactly zero -- an equality there would fail on correct hardware, which is how the first draft of this row was wrong | apply a fake offset larger than the bracket: the middle reading falls outside and the test names the pair and the offset |
| `clock-invariant-gate` | a machine whose TSC is not invariant does not use it as the clock, and says so in the boot line | pretend the bit is set when it is not: the boot claims a TSC clock on a machine that cannot keep one |
| `blk-unregister-drain` (existing) | unchanged, with a comment naming the property it depends on | — |

**Vacuity, named in advance.** `clock-cross-cpu` and
`clock-scope-aarch64` both pass trivially on a machine with no skew,
which is every machine this project currently boots. That is why each
has an *injection* bug-proof rather than only a claim: the test is
evidence about the mechanism only if it fails when the mechanism is
broken, and on QEMU nothing is broken. The injection is `CONFIG_DEBUG`
only.

**And the honest limit**: no machine available to this project has
skewed TSCs, so the correction's *arithmetic* is tested by injection and
its *effect on real hardware* is not tested at all. The report says so
here rather than in the as-built, because it is a limit of the
environment and not a discovery — the same shape as the VMX backend,
which the inventory already records as never executed.

## Benchmarks

- **The clock path**, before and after, in cycles per `clock_now_ns`:
  one add against a 128-bit multiply and a shift. The claim is that it
  is not measurable; the benchmark is what makes that a measurement.
- **AP bring-up**, before and after, since the measurement happens
  there. Against the 10 ms per CPU the boot already spends.
- **The worst offset measured** on each machine this boots on, which is
  the number the whole unit is about and which nobody has ever printed.

## Risks

- **A wrong correction is worse than none**, because it moves timestamps
  that were already fine. The mitigation is the order of the migration:
  the saturating helper lands first and is not conditional on the
  correction being right, and the AArch64 path takes no offset at all.
- **The measurement is a round-trip estimate**, and a round trip on a
  busy machine is not the same as one at bring-up. It is taken at
  bring-up for exactly that reason, and the narrowest of several
  attempts is kept rather than the last.
- **`has_invariant_tsc` false on a machine that works fine today.** If
  the fallback clock is worse than the TSC that has been serving, the
  gate makes things worse. Step 3 is separate from step 4 so this can be
  measured on its own, and the fallback is the timer the calibration
  already trusts.
- **Nothing here is exercised by real skew.** See the honest limit
  above. The injection proves the arithmetic; only hardware proves the
  measurement, and this project has none.

## Alternatives considered

- **Only fix the subtractions, and never correct the clock.** Tempting,
  cheap, and half the answer: the saturating helper turns a catastrophic
  timeout into a wrong-but-bounded number, and leaves every interval
  measured across CPUs quietly wrong. It is step 1 of this plan rather
  than a substitute for it.
- **Make `clock_now_ns` read a shared source** (HPET, or the APIC
  timer's count). Correct by construction and far slower: a memory-mapped
  read on a path taken in every log line, every scheduler tick and every
  block completion. The TSC exists to avoid exactly that.
- **Forbid cross-CPU subtraction instead of enabling it.** Consistent,
  and it would mean the block layer cannot time out a bio issued
  elsewhere, and the lockup detector cannot say how long a stuck CPU has
  been stuck. Those are the two features the rule would remove; both are
  wanted.
- **Trust QEMU.** The counters agree on QEMU, which is why nothing has
  failed. This project's own inventory lists a real-hardware test matrix
  as unbuilt, and "correct on the emulator" is the property that row
  exists to distrust.

### As built

Not yet built: this report is the plan. The implementation pull request
fills this section.

### As run

Not yet run: this report is the plan. The implementation pull request
fills this section.
