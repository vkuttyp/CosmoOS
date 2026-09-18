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

**Observed once, not yet on the list: the TLB shootdown deadline.**
`kernel/arch/x86_64/mmu.c:324` gives every other CPU one second to
acknowledge an IPI and panics otherwise. On 2026-09-17 a debug boot
panicked with `TLB shootdown ... acknowledged by 2 of 3 CPUs` on a
developer machine running several QEMU boots and a build at once; the
next boot passed and no CI run has shown it. It is recorded here rather
than added to the list because one observation on a deliberately
overloaded host is not evidence about the bound — but it is the same
family as the rows above ("a host holding the vCPU"), and if it recurs
this is where it starts. It is not a test bound: a shootdown that really
never completes is a kernel defect, so widening it would hide the thing
it exists to catch. A re-run distinguishes the two, as everywhere else
here.

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

**`lockup-sample-busy` itself, three times on 2026-09-17 and 18**, at
`lockuptest.c:399` (`el < LOCKUP_SAMPLE_TIMEOUT_NS + 2 ms`): once on the
socket-verdict branch, once on that branch's CI again, and once on a
`main` run whose commit was a **documentation-only** merge — a report,
no code at all. That last one is the clearest of the three: a tree that
changed one Markdown file cannot have slowed a lockup sample.

It is the load-sensitive family this file's list describes, and it is not
*on* the list. The bound is `LOCKUP_SAMPLE_TIMEOUT_NS` plus two
milliseconds of slack, and the slack is what a loaded host eats. Adding
it to the list would mean widening the bound, and that is the trade the
list exists to refuse when the bound is the property: a lockup sample
that answers late is a lockup sample that did not work. What is recorded
instead is the rate — three in two days, at least one on a tree that
cannot have caused it — because the next unit to hit this should know it
is not the first, and that re-running is the right first move.

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

**A fifth and a sixth, both on the pull request that added this
paragraph, on consecutive runs.** Same assertion, same architecture, on
a branch whose subject is a lock and a counter in cosmofs. Neither is a
new fact about the cause. Together they are two facts about the *rate*,
which nothing here had recorded:

- **the tally is kept here and nowhere else** (see *The count*, below),
  at least three of them on trees that cannot have caused them — a
  documentation-only branch, a `main` run, and this one;
- and **twice in a row on one branch**, which needs care, because this
  file says near the top that *a listed test that fails twice in a row
  is a regression until shown otherwise*.

That rule stands and this does not weaken it. Two things discharge the
"until shown otherwise" here, and neither of them is the count:
`net-harness` is deliberately **not** on that list (the paragraphs above
say why -- a host-dependent exchange is not a bound this project can
widen), and the branch it failed on twice changes a lock and a counter
in cosmofs, with no path to a TCP exchange against a process on the
host. The same assertion has already failed on `main` and on a
documentation-only branch.

The general form is worth stating, because the count is the tempting
thing to reason from and it is the wrong thing: **what discharges "until
shown otherwise" is the diff, not the number of failures.** A second
failure on a branch that cannot reach the code is not twice the evidence
of a regression; it is the same zero evidence, twice.

A test that fails this often on unrelated work is a cost paid by every
unit that follows, and the re-runs are the toll. Naming that is not the
same as fixing it, and this file is not where the fix would go -- it is
where the price is written down, and the price is now large enough to be
worth a unit of its own.

**That unit was done** (`docs/audit/next-subsystem-nettest-deadline.md`).
What it found, and what it did not, both matter to this file.

**It did not find the cause, and then its own instrumentation did.** The
candidate it went after was a real defect and not this bug: the harness
armed a 120-second `accept()` deadline in `NetTest.__init__`, which runs
before QEMU is launched, and closed the listener when it expired. That
is fixed, and the mechanism it suggested is superseded by the paragraph
two below.

The arithmetic, for the record, because it was close enough to be
persuasive. The guest's back-connection lands at about 72 % of the boot
-- 76 to 83 seconds here -- which projects onto CI's 140-to-146 second
boots at 101 to 105 seconds: a margin of fifteen to nineteen seconds
against the old deadline. Thin, and never shown to be crossed. Across
sixteen aarch64 jobs a 145.6-second boot passed and a 145.8-second one
failed, so boot length does not predict the outcome — and the run that
settled it had **78.7 seconds of that budget unspent**.

