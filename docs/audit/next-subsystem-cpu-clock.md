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
|observed apparent offset|  <=  clock_worst_offset_ns() + bracket width
```

where the bracket width is the measurement's noise floor and
`clock_worst_offset_ns()` is the advertised accuracy, **both
magnitudes**. If the left side exceeds the right, either the correction
has drifted or the boot's number was optimistic, and both are findings.

**Magnitudes, and the bars are load-bearing.** An offset has a sign: a
CPU can run ahead of another or behind it, and a correction can
overshoot in either direction. A one-sided `observed <= bound` is
satisfied by *every* negative offset however large, so a CPU running
behind by a second would pass — and an injection bug-proof that happened
to inject a negative offset would not fail, which is worse, because it
would certify an oracle that does not work. `clock_worst_offset_ns`
returns the worst **magnitude** for the same reason: a signed worst
offset invites exactly this comparison to be written again.

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
TSC is not a clock: the boot says so ~~and uses the fallback~~ rather
than producing plausible wrong numbers. (**As built there is no
fallback** — see step 3 in the as-built. The kernel keeps the TSC, still
monotonic per CPU, and withdraws the cross-CPU claim instead.) A measurement that cannot be narrowed
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
| `kernel/arch/x86_64/timer.c` | the per-CPU offset applied in `clock_now_ns`; ~~the fallback when the TSC is not invariant~~ (no fallback exists: `arch_clock_is_common` reports the bit and the kernel withdraws the claim) |
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
 * The worst residual skew the boot measured, as a **magnitude**: the
 * contract's bound, the boot line's number and what the cross-CPU tests
 * check themselves against. One quantity with one name, so that a test
 * cannot quietly assert something narrower than what was advertised --
 * and unsigned, so that a comparison against it cannot quietly be
 * one-sided while an offset the other way sails through.
 */
uint64_t clock_worst_offset_ns(void);
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
3. **`has_invariant_tsc` read**, the boot line, and ~~the fallback~~ the
   withdrawal of the cross-CPU claim (there is no fallback to fall back
   to). No offset yet; the machine either has a usable TSC or says it
   does not.
4. **The measurement and the offset.** ~~applied in `arch_clock_read`~~ —
   applied in `clock_now_ns`, and **the measurement runs unconditionally
   while only the correction is gated**, because gating the measurement
   too would leave it dead on every machine this project has.
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
| `clock-cross-cpu` | **the promise, checked against the number the boot advertised**: a timestamp on CPU A, one on CPU B ordered after it by a handshake, then one on CPU A again; the middle value lies within the bracket the outer two form, and the **magnitude** of the apparent offset it implies is within `clock_worst_offset_ns()` plus the bracket's own width. Over many rounds and every online pair, reporting the largest magnitude seen | inject a per-CPU offset wider than the advertised bound **in each direction, one run ahead and one behind**: both must fail, naming the pair, the offset and the bound. A one-sided check passes the negative case, and an injection that happened to be negative would then certify an oracle that does not work. And the converse: advertise a bound of zero on a machine with skew and the assertion must fail, which stops the test agreeing with whatever the boot printed |
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
  measured on its own. ~~and the fallback is the timer the calibration
  already trusts.~~ **This turned out to be wrong, and it is the risk
  that actually materialised**: there is no fallback. The calibration
  drives PIT channel 2 as a one-shot gate, which is not a free-running
  counter, and this tree has no HPET driver. The kernel keeps the TSC --
  still monotonic on one CPU, which is what most callers need -- and
  gives up the cross-CPU claim instead. QEMU's x86-64 TCG turned out to
  be exactly such a machine, so this is the shipping configuration
  rather than a hypothetical. See "Step 3" in the as-built.
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

#### Step 6, the sweep — run first, because it changed the plan

The plan put the sweep at step 6, as a re-check of step 1. Run as a grep
instead of as a re-reading, it stopped being a re-check: it found sites
the reading pass had not reached, in files the report never named, and
one of its findings changed the shape of the API.

**What the grep found that reading had not.** The report expected the
block timeout, the two lockup reports and the scheduler's dump — four
sites, and it described the scheduler's as "the one place with a
hand-rolled guard". There were **three** hand-rolled guards, written
independently, each for the same reason and none referring to the
others:

| site | the guard it had grown | did the report know? |
| --- | --- | --- |
| `kernel/scheduler/sched.c` (`sched_dump`) | `pc && now > pc->last_tick_ns ? (now - pc->last_tick_ns) / 1000000 : 0` | yes — this is the one it named |
| `kernel/scheduler/sched.c` (the watchdog) | `now <= last \|\| now - last < timeout`, with a comment explaining the wrap | no |
| `kernel/scheduler/thread.c` (`thread_dump_all`) | `if (t->state == THREAD_RUNNING && now > t->last_start_ns)` | no |

Three independent reinventions of one rule is the argument for the rule
existing, made better than the report made it. All three are the
saturating subtraction now and their guards are gone; the watchdog keeps
its comment, rewritten to say the test is written once for the tree.

**Separately**, and not a guard: `kernel/core/lockup.c` had three
unguarded sites and the first pass over it *by reading* found two. That
is the miss the report predicted when it put a grep in the plan — a
different failure from the reinvented guards above, and the two should
not be run together.

**What the grep found that the API did not cover.** Several sites hold a
`now` they compare *many* stamps against: the block timeout scans a
whole in-flight list, `sched_dump` prints a line per CPU. Rewriting
those as `clock_since_ns(stamp)` re-reads the clock per item — a clock
read per iteration, one of them inside a spinlock, against a `now` that
moves underneath the comparison, so two lines of one dump would be ages
against two different instants. That is a regression, and step 1 had
already written it before the sweep caught it. So the helper comes in
two forms:

```c
uint64_t clock_since_ns(uint64_t stamp);                  /* fresh read */
static inline uint64_t clock_delta_ns(uint64_t now, uint64_t stamp);  /* a `now` in hand */
```

with `clock_since_ns` defined as `clock_delta_ns(clock_now_ns(), stamp)`,
so there is one saturating subtraction and two ways to reach it.

**The classification rule, which is not the one the report implied.**
The report speaks of stamps "taken on another CPU", which sounds like a
property you can read off the code: shared state is foreign, a local
variable is local. It is not. A thread that sleeps between two clock
reads can wake on a different CPU, so

```c
uint64_t t0 = clock_now_ns();
thread_sleep_ms(500);
uint64_t dt = clock_now_ns() - t0;   /* t0 is a foreign stamp */
```

is a cross-CPU subtraction with no shared state in sight. That is the
shape of nearly every timing assertion in the test suite. The rule the
sweep actually used:

> A stamp is **foreign** iff the CPU that wrote it may differ from the
> CPU that reads it — which for a local variable means iff the thread
> can be descheduled between the two reads.

**Correction, added by the thread-migration unit
(`docs/audit/next-subsystem-thread-migration.md`).** The worked example
above is false for the kernel this unit shipped into. A thread is
assigned a CPU once, at `sched_enqueue_new`, and nothing ever moves it:
`sched_wake` returns it to `g_rqs[t->cpu]`, and the only other writes to
`t->cpu` are the idle and boot threads at init. A sleeping thread wakes
where it slept, so the 42 sites in the first row of the table below were
**same-CPU at the time of this sweep**.

The conversions stand — a saturating subtraction costs a compare and is
correct whether or not the stamp is foreign — and they become *necessary*
the moment migration lands, which is why that unit re-runs this sweep as
a gate rather than trusting it. But the rule as stated described a
scheduler this kernel did not have, and saying so here is cheaper than
letting the next reader infer a migration that does not happen.

**The sweep, by category.** Counts are `grep -c` on the tree, not a
tally kept by hand:

| category | count | disposition |
| --- | --- | --- |
| a local `t0`, with a sleep or a blocking call between the two reads | 42 | `clock_since_ns(t0)` — foreign by the rule above |
| a stamp in shared state, read with a fresh clock read | 1 | `clock_since_ns(stamp)` (cosmofs's writeback interval) |
| a stamp in shared state, read with a `now` already in hand | 19 | `clock_delta_ns(now, stamp)` |
| userland, the same shape through the `SYS_clock_ns` syscall | 4 | new `cosmo_clock_since_ns` in `libc/include/cosmo/syscall.h` |
| genuinely local | 1 | left a plain subtraction, with a comment saying why |
| not an elapsed time at all | 4 | left alone |
| the host's own clock | 2 | out of scope |

66 sites changed in all. The counts come from the diff against `main`
(`git diff main | grep '^-'` over the two subtraction shapes), not from a
tally kept while editing — an earlier draft of this table said 43 and 15,
having counted what one script reported rather than what landed.

The one genuinely local site is `kernel/timer/timer.c`'s tick cost: both
reads are this CPU's, inside one tick, with interrupts disabled between
them. It keeps the plain subtraction **on purpose** — saturating there
would hide a counter that went backwards on a single CPU, which is a
fault in the time source rather than the skew this tree tolerates.

The four that are not elapsed times: the realtime offset
(`epoch - clock_now_ns()`, a signed base that is meant to go negative),
`g_keep_idle_ns - idle` and the SRTT deviation in `tcp.c` (both a
duration minus a duration, already guarded), and the round-robin slice
decrement. Saturating any of them would be wrong, not safer.

**Userland was in scope and the report had not noticed.** Four sites in
`init.c` and `thrtest.c` take a `t0` from `cosmo_clock_ns()`, sleep, and
subtract — the same defect, one privilege level down. They are fixed by
one inline helper rather than left as a documented hole; userland
inherits step 4's correction for free, because the syscall reads the
same clock the kernel does.

#### Step 5 — the comment would have had to describe a bug

The plan: "`blk-unregister-drain` gains a line saying which property it
rests on, now that the property is stated somewhere." Writing that line
showed the property does not hold.

The test asserts an order between two events — a parked submitter
leaving the driver, and `blk_unregister` returning — and the two happen
on **different CPUs**, the submitter pinned away from the unregister on
purpose. `blk_test_drain_ordered()` compared two `clock_now_ns()`
stamps. Comparing two CPUs' readings is exactly what this kernel stopped
promising three steps earlier: on x86-64 here `clock_is_common()` is
false, so the assertion rested on a guarantee the kernel declines to
give, and passed only because QEMU's counters agree.

So the step is a fix rather than a comment. The two stamps are positions
in an atomic sequence now: the read-modify-write puts the events in a
total order by itself, on any machine, however the counters behave — and
an order is all the assertion ever wanted. The comment still goes in,
and now it describes something true.

This is the unit finding a live dependency on the property it was
defining, which is the best argument available that the property was
worth defining.

**And the bug-proof for it fails to fail, which is the finding.** Put
`blk_test_drain_ordered` back on two `clock_now_ns()` readings and
`blk-unregister-drain` still passes — every test green, no failure. That is not
a weak proof, it is the point: QEMU's counters agree, so the defect
cannot be observed on any machine this project runs, and no amount of
running the test suite would ever have found it. It was found by writing
down what the test assumed and checking whether the kernel promised it.
The other two proofs in this step do fail as expected — the lockup
report at `age == 0`, and the measured bound at "finer than the counter
can express".

#### Step 4 — the measurement runs, the correction does not

Step 3's finding forced a change of shape here. If the measurement were
gated on the invariant-TSC bit as the report implies, it would never run
on any machine this project has: x86-64 TCG refuses the bit, and AArch64
has nothing to measure. The whole step would be dead code everywhere.

So the **measurement is unconditional and the correction is what the
gate governs**. Every boot with a per-CPU counter measures each AP
against CPU 0 and prints what it found, whether or not it is allowed to
act on it — which is the report's own step-4 bullet 4 ("the boot line
gains the worst offset observed") applied more widely than the report
applied it. On a machine that may not be trusted, the number is still
the most informative line that boot can print about its own timekeeping.

Three details the report did not settle:

**`clock_raw_ns()`.** The three-read exchange must read the counter
*without* the correction, or it measures the correction it is producing.
`clock_now_ns` applies the addend; `clock_raw_ns` does not, and nothing
else uses it.

**The bound is the narrowest half-width, not the worst offset.** After
the correction is applied what remains is the uncertainty of the
estimate, not the offset it removed. Advertising the worst offset would
claim a bound the kernel has already corrected away.

**AArch64 measures nothing at all.** A new `arch_clock_is_percpu()` is
false there: the system counter is shared by construction, so a measured
"correction" could only introduce the error it claims to remove, and the
advertised bound stays exactly 0 rather than drifting to whatever a
thread handshake happened to observe.

**A bound of zero that no measurement can justify.** The first run
printed:

```
timer: CPU 1 offset 0 ns +-0 ns over 1000 exchanges
timer: measured 3 CPU offsets against CPU 0: worst 0 ns, uncertainty +-0 ns
```

`±0 ns` is not a measurement. It means the narrowest bracket had width
zero — the counter did not advance across a cross-CPU handshake that
certainly took real time, TCG running a vCPU in long translated blocks
so a whole exchange can land between two counter values. Had the gate
been open, that would have advertised a bound of 0 ns on the strength of
a degenerate reading. An offset cannot be known more precisely than the
counter can express, so the bound is floored at one tick of the counter,
and `clock-offset-bound` keeps the three cases apart permanently:

| advertised bound | legitimate only when |
| --- | --- |
| `0` | nothing was measured — the counter is shared by construction, so zero is *exact* |
| `CLOCK_OFFSET_UNBOUNDED` | the kernel has declined to promise at all |
| anything else | at least one tick of the counter's resolution |

AArch64's zero is honest under the first row; x86-64's would have been a
promise under none of them.

**What ran.** x86-64: 3 CPUs measured against CPU 0 over 1000 exchanges
each, worst offset 0–500 ns and uncertainty ±2–500 ns across runs
(it varies with scheduling luck), counter resolution 2 ns, correction
**not applied**. AArch64: nothing measured, bound 0, exact.

**What did not run, and cannot here.** The applied correction. No
machine available to this project both has a per-CPU counter and
advertises it as invariant, so `g_apply_offset` is false on every boot
and the corrected path in `clock_now_ns` is never taken. The arithmetic
is exercised by `clock-skew-detected`'s injection; its effect on real
hardware is untested, and this is a stronger statement than the report's
"tested by injection and not on real hardware" — the path does not
execute at all. The same shape as the VMX backend the inventory already
records as never executed.

#### Step 3 — the gate fired on the first machine it met

`has_invariant_tsc` has been detected in `cpu.c` since this kernel had an
x86 port and read by nothing. Reading it produced a result the report did
not expect:

```
[WARN] timer: tsc is not comparable across CPUs: the TSC is not invariant
       (CPUID 0x80000007 EDX[8] clear): it varies with core frequency and
       halts in deep C-states
