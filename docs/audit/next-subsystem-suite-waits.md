# NEXT SUBSYSTEM — the suite waits for time instead of for the property

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. **This report is as built**, and
its proof design was wrong in the one way a first step could find: it
proposed slowing the *host*, and slowing the host tips nothing.

**Subsystem: the boot suite's timing assumptions.** Two hundred and
forty-six self-tests are the evidence every unit in this repository cites,
and a measurable share of them assert on **how long something took** or
**how much happened in a fixed window** rather than on the property under
test. On a loaded host those assertions are about the host. **Four
distinct tests flaked in a single session on 2026-09-13**, one of them
four times, one of them blocking a merge that had to be diagnosed, argued
and re-run before it could land.

**What the build changed, each found by building rather than reading:**

1. **The proof lever.** The report proposed a slowed host (CPU spinners).
   `settle(100)` is ten `thread_sleep_ms(10)` calls, so load stretches
   the wait and the packet processing *together*: four spinners produced
   one unrelated failure, twelve produced none, and full-speed spinners
   never reached userland. **Slowing the work is the lever**: a 400,000-
   iteration delay per frame in `lo_transmit` fails the unconverted tests
   at exactly the lines that flaked (`983`, `1236`) and passes the
   converted ones. It is injected by the proof and restored
   byte-identical; nothing ships.
2. **Twelve bare sites, not eleven**, and the twelfth was found only
   after `settle` was deleted and the function it hid behind was gone.
   Twenty-four `wait_until` sites over twelve predicates replaced
   twenty-two `settle` calls.
3. **The predicate rule gained an exception.** The absolute form -- one
   field -- was written into the helper's contract and broken two
   functions later by a predicate that sums two monotonic counters. That
   is safe, and the contract now says why: with every field monotonic
   and the predicate monotone in them, a torn read can only under-count.
4. **Review found four waits that returned before the thing asserted**
   -- the same defect one level up: a retransmit counter that means
   "scheduled" where "delivered" was needed; a baseline taken too early;
   `-EAGAIN` back-offs whose expiry was dropped; a two-field predicate
   against the rule. All fixed; the fourth is why item 3 exists.
5. **Shape B, as classified**: four restated into an observable, four
   removed because the failure they named is a hang the watchdog reports,
   two widened and labelled -- and `hv-vcpu-stop:601`, a third shape the
   report's inventory missed ("another thread got there first"), restated.
6. **The list has two entries**, not the example's `net-tcp-syncache`,
   which is converted and cannot be load-sensitive any more. The harness
   reads the table under one heading of `docs/testing/flakes.md` and
   says so when the file is missing or parses to nothing.
7. **Two things the report asked for and the first cut forgot**, added in
   the last step: `warn_unused_result` on the helper (a dropped result is
   a compile error under `-Werror`, proved) and a report line when a wait
   used more than half its budget.
