# NEXT SUBSYSTEM — a harness that cannot say why its own exchange failed

Date: 2026-09-17. Tree: `main` at b729a03 (after PR #165, the writeback
count). Chosen from `docs/audit/2026-09-deferred-work-inventory.md` §3.

**Subsystem: the network harness's deadlines — measured from the event
they are about, derived rather than guessed, and reported when they
expire.**

This report **takes up** the inventory's §3 row that begins
"`net-harness` fails intermittently, on both architectures, and **not
only in CI**". The row is not struck until the unit lands; striking it
is the last step of the migration plan.

It reports a **verified structural defect** in the harness — a
hard-coded 120-second `accept()` deadline started before QEMU launches,
shorter than the harness's own sibling deadline for the same event, and
closing the listener when it expires — and it proposes fixing that.

**It does not establish that this defect causes the observed
failures, and an earlier draft of this report said it did.** The
correction is in *The measurement that did not confirm it* below. The
distinction matters more here than almost anywhere: this row exists
because two documents drew a confident conclusion from good
observations, and replacing their wrong cause with another wrong cause
would be worse than leaving it alone.

**Built as PR #167.** As-built, with what the building changed:

- **The margin is thinner than the corrected report said.** Step 1's
  measurement is above: the back-connection lands at ~72 % of the boot,
  not the 43 % an earlier draft computed by dividing by a `make test`
  wall clock. That moves the projected CI margin from "roughly 60
  seconds, nowhere near" to **15-19 seconds**. The deadline is a far
  more plausible suspect than the retraction concluded — and still not a
  proven one, because boots of equal length pass and fail.
- **The test found a fourth property worth holding.** The plan named
  three; the built test also asserts that *an expired deadline leaves
  nothing listening*, which is the step from "late" to "fatal" and the
  reason a guest saw `ksock_connect` return 0 with no echo.
- **`flakes.md` is corrected without being given a new cause.** The plan
  said its paragraphs would be corrected; what they say now is that the
  deadline was fixed, that it was not shown to be the cause, and that
  the next sighting will carry the timings that settle it. Replacing one
  unproven cause with another was the failure mode this whole row exists
  to illustrate.
- **The inventory row is not struck.** Per the plan's condition, which
  the build did not meet.

### And then the instrumentation answered it, on the first failing run

Within an hour of this unit being pushed, its own x86-64 CI job failed
`net-harness` and printed:

```
network harness: ready at 90.9s, back-connection accepted at 92.0s,
budget 150.0s, listener closed at 102.0s
  - network harness: guest-initiated connection failed (TimeoutError('timed out'))
    — listening on 127.0.0.1:55835, gave up 102.0s after the harness started,
    guest reported ready at 90.9s
```

**The accept succeeded.** At 92.0 seconds, one second after the guest
reported ready and well inside the budget. The `TimeoutError` at 102.0
is `accept + 10`, and `conn.settimeout(10)` is on line 66: it came from
`conn.recv()`, not from `accept()`.

So the deadline this unit fixed was never the cause, which is what the
report declined to claim and is why the row stayed open. The real shape,
for the first time in seven sightings:

| | |
| --- | --- |
| the TCP connection | **established** — accepted host-side, `ksock_connect` returned 0 guest-side |
| the guest's `cosmo hello\n` | **never arrived**, in ten seconds |
| everything else on the wire | **fine** — `NETTEST: done tcp_conns=2 udp_pkts=20 quit=1` |
| the guest | spent 11.6 s (budget 8 s), consistent with sending and then blocking in `ksock_recvfrom` |

Twelve bytes, guest to host, on an established connection, while a
256 KiB TCP echo and twenty UDP datagrams crossed the same interface in
the same run. That is a far smaller and far stranger target than "the
network harness is flaky", and it is what the next unit should take.

**And it reproduces here**, which nothing in a fortnight of trying had
managed. One failure in three x86-64 runs on this machine, with the
signature identical to CI's:

```
run 2: ready at 75.3s, back-connection accepted at 76.1s, listener closed at 86.1s
  connection accepted at 76.1s, then 0 of 12 bytes: b''; gave up at 86.1s,
  guest reported ready at 75.3s, accept budget 78.7s, recv budget 10.0s
```

Accepted 0.8 seconds after readiness, with **78.7 seconds of accept
budget unspent** — so the deadline is not merely unproven as the cause,
it is excluded. The failure is ten seconds later, in the read.

Two things follow for whoever takes this next.

**The bug is on both architectures; only the local reproduction is not.**
CI failed it on x86-64 and on aarch64 within the hour, with the same
signature each time:

```
x86-64   accepted at 92.0s, 0 of 12 bytes, gave up at 102.0s, 59.1s of accept budget unspent
aarch64  accepted at 92.6s, 0 of 12 bytes, gave up at 102.6s, 65.3s unspent
local    accepted at 76.1s, 0 of 12 bytes, gave up at 86.1s,  78.7s unspent
```

Every local attempt during this unit was on aarch64, where it did not
reproduce in eleven runs, and it appeared on the first x86-64 attempt --
which says where to run the loop, not what the bug is.

**And the loop is two minutes rather than a twenty-minute CI round
trip**, which is the difference between chasing this and waiting for
it.

Worth stating about this report: its first draft named a cause, its
retraction named none, and the thing that actually produced an answer
was making the harness record four numbers. The measurement was the
unit; the deadline was a real defect found on the way to it.

## Problem

**As the tree stood before PR #167.** The defect described here was
fixed; it was *not* the cause of the failures, which the as-built
section above records and the paragraphs below reach by reasoning that
the numbers later contradicted.

`net-harness` has failed **seven times in about two weeks**, on both
architectures, on CI and on this machine, and at least three of those
were on trees that cannot have caused it — a documentation-only branch,
a `main` run, and a branch whose subject is a lock and a counter in
cosmofs. On that last branch it failed **four runs in a row**, and then
the *identical commit* passed on a re-run while the same build passed
five of five locally.

Every unit this session has paid for it in re-runs. The row and
`flakes.md` both conclude that the exchange depends on the host and
cannot be bounded by this project. That conclusion is wrong, and it has
been sending readers away from a two-line defect for two weeks.

### What the harness does

`tests/boot/nettest.py`, in `NetTest.__init__`:

```python
self.listener.bind(("127.0.0.1", self.back_port))
self.listener.listen(1)
self.listener.settimeout(120)
self.back_thread.start()        # _back_server: self.listener.accept()
```

`tests/boot/run_boot_test.py` constructs it at **`:536`** and launches
QEMU at **`:589`** (and again at `:598` if the firmware handover has to
be retried):

```python
nettest = NetTest()             # :536  -- accept() starts counting here
env.update(nettest.env())
...
proc = launch(log)              # :589  -- QEMU starts here
...
    proc = launch(log)          # :598  -- and again, if the firmware
                                #          handover needed a retry
```

That second launch matters. `FIRMWARE_HANDOVER_S` is 30 seconds, and a
run that does not see the handover in time kills QEMU and boots again
from `:598`, resetting `start` — the *run's* clock — but not the
harness's, which has been counting since `:536`. A boot that retries
therefore spends up to thirty seconds of an accept budget it does not
know it has.

So the accept deadline begins **before the guest exists**. The guest
must boot, run every self-test, reach `selftest_net_harness`, and open
the back-connection inside those 120 seconds.

And on expiry `_back_server` runs `finally: self.listener.close()` — the
port goes away.

### The measurements

| | |
| --- | --- |
| guest prints `NETTEST: ready` on this machine | **t+82 s**, where t=0 is `make test` starting |
| the back-connection | immediately after that line: the guest prints readiness at `nettest.c:899` and connects at `:907` |
| the accept deadline | **120 s**, started at `NetTest()` construction |
| margin on a machine that passes 5 of 5 | **~38 s**, and a little more: the harness's clock starts a second or two after `make`'s |
| the failing CI boots | **132 s, 142 s, 146 s** total |