[WARN] timer: timestamps stay monotonic per CPU; a difference between two
       CPUs' readings is not an interval
```

**QEMU's x86-64 TCG does not advertise an invariant TSC.** Not a
misconfiguration: `-cpu qemu64,...,+invtsc` is refused outright —

```
qemu-system-x86_64: warning: TCG doesn't support requested feature:
  CPUID[eax=80000007h].EDX.invtsc [bit 8]
```

— so the bit cannot be turned on for the boot tests, and the only
x86-64 machine this project runs on is one where the kernel must decline
to promise. AArch64 is unaffected: the system counter is common by
architecture and its boot line says so.

This is the gate doing its job on its first outing, and it is worth
sitting with: the counters on this machine demonstrably *do* agree —
step 2 measured 0 ns outside the bracket over 2400 handshakes — and the
kernel still refuses to promise, because the promise is about the
hardware's contract and not about what happens to work today. A kernel
that subtracted these timestamps would be right on this emulator and
wrong on the first laptop it met.

**The fallback in the report does not exist.** The design said an
unusable TSC should fall back to "the platform timer the calibration
already uses". That timer is PIT channel 2 driven as a one-shot gate to
count a fixed interval; it is not a free-running counter, and this tree
has no HPET driver. There is nowhere to fall back to. So the kernel
keeps using the TSC — it is still monotonic on one CPU, which is what
most callers need — and gives up the cross-CPU claim instead:
`clock_worst_offset_ns()` returns `CLOCK_OFFSET_UNBOUNDED` and
`clock_is_common()` returns false. That is a different statement from a
large measured offset, and the two are deliberately not spelled the same
way.

`clock-cross-cpu` therefore **skips on x86-64**: it checks the machine
against its own advertised bound, and there is no promise on that machine
to check, so asserting anything would be inventing one.

`clock-skew-detected` does **not** skip, and an earlier version of this
implementation had it skipping too. That was tidy and wrong: it is the
test that makes the cross-CPU oracles non-vacuous, so gating it on the
promise left x86-64 — the architecture this entire unit is about —
with no cross-CPU coverage at all. The mechanism it tests (is a reading
outside its bracket detected, and is it weighed against the advertised
bound) does not depend on the machine promising anything, so it sets the
bound itself and restores it afterwards. It runs on both architectures.

**What `clock-invariant-gate` does and does not test.** It cannot test
the CPUID read: on x86-64 here the answer is always false and on AArch64
always true, so `arch_clock_is_common` cannot be made to change its mind.
What it tests is everything downstream of the answer, which is the part
that can rot silently while a one-line bit read keeps working — that a
"no" empties the advertised bound rather than leaving a stale number,
that `clock_is_common` reports it, and that `clock-cross-cpu` stands
down rather than asserting against `UINT64_MAX`. It runs the cross-CPU
test for real inside the forced-shut window, because a version of that
test which asserted against an unbounded bound would pass, and pass
meaninglessly.

#### Step 2

The bracket: A reads, hands a turn to B, B reads, hands it back, A reads
again. The handshake orders the three reads in real time, so on a clock
common to both CPUs the middle one must land between the outer two
numerically as well. How far it falls outside is the apparent offset,
and it is the only quantity these tests assert on. Both threads are
pinned with `thread_create_on`, every ordered pair of online CPUs is
measured, and a stalled handshake gives up after a second rather than
hanging the boot.

**The measurement, which is the point of the step.** 2400 handshakes per
architecture, every online pair. These numbers were taken **at step 2**,
before step 3 read the invariant-TSC bit; x86-64's advertised bound
became `CLOCK_OFFSET_UNBOUNDED` one step later, and the As-run table at
the end of this document is the final state:

| | worst reading outside its bracket | widest bracket | advertised bound *then* |
| --- | --- | --- | --- |
| x86-64 (TCG) | **0 ns** | 269–345 µs | 0 ns (later: unbounded) |
| AArch64 (TCG) | **0 ns** | 89 µs | 0 ns (unchanged) |

So QEMU's counters agree exactly, which is what the report predicted and
why nothing in this tree has ever failed. The widest bracket is a
scheduling hiccup inside a handshake, not skew.

**Which makes both tests vacuous, so the injection is a test rather than
a script.** The report's bug-proof for these rows was "inject an offset
and watch them fail". Run as a one-off revert that evidence exists once,
in a terminal nobody keeps. `clock-skew-detected` runs in CI instead: it
injects ±2 ms on one CPU through a debug-only per-CPU addend in
`clock_now_ns` and asserts three things in each direction —

1. the injected magnitude shows up as a reading outside the bracket
   (2000000 ns injected, 2000000 ns measured, both directions, both
   architectures);
2. it exceeds what the advertised bound allows, so `clock-cross-cpu`
   would reject it;
3. widening the advertised bound *accepts* the same measurement — which
   is the check that catches an oracle ignoring `clock_worst_offset_ns()`
   altogether. Neither (1) nor (2) would notice that.

Both directions are asserted because a one-sided comparison passes the
negative case, and a bug-proof that happened to be negative would then
certify an oracle that does not work. This is the same defect the
report's own step 3 review found in an earlier draft of the bound.

**The injection is only safe because of step 1.** It makes one CPU's
clock jump 2 ms, which every timestamp subtraction on that CPU then
sees. On the tree as it stood before the sweep, `clock-skew-detected`
would have been a hazard rather than a test — several of those
subtractions would have wrapped.

#### Step 1

`clock_since_ns`, `clock_delta_ns` and `clock_worst_offset_ns` added;
the sweep above applied. Two tests: `blk-timeout-skew` (the block
timeout with an injected skew, via a new `blk_test_set_issue_skew_ns`
hook) and `clock-since-saturates` (the helper alone, in a new
`kernel/timer/clocktest.c`). `blk-timeout-skew`'s second phase is the
control that stops the first from being vacuous: the same device, the
same 200 ms timeout and the same scanner *do* fire once the skew is
gone.

`clock-since-saturates` failed on its first boot, and the way it failed
is worth keeping. It asserted

```c
CHECK(clock_since_ns(now + 1) == 0);
```

which is not a claim about saturation at all: `clock_since_ns` reads the
clock itself, so a stamp one nanosecond ahead has already been overtaken
by the time the call reads it, and the assertion measures how fast the
clock advances between two statements. The margin is a minute now, and
the one-nanosecond boundary is asserted on `clock_delta_ns` instead,
where both operands are chosen and no clock runs between them. Same
family as the two test oracles that measured the allocator in the
snapshot unit: the test was reading a property of the machine where it
meant to read a property of the code.

### As run

**329 self-tests PASS on x86-64 and aarch64, debug and release.** 319 at
the branch point, so ten new: `clock-since-saturates`,
`blk-timeout-skew`, `clock-cross-cpu`, `clock-scope-aarch64`,
`clock-skew-detected`, `clock-invariant-gate`, `clock-offset-bound`,
`lockup-report-skew`, `clock-cost` and `clock-tick-owner`.
`blk-unregister-drain` is the eleventh test this unit touched and the
only existing one it changed.

Four of those ten are not in the report's table. `clock-skew-detected`
is the injection the report described as a bug-proof, made permanent
because the tests it proves pass vacuously on every machine here.
`clock-offset-bound` exists because the measurement's first run produced
a bound no measurement can justify. `clock-cost` is the benchmark the
report asked for, as a test rather than a number in a terminal.
`clock-tick-owner` exists because review would not let the deadline
hazard go, and it was right not to.

**The numbers, from the final run.**

| | x86-64 | AArch64 |
| --- | --- | --- |
| counter | `tsc`, per-CPU | `arch-timer`, one for the system |
| invariant / common | **no** (TCG refuses `+invtsc`) | yes, by architecture |
| offsets measured | 3 APs × 1000 exchanges | none — nothing to measure |
| worst offset seen | 0–500 ns across runs | — |
| measured bound | 2 ns (the counter's resolution — the floor) | — |
| advertised bound | `CLOCK_OFFSET_UNBOUNDED` | 0 ns, exact |
| correction applied | no | not applicable |
| bracket, 2400 handshakes/arch | 0 ns outside, widest 270–345 µs | 0 ns outside, widest 54–89 µs |
| injected ±2 ms detected | 2000000 ns, both directions | 2000000 ns, both directions |
| lockup report, 5 s skew | ages it at 0 ms | — |

**Bug-proofs.** `clock-tick-owner` is proved against *both* reverted
designs, by putting each one back:

| the design put back | what fails |
| --- | --- |
| the owner is never replaced when it stops | `a stalled tick owner is never replaced: every deadline would stall with it` — phase 2 |
| every CPU advances the counter | `the machine-wide tick does not advance at the tick rate` — phase 1's upper bound |

So the test discriminates between the design that works and the two that
did not, rather than merely passing alongside them. That is the property
the first two attempts lacked, and the reason the third has a test at
all.

`lockup-report-skew` with `lockup.c` back on a plain
subtraction: fails at `age == 0`. `clock-offset-bound` with the
resolution floor removed: fails with "a measured bound finer than the
counter can express" — the +-0 ns defect, caught by the test written for
it. `blk-unregister-drain` back on two cross-CPU clock readings:
**passes**, which is the finding rather than a weak proof (see step 5).
`blk-timeout-skew` with the timeout's subtraction
reverted to `now - issued_ns`: fails, having logged "a stamp 5 s ahead
timed out 1 request(s): the subtraction underflowed".
`clock-since-saturates` with `clock_since_ns` made a plain subtraction:
fails at the future-stamp assertion. `clock-cross-cpu` and
`clock-scope-aarch64` are proved by `clock-skew-detected` rather than by
a revert, and that test proves itself in both directions and in the
converse (widen the bound and the same measurement is accepted).

**Benchmarks.** The report asked for the clock path before and after,
"the claim is that it is not measurable; the benchmark is what makes that
a measurement". It is measurable, and the claim was wrong:

| | `clock_now_ns` | `clock_raw_ns` | the correction |
| --- | --- | --- | --- |
| x86-64 | 123 ns | 95 ns | **28 ns** |
| AArch64 | 192 ns | 160 ns | **32 ns** |

About 20%, over 200000 calls each. What that means on real silicon is
*not* measured and the ratio does not carry: under TCG every instruction
is emulated, so a load, a branch and an add cost far more relative to the
counter read than they would on hardware, where the read alone is tens
of cycles. The honest statement is that the correction is measurable
under emulation and its cost on real hardware is unknown — the same
limit as everything else in this unit.

**The branch that turned out not to be about speed.** It was removed
once, on the reasoning that the addends are zero when no correction
applies, so a machine using none could add zero instead of testing a
flag. That was wrong for a reason the benchmark would never have shown:
`arch_cpu_id()` reads the per-CPU block through GS, so indexing the
offsets unconditionally makes *every* `clock_now_ns` depend on percpu
being installed — including the ones an AP takes partway through its own
bring-up. The values would have been right and the load to get them
would not have been safe. It booted on QEMU, which is exactly the
evidence this unit exists to distrust. The flag is back and its comment
now says what it guards.

**Deadlines, which this unit does not fix.** Raised in review and valid.
A deadline is a timestamp:

```c
uint64_t d = clock_now_ns() + delay;
while (clock_now_ns() < d) { ... }          /* may migrate in between */
```

is a cross-CPU comparison whenever the thread can be descheduled, and
saturating subtraction does not help — the comparison is an ordering,
not a difference, so there is nothing to saturate. When
`clock_is_common()` such a wait is wrong by at most the advertised
bound; when it is false it may expire early or late by an unbounded
amount, and `timer_ticks()` is per-CPU, so at the time this was written
the kernel had no cross-CPU time source to offer instead. It has one now
(below).

**Fourteen** non-test sites compute a deadline, and every one is
accounted for here — ten on the safe pair and four that are deliberately
not, each saying so where it sits:

| file | sites | what it waits for | disposition |
| --- | --- | --- | --- |
| `drivers/usb/usb.c`, `xhci.c` ×2, `usb_storage.c` | 4 | hardware to respond | `clock_deadline_ns` / `clock_deadline_passed` |
| `drivers/storage/ahci.c` | 2 | hardware to respond | the same |
| `drivers/virtio/virtio_console.c` | 1 | the device to return its buffers | the same |
| `kernel/module/module.c` | 1 | a module's users to leave | the same |
| `kernel/interrupt/ipi.c`, `kernel/arch/x86_64/mmu.c` | 2 | an IPI acknowledgement | the same |
| `kernel/core/lockup.c` | 2 | a sampled CPU to answer | **raw clock**: a 5 ms window, below the 4 ms tick |
| `kernel/timer/timer.c` (`ndelay`) | 1 | a sub-microsecond busy-wait | **raw clock**: far below a tick, and same-CPU |
| `kernel/timer/timer.c` (`timer_start`) | 1 | a timer's expiry | **raw clock**: armed on and fired from the same CPU's queue |

The block layer's request timeout is not in this table because it is not
a deadline: it is an age compared against a per-device timeout, and it
carries a second age (`bio->scans`) that no clock can distort, because it
is the one whose failure is a hang rather than a spurious timeout.

`kernel/syscall/native.c` mentions the pattern in a comment without being
a site; an earlier draft of this table counted it, which is where
"sixteen" came from.

**A machine-wide tick counter is here, and it is the third design in
this position.** The first two were built and reverted, and both failed
in ways a test would have caught at once, which is why this one ships
with one. Their post-mortem, because it is the useful part: The counter
took the *highest* `pc->ticks` any CPU had reached, on the reasoning that
every CPU ticks at `CONFIG_HZ` so the maximum advances at `CONFIG_HZ`.
But those counters do not share an origin: each starts when its CPU comes
online, so CPU 0 leads every AP by the whole of bring-up. Stop CPU 0
ticking and no AP can advance the global value until it has caught up
seconds later — stalling every deadline on the machine, including the IPI
and TLB-shootdown waits whose entire purpose is to escape a CPU that has
stopped answering. On x86-64 here, where the clock is not common, that is
the shipping path. **A documented limitation was replaced with a silent
stall, which is strictly worse**, and two attempts at this hazard in two
rounds is where to stop rather than try a third.

The other repair is worse, not better: every CPU adding its own delta
makes the counter advance at `CONFIG_HZ` times the CPU count, so
deadlines expire that many times too early.

**What works is a designated timekeeper with handoff.** One CPU advances
the counter, so it runs at `CONFIG_HZ` rather than a multiple of it. A
CPU that is not the owner watches the counter, and if it has not moved
for four of that CPU's own ticks — the owner offline, wedged, or simply
not taking interrupts — claims ownership with a compare-exchange.
Several may notice at once and exactly one wins. So the counter is late
by at most four ticks across a handover and **cannot stop while any CPU
is still ticking**, which is the property the first design lacked.

`clock-tick-owner` is the test, and phase 2 is the one that matters: it
points ownership at a CPU id that will never tick, which a stalled or
offline owner is indistinguishable from, and requires the counter to
resume on its own. It reports which CPU took over. Phase 1 checks the
rate, with an upper bound deliberately tight enough that the
every-CPU-contributes design would fail it. Measured: 50 ticks in 200 ms
on x86-64 and 51 on AArch64 — `CONFIG_HZ` is 250, so 50 is exact — and
with the owner stopped, CPU 3 (x86-64) and CPU 2 (AArch64) took over
within four ticks.

The first shape, kept so it is recognised rather than rediscovered:

```c
/* REVERTED -- do not resurrect. pc->ticks counters do not share an
   origin, so the maximum stalls when its leader stops ticking. */
