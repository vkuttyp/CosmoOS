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
| `kernel/scheduler/sched.c` (watchdog) | `now <= last \|\| now - last < timeout`, with a comment explaining the wrap | the report knew about this one |
| `kernel/scheduler/thread.c` (`thread_dump_all`) | `if (t->state == THREAD_RUNNING && now > t->last_start_ns)` | the report did not |
| `kernel/core/lockup.c` | one of its three sites | the report counted two |

Three independent reinventions of one rule is the argument for the rule
existing, made better than the report made it. Both survivors are now
the saturating subtraction and their guards are gone; the watchdog keeps
its comment, rewritten to say the test is now written once for the tree.

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

**The sweep, by category.** Counts are `grep -c` on the tree, not a
tally kept by hand:

| category | count | disposition |
| --- | --- | --- |
| a local `t0`, with a sleep or a blocking call between the two reads | 43 | `clock_since_ns(t0)` — foreign by the rule above |
| a stamp in shared state, read with a `now` already in hand | 15 | `clock_delta_ns(now, stamp)` |
| userland, the same shape through the `SYS_clock_ns` syscall | 4 | new `cosmo_clock_since_ns` in `libc/include/cosmo/syscall.h` |
| genuinely local | 1 | left a plain subtraction, with a comment saying why |
| not an elapsed time at all | 4 | left alone |
| the host's own clock | 2 | out of scope |

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
architecture, every online pair:

| | worst reading outside its bracket | widest bracket | advertised bound |
| --- | --- | --- | --- |
| x86-64 (TCG) | **0 ns** | 269–345 µs | 0 ns |
| AArch64 (TCG) | **0 ns** | 89 µs | 0 ns |

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

Not yet run: this report is the plan. The implementation pull request
fills this section.