What it did find is why nobody could tell. **The harness recorded none
of those numbers.** Every sighting produced `TimeoutError('timed out')`
and nothing about when the guest connected, how much budget was left, or
whether the port was still open. Every paragraph above this one is an
attempt to reason about a failure from a log that omitted the one
measurement that would have settled it.

So the deadline is fixed -- bound early, accepted after the guest
reports ready, with one budget derived from the run's `--timeout` -- and
**the harness now prints its timings on every run, pass or fail**:

```
network harness: ready at 80.8s, back-connection accepted at 80.8s,
budget 150.0s, listener closed at 81.0s
```

The next sighting will say whether the deadline was ever the problem.

**It did, within the hour** — on the x86-64 job of the pull request that
added the timings:

```
network harness: ready at 90.9s, back-connection accepted at 92.0s,
budget 150.0s, listener closed at 102.0s
  guest-initiated connection failed (TimeoutError('timed out')) —
  gave up 102.0s after the harness started, guest reported ready at 90.9s
```

**The accept succeeded**, at 92.0 s, one second after the guest reported
ready. The timeout ten seconds later is `conn.settimeout(10)` on the
*accepted* connection: the `TimeoutError` came from `recv`, not from
`accept`. So the deadline was never the cause — which is why the unit
did not claim it was.

What is left is much sharper than anything the paragraphs above could
reach. The TCP connection is established in both directions
(`ksock_connect` returns 0, the host accepts). The guest's twelve bytes
never arrive. And everything else on the same interface in the same run
is fine: `NETTEST: done tcp_conns=2 udp_pkts=20 quit=1`, a 256 KiB TCP
echo and twenty UDP datagrams.

Twelve bytes, guest to host, on a connection both ends agree exists.
That is the thing to chase, and it took four recorded numbers to see it
after a fortnight of re-runs.