static uint64_t deadline_now_ns(void)
{
    if (clock_is_common())
        return clock_now_ns();
    uint64_t ticks = __atomic_load_n(&g_global_ticks, __ATOMIC_ACQUIRE);
    return ticks == 0 ? clock_now_ns() : ticks * TICK_NS;
}
```

So `clock_deadline_ns` and `clock_deadline_passed` measure against the
corrected clock where every CPU agrees on it and against the
machine-wide tick where they do not. Both read the same quantity, so
where a deadline is built and where it is tested no longer matters —
**which is what the review asked for across five listings, and it was
right to keep asking.** The inventory row filed for this is closed by
the same commit that filed it.

**And while it existed, the second domain immediately broke the first.**
`timer_start` had been migrated to `clock_deadline_ns` in the mechanical
pass, harmless while that was just `clock_now_ns() + x`. Once it returned
tick-domain values, timers were armed in one domain and compared in the
tick against `clock_now_ns()` in the other — so every timer in the kernel
fired at once, and `preempt`, `sleep` and `completion` returned in
single-digit milliseconds instead of tens. The timer queue is per-CPU and
is armed and fired on the same CPU, so the raw clock was always right for
it. That comment survives the revert, because the two calls still look
interchangeable and the next person deserves the warning.

They do fix something real today. `clock_now_ns() + budget` wraps into
the past for a large budget and expires immediately, and the tree already
knew: `sys_futex_wait` rejects a duration past `INT64_MAX` at its own
call site, with a comment explaining the wrap, and **no other site was
guarded**. One caller had the rule and fifteen did not, which is the
shape this project keeps finding. `clock_deadline_ns` saturates, so
`timer_start` is guarded now whoever calls it; the futex check stays
because a syscall should refuse an impossible duration rather than
silently turn it into "never".

**The saturating fix traded a correctness bug for a liveness one, and
review caught it.** The block timeout's whole purpose is that a stalled
device enters recovery. Saturating the age means a bio issued on a CPU
that runs ahead of the timeout thread reads as zero seconds old for
ever, so on a machine with unbounded skew the request is never timed out
and the device hangs instead — worse than the spurious timeouts the
saturation was added to prevent. The scan now keeps a second age that no
clock can distort: `bio->scans`, written only by the timeout thread,
counting the 500 ms scans that have seen the bio in flight. A request is
overdue when *either* age says so. The walk also no longer stops at the
first request that is not overdue, because with two ages "oldest first"
no longer implies the oldest expires first.

**A partial measurement no longer enables the correction.** Also from
review. `clock_worst_offset_ns()` says two readings taken on *any* two
CPUs differ by at most that much; a CPU whose measurement failed keeps a
zero correction and contributed nothing to the bound, so publishing a
finite bound with one missing states something about that CPU which
nothing established. It is all of them or none now — one failure and the
machine keeps its raw counter and advertises no bound, the same answer
as a counter that is not a clock, for the same reason.

**What did not run.** The applied correction, on any machine. No machine
available to this project both has a per-CPU counter and advertises it as
invariant, so `g_apply_offset` is false on every boot. The arithmetic is
exercised by injection; the path is not taken. This is a stronger
statement than the report's "untested on real hardware" and it is stated
here rather than in the risks, because the report did not know it.

**A flake.** `net-harness` failed once at `nettest.c:929` during a run in
which this host killed a background build for memory, and passed on an
immediate re-run of the same image. Recorded in `docs/testing/flakes.md`
with the memory pressure named as a circumstance and explicitly not as a
cause: a dropped SYN, a slow host process and an unrelated timing window
all look identical from `client_ok == false`.
