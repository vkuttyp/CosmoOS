# NEXT SUBSYSTEM — a back-connection that survives one reset: the harness stops being a coin flip on someone else's bug

> **BUILT.** This is the report as written, with an as-built banner.
> What the build changed, and what it found:
>
> 1. **The measurement the report promised was not the one that
>    answered.** The probe hunted a one-in-twenty flake for six boots
>    and did not catch it. What settled the question instead was the
>    injected test: with the first attempt shut down from inside the
>    guest, **the second connection through the same slirp reached the
>    harness and carried the exchange** — which is the claim a retry
>    rests on, demonstrated against the real harness rather than
>    inferred from sighting thirty. The probe ships unchanged, since the
>    wild case is still worth catching, but the unit does not rest on
>    it.
> 2. **The host harness really did need no change**, and the injected
>    boot is what proves it: PR #177's roster accepted the second
>    connection and let it win, with nothing altered in
>    `tests/boot/nettest.py`.
> 3. **The retry needed its own build, not its own test.** A second
>    exchange cannot reach the harness in the same boot: the accept loop
>    runs `while winner is None`, so once the guest's first connection
>    delivers the request there is no winner-less loop left to accept
>    another. So `HARNESS_BREAK=N` and `make test-harness-retry`, in the
>    shape `SCHED_CHAOS` and `CRASH_TEST` already set, rather than a
>    `net-harness-retry` entry in the self-test table as the report
>    supposed.
> 4. **The runner requires both halves.** `--harness-retry` demands that
>    an attempt was really broken *and* that a later one carried the
>    exchange, because requiring only the second would pass a build
>    whose injection silently did nothing — an ordinary boot looks
>    exactly like a successful retry if you only check that the
>    exchange succeeded.
> 5. **The injected failure is not the flake's shape exactly.** A local
>    shutdown gives `sent -32` (EPIPE) where slirp gives `sent -104`
>    (ECONNRESET) or a reset connect. It reproduces what the retry has
>    to survive — a connection that completed its handshake and cannot
>    carry the exchange — and the report says so rather than claiming
>    the cause is reproduced.
>
> 6. **And it goes in CI**, which the report's plan did not say. Without
>    a step that exercises the retry, a change that broke it would stay
>    green until the flake next struck — and that failure would be
>    indistinguishable from the flake itself, which is the exact
>    confusion this unit exists to end. One boot per architecture, in
>    the shape of the chaos job already there.
>
> **The mutations**, each run alone on x86-64 with the boot confirmed:
>
> | # | mutation | what failed |
> | --- | --- | --- |
> | 1 | `HARNESS_ATTEMPTS` 3 → 1 | `make test-harness-retry`: `client failed every attempt (1 of 1)`, and the boot fails exactly as the flake does |
> | 2 | `HARNESS_BREAK=3`, every attempt broken | `net-harness` fails with `client_ok`, which is what the bound being a failure means |
>
> No benchmark: this unit changes no path outside the self-test.

Constitution §68 report. The inventory's `net-harness` row
(`docs/audit/2026-09-deferred-work-inventory.md` §3) is the longest in
this repository and ends in a sentence no unit of this kernel can act on:

> **Why slirp resets it is not established and is outside this kernel** …
> What is NOT named is the line of code; the next measurement is a
> host-loopback capture, which needs root, or slirp's own source — both
> outside this tree.

Three units have already worked on it. The first made the host side able
to say what it saw, the second made it say what the connection was
*doing*, and the third gave the guest a per-socket verdict instead of a
machine-wide counter. Between them they localised the defect completely:
QEMU's user-mode networking resets the guest's half of one connection
while keeping its own half open, and answers a probe through the same
instance a millisecond later.

**Nothing is left to diagnose here, and the test keeps failing** -- seven
CI jobs on 2026-09-22 alone, and eighty-four occurrences recorded since
the count began. This unit is the other half of the answer: not to find
the bug, which is not ours, but to stop a test of *this* kernel from
reporting someone else's intermittent defect as a failure of ours —
while keeping every sighting counted, because a flake that stops being
visible is a flake that stops being fixed.

## What is established (before this unit)

