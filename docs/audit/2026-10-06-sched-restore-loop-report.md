# Scheduler restore loop: bounded preempt-at-restore

Date: 2026-10-06. Branch `sched-restore-loop` from `main` at `c9292e3d`.
Scope: `docs/plan.md` §4 "Implementation — bounded preempt-at-restore
recursion", under the plan's fourth suggested increment. Origin: the
[raw-pairing report](2026-10-06-lockdep-irq-pairing-report.md), "A latent
scheduler dependency".

## Summary

`schedule()` ended in `arch_irq_restore(s)`. When that restore enabled
interrupts it ran `preempt_point()`, and a reschedule pending at that
moment entered `schedule()` again, from inside the first call's tail. The
stack stayed bounded only because every link compiled to a tail call
(`schedule_internal → arch_irq_restore → arch_irq_restore_hw →
preempt_point → sched_preempt → schedule_internal`), and
`scripts/check-kernel-elf.sh` enforced that per link.

The pass is now `schedule_pass()`. `schedule_internal()` loops while
`preempt_point_due(s)` -- the restore point's predicate asked with
interrupts off, before the restore -- and then restores through
`arch_irq_restore_nopoint(s)`, which has no preemption point. The
predicate, the `preempt = true` trip and the `restore-preempts` count are
unchanged; the second pass runs in the first pass's frame. New invariant
S31: `schedule()` never re-enters itself on one thread's stack. Debug
builds assert it on every entry. The ELF check now verifies the structure
(`schedule_internal` reaches no preempting restore) rather than code
generation, and a new self-test with a probe shows the bound holds under
a forced non-tail link that grows the old structure by 192 bytes a trip.

**The §4 item is complete**: the recursion is gone from the code, the
evidence below supports the bound on both architectures, and no existing
test or budget changed.

## Baseline (unchanged `main` plus a debug instrument)

The first commit on the branch, `f3db569c`, adds only a debug measurement
and leaves the structure as on `main`: per CPU, the deepest stack
`schedule()` is entered on; the "trips" -- restores at `schedule()`'s own
tail that took a reschedule (armed with interrupts off just before the
restore, consumed by `preempt_point`); the deepest entry a trip made; and
the longest *chain*, the trips one thread took in a row, each inside the
last. A chain is how many frames deep the recursion would be if a link
were a call. The numbers print once at the end of a debug boot.

| Boot (`main` + instrument) | Verdict | Trips per CPU | Longest chain | Deepest trip entry |
|---|---|---|---|---|
| x86-64, 1 CPU, #1 | PASS | 1085 | **47** (`idle`) | 9360 B (`kmain`, boot stack) |
| x86-64, 1 CPU, #2 | PASS | 1083 | **60** (`idle`) | 9360 B (`kmain`) |
| x86-64, 1 CPU, #3 | PASS | 1102 | **72** (`idle`) | 9360 B (`kmain`) |
| x86-64, 2 CPUs | PASS | 148 / 75 | 1 / 7 (`idle`) | 4344 B (`cwdtest`) / 1664 B |
| x86-64, 4 CPUs | PASS | 85 / 38 / 37 / 4 | 2 on every CPU (`idle`) | 752 / 368 (`idle`) / 4344 / 368 (`idle`) B |
| AArch64, 2 CPUs | PASS | 40 / 277 | 4 / 3 (`idle`) | 1952 / 5248 B |
| AArch64, 4 CPUs | FAIL (see below) | 26 / 3 / 30 / 119 | 1 / 1 / 2 / **14** (`idle`) | 624 (`idle`) / 576 (`idle`) / 272 / 5200 B |

Reading it: one CPU under the boot's load is where the chain lives -- the
idle thread is resumed with a reschedule pending up to 72 times in a row,
because each thread it switches to wakes or exits and leaves work behind.
On `main` this costs nothing, since a trip's entry is at the same depth as
the call it replaces: the deepest trip entries are ordinary deep callers
(`kmain` on the 64 KiB x86-64 boot stack, `cwdtest` in a path walk), and the idle
thread's own trip entries are 368–624 bytes. More CPUs spread the work and
the chains shorten to 1–14.

**The AArch64 4-CPU baseline failure** was `self-test net-nicbench took
21523 ms (budget 20000 ms)`, every self-test passing. It is the
`net-nicbench` budget family already in `docs/testing/flakes.md` (two
sightings, 2026-09-29 and -30), on a tree whose scheduler is `main`'s, and
the scheduler numbers in that boot are like the others. Not this unit's;
recorded rather than retried away. The interleaved benchmark boots below
re-ran that configuration on `main`.

### Forcing one link to be a call

