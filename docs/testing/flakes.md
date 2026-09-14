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

Earlier, the same family: `schedtest.c`'s tick-rate lag bound was widened
in pull request #63 after failing on a loaded host, and its comment
already says what this file says -- a tighter bound was only ever
measuring the host.