- **The defect is not in this kernel.** The guest is correct from first
  SYN to final reset, verified against a packet capture; slirp
  acknowledges the twelve bytes into its own buffer, never delivers
  them, and resets the guest (inventory row, the 2026-09-18
  demonstration).
- **slirp itself stays healthy.** On sighting thirty the probe built by
  `next-subsystem-nettest-probe.md` answered through the *same* slirp in
  **1 ms** while the guest's connection was dead and the host's half was
  still `ESTABLISHED`. The failure is per-connection, not per-instance.
- **The host harness already tolerates more than one connection.**
  Since PR #177 it accepts with a backlog of 8, keeps a roster of every
  connection that reaches the port, gives each its own receive budget,
  and picks the one that delivers `cosmo hello\n` as the winner. It
  keeps accepting for `BACK_GRACE_S` (10 s) after the connections that
  arrived have settled.
- **The guest connects exactly once.** `selftest_net_harness`
  (`kernel-services/network/nettest.c`) creates one socket, connects,
  sends twelve bytes, reads twelve back, and fails if any step fails.
  One reset is the end of it.

## The problem

### One reset from a third party fails a test of this kernel

The guest's back-connection is a single attempt. When slirp resets it —
during the handshake, before the write, or after a segment is on the
wire, the three shapes the tally records — the test reports
`check failed: client_ok`, the boot fails, and CI goes red on a branch
that may have changed nothing but documentation.

### Measured

**The tally** (`docs/testing/flakes.md`, "The count", which is the only
place this number lives):

| | |
| --- | --- |
| occurrences recorded | 84 |
| distinct rows | 69 |
| local reproduction rate, x86-64 | about one boot in twenty |

**On 2026-09-22 alone**, seven CI jobs failed on this one test, across
five sources -- four pull requests and `main`:

| run | branch | what the branch changed |
| --- | --- | --- |
| 35709652208 | PR #213 | the percpu-migration unit |
| 35715715021 | PR #213 | the same |
| 35719533086 | `main` | the merge of that unit |
| 35724183483 | PR #214 | **a document and a tool** |
| 35725274548 | PR #215 | **documentation only** |
| 35731929745 | PR #215 | documentation and a test instrument |
| 35734557638 | PR #216 | the balancer |

Two of those branches could not have affected the network stack at all.
That is the cost this unit is about: the signal that a change broke
something is worth less every time a red run means nothing.

### What a retry depends on, and whether it holds