8. **One deferral withdrawn as already built**: a per-test time budget in
   the harness exists (`SELFTEST_BUDGET_MS`, 8 s, the watchdog's period).

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
in `nettest.c` -- which has eleven bare sleeps of the other kind, and
eleven places where the same idea is written out by hand. As with the
last two units, this one finishes a rule the repository already holds
rather than inventing one.

### Two shapes, and only one of them is a sleep

**Shape A — twenty-two `settle` calls in `nettest.c`, and only half of
them are the defect.** This report said twenty-two twice before counting
properly, and the correction halves the unit:

| | count | what it is |
| --- | --- | --- |
| **already a bounded wait on a condition** | **11** | `for (i = 0; i < 300 && tcp_state_of(c->tcp) != TCP_CLOSED; i++) settle(10);` — or the same written as a loop body with a `break`. This **is** `wait_until`, hand-written, with the budget expressed as iterations × 10 ms |
| **bare sleep-then-assert** | **11** | `settle(100);` then a count. No relationship between the interval and the work. **The defect.** |

**As built: twelve bare sites.** Before forging the ICMP "fragmentation
needed" quote, the path-MTU test slept twenty milliseconds so that a
blackholed 2000-byte send would have been *transmitted* before its
sequence was read; it is observable (`snd_nxt` past a baseline) and is
now waited for. Hand inspection found it only once `settle` was gone.

The eleven that already wait are not flaky and are not what this unit is
for. They would still read better as `wait_until` — a budget in
milliseconds says what it means where `i < 300` does not, and the failure
message can name what was waited for — but that is tidying, and the report
separates it from the work so that neither hides behind the other.

**Nor are the eleven bare ones all the same conversion.** The observable
differs, and so does what "done" means:

| kind | what is waited for | conversion |
| --- | --- | --- |
| a counter reaching a total | `tcp_get_stats`, the IPv4 counters | wait for the count; the budget is the only new number |
| a connection reaching a state | `tcp_state_of(c->tcp)` | wait for the state |
| readiness or a result | `ksock_ready`, `ksock_recvfrom`, `ksock_accept` | wait for the readiness bit, then keep the existing assertion on the *result* |
| a derived value | `tcp_path_mss`, `c->tcp->mss`, `snd_una` | a direct field read: waitable, but the predicate is reading TCP internals and should say so |
| a settle *inside* a loop | retry and poll loops | the loop's own termination has to be re-thought, not just the sleep — these are the ones to do by hand and last |

The first three are mechanical. A derived value read out of TCP's
internals is not, and neither is a `settle` whose surrounding loop has its
own termination to re-think. Calling them all "mechanical" is how a
conversion quietly changes what a test asserts.

**Shape B — bounds on elapsed time and on work done.** Twenty-six
assertions match a search for a duration; **one of them is not timing at
all** (`blktest.c:297` compares a configured constant), leaving
twenty-five. Of those, **the direction is what decides everything**:

| | count | what a loaded host does |
| --- | --- | --- |
| **lower bounds** — `elapsed >= MS(30)` | **13** | makes them *more* true; left alone |
| **upper bounds and work ratios** | **12** | breaks them |
| of which: generous guards — `irqtest.c:85`, `proctest.c:240` (15 s), `:927` (2 s) | 3 | a host that breaks these is genuinely broken; left alone |
| **of which: load-sensitive — this unit's Shape B work** | **9** | the table below |

13 + 12 = 25, and 13 + 3 = **16 duration assertions this unit deliberately
does not touch**.

So the unit acts on **11 Shape A sites and 9 Shape B sites, 20 in all**,
with eleven hand-written waits worth tidying alongside. The tightest of
the nine are very tight:

| site | assertion | what a loaded host does to it |
| --- | --- | --- |
| `smptest.c:350` | `cw.woke_at - sent < MS(2)` | two milliseconds for an IPI to wake an idle CPU |
| `polltest.c:89` | two clocks agree within `1 ms` | a scheduling gap between the two reads breaks it |
| `schedtest.c:368` | `d < MS(20) + 3*TICK_NS + MS(10)` | **flaked today** (`SELFTEST: sleep`) |
| `smptest.c:239` | `work[1].iterations > work[0].iterations / 4` | **flaked today** (`smp-parallel`) |
| `hvtest.c:1279` | `late_ticks < asked_ticks` | **flaked today** (`el2-guest-timer-ontime`) |
| `schedtest.c:166`, `:354`, `:485`, `smptest.c:263` | `< MS(80)`, `< MS(200)`, `< MS(500)`, `< MS(100)` | progressively safer, none proof against a busy host |

**As built**, each classified from what it asserts, with the reasoning at
the site (`fbe9c57`):

| site | outcome | what it says now |
| --- | --- | --- |
| `smptest.c:239` (`smp-parallel`) | **restate** | an observer pinned to CPU 0 watches CPU 1's counter advance; both spinners pinned, a `strays` count proves neither ran elsewhere. An advance seen from CPU 0 is two CPUs executing at once. Proved: an AP that counts only while CPU 0 sleeps fails at `obs.advanced` |
| `smptest.c:350` (`smp-wake`) | **restate** | the waiter reads `IPI_RESCHEDULE` handled on its own CPU either side of the block and the count must move; the "it is blocked now" sleep became a wait for `THREAD_BLOCKED`. The 2 ms never proved "the IPI, not the tick" -- a tick inside 2 ms passes too. Proved: the wake IPI removed in `sched.c` fails at the count, and *nothing else in the suite noticed* -- the tick carries liveness |
| `polltest.c:89` (`realtime`) | **restate** | `clock_pair` brackets the wall-clock read with two monotonic reads and re-reads a pair wider than 100 µs; the tolerance is the bracket, ten times tighter. Proved: a 5 % drift fails the agreement; a pair interrupted every time fails the bracket |
| `hvtest.c:601` (`hv-vcpu-stop`) | **restate** | `k.sent` was read the instant the run returned, but the kicker stores it *after* the stop that ends the run. The kicker is joined first and the claim is the ordering: `t1 >= sent_ns`. Proved three ways: the old check fails with the kicker held 50 ms after its stop; the new one passes under the same hold; a stop that is not the kicker's fails the ordering |
| `smptest.c:263`, `schedtest.c:354`, `:485` | **restate into what was already asserted** | each named a failure that presents as no return, not a slow one -- the watchdog (8 s, scheduler dump) or `smp_call_function_single`'s own one-second panic. Nothing a kernel does wrong lands between "one slice late" and "never"; the bounds could fail only on a loaded host. Removed |
| `schedtest.c:166` | **restate** | `udelay` spins on the very clock it was compared against (`timer.c`); the upper bound could measure only time off the CPU. Removed; the tick-rate check that follows is untouched |
| `schedtest.c:368` (`sleep`) | **widen and label** | slack 3 ticks + 10 ms → 3 ticks + 100 ms, still an order of magnitude under a coarse-mechanism sleep. `LOAD-SENSITIVE`, listed |
| `hvtest.c:1279` (`el2-guest-timer-ontime`) | **widen and label** | `late < asked` → `late < 4 × asked` (~60 ms), still an order of magnitude under a coarse park; measured 1.8 ms late against 15.6 ms asked. `LOAD-SENSITIVE`, listed |

### What it cost, on one day

| test | site | shape | times |
| --- | --- | --- | --- |
| `net-tcp-syncache` | `nettest.c:983` | A | **4**, one of them blocking a merge |
| `SELFTEST: sleep` | `schedtest.c:368` | B | 1 |
| `smp-parallel` | `smptest.c:239` | B | 1 |
| `el2-guest-timer-ontime` | `hvtest.c:1279` | B | 1 |
| `net-icmp-limit` | `nettest.c:1236` | A | 1 — **on this report's own pull request** |

**A fifth arrived while this report was being reviewed, and it is the one
worth reading.** The pull request carrying this document — which changes
one markdown file and no code — was failed by CI:

```
SELFTEST: net-icmp-limit ... FAIL: check failed:
  i1.icmp_echo_rcvd - i0.icmp_echo_rcvd == 300 at line 1236
```

That is the assertion three paragraphs of this report are about: the
`== 300` immediately after `settle(100)`, in the test family whose
deferral had been withdrawn an hour earlier on the grounds that converting
the wait would fix it. **A document about tests that sleep and then count
was blocked by a test that sleeps and then counts, at the line it
quotes.** Eight flakes across five tests in one session, and the last one
arrived to make the argument unassisted.

Every one passed on re-run. That is the definition of the problem rather
than a mitigation of it: **a suite that passes on the second attempt is a
suite whose verdict means less on the first.** Every unit in
`docs/audit/` ends with "246 self-tests PASS on both architectures", and
that sentence is worth what the suite's determinism is worth.

## Current implementation

- `settle(ms)` in `nettest.c`, twenty-two call sites -- **eleven of them
  already inside a bounded wait on a condition**, eleven bare sleeps with
  no relationship between the interval and the work.
- `threads_settle(expected)` in three scheduler and quiescence tests:
  correct, deadline-bounded, fails loudly.
- Twenty-five duration assertions (a twenty-sixth match is a constant
  comparison, not timing): **thirteen** lower bounds that are sound,
  twelve upper bounds or ratios, of which three are guards so generous
  that a host breaking them is broken. **Nine to classify.**
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

3. **Most of it is small.** Eleven bare sleeps where the observable
   already exists and only the wait is wrong -- and of those, the counter,
   state and readiness kinds are mechanical while two kinds are not.

4. **The remaining third is a design question worth asking once.** An
   assertion that an IPI wakes an idle CPU within 2 ms is trying to say
   "the IPI woke it, not the tick". That is a real property and the
   timing is a proxy for it — often a bad one, and sometimes the only one
   available. Each upper bound deserves the question *what is this really
   asserting, and can it be said without a clock?*

## Design

### Shape A: one helper, eleven conversions

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
the repository, and says so in the failure line. **As built**, the list is
the table under the "The list" heading of `docs/testing/flakes.md` and no
other table in that file, and the example below is a captured report --
`selftest_sleep` with its vCPU held 150 ms by an injected `udelay` --
rather than the one the report imagined (whose test, `net-tcp-syncache`,
is converted and not on the list):

```
boot-test: FAIL after 82.9s
  - kernel reported failure via debug-exit
  - forbidden marker /SELFTEST: FAIL/: SELFTEST: FAIL (1 of 246)
  - note: sleep is on the load-sensitive list (docs/testing/flakes.md); a re-run distinguishes a flake from a regression
  - no 'SELFTEST: PASS' line
```

A run in which a self-test failed while the file is missing, or parses to
no rows, says that instead; the list cannot go silently empty. Proved
with the harness's own functions on captured logs: a failing test that is
in the file's history table but not the list gets no note; a listed one
gets the note; the heading renamed yields "lists no tests"; the file
missing yields "is missing"; no failure yields no note whatever the
list's state.

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

**As built, the rule has one exception, and the file needed it two
functions after the rule was written.** A predicate may span several
unsynchronised fields when every field is monotonic and the predicate is
monotone in them: a torn read then under-counts, which delays termination
and never causes it. `syn_answered` (SYNs cached plus cookies sent) is
the one instance, and the contract, the predicate and the call site now
say the same thing (`ab9df52`).

**Ownership / Lifetime.** None: the helper owns nothing and outlives
nothing.

**Failure.** The expiry path is the interesting one, and it must be
`CHECK`ed by every caller rather than ignored — a `wait_until` whose
result is dropped is a `settle` again. The migration should make that
impossible to get wrong where the language allows it (a
`__attribute__((warn_unused_result))`). **As built**: the attribute is on
the helper and the kernel builds with `-Werror`; one site's `CHECK`
removed gives `error: ignoring return value of function declared with
'warn_unused_result' attribute`. A wait that used more than half its
budget also prints `selftest: wait_until: waited N ms of a M ms budget`,
the Risks section's mitigation, built.

**Security.** None: test-only code.

**Performance.** The suite gets *faster*, not slower. The bare fixed
sleeps totalling roughly 900 ms of unconditional waiting become waits that
return as soon as the condition holds — typically in single-digit
milliseconds. The generous deadlines are ceilings, not costs.

**Scalability.** The suite's runtime stops scaling with the number of
fixed sleeps and starts scaling with the work.

## Affected files

| file | change |
| --- | --- |
| `kernel-services/network/nettest.c` | `wait_until`; **12 bare `settle` sites converted** (as built; the report said 11) and 11 hand-written waits re-expressed; `settle` deleted; 24 wait sites over 12 predicates; `warn_unused_result`; the near-budget report |
| `kernel/scheduler/smptest.c`, `-/schedtest.c`, `kernel/io/polltest.c`, `kernel-services/virtualization/hvtest.c` | the upper bounds classified: 4 restated, 4 removed as restated into existing checks, 2 widened and labelled; `hvtest.c:601` restated |
| `tests/boot/run_boot_test.py` | name a failing test against the load-sensitive list in the failure line; report a missing or empty list |
| `docs/testing/flakes.md` | **new**: the list (two entries), what each bound asserts, what is not listed and why, the rule, the history |
| `docs/verification/design.md` | §6: the note, beside the per-test timing it extends (as built; not in the report's table) |
| `docs/kernel/scheduler/testing.md`, `-/smp/testing.md`, `-/smp/invariants.md`, `-/io/testing.md`, `docs/kernel-services/network/testing.md`, `-/virtualization/testing.md` | each test's description matches what it asserts now, and the waits-and-bounds rule where each suite documents itself |
| `README.md` | Status entry |

**No kernel change outside test files**, and no change any program can
observe. If the implementation finds itself editing a non-test kernel
source, the design was wrong.

## New APIs

One test-local helper. No syscall, no public header, no ABI.

## Migration plan

1. **`wait_until`, and the worst bare Shape A sites** — starting with
   `nettest.c:979`, the one that has actually flaked four times. Small enough to review as a pattern before it is
   applied to the rest.
2. **The remaining Shape A sites**, and separately the eleven
   hand-written waits, as tidying that must not be confused with the fix.
3. **`settle` deleted**, which is the check that step 2 was complete: a
   remaining caller means a missed site.
4. **Shape B, classified one at a time**, with the reasoning recorded per
   site. The three that flaked today first.
5. **The harness note and `docs/testing/flakes.md`.**
6. **The documents.**

**As built: all six steps landed**, in that order, as separate commits:
`4e5d46a` (step 1, with the proof lever corrected), `79f7365` (step 2:
ten of eleven, and the two that a mechanical conversion would have
broken), `c32afa4` (step 3: `settle` deleted, the eleven hand-written
waits, the twelfth bare site), `193d9f5` and `ab9df52` (review: the four
waits, the contract), `fbe9c57` and `b3f366d` (step 4, and one review
finding on it), `ffd6202` (step 5), and this document's commit (step 6,
with `warn_unused_result` and the near-budget report).

## Tests

The awkward part, and worth stating plainly: **this unit's subject is
test reliability, and reliability is not something a single run can
demonstrate.** A converted test passing proves nothing that the old one
did not also prove on a good day.

So the proof is a **deliberately slowed host** -- **which is where the
report was wrong; the as-run section below says what was done instead**:

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

**As built, the switch is a slowed *work* path, not a slowed host.** A
delay per frame in `lo_transmit` (400,000 iterations of a volatile loop
before the frame is queued), injected by the proof script and restored
byte-identical afterwards. Under it, steps 2 and 3 of the list above hold
exactly: the unconverted tree fails `net-tcp-syncache` at line 983 and
`net-icmp-limit` at line 1236 -- the two lines that flaked -- and the
converted tree fails nothing, waiting longer for the right answer
(syncache ~630 ms, icmp-limit ~1.7 s). The only trip under the delay is
`net-bench` against its 8 s budget, the scaffolding's own cost.

**Then a repetition count.** Twenty consecutive boots of each
architecture with no `SELFTEST: FAIL`. Twenty is not a proof of absence
and the report does not pretend otherwise; it is the number at which
today's rate — four failures across roughly forty boots — would be
expected to show at least once. **As run:** REPETITION_RESULT

**Bug-proofs**, each failing for its own reason: a converted site whose
`wait_until` result is not `CHECK`ed (the expiry passes silently — this is
`settle` in disguise, and the `warn_unused_result` should make it a
compile error rather than a test); a predicate that is already true on
entry (the wait returns immediately and proves nothing, which is the
vacuity trap this repository has hit five times in three units); and a
budget set below the work's real duration (the test fails loudly with the
count it reached, which is the failure mode the design is for).