**What was measured, exactly**: the appearance of the `NETTEST: ready`
line in the boot log, timed from the start of `make test`. That is not
the back-connection, it is the line the guest prints immediately before
making it — near enough to stand in for it, and named precisely here
because this is a report about a clock started against the wrong event
and it should not contain one.

The zero is also not quite the harness's zero: `make` spends a second or
two on dependency checks before `run_boot_test.py` constructs `NetTest`,
so the true margin is a little *more* than 38 seconds. A runner half
again slower than this one still crosses it, and a runner that also hits
the firmware-handover retry crosses it by thirty seconds more.

**This is one measurement, and the plan's first step replaces it with a
distribution** before any code changes. It is enough to identify the
mechanism and not enough to quote as the margin.

### The measurement that did not confirm it

The mechanism above predicts something checkable: runs whose boot is
slow enough to push the back-connection past 120 seconds should fail,
and faster ones should pass. **Measured across sixteen recent aarch64
CI jobs, that correlation is not there.**

| outcome | boot-test duration |
| --- | --- |
| PASS | 145.6 s, 143.7 s, 143.6 s, 141.6 s, 141.4 s, 140.3 s, 140.0 s, 140.0 s, 139.4 s, 137.2 s, 102.9 s |
| FAIL | 145.8 s, 142.3 s |

A **145.6-second boot passed** and a **145.8-second boot failed**. Boot
duration does not predict the outcome.

**What that does and does not show.** It does not exonerate the
deadline: the boot-test's total duration is a poor proxy for the thing
the deadline actually spans, which is `NetTest()` construction to the
guest's back-connection. `net-harness` runs partway through the suite.

**Measured, as step 1 of the plan below** (the harness now records it on
every run):

| run | back-connection accepted | boot-test total | fraction |
| --- | --- | --- | --- |
| 1 | 82.8 s | 116.7 s | 71 % |
| 2 | 79.6 s | 109.5 s | 73 % |
| 3 | 76.4 s | 105.8 s | 72 % |

So the back-connection lands at about **72 %** of the boot, and the
margin on this machine is 37–44 seconds.

An earlier draft of this section put the fraction at 43 %, from dividing
t+82 s by a 189-second figure that was the whole `make test` wall clock
rather than the boot. On that arithmetic a 140-second CI boot reached
the back-connection near 60 seconds and the deadline was nowhere near
being crossed. **The corrected fraction says otherwise**: 0.72 × 140–146
seconds puts it at **101–105 seconds against a 120-second deadline — a
margin of 15 to 19 seconds.**

That does not make the deadline the cause, and the absent correlation
above still stands: boots of the same length pass and fail. But it moves
the deadline from "not remotely crossed" to "crossed by anything that
costs the tail another fifteen seconds", which is a different suspect
altogether and a far thinner margin than a reader of this report was
previously told.

It does show that the earlier draft's confidence was unearned. The
structural defect is verified by reading the code; its *sufficiency* as
an explanation rested on a correlation that, once measured, is absent.

**So the unit's first job is the measurement that settles it**: the
elapsed time from `NetTest()` to the back-connection, recorded on runs
that pass and on a run that fails. If it is under the deadline on a
failing run, the deadline is innocent and this report has found a real
defect that is not this bug — which is still worth fixing, and is still
worth knowing, because it removes the most plausible suspect and points
the next investigation elsewhere.

### Why the recorded symptoms were consistent with it

**Superseded — see the as-built section at the top.** The measurement
that this unit added showed the accept succeeding and the read timing
out, so the readings below, which were how the deadline looked like a
sufficient explanation, are kept as the reasoning that was available
before the numbers were and not as an account of the failure.

- **`ksock_connect` returns 0 and the echo never comes.** The row
  records exactly this (`NETTEST: client failed (0)`,
  `nettest.c:907,914`), and reads it as "the guest reached slirp and
  slirp never delivered the connection". QEMU's user networking does
  complete the guest's handshake locally, so a guest connect succeeds
  whether or not anything is listening — which is why a closed listener
  *could* have produced it. What actually happened is that the listener
  was open and accepted the connection, and the twelve bytes never came.
