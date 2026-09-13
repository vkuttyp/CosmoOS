# NEXT SUBSYSTEM — the suite waits for time instead of for the property

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This is that report, and
**nothing in it is implemented**.

**Subsystem: the boot suite's timing assumptions.** Two hundred and
forty-six self-tests are the evidence every unit in this repository cites,
and a measurable share of them assert on **how long something took** or
**how much happened in a fixed window** rather than on the property under
test. On a loaded host those assertions are about the host. **Four
distinct tests flaked in a single session on 2026-09-13**, one of them
four times, one of them blocking a merge that had to be diagnosed, argued
and re-run before it could land.

## Problem

A test that waits for a fixed interval and then counts is measuring the
machine it runs on. `kernel-services/network/nettest.c`:

```c
for (unsigned i = 0; i < 300; i++)
    inject_tcp(...);
settle(100);                                   /* sleep 100 ms */
...
CHECK(cookies > 0 && cached + cookies == 300); /* ...and hope all 300 arrived */
```

`settle` is exactly what it looks like:

```c
static void settle(unsigned ms)
{
    for (unsigned i = 0; i < ms; i += 10) {
        thread_sleep_ms(10);
        sched_watchdog_kick();
    }
}
```

There is no relationship between 100 ms and 300 packets. On an unloaded
host the work finishes in far less; on a host that has been building and
booting for hours it does not, and `net-tcp-syncache` fails at
`nettest.c:983` having proved nothing about SYN cookies.

### The right pattern is already in the tree

`kernel/scheduler/schedtest.c`, forty lines from a flake of its own:

```c
static bool threads_settle(unsigned expected)
{
    uint64_t deadline = clock_now_ns() + MS(200);
    while (thread_count() != expected) {
        if (clock_now_ns() > deadline)
            return false;
        sched_yield();
    }
    return true;
}
```

**That is the whole design of this unit.** Wait for the *condition*,
bounded by a generous deadline, and fail loudly if it never arrives. It is
used by `schedtest.c`, `smptest.c` and `quiescetest.c`; it does not exist
in `nettest.c`, which has twenty-two sites of the other kind. As with the
last two units, this one finishes a rule the repository already holds
rather than inventing one.

### Two shapes, and only one of them is a sleep

**Shape A — settle-then-assert. Twenty-two sites, all in `nettest.c`**, and
every one is followed by an assertion within five lines: there is no
`settle` in that file that is not immediately counted against.

**They are not all the same conversion, and the plan should not pretend
they are.** The observable differs, and so does what "done" means:

| kind | what is waited for | conversion |
| --- | --- | --- |
| a counter reaching a total | `tcp_get_stats`, the IPv4 counters | wait for the count; the budget is the only new number |
| a connection reaching a state | `tcp_state_of(c->tcp)` | wait for the state |
| readiness or a result | `ksock_ready`, `ksock_recvfrom`, `ksock_accept` | wait for the readiness bit, then keep the existing assertion on the *result* |
| a derived value | `tcp_path_mss`, `c->tcp->mss`, `snd_una` | a direct field read: waitable, but the predicate is reading TCP internals and should say so |
| a settle *inside* a loop | retry and poll loops | the loop's own termination has to be re-thought, not just the sleep — these are the ones to do by hand and last |

The first three are mechanical. The last two are not, and calling all
twenty-two "mechanical" is how a conversion quietly changes what a test
asserts.

**Shape B — bounds on elapsed time and on work done.** Twenty-six
assertions match a search for a duration; **one of them is not timing at
all** (`blktest.c:297` compares a configured constant), leaving
twenty-five. Of those, **the direction is what decides everything**:

| | count | what a loaded host does |
| --- | --- | --- |
| **lower bounds** — `elapsed >= MS(30)` | **12** | makes them *more* true; this unit leaves them alone |
| **upper bounds and work ratios** | **12** | breaks them |
| of which: generous guards — `irqtest.c:85`, `proctest.c:240` (15 s), `:927` (2 s) | 3 | a host that breaks these is genuinely broken; left alone |
| **of which: load-sensitive, and this unit's Shape B work** | **9** | the table below |

So the unit acts on **22 Shape A sites and 9 Shape B sites, 31 in all**,
and deliberately leaves fifteen duration assertions untouched. The
tightest of the nine are very tight:

| site | assertion | what a loaded host does to it |
| --- | --- | --- |
| `smptest.c:350` | `cw.woke_at - sent < MS(2)` | two milliseconds for an IPI to wake an idle CPU |
| `polltest.c:89` | two clocks agree within `1 ms` | a scheduling gap between the two reads breaks it |
| `schedtest.c:368` | `d < MS(20) + 3*TICK_NS + MS(10)` | **flaked today** (`SELFTEST: sleep`) |
| `smptest.c:239` | `work[1].iterations > work[0].iterations / 4` | **flaked today** (`smp-parallel`) |
| `hvtest.c:1279` | `late_ticks < asked_ticks` | **flaked today** (`el2-guest-timer-ontime`) |
| `schedtest.c:166`, `:354`, `:485`, `smptest.c:263` | `< MS(80)`, `< MS(200)`, `< MS(500)`, `< MS(100)` | progressively safer, none proof against a busy host |

### What it cost, on one day

| test | site | shape | times |
| --- | --- | --- | --- |
| `net-tcp-syncache` | `nettest.c:983` | A | **4**, one of them blocking a merge |
| `SELFTEST: sleep` | `schedtest.c:368` | B | 1 |
| `smp-parallel` | `smptest.c:239` | B | 1 |
| `el2-guest-timer-ontime` | `hvtest.c:1279` | B | 1 |

Every one passed on re-run. That is the definition of the problem rather
than a mitigation of it: **a suite that passes on the second attempt is a
suite whose verdict means less on the first.** Every unit in
`docs/audit/` ends with "246 self-tests PASS on both architectures", and
that sentence is worth what the suite's determinism is worth.

## Current implementation

- `settle(ms)` in `nettest.c`, twenty-two call sites, no relationship
  between the interval and the work.
- `threads_settle(expected)` in three scheduler and quiescence tests:
  correct, deadline-bounded, fails loudly.
- Twenty-five duration assertions (a twenty-sixth match is a constant
  comparison, not timing): twelve lower bounds that are sound, twelve
  upper bounds or ratios, of which three are guards so generous that a
  host breaking them is broken. **Nine to classify.**
- **The harness retries exactly one class of failure, and it is not this
  one.** `tests/boot/run_boot_test.py:416` re-launches the boot once when
  *the firmware never hands over* — an environmental failure before the
  kernel starts. So the precedent that some failures are the host's and
  not the code's is already in the tree and already acted on. For a
  `SELFTEST: FAIL` there is nothing: the boot fails, and a human decides
  whether to re-run. Nothing records that a test has flaked before, so the
  fifth flake of a day is diagnosed exactly like the first — which is what
  happened.

## Why it matters

1. **It costs the thing this repository runs on: evidence.** Every §68
   unit's claim is the suite. A test that fails for the host's reasons
   teaches everyone to re-run rather than to read, and the next real
   failure gets the same shrug. That is the actual risk, and it is already
   visible in how quickly "it flaked, re-run it" became the reflex today.

2. **It costs merges.** One of today's four blocked a merge the user had
   asked for. It took a local reproduction, a reading of the test, a
   comparison against `main`, and a CI re-run to establish that a
   markdown-only branch had not broken TCP.

3. **Twenty-two of the thirty-one sites are mechanical.** The observable
   already exists and only the wait is wrong -- though not all twenty-two
   are the *same* mechanical change; see the conversion shapes below.

4. **The remaining third is a design question worth asking once.** An
   assertion that an IPI wakes an idle CPU within 2 ms is trying to say
   "the IPI woke it, not the tick". That is a real property and the
   timing is a proxy for it — often a bad one, and sometimes the only one
   available. Each upper bound deserves the question *what is this really
   asserting, and can it be said without a clock?*

## Design

### Shape A: one helper, twenty-two conversions

```c
/* kernel-services/network/nettest.c (and wherever else it is wanted) */

/* Wait until `pred(arg)` holds, or `budget_ms` passes. Returns false on
 * expiry, which the caller must CHECK -- a wait that quietly gives up is
 * the bug this replaces, wearing a different hat. */
static bool wait_until(bool (*pred)(void *), void *arg, unsigned budget_ms);
```

Each site becomes:

```c
-   settle(100);
-   tcp_get_stats(&t1);
-   CHECK(cookies > 0 && cached + cookies == 300);
+   /* Wait on one counter -- the total of SYNs the stack has answered --
+    * because `tcp_get_stats` copies `g_stats` with no lock and a
+    * predicate over two of its fields can see them from either side of
+    * an update. The two-field assertion stays where it was, after the
+    * wait, as the test's claim rather than the wait's condition. */
+   CHECK(wait_until(syn_answered_reached, &(unsigned){300}, 2000));
+   tcp_get_stats(&t1);
+   CHECK(cookies > 0 && cached + cookies == 300);
```