### As run

| proof | expected | got |
| --- | --- | --- |
| per-frame delay, unconverted tree | syncache and icmp-limit fail where they flaked | `FAIL ... at line 983`, `FAIL ... at line 1236` |
| per-frame delay, converted tree | no assertion failure; waits take longer | zero failures; syncache ok (633 ms), icmp-limit ok (1751 ms), the near-budget line printed for the waits past half their budget (REPETITION_WAITED) |
| a `wait_until` result dropped | compile error | `error: ignoring return value ... 'warn_unused_result' ... [-Werror,-Wunused-result]` |
| an AP that counts only while CPU 0 sleeps | `smp-parallel` fails at `obs.advanced` | yes (line 293, and 296 after the review fix) |
| the wake IPI removed (`sched.c`) | `smp-wake` fails at the IPI count; nothing else notices | yes, line 423; 245 of 246 pass |
| the wall clock drifts 5 % | `realtime` fails at the agreement | yes, line 120 |
| every clock pair interrupted (`udelay(500)`) | `realtime` fails at `clock_pair` | yes, line 108 |
| the kicker held 50 ms after its stop, old check | fails at `k.sent` | yes, line 612 |
| the same hold, new check | passes | yes |
| a stop that is not the kicker's | fails at `t1 >= k.sent_ns` | yes, line 614 |
| harness: a failing test in the history table, not the list | no note | none |
| harness: a listed test fails | the note | the note |
| harness: heading renamed / file missing / no failure | "lists no tests" / "is missing" / nothing | each as expected |
| harness, end to end: `selftest_sleep` held 150 ms | the real bound fails and the report names `sleep` | yes (captured in `docs/testing/flakes.md`) |
| twenty boots per architecture | no `SELFTEST: FAIL` | REPETITION_RESULT |