The repair is obvious — connect again — and it rests on exactly one
claim: **a fresh connection through the same slirp succeeds when the
first has been reset.** The recorded evidence says it should (sighting
thirty's 1 ms probe through the same instance), but that probe was a
*different* kind of connection, made by the harness rather than the
guest.

`tools/nettest-retry-probe.py`, shipped with this report, measures the
claim directly: it adds a second, diagnostic-only exchange on the
failure path — same address, same twelve bytes, fresh socket — and
prints what happened, without changing the verdict, so the boot still
fails and the log says what a retry would have done:

```text
NETTEST: retry probe: connect 0 in 2 ms, sent 12, recv 12 -> WOULD HAVE PASSED
```

**The line above is the probe's output format, not a result.** At the
time of writing the probe is applied and boots are running; the flake
reproduces about one boot in twenty, so this is a measurement the
implementation completes and records in its banner, not one this report
can promise. If thirty boots pass without a reproduction, the banner
says that instead, and the unit rests on sighting thirty's 1 ms probe
through the same slirp rather than on a direct measurement.

The read is non-blocking with a five-second deadline, because the
failure it runs after is slirp accepting a connection and never
forwarding it: a blocking read there would turn a diagnosed failure into
a hung boot with no answer at all (found in review of this report).

## Current implementation

`kernel-services/network/nettest.c`, `selftest_net_harness`:

1. Reads the host's port from `fw_cfg`.
2. Starts TCP and UDP echo services on port 7 for the harness's forwards.
3. Creates one socket, connects to `10.0.2.2:hostport`, sends
   `cosmo hello\n`, reads, compares — `client_ok`.
4. Serves echo until the harness sends `QUIT`.
5. `CHECK(client_ok)` and `CHECK(g_h_quit)`.

Step 3 has no retry, and the failure diagnostics around it — three send
buffer samples, the pcb's pending error, the timer and work state, the
machine-wide counters — exist because three previous units needed them
to localise the defect.

`tests/boot/nettest.py`: the roster, the per-connection budgets, the
winner rule and the grace period described above.

## Why it matters

1. **It is the largest single source of red CI in this repository.**
   Seven jobs in one day, two on branches that changed no code.
2. **A red run that means nothing teaches people to ignore red runs.**
   That is the real cost, and it compounds.
3. **The diagnosis is finished.** Three units localised this; a fourth
   that looks for the same answer would find the same answer, because
   the remaining evidence is in slirp's source or a root-only capture.
4. **The test's own claim survives.** What `net-harness` exists to prove
   is that this kernel's TCP can establish a connection through a
   gateway, send, and receive. A retry does not weaken that: it removes
   a third party's ability to veto the measurement once.
5. **The tally keeps working.** A retry that hides the flake would be
   worse than the flake. The count must keep rising, in the boot log and
   in `flakes.md`, or the next person will believe it was fixed.

## Design

### 1. The exchange, not the connection, is what retries

A reset kills a connection; the thing worth repeating is the whole
exchange. On failure the guest closes the socket, creates a new one, and
runs connect–send–receive again from the start.

Retrying *within* a connection is not possible and not wanted: once the
pcb has taken a reset it is `TCP_CLOSED` with `pending error` set, and
there is nothing to resume.

### 2. Bounded, and the bound is a failure

```c
#define HARNESS_ATTEMPTS 3
```

Three attempts, then the test fails as it does today, with the last
attempt's full diagnostics and a line saying how many were made. The
bound is not a fallback: **a run that exhausts it has failed**, and the
existing failure line is what reports it.

Three is chosen from the shape of the defect rather than a guess: every
recorded sighting is a single connection being reset, never a sequence,
and slirp answered a probe through the same instance in 1 ms while one
connection was dead. If a second and a third attempt both die, the thing
being measured is no longer the one this row describes.

### 3. Every attempt is counted, and a retry is never silent

- The success line becomes
  `NETTEST: client ok (attempt N of 3)` — so a passing boot whose first
  attempt was reset *says so*, and a grep of the CI logs still finds
  every occurrence.
- Each failed attempt prints the full diagnostic block that today's
  single failure prints: the timer and work state, the three send-buffer
  samples, the pcb's pending error, the counters. Three units built that
  block; a retry must not discard it.
- The boot test's summary line carries the attempt count when it is
  above one, so `run_boot_test.py` can report it without parsing the
  guest's log.
- `docs/testing/flakes.md` gains a section for **retried-and-recovered**
  sightings, distinct from the failures, and the tally's headline number
  keeps counting both. A recovered sighting is still a sighting.

### 4. What the harness needs: nothing

The host side already accepts up to eight connections, rosters them,
budgets each separately and picks the one that delivers the request.
A second attempt arrives inside `BACK_GRACE_S` and wins on its content.
The report proposes **no change** to `tests/boot/nettest.py`, and the
implementation should treat any need to change it as a sign that the
guest's retry is arriving outside the window rather than as licence to
widen the window.

### 5. What it does not do

- **It does not fix slirp**, and it does not claim to. The inventory row
  keeps its open ending, with a pointer to this unit as what was done
  instead.
- **It does not retry anything else.** No other test's connection
  through slirp is made to retry, because no other test has a recorded
  failure of this kind. A general "retry network operations" rule would
  hide defects this kernel *is* responsible for.
- **It does not weaken the assertion.** Three resets in a row is still a
  failure, and the first attempt's failure is still printed in full.
- **It does not touch the guest's TCP.** Nothing in `tcp.c` changes: the
  stack was proved correct against a capture, and the unit that changed
  it would be changing the wrong thing.

## Affected files

| file | change |
| --- | --- |
| `kernel-services/network/nettest.c` | the attempt loop, the per-attempt diagnostics, the attempt count in both outcome lines |
| `tests/boot/run_boot_test.py` | surface the attempt count in the run's summary when above one |
| `docs/testing/flakes.md` | a section for retried-and-recovered sightings; the tally keeps counting both |
| `docs/kernel-services/network/testing.md` | what `net-harness` now asserts and why the retry is bounded |
| `docs/audit/2026-09-deferred-work-inventory.md` | the `net-harness` row: what this unit did, with the diagnosis left open |
| `README.md` | Status entry |
| `tools/nettest-retry-probe.py` | shipped with this report; deleted by the implementation, whose real retry replaces it |

## New APIs

None. This unit adds no kernel interface: it is a test, a bound and a
count.

## Migration plan

1. **The attempt loop and its diagnostics**, with the count in both
   outcome lines. Boot both architectures; the normal path must print
   `attempt 1 of 3` and be otherwise unchanged.
2. **The adversary**, because a retry that has never been exercised is
   not a retry. `tools/nettest-retry-probe.py`'s successor injects a
   reset of the first attempt from inside the guest — closing the socket
   under the test — so the retry path runs on *every* boot in that
   build, not one in twenty.
3. **The boot test's summary**, then the docs, the inventory row and the
   README entry.
4. **Release builds both architectures, `gmake host-test`, every
   mutation alone**, as usual.

## Tests

| test | what it proves | bug-proof |
| --- | --- | --- |
| `net-harness` (existing) | unchanged on the normal path: one attempt, `client ok (attempt 1 of 3)` | — |
| `net-harness-retry` (new, fault-injected) | the first attempt's socket is reset from inside the guest; the second attempt completes and the boot passes, with the line naming attempt 2 | remove the retry: the test fails exactly as the flake does today |
| the same, three resets | all three attempts reset → the test fails, with the last attempt's full diagnostics and `3 of 3` | make the bound unbounded: the test hangs rather than failing, which is the opposite of what a bound is for |

The injected reset is the only honest way to test this: waiting for
slirp to do it is a one-in-twenty event, and a test that runs one boot
in twenty is not a test.

## Benchmarks

None. This unit changes no path that runs outside the self-test, and the
only number it produces is the attempt count.

What it should be measured by instead, in the banner: **the rate of red
CI runs attributable to this flake, before and after**, over the week
that follows. The before is seven jobs in one day.

## Risks

- **A retry can hide a defect this kernel owns.** If a future change
  makes the guest's connect fail for a real reason, the retry masks the
  first two failures. Mitigations: every attempt prints the full
  diagnostic block, the success line names the attempt number, and the
  tally counts recovered sightings. A boot that says `attempt 3 of 3` is
  a boot to look at.
- **The bound could be reached by a real regression** and read as the
  flake. That is why the failure output is unchanged and complete: the
  three attempts' diagnostics are what distinguish "reset three times by
  a third party" from "this kernel cannot connect".
- **The window could move.** If the retry arrives after the harness has
  stopped accepting, the test fails in a new way. The grace period is
  10 s and the failure is detected in milliseconds, so the margin is
  three orders of magnitude; the implementation asserts the retry
  happens inside it rather than assuming.
- **The injected-reset test could diverge from the real defect.** It
  reproduces the *shape* (a reset connection, a healthy gateway) and not
  the cause, which is the most any test in this tree can do about a bug
  in QEMU. The report says so rather than implying the fault is
  reproduced.

## Alternatives considered

- **Leave it.** Seven red jobs in one day, two on documentation-only
  branches, is the argument against.
- **Mark the test as expected-to-flake** and ignore its failures. That
  is the same as deleting it, with extra steps, and it deletes the
  coverage the test really does provide.
- **Take the back-connection off slirp** — use the tap device, or a
  virtio-serial channel, or `fw_cfg` in the other direction. This is the
  strongest alternative and it is a bigger unit: the tap path is built
  and tested (`el2-tap-host`), so a harness over it would avoid the
  defect entirely rather than survive it. It is also a different test:
  connecting out through a gateway is the thing `net-harness` exists to
  prove, and a tap has no gateway. Worth its own report if the retry
  turns out not to be enough.
- **Retry inside `ksock_connect`**, so every caller benefits. No: a
  kernel that silently retries a reset connection is lying to its
  callers about what the network did, and this kernel's socket layer has
  just spent a unit learning to report exactly that
  (`docs/audit/next-subsystem-socket-verdict.md`).
- **Report the flake's cause upstream to QEMU.** Worth doing and not a
  unit of this repository; the row already records what a reporter would
  need. It also does not help this tree's CI in any useful timeframe.