**And then the guest's half of the instrumentation answered it, on the
pull request that added it** (PR #169) — twice, once per architecture.
The x86-64 job:

```
NETTEST: client failed: connect 0, sent -104, recv -1,
  sndbuf free 65536 before, 65536 after send, 65536 after read
  (outstanding 0 then 0), state 0, segs_out +0 retransmits +0 rsts_in +0
```

and the aarch64 job, the same in every field but the last:

```
  ... segs_out +0 retransmits +0 refused +0 rsts_in +1
```

`-104` is `ECONNRESET`; state `0` is `TCP_CLOSED`. **`ksock_sendto`
failed.** The bytes were never queued and never transmitted: the
connection had already been reset when the guest wrote to it, while the
host had accepted it a second earlier.

So nothing was ever lost on the wire. The twelve bytes are a symptom;
the defect is that **an established connection is reset immediately
after `ksock_connect` returns**. Who sends that reset is not yet known —
the counters' window starts after the connect, so it cannot see one that
arrives during it, which is the next instrument to fix.

**And it reproduces locally on x86-64**, one run in three, with the same
signature — accepted 0.8 s after readiness, `0 of 12 bytes`, gave up ten
seconds later with 78.7 s of accept budget unspent. Every earlier local
attempt in this file and in the inventory was on **aarch64**, where it
did not reproduce in eleven runs. It appeared on the first x86-64 try.

That is worth more than the diagnosis to anyone reading this file later:
"does not reproduce locally" had been recorded for weeks, and what it
meant was "does not reproduce on the architecture we kept trying".

**Not that the defect is x86-only** -- it is not, and the distinction
matters. CI has now failed it on both architectures with the same
signature within the hour:

```
x86-64   accepted at 92.0s, 0 of 12 bytes, gave up at 102.0s, accept budget 59.1s unspent
aarch64  accepted at 92.6s, 0 of 12 bytes, gave up at 102.6s, accept budget 65.3s unspent
local    accepted at 76.1s, 0 of 12 bytes, gave up at 86.1s,  accept budget 78.7s unspent
```

Three machines, two architectures, one mechanism, and in every case the
accept succeeded with a minute or more of its budget to spare. What is
architecture-dependent is only whether *this* developer's machine
reproduces it, which is a fact about where to run the loop, not about
the bug.

**The standing advice does not change.** A re-run still distinguishes a
flake from a regression, and what discharges "until shown otherwise" is
still the diff rather than the count.

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

## The count

`net-harness` sightings live here, in one place, because six different
figures for one number appeared across four files in a single day —
the reports, the inventory row, this file twice, and a comment in
`nettest.c` — each correct when written and none of them corrected
together. Anything that needs the number refers to this section rather
than repeating it.

**Twenty-eight, to 2026-09-18**, across CI and this developer's machine, on
both architectures. Counted rather than asserted, because the first version
of this section said eight and then listed nine:

| sighting | source |
| --- | --- |
| PR #140, twice | the inventory row's history |
| PR #142 | the inventory row's history |
| PR #144, twice | the inventory row's history |
| PR #146 | the inventory row's history |
| two documentation-only commits | the inventory row's history |
| PR #167's own CI run | observed, with timings |
| PR #169's own CI runs, twice — x86-64 and aarch64 | observed, with the guest's returns: `sent -104` both times, and `rsts_in +1` on aarch64 |
| PR #170's own CI run, twice in one run — x86-64 and aarch64 | observed, on a **documentation-only** branch; the x86-64 job is the first sighting where the guest **sent** the bytes |
| `main`, twice — at c47d353 and again at c1e6071 | observed, aarch64 both times, `sent -104` with `rsts_in +0` then `+1` |
| PR #171's own CI runs, three times | observed, aarch64 each time, and the **first three with the counters sampled before the connect**: `connect -104`, `connect 0 in 1270 ms`, `connect -104 in 569 ms` |
| PR #174's own CI runs, twice in a row | observed, aarch64 both times, on a branch whose diff is **three Markdown files and no code at all**: `connect 0 in 1355 ms` with the bytes sent, then `connect 0 in 1063 ms` with `sent -104` |
| PR #175's own CI run | observed, aarch64: `connect 0 in 1460 ms`, `sent 12`, never acknowledged -- the first sighting on a branch that changes code |
| PR #176's own CI run | observed, aarch64, documentation-only: `connect 0 in 1031 ms`, `sent -104`, `segs_out +3 retransmits +1` -- the first read against the 150 us baseline |
| PR #177's own CI run | observed, aarch64: `connect 0 in 1116 ms`, `sent -104` -- **the first with a roster**, which named one connection carrying nothing |
| PR #177's own CI run, the protection-capable CPU boot | observed, aarch64: `connect 0 in 1089 ms`, `sent 12`, `outstanding 12 then 12` -- one connection carrying nothing again, the other guest arm |
| PR #178's own CI run | observed, aarch64, documentation-only: `connect 0 in **787 ms**`, `sent -104`, **`segs_out +2 retransmits +0`** -- one connection carrying nothing a third time, and **the run that falsified the retransmission constant** |
| PR #179's own CI run | observed, aarch64: `connect 0 in **1510 ms**`, `sent -104`, `segs_out +3 retransmits +1` -- one connection carrying nothing a fourth time, and the slowest connect yet |
| PR #179's own CI run, the very next one | observed, aarch64: `connect 0 in **597 ms**`, `sent -104`, **`segs_out +2 retransmits +0`** -- one connection carrying nothing a fifth time, the FASTEST connect, and the second with no retransmission |
| PR #179's own CI run, a third in a row | observed, aarch64: one connection carrying nothing a sixth time |

Nineteen entries, twenty-eight occurrences -- and the table is the tally,
so a sighting recorded only in prose below is a sighting this section
has lost. The first five rows are inherited from the row that recorded
them and are not independently re-verified here. The last fourteen rows
were watched as they happened: PR #167's carries the host's `accepted at
92.0s, 0 of 12 bytes`, and the **nineteen instrumented** occurrences
behind the other thirteen rows carry the guest's side. (These three figures
are computed from the table, not carried forward: they were wrong before
sighting twenty-five, because each update incremented them instead of
counting the rows.) Rows and occurrences
differ because three rows hold more than one sighting; the shapes table
below is per *sighting* and is the one to count from.

**And one of them broke the pattern the others set** — PR #170's x86-64
job, on a branch that changes one Markdown file:

```
NETTEST: client failed: connect 0, sent 12, recv -104,
  sndbuf free 65536 before, 65524 after send, 65524 after read
  (outstanding 12 then 12), state 0,
  segs_out +1 retransmits +0 refused +0 rsts_in +1
```

`sent 12`. The send buffer went 65536 → 65524 and **stayed** there.
`segs_out +1`. So this time the guest queued the twelve bytes, put a
segment on the wire, and they were never acknowledged — while the host
had accepted the connection and read nothing. The three sightings before
it all said `sent -104, segs_out +0`: the bytes never written because
the connection was already reset.

| run | `sent` | `segs_out` | outstanding after the read | `rsts_in` |
| --- | --- | --- | --- | --- |
| PR #169, x86-64 | `-104` | `+0` | 0 | `+0` |
| PR #169, aarch64 | `-104` | `+0` | 0 | `+1` |
| PR #170, x86-64 | **`12`** | **`+1`** | **12** | `+1` |
| PR #170, aarch64 | `-104` | `+0` | 0 | `+1` |
| `main` @ c47d353, aarch64 | `-104` | `+0` | 0 | `+0` |
| `main` @ c1e6071, aarch64 | `-104` | `+0` | 0 | `+1` |
| PR #176, aarch64 (docs-only) | `-104` | `+3` | 0 | `+1` |
| PR #177, aarch64 (**roster**) | `-104` | `+3` | 0 | `+1` |
| PR #177, aarch64 (protection CPU) | **`12`** | `+4` | **12** | `+1` |

Two things this does and does not say. It **does** rule out the send
path as the defect: in one instance `ksock_sendto` returned 12, a
segment went out, and the host still saw nothing — so "the twelve bytes
were never written" describes every instrumented sighting but that
one. It
does **not** establish a retransmission bug, although `retransmits +0`
with twelve bytes outstanding is row three of the four-outcome table in
`docs/audit/next-subsystem-twelve-bytes.md`. That row assumed no reset.
Here `rsts_in +1` says one arrived, and a reset that kills the pcb
before the retransmission timer fires leaves the count flat with nothing
wrong in the timer. Distinguishing the two needs the pcb's own pending
error and a timestamp, which is what
`docs/audit/next-subsystem-socket-verdict.md` is for.

**And then the instrument built to answer this printed its first
failure**, on PR #171's own aarch64 CI — the pull request that added the
socket's pending error and moved the counters to *before* the connect:

```
NETTEST: client failed: connect -104 in 1381 ms, sent -1 in 0 ms,
  recv -1 in 0 ms, pending error -104,
  sndbuf free 0 before, 0 after send, 0 after read
  (outstanding 0 then 0), state 0,
  segs_out +3 retransmits +1 refused +0 rsts_in +1
  (counters from before the connect)
```

**`ksock_connect` itself failed**, with `ECONNRESET`, after 1381 ms.
Every instrumented sighting before this one said `connect 0`. And
`pending error -104` is the first time the *socket's own* verdict has
been read rather than inferred: `ECONNRESET` for this pcb, not a
machine-wide counter that might have belonged to anything.

Read with `tcp.c`, it says where the reset lands. `ksock_connect` waits
for the pcb to leave `SYN_SENT`/`SYN_RCVD` and then reports
`ksock_error` if the state is not `ESTABLISHED` or `CLOSE_WAIT`
(`socket.c`); and `ECONNRESET` rather than `ECONNREFUSED` is set only by
a reset accepted **on a synchronized connection** (`tcp.c`, the RFC 5961
§3 path — a reset in `SYN_SENT` gives `ECONNREFUSED`). So the handshake
*completed*, and the reset arrived before the connecting thread ran
again. `retransmits +1` and 1381 ms are one SYN retransmission at the
one-second timer, so the handshake was slow as well as short-lived.

**That unifies the shapes.** Twelve instrumented sightings, and the
guest's progress when the reset lands is the only thing that differs:

| run | how far the guest got | `rsts_in` |
| --- | --- | --- |
| PR #169, x86-64 | connected, then `sendto` refused | `+0` |
| PR #169, aarch64 | connected, then `sendto` refused | `+1` |
| PR #170, x86-64 | connected, **sent 12**, never acknowledged | `+1` |
| PR #170, aarch64 | connected, then `sendto` refused | `+1` |
| `main` ×2, aarch64 | connected, then `sendto` refused | `+0`, `+1` |
| PR #171, aarch64 | **the connect itself reset** | `+1` |
| PR #171, aarch64 again | connected, **sent 12**, never acknowledged | `+1` |
| PR #171, aarch64, third | **the connect reset, with no retransmission** | `+1` |
| PR #174, aarch64 | connected in 1355 ms, **sent 12**, never acknowledged | `+1` |
| PR #174, aarch64 again | connected in 1063 ms, then `sendto` refused | `+1` |
| PR #175, aarch64 | connected in 1460 ms, **sent 12**, never acknowledged | `+1` |

The constant is not the twelve bytes and never was: it is **an inbound
reset on an established connection to slirp, arriving at whatever point
the guest has reached** — while slirp's own host-side socket connects
successfully, which is why the host's `accept` keeps succeeding and then
reading nothing. QEMU user-mode networking being a proxy rather than a
wire is what makes those two facts consistent.

What is still not established is why slirp resets it. That is outside
this kernel, and saying so with evidence was named as a possible result
from the beginning (`docs/audit/next-subsystem-twelve-bytes.md`, Risks).

`rsts_in +1` in ten of the twelve; the two `+0`s are the instrument's own
window, which opened after the connect until PR #171 moved it.

**The first sighting on a branch with code in it, and how that is
discharged.** PR #175 -- the quiesce-wake unit -- changes
`kernel/scheduler/`, `kernel/core/quiesce.c` and both architectures' trap
returns, which are hot paths a network exchange runs through. So "the
diff is Markdown" is not available here, and the discharge has to be
narrower:

- the signature is the one this row has recorded twenty times, including
  on trees with no code at all: an inbound reset (`rsts_in +1`), the
  guest's twelve bytes unacknowledged, the host's `accept` having
  succeeded;
- the failing exchange is a TCP connection to a process on the host
  through slirp, and the branch touches no network file at all
  (`git diff --name-only main...HEAD` matches nothing under
  `kernel-services/network/`);
- the mechanism the row has established -- slirp resetting an established
  connection -- has no path to grace-period wake latency.

That is weaker than the documentation-branch discharge and is written
down as weaker. What would settle it is the row being closed, not another
re-run.

**Twice in a row on one branch, and the rule that covers it.** PR #174
failed `net-harness` on consecutive aarch64 runs. This file says near the
top that *a listed test that fails twice in a row is a regression until
shown otherwise*, and says elsewhere that **what discharges "until shown
otherwise" is the diff, not the number of failures**. The diff here is
three Markdown files — this file among them — and no code: `git diff
--name-only main...HEAD` returns `.md` and nothing else. So the rule is
discharged the way it was for the earlier pair, by the change rather than
by the count.

It is worth saying what this costs rather than only that it is explained.
Two consecutive failures on a documentation branch mean the merge gate
for a report is now a coin toss on an unrelated defect, and the honest
options are to re-run until it passes or to stop gating on it. This file
is not where that is decided; it is where the evidence for deciding it
lives.

**The withdrawn retransmission claim, with a fourth data point.** PR
#174's sighting is the fourth with the window moved, and it retransmitted
a SYN and took 1355 ms to connect — so the moved-window runs now stand at
`+1`, `+1`, `+0`, `+1`. The claim withdrawn above stays withdrawn: one
counter-example is enough to show a lost SYN is not *necessary*, and
three of four is not a mechanism. What is worth recording is that it is
**frequent** rather than incidental, and that it is the only feature of
these failures the earlier instrument could not see at all. A unit that
takes this row next should start by asking why the handshake to slirp is
slow, not by assuming it must be.

**The sightings with the window moved show the handshake, which the
other six could not.** The first two suggested a pattern and the third
refuted it, inside a day:

```
connect -104 in 1381 ms ... segs_out +3 retransmits +1 rsts_in +1
connect   0 in 1270 ms  ... segs_out +4 retransmits +1 rsts_in +1
connect -104 in  569 ms ... segs_out +2 retransmits +0 rsts_in +1
```

The first two both took over a second and both retransmitted a SYN,
which was written down here as "the first SYN went unanswered — the
first thing in this chase that looks like a beginning". **The third has
no retransmission at all**: two segments out, 569 ms, and the same
reset. So a lost SYN is not the mechanism, or not the only one, and the
claim is withdrawn rather than left standing with a caveat. It was two
observations, and this file has a history of two observations becoming a
rate that the next run halves.

What survives all three, and every instrumented sighting, is
narrower and duller: **an inbound reset arrives on a connection to
slirp, at whatever point the guest has reached** — during the handshake,
after it, or after a segment is already on the wire — while slirp's own
host-side socket connects fine, which is why the host's `accept` keeps
succeeding and then reading nothing. Nothing observed so far
distinguishes *why*, and the next thing to measure is on the host side
of slirp rather than in this kernel. The locus is
now: **an established connection to slirp is reset — sometimes before the
guest writes and sometimes after a segment is already on the wire — and
the payload never reaches the host's accepted socket.**

**The CI rate rose sharply on 2026-09-17**, and has not fallen since.
Six instrumented failures inside about two hours that day — two on PR
#169, two on PR #170, two on `main` — against one local boot in
twenty-one; four more have followed on 17-18 September. The count above
is the running total; this paragraph is about the day the rate changed. Nothing here explains the jump and
this file does not guess at one; it is recorded because "one in
twenty-one locally" is the only rate this file has measured, and CI is
plainly not that. What it does mean practically: the instrument no longer
has to be waited for. It reports several times a day.

At least four were on trees that cannot have caused them, and PR #170's
two are the clearest of them: that branch adds one Markdown file and
changes no code at all.

**One in twenty-one** local x86-64 boots reproduce it, measured after
PR #169's instrumentation landed; six of those boots ran under CPU load
without raising the rate. Every earlier local attempt was on aarch64,
where it did not appear in eleven runs — the architecture decides where
the loop runs, not what the defect is.

**Reproduced deliberately, 2026-09-18 — and this is not a sighting.**
What follows was induced on purpose and is not counted: the total moved
that day for separate, real failures recorded below, not for this. The
count section above owns the number; this paragraph does not repeat it. At the time, the harness listened on the back-connection port
with a backlog of one and accepted once, without checking what it
accepted — PR #177 has since replaced both. Occupying that single slot
before QEMU
starts — one silent connection, opened behind an environment variable —
reproduced the `net-harness` signature **on the first boot**: host
`accept` succeeded at 76.6 s, read `0 of 12 bytes`, and gave up with
`TimeoutError`, while the guest reported
`connect 0 in 217 ms, sent 12, recv -104, pending error -104, rsts_in +1`.

The capture taken during that boot is the part worth keeping:

```
418.239235  10.0.2.15.50546 > 10.0.2.2.51821  [S]                    SYN
418.456149  10.0.2.2.51821 > 10.0.2.15.50546  [S.]                   SYN-ACK, 217 ms later
418.456851  10.0.2.15.50546 > 10.0.2.2.51821  [P.] seq 1:13, len 12  the twelve bytes
418.456873  10.0.2.2.51821 > 10.0.2.15.50546  [.]  ack 13            slirp acknowledges them
428.422331  10.0.2.2.51821 > 10.0.2.15.50546  [R.]                   RST, ten seconds later
```

slirp acknowledged the twelve bytes into its own buffer, never delivered
them, and reset the guest ten seconds later. The guest is correct at
every step — which is the outcome `nettest.c`'s own comment predicted
before it was ever observed.

**A baseline, which this file did not have.** From a passing boot's
capture: slirp answers the guest's SYN in **150 microseconds** and the
whole exchange — SYN, SYN-ACK, twelve bytes each way, FIN with
`seq 13, ack 13` — completes in **52 ms**. Every sighting above was read
without that number. The induced run's `connect 0 in 217 ms` is three
orders of magnitude off it.

**Two hypotheses closed by measurement.** A backlog of one makes a second
connect **hang silently** on this host — the SYN is dropped, `connect`
does not refuse — so a stale connection both wins the `accept` and
stalls the real one. And `free_port` is clean: **zero collisions in three
thousand triples**, over the observed ephemeral range 49152–65535, so the
three-port collision idea is dead.

**What was not established by the reproduction is the wild trigger.**
The adversary was injected; nothing in it says a foreign connection is
what happens on CI. It names a mechanism the harness could not report,
not a cause. Taken up by
`docs/audit/next-subsystem-nettest-accept.md` — and **answered below by
sighting twenty-three**, which found exactly one connection carrying
nothing and ruled the foreign connection out.

**Sighting twenty-two, 2026-09-18, on PR #176's own aarch64 CI** — the
pull request that proposes the fix, on a branch that changes three
Markdown files and no code:

```
NETTEST: client failed: connect 0 in 1031 ms, sent -104 in 0 ms,
  recv -1 in 0 ms, pending error -104,
  sndbuf free 65536 before, 65536 after send, 65536 after read
  (outstanding 0 then 0), state 0,
  segs_out +3 retransmits +1 refused +0 rsts_in +1
```

with the host reporting `ready at 89.9s, back-connection accepted at
90.9s`, `0 of 12 bytes: b''`, and `TimeoutError` at 100.9 s.

**`connect 0 in 1031 ms` is the line that matters, and it is only
readable because of the baseline above.** A healthy back-connection is
answered in 150 microseconds; this one took four orders of magnitude
longer and still returned 0. `segs_out +3 retransmits +1` is one SYN
retransmission: **slirp did not answer the first SYN and answered the
second**, about a second later. That is the induced reproduction's
mechanism — a host-side connect that does not complete promptly — at a
slower speed, and it is the first sighting whose `connect` time can be
compared against anything.

What this sighting still cannot say is **which connection the host
accepted**. `accepted at 90.9s` is one second after `ready`, so the
accept is contemporaneous with slirp finally answering, and the
instrument reports a time without an identity. That is exactly the gap
`docs/audit/next-subsystem-nettest-accept.md` proposes to close, and it
is the reason this sighting is recorded here rather than argued from:
the roster the unit adds would have said whose connection that was.

**Sighting twenty-three answered the question, on PR #177's own CI —
the run that installed the instrument.** The roster said:

```
1 connection(s): 127.0.0.1:46652 accepted at 90.8s, 0 byte(s): b''
```

**Exactly one connection reached the port, and it carried nothing.**
This file predicted that case in as many words, one commit earlier: "if
it names exactly one connection, carrying nothing, then the wild trigger
is not a foreign connection and the next place to look is slirp's
host-side connect." So **the foreign-connection hypothesis is dead as
the wild trigger.** The stale-slot reproduction reproduces the
*symptom* faithfully and is not what happens on CI.

The guest's side, near-identical to sighting twenty-two:

```
connect 0 in 1116 ms, sent -104, recv -1, pending error -104,
  (outstanding 0 then 0), state 0,
  segs_out +3 retransmits +1 refused +0 rsts_in +1
```

Two consecutive sightings now share one shape, and the 150 us baseline
is what makes it legible: **a connect that succeeds in about a second**
against a baseline three orders of magnitude faster, with **exactly one
SYN retransmission** (`segs_out +3`, `retransmits +1`) — slirp ignoring
the first SYN and answering the second — then a reset before the guest
can write. Host-side, slirp's connection is accepted and delivers
nothing.

**The locus is slirp's own host-side connect**, not anything that
reaches the port and not this kernel. What is still unnamed is why that
connect stalls for about a second and then fails, and the next
measurement is a packet capture of the host's loopback rather than the
guest's wire — the guest's side is now fully accounted for.

**Sighting twenty-four confirmed it, two hours later, on the same pull
request** — the "protection-capable CPU" boot of an aarch64 job whose
first two boots passed. The roster again:

```
1 connection(s): 127.0.0.1:60860 accepted at 88.7s, 0 byte(s): b''
```

**One connection, carrying nothing, for the second time running.** The
guest's side took the other arm of the four-outcome table, which makes
the pair more informative than either alone:

```
connect 0 in 1089 ms, sent 12 in 0 ms, recv -104 in 0 ms,
  pending error -104, (outstanding 12 then 12), state 0,
  segs_out +4 retransmits +1 refused +0 rsts_in +1
```

`sent 12` with **`outstanding 12 then 12`**: the twelve bytes went onto
the wire and were **never acknowledged** — where sighting twenty-three's
guest never got to write at all. So slirp answers the handshake late,
then either takes the bytes and drops them (the induced reproduction) or
never acknowledges them, and in both cases its host-side socket is
connected — the harness accepted it — and carries nothing.

~~**Three consecutive connects: 1031 ms, 1116 ms, 1089 ms, each with
exactly one SYN retransmission** ... a constant: slirp ignores the first
SYN and answers the second, one retransmission timer later.~~
**WITHDRAWN by sighting twenty-five**, which connected in **787 ms**
with **`segs_out +2 retransmits +0`** — slirp answered the *first* SYN
and still took most of a second. The delay is real and large (787 ms to
1116 ms against a 150 microsecond baseline) but it **does not require a
retransmission**, so "one retransmission timer" was a coincidence of
three samples and not the mechanism.

**This file has now made that exact mistake twice.** The first
SYN-retransmission correlation was recorded and withdrawn when a fourth
run came back `retransmits +0`; this one was recorded across three
sightings and withdrawn when the fourth came back `retransmits +0`. The
lesson is not about retransmissions. It is that **three samples of a
timing coincidence look like a constant**, and this defect produces
three-sample runs readily enough to catch a careful reader twice. What
survives is only what every sighting shares: a connect that succeeds
after hundreds of milliseconds, and a host-side connection that carries
nothing.

**Sightings twenty-six and twenty-seven widened the range rather than
narrowing it**, and they landed on consecutive CI runs of the same pull
request. The observed connects are now **597, 787, 1031, 1089, 1116 and
1510 ms** — a spread of two and a half to one — and **two of the six had
no retransmission at all**. Four of six having exactly one retransmission
is what a three-sample reader saw as a timer. It is not one.

The roster is **six-for-six on one connection carrying nothing** -- a
third consecutive aarch64 CI run of the same pull request -- and that
remains the only part of this defect that has never varied. When the
next theory arrives, that is the line it has to explain.

**And the grace bound held in production.** The harness gave up at
108.7s -- twenty seconds after the accept, the receive budget plus the
grace -- rather than burning the 69.3s that remained. The run failed at
153.8s, inside its 180 s timeout, and reported *only* the harness
markers: the five unrelated missing markers that sighting twenty-three's
run produced are gone. That is the regression fix working on the exact
failure that exposed it.

**The instrument was rebuilt, PR #177.** The harness no longer assumes
the first connection to arrive is the guest's: it listens with a backlog
of eight, accepts every connection until one delivers `cosmo hello\n`,
gives each its own receive budget from its own accept, and reports a
roster -- every peer, when it was accepted, bytes read, a thirty-two
byte preview -- in place of `TimeoutError`. **It answered on its first
outing**, which is the sighting recorded above: the question this file
could not ask for three weeks was answered by the next failure after
the instrument landed.

**And one hour spent for nothing, recorded so it is not spent twice.**
A twenty-two-boot aarch64 hunt with packet capture on 2026-09-18 found
no failure. That is consistent with the paragraph above — aarch64 did
not reproduce it in eleven earlier runs either — and is evidence about
nothing. The local rate this file measures is **x86-64's**. Hunt on
x86-64.

Update this section and leave the rest alone.