- **The host harness reports `TimeoutError`.** In the failing CI logs it
  is the *read* giving up ten seconds after a successful accept, not
  `accept()` — which nothing could tell until the timings were printed,
  and which is the whole point of the unit. The row asks for "the host
  side instrumented on a run that fails"; the error was already there,
  and the timing was not.
- **It struck documentation-only branches and `main`**, and four times
  in a row on a branch about a lock in cosmofs, after which the identical
  commit passed. Those rule out the *tree*, which is what they have
  always shown; they do not choose between the deadline and anything else
  that varies from run to run.
- **It has never reproduced here** in any deliberate attempt — five of
  five on the branch where CI failed four times.

None of these distinguishes the deadline from another run-to-run
variable, which is the point of the section above.

### The defect in one sentence

The harness has **two deadlines for the same event, and they disagree**:

| deadline | value | starts |
| --- | --- | --- |
| waiting for `NETTEST: ready` (`run_when_ready`) | `args.timeout - 30` = **150 s** with the default 180 s | when its thread starts, *after* QEMU launches |
| `accept()` (`nettest.py:36`) | hard-coded **120 s** | at `NetTest()` construction, *before* QEMU launches |

The accept deadline is both **shorter than** and **earlier than** the
readiness deadline for the same guest. A guest that `run_when_ready`
correctly judges ready at t=130 s finds that the thing waiting to accept
it gave up ten seconds earlier and closed the port.

The harness already knows the right pattern — derive the budget from the
run's own timeout, start it with the run — and uses it in one of the two
places.

## The implementation this unit changed

**Past tense as of PR #167**: what follows describes the harness before
the unit, which is what the rest of the plan below is written against.

`NetTest` did four things in `__init__`: picked three ports, bound and
listened on the back-connection port, set a 120-second timeout, and
started a thread that blocked in `accept()`. Only the first two needed
to happen that early: the ports go into QEMU's environment, and binding
reserves the back port before QEMU is told about it. The last two are
what moved.

The guest's side, for the record, prints readiness **before** it
connects (`nettest.c:899` then `:907`), so a harness that waits for the
ready line and *then* accepts cannot miss the connection: the kernel
queues it in the listen backlog, which `listen(1)` already provides.

## Why it matters

- **It is a tax on every unit.** Seven failures in two weeks — the
  latest on *this report's own pull request*, which adds one Markdown
  file — each costing a re-run and, worse, a decision about whether the
  branch is at fault. This session spent three separate investigations
  on it.
- **The harness cannot answer the question, and that is the finding
  that survives the measurement above.** When the exchange fails the log
  says `TimeoutError('timed out')` and nothing about *when* the guest
  connected, whether it connected at all, or how much budget was left.
  Two weeks of sightings produced no way to tell the deadline apart from
  its alternatives. That is a defect in the harness whichever suspect is
  right.
- **The deadline is wrong whether or not it is the cause.** Started
  before QEMU, shorter than its own sibling for the same event, and
  closing the listener on expiry: each is indefensible on its own terms.
- **It will get worse.** The margin shrinks every time a self-test is
  added, because the deadline is measured from before the boot and the
  boot keeps growing. This session alone added three self-tests.
- **It is small.** The fix is to start the clock at the right moment and
  derive its length, both of which the file already does elsewhere.

## Design

### 1. Bind early, accept late

Keep the bind and listen in `__init__` — the port must be reserved
before QEMU is told about it. Move the `accept()` into
`run_when_ready`, after the ready line is seen:

```python
def __init__(self):
    ...
    self.listener.bind(("127.0.0.1", self.back_port))
    self.listener.listen(1)
    # No accept yet, and no timeout yet: the guest does not exist.

def run_when_ready(self, log_path, proc, timeout):
    deadline = time.monotonic() + timeout
    ...wait for "NETTEST: ready" as now...
    self.results["ready"] = ready
    if not ready:
        return
    # The guest prints readiness before it connects, so the accept
    # cannot be late; the backlog holds it if it is early.
    self._accept_back(deadline)
    self._tcp_echo()
    ...
```

### 2. One budget, derived, for the whole exchange

`run_when_ready` already receives `args.timeout - 30`. The back-connection
accept takes what is left of it:

```python
def _accept_back(self, deadline):
    remaining = max(1.0, deadline - time.monotonic())
    self.listener.settimeout(remaining)
```

That does not make the accept deadline *later* than the readiness one —
it makes it **the same deadline**, which is the point. There is one
absolute budget for the whole exchange, derived from the run's timeout
and started when the run starts; waiting for readiness and accepting the
connection draw from it in turn.

Sharing is the correct shape rather than a compromise. An accept given
its own fresh budget *after* readiness could outlast the run's own
180-second timeout, and a harness deadline that outlives the run it
belongs to is the same class of mistake as one that starts before it.

What the change removes is the **inversion**: today the accept deadline
expires *before* the readiness deadline it should outlast — 120 seconds
counted from before QEMU against 150 counted from after it — so the
thing waiting to accept can give up before the harness has even decided
the guest is ready. After, that cannot happen, because there is nothing
left to disagree.

It also removes the second hard-coded number: a run given a longer
`--timeout` now gives the exchange more room, as a reader would expect.

### 3. Say what the port was doing

When the accept does time out, the failure should distinguish "nothing
ever connected" from "something connected and it was not the guest", and
should say whether the listener was still open. One line in the failure
text, because the next person to see this deserves better than
`TimeoutError('timed out')`:

```
network harness: guest-initiated connection failed: no connection to
127.0.0.1:<port> within <n> s of the guest reporting ready (listener
open, backlog empty)
```

### 4. What this does not do

It does not make the exchange independent of the host, and the report
should not pretend to. If the host is so loaded that a loopback connect
does not complete inside the run's whole budget, the test fails and
should. The claim is narrower and checkable: **the harness will no
longer fail because it started timing before the thing it was timing.**

## Affected files

| file | change |
| --- | --- |
| `tests/boot/nettest.py` | `__init__` binds and listens, no timeout and no accept thread; `run_when_ready` accepts after readiness with the remaining budget; `failures()` gains the port and the elapsed time |
| `tests/boot/run_boot_test.py` | nothing expected — the construction/launch order stays; **if this needs editing, the design is wrong and it goes in the report as built** |
| `docs/testing/flakes.md` | the `net-harness` paragraphs gain the structural defect and the measurement that failed to confirm it as the cause. **They are not rewritten to name a new cause**: the old conclusion is marked unproven, not replaced by a second unproven one |
| `docs/kernel-services/network/testing.md` | the harness's two deadlines, and which event each is measured from |
| `README.md` | the Status entry |
| `docs/audit/2026-09-deferred-work-inventory.md` | the §3 row: struck **only if** the measurement in step 1 shows the deadline was the cause and the fix ends the failures. Otherwise the row stays open with the deadline eliminated as a suspect, which is progress and is not closure. Its "needs the host side instrumented" is corrected either way — `failures()` already prints the `TimeoutError`; what is missing is the *timing*, not the error |

No kernel change. `nettest.c` is not touched: the guest's side is
correct, including printing readiness before connecting.

## New APIs

None. One socket timeout moved and derived.

## Migration plan

1. **Measure first, on this machine**: the time from QEMU launch to the
   guest's back-connection, printed by a temporary line in the harness.
   The report has one figure (ready at t+82 s); the unit should have the
   distribution over several boots, because the margin is the whole
   subject.
2. **The failing test**, which is the interesting step: a harness-level
   check that constructs `NetTest`, waits past the old 120-second
   deadline, then connects to the back port and asserts the connection
   is accepted. It fails today in about two minutes and passes after.
   It needs no guest, because the defect is not in the guest.
