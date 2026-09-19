# NEXT SUBSYSTEM — the suite behind one line

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. **This report is as built**
(PR #188), and the banner below records where the build differed from
it.

**What the build changed, each found by building rather than reading:**

1. **The first measurement contradicts the section sizes this report
   tabulated.** The design leaned on check counts to say which sections
   were large. They do not predict time at all: `svc` is the slowest at
   **1386 ms on x86-64 and 1489 ms on aarch64**, about two fifths of
   the suite, from **34** checks and nine sleeps waiting on service
   state -- while `proc` with 235 checks takes 912 / 939 ms and `net`
   with 87 takes 34 / 44 ms. Time in this suite is spawning and
   waiting, not checking, and no amount of reading the source would
   have said so. That is the unit justifying itself on its first run.
2. **The sections account for 3610 of 3711 ms (x86-64) and 3827 of
   3938 (aarch64).** The ~100 ms outside them is the spawn, `init`'s
   startup and its teardown -- small, which the report was careful not
   to assume in advance, and now measured rather than guessed.
3. **`trap_selftest` reports `0 ms` on aarch64**, as designed: the
   empty function is still a row, still called, still timed. The
   property the report argued for is visible in a real run.
4. **The harness needed three functions, not one.** The design named
   `summarize_sections`. Testing *which* section gets named needs the
   summary line to be a function too, so `format_section_summary` was
   extracted as well, and the refusals became `section_failures`. The
   bug-proof is only writable because of the second one.
5. **A bug of my own, caught before it shipped and worth recording.**
   The first draft guarded the new check with `want_selftest`, which
   `main()` assigns *below* that point -- an `UnboundLocalError` on
   every run that had sections at all. The guard was unnecessary: a
   build with no user-mode suite prints no section lines.
6. **The invariant is F13, not F7.** `docs/verification/invariants.md`
   already had F7 through F12; the report did not check before
   reserving a number, which is the same class of mistake as reusing a
   test name.
7. **One section had a position requirement, and a table with an
   ordering rule is still a convention.** The trailing body ends by
   closing stderr, so as a row it had to be last: any section after it
   whose `CHECK` failed would write to a closed descriptor and report
   nothing. The first build wrote `/* last: it closes stderr */`
   beside the row — which is precisely the kind of comment this table
   replaced, one line away from the paragraph saying so. The close is
   now teardown in `selftest()` after the loop, structurally last
   instead of conventionally last, and **the rows may run in any
   order**. Proved by reversing the table and booting: `boot-test:
   PASS in 98.0s`, all ten sections reported, and the same three
   slowest in the same order (`svc` 1351 ms, `proc` 859 ms, `fpu`
   665 ms) — the timings follow the sections, not their position.

**Subsystem: what `process-user` reported about itself.** One
`SELFTEST` line stood for the entire user-mode suite — nine sections,
about 540 checks inside them in any one build and 54 more outside any
section at all, plus a process spawn for each tool it drives. When
that line was slow, nothing in the tree said which part of it was
slow, and the harness had twice widened a budget it could not aim.

Everything from here to the **Design** section describes the tree as it
stood before PR #188; the table of ten sections described in the Design
section is what it does now.

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
no part of the line said whether that was one section or all nine.
(It does now, and the answer is that the suite is not uniform: `svc`
alone is about two fifths of it.)

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

**There is prose in the output, and it is not a boundary.** Each
section prints one or more `usertest: … ok` lines, and they reach the
log in call order. `stdout` is **line-buffered** (`libc/src/stdio.c:71`,
`F_WRITE | F_LINEBUF`), so each leaves the guest when it is printed
rather than at exit. It is tempting to read the last one as "this
section finished" — **and that is wrong, in two ways that review caught
in the first draft of this report.**

| section | its `usertest:` lines | where the last one sits |
| --- | --- | --- |
| `fs_selftest` (49-255) | `symbolic links ok` (**110**), then `cosmofs mounted and read from user mode` / `no cosmofs to mount` (249/252) | **not last** — `CHECK(cosmo_umount("/") == -EBUSY)` runs after it (254) |
| `net_selftest` (258-471) | `sockets ok` (470) | last |
| `proc_selftest` (474-1119) | `a symbolic link stays inside a process root` (**734**), `processes ok` (1118) | last |
| `trap_selftest` | x86-64: `user exceptions ok` (**1616**), then the UMIP check to 1627. **aarch64: the function is empty and prints nothing at all** | **not last on x86-64, absent on aarch64** |
| `fpu_selftest` | `fpu isolation ok` | last |
| `priv_selftest` (2967-3002) | `privilege boundary ok` (3001) | last |
| `svc_selftest` (3065-3238) | `services ok` (3237) | last |
| `proc_fs_selftest` (3264-3330) | `/proc ok` (3329) | last |
| `fsctl_selftest` (3353-3553) | `fsctl ok` (3552) | last |
| *(the trailing body)* | `write ok` and others | interleaved throughout |

Two sections print an **early** marker as well: `fs` at line 110 with
145 lines of its function still to run, and `proc` at 734 with 385
still to run. So a marker appearing is not even evidence that its
section is near the end. And `trap_selftest`'s marker being followed by
the UMIP check is still visible in every log, where `usertest: umip:
absent` prints **after** `usertest: user exceptions ok` -- the prose
lines were left exactly as they were.

**Nothing makes a marker the last thing a section does. Seven of the
nine happen to end with one**, which is a convention each section has
to remember rather than a structure, and two have already forgotten
it — `fs_selftest`, which unmounts `/` and checks the errno after
printing, and `trap_selftest`, which on x86-64 runs the UMIP check
after printing and **on aarch64 is an empty function that prints
nothing at all**. The count is also not the same on both
architectures, which a description of the markers has to say and this
report's second draft did not. So:

1. **A hang was *not* reliably attributable.** The first draft of
   this report claimed it was, on the strength of the markers appearing
   in call order. A hang in `fs_selftest` after line 110, or in
   `trap_selftest` after 1616, would put the last printed marker at the
   end of a section that had not finished — and a reader trusting it
   would look in the *next* section. Closer than nothing and not
   dependable, which is worse than either.
2. **A *slow* run is not attributable at all.** Nothing measures a
   section, so there is no duration anywhere for any of the ten rows
   above. That is the larger gap, and slowness — not hanging — is what
   has twice forced a budget change.

Both follow from the same thing: the output was written for people to
read, and nobody has ever made a structural claim about it. That is
what the design below changes, and it is why the timing line is emitted
by the driver rather than by each section.

**The harness reads none of it.** `run_boot_test.py` requires exactly
one line from the suite, `^USERTEST: PASS` (`USERTEST_MARKER`,
line 328), plus `^usertest: umip: enforced$` on the x86-64 guard boot.
The prose lines are not parsed, not reported and not in the verdict.

**And the cost of a composite that reports nothing about itself shows up
in the prose about it.** `cosmofs-replay` — the other entry in
`composite_budget_ms` — is documented as mounting and checking **211**
filesystem images in both `docs/verification/design.md:210` and
`docs/verification/invariants.md:69`. The boot log says **410
prefixes**. The harness's own comment records the two growths that made
it stale (211 → 334 → 410). A number nobody can read off a run is a
number that rots, and this is the same defect one level up.

## The problem

The `SELFTEST` line's duration **was** the only number the harness had,
and it is the sum of ten things that grow independently. Every question
an operator actually has about it was unanswerable:

- *Which section got slower between these two runs?*
- *Is CI's 2.2× uniform, or is one section paying for a shared runner's
  disk while the rest are unchanged?*
- *This unit added checks to `svc_selftest`; did the suite grow by what
  I added, or by that plus something else?*
- *We raised the budget to 20 s. From what, to what, in which section?*

Both budget changes were made blind. The design doc was candid about it
-- "each entry is an admission that the line reports too little" -- and
the admission had been standing since the fsctl unit. It is discharged
now, and the first answer to the second question above is that CI's
2.2x is **not** uniform: `svc` alone is two fifths of the suite.

This is a shape this tree keeps meeting: an instrument that cannot
distinguish the cases it exists to distinguish reads like evidence
without being any. The nettest roster (PR #177) and the slirp probe
(PR #182) were the same complaint one subsystem over, and both were
worth building.

## Design

**Each section reports its own duration, from the guest's clock, on a
line the harness parses.**

1. **`selftest()` drives a table, and the table is what makes the
   claim true.** The first draft of this design said the driver
   bracketing each call means "a section cannot be added without a
   line", and review was right that it does not: the nine are nine
   plain calls, and a tenth plain call added below them is exactly as
   invisible as it was before this unit. Bracketing by hand is the same kind of
   convention as printing a marker last, and this report has just
   finished documenting two sections that forgot that one.

   So the sections become a table the driver iterates, in the shape
   `selftest.c` already uses for the kernel's own registry:

   ```c
   static const struct selftest_section g_sections[] = {
       { "fs", fs_selftest }, { "fsctl", fsctl_selftest }, /* ... */
   };
   ```

   Now there is no way to *call* a section except through the loop that
   times it, adding one is adding a row, and "cannot be added without a
   line" is a property of the code rather than a promise about future
   authors.

   **As built, the harness keeps its own copy of the names**
   (`USERTEST_SECTIONS` in `run_boot_test.py`) rather than getting the
   list from the guest, because a list the guest supplies cannot check
   the guest. The two disagreeing is itself a failure, in both
   directions: a name the harness expects and does not see, and a name
   it sees and does not expect. The first draft of this design said the
   table gave the harness the list "for free", which would have made
   the check vacuous.

   `cosmo_clock_ns()` is already used inside the suite, so there is no
   new syscall and no new dependency. The line goes in the existing
   machine channel — `USERTEST:` uppercase, which is what
   `USERTEST: PASS` and `USERTEST: FAIL (n checks)` already use:

   ```text
   USERTEST: section fs 812 ms
   ```

   The lowercase `usertest: … ok` prose lines stay exactly as they are,
   and stay prose. They are read by people, one of them is a required
   marker, and teaching them to carry a number would make the parser
   depend on the convention this design exists to stop depending on.

2. **The trailing body becomes a real section, in the table.** Those 54
   checks belonged to no section, so timings would not have summed and the
   blind spot would survive the unit that exists to remove it. With a
   table it is not enough to bracket it in place: it has to be
   extracted into a function and given a row, or it is the one thing
   the loop does not cover — which is precisely the hole review found
   in the first draft. Extracted, and named.

3. **One total line**, so the sum can be reconciled against the kernel's
   `SELFTEST: process-user … (N ms)`. The difference is the process
   spawn, `init`'s own startup and teardown, and it is worth seeing
   rather than assuming: if the sections sum to 4 s of an 8 s line, the
   interesting half is the one nobody is measuring.

4. **The harness parses the section lines and reports the slowest**, the
   way it already does for tests:

   **As built**, from a real run rather than the invented example this
   report first carried (which guessed `proc` was the slowest):

   ```text
   boot-test: user-mode suite 3610 ms in 10 sections; slowest: svc 1386 ms,
   proc 912 ms, fpu 664 ms, fsctl 377 ms, fs 128 ms (the process-user line
   is 3711 ms; 101 ms is spawn and teardown)
   ```

5. **Report, don't assert — no per-section budget.** This is the trap
   the unit exists to remove, and re-introducing it ten times smaller
   would be worse than leaving it once. A section budget would ration
   sections that got more thorough, need widening every time userland
   grows, and fail runs on a shared runner's noise. The composite 20 s
   budget stays as the only **duration** the harness will fail a run
   over, because its job is to catch a suite that stopped terminating.
   The section numbers are for attribution and nothing refuses a run
   on their size.

   **As built, the harness does gain failure conditions** — they are
   all about the suite's *self-consistency*, never about how long
   something took: a run that stopped part-way, a declared count that
   does not match the lines printed, an expected section that produced
   no line, and a section name the harness does not know. The report's
   first draft said "no new failure condition", which was the wrong
   way to say "no new budget".

   This is the same conclusion the straggler-kick unit reached after
   three wrong answers (`docs/audit/next-subsystem-straggler-kick.md`):
   an unbounded wait would hang, a cap would pass vacuously, and
   failing on the cap asserted something that is not true on CI. What
   was left was to report the number and let a human read it.

6. **The parse must become a function to be testable.** The whole
   timing block lived inside `main()` (`run_boot_test.py:467` onward),
   so nothing about it can be unit-tested. `nettest.py` was extracted
   for exactly this reason and now has 61 host checks behind it. A
   `summarize_sections(lines)` returning the parsed rows is the smallest
   thing that makes the cases below writable.

## Affected files

| file | change |
| --- | --- |
| `userland/init/init.c` | the trailing body extracted as `syscalls_selftest`, the `g_sections[]` table, and `selftest()` reduced to the loop that times it. **As built:** a table, not ten bracketed calls |
| `tests/boot/run_boot_test.py` | `summarize_sections`, `section_failures` and `format_section_summary` lifted out of `main()`; the summary printed beside the per-test one. **As built: three functions, not one** (item 4) — and it *does* add failure conditions, all about the suite's own self-consistency, never a duration |
| `tests/boot/test_usertest_sections.py` (new) | the host cases below, 34 checks |
| `tests/host/host.mk` | run the new host test beside `test_nettest_deadline.py` |
| `docs/verification/design.md` | §6: the admission is discharged, with the first measurement; **and the stale 211 → 410** |
| `docs/verification/invariants.md` | **F13** (not F6, and not F7 — item 6): a composite test reports its sections. F6 gains the 8284 ms figure; **and the stale 211 → 410** |
| `docs/kernel/process/testing.md` | what `process-user` now prints |
| `docs/userland/testing.md` | the same, from the userland side |
| `docs/development.md` | **not in the design**: its worked example of a boot's output is the first thing a newcomer reads, and it showed `USERTEST: PASS` with nothing before it |
| `README.md` | the Status entry |
| `docs/audit/2026-09-deferred-work-inventory.md` | strike the §3 row |

No kernel change. The guest side is userland, the host side is the
harness — the same division as the nettest units.

## Tests

Host tests, driving the parser against synthetic harness output, no
boot:

`tests/boot/test_usertest_sections.py`, 34 checks, run by
`make host-test`. **As built**: five of these were named in the design
and seven more were added — six while building and one in review, each
marked.

| test | asserts |
| --- | --- |
| `sections_parsed` | ten section lines in, ten rows out, in the order printed |
| `lines_among_noise` | **added**: the lines are found among everything else a boot prints, and the lowercase `usertest: … ok` prose is *not* parsed as a section |
| `slowest_named` | the summary names the slowest section, and names a *different* one when a different section is slow |
| `total_reconciled` | the reported sum and the kernel's `SELFTEST` duration are both shown, and their difference is not silently dropped |
| `truncated_run_parses` | a run that ends early — a hang, so the later sections never printed — parses to the sections that did finish rather than raising, and says how many did. This is the case where the output matters most, and it is why the parser tolerates a short list; it does **not** mean an absent section can hide, which the next three rule out |
| `a_row_that_left_the_table` | **added**: a run with `svc` gone is refused *by name* — the tolerance above is not a hiding place |
| `declared_and_printed_must_agree` | **added**: the suite's own count against the lines seen, which is what catches a garbled stream |
| `an_unknown_section_is_named` | **added**: a section added to the table in `init.c` and not to the harness shows up as a name it does not know, rather than being counted silently |
| `no_sections_is_not_an_error` | a release build, which runs no user-mode suite, produces no summary and no failure |
| `a_total_with_no_sections_is_refused` | **added in review**: a stream whose section lines were all lost but whose total survived declares ten and prints none. It was being *skipped*, because the caller guarded on `sections` being non-empty; the decision moved inside `section_failures`, where it is testable |
| `a_zero_length_section_still_reports` | **added**: `trap_selftest` is empty on aarch64; its row reads `0 ms` and is a reading, not an absence — which is the whole difference from the prose markers |

**The bug-proof, and it was run.** Two runs identical except for
*which* section is slow must produce **different** summaries naming the
right section each time. Every arrangement of the same total used to
produce the same single line, which is the whole defect: a summary that
says the same thing whichever section was slow is not a summary. The
two arrangements differ only in the durations, so a parser that names a
section for being first in the list, last, or longest-named still
fails.

Proved by replacing `format_section_summary` with the pre-unit shape —
one line carrying only the total — and running the suite: **3 of 34
fail**, at `the summaries differ`, `the svc run names svc first` and
`the net run names net first`, and nowhere else. A second proof removes
the missing-section check from `section_failures`: **2 of 34 fail**, at
`the missing section is named` and `and every missing section is
named`, which is the check that stops the truncated-run tolerance from
becoming a hiding place.

And one boot assertion, which is the cheap half and the other half of
review's point: a **complete** run must carry a section line for every
row in the table. The table makes an unbracketed section impossible to
write; the assertion is what catches the table and the suite drifting
apart anyway — a row deleted, a function that returns early, a build
where a section compiles to nothing. `trap_selftest` is already that
last case on aarch64, so this is not hypothetical. The parser's
tolerance of a short list (above) is for a truncated run and is why
the assertion has to be separate from it.

## Risks

- **Guest timing under TCG is noisy.** These numbers are for
  attribution between sections of one run, not for benchmarking across
  runs or machines. The design carries this by refusing to put a budget
  on them; the report says so here so that a later unit does not read
  the numbers as a performance baseline.
- **More lines on the console cost time in the thing being measured.**
  Eleven extra line-buffered writes, against a suite that already
  prints thirty-four. **Measured after the build and it is in the
  noise**: `process-user` was 3707 / 3896 ms (x86-64 / aarch64) before
  and 3711 / 3938 after, inside the run-to-run spread of the same
  code.
- ~~**It may find that the time is evenly spread**~~ — **it did not**.
  The risk was that the answer would be "the suite is simply large",
  which would have been a result and not a villain; the report was
  written so as not to promise one. In fact `svc` alone is two fifths
  of it, and the order is stable across both architectures, both boot
  types and a reversed table. Kept rather than deleted, because the
  measurement could have gone the other way and the report should not
  read as though it knew.
- **The trailing body was the one place the design could be dodged**,
  and in the first draft it still was: the design said "bracketed in
  place or extracted, a build decision", which left 54 checks able to
  sit outside the instrumentation while every other number looked
  clean. It is a table row now, so the build has nowhere to put it
  except inside the loop. Recorded rather than quietly amended,
  because the hole was real and review found it.

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
- **Do nothing; the prose markers are close enough.** They are not,
  and the first draft of this report said they were. Two of the nine
  print mid-section and one is followed by a further check, so reading
  the last marker as a boundary names the wrong section — and a
  diagnostic that is usually right is the kind this file keeps having
  to retract. Every actual incident so far has been slowness anyway,
  which the markers say nothing about at all.
