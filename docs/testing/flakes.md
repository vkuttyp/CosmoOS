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

**Observed three times, not yet on the list: the TLB shootdown deadline.**
`kernel/arch/x86_64/mmu.c:324` gives every other CPU one second to
acknowledge an IPI and panics otherwise. On 2026-09-17 a debug boot
panicked with `TLB shootdown ... acknowledged by 2 of 3 CPUs` on a
developer machine running several QEMU boots and a build at once; the
next boot passed and no CI run has shown it. It was recorded here rather
than added to the list because one observation on a deliberately
overloaded host is not evidence about the bound. **It recurred on
2026-09-20**, on the native thread door branch, x86-64 debug, with one
QEMU and nothing else running: `mmu: TLB shootdown of
0xffffc000104f3000+0x2000 acknowledged by 2 of 3 CPUs`, `CPU: 1`,
during the interactive harness's `dmesg` after every self-test and
`thrtest` had passed; the rerun passed. **A third, 2026-09-21**, x86-64
debug on the same machine, in a bug-proof boot of the unix-sockets
branch (a mutated `handle.c`, nothing near the MMU): `TLB shootdown of
0xffffc000104af000+0x4000 acknowledged by 2 of 3 CPUs`, 68 s into the
boot, with only that one QEMU running; the mutation's re-run passed and
was caught by the test it targets. Still the shape, still no answer to
which CPU did not answer. That branch changes nothing in
the MMU or the IPI path (futex, signal targeting, libc) and the panic
site is the same, so the second sighting is on the shape and not on the
unit — but a second sighting on a quiet host is what the first was
missing, and the next one should carry the instrumentation this record
asks for elsewhere: which CPU did not answer and what it was doing. It is
the same family as the rows above ("a host holding the vCPU"), and if it
recurs this is where it starts. It is not a test bound: a shootdown that really
never completes is a kernel defect, so widening it would hide the thing
it exists to catch. A re-run distinguishes the two, as everywhere else
here.

