# Load-sensitive tests

A self-test on this list asserts something genuinely temporal, with a
bound that a correct kernel can fail when the host holds its vCPU for
long enough. The list exists so that when one of them fails, the reader
knows within one line whether to re-run or to investigate. It is short
on purpose, and the rule for joining it is at the end.

## What the harness does with it

`tests/boot/run_boot_test.py` reads the table under "The list" below --
every row of it whose first cell is a self-test name in backticks, and no
table under any other heading -- and, when a self-test fails, names it
against the list in the failure report:

```
boot-test: FAIL after 82.9s
  - kernel reported failure via debug-exit
  - forbidden marker /SELFTEST: FAIL/: SELFTEST: FAIL (1 of 246)
  - note: sleep is on the load-sensitive list (docs/testing/flakes.md); a re-run distinguishes a flake from a regression
  - no 'SELFTEST: PASS' line
```

(A real report, from a boot in which `selftest_sleep` had its vCPU held
for 150 ms by an injected `udelay`; the test's own line, with the
expression that failed and its line number, is in the serial log the
harness echoes below the report.)

The run still **fails**. The note is a label, not a retry: hiding a flake
is worse than the flake, and a listed test that fails twice in a row is a
regression until shown otherwise. A test that is not on the list gets no
note, and its failure is a regression until shown otherwise.

If this file is missing, or its table no longer parses, the harness says
so in the failure report of any run in which a self-test failed. That
is deliberate: the list going silently empty is the failure it guards.

## The list

| test | site | the bound | what it asserts | why nothing observable replaces it |
| --- | --- | --- | --- | --- |
| `sleep` | `kernel/scheduler/schedtest.c`, `selftest_sleep` | a 20 ms sleep returns within 20 ms + 3 ticks + 100 ms | the sleep is woken by the first tick past its deadline, not by a coarser mechanism (a sleep serviced every 100 ms would fail it) | "promptly" is the property; the wake is a timer callback on this CPU and there is no wake-reason to count that a coarse mechanism would not also produce |
| `net-icmp-limit` | `kernel-services/network/nettest.c`, `selftest_net_icmp_limit` | the 300-echo flood is decided within the one-second limiter window the test saw begin | at most `ICMP_RATE_PER_SEC` replies to a burst, exactly one window's worth | the window's phase is now observed (an echo refused, then one replied), but that the flood's ~20 ms fits in the window's remaining second is time; a host holding the vCPU for most of a second inside the flood fails it. A 50× margin, the largest here |
| `el2-guest-timer-ontime` | `kernel-services/virtualization/hvtest.c`, `selftest_el2_guest_timer_ontime` (runs under `make test-gic`) | the guest's timer is late by less than four times the ~15 ms it asked for | the WFI park wakes on the guest's deadline in 1 ms slices, not by sleeping the whole interval or in coarse slices | a wake-reason counter would say "the deadline passed", which a coarse park also satisfies; only the lateness distinguishes them, and lateness is time |

The first two were widened on 2026-09-14 after failing on a correct
kernel the day before (`sleep` at 3 ticks + 10 ms of slack; the guest
timer at "less than what it asked for"); both bounds still sit an order
of magnitude under the regression they exist to catch. The third is not
a widened bound but a residual: the unit's twenty-boot run found the
limiter's window boundary inside the burst once in forty boots, the
test now makes the window's phase known, and what it still assumes is
recorded here because no observable replaces it. Each site carries a
comment pointing here.

## What is *not* on the list, and why

The list is what remains after the suite-waits unit
(`docs/audit/next-subsystem-suite-waits.md`) classified every upper bound
on elapsed time. The other kinds are handled differently and must not be
added here to make a red run go away:

- **Waits for a property** (`wait_until` in `nettest.c`, and the same
  shape elsewhere): a deadline loop on an observable -- a state, a
  monotonic counter, a readiness bit -- that returns as soon as the
  property holds. Its budget is a hang guard measured in seconds, not a
  measurement. If one expires on a loaded host, the predicate or the
  budget is wrong, not the host; fix that, and do not list the test.
- **Restated bounds**: a timing that stood for something observable, now
  observed. `smp-parallel` watches CPU 1's counter move from CPU 0 instead
  of comparing two counts; `smp-wake` counts the reschedule IPI on the
  target instead of timing the wake; `realtime` brackets each clock pair
  and re-reads an interrupted one instead of tolerating 1 ms;
  `hv-vcpu-stop` joins the kicker and asserts the run outlived the kick.
  None of these can fail for the host's reasons any more.
- **Bounds that named a hang**: a spinner never preempted, a lost
  semaphore wake, a cross call never answered. These do not present as a
  slow return but as no return, which the self-test watchdog (8 s, with a
  scheduler dump) or the call's own one-second panic reports. The
  `< 200 ms`, `< 500 ms` and `< 100 ms` that used to sit on them could fail
  only on a loaded host and were removed.
- **Lower bounds** (`elapsed >= MS(30)`): a loaded host makes them more
  true. Left alone.