**The budget goes up, not down** — from 100 ms to a couple of seconds —
and that is the point rather than a regression. A generous deadline that
is never reached on a healthy host costs nothing, and when it *is*
reached the test fails with "waited 2 s for 300 SYNs, saw 214" instead of
an arithmetic mismatch that looks like a protocol bug.

### Shape B: classify each bound, and say which kind it is

Three outcomes per site, decided one at a time:

- **Keep**, where the bound is the property (a lower bound on a sleep) or
  is so generous that a host breaking it is genuinely broken
  (`proctest.c`'s 15 seconds).
- **Restate**, where the timing is a proxy for something directly
  observable. `smptest.c:350`'s 2 ms is really "the IPI woke it, not the
  tick"; if the wake path can be counted, count it.
- **Widen and label**, where the property really is temporal and no
  observable substitutes. Then the bound should be *documented as a
  flake risk* at the site, so the next person to see it fail knows within
  one line whether to investigate.

The report deliberately does not pre-decide the nine. Doing so from a
reading is how the last three units' test designs went wrong.

### The harness remembers

`tests/boot/run_boot_test.py` gains one thing: when a self-test fails, it
names it against a short list of **known load-sensitive tests** carried in
the repository, and says so in the failure line:

```
boot-test: FAIL after 74.4s
  - SELFTEST: FAIL (1 of 246): net-tcp-syncache
  - note: net-tcp-syncache is on the load-sensitive list (docs/testing/flakes.md);
          a re-run distinguishes a flake from a regression
```

It does **not** re-run automatically and it does **not** pass. Hiding a
flake is worse than the flake. What it removes is the twenty minutes
between seeing a red CI and knowing which kind of red it is.

### The §70 gate

**Correctness.** A bounded wait on an observable predicate is strictly
more correct than a sleep of unrelated length: it passes exactly when the
property holds and fails when it does not, where the sleep passes when
the host is fast.

**Concurrency.** The waits run on the test thread and poll observables
that are **not** synchronised, which the first draft of this section got
wrong by asserting the opposite. `tcp_get_stats` is `*out = g_stats;` — a
plain structure copy with no lock — and `ksock_ready` reads `s->state`,
`s->error` and `s->shut` outside the socket mutex. Both are fine for what
they are: counters and readiness bits, read by a test.

What follows from it is a **constraint on the predicates**, and it is the
kind that is easy to get wrong later: *a predicate may not require a
coherent snapshot of more than one field.* Waiting for
`syn_cached + syn_cookies == 300` reads two words of an unsynchronised
structure and can see them from either side of an update. The safe shape
is to wait on a single monotonic counter and to leave the multi-field
assertion where it is — **after** the wait, where it is the test's claim
rather than the wait's termination condition. No new sharing is
introduced either way.

**Ownership / Lifetime.** None: the helper owns nothing and outlives
nothing.

**Failure.** The expiry path is the interesting one, and it must be
`CHECK`ed by every caller rather than ignored — a `wait_until` whose
result is dropped is a `settle` again. The migration should make that
impossible to get wrong where the language allows it (a
`__attribute__((warn_unused_result))`).

**Security.** None: test-only code.

**Performance.** The suite gets *faster*, not slower. Twenty-two fixed
sleeps totalling roughly 900 ms of unconditional waiting become waits that
return as soon as the condition holds — typically in single-digit
milliseconds. The generous deadlines are ceilings, not costs.

**Scalability.** The suite's runtime stops scaling with the number of
fixed sleeps and starts scaling with the work.

## Affected files

| file | change |
| --- | --- |
| `kernel-services/network/nettest.c` | `wait_until`, and 22 `settle` sites converted; `settle` deleted when the last one goes |
| `kernel/scheduler/smptest.c`, `-/schedtest.c`, `kernel/io/polltest.c`, `kernel-services/virtualization/hvtest.c` | the upper bounds classified: keep, restate or widen-and-label |
| `tests/boot/run_boot_test.py` | name a failing test against the load-sensitive list in the failure line |
| `docs/testing/flakes.md` | **new**: the list, what each bound is really asserting, and the rule |
| `docs/kernel/*/testing.md` | the rule where each suite documents itself |
| `README.md` | Status entry |

**No kernel change outside test files**, and no change any program can
observe. If the implementation finds itself editing a non-test kernel
source, the design was wrong.

## New APIs

One test-local helper. No syscall, no public header, no ABI.

## Migration plan

1. **`wait_until`, and the three worst Shape A sites** — the ones that
   have actually flaked. Small enough to review as a pattern before it is
   applied twenty-two times.
2. **The remaining nineteen Shape A sites.**
3. **`settle` deleted**, which is the check that step 2 was complete: a
   remaining caller means a missed site.
4. **Shape B, classified one at a time**, with the reasoning recorded per
   site. The three that flaked today first.
5. **The harness note and `docs/testing/flakes.md`.**
6. **The documents.**

## Tests

The awkward part, and worth stating plainly: **this unit's subject is
test reliability, and reliability is not something a single run can
demonstrate.** A converted test passing proves nothing that the old one
did not also prove on a good day.

So the proof is a **deliberately slowed host**:

1. **A load switch.** A boot parameter (or a `#ifdef` the bug-proof
   builds) that makes `thread_sleep_ms` and the scheduler tick behave as
   they do under load — the simplest version is a self-test-only thread
   that burns CPU on every core for the duration of the network tests.
2. **Under it, the *unconverted* tests must fail** — that is the
   demonstration that the flake is the fixed sleep and not bad luck. This
   is the one proof that matters, and it should be run *before* the
   conversion, against today's tree, so the unit begins by reproducing
   the problem on demand.
3. **Under it, the converted tests must pass**, which is the unit's claim.
4. **Without it, both pass**, which is what makes the switch the variable.

**Then a repetition count.** Twenty consecutive boots of each
architecture with no `SELFTEST: FAIL`. Twenty is not a proof of absence
and the report does not pretend otherwise; it is the number at which
today's rate — four failures across roughly forty boots — would be
expected to show at least once.

**Bug-proofs**, each failing for its own reason: a converted site whose
`wait_until` result is not `CHECK`ed (the expiry passes silently — this is
`settle` in disguise, and the `warn_unused_result` should make it a
compile error rather than a test); a predicate that is already true on
entry (the wait returns immediately and proves nothing, which is the
vacuity trap this repository has hit five times in three units); and a
budget set below the work's real duration (the test fails loudly with the
count it reached, which is the failure mode the design is for).