**Observed once, not yet on the list: `timer-cancel-sync`'s lower
bound.** `kernel/core/quiescetest.c:960` asserts that a `timer_cancel_sync`
against a callback holding for 20 ms on another CPU took at least
10 ms -- the wait spanned the callback. On 2026-09-21, x86-64 debug on
the developer's machine, during a bug-proof boot of a mutated tree
(the mutation in `devtest.c`'s removal submitter, nothing near timers):
`check failed: sync_ns >= MS(10)`, 23 ms into the test. The clock
starts after `wait_flag(&p->entered)` returns, so a host that holds
this vCPU for more than ten of the callback's twenty milliseconds
between that return and `t0` makes a correct sync look short; a sync
that returned before the callback ended would also fail
`p->done == 1` on the next line, and that passed. One local sighting,
the same family as the rows above, recorded here rather than listed;
if it recurs the fix is to time from `entered` itself, not from a
point this thread reaches later.

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

**A fourth on 2026-09-19**, same assertion, same line, on the libc
shared-tables branch — in `aarch64 BUILD=debug test-guard`, which
passed on an immediate re-run of the same tree. That branch is `libc`
locking, a shell parser fix and documentation; it cannot slow a lockup
sample either. Four in three days now, and re-running remains the
right first move.

**A fifth on 2026-09-23**, on PR #226's first CI run, `build, boot,
analyze (aarch64)`, the plain debug boot — at `lockuptest.c:478`, the
three-CPU variant's `el < LOCKUP_SAMPLE_TIMEOUT_NS + 2 ms` this time
rather than the two-CPU one, on a branch of one report, one probe script
and two Markdown edits. Same family, same answer: re-run.

**A sixth the same day**, on PR #227's CI, `build, boot, analyze
(aarch64)`, the chaos-migrator boot, `lockuptest.c:478` again. That
branch changes `mmap`'s placement, which the lockup sample never calls;
its own racer printed `600 placed from two threads, 0 EEXIST` in the
same boot. Re-run.

**A seventh on 2026-09-24**, locally, x86-64 debug, on the cwd-name
branch (PR #232), `lockuptest.c:478` again (241 ms), with the host's
load average near ten from other work. That branch changes path naming
and procfs, which the lockup sample never calls; the two mutation boots
run straight after it on the same host passed the test, and so did the
re-run of the unmutated tree. The first sighting on x86-64.

**An eighth and a ninth on CI**: PR #229's run 35875986810 (aarch64, the
harness-retry boot, 101 ms) and PR #235's run 35970620814 (aarch64, the
protection-capable-CPU boot, 88 ms), both at `:478`, neither branch touching the
detector. **Taken up by `docs/audit/next-subsystem-lockup-bound.md`**,
which measured the bound under host load (`tools/lockup-busy-probe.py`):
quiet, the sampler takes 5.00-5.12 ms; loaded, it reproduces these
failures, and the excess is time the virtual CPU did not run -- in most
cases one gap between two clock reads holding all of it. The unit
proposes replacing the wall-clock bound with a count of the waits a
sample arms. **Resolved (PR #239)**: the test now checks the count of
waits the sample armed and that neither target answered, with the
targets made unable to answer on both architectures; the only time
bounds left are 1 s hang guards. (Before the build, the test still
checked the bound and re-running was the answer.) A tenth sighting came on that report's
own CI run (PR #238, run 36000131643, aarch64, the harness-retry boot,
101 ms) -- a branch of one report, one probe script and this file.

It is the load-sensitive family this file's list describes, and it is not
*on* the list. The bound is `LOCKUP_SAMPLE_TIMEOUT_NS` plus two
milliseconds of slack, and the slack is what a loaded host eats. Adding
it to the list would mean widening the bound, and that is the trade the
list exists to refuse when the bound is the property: a lockup sample
that answers late is a lockup sample that did not work. What is recorded
instead is the rate — ten in eight days as of 2026-09-24, every one on a
tree that cannot have caused it — because the next unit to hit this
should know it is not the first, and that re-running is the right first
move.

Three CI runs of one branch, 2026-09-17, failed three *different* tests.
The branch was the VMState-layout unit: a compile-time assertion in a
UAPI header, one self-test that runs in 7 ms, and documentation. It
touched nothing in the network stack, the filesystem or the lockup
detector. Recorded together because the three needed three different
answers, and telling them apart is the whole skill this file is about.

## `thrtest` cannot start a thread

**2026-09-19, `aarch64 BUILD=debug test-gic`**, one of three
`env_reader` starts in `env-grow-under-readers`:
`thrtest: FAIL cosmo_thread_start(...) == 0 at line 866`, with the join
of that slot failing after it — two `CHECK`s, one cause. It passed on
an immediate re-run of the same tree, and `test-guard` on the same
architecture and the same build ran `thrtest` clean.

**A second, on CI the same day**, in `build, boot, analyze (aarch64)`
on `096a15d` — a documentation-only commit — in the *protection-capable*
boot this time rather than the GIC one. Same test, same two `CHECK`s,
and the same position: the first reader start after
`env-spawn-under-setenv` has joined its two threads. That job was red
anyway, because `net-harness` tripped the forbidden-marker rule in the
same boot.

Two sightings at one instruction is not a coincidence, and the suspect
is the new case itself: `env-spawn-under-setenv` adds two thread
create/join cycles immediately before a four-thread test, and
`cosmo_thread_join` returns from `thread_clear_tid`, which
`process_thread_exit` calls *immediately before* `thread_exit` — the
ordering the `lx_join` entry above describes. A joiner can therefore
start a new thread while the one it just joined is still being torn
down.

**That hypothesis was wrong, and so was the one after it.** The third
sighting carried the number, and the number settled it:

```
thrtest: FAIL env_reader start at line 883: rc -17
```

`-17` is `EEXIST`. Not the join/teardown race above, and not memory
pressure — `RLIMIT_AS` defaults to 2 GiB and the kernel-allocation
theory that replaced it predicted `-12`. Both were reasoning from
plausibility; one line of instrumentation beat both.

`EEXIST` from a *thread start* points at one place.
`cosmo_thread_start` builds a stack in three syscalls — reserve the
range `PROT_NONE`, `munmap` a hole for the stack and TCB page, `mmap`
that hole back `MAP_FIXED` — and the punch and the fill are two
syscalls with unmapped address space between them. Another thread's
`mmap(NULL, …)` can be handed that gap, because `vm_user_find_free`
looks for exactly such a hole, and this kernel's `MAP_FIXED` refuses
to overwrite rather than replacing, so `space_insert` returns
`-EEXIST` and the loser gets it back from `cosmo_thread_start`.

So this was never a flake. It is a real race, and it belongs in the
section below rather than this file — kept here because this is where
the three sightings were recorded while it was still thought to be
one. `env_churn` is why it appeared now: a test that mallocs
continuously beside threads that start continuously is what the
window needs, and both arrived in this unit.

Fixed in `libc/src/thread.c` by losing the race harmlessly — the
cleanup path already restored the address space exactly, so the
attempt is retried, bounded at 16. The real repair is
`MAP_FIXED` replacing as POSIX says, which would remove the punch
entirely; that is a kernel change, filed in the deferred-work
inventory. The bug-proof forces the loss on *every* attempt rather
than reproducing the natural race: with the retry the suite passes,
with one attempt every thread start in the program fails.

(The first guess, written before the number arrived, was that this
was the `-ENOMEM` thread-stack condition `/etc/rc.test` already
carries a comment about — the one that moved `thrtest` ahead of the
hypervisor section. It is not; that condition is real but is not
this.)

**A fourth sighting, 2026-09-23, after the punch was gone** -- and it is
a *different* race, on the same line. x86-64, one debug boot of a
documentation-only rebase (PR #225's), `env-grow-under-readers`:

```
thrtest: FAIL env_reader start at line 1169: rc -17
```

`MAP_FIXED` replaces now (PR #193), libc's stack fill can no longer
lose to a hole, and the sixteen-attempt retry that masked losses is
gone -- so an `EEXIST` out of a thread start has exactly one source
left, and reading `sys_mmap` finds it: the **non-fixed** path calls
`vm_user_find_free`, which takes and releases the space lock, and then
`vm_user_map_anon`, which takes it again to `space_insert`
(`kernel/syscall/native.c`, the `else` branch; the Linux `mmap` in
`compat/linux/syscalls.c` is the same two calls). Two threads asking for
an anonymous placement at once can both be handed the same hole, and
the loser's insert is `-EEXIST`. `env_churn` mallocs beside a thread
start that reserves, which is two `mmap(NULL, …)` racing, which is what
this test does on purpose. The retry the MAP_FIXED unit removed was
absorbing this race too, without anyone knowing it existed.

Not a flake: a placement and its insertion must be one critical section
at both doors. **Fixed (the mmap-place unit,
`docs/audit/next-subsystem-mmap-place.md`)**: the probe measured about
half of all concurrent placements colliding, both architectures, and
both doors now choose and insert under one hold (M46). `thrtest`'s
`env-grow-under-readers` has nothing left to lose a thread start to.

Two things are still worth keeping. The **printf is not honest under
this failure**: it reports `3 readers` from `ENV_READERS` whatever actually
started, so the line said "3 readers over 400 growths, 0 misses" on a
run where one reader never existed. And `0 misses` from two readers is
a weaker result than the same words from three — the check passed with
less concurrency than it claims. Neither is repaired here; both belong
to the test.

The run is also the first recorded instance of `SHTEST: FAIL 1`
appearing at all — the exit status and the shell's AND-OR
associativity were both fixed on this branch, and before them this
same failure would have printed `SHTEST: PASS`.

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

**The second sighting named the cause, because the trace was kept.**
2026-09-19, x86-64 CI, on a **documentation-only commit** (`1ec27af`,
PR #186 — no code changed since `bcd11d6`, which was green on both
architectures), same assertion, `lockuptest.c:157`. This time the
sampled stack was in the log:

```text
cpu 1: pc 0xffffffff8000589e (nmi, 27488 us ago)
  #0 lock_common                        kernel/core/spinlock.c:51
  #1 spin_lock_irqsave                  kernel/core/spinlock.c:124
  #2 waitqueue_empty                    kernel/scheduler/wait.c:90
  #3 quiesce_note_quiescent_preemptible kernel/core/quiesce.c:118
  #4 x86_trap_dispatch                  kernel/arch/x86_64/trap.c:103
  #5                                    kernel/arch/x86_64/isr.S:145
  #6 spinner_main                       kernel/core/lockuptest.c:71
  #7 thread_trampoline
```

**The spinner was not elsewhere and not descheduled.** It is right
there at frame #6 — the NMI sampled it while an ordinary **interrupt**
was in flight on the same CPU, and the PC was in that handler's trap
tail. Neither mechanism the first sighting offered applies; the
assertion simply has no allowance for the spinner being interrupted,
which a spinning thread with interrupts enabled will be.

**A hypothesis about the window, held as one.** Frame #3 is
`quiesce_note_quiescent_preemptible`, and since PR #181 that call does
more than publish: it takes the waitqueue lock to decide whether to
wake a grace-period waiter (invariant Q18). A longer trap tail is a
wider window in which a sample lands inside it. That is consistent with
this trace and with `lockup-sample` having no sighting in this file
before 2026-09-18, and it is **not measured** — no before-and-after
rate was taken, and two sightings cannot supply one.

So the assertion is now a **known-brittle** one with a named cause,
rather than an unexplained flake: it asserts the sampled PC is in
`spin_here` while sampling a thread that can be interrupted. The trace
already carries what would fix it — `spinner_main` is in the walk — so
a repair exists (accept a PC anywhere in the interrupted thread's
stack, or assert on the trace rather than the leaf PC). It is recorded
here rather than patched inside an unrelated unit.

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

## `lxtest`'s tgkill-after-join, and why eleven markers went missing

**2026-09-19, x86-64 CI, the protection-capable boot, on a
documentation-only commit (`83b42cb`).** The run reported **thirteen**
missing markers -- `SHTEST: PASS`, the musl program, `LINUXTEST: PASS`,
`lxinterp: ok`, `lxdyn: ok` and all eight `lxsig` lines -- while **all
352 self-tests passed** and the kernel shut down with status 0. That
suffix looks like a boot that died early. It was not.

One check failed, and the log says which:

```text
LINUXTEST: FAIL sc3(LX_tgkill, pid, ctid, 0) == -3 (0)
LINUXTEST: FAIL (1 checks)
```

`/etc/rc.linux` runs `lxhello`, then `lxtest || exit 1`, then
everything else. So the one failure accounts for **all thirteen**:

| marker | why it is missing |
| --- | --- |
| `LINUXTEST: PASS` | `lxtest`'s own verdict -- it printed `FAIL` instead |
| `lxdyn: ok`, `lxinterp: ok` | never ran; `lxinterp` is `lxdyn`'s ELF interpreter, so it goes with it |
| eight `lxsig` lines | never ran |
| the musl program | never ran -- it is the last line of `rc.linux` |
| `SHTEST: PASS` | `rc.test` recorded `FAILS=1`, and its verdict line only prints `PASS` at zero |

**Thirteen missing markers, one failed check, and nothing in the output
says so** -- the same defect `docs/audit/next-subsystem-usertest-sections.md`
is about, in a second suite. (The first version of this entry said
twelve markers and "eleven of the twelve", which was a miscount of a
list printed in full four lines above it.)

**The check races a window the kernel documents.** `lxtest.c:746-750`:

```c
CHECKV(lx_join(&g_tidword[0]) == 0, g_tidword[0]);
CHECKV(g_child_tid == ctid && g_child_fs_ok, g_child_tid);
CHECKV(sc0(LX_gettid) == pid, 0);
CHECKV(sc3(LX_tgkill, pid, ctid, 0) == -3, 0);          /* "gone" */
```

`lx_join` waits for the `CHILD_CLEARTID` word to reach zero. The kernel
zeroes that word and wakes the joiner in `thread_clear_tid`, which
`process_thread_exit` calls **immediately before `thread_exit`**
(`kernel/process/process.c:870-871`) -- and the comment above it
records that the wake was deliberately moved to that point, after the
thread stops being counted, so a joiner can immediately create another
thread.

Not being *counted* is not the same as not being *findable*. Between
the `futex_wake` and `thread_exit` completing, the exiting thread still
resolves by tid, so `tgkill(pid, ctid, 0)` can return 0 where the test
demands `-ESRCH`. Two intervening checks are all that normally covers
the window, and on a loaded shared runner they did not.

**A second sighting, 2026-09-19, aarch64 CI** (`1e5b90f`, a
README-and-docs commit on the libc shared-tables branch), identical
line and identical consequence: `LINUXTEST: FAIL sc3(LX_tgkill, pid,
ctid, 0) == -3 (0)`, and with it `SHTEST: PASS` and every `lxsig`
marker. Two sightings on two architectures in one day, both on
commits that cannot have caused it.

**The repair named below is now made** (PR #191). The check waits for
the condition instead of asserting it once: up to 2000 attempts with
a `sched_yield` between them, because the exiting thread needs the
CPU the loop is spinning on. It is no weaker — the bound is finite,
so a kernel that never releases the tid still fails — and it is no
longer a coin toss on how far `thread_exit` has got. It is the
Linux ABI test rather than this branch's subsystem, taken because it
was blocking this branch's aarch64 gate and the repair was already
written down here.

**The kernel's ordering is the deliberate one; the test's assumption is
the wrong part** -- "my join returned, therefore that tid is
unresolvable" was never promised, here or on Linux. The repair is for
the test to wait for the condition it actually means rather than infer
it from the join, which is the same shape as `lockup-sample` above: an
assertion with no allowance for a window the implementation genuinely
has. Made in PR #191, after the second sighting — see above.


## `lockup-*`'s thread count, and a window the implementation has

**`lockup-soft` failed once on 2026-09-20**, AArch64 debug, on the
branch of the virtio-removal unit: `check failed: thread_count() ==
before at line 464`. Six checks in `kernel/core/lockuptest.c` asserted
that, each immediately after joining the threads the test made.

**The assertion was the wrong part, and the kernel's ordering is
deliberate** — the same shape as `lxtest`'s tgkill-after-join above.
`thread_join` returns when the exiting thread calls
`complete(&self->exited)`; `thread_count()` falls in
`thread_unregister`, which runs from the **last** `thread_put`, and the
exiting thread's own reference is dropped by the reaper after it has
switched away (`kernel/scheduler/thread.c`). Between a join returning
and the count falling there is a window, by design, and a one-shot
assertion had no allowance for it.

**What the branch changed was timing, not mechanism.** It registers a
thread-creating self-test (`virtio-remove-inflight`) shortly before
these, so the reaper has company; the window was always there and
nothing in the branch touches the scheduler. The repair is the one this
file prescribes: wait for the condition with a bound
(`threads_settled`, one second, yielding so the reaper gets the CPU the
loop is on). It is no weaker — a test that really leaks a thread still
fails, which the unit proved by removing a `thread_join` and watching
the bound fire — and it is made in the branch it blocked, as `lxtest`'s
was.

The sibling helper in `kernel/device/devtest.c` (`threads_settle_blk`)
already had the bounded shape; that is where this one came from.

**Why the bound has no committed test.** Review asked for one twice.
An automated negative means a test that deliberately leaks a thread
and then waits out the deadline: a second of dead time in every debug
boot, and a thread left running while it decides, in the file whose
subject is detecting stuck CPUs. The bound is proved by mutation
instead — remove a `thread_join` and the helper returns false, which
fails the step — and that is recorded here so an edit to the helper
knows what to re-run. If it ever grows a caller outside
`kernel/core/lockuptest.c` it should move somewhere testable and take
a real test with it.

## `irq-route`'s interrupt count, and the failure it manufactured

**Seen once, 2026-09-20**, AArch64 debug, on the virtio-removal branch:
`irq-route ... FAIL: check failed: hits >= 5` — a count of PIT
interrupts over a fixed `udelay(50000)` (200 Hz, so ten expected and
five demanded). That is the family this file exists for: N things after
a fixed interval, which measures the host when the host is busy. Not
reproduced in three further runs with the same image, and the same tree
with the branch's new device absent (`QEMU_RMDISK=0`) passed three for
three as well, so it is a sighting and not a consequence of the branch.

**A second, 2026-09-24**, x86-64 debug, locally, on the mount-rel branch
(`hits >= 5` at `irqtest.c:247`, 51 ms), in one of the unit's mutation
boots -- a mutation of `vfs_umount_at`, which no interrupt path calls.
The boots before and after it on the same host passed the test. The
first on x86-64; still the family, still a sighting.

**It also manufactured a second failure, and that part is a real
defect in the test.** `irq-affinity` failed immediately after with
`irq_request(...) == 0` returning `-EBUSY`: `irq-route`'s `CHECK`
returns the moment the count fails, *before* its `irq_disable` and its
release, so the GSI stays held and the next test to request one is
refused. One flake presented as two failures, and the second names a
subsystem it has nothing to do with. Naming it here rather than fixing
it: the repair is the usual one for this file — a test that acquires a
resource releases it on every exit — and it belongs to whoever next
touches `kernel/interrupt/irqtest.c`.

## The count

`net-harness` sightings live here, in one place, because six different
figures for one number appeared across four files in a single day —
the reports, the inventory row, this file twice, and a comment in
`nettest.c` — each correct when written and none of them corrected
together. Anything that needs the number refers to this section rather
than repeating it.

**Eighty-four, to 2026-09-22**, across CI and this developer's machine, on
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
| PR #180's own CI run | observed, aarch64: `connect 0 in **803 ms**`, `sent 12`, `outstanding 12 then 12`, **`retransmits +0`** -- one connection carrying nothing a seventh time, and the THIRD sighting with no retransmission |
| PR #182's own CI run | observed, aarch64, **the first sighting with the probe**: `connect 0 in 894 ms`, `sent -104`; host side `[deadline, ESTABLISHED]` with slirp answering in 1 ms. See below -- this is the one that names where to look |
| **Local x86-64, 2026-09-19** | observed while verifying an unrelated module unit: `connect 0 in 743 ms`, `sent -104`, `segs_out +2 retransmits +0`, and `tcp_conns=3` -- the probe ran. **Its reading was lost**: the roster goes to the runner's stdout and the run was grepped down to PASS/FAIL. First LOCAL sighting since the probe landed, and the instrument's output was thrown away by the person who built it |
| PR #186's own CI run | observed, aarch64 (the protection-capable-CPU job), on a **documentation-only commit** (`a0558b6`): `connect 0 in 791 ms`, `sent -104`, `segs_out +2 retransmits +0 rsts_in +1`; host side `127.0.0.1:36662 accepted at 91.9s, 0 byte(s)`, **`[deadline, ESTABLISHED]`**, `slirp probe: connect 1 ms, echo 1 ms`, gave up 20.0s later. **The probe's reading reproduced** -- see below |
| PR #187's own CI run | observed, aarch64 (the protection-capable-CPU job), on another **documentation-only commit** (`83b42cb`): `connect 0 in 947 ms`, **`sent 12`**, `recv -104`, `outstanding 12 then 12`, `segs_out +3 retransmits +0 rsts_in +1`; host side `127.0.0.1:52290 accepted at 91.8s, **0 byte(s)**`, `[deadline, ESTABLISHED]`, `slirp probe: connect 1 ms, echo 1 ms`, gave up 20.0s later. Row one a **third** time, and the first where the guest's write succeeded and the host still read nothing -- see below |
| PR #187's own CI run, the very next one | observed, aarch64, the **GICv3** job this time (`9b5b5f9`, documentation-only): `connect 0 in 755 ms`, `sent -104`, `outstanding 0 then 0`, `segs_out +2 retransmits +0 rsts_in +1`; host side `127.0.0.1:37472 accepted at 82.4s, 0 byte(s)`, `[deadline, ESTABLISHED]`, `slirp probe: connect 1 ms, echo 1 ms`. Row one a **fourth** time, on a third distinct aarch64 job |
| PR #188's own CI run | observed, **x86-64** (`f446890`, documentation-only): `connect 0 in 890 ms`, `sent -104`, `outstanding 0 then 0`, `segs_out +2 retransmits +0 rsts_in +1`; host side `127.0.0.1:48970 accepted at 74.8s, 0 byte(s)`, `[deadline, ESTABLISHED]`, `slirp probe: connect 1 ms, echo 1 ms`. **The first probe reading on x86-64**, and row one a fifth time -- see below |
| PR #189's own CI run | observed, aarch64 (`2559c32`): **`connect 0 in 529 ms`** -- the FASTEST connect recorded, and below the band this file had been quoting -- `sent 12`, `recv -104`, `outstanding 12 then 12`, `segs_out +3 retransmits +0 rsts_in +1`; host side `[deadline, ESTABLISHED]`, probe 1 ms / 1 ms. Row one a sixth time |
| PR #189's own CI run, the next one | observed, aarch64 (`a81365c`, documentation-only): `connect 0 in 1382 ms`, `sent -104`, `outstanding 0 then 0`, `segs_out +3` **`retransmits +1`** `rsts_in +1`; host side `[deadline, ESTABLISHED]`, probe 1 ms / 1 ms. Row one a seventh time, and the first retransmission in nine sightings |
| PR #190's own CI run | observed, aarch64 (`a909ba8`), on a branch whose ONLY change is one new Markdown file: `connect 0 in 623 ms`, `sent -104`, `outstanding 0 then 0`, `segs_out +2 retransmits +0 rsts_in +1`; host side `[deadline, ESTABLISHED]`, probe 1 ms / 1 ms. Row one an eighth time |
| PR #191's own CI run | observed, aarch64 (`f89a240`, documentation-only): **`connect 0 in 493 ms`** -- a new fastest, below the floor set three sightings earlier -- `sent -104`, `outstanding 0 then 0`, `segs_out +2 retransmits +0 rsts_in +1`; host side `[deadline, ESTABLISHED]`, probe 1 ms / 1 ms. Row one a ninth time |
| PR #191's own CI run, a later one | observed, **x86-64** (`7ca342b`): `connect 0 in 715 ms`, `sent -104`, `outstanding 0 then 0`, `segs_out +2 retransmits +0 rsts_in +1`; host side `[deadline, ESTABLISHED]`, probe 1 ms / 1 ms. Row one a tenth time, and **only the second x86-64 reading** after sighting thirty-five |
| PR #191's own CI run, the protection-capable aarch64 job | observed, aarch64 (`096a15d`, a **documentation-only** commit): **`connect 0 in 1478 ms`** -- a new slowest, where sighting thirty-nine set a new fastest -- `sent -104`, `recv -1`, `pending error -104`, `outstanding 0 then 0`, `segs_out +3` **`retransmits +1`** `rsts_in +1`; host side `127.0.0.1:55062 accepted at 92.9s, 0 byte(s)`, `[deadline, ESTABLISHED]`, `slirp probe: connect 1 ms, echo 1 ms`, gave up at 112.9s. Row one an eleventh time, and the **second `retransmits +1` ever recorded**, after sighting thirty-seven -- named rather than counted, because a count of "sightings since" is the figure this section keeps getting wrong. The self-test also blew its budget at 21971 ms against 8000 ms, which is the waiting, not a second fault |
| PR #191's own CI run, the same job re-run | observed, aarch64 (`8aff8ad`), on the **re-run of the job above** -- so twice on one commit: `connect 0 in 668 ms`, `sent -104`, `recv -1`, `pending error -104`, `outstanding 0 then 0`, `segs_out +2 retransmits +0 rsts_in +1`; host side `127.0.0.1:53036 accepted at 91.9s, 0 byte(s)`, `[deadline, ESTABLISHED]`, `slirp probe: connect 1 ms, echo 1 ms`, gave up at 111.9s. Row one a twelfth time, and the first time a **re-run reproduced it on the same commit** -- which is worth more than another reading, because "re-run it" has been this file's standing advice |
| PR #192's own CI run | observed, aarch64 (`876eec8`), on a report branch whose **only** file is one Markdown document: `connect 0 in 715 ms`, **`sent 12`**, `recv -104`, `pending error -104`, `outstanding 12 then 12`, `segs_out +3 retransmits +0 rsts_in +1`; host side `127.0.0.1:49176 accepted at 89.0s, 0 byte(s)`, `[deadline, ESTABLISHED]`, gave up at 109.0s. Row one a thirteenth time, the `sent 12, never acknowledged` variant, and the **first probe reading that is not 1 ms / 1 ms**: `slirp probe: connect 1 ms, echo 2 ms`. One millisecond is not a mechanism, and it is recorded because this file's rule is to write the numbers down, not because it means anything yet |
| PR #192's own CI run, the next commit | observed, aarch64 (`3ff1df2`), the **immediately following** commit on the same one-document branch: `connect 0 in 676 ms`, **`sent 12`**, `recv -104`, `outstanding 12 then 12`, `segs_out +3 retransmits +0 rsts_in +1`; host side `127.0.0.1:33140 accepted at 91.9s, 0 byte(s)`, `[deadline, ESTABLISHED]`, `slirp probe: connect 1 ms, echo 1 ms`, gave up at 111.9s. Row one a fourteenth time, the same `sent 12` variant as the row above, and the probe is back to 1 ms / 1 ms — so the 2 ms in sighting forty-three was a single reading and nothing more |
| PR #193's own CI run, twice in one run — x86-64 and aarch64 | observed on both architectures of the same run (`dc52e54`), in the protection-capable and GIC boots respectively: x86-64 `connect 0 in 1274 ms`, aarch64 `connect 0 in 1479 ms`; both `sent -104`, `outstanding 0 then 0`, `segs_out +3` **`retransmits +1`** `rsts_in +1`, host side `0 byte(s)`, `[deadline, ESTABLISHED]`, `slirp probe: connect 1 ms, echo 1 ms`. Row one a fifteenth and sixteenth time. **Both carry a retransmission**, which had been recorded only twice before in the whole file — two of them in one run, on two architectures, with otherwise identical counters. Recorded together because they are one run; no claim is made from the pair beyond that |
| PR #195's own CI run, the head with the panic fix | observed, aarch64 (`f560ab8`), the plain debug boot: `connect 0 in 1266 ms`, `sent -104`, `recv -1`, `pending error -104`, `outstanding 0 then 0`, `segs_out +3` **`retransmits +1`** `rsts_in +1`; host side `127.0.0.1:36528 accepted at 93.9s, 0 byte(s)`, `[deadline, ESTABLISHED]`, `slirp probe: connect 1 ms, echo 2 ms`, gave up at 113.9s. Row one a seventeenth time, on a memory-syscall branch that touches no network code — the x86-64 job of the same run passed. The fourth `retransmits +1` in the last five sightings, where the whole file before them held two; and `echo 2 ms` for the second time, after sighting forty-three. Two counts, written down, no claim from either |
| PR #199's own CI run | observed, aarch64 (`a82c833`), the **other interrupt controller** boot: `connect 0 in 526 ms`, `sent -104`, `recv -1`, `pending error -104`, `outstanding 0 then 0`, `segs_out +2 retransmits +0 rsts_in +1`; host side `127.0.0.1:60998 accepted at 92.9s, 0 byte(s)`, **`[deadline, ESTABLISHED]`**, `slirp probe: connect 1 ms, echo 2 ms`, gave up at 112.9s. Row one an eighteenth time, on a branch that touches the block layer, one driver and the tests and no network code at all; the x86-64 job of the same run passed. `echo 2 ms` for the third time (sightings forty-three and forty-seven), and `retransmits +0` after four of the last five carried one — both written down, neither claimed |
| PR #199's own CI run, a later commit | observed, aarch64 (`9e93d89`), the **plain debug** boot this time: `connect 0 in 931 ms`, **`sent 12`**, `recv -104`, `pending error -104`, `outstanding 12 then 12`, `segs_out +3 retransmits +0 rsts_in +1`; host side `127.0.0.1:35338 accepted at 84.7s, 0 byte(s)`, **`[deadline, ESTABLISHED]`**, `slirp probe: connect 1 ms, echo 1 ms`, gave up at 104.7s. Row one a nineteenth time, and **this PR's second** — the sighting above was the other-interrupt-controller boot of an earlier commit, this one the plain boot of a later one, so two distinct aarch64 jobs on one branch. The `sent 12` variant, where the guest's write succeeded and the host still read nothing |
| `main` @ `26c1b5f`, twice in one run — aarch64 and x86-64 | observed on 2026-09-20 on both architectures of one `main` run (35493372819), read from the logs a day later rather than watched: aarch64 (plain debug) **`connect -104 in 849 ms`** -- the connect itself reset, `sent -1`, `recv -1`, `pending error -104`, `sndbuf free 0`, `outstanding 0 then 0`, `segs_out +2 retransmits +0 rsts_in +1`; x86-64 (protection-capable) `connect 0 in 914 ms`, `sent -104`, `recv -1`, `pending error -104`, `outstanding 0 then 0`, `segs_out +2 retransmits +0 rsts_in +1`; both host sides `0 byte(s)`, `[deadline, ESTABLISHED]`, probes 2 ms / 2 ms and 1 ms / 1 ms. The aarch64 one is the connect-reset shape of PR #171's first; the x86-64 one is row one |
| `main` @ `8e2cc55` | observed, aarch64, the plain debug boot of a `main` run on 2026-09-20 (35497897134), read from the logs a day later: `connect 0 in 911 ms`, **`sent 12`**, `recv -104`, `pending error -104`, `outstanding 12 then 12`, `segs_out +3 retransmits +0 rsts_in +1`; host side `127.0.0.1:37554 accepted at 93.9s, 0 byte(s)`, `[deadline, ESTABLISHED]`, `slirp probe: connect 1 ms, echo 1 ms`, gave up at 113.9s. Row one, the `sent 12` variant |
| `main` @ `238ac40` | observed, aarch64, the protection-capable boot of a `main` run on 2026-09-20 (35502406994), read from the logs a day later: `connect 0 in 1219 ms`, `sent -104`, `recv -1`, `pending error -104`, `outstanding 0 then 0`, `segs_out +3` **`retransmits +1`** `rsts_in +1`; host side `127.0.0.1:57212 accepted at 94.9s, 0 byte(s)`, `[deadline, ESTABLISHED]`, `slirp probe: connect 1 ms, echo 2 ms`, gave up at 114.9s. Row one, with a SYN retransmission |
| `main` @ `2332d59` | observed, aarch64, the protection-capable boot of the file-regions merge's `main` run on 2026-09-21 (35577048217), read from the logs the same day: `connect 0 in 494 ms` -- one millisecond above the fastest recorded -- `sent -104`, `recv -1`, `pending error -104`, `outstanding 0 then 0`, `segs_out +2 retransmits +0 rsts_in +1`; host side `127.0.0.1:38114 accepted at 93.9s, 0 byte(s)`, `[deadline, ESTABLISHED]`, `slirp probe: connect 1 ms, echo 1 ms`, gave up at 113.9s. Row one |
| PR #203's own CI run | observed, aarch64, the GICv3 boot (`b7dcf9b`, run 35585299449, the shared-futex build, no network file in its diff): `connect 0 in 1395 ms`, `sent -104`, `recv -1`, `pending error -104`, `outstanding 0 then 0`, `segs_out +3` **`retransmits +1`** `rsts_in +1`; host side `127.0.0.1:50412 accepted at 94.1s, 0 byte(s)`, `[deadline, ESTABLISHED]`, `slirp probe: connect 1 ms, echo 2 ms`, gave up at 114.1s. Row one, with a SYN retransmission. First recorded in prose only, as "sighting forty-one" counted from the prose while this table stood at forty-nine -- the loss the paragraph below warns of -- and in the table since the same day |
| PR #203's own CI run, a later commit | observed, aarch64, the plain debug boot (`a0a0c38`, run 35586739597): **`connect 0 in 286 ms`** -- the fastest connect this file has recorded, by two hundred milliseconds -- **`sent 12`**, `recv -104`, `pending error -104`, `outstanding 12 then 12`, `segs_out +3 retransmits +0 rsts_in +1`; host side `127.0.0.1:39430 accepted at 95.0s, 0 byte(s)`, `[deadline, ESTABLISHED]`, `slirp probe: connect 2 ms, echo 1 ms`, gave up at 115.0s. Row one, the `sent 12` variant |
| PR #203's own CI run, a documentation-only commit | observed, aarch64, the plain debug boot (`5122041`, run 35587325294, a commit that changes one paragraph of one report): `connect 0 in 1353 ms`, **`sent 12`**, `recv -104`, `pending error -104`, `outstanding 12 then 12`, `segs_out +4` **`retransmits +1`** `rsts_in +1`; host side `127.0.0.1:38064 accepted at 93.8s, 0 byte(s)`, `[deadline, ESTABLISHED]`, `slirp probe: connect 1 ms, echo 1 ms`, gave up at 113.9s. Row one, the `sent 12` variant, and the first `sent 12` reading that also retransmitted |
| `main` @ `2f79ea5`, twice in one run — x86-64 and aarch64 | observed on 2026-09-21 on both architectures of the shared-futex merge's `main` run (35595160535), each the plain debug boot: x86-64 `connect 0 in 640 ms`, `sent -104`, `recv -1`, `pending error -104`, `outstanding 0 then 0`, `segs_out +2 retransmits +0 rsts_in +1`, host side `127.0.0.1:39696 accepted at 91.1s, 0 byte(s)`; aarch64 `connect 0 in 1358 ms`, `sent -104`, `recv -1`, `pending error -104`, `outstanding 0 then 0`, `segs_out +3` **`retransmits +1`** `rsts_in +1`, host side `127.0.0.1:51800 accepted at 95.0s, 0 byte(s)`; both `[deadline, ESTABLISHED]`, `slirp probe: connect 1 ms, echo 1 ms`. Row one on both, the second run to show it on both architectures at once (PR #193's was the first) |
| PR #204's own CI run | observed, aarch64, the plain debug boot (`3c170f5`, job 106306472451, a driver-test branch): `connect 0 in 1223 ms`, **`sent 12`**, `recv -104`, `pending error -104`, `outstanding 12 then 12`, `segs_out +4` **`retransmits +1`** `rsts_in +1`; host side `127.0.0.1:59216 accepted at 95.0s, 0 byte(s)`, `[deadline, ESTABLISHED]`, `slirp probe: connect 1 ms, echo 1 ms`, gave up at 115.0s. Row one, the `sent 12` variant with a retransmission |
| PR #205's own CI run | observed, aarch64, the plain debug boot (`e51f07e`, run 35596999894, a one-document branch): `connect 0 in 606 ms`, **`sent 12`**, `recv -104`, `pending error -104`, `outstanding 12 then 12`, `segs_out +3 retransmits +0 rsts_in +1`; host side `127.0.0.1:33812 accepted at 95.9s, 0 byte(s)`, `[deadline, ESTABLISHED]`, `slirp probe: connect 1 ms, echo 1 ms`, gave up at 116.0s. Row one, the `sent 12` variant |
| PR #207's own CI run | observed, **x86-64**, the plain debug boot (`89bd3d5`, run 35611499251, the unix-sockets build, whose diff touches no inet path): `connect 0 in 1008 ms`, `sent -104`, `recv -1`, `pending error -104`, `outstanding 0 then 0`, `segs_out +3` **`retransmits +1`** `rsts_in +1`; host side `127.0.0.1:38154 accepted at 75.2s, 0 byte(s)`, `[deadline, ESTABLISHED]`, `slirp probe: connect 1 ms, echo 1 ms`, gave up at 95.2s. Row one |
| `main` @ `92f5bd2` | observed, aarch64, the protection-capable boot of the unix-sockets report merge's `main` run on 2026-09-21 (35601645471), read from the logs afterwards: **`connect -104 in 1327 ms`** -- the connect itself reset, `sent -1`, `recv -1`, `pending error -104`, `sndbuf free 0`, `outstanding 0 then 0`, `segs_out +3` **`retransmits +1`** `rsts_in +1`; host side `127.0.0.1:43980 accepted at 74.7s, 0 byte(s)`, `[deadline, ESTABLISHED]`, `slirp probe: connect 1 ms, echo 1 ms`, gave up at 94.7s. The connect-reset shape of PR #171's first and `26c1b5f`'s aarch64, this time with a retransmission |
| `main` @ `19f508c` | observed, aarch64, the plain debug boot of the named-pipes report merge's `main` run on 2026-09-21 (35620649878, a documentation-only commit), read from the logs afterwards: `connect 0 in 1213 ms`, `sent -104`, `recv -1`, `pending error -104`, `outstanding 0 then 0`, `segs_out +3` **`retransmits +1`** `rsts_in +1`; host side `127.0.0.1:35822 accepted at 96.0s, 0 byte(s)`, `[deadline, ESTABLISHED]`, `slirp probe: connect 2 ms, echo 2 ms`, gave up at 116.1s. Row one |
| PR #209's own CI run | observed, aarch64, the plain debug boot (`84df382`, run 35631426307, the named-pipes build, whose diff touches no inet path): `connect 0 in 638 ms`, `sent -104`, `recv -1`, `pending error -104`, `outstanding 0 then 0`, `segs_out +2 retransmits +0 rsts_in +1`; host side `127.0.0.1:38752 accepted at 95.0s, 0 byte(s)`, `[deadline, ESTABLISHED]`, `slirp probe: connect 1 ms, echo 1 ms`, gave up at 115.0s; the self-test blew its budget at 21128 ms against 8000 ms, which is the waiting. Row one |
| PR #209's own CI run, a later commit, twice in one run — x86-64 and aarch64 | observed on both architectures of one run (`fd8beaa`, run 35634418562, a **documentation-only** commit: five files of prose restating one rule), each the plain debug boot: x86-64 `connect 0 in 926 ms`, `sent -104`, `recv -1`, `pending error -104`, `outstanding 0 then 0`, `segs_out +2 retransmits +0 rsts_in +1`, host side `127.0.0.1:43012 accepted at 82.8s, 0 byte(s)`, probe 1 ms / 1 ms, gave up at 102.8s; aarch64 `connect 0 in 891 ms`, the same returns and counters, host side `127.0.0.1:56710 accepted at 95.1s, 0 byte(s)`, probe 2 ms / 2 ms, gave up at 115.1s; both `[deadline, ESTABLISHED]`. Row one on both, the third run to show it on both architectures at once (PR #193's and `2f79ea5`'s before it), and this PR's second and third sightings |
| PR #209's own CI run, a fourth sighting -- the GICv3 boot | observed, aarch64, the **GICv3** boot (`make test-gic`) rather than the plain one (`69fceeb`, run 35636508451, a commit that adds one diagnostic line to a hypervisor self-test and a paragraph to this file): `connect 0 in 1274 ms`, `sent -104`, `recv -1`, `pending error -104`, `outstanding 0 then 0`, `segs_out +3` **`retransmits +1`** `rsts_in +1`; host side `127.0.0.1:33110 accepted at 94.9s, 0 byte(s)`, `[deadline, ESTABLISHED]`, `slirp probe: connect 1 ms, echo 1 ms`, gave up at 114.9s. Row one; the plain boot of the same job had passed. Four sightings on one PR's six runs, none of whose diffs touch the network |
| PR #210's own CI run -- **x86-64** | observed, x86-64, the plain debug boot (`94afdf9`, run 35674822582, a one-document branch: the device-readiness report): `connect 0 in 1166 ms`, `sent -104`, `recv -1`, `pending error -104`, `outstanding 0 then 0`, `segs_out +3` **`retransmits +1`** `rsts_in +1`; host side `127.0.0.1:54818 accepted at 93.3s, 0 byte(s)`, `[deadline, ESTABLISHED]`, `slirp probe: connect 2 ms, echo 1 ms`, gave up at 113.3s. Row one |
| `main` @ `eadba84` | observed, aarch64, the **GICv3** boot of the named-pipes merge's `main` run on 2026-09-22 (35674273649), read from the logs afterwards: `connect 0 in 1475 ms`, `sent -104`, `recv -1`, `pending error -104`, `outstanding 0 then 0`, `segs_out +4` **`retransmits +1`** `rsts_in +1`; host side `127.0.0.1:37448 accepted at 94.1s, 0 byte(s)`, `[deadline, ESTABLISHED]`, `slirp probe: connect 1 ms, echo 2 ms`, gave up at 114.1s; the plain boot of the same job had passed. Row one, and the second sighting in a GICv3 boot (PR #209's fourth was the first) |
| this developer's machine, the device-readiness build | observed, x86-64, the plain debug boot on 2026-09-22 (the clean run before that unit's mutation runs, on `6b3ee0a` plus a test-only commit): `connect 0 in 574 ms`, `sent -104`, `recv -1`, `pending error -104`, `outstanding 0 then 0`, `segs_out +2 retransmits +0 rsts_in +1`; host side `127.0.0.1:50389 accepted at 78.4s, 0 byte(s)`, `[deadline, ESTABLISHED]`, `slirp probe: connect 12 ms, echo 3 ms`, gave up at 98.4s. Row one, off CI |
| PR #211's own CI run | observed, aarch64, the **GICv3** boot (`74e208d`, run 35680602142, the device-readiness build; its diff touches the tap's read path and no inet code): `connect 0 in 1178 ms`, `sent -104`, `recv -1`, `pending error -104`, `outstanding 0 then 0`, `segs_out +3` **`retransmits +1`** `rsts_in +1`; host side `127.0.0.1:40702 accepted at 95.0s, 0 byte(s)`, `[deadline, ESTABLISHED]`, `slirp probe: connect 2 ms, echo 1 ms`, gave up at 115.0s; the plain boot of the same job had passed. Row one, the third in a GICv3 boot |
| this developer's machine, the percpu-migration build, **under the chaos migrator**, twice | observed, x86-64, two of the first four `make test-chaos` boots of the final tree on 2026-09-22 (`c3beadc6` and before): `connect 0 in 1200 ms` / `connect 0 in 1309 ms`, `sent 12 in 0 ms`, `recv -104`, `pending error -104`, `outstanding 12 then 12`, `segs_out +4` **`retransmits +1`** `rsts_in +1`; host side `127.0.0.1:59577 accepted at 83.7s, 0 byte(s)` / `127.0.0.1:59768 accepted at 79.7s, 0 byte(s)`, `[deadline]`, `slirp probe: connect 7 ms, echo 2 ms`. The guest's timeline is the family's (a SYN retransmitted after a second, the data segment lost, a reset), no guest-side detector fired (the READY-stall, sleep-overshoot and tick-gap detectors were in the image), and the plain debug boot of the same tree passed -- but two in four is above the family's rate, and a migrator that moves the retransmit timer's waker and the receive worker between CPUs is the one new variable. Recorded as the family with that caveat; a sighting on a plain boot of this tree would make it the tree's. |
| this developer's machine, the percpu-migration build, **under the chaos migrator**, twice more | observed, one x86-64 and one AArch64 `make test-chaos` boot of the final tree on 2026-09-22 (`7b6b8b79` and the commit before): `connect 0 in 905 ms` / `connect 0 in 867 ms`, `sent 12 in 0 ms`, `recv -104`, `outstanding 12 then 12`, `segs_out +3` **`retransmits +0`** `rsts_in +1`; host side accepted, `0 byte(s)`, `[deadline]`. Four of thirteen chaos boots against none of the plain ones on this tree. First read as a guest that never retransmitted; read again, `recv -104 in 0 ms` says the reset was already there when the guest asked, right after its send -- the family's reset, arriving after the send instead of before it, and nothing left to retransmit. The harness's failure line now prints the connection's retransmit timer and work state on a short line of its own (`NETTEST: client state:`), which on the CI sighting below showed the pcb already detached by the reset. The family, at a higher rate under the migrator. |
| PR #213's own CI run | observed, x86-64, the **protection-capable** boot (`test-guard`, `7e0c5147`, run 35709652208): `connect 0 in 1252 ms`, `sent -104`, `recv -1`, `pending error -104`, `outstanding 0 then 0`, `segs_out +3` **`retransmits +1`** `rsts_in +1`; host side `127.0.0.1:40428 accepted at 74.7s, 0 byte(s)`, `[deadline, ESTABLISHED]`, `slirp probe: connect 1 ms, echo 1 ms`; the new state line: `rexmit timer state -1 ... pcb state -1` -- the pcb was already detached by the reset when the harness looked. |
| PR #213's own CI run, again | observed, aarch64, the **GICv3** boot (`test-gic`, `439d543c`, run 35715715021): the family's signature, host side `127.0.0.1:40928 accepted at 102.0s, 0 byte(s)`, `[deadline, ESTABLISHED]`. |
| The balancer report's measuring boot | observed, x86-64, this developer's machine, a boot carrying `tools/sched-balance-probe.py`: the shape where the guest **sent** the bytes: `sent 12 in 0 ms, recv -104 in 0 ms, pending error -104, ... segs_out +3 retransmits +0 rsts_in +1`. The probe touches the scheduler's tick and nothing in the network stack, and the next boot of the same tree passed. |
| `main` @ e2f3d2b6 | observed, aarch64, the **protection-capable** boot (run 35719533086, the percpu-migration unit's own merge to `main`): again the shape where the guest **sent** the bytes, `sent 12 in 0 ms, recv -104 in 0 ms, pending error -104, ... segs_out +3 retransmits +0 rsts_in +1`, rexmit timer state 0 on cpu 0. The x86-64 job of the same run passed. |
| PR #214's own CI run | observed, aarch64, the **protection-capable** boot (run 35724183483, `12d666b9`, a **documentation-and-tool-only** branch): the same shape again, `sent 12 in 0 ms, recv -104 in 0 ms, pending error -104, ... segs_out +3 retransmits +0 rsts_in +1`. |
| PR #215's own CI run | observed, aarch64, the **plain debug** boot (run 35725274548, this very record's branch at `34f46c96`, a revision that changed documentation only): the **other** shape, `connect 0 in 1379 ms, sent -104 in 0 ms, recv -1 in 0 ms, pending error -104, outstanding 0 then 0, segs_out +3` **`retransmits +1`**. The record of the flake was failed by the flake, and by its other half. |
| PR #215's own CI run, again | observed, aarch64, the **protection-capable** boot (run 35731929745, `7d161b0a`): `connect 0 in 670 ms, sent -104 in 0 ms, recv -1 in 0 ms, pending error -104, outstanding 0 then 0`. The `el2-guest-irq-queue` fix in the same revision held on that boot; this is the other flake. |
| PR #216's own CI run | observed, x86-64, the **chaos migrator** boot (run 35734557638, `d3ca6d6c`, the balancer build): `connect 0 in 637 ms, sent -104 in 0 ms, recv -1 in 0 ms, pending error -104, outstanding 0 then 0, segs_out +2`. The balance benchmark in that same boot read 104%, so the boot was healthy apart from this. |

### Recovered sightings: the retry does not stop the count

Since `docs/audit/next-subsystem-nettest-retry.md`, the guest's
back-connection runs the exchange up to three times, so **most
occurrences of this flake now end in a passing boot**. They are still
occurrences, and the reason this section exists is that a flake which
stops being visible is a flake that stops being fixed.

A recovered sighting prints, in the boot log:

```text
NETTEST: client attempt 1 of 3 failed
NETTEST: client ok (attempt 2 of 3)
```

so `grep 'client ok (attempt [2-9]'` finds every one of them, and the
failed attempt's full diagnostic block is above it. **Add them to the
table above like any other sighting**, with `recovered` in the row, and
they count towards the same total: the number at the head of this
section is occurrences of the *defect*, not of the red CI run it used to
cause. A row that says `recovered` and a row that says nothing are both
one sighting.

The distinction worth recording per row is how far the guest got before
the reset, which the failed attempt's line still carries — `sent 12`,
`sent -104`, or a reset `connect` — since that is what the shapes table
below counts.

**A row's prose must not borrow the words the tally counts.** The
multiplicity of a row is read from "twice" and "three times" in it, so a
row that merely *mentions* either -- "twice on one PR", or a note about
the convention itself -- counts itself twice or three times. Both
happened while sighting forty-nine was being written, at 50 and then at
51, and both were caught by recomputing rather than by reading. Say "a
second" or "this PR's second" in prose and leave the two words to the
count.

Sixty-nine entries, eighty-four occurrences -- and the table is the tally,
so a sighting recorded only in prose below is a sighting this section
has lost (it happened once more on 2026-09-21, and the row is above).
The first five rows are inherited from the row that recorded them and
are not independently re-verified here. The last sixty rows carry
the instrument's reading, fifty-one of them watched as they happened
and nine of `main`'s read from the logs afterwards (**this split is the
one figure here the table does not encode**: it is which runs were being
watched when they failed, which no row records, so it is carried forward
as rows are added rather than recomputed like the rest): PR #167's carries
the host's `accepted at 92.0s, 0 of 12 bytes`, and the **sixty-nine
instrumented** occurrences behind the other fifty-nine rows carry the
guest's side. Rows and occurrences differ because **fourteen** rows hold
more than one sighting; the shapes table below is per *sighting* and
is the one to count from.

(**Every figure in this paragraph is computed from the table, not
carried forward.** They were wrong before sighting twenty-five because
each update incremented them instead of counting the rows -- and wrong
again at sighting thirty-four, when three of them were incremented and
the instrumented pair and the multi-sighting row count were not. The
lesson keeps being the same one: a count that appears in prose beside a
table is a count somebody has to re-derive, so re-derive all of them or
none.)

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
path as the defect: in two of the nine rows above `ksock_sendto`
returned 12, a segment went out, and the host still saw nothing — so
"the twelve bytes were never written" does not describe those. **And
the successful send is not the exception this paragraph once made it
sound.** Counted across the tally above rather than across the nine rows
here: of the eighty-four occurrences, twenty record `sent 12`,
forty-seven record `sent -104`, two record `sent -1` (the connect itself
reset, so nothing was ever sent) and fifteen record no send value at
all. One row holds one of each, which is why this is counted per
occurrence and not per row. So roughly a third of
the sightings that name a send at all have the bytes on the wire, and
both shapes are current — 2026-09-22 produced each of them. This
paragraph was written when the successful send was one instance, and
nothing re-read it against the growing table until then. It
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

- the signature is the one this row had recorded twenty times **by
  then**, and has recorded at every sighting since (*The count* owns
  the running figure), including on trees with no code at all: an
  inbound reset (`rsts_in +1`), the guest's twelve bytes
  unacknowledged, the host's `accept` having succeeded;
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
request. The observed connects were then **597, 787, 803, 1031, 1089, 1116 and
1510 ms** — a spread of two and a half to one — and **three of the seven
had no retransmission at all**. Four of seven having exactly one
retransmission is what a three-sample reader saw as a timer. It is not
one, and each new sighting has moved that ratio further from one.

**And the floor keeps moving.** Sighting thirty-six came in at
`529 ms` against a previous fastest of 597; sighting thirty-nine at
**`493 ms`**. Every statement of a band in this file and in
`docs/audit/next-subsystem-nettest-probe.md` has been true when
written and wrong at the bottom within a few sightings — 597, then
529, now **493 to 1510 ms**. Twice in one day is enough to stop
treating the lower bound as a property: **the range has widened at
both ends every time it has been tested**, which is the opposite of
what a timer would do, and the number to quote is the one in this
paragraph on the day it is read. The retransmission ratio moved too —
sighting thirty-seven is the first `retransmits +1` in nine sightings.

The roster is **seven-for-seven on one connection carrying nothing**,
and that remains the only part of this defect that has never varied.
When the next theory arrives, that is the line it has to explain.

**And the grace bound held in production.** The harness gave up at
108.7s -- twenty seconds after the accept, the receive budget plus the
grace -- rather than burning the 69.3s that remained. The run failed at
153.8s, inside its 180 s timeout, and reported *only* the harness
markers: the five unrelated missing markers that sighting twenty-three's
run produced are gone. That is the regression fix working on the exact
failure that exposed it.

**Sighting thirty answered the question the probe was built for, on the
probe's first outing.** Both halves of the connection, at the same
moment, for the first time in this file:

```
guest: connect 0 in 894 ms, sent -104, segs_out +2 retransmits +0 rsts_in +1
host : 1 connection(s): 127.0.0.1:34378 accepted at 92.0s, 0 byte(s): b''
       [deadline, ESTABLISHED]; slirp probe: connect 1 ms, echo 1 ms
```

Read against the table written down **before** the measurement
(`docs/audit/next-subsystem-nettest-probe.md`), this is row one:

- **slirp was healthy.** A connection through the *same* slirp to the
  guest's echo service answered in **1 ms to connect and 1 ms to echo**.
  So QEMU's main loop was being serviced and the guest was responsive.
  **The starvation hypothesis is dead** -- the only mechanism still
  standing after the foreign-connection and retransmission theories.
- **slirp's host-side socket was `ESTABLISHED` and still open**, and the
  connection ended at *our* deadline rather than by FIN or reset. slirp
  did not close it, did not reset it, and never wrote a byte to it.
- **The guest's half was reset** (`sent -104`, `rsts_in +1`) before it
  could write.

So **slirp tore down the guest's half of this connection and left the
host's half open, established and silent**, while remaining perfectly
responsive to everything else. That is a per-connection failure inside
slirp, not a stall, not a foreign connection, and not this kernel: the
guest's side has been fully accounted for since the socket-verdict unit,
and the host's side now says the same.

**Sighting thirty-two reproduced it, and that matters more than the
first reading did.** 2026-09-19, aarch64 CI again, on a
**documentation-only commit** (`a0558b6`, PR #186 -- no code had
changed since a green run on both architectures), so nothing about it
can be attributed to a change under test:

```
guest: connect 0 in 791 ms, sent -104, segs_out +2 retransmits +0 rsts_in +1
host : 1 connection(s): 127.0.0.1:36662 accepted at 91.9s, 0 byte(s): b''
       [deadline, ESTABLISHED]; slirp probe: connect 1 ms, echo 1 ms
       gave up at 111.9s
```

Row one of the table again, in every particular: the guest's half
reset before it could write, the host's half **open, established and
silent for the full twenty seconds** to the deadline, and slirp
answering a *fresh* connection through itself in 1 ms to connect and
1 ms to echo while it did so. The only figure that moved is the
connect: 791 ms against sighting thirty's 894 ms, both inside the
band this file had recorded to that point (597--1510 ms; sighting
thirty-six took the floor to 529 and thirty-nine to 493).

**What the second reading buys.** One reading of a new instrument is a
reading; two independent ones are a finding. The conclusion above no
longer rests on a single outing of freshly written code -- which was
the honest reservation to have about it. Both sightings are aarch64,
so this is a reproduction and **not** a second architecture; the
reading had not yet been taken on x86-64 at that point -- sighting
thirty-five has since taken it, and it is row one as well.

**Sighting thirty-three, and the sharpest version of the reading.**
2026-09-19, aarch64 CI again, on another documentation-only commit
(`83b42cb`):

```
guest: connect 0 in 947 ms, sent 12, recv -104, outstanding 12 then 12,
       segs_out +3 retransmits +0 rsts_in +1
host : 127.0.0.1:52290 accepted at 91.8s, 0 byte(s): b''
       [deadline, ESTABLISHED]; slirp probe: connect 1 ms, echo 1 ms
       gave up at 111.8s
```

Row one a third time -- and this one rules out something the other two
could not. In thirty and thirty-two the guest was reset **before** it
could write (`sent -104`), so "the host read nothing" was trivially
consistent with nothing having been sent. Here the write **succeeded**:
the guest's stack accepted twelve bytes, emitted them (`segs_out +3`,
`retransmits +0`), and they were still unacknowledged at both sample
points (`outstanding 12 then 12`) when the reset arrived. The host's
half of that same connection read **zero bytes** while sitting
`ESTABLISHED` for the full twenty seconds.

**What that establishes, and what it does not.** It establishes that
the failure is not "the guest never sent": a write was accepted and
segments were emitted, and nothing arrived. It does **not** establish
where the bytes stopped. `sent 12` is the guest stack accepting them,
`outstanding 12` is them going unacknowledged, and neither can
distinguish a loss in the guest's own transmit path, in virtio-net, at
slirp's input, or inside slirp between its two halves. The 1 ms probe
says only that slirp was serving *other* connections at the time.

The first version of this paragraph said the bytes "entered slirp and
never left it", which is one of those four and was asserted from
counters that cannot pick between them -- the same over-reading this
file already records for `straggler_ipis`. Locating the loss still
needs the host-loopback capture, which is root-only and recorded above
as blocked.

**Sighting thirty-four, the next run, and the reading is stable.**
`9b5b5f9`, documentation-only again, aarch64 again -- but the **GICv3**
job, a third distinct aarch64 configuration after the default and the
protection-capable boots. `connect 0 in 755 ms`, `sent -104`,
`outstanding 0 then 0`, `segs_out +2 retransmits +0 rsts_in +1`; host
side `[deadline, ESTABLISHED]` with `slirp probe: connect 1 ms, echo
1 ms`. Row one a fourth time, in the shape of thirty and thirty-two
rather than thirty-three: reset before the write.

**2026-09-19/20, four sightings across two pull requests, and one of
them broke the standing advice.** PR #191 produced two (one of them a
**re-run of the same commit**, which is the first time re-running has
failed to clear this) and PR #192 two more, on **consecutive commits**
of a branch whose only content is one Markdown file. That is a
cluster, and it is recorded as one. It is **not** a rate: four
sightings in two days says nothing reliable about frequency, this
file has been wrong about that twice, and nothing here identifies a
change that would have altered it. What it does say is that
"re-run it first" can no longer be relied on for this family, which
is a practical fact for whoever hits it next.

**Three consecutive branches, and the aarch64 rate is worth a sentence
of caution rather than a claim.** Sightings thirty-six through
thirty-eight landed on three successive pull requests (#189 twice,
#190 once), two of them documentation-only and the third a branch
whose only change is a new Markdown file. That *looks* like the rate
has risen sharply on aarch64 CI, and this file has been wrong about a
rate before: the honest statement is that three consecutive sightings
is what a constant rate also produces sometimes, no before-and-after
measurement exists, and the only rate this file has ever measured is
the one-in-twenty-one local x86-64 figure. Recorded as an observation
to test, not as a trend.

**Sighting thirty-five took the reading on x86-64 at last**, and it is
row one too. `f446890`, documentation-only again, PR #188's own CI:
`connect 0 in 890 ms`, `sent -104`, `outstanding 0 then 0`,
`segs_out +2 retransmits +0 rsts_in +1`; host side
`[deadline, ESTABLISHED]` with `slirp probe: connect 1 ms, echo 1 ms`.
The shape of thirty, thirty-two and thirty-four: reset before the
write.

**Ten readings, ten times row one, on both architectures** — two of
them x86-64 (sightings thirty-five and forty) and the rest aarch64
across three job configurations (default, protection-capable, GICv3).

**Sighting fifty-five, 2026-09-21** -- this paragraph first said
forty-one, a number counted from the prose while the table stood at
forty-nine, the mistake *The count* exists to prevent; the table now
has the row and the paragraph the table's number -- aarch64, the GICv3
boot of PR #203's CI (run 35585299449, `b7dcf9b`: the shared-futex
build, whose diff touches no network code): `connect 0 in 1395 ms, sent -104 in
0 ms, recv -1 in 0 ms, pending error -104, sndbuf free 65536 before,
65536 after send, 65536 after read (outstanding 0 then 0), state 0,
segs_out +3 retransmits +1 refused +0 rsts_in +1`. Row one again --
reset before the write, one SYN retransmission. Read as this file says
to read it, recorded, and the job re-run.

**Sightings fifty-six and fifty-seven, the same day and the same
branch**, both the plain aarch64 debug boot: `a0a0c38` at `connect 0
in 286 ms` -- the fastest connect recorded, two hundred milliseconds
under the previous floor -- and `5122041`, a commit that changes one
paragraph of one report, at `connect 0 in 1353 ms` with `retransmits
+1`; both `sent 12`, `recv -104`, `rsts_in +1`, the host having read
nothing. Three of this branch's five aarch64 runs failed this way, and
reading `main`'s own runs for 2026-09-20 and 21 found five more
occurrences nobody had recorded (the `main` rows above), on trees that
differ from each other only in what this file already discharges.
Three in five is what the rate looks like on the CI runner today,
written down as *Three consecutive branches* was: an observation, not
a measured rate.

This paragraph was written at five readings and its conclusion has not
needed changing since, which is worth as much as the readings
themselves: **slirp holds a host-side connection open, established and
silent, while remaining responsive to other connections through
itself.** The x86-64 sightings were the ones this file had been
careful to say were missing; having them changes the claim's scope and
not its content. Where the guest's bytes are lost, when there are any,
remains unlocated.

**What this does not name is the line of code.** It names the component
and the shape, which is what the unit promised and more than thirty
sightings had produced. The next measurement is a capture of the host's
loopback -- root-only, so a human with `sudo` -- or slirp's own source,
and either is outside this tree.

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


## An aarch64 boot that reached the self-tests and no further

**2026-09-20, `0814e79`, the same one-document branch** (`make
ARCH=aarch64 BUILD=debug test`). The harness reported **339**
self-tests in 76.3 s — the usual count on this architecture is 360 —
and then failed at 85.8 s with only userland markers missing:

```text
  - missing marker /^init: CosmoOS userland, pid \d+/
  - missing marker /^CosmoOS userland ready/
  - missing marker /^init: rc exited with status 0/
```

**No test failed.** There is no `SELFTEST: FAIL`, no forbidden
marker, and no panic in the dump; the serial log simply ends inside
`cosmofs-replay`, which is this architecture's slowest test and was
mid-way through its mount/unmount loop over `/mnt/crash`. The guest
stopped producing output and userland never started.

This is **not** the `net-harness` family above — different symptom,
different place, and the previous commit on this same branch failed
the other way. It is recorded as its own shape with one sighting and
no explanation, because the alternative is to file it under a family
it does not belong to, which is the mistake this file exists to stop.

The branch it appeared on contains three Markdown files and no code
(`git diff main..HEAD --stat`), so whatever it is, it is not the
change under review.

## An x86-64 boot that printed nothing at all

`main` @ `c6aaef0`, 2026-09-20, the x86-64 plain debug boot (run
35509878126): `boot-test: FAIL after 0.5s`, the serial log empty,
every marker missing from `cosmoboot-uefi` onward. Half a second is
before the firmware's first line, so nothing the kernel does is inside
the window; the next `main` run (`d04a8b1`) passed. One sighting and no
mechanism, recorded so that the next one has a first to compare
against. What to read on that one is the runner's QEMU invocation and
its stderr, which this log did not keep.

## An aarch64 release boot whose console stopped mid-line after an interrupt

**2026-09-21, `43f6790`, PR #203's aarch64 job, the release boot** (run
35590771083): the three debug boots of the same job had passed, and the
release boot's interactive harness got through its first eleven
commands. `sleep 5` was interrupted -- the terminal echoed `^C`, `sleep`
exited with status 130, the prompt came back -- and the next line the
harness typed, `echo after-interrupt-ok`, echoed as far as
`cosmo$ echo after-interrupt-o` and stopped. Nothing followed for the
rest of the 180 s: no prompt, no output, no kernel line. The harness
reported `no prompt before command 12` and every later command as never
sent. The same image built locally from the same commit passed the same
harness in 13.9 s.

**What the log does and does not say.** The prompt came back after the
interrupt, so the shell reaped the job and was reading again; the
keystrokes up to `o` were echoed, so the console's receive path was
alive after `^C`; then one keystroke was not echoed, and nothing after
it. Echo is the terminal's work, not the shell's, so the reader is not
what stopped: either the serial receive path stopped delivering or the
guest stopped altogether, and a release build carries no lockup
detector to say which. One sighting, on a branch that changes the
futex, the file fault's interrupt mask and one reference count -- none
of them on the path from a receive interrupt to an echo -- recorded so
that the next one is read for the two things this log cannot answer:
whether the guest still ticks (the harness could send a second Enter
and a `^C` before giving up and report whether either echoed), and
which CPU the console's receive interrupt was on (`serial: console
input on IRQ` is in the `dmesg` output above the stall). Not a bound
and not a list entry: re-run, and if it recurs, instrument before
theorising.

**It recurred, 2026-09-21, `4be3b72`, PR #208's aarch64 job, the
release boot** (run 35620152470; the branch changes one Markdown file):
the three debug boots passed, the interactive harness got through its
first twenty-six commands -- the interrupt, the two `fg`s, the line
editing, `after-pipeline-ok` -- and then typed `sleep 1 &`, `jobs`, and
`pkg update && pkg install hello && hello && pkg list`. The echo of
that last line stopped at `pkg update && pkg install hel`, and the very
next thing in the log is `process: pid 30 'sleep' exited with status
0` -- the background job from two commands earlier finishing. Nothing
followed for the rest of the 180 s: `no prompt before command 28`, no
prompt, no kernel line. Second sighting, same shape as the first --
keystrokes echoed up to a point and then not, right after a job event
the shell must handle (there `^C` and a reaped foreground job, here a
reaped background job) -- and the same build kind, the aarch64
release. Two is not a mechanism, but two is a place to look: what the
shell does when a child exits while the terminal is in the middle of a
line, and whether the console's receive path is the thing that stops
or merely the thing that goes quiet. The sequence is deterministic
enough to try on purpose: a background `sleep` timed to exit
mid-keystroke. Still not a list entry; still re-run first.

**A third, on `main` itself, 2026-09-24** (run 35942625646, the merge of
#231, a report): the echo of `echo after-interrupt-ok` stopped at
`cosmo$ echo after-interru`, right after the `^C`'d `sleep`. **Taken up
by `docs/audit/next-subsystem-console-rx.md`**, which provoked it on
purpose (`tools/console-stall-probe.py`: 13 stalls in 20 aarch64 release
boots) and looked from outside: the guest idle and ticking, the
PL011's receive FIFO full with its interrupt enabled and none pending.
`rx_irq` drains the FIFO and then clears the receive interrupt, so a
character arriving between the two has its interrupt cleared and is
never read, and QEMU's PL011 raises nothing for the characters after it.
Clearing first: no stall in six boots. Until the build lands, re-run.

## `net-harness` printed `ready` and nothing else, once, on this machine

**2026-09-22, x86-64, the plain debug boot on this developer's machine**,
a mutation run of the device-readiness unit (the mutation touches
`do_select` alone): the guest printed `NETTEST: ready tcp=7 udp=7` after
`net-nicbench` and then nothing for the remaining 125 s -- no
`client failed` line, no `client ok`, no kernel line -- and the host
harness printed no accept line either; the boot timed out at 201 s
with the key harness reporting the guest never asked for keys. That is
not the reset the tally above counts (the guest's client never spoke),
and it is not the aarch64 shape that stops before the self-tests. The
same mutated build's next run reached `net-harness` and passed it. One
sighting, under a host also building for the other architecture;
recorded rather than filed, and the thing to read on a second one is
whether the host-side harness ever accepted the back-connection.

## `quiesce-kick-spinner` is upset by any test that creates threads

**2026-09-20, found while building the `MAP_FIXED` replacement unit,
and it is not a flake at all — which is the point of writing it
here.** `quiesce-kick-spinner` failed on
`mid.straggler_ipis > before.straggler_ipis` twice in a row at the
same line, and I twice put it down to the loaded-host family this
file describes. The control settled it in one run: **`main` passed
360/360 on the same machine while the branch failed reliably.** A
failure that reproduces is not a flake, and the cheapest way to tell
is to run the parent commit.

Bisected from there:

| step | result |
| --- | --- |
| disable the new `vm-replace-race` only | 361/361 pass |
| pin its racer threads to one CPU | still fails |
| cut its rounds from 200 to 20 | still fails |
| **a stub that creates and joins six no-op threads, no VM work** | **still fails** |

So nothing in the new code is involved. The sensitivity is
`quiesce-kick-spinner`'s: it pins a spinner to `other_cpu()` and
requires a straggler IPI to be sent to it, and **any** test that
churns kernel threads beforehand — fifty tests beforehand, in this
case — is enough to stop that happening.

Not repaired here, because it belongs to the quiescence unit and not
to a VM one. `vm-replace-race` is registered after the quiesce block
instead, with the reason in a comment beside it. **The next unit that
adds a thread-creating self-test before those tests will hit this**,
and the useful part of this entry is that the bisect above takes
twenty minutes and the control takes four.

## `net-nat`'s expiry step found an entry after aging the table

**2026-09-20, x86-64 CI, the debug boot, on the `mprotect` unit's
first CI run (`60ccfd7`)** — a change to the memory syscalls that
touches nothing in the network stack, and a test that had passed four
times on the same tree locally (`test`, `test-gic`, `test-guard`, and
the release boot) in the hour before:

```text
SELFTEST: net-nat          ... FAIL: check failed: ns1.entries == 0 && ns1.expired > ns0.expired at line 4186 (1300 ms)
```

Step (6) of `net-nat` (`kernel-services/network/nettest.c`) calls
`nat_age` with a timestamp two UDP timeouts in the future and then
reads the statistics, expecting an empty table. That is deterministic
on its face — nothing about it waits — so the only way `entries` is
non-zero afterwards is that **an entry was created between the aging
and the read**. Two candidates, neither established: a frame from step
(5)'s flood still arriving through the tap after the drain loop
returned, or the periodic age work (`nat_age` is called from the ARP
ageing thread) interleaving with the test's own call in a way that
leaves one entry re-created. Both are the "N things after an action"
family this file's list describes: the check assumes a quiet interval
it never arranged.

One sighting, so no rate and no mechanism claimed — and the very next
CI run of the same branch (`ea811a0`, a documentation commit on top
of the same code) passed the x86-64 boot, so it did not reproduce on
the next try. What it needs if it recurs is the counters at the
moment of failure — `entries`, `expired`,
`out_new` before and after — printed by the check rather than
recovered from a log, which is the instrument-before-theory lesson
this file keeps re-learning. Not repaired here; it belongs to the
network tests.

## `virtio-remove-inflight`'s held pass found nothing

Not a flake to list: a defect in the test's own count, found by its
second sighting and closed by construction -- and a hole in a test seam,
found by its first sighting and closed on the way, which was not the
cause.

**The sightings.** 2026-09-21, twice. First the aarch64 GICv3 boot of
PR #203's CI (run 35586893021, `51c5b23`, a branch that touches
nothing under `drivers/`): `SELFTEST: virtio-remove-inflight ... FAIL:
check failed: found >= 1 at line 1219 (30 ms)`. Then, the same day and
with PR #204 merged, the x86-64 protection-capable boot of PR #205's
CI (run 35596999894, a documentation-only branch): the same check, 35
ms. Both the held pass -- the driver's slot table filled by
construction, the completions held -- and the remove found **0** in
flight, with every other assertion in the pass holding: every accepted
bio completed exactly once, with `0` or `-ENODEV` and no `-EIO`, and
`c_eio == found == 0`.

**The cause: the test's own count.** The submitter counted an accept
*after* `blk_submit` returned, while the completion callback on the
other CPU had already counted the completion -- a QEMU device answers
in microseconds -- so for that instant `completed` exceeded
`accepted`, and `accepted - completed`, two unsigned words, wrapped to
a huge number. The held pass's wait for "more outstanding than the
table holds" exited on it at once, the remove ran against a table with
nothing in it, and every assertion but the count held: exactly the
shape both sightings had. The accept is now counted before the submit
and uncounted on refusal, and the pass asserts `completed <= accepted`
on every turn of both its loops, completed read first and both
atomically (read the other way round, an accept and its completion
landing between the reads would wrap the difference again), from the
test thread while the submitter runs on the other CPU -- the only
observer an ordering between two counters can have. The worst case of
the old order (the accept counted after its own completion, held open
for a millisecond) fails that assertion within a millisecond of the
pass starting; a mere `sched_yield` in the window did not, because a
yield with nothing else runnable is a no-op, and that is worth writing
down too (PR #206).

**What the first sighting found instead, and why it stays.** Reading
the first failure found that the hold -- the debug seam that parks a
device's finished requests so the remove has a full table to walk --
was checked once, at `vblk_done`'s entry, so a handler already inside
its pop loop when the hold landed kept popping. That is a hole in the
seam's contract ("from this moment, finished requests stay in
flight"), and it was closed (PR #204): the check runs before every pop,
and a fourth pass, `held-inside`, builds the moment rather than racing
for it -- every hold in it stored from a completion callback, from
inside the handler, the two requests it parks known to be finished at
the device by a new exact seam (`unconsumed`: used-ring entries not
yet popped) rather than by waiting. It fails deterministically with
the check back at the door (`docs/kernel/device/testing.md`). It was
taken for the cause, and the second sighting on the driver with the
hole closed is what showed it was not: a mechanism that explains every
number is sufficient, not identified, until the failure is shown to
stop -- the reading this file's own rule (*instrument before
theorising*) exists to prevent, and the one it caught this time by
the cheapest instrument there is, a second look at the failing code
after the first fix.

## `quiesce-kick-spinner` is upset by any test that creates threads

**2026-09-20, found while building the `MAP_FIXED` replacement unit,
and it is not a flake at all — which is the point of writing it
here.** `quiesce-kick-spinner` failed on
`mid.straggler_ipis > before.straggler_ipis` twice in a row at the
same line, and I twice put it down to the loaded-host family this
file describes. The control settled it in one run: **`main` passed
360/360 on the same machine while the branch failed reliably.** A
failure that reproduces is not a flake, and the cheapest way to tell
is to run the parent commit.

Bisected from there:

| step | result |
| --- | --- |
| disable the new `vm-replace-race` only | 361/361 pass |
| pin its racer threads to one CPU | still fails |
| cut its rounds from 200 to 20 | still fails |
| **a stub that creates and joins six no-op threads, no VM work** | **still fails** |

So nothing in the new code is involved. The sensitivity is
`quiesce-kick-spinner`'s: it pins a spinner to `other_cpu()` and
requires a straggler IPI to be sent to it, and **any** test that
churns kernel threads beforehand — fifty tests beforehand, in this
case — is enough to stop that happening.

Not repaired here, because it belongs to the quiescence unit and not
to a VM one. `vm-replace-race` is registered after the quiesce block
instead, with the reason in a comment beside it. **The next unit that
adds a thread-creating self-test before those tests will hit this**,
and the useful part of this entry is that the bisect above takes
twenty minutes and the control takes four.

## `net-nat`'s expiry step found an entry after aging the table

**2026-09-20, x86-64 CI, the debug boot, on the `mprotect` unit's
first CI run (`60ccfd7`)** — a change to the memory syscalls that
touches nothing in the network stack, and a test that had passed four
times on the same tree locally (`test`, `test-gic`, `test-guard`, and
the release boot) in the hour before:

```text
SELFTEST: net-nat          ... FAIL: check failed: ns1.entries == 0 && ns1.expired > ns0.expired at line 4186 (1300 ms)
```

Step (6) of `net-nat` (`kernel-services/network/nettest.c`) calls
`nat_age` with a timestamp two UDP timeouts in the future and then
reads the statistics, expecting an empty table. That is deterministic
on its face — nothing about it waits — so the only way `entries` is
non-zero afterwards is that **an entry was created between the aging
and the read**. Two candidates, neither established: a frame from step
(5)'s flood still arriving through the tap after the drain loop
returned, or the periodic age work (`nat_age` is called from the ARP
ageing thread) interleaving with the test's own call in a way that
leaves one entry re-created. Both are the "N things after an action"
family this file's list describes: the check assumes a quiet interval
it never arranged.

One sighting, so no rate and no mechanism claimed — and the very next
CI run of the same branch (`ea811a0`, a documentation commit on top
of the same code) passed the x86-64 boot, so it did not reproduce on
the next try. What it needs if it recurs is the counters at the
moment of failure — `entries`, `expired`,
`out_new` before and after — printed by the check rather than
recovered from a log, which is the instrument-before-theory lesson
this file keeps re-learning. Not repaired here; it belongs to the
network tests.

## `virtio-remove-inflight`'s held pass found nothing

Not a flake to list: a defect in a test seam, found by the one failure
it produced and closed by construction.

**The sighting.** 2026-09-21, aarch64, the GICv3 boot of PR #203's CI
(run 35586893021, `51c5b23`, a branch that touches nothing under
`drivers/`): `SELFTEST: virtio-remove-inflight ... FAIL: check failed:
found >= 1 at line 1219 (30 ms)`. The held pass -- the driver's slot
table filled by construction, the completions held -- and the remove
found **0** in flight. Every other assertion in the pass held: every
accepted bio completed exactly once, with `0` or `-ENODEV` and no
`-EIO`, and `c_eio == found == 0`. So the requests were not lost; they
were *consumed*, by the driver, after the hold was stored.

**The reading.** The hold was checked once, at `vblk_done`'s entry. A
handler already inside its pop loop when the hold landed kept popping
-- the check was behind it -- and a QEMU device finishes a table's
worth in one burst (its AIO completions land a batch per main-loop
turn), so a handler entered on such a burst pops the whole table after
the hold is stored. The test then saw its condition (accepted minus
completed above the table's size: the pending list had formed behind a
full table, which is true whether or not the handler is draining it),
removed the device, and `vblk_remove`'s `virtq_free` waited, as it
must, for that handler to finish -- which is exactly why the walk found
nothing. The seam's contract was "from this moment, finished requests
stay in flight"; the check at the door kept it only for handlers not
yet running.

**What changed.** The check runs before every pop, so a handler inside
its loop stops at its next one; and a fourth pass, `held-inside`,
builds the moment rather than racing for it -- every hold in it is
stored from a completion callback, from inside the handler, and the
two requests it parks behind the hold are known to be finished at the
device by a new exact seam (`unconsumed`: used-ring entries not yet
popped) rather than by waiting. It fails deterministically with the
check back at the door (`docs/kernel/device/testing.md`).

**The reading above was wrong, and the second sighting said so.** With
the per-pop check merged (`cc645a0`), PR #205's x86-64
protection-capable boot failed the same way the same day (run
35594…, `found >= 1`, 35 ms, every bio completed `0`). The contract
hole the per-pop check closed is real -- the `held-inside` pass proves
it -- but it was not this failure's mechanism. The mechanism is the
**test's own count**: the submitter counted an accept *after*
`blk_submit` returned, while the completion callback on the other CPU
had already counted the completion (a QEMU device answers in
microseconds), so for that instant `completed` exceeded `accepted`,
and `accepted - completed` -- two unsigned words -- wrapped to a huge
number. The held pass's wait for "more outstanding than the table
holds" exited on it at once, the remove ran against a table with
nothing in it, and every assertion but the count held, which is
exactly the shape both sightings had. The accept is now counted before
the submit and uncounted on refusal, and the pass asserts `completed
<= accepted` on every turn of both its loops, from the test thread
while the submitter runs on the other CPU -- the only observer an
ordering between two counters can have. The worst case of the old
order (the accept counted after its own completion, held open for a
millisecond) fails that assertion within a millisecond of the pass
starting; a mere `sched_yield` in the window did not, because a yield
with nothing else runnable is a no-op, and that is worth writing down
too. What this cost: a plausible mechanism that explained every number
was taken for the mechanism, and the second sighting on the fixed code
is what named the real one -- the reading this file's own rule
(*instrument before theorising*) exists to prevent.

## `el2-guest-irq-queue`: the second injection was not still pending

**2026-09-20, aarch64 CI, the GIC boot, on the `mprotect` unit's first
CI run (`60ccfd7`)** — a memory-syscall change touching nothing in the
hypervisor — and the same tree passed `aarch64 BUILD=debug test-gic`
in the local 22-step list an hour later, plus the plain and guard
boots:

```text
SELFTEST: el2-guest-irq-queue ... FAIL: check failed: x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 2 at line 1157 (3 ms)
```

Step: inject INTID 42, run until the guest acknowledges it (hypercall
42), inject 42 again **while the first is Active**, run again, and
expect the guest back at its heartbeat (hypercall 2) with the second
still pending — "the completion of one instance is not the delivery of
the next" (`kernel-services/virtualization/hvtest.c`). What came back
instead is not in the log, and the check prints only that it was not
hypercall 2. So the candidates are the obvious two and neither is
established: the guest's EOI and the second injection interleaved the
other way round on a loaded runner under TCG, delivering the second
instance early; or the run returned a different exit kind altogether.

One sighting, no rate. If it recurs, the check should print `x.kind`
and `x.hypercall.nr`, which would settle which of the two it is at no
cost — the instrument-before-theory point again. Not repaired here; it
belongs to the vGIC tests.

**It recurred on 2026-09-22**, the same check and the same step, on
PR #215's aarch64 GICv3 boot (run 35726616478). That revision
(`34f46c96`) changed **documentation only**, which rules out the change
under test as it did the first time — the instrument described below
was added to the same branch afterwards, in response to this sighting,
so the branch as merged is not documentation-only and the revision that
failed was:

```text
SELFTEST: el2-guest-irq-queue ... FAIL: check failed: x.kind == COSMO_VM_EXIT_HYPERCALL && x.hypercall.nr == 2 at line 1159 (8 ms)
```

Two sightings, both aarch64 CI, both the GIC boot, both on revisions
that touch nothing in the hypervisor. So the instrument the paragraph
above asked for is built: `CHECK_HC(x, n)` in
`kernel-services/virtualization/hvtest.c` prints the exit kind, the
hypercall number and its first argument before it fails, and the
fifty-one plain hypercall expectations in that file go through it. The
next sighting will say which of the two candidates it is instead of
only that it was not hypercall 2.

**And the next sighting, hours later, was diagnosed by it.** PR #215's
aarch64 GICv3 boot again (run 35730173247):

```text
[ERROR] selftest: hv: line 1185: expected hypercall 2, got exit kind 4 hypercall nr 42 a0 0
SELFTEST: el2-guest-irq-queue ... FAIL: unexpected vm exit at line 1185
```

Kind 4 **is** `COSMO_VM_EXIT_HYPERCALL`, so it was not a different kind
of exit. It was hypercall **42**: the guest took the second instance of
INTID 42 instead of reaching the heartbeat at the top of its loop. That
is the first of the two candidates named above, and it is not a defect
in the vGIC -- once the guest deactivates the first instance the second
is pending and unmasked, so taking it immediately is correct. Whether
the heartbeat happens first is a matter of how the run is scheduled.

So the test was asserting a timing accident. It now runs until the
second instance arrives, allowing heartbeats on the way and requiring
42 to read as pending at each of them, then requires the guest back at
its heartbeat with nothing pending -- the substance, which is that the
second instance is kept and delivered exactly once, without the
ordering that was never guaranteed. It reports how many heartbeats
intervened, so the two orderings stay visible.

Three sightings, one instrument, one diagnosis, one fix. The
instrument stays: the fifty-one hypercall expectations in that file
now name what they got.


## `mmu: TLB shootdown acknowledged by 2 of 3 CPUs`

**2026-09-22, x86-64, this developer's machine**, one plain debug boot
of the balancer tree, at 73 s:

```text
SELFTEST: net-hostinput    ... FAIL: ... at line 6189 (3030 ms)
SELFTEST: net-hoststate    ... FAIL: ... at line 6786 (3664 ms)
SELFTEST: net-output       ... FAIL: check failed: n >= ETH_HLEN + 20 + 8 at line 7042 (2991 ms)
KERNEL PANIC: mmu: TLB shootdown of 0xffffc000104df000+0x4000 acknowledged by 2 of 3 CPUs
```

The next boot of the same tree passed, and so did every other boot of
it. **Not attributed to the balancer**, and the reason is the bound:
the shootdown waits a full second
(`kernel/arch/x86_64/mmu.c`), and no run-queue lock this tree takes is
held for anything approaching that — so "a CPU was spinning on a lock
the balancer now takes" does not explain it. What does fit is the three
host-networking tests failing immediately before, each after three
seconds of not receiving a frame: the host was not scheduling this
machine's vCPU threads, and one of them missed the second.

Worth keeping anyway, because the balancer did change something real
here: **the tick now spins on other CPUs' run-queue locks**, which it
never did before this unit — `balance_tick` calls `sched_migrate_from`,
which takes a pair with interrupts off. The hold is bounded and short,
but if this panic is ever seen again on a quiet host, the first thing
to try is a `spin_trylock` pair in the balancer, so a contended queue
is skipped until the next tick rather than waited for. A policy has no
business waiting for a lock.

## `tcp-pcb-timer-free` held a stranger's callback

2026-09-23, x86-64, one debug boot of the dirfd unit's branch (which
touches no network code). The log kept beside that unit's session notes
is `tcp-timer-free-nospin.serial`:

```
[ WARN] timer: cpu 0: no tick for 4995 ms; interrupts came back at pc ... (last tick interrupted pc ..., thread 'netrx/0')
SELFTEST: tcp-pcb-timer-free ... FAIL: check failed: spins > 0 at line 723 (5005 ms)
```

**This is not PR #225's deadlock**: the releaser existed, released at
its five-second deadline, and the boot went on. What failed is the
claim that a cancel waited: the test's `tcp_close` found no callback
inside its pcb to wait for. The reason is in how the hold is armed:
`tcp_test_hold_callback(true)` holds **the next TCP timer callback of
any pcb** (`timer_kick`'s check has no pcb in it), and the tests before
this one -- `net-tcpverdict` and its neighbours -- leave connections
whose timers are still live. One of those fired first, on CPU 0, above
`netrx/0`, and spun there for five seconds (the tick warning is that
spin); the test's own rexmit timer was never held, so its close had
nothing to cancel against. The same shape the anonymous-fault and
held-walk seams were built to avoid: **arm by identity, not "the next
one anywhere"**. The repair, for the unit that owns the test: arm the
hold for the test's own pcb, and have `timer_kick` take it only for that
pcb. Not attributable to the dirfd branch; the rerun of the same tree
passed.

**Repaired (PR #230).** `tcp_test_hold_callback(pcb)` arms the hold for
one pcb, and `timer_kick` takes it only for that pcb, counting every
other callback it lets through while armed. The test now makes the
sighting's shape certain: its armer starts a **decoy** pcb's rexmit
timer a tick before the test pcb's, on the same CPU, and the test
asserts the decoy was let through (`passed >= 1`) before any claim about
the cancel. Mutation, alone, booted: restore "any pcb" and the decoy is
the one held -- `passed >= 1` fails on x86-64 in 18 ms.

## `smp-wake`: no reschedule IPI counted on the target

2026-09-23, CI run 35901262531 (PR #230, which changes only the TCP
timer test's hook), x86-64, the second debug boot (protection-capable
CPU); the first boot of the same image passed:

```
SELFTEST: smp-wake         ... FAIL: check failed: cw.ipis_after > cw.ipis_before at line 454 (2 ms)
```

The first sighting since the test was restated to count the IPI instead
of timing the wake. Not attributable to #230: `smp-wake` runs before any
network test and before any TCP pcb exists.

**A candidate mechanism, not proven for this sighting.** `sched_wake`
calls `request_resched` -- and so sends `IPI_RESCHEDULE` -- only when the
target CPU is idle or running something of lower priority. If a thread
of equal priority was on the target at the post, the waiter was enqueued
with no IPI and ran at that thread's next tick or block; the whole test
took 2 ms, which a tick fits inside. The check then fails on a correct
kernel, because it assumes the target is idle, and nothing in the test
makes it so. The log has no dump of what the target was running, so
this is the story that fits, not a finding: the next step is to record
`rq->current` on the target at the post (or assert the target idle
before posting) before changing the claim.

## `sched-migrate-stress`: a worker made no progress, 2026-09-24

`SELFTEST: sched-migrate-stress ... FAIL: a worker made no progress
under migration (260 ms)`, on CI: PR #238's run 36002630250, aarch64,
the harness-retry boot -- a branch of one report, one probe script and
this file. The test sleeps a fixed 200 ms and then requires every one
of its workers to have made a round: the "N things after a fixed
settle" shape, which a host stall of 200 ms defeats. The re-run passed.
First sighting.

## `quiesce-straggler` and `signal-group`: one sighting each, 2026-09-24

Local, aarch64 debug, both on the balance-pair fix's branch, which changes
only `smptest.c`'s two balance tests:

- `quiesce-straggler ... FAIL: check failed: kicks >= 1` (line 173), in
  the chaos boot. The test holds a reader for 30 ms and requires the
  waiter to have kicked its CPU at least once; no kick means the reader
  was already done when the wait began -- the waiting thread delayed
  past 30 ms after the reader entered, the host-time family
  `lockup-sample-busy` belongs to. First sighting.
- `signal-group ... FAIL: check failed: process_count() == before`
  (line 254, 2477 ms), in a plain boot that also ran a scratch loop of
  thirty extra balance rounds. A settle on an exact count; first
  sighting.

Recorded, not attributed: neither test touches the scheduler's balance
tests, and the next boots of the same tree passed both.

## Under the chaos migrator: `sched-balance-pull` on CI, three times on 2026-09-23, and twice on the 24th

`SELFTEST: sched-balance-pull ... FAIL: runnable threads stayed on the
CPUs creation order gave them (3005 ms)`, in the x86-64 job's chaos boot
only: once on `main` itself (run 35827861816, the ELF shared-text merge),
once on PR #225's first run (35842572114), whose change is in a TCP
test that runs minutes later, and once on PR #229 (35882609779), whose
change is in the Linux door's path calls. **A fourth on 2026-09-24**, the
first on aarch64: PR #232's run 35948747763 (3009 ms), on a
documentation-only commit whose two predecessors, carrying the same code,
passed every boot including this one. **A fifth the same day**, aarch64
again: PR #233's run 35953787997 (3016 ms), a branch of one report, one
probe script and one inventory line -- no kernel code at all. Five in two
days, the last two in a row on aarch64: the rate is rising, and a test
that fails this often on trees that cannot cause it is costing every
unit a CI round. The decision the balancer unit owns -- whether this
test asserts under the chaos migrator at all -- is due. The scheduler testing doc already says this
is the one balancer test that still asserts, and that under a chaos
migrator its claim is about the machine rather than the balancer
(`docs/kernel/scheduler/testing.md`). Recorded as a sighting; the
balancer unit owns the decision whether it asserts under chaos at all.

**Resolved (PR #236, the balance-movable unit,
`docs/audit/next-subsystem-balance-movable.md`).** Not a flake of the
host but a race the test asserted and the rules let chaos lose: a
spinning worker is movable only until its first preemption, and one
chaos move of a released spinner onto a busy CPU makes a pair S26
forbids any migrator to separate -- measured deterministically, an idle
CPU refused some five hundred times a second. The test keeps asserting
under chaos: its released workers now yield, so it asserts the
balancer's contract (an idle CPU pulls a *movable* thread), and
`sched-balance-pair` asserts both halves of the mechanism. The balancer
compiled out still fails it, in the plain image.

## Under the chaos migrator: `thrtest`'s stack replacement and `tty-isatty`'s release

Two sightings from `make test-chaos` on the percpu-migration tree,
2026-09-22, each once in about fifty chaos boots and never in a plain
boot:

- `THREADTEST: FAIL 2` on AArch64: `thrtest: FAIL env_reader start at
  line 1169: rc -17`, the `EEXIST` that `cosmo_thread_start`'s
  `MAP_FIXED` replacement of its stack reservation used to lose before
  the map-fixed unit made replacement one operation. The reservation was
  taken and the replacement refused in the same call, with the process's
  churn thread growing the environment concurrently; the fixed path's
  own unmap should leave nothing to refuse. Not understood; recorded
  with its line, and a second sighting is the time to instrument
  `vm_user_map_anon_replace`.
- `tcp-pcb-timer-free`: `spins > 0` at line 674 — the cancel that was
  supposed to wait for a parked callback did not wait at all, and the
  test failed in 6 ms rather than after the releaser's five-second
  bound. Once, in the first `make test-chaos` boot of the balancer
  tree, and **not once in the eight chaos boots that followed** (three
  with the balancer, three without, and two on AArch64), so it is not
  the balancer: the run with it disabled is the control and the
  failure did not follow the variable. The mechanism is unexplained.
  The test arms the timer on another CPU so the callback runs there,
  waits for it to enter the hold, and only then closes; for the count
  to stay zero, no cancel can have found `q->running` set. Recorded
  with its line, and a second sighting is the time to print which of
  the four timers was cancelled first and what its queue's `running`
  was.
- `tty-isatty`: `process_count() == before` after a 500 ms wait for the
  child's release once its thread is reaped. The reaper's turn came
  later than that once under migration; the bound catches a leak, not
  slowness, and is two seconds now (`LOAD-SENSITIVE`).

## `tcp-pcb-timer-free`'s held callback parked on a CPU nobody could release it from

Seen once, 2026-09-23, on this machine, x86-64, during the cwd-hold
report's measurement boots (`tools/cwd-race-probe.py` applied: an unowned
cwd read in `sys_open` and a poison on freed vnodes -- neither on any path
below). The log is kept beside the report's session notes as
`lockup-timer-kick-boot6.log`.

**What it looked like.** `SELFTEST: net-lo-udp ... ok` and then, ten
seconds later with no further test line:

```
[ WARN] hard lockup: cpu 3 no tick for 10000 ms; last tick 10014 ms ago at pc 0xffffffff800da920 (seen from cpu 2)
cpu 3: pc arch_cpu_relax  #1 timer_kick (tcp.c)  #2 run_expired (timer.c:508)  #3 x86_trap_dispatch   -- 8 samples, 250 us apart, identical
cpu 0, 1, 2: idle_main -> arch_cpu_wait_for_interrupt
```

The next test in the table after `net-lo-udp` is `tcp-pcb-timer-free`,
whose first act is `tcp_test_hold_callback(true)`: the next TCP timer
callback parks inside `timer_kick` and spins on `g_test_cb_release`
(`tcp.c:397`). CPU 3 is that callback. It is **not** the host-starvation
false positive this file describes above: the samples show the CPU
executing, in the same two frames, eight times.

**What makes it a hang rather than a slow test.** The other three CPUs
are halted. The test's own thread, which after the callback enters
should be creating the releaser and calling `tcp_close`, is nowhere;
neither is a releaser (`tcp_releaser_main`, which would let the callback
go after five seconds even if no cancel ever spun). The test chooses
`cpu` (where the callback should land) and `rel_cpu` as the first two
online CPUs other than its own, and its comment says the hazard it knows:
"putting [the releaser] beside the callback is what hung the first
version". The picture here is the *other* placement: **the callback
landed on the test thread's own CPU**, in interrupt context above it, so
the thread that would create the releaser never ran again. Why the timer
fired there rather than on `cpu`, where the armer thread was pinned, is
not established -- a re-armed timer keeping its first queue, or the pcb's
timer having been started on this CPU before the test's armer touched it,
are guesses and are written here as guesses.

**What would settle it** costs one line: the hold seam records
`arch_cpu_id()` when the callback enters, and the test prints it beside
`cpu`, `rel_cpu` and its own. A second sighting then names the placement
instead of inferring it from three idle CPUs.

**A second sighting the same day**, on the cwd-hold unit's own branch,
x86-64, one debug boot of six, in the same slot -- `SELFTEST: net-lo-udp
... ok` and then:

```
KERNEL PANIC: mmu: TLB shootdown of 0xffffc000104af000+0x4000 acknowledged by 2 of 3 CPUs
```

with no failing test before it (which distinguishes it from the
2026-09-22 sighting of this panic recorded above, where three
host-networking tests had failed first). A CPU spinning in interrupt
context with interrupts masked cannot acknowledge a shootdown, and the
shootdown gives up after one second where the lockup detector gives up
after ten: two symptoms of one CPU in one state, and which one fires is
only which bound is reached first. Reading the test with both in hand
named the mechanism: the armer thread, pinned to `cpu`, arms a **1 ms**
timer and must then exit on that CPU before the next tick; when it does
not, the callback fires above the still-live armer and spins there, the
test thread's `thread_join(armer)` -- which came *before* the releaser
was created -- never returns, and nothing exists that can release the
callback. The idle CPUs are the test thread in its join and the
releaser that was never made.

**Reproduced deterministically, and fixed (PR #225).** The mechanism was
made the adversary: the armer lingers two ticks on its CPU after arming,
so the callback parks in interrupt context above it in every run. With
the test's original order that boot hung in this slot and the detector
named it exactly as sighting one did:

```
SELFTEST: net-lo-udp       ... ok (64 ms)
[ WARN] hard lockup: cpu 0 no tick for 10000 ms; last tick 10018 ms ago at pc 0xffffffff800da966 (seen from cpu 3)
cpu 0: arch_cpu_relax <- timer_kick <- run_expired <- trap <- ... <- thread_trampoline   (8 samples, identical)
cpu 1, 2, 3: idle_main
```

-- the callback above the armer's own thread (`thread_trampoline` at
the bottom of the stack), the test thread in `thread_join(armer)`, no
releaser. The fix creates the releaser before the timer is armed and
joins the armer only after the release; with the join alone moved back,
the test fails by name in five seconds (`spins > 0`, once the releaser's
deadline lets the callback go) instead of hanging. The callback records
its CPU and the test asserts it is the armer's and prints all three
placements. Green on both architectures since, with the hostile
placement every time.

Not on the list: not a bound, and not attributable to either unit's
mutation -- the spinning path holds no vnode and makes no system call.