- **Generous guards** (`irqtest.c`, `proctest.c`'s 15 s and 2 s): a host
  that breaks these is genuinely broken. Left alone, and not listed.

## Lockup reports under load

The lockup detectors (`docs/kernel/diagnostics/design.md`, "lockup.c")
are not tests, and their reports are forbidden markers, so a report
fails the run with the CPU's trace in the log. The hard-lockup detector
watches a neighbour's tick count, and QEMU runs each vCPU on a host
thread: a host that starves one for longer than the threshold (10 s)
produces `hard lockup: cpu J no tick for M ms` on a correct kernel --
the `cpu1: up` flake of PR #112 was this kind of starvation, at a
smaller scale. The line carries the stall's length and the target's
tick age; a first CI sighting is read as "the host starved vCPU J for M
ms" and investigated as this false positive first, before the kernel.
The soft-lockup detector cannot false-positive this way: it counts the
victim's own ticks, which a starved vCPU does not take.

## The rule for joining the list

A test goes on this list only after its bound has been classified, in
the failing site's own comment, as one of:

1. **Keep** -- the bound is the property, or is so generous that a host
   breaking it is broken. Not listed.
2. **Restate** -- the timing is a proxy for something directly
   observable. Observe that instead. Not listed.
3. **Widen and label** -- the property really is temporal and nothing
   observable substitutes. Widen the bound to the largest value that
   still catches the regression it exists for, write a `LOAD-SENSITIVE`
   comment at the site naming this file, and add a row above saying what
   the bound asserts and why nothing observable replaces it.

Adding a row without the classification, or to make a run pass, defeats
the list: the note it produces is only worth reading if every test on
it has been argued in.

## History

The failures that produced the unit, all on 2026-09-13, all on correct
code:

| test | site then | kind | times | what became of it |
| --- | --- | --- | --- | --- |
| `net-tcp-syncache` | `nettest.c:983` | a fixed `settle(N)` before an assertion | 4, one blocking a merge | waits for the SYN-answered counter |
| `net-icmp-limit` | `nettest.c:1236` | the same, "N things after a fixed settle" | 1, on the unit's own pull request | waits for the echoes decided |
| `net-bench` | its 8 s per-test budget (2026-09-14, the unit's third twenty-boot run, x86-64) | 71 s with every assertion passing and normal throughput: time lost between rounds, a retransmit backoff after a receive-queue drop the likeliest mechanism | 1 in 100 | **not listed** -- a real slowness with a mechanism to find, named as a follow-up in the unit's report |
| `net-icmp-limit` | `nettest.c:1444` (2026-09-14, the unit's own twenty-boot run) | the limiter window's boundary inside the burst -- a phase the test never controlled | 1 in 40 | the phase made known: fill and probe until a refusal, then probe until a reply, flood into the fresh window; the residual listed above |
| `sleep` | `schedtest.c:368` | upper bound | 1 | widened and labelled; listed above |
| `smp-parallel` | `smptest.c:239` | work ratio | 1 | restated: parallelism observed from CPU 0 |
| `el2-guest-timer-ontime` | `hvtest.c:1279` | upper bound | 1 | widened and labelled; listed above |

A different family, recorded here because a flake seen once and not
written down is a flake somebody else debugs from scratch. On
2026-09-16, one CI run of PR #154 failed on **both** architectures, and
the same commit passed on a re-run with nothing changed:

| where | what it looked like | why it is not a bound |
| --- | --- | --- |
| x86-64, the default boot | all 319 self-tests passed and `USERTEST: PASS`; the shell script ran its whole command list to `exit 0`; then `SHTEST: PASS` **and every Linux-ABI marker** -- the musl hello, `LINUXTEST: PASS`, `lxinterp`, `lxdyn`, `lxsig term` -- were all missing together | not a bound a loaded host trips. **What the cause was is not established**: those markers are the tail of the run -- the Linux ones come from `rc.test` and `SHTEST: PASS` is printed last -- so an absent boot archive, a Linux-test build that did not arrive, a hang and a premature shutdown all suppress exactly the same suffix. The observation is recorded; the diagnosis is not |
| aarch64, the guard boot | `selftest: hv: skipped: no backend`, a forbidden marker | the virtualisation backend was absent on a machine variant that normally has one |

Neither is on the list above and neither should be: they are not bounds a
loaded host can trip, so a re-run distinguishes them from a regression
rather than curing them. What the rows are for is the next reader, who
should know that this pair has been seen once, on correct code, and that
the same commit passed immediately afterwards.

**What they are not is explained.** The first draft of this entry said
the x86-64 marker loss "is a boot archive or a Linux-test build that did
not arrive" -- a cause asserted from a symptom, when a hang or an early
shutdown produces the identical suffix. A confident wrong cause in this
file is worse than no cause at all, because the next reader starts where
it points.

A third observation, 2026-09-16, on the CPU-clock unit's branch:
`net-harness` failed once at `nettest.c:929` (`client_ok`) on x86-64 and
passed on an immediate re-run of the same image. The assertion is a TCP
connect-send-receive exchange with an echo server in the *host* harness
process, so it depends on the host scheduling that process promptly. The
run happened while this machine was under enough memory pressure to have
a background build killed for it.

It was then seen a second time, in CI, on the same pull request: one run
failed it on **both** architectures, and a re-run of the identical commit
passed all three of its boot tests -- default, guard and release --
before failing much later at an unrelated build step. Five local runs of
the same image, three in the default configuration and two in the guard
one, all passed with `NETTEST: client ok`.

**That last sentence is a circumstance, not a cause.** The pressure was
real and it is the reason the re-run was tried, but nothing here
establishes that it produced the failure: a dropped SYN, a slow host
process and an unrelated timing window all look identical from
`client_ok == false`. What is recorded is what was seen. `net-harness`
is not added to the list above, because a host-dependent exchange is not
a bound this project can widen -- a re-run is what distinguishes it from
a regression.

**Seen a third time, on a source-equivalent tree.** On 2026-09-16 the
aarch64 CI job of a **documentation-only** pull request failed the same
assertion, on a branch whose only commit adds one Markdown file to a
`main` that had just passed CI green on both architectures several times
over.

**What that does and does not show.** It rules out this branch's
changes, and it is the strongest evidence so far that the failure is
nondeterministic rather than caused by whatever landed most recently --
which matters, because the previous unit spent five local runs hunting
it as a suspected regression. It does **not** exonerate the repository.
A latent race in the network stack or in the harness thread would
produce exactly this result, and one observation on one branch cannot
distinguish that from a host-side cause.

(Nor is it literally the same binary: `BUILD_ID` is
`git describe --always --dirty`, compiled in through
`-DCOSMO_BUILD_ID`, and CI rebuilds rather than reusing an artefact. The
source is equivalent; the image is not identical.)

An earlier version of this paragraph said "whatever `net-harness`
depends on, it is not in this repository". That is a categorical
conclusion from a single run, and it is the exact failure this file
warns about four paragraphs above -- a confident wrong cause sends the
next reader in the wrong direction, and this one would have sent them
away from the code.

**A warning about reading the re-run, which cost an hour here.** A run
failing twice is not the same as a *test* failing twice. The second run
above failed at a different step entirely, and net-harness passed in it;
reading "the run failed again" as "net-harness failed again" sent the
hunt after a flake while the real defect -- a link error in a target the
default build does not build -- sat further down the same job. Read the
failing step, not the failing run.

Earlier, the same family: `schedtest.c`'s tick-rate lag bound was widened
in pull request #63 after failing on a loaded host, and its comment
already says what this file says -- a tighter bound was only ever
measuring the host.

## `lockup-sample`, and a failure that was not a flake at all

Three CI runs of one branch, 2026-09-17, failed three *different* tests.
The branch was the VMState-layout unit: a compile-time assertion in a
UAPI header, one self-test that runs in 7 ms, and documentation. It
touched nothing in the network stack, the filesystem or the lockup
detector. Recorded together because the three needed three different
answers, and telling them apart is the whole skill this file is about.

**`net-harness` (aarch64), a fourth sighting.** The same
`nettest.c:929` (`client_ok`) as the three above, on a branch that
cannot have caused it, and it failed on a `main` run in the same hour.
Nothing new; it is here to say the count is four and that one of them
was on `main`.

**`lockup-sample` (x86-64), the first sighting, and not previously in
this file.** `lockuptest.c:157`:

```c
CHECK(in_fn(pc, (const void *)spin_here, SPIN_FN_BOUND));
```

The test starts a spinner pinned to another CPU, samples every CPU
through the NMI path, and asserts that CPU's sampled program counter
lies inside `spin_here`. It passed on a re-run of the same commit.

What is *not* the explanation: a naive startup race. `start_spinner`
waits for `s->running` before it returns, so the spinner is confirmed
alive before the sample is taken. What remains is that `running` is set
at the top of `spinner_main`, a little before it enters `spin_here`, and
that under TCG a guest CPU is a host thread the host may have
descheduled -- so the sample can land while that CPU is somewhere else,
or is not executing at all.

**That is a mechanism, not a cause.** Nothing here establishes which of
them produced this failure, and one observation cannot. It is not added
to the bounds list, because the bound is not a duration this project can
widen: it is "the sampled PC is in the function the thread is spinning
in", which is the assertion's whole content. A re-run is what
distinguishes it from a regression.

**And the third was not a flake.** `cosmofs-writeback` failed on the
same branch and looked exactly like the other two -- a timing-ish test,
in a subsystem the branch does not touch, on one run of three. It was a
race in the code, and what said so was the log rather than the count:
the test failed after **80 ms**, not at its 2-second deadline, with
`committed generation 2` printed above the failure. So the commit had
happened and the assertion about it had still failed, which no amount of
host load explains. `cfs_writeback_thread` was incrementing
`fs->wb_commits` after `cosmofs_sync` had dropped `fs->lock`, while
`cosmofs_stats` reads that counter and the generation together under it.

The lesson for this file is its own warning run backwards. It warns
against a confident wrong *cause*; this is a confident wrong
*category*. Three failures on a branch that cannot have caused any of
them is good evidence for "not this branch" and no evidence at all for
"not the repository", and the cheapest thing that separated them was
reading how long the test took before it failed.
