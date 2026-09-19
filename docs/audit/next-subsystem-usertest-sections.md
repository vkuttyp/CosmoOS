# NEXT SUBSYSTEM — the suite behind one line

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it.

**Subsystem: what `process-user` reports about itself.** One `SELFTEST`
line stands for the entire user-mode suite — nine sections, about 540
checks inside them in any one build and 54 more outside any section at
all, plus a process spawn for each tool it drives. When that line is
slow, nothing in the tree says which part of it was slow, and the
harness has twice had to widen a budget it could not aim.

This is the item the verification design already names as the answer it
did not build. `docs/verification/design.md` §6, after listing the two
composite tests and their enlarged budgets:

> the better answer is for a suite to report its sections' timings so a
> slow section is named instead of the whole suite, and that is an
> inventory item

It is inventory §3, "found by the fsctl unit". This report takes it up.

## What is established

**The line is slow and getting slower, and the numbers are current.**

| where | `process-user` |
| --- | --- |
| local x86-64 debug | 3707 ms |
| local aarch64 debug | 3896 ms |
| CI x86-64 (`1ec27af`) | 4791 ms |
| **CI aarch64, protection-capable boot (`a0558b6`)** | **8284 ms** |

The last of those is **over the 8000 ms budget every ordinary test is
held to**. It passes only because `composite_budget_ms` gives this one
line 20 s. The historical figure in the design doc — 7129 ms of 8000,
before the fsctl unit added to it — was not a one-off high-water mark;
CI has since gone past 8000 on a run where nothing in userland changed
at all (`a0558b6` is a documentation-only commit).

**The spread is 2.2× between the same code on a laptop and on CI**, and
no part of the line says whether that is one section or all nine.

**What the suite is, measured rather than described.** `selftest()`
(`userland/init/init.c:3556-3660`) calls nine sections and then runs 54
checks of its own:

| section | lines | checks |
| --- | --- | --- |
| `proc_selftest` | 645 | 235 |
| `net_selftest` | 213 | 87 |
| `fs_selftest` | 206 | 100 |
| `fsctl_selftest` | 200 | 43 |
| `svc_selftest` | 173 | 34 |
| `proc_fs_selftest` | 66 | 24 |
| `priv_selftest` | 35 | 13 |
| `trap_selftest` | 24 | 2 |
| `fpu_selftest` | 12 | 4 |
| *(the body of `selftest()` itself)* | 105 | 54 |

`trap_selftest` and `fpu_selftest` each have two architecture-conditional
definitions and only one of each compiles, which is why the per-build
total is about 540 and not the 546 a naive count of both gives. Line
counts are brace-matched function bodies, not estimates.

**Half the instrument already exists, which is why this unit is small.**
Every section already prints a boundary when it finishes, and they all
reach the log in call order:

```text
usertest: symbolic links ok              <- fs_selftest
usertest: fsctl ok                       <- fsctl_selftest
usertest: sockets ok                     <- net_selftest
usertest: processes ok                   <- proc_selftest
usertest: fpu isolation ok               <- fpu_selftest
usertest: user exceptions ok             <- trap_selftest
usertest: privilege boundary ok          <- priv_selftest
usertest: /proc ok                       <- proc_fs_selftest
usertest: services ok                    <- svc_selftest
usertest: write ok                       <- the unnamed trailing body
```

And `stdout` is **line-buffered** (`libc/src/stdio.c:71`,
`F_WRITE | F_LINEBUF`), so each of those lines leaves the guest when it
is printed rather than at exit.

**Two consequences, and only one of them is a gap.**

1. **A hang is already attributable** — the last boundary printed names
   the section that finished, and the next one is where it stopped. No
   report has ever said so, and the harness does not use it, but the
   information is in the log today. This unit should claim only that it
   makes this explicit and machine-read, not that it invents it.
2. **A *slow* run is not attributable at all.** Nothing measures a
   section, so there is no duration anywhere for any of the ten rows
   above. That is the actual gap, and slowness — not hanging — is what
   has twice forced a budget change.