3. **The change** (Design 1–3).
4. **Re-measure**: the margin after the fix is the run's whole budget
   minus the boot, rather than 120 seconds minus the boot.
5. Docs — including the corrections to `flakes.md` and the row — then
   the README Status entry, and the inventory row struck **only if**
   step 1 showed the deadline was the cause and step 4 shows the
   failures have stopped. If they have not, the row stays open with one
   suspect eliminated and the harness able to answer the question next
   time, which is what this unit is really buying.

Step 2 before step 3, and step 1 before both: this is a unit about a
number being measured from the wrong place, so measuring it from the
right place is the work.

## Tests

| test | claim | how it fails if the fix is reverted |
| --- | --- | --- |
| `tests/boot/test_nettest_deadline.py` (new, host-side) | a back-connection made long after the harness was constructed is still accepted, provided the run's budget has not expired | reverted, `accept()` has already timed out and closed the listener: the connect is refused and the test says so by name |
| `net-harness` (existing, every boot) | the exchange completes | unchanged in content; what changes is that it stops failing for a reason that has nothing to do with the network |
| the measurement in step 4 | the margin is the run's budget minus the boot | a number in the unit's report, not an assertion |

The first is the bug-proof and it is deliberately **not** a guest test.
The defect is entirely in the harness, and a proof that boots a guest
would take three minutes to demonstrate something a socket and a clock
can show in two.

**It must not prove the bug by waiting 120 seconds.** The deadline
becomes a parameter with the production value as its default, and the
test passes a small one — which is the same shape as the defect
(a deadline that should be derived, not hard-coded) and is the reason
the test is fast.

## Benchmarks

None. The change removes waiting; it cannot add time. The one number
worth recording is the margin, which is the subject.

## Risks

- **The listener is bound in `__init__` and accepted much later.** That
  is already true for the 120-second window; the unit lengthens it. The
  port is held by this process throughout, so nothing else can take it —
  which is more than can be said for `tcp_port` and `udp_port`, whose
  `free_port()` binds, closes and hands the number to QEMU to re-bind
  later. That is a real race and it is **not** this unit: no failure has
  been traced to it, and a report should not bundle a second defect it
  has not seen fire. It goes in the inventory instead.
- **The diagnosis rests on one local timing measurement** (ready at
  t+82 s) plus the CI boot durations. Step 1 of the plan exists to
  replace that with a distribution before any code changes.
- **A genuinely broken network now fails later**, since the accept can
  use the rest of the run's budget rather than giving up at 120 s. That
  is the correct trade: the run has its own timeout, and a test that
  fails at the run's limit is not worse than one that fails early for
  the wrong reason.
- **Correcting two documents that say otherwise.** `flakes.md` and the
  inventory both assert the host-dependent reading. The unit changes
  them, and the report is careful to say what that reading got right —
  the observations were accurate; the conclusion drawn from them was
  not.

## Alternatives considered

- **Raise 120 to 300 and move on.** The cheapest change and it treats
  the symptom: the deadline would still start before QEMU and still be a
  number nobody derived, and the margin would still shrink with every
  self-test added. It would also leave both documents wrong.
- **Re-run on failure automatically.** `flakes.md` argues against
  exactly this ("hiding a flake is worse than the flake") and it is
  right; it would also have hidden a real defect for as long as the
  project lasted.
- **Make the guest connect earlier in the boot.** Moves the test to fit
  the harness rather than fixing the harness, and buys a margin that the
  next self-test spends.
- **Accept that it is host-dependent.** What the tree currently
  believes, and this report does **not** refute it. An earlier draft
  said the numbers did; they do not, and the section above says so. What
  can be said is narrower: the tree believes it *without having been
  able to check*, because the harness does not record the one thing that
  would distinguish host-dependence from a deadline — when the guest
  connected relative to when the harness started waiting. Accepting a
  conclusion that nothing can test is the position this unit ends, and
  it ends it whichever way the measurement falls.