Every injection was restored byte-identical (`cmp`, or `git checkout` on
a committed tree with `git status` clean afterwards).

## Benchmarks

1. **Suite wall-clock, before and after.** Roughly 900 ms of
   unconditional sleeping in `nettest.c` alone should mostly disappear.
   The number matters because a faster suite is re-run more willingly.
   **As run:** WALLCLOCK_RESULT
2. **The flake rate itself**, over the twenty-boot runs: the metric the
   unit exists to move, and the only honest way to state the result.
   **As run:** REPETITION_RESULT

## Risks

- **A widened deadline hides a real regression.** A test that used to
  fail in 100 ms now waits two seconds before failing, and a genuine
  slowdown looks like success until it crosses the new ceiling. The
  mitigation is that `wait_until` can report *how long it waited* when it
  is close to the budget — a test that habitually takes 1.9 s of a 2 s
  budget is a finding, not a pass. **Built**: a wait past half its budget
  prints how long it waited.
- **Shape B may not be fixable in every case.** Some properties are
  genuinely temporal. The honest outcome for those is "widen and label",
  and the report says so in advance rather than discovering it and
  quietly leaving them.
- **The load switch is test scaffolding that ships.** The same argument
  the condition-variable unit had about its probe, and it should get the
  same treatment: compiled in, argued for, and priced — or built as a
  boot parameter that costs nothing when unset. **As built, nothing
  ships**: the delay is a three-line injection into `loopback.c` that
  the proof applies and restores, which is possible only because the
  lever turned out to be the work and not the host.
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
assumptions (`thrtest`, `cwdtest`). ~~And a per-test time budget in the
harness~~ -- **withdrawn as built**: one exists, `SELFTEST_BUDGET_MS` (8 s,
the watchdog's period), and the harness has failed a test on it since
the verification unit (`docs/verification/design.md`, §6).

**A third deferral was withdrawn after review asked why it conflicted with
the scope, and the answer was that it should not have been there.** It
read: "the `net-icmp-limit` family's shared `settle` in the other
direction — a *maximum* count after a flood, which a slow host makes pass
spuriously." Five of the eleven bare `settle` sites are inside
`selftest_net_icmp_limit`, so the deferral and the conversion count
contradicted each other, which is what review found.

Looking at the code resolves it in the other direction. One `settle(100)`
feeds **both** assertions:

```c
settle(100);
ipv4_get_stats(&i1);
CHECK(i1.icmp_echo_rcvd - i0.icmp_echo_rcvd == 300);
CHECK(sent <= ICMP_RATE_PER_SEC && limited >= 300 - ICMP_RATE_PER_SEC);
```

`sent <= ICMP_RATE_PER_SEC` passes spuriously on a slow host **only
because the flood may not have landed**: fewer echoes received means fewer
replies sent, and the limit is satisfied without the limiter doing
anything. Waiting for all three hundred to arrive is exactly what makes
that assertion mean something — and `limited >= 300 - ICMP_RATE_PER_SEC`,
in the same line, *fails* on a slow host for the same reason. So the
deferred problem is not a separate one at all: **it is this unit's defect,
seen from the other side, and the conversion fixes it.** The five sites
are in scope and the deferral is gone.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01FtzXcfogMnEqCnyAVzZYFj