**The harness reads none of it.** `run_boot_test.py` requires exactly
one line from the suite, `^USERTEST: PASS` (`USERTEST_MARKER`,
line 328), plus `^usertest: umip: enforced$` on the x86-64 guard boot.
The nine boundaries are not parsed, not reported and not in the verdict.

**And the cost of a composite that reports nothing about itself shows up
in the prose about it.** `cosmofs-replay` — the other entry in
`composite_budget_ms` — is documented as mounting and checking **211**
filesystem images in both `docs/verification/design.md:210` and
`docs/verification/invariants.md:69`. The boot log says **410
prefixes**. The harness's own comment records the two growths that made
it stale (211 → 334 → 410). A number nobody can read off a run is a
number that rots, and this is the same defect one level up.

## The problem

The `SELFTEST` line's duration is the *only* number the harness has, and
it is the sum of ten things that grow independently. Every question an
operator actually has about it is unanswerable:

- *Which section got slower between these two runs?*
- *Is CI's 2.2× uniform, or is one section paying for a shared runner's
  disk while the rest are unchanged?*
- *This unit added checks to `svc_selftest`; did the suite grow by what
  I added, or by that plus something else?*
- *We raised the budget to 20 s. From what, to what, in which section?*

Each budget change so far has been made blind. The design doc is candid
about it — "each entry is an admission that the line reports too little"
— and the admission has been standing since the fsctl unit.