The same baseline tree, with lockdep's `arch_irq_restore` given the
pre-fix wrapper's shape -- a 160-byte buffer whose address escapes, live
across the hardware restore, so the wrapper *calls* `arch_irq_restore_hw`
(192-byte frame on both architectures, checked in the disassembly) -- and
the one tail-call check that would refuse it removed:

| Boot (`main` + instrument + forced call) | Verdict | Longest chain | Deepest trip entry |
|---|---|---|---|
| x86-64, 1 CPU, #1 | PASS | 68 (`idle`) | **13376 B (`idle`)** |
| x86-64, 1 CPU, #2 | PASS | 69 (`idle`) | **13568 B (`idle`)** |
| x86-64, 1 CPU, #3 | PASS | 70 (`idle`) | **13760 B (`idle`)** |

The idle thread's trip entries went from at most 624 bytes to 13,760 of
its 16,384-byte stack: 192 bytes per level, about 70 levels. None of the
three boots double faulted. The original fault came from the first
raw-pairing wrapper under the same load; its frame size was not recorded,
so whether it was larger than this one is not known. The claim
this supports is the stated one -- the forced call makes the idle stack's
depth proportional to the chain, and the chain reaches 70 -- not a
reproduced fault.

A first version of this control declared `volatile char buf[160]` and
touched two bytes; clang kept only those two bytes and the frame was 48
bytes. The disassembly check caught it (`subq $0x18` where `$0xa8` was
meant), and the buffer now escapes through an empty `asm` operand, as
`ksnprintf`'s did.

## Design

Written first in `docs/kernel/scheduler/design.md` ("The restore loop",
with the per-point state table and the missed-reschedule argument) and
`invariants.md` (S31; S5 and S8 amended). In short:

- **One save, many passes, one restore.** `schedule_internal` saves once,
  runs `schedule_pass()` until `preempt_point_due(s)` is false, and
  restores once. `pc`/`rq` are re-read at the top of each pass, with
  interrupts off, because a thread switched out and back may have moved.
  Interrupts stay masked from the save to the restore on both
  architectures; `preempt_count` is 1 only inside the run-queue lock;
  `irq_depth` is 0 throughout.
- **Same policy.** A trip is a pass with `preempt = true`, as
  `preempt_point → sched_preempt` made it: `THREAD_FLAG_PREEMPTED`, head of
  the queue if the slice is left. `preempt_point_due` increments the same
  per-CPU counter as `preempt_point`, so `restore-preempts` counts the same
  events. The trap-return path enters with interrupts masked in `s`, so it
  is a single pass, as before.
- **No missed reschedule.** Between the predicate and the restore this CPU
  runs only the scheduler's tail, which wakes nothing (the reaper's wake is
  in `sched_finish_switch`, before the predicate). A local interrupt that
  sets `need_resched` is delivered at the restore, and its return is a
  preemption point. A remote `request_resched` stores the flag and sends
  `IPI_RESCHEDULE`; the IPI is pending across the masked interval and is
  taken at the restore. The difference from `main`: a remote store in the
  instant between the enable and the old predicate read was taken by the
  restore point and is now taken by the IPI's return a few instructions
  later.
- **The other callers.** `preempt_enable` and the restores outside the
  scheduler still call `sched_preempt` through `preempt_point`. That is one
  level on the caller's stack: the `schedule()` it enters loops and leaves
  through the no-point restore, so it cannot nest again. The idle loop's
  `schedule()` is the same one level.
- **What changed observably.** Nothing in policy or in what
  `preempt-wake`, `preempt-wake-direct` and `preempt-wake-locked` assert.
  Between two trips interrupts used to be enabled for the instant before
  the nested save; now they stay masked across the trip. The switched-to
  thread re-enables them when it resumes, so a pending interrupt waits one
  pass longer, which is the same interval as any other switch.

### New primitives

- `arch_irq_write_hw(state)`, per architecture: the hardware restore alone
  (x86-64: `sti` when `IF` is set; AArch64: write `DAIF`).
  `arch_irq_restore_hw` is unchanged.
- `arch_irq_restore_nopoint(state)`: with lockdep, the same pairing check
  as `arch_irq_restore`, then `arch_irq_write_hw`; without lockdep, inline.
- `preempt_point_due(s)`, in `kernel/core/percpu.c` beside `preempt_point`.
- `schedule_internal` is `__noinline` so the ELF check finds it by name.

## The tail-call guard

Each of the six `tail_call` checks in `scripts/check-kernel-elf.sh`, decided:

| Check | Decision | Reason |
|---|---|---|
| `schedule_internal → arch_irq_restore` | **replaced by a stronger check** | `schedule_internal` must not reach `arch_irq_restore` at all, by call or by jump |
| `schedule_internal → arch_irq_restore_hw` | **replaced**, same | as above |
| `arch_irq_restore → arch_irq_restore_hw` | **removed** | no longer on a recursive path: a frame there is one level per restore point that preempts, entered at most once per stack |
| `arch_irq_restore_hw → preempt_point` | **removed** | same |
| `preempt_point → sched_preempt` | **removed** | same |
| `sched_preempt → schedule_internal` | **removed** | same; `sched_preempt`'s callers are one level, and `schedule_internal` cannot reach it (checked) |

What the script checks now, failing on any branch or call whose target is
the named function:

- `schedule_internal` (required symbol) reaches none of `arch_irq_restore`,
  `arch_irq_restore_hw`, `preempt_point`, `sched_preempt`;
- `arch_irq_write_hw` (required) does not reach `preempt_point`;
- `arch_irq_restore_nopoint` (present only with lockdep; skipped only when
  the symbol table confirms it absent) reaches none of `arch_irq_restore`,
  `arch_irq_restore_hw`, `preempt_point`.

A missing required symbol or a failed disassembly fails the link, as the
old check did. The removed checks are not silently dropped: the comment in
the script names them and points here. The guard's negative control is
the probe's `guard` mode.

## Tests

**`sched-restore-loop`** (pinned, debug). A partner thread of the test
thread's priority, pinned to the same CPU, yields in a loop. The test sets
its seam -- `test_resume_resched`, read by `schedule_internal` in
self-test builds -- so that each of its next 32 resumptions inside
`schedule()` finds a reschedule pending and its slice spent, and calls
`sched_yield()`; 100 runs, 3,200 trips, each a real switch to the partner
and back. Asserted in order: the deepest stack the test thread entered
`schedule()` on is within 1,024 bytes of the test's own frame; it was
never inside `schedule()`'s interrupts-off body twice; the seam was used
up; at least 3,200 trips were counted and 32 in one call. Measured on the
fix: 144 bytes above the frame on x86-64, 176 on AArch64; nesting 1;
3,200–3,201 trips; 8–15 µs a trip under TCG.

**S31 assertion** (debug, every boot): a per-thread `sched_nest` raised
after the save and lowered before the restore must be zero on entry.

**Negative controls** (`tools/sched-restore-loop-probe.py`; each builds
HEAD in a throwaway worktree with the 192-byte forced call above):

| Mode | Edit | Required | x86-64 | AArch64 |
|---|---|---|---|---|
| `recursive` | the old structure (one pass, then `arch_irq_restore`), S31's assertion and the ELF check removed | the stack-bound `CHECK` fails, at its line | FAIL at line 901: **6288 B** above the frame after 32 trips (192 B each) — PROBE PASS | same, **6288 B** — PROBE PASS |
| `loop` | none beyond the forced call | the test passes | ok, 144 B, 3200 trips — PROBE PASS | ok, 176 B, 3200 trips — PROBE PASS |
| `guard` | the old structure, ELF check kept | the link is refused naming `schedule_internal` | refused — PROBE PASS | refused — PROBE PASS |

## Validation

On the branch (debug unless stated; serial logs copied per boot). The
first release attempt failed to compile on both architectures: release
builds compile `schedtest.c` with `CONFIG_DEBUG=0`, where the test is
skipped and the partner thread's entry was an unused function under
`-Werror`. Fixed in `74266cdd`; the release boots below are after it. The
restore-loop fields were then moved to the end of `struct thread`
(performance, below), and the x86-64 4-CPU (×3), AArch64 1- and 4-CPU
debug boots, both release boots, `analyze` and `host-test` were run
again on that layout and passed.

| Run | x86-64 | AArch64 |
|---|---|---|
| `make host-test` | pass | (host) |
| `make analyze` | clean | clean |
| debug boot, 1 CPU | PASS ×3 (122–140 s), 426 self-tests | PASS |
| debug boot, 2 CPUs | PASS | PASS |
| debug boot, 4 CPUs | PASS | PASS |
| `make test-smp2` | PASS, 426 self-tests | PASS, 426 |
| `make test-chaos` | PASS, 426 | PASS, 426 |
| `make BUILD=release test` | PASS (17–18 s) | PASS (20 s) |
| `tools/sched-restore-loop-probe.py`, all three modes | PROBE PASS | PROBE PASS |

The restore loop under the boot's own load, from the end-of-boot line:

| Boot (fix) | Trips (CPU 0) | Longest chain in one call |
|---|---|---|
| x86-64, 1 CPU ×3 | 4278 / 4291 / 4277 (3,200 are the test's) | 72 / 71 / 69 (`idle`) |
| AArch64, 1 CPU | 4288 | 70 (`idle`) |

The one-CPU chains are the same length as on `main` (47–72) and are now
passes of one frame.

## Performance

Compared against the baseline tree (`main` plus the debug instrument),
debug builds under TCG, from `irqrestore-bench` (a million save/restore
pairs with no reschedule pending), `fpu-bench`'s switch cost without FP
state (two pinned threads yielding), the `preempt-wake*` wake latencies
and the self-test totals. Base and fix were booted alternately at 4 CPUs,
four pairs on x86-64 and two on AArch64; every other boot of each tree is
included too.

| Measure | Base | Fix | Reading |
|---|---|---|---|
| AArch64 save/restore pair | 296, 299, 453, 568 ns | 276, 276, 276, 292, 413, 415, 437 ns | no shift; the same two per-boot modes in both |
| AArch64 switch | 4277, 4299, 6108, 6117 ns | 4141, 4211, 4211, 4251, 5729, 6005, 6542 ns | no shift; same two modes |
| x86-64 save/restore pair (10 base, 12 fix boots) | median 360 ns (nine at 357–376, one at 463) | five at 399–403, seven at 492–525 | **slower**, see below |
| x86-64 switch (same boots) | median 4,430 ns (eight at 4,298–4,885, two at 5,184–5,314) | five at 4,474–4,596, seven at 5,369–5,615 | **slower**, see below |
| `preempt-wake`, `-direct`, `-locked` latency | 112–160, 32–49, 51–60 µs | 119–141, 31–44, 46–69 µs | within noise |
| self-test time, 4-CPU pairs | 106.7–114.0 s | 105.6–113.8 s | within noise |
| one-CPU x86-64 restore-point reschedules (excluding the test's 3,201) | 1083–1102 | 1076–1090 | unchanged |

**AArch64: no measurable change.** **x86-64: an unexplained slowdown on
the debug micro-benchmarks.** The fix's x86-64 boots fall into two
per-boot modes, both above base's usual one: about +40 ns a save/restore
pair and +150 ns a switch in the faster mode, +140 ns and +1,150 ns in the
slower, which most fix boots landed in. What was checked:

- The save/restore path never enters `schedule()`. Its disassembly differs
  from base only in `struct thread` offsets (`arch_irq_save`,
  `irq_restore_track`) and in `preempt_point`, which is *shorter* on the
  fix (base's instrument called out of it on every restore). The hot
  functions sit on the same 4 KiB pages in both kernels, so TCG's
  same-page block chaining does not distinguish them.
- Moving the new fields to the end of `struct thread`, so every existing
  offset is `main`'s, did not remove it: three more x86-64 boots read 401,
  403 and 500 ns a pair against 360 for a base boot run between them. The
  move was kept anyway.
- `fpu-bench`'s switch does go through the loop, which adds per pass an
  out-of-line `preempt_point_due` call and, in debug builds, the S31 and
  depth bookkeeping. That plausibly accounts for the faster mode's 150 ns;
  it does not account for the pair benchmark, which runs none of it.

Release kernels do not run the benchmarks (`SELFTEST=1` with
`BUILD=release` does not link on `main` either), so the release cost was
not measured. The finding is recorded as unexplained, for the
performance item in plan §1/§12, not as within noise.

## Limits and what remains

- **The forced-call baseline did not double fault** in three boots; it
  reached 13,760 of 16,384 bytes. The report states the depth, not a
  reproduction.
- **The ELF check is structural, not semantic.** It catches the restore
  coming back to `schedule_internal`'s tail by name; a new indirect path
  (a function pointer to a preempting restore) would pass it. The S31
  assertion would catch that at run time on the first trip.
- **The x86-64 debug benchmark shift is unexplained** (Performance). It
  is TCG-only evidence and AArch64 shows none, but the cause was not found.
- **Interrupts stay masked across a trip** where `main` opened a window of
  a few instructions. No test measures interrupt latency at this
  granularity; the argument is that the switched-to thread re-enables them.
- **One self-test seam** in `schedule_internal`, compiled only with
  `CONFIG_SELFTEST`; release kernels do not contain it.
- **Per-thread fields**: `sched_nest`, `sched_nest_max`, `sched_chain`,
  `sched_trips`, `sched_depth_max`, `test_resume_resched` are at the end of
  every build's `struct thread` (about 40 bytes; every older field keeps
  its offset), written only in debug or self-test builds, as `bal_pulls`
  and `wake_resched` are.