## Benchmarks

1. **Suite wall-clock, before and after.** Roughly 900 ms of
   unconditional sleeping in `nettest.c` alone should mostly disappear.
   The number matters because a faster suite is re-run more willingly.
2. **The flake rate itself**, over the twenty-boot runs: the metric the
   unit exists to move, and the only honest way to state the result.

## Risks

- **A widened deadline hides a real regression.** A test that used to
  fail in 100 ms now waits two seconds before failing, and a genuine
  slowdown looks like success until it crosses the new ceiling. The
  mitigation is that `wait_until` can report *how long it waited* when it
  is close to the budget — a test that habitually takes 1.9 s of a 2 s
  budget is a finding, not a pass.
- **Shape B may not be fixable in every case.** Some properties are
  genuinely temporal. The honest outcome for those is "widen and label",
  and the report says so in advance rather than discovering it and
  quietly leaving them.
- **The load switch is test scaffolding that ships.** The same argument
  the condition-variable unit had about its probe, and it should get the
  same treatment: compiled in, argued for, and priced — or built as a
  boot parameter that costs nothing when unset.
- **Twenty boots is a long verification.** Roughly forty minutes per
  architecture. That is the cost of making a claim about reliability at
  all, and the alternative is asserting it.

## Alternatives considered

- **Re-run failed tests automatically in the harness.** The cheapest fix
  and the worst: it converts a visible flake into an invisible one, and
  the next real intermittent bug — the sort the `guest_psci_race` and
  vCPU-threads units chased for days — becomes undetectable.
- **Delete the flaky tests.** They test real properties: SYN cookies
  under flood, IPI wake latency, scheduler fairness. The assertions are
  wrong, not the subjects.
- **Raise every fixed sleep.** Makes the suite slower and the flakes
  rarer without making any test correct; the first busy CI host brings
  them back.
- **A global "slow host" multiplier applied to every bound.** Tempting
  and wrong for Shape A: the problem is not that 100 ms is too short, it
  is that no number is right when the thing being waited for is
  observable.

---

Named and deferred by this unit: the userland test programs' own timing
assumptions (`thrtest`, `cwdtest`); a per-test time budget in the harness;
and the `net-icmp-limit` family's shared `settle` in the other direction
(a *maximum* count after a flood, which a slow host makes pass
spuriously).

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01FtzXcfogMnEqCnyAVzZYFj