This is a shape this tree keeps meeting: an instrument that cannot
distinguish the cases it exists to distinguish reads like evidence
without being any. The nettest roster (PR #177) and the slirp probe
(PR #182) were the same complaint one subsystem over, and both were
worth building.

## Design

**Each section reports its own duration, from the guest's clock, on a
line the harness parses.**

1. **Time each section in `selftest()`.** `cosmo_clock_ns()` is already
   used inside the suite, so there is no new syscall and no new
   dependency. Each call is bracketed and prints one line in the
   existing machine channel — `USERTEST:` uppercase, which is what
   `USERTEST: PASS` and `USERTEST: FAIL (n checks)` already use:

   ```text
   USERTEST: section fs 812 ms
   ```

   The lowercase `usertest: … ok` prose lines stay exactly as they are.
   They are read by people, one of them is a required marker, and
   rewriting them to carry a number would put a parser and a sentence in
   the same string for no gain.

2. **The trailing body becomes a named section.** Those 54 checks belong
   to no section today, so timings would not sum and the blind spot
   would survive the unit that exists to remove it. Whether it is
   extracted into a function or merely bracketed in place is a build
   decision; that it is *named* is not.

3. **One total line**, so the sum can be reconciled against the kernel's
   `SELFTEST: process-user … (N ms)`. The difference is the process
   spawn, `init`'s own startup and teardown, and it is worth seeing
   rather than assuming: if the sections sum to 4 s of an 8 s line, the
   interesting half is the one nobody is measuring.

4. **The harness parses the section lines and reports the slowest**, the
   way it already does for tests:

   ```text
   boot-test: user-mode suite 8284 ms in 10 sections; slowest: proc 3960 ms, fs 1204 ms, …
   ```

5. **Report, don't assert — no per-section budget.** This is the trap
   the unit exists to remove, and re-introducing it ten times smaller
   would be worse than leaving it once. A section budget would ration
   sections that got more thorough, need widening every time userland
   grows, and fail runs on a shared runner's noise. The composite 20 s
   budget stays as the only failure condition, because its job is to
   catch a suite that stopped terminating. The section numbers are for
   attribution.

   This is the same conclusion the straggler-kick unit reached after
   three wrong answers (`docs/audit/next-subsystem-straggler-kick.md`):
   an unbounded wait would hang, a cap would pass vacuously, and
   failing on the cap asserted something that is not true on CI. What
   was left was to report the number and let a human read it.

6. **The parse must become a function to be testable.** Today the whole
   timing block lives inside `main()` (`run_boot_test.py:467` onward),
   so nothing about it can be unit-tested. `nettest.py` was extracted
   for exactly this reason and now has 61 host checks behind it. A
   `summarize_sections(lines)` returning the parsed rows is the smallest
   thing that makes the cases below writable.

## Affected files

| file | change |
| --- | --- |
| `userland/init/init.c` | bracket the nine sections and the trailing body; print `USERTEST: section …` and the total |
| `tests/boot/run_boot_test.py` | extract the section parse into a function; report the slowest sections; no new failure condition |
| `tests/boot/test_usertest_sections.py` (new) | the host cases below |
| `tests/host/host.mk` | run the new host test beside `test_nettest_deadline.py` |
| `docs/verification/design.md` | §6: the admission is discharged; **and the stale 211 → 410** |
| `docs/verification/invariants.md` | **F6**: a composite test reports its sections; **and the stale 211 → 410** |
| `docs/kernel/process/testing.md` | what `process-user` now prints |
| `docs/userland/testing.md` | the same, from the userland side |
| `README.md` | the Status entry |
| `docs/audit/2026-09-deferred-work-inventory.md` | strike the §3 row |

No kernel change. The guest side is userland, the host side is the
harness — the same division as the nettest units.

## Tests

Host tests, driving the parser against synthetic harness output, no
boot:

| test | asserts |
| --- | --- |
| `sections_parsed` | ten section lines in, ten rows out, in the order printed |
| `slowest_named` | the summary names the slowest section, and names a *different* one when a different section is slow |
| `total_reconciled` | the reported sum and the kernel's `SELFTEST` duration are both shown, and their difference is not silently dropped |
| `missing_section_tolerated` | a run that ends early (a hang, a section that never printed) parses to the sections that did finish rather than raising — this is the case where the output matters most |
| `no_sections_is_not_an_error` | a release build, which runs no user-mode suite, produces no summary and no failure |

**The bug-proof.** Two runs identical except for *which* section is
slow must produce **different** summaries naming the right section
each time. Today every arrangement of the same total produces the same
single line, which is the whole defect: a summary that says the same
thing whichever section was slow is not a summary. It must also fail
for the stated reason — a parser that names a section because it was
first in the list, or last, or longest-named, passes a careless version
of this check, so the two arrangements differ only in the durations.

And one boot assertion, which is the cheap half: the run's own output
must contain a section line for **every** section `selftest()` calls.
That is the guard against the failure mode this tree has hit before — a
section added later that nobody brackets, invisible again, with the
summary looking complete.

## Risks

- **Guest timing under TCG is noisy.** These numbers are for
  attribution between sections of one run, not for benchmarking across
  runs or machines. The design carries this by refusing to put a budget
  on them; the report says so here so that a later unit does not read
  the numbers as a performance baseline.
- **More lines on the console cost time in the thing being measured.**
  Ten extra line-buffered writes, against a suite that already prints
  thirty-four. Negligible, and it is measurable after the build — if it
  is not, that belongs in the as-built banner.
- **It may find that the time is evenly spread**, in which case the
  budget was right and no section is at fault. That is a result: it
  would mean the suite is simply large, and the honest next step is to
  say so instead of hunting a culprit that does not exist. The report
  should not promise a villain.
- **The trailing body is the one place the design could be dodged.**
  Leaving those 54 checks unnamed would make every other number look
  clean while the gap survives. Named in the design so the build cannot
  quietly skip it.

## Alternatives considered

- **Timestamp the lines on the host instead.** Requires no guest
  change, and measures the wrong thing: under TCG a serial write can
  take longer than the work that produced it, so host arrival times
  conflate console latency with section cost. The guest's own clock is
  what the kernel's `SELFTEST` durations already use.
- **Split `process-user` into nine kernel self-tests.** It would give
  per-section lines for free from machinery that already exists — and
  it would cost nine process spawns instead of one, changing the thing
  under test to measure it, and losing the shared state the sections
  build up in order. The suite is one program deliberately.
- **Raise the budget again and move on.** That is what happened twice,
  and this row is the record of it not working: the budget went to 20 s
  and CI has since put 8284 ms of one line against it with no way to
  say what moved. A third widening buys another few months of not
  knowing.
- **Do nothing; the hang case is already covered.** True and not
  sufficient — the last boundary does name a hang, which is why this
  report says so plainly rather than claiming otherwise. But every
  actual incident so far has been slowness, not a hang, and slowness is
  the case with no instrument at all.
