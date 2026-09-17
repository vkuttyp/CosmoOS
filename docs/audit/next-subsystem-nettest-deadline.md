# NEXT SUBSYSTEM — the flake that was a stopwatch started before the race

Date: 2026-09-17. Tree: `main` at b729a03 (after PR #165, the writeback
count). Chosen from `docs/audit/2026-09-deferred-work-inventory.md` §3.

**Subsystem: the network harness's deadlines — measured from the event
they are about, and derived rather than guessed.**

This report **takes up** the inventory's §3 row that begins
"`net-harness` fails intermittently, on both architectures, and **not
only in CI**". The row is not struck until the unit lands; striking it
is the last step of the migration plan.

It also **corrects** that row and
`docs/testing/flakes.md`, both of which record this as host-dependent
and unbounded. It is neither. It is a hard-coded 120-second timer
started before QEMU launches.

## Problem

`net-harness` has failed **six times in about two weeks**, on both
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
QEMU at **`:598`**:

```python
nettest = NetTest()             # :536  -- accept() starts counting here
env.update(nettest.env())
...
proc = launch(log)              # :598  -- QEMU starts here
```

So the accept deadline begins **before the guest exists**. The guest
must boot, run every self-test, reach `selftest_net_harness`, and open
the back-connection inside those 120 seconds.

And on expiry `_back_server` runs `finally: self.listener.close()` — the
port goes away.

### The measurements

| | |
| --- | --- |
| guest reaches `NETTEST: ready` on this machine | **t+82 s** |
| the accept deadline | **120 s**, from before QEMU |
| margin on a machine that passes 5 of 5 | **~38 s, about 46 %** |
| the failing CI boots | **132 s, 142 s, 146 s** total |

A runner half again slower than this one puts the back-connection at
about 123 seconds. That is the whole phenomenon.

### Why every recorded symptom follows

- **`ksock_connect` returns 0 and the echo never comes.** The row
  records exactly this (`NETTEST: client failed (0)`,
  `nettest.c:907,914`), and reads it as "the guest reached slirp and
  slirp never delivered the connection". True, and the reason is that
  the listener had already closed: QEMU's user networking completes the
  guest's handshake locally and only then connects to
  `127.0.0.1:back_port`, so a guest connect succeeds whether or not
  anything is listening.
- **The host harness reports `TimeoutError`.** It is in the failing CI
  logs today — `network harness: guest-initiated connection failed
  (TimeoutError('timed out'))` — and it is `accept()` giving up, not the
  network misbehaving. The row asks for "the host side instrumented ...
  on a run that fails"; the instrument is already there and already
  answering.
- **It strikes loaded CI and not this machine.** It is a function of
  boot *speed*, not of anything in the tree.
- **It struck documentation-only branches and `main`.** Same reason.
- **Four in a row, then the same commit passing.** A slow window, not a
  property of the commit.

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

## Current implementation

`NetTest` does four things in `__init__`: picks three ports, binds and
listens on the back-connection port, sets a 120-second timeout, and
starts a thread that blocks in `accept()`. Only the first two need to
happen that early: the ports go into QEMU's environment, and binding
reserves the back port before QEMU is told about it.

The guest's side, for the record, prints readiness **before** it
connects (`nettest.c:899` then `:907`), so a harness that waits for the
ready line and *then* accepts cannot miss the connection: the kernel
queues it in the listen backlog, which `listen(1)` already provides.

## Why it matters

- **It is a tax on every unit.** Six failures in two weeks, each one
  costing a re-run and — worse — a decision about whether the branch is
  at fault. This session spent three separate investigations on it.
- **It has been teaching the wrong lesson.** `flakes.md` says a
  host-dependent exchange "is not a bound this project can widen", and
  the inventory says the same. Both are wrong, and both are *documents
  this project wrote to stop itself chasing ghosts*. A wrong entry in
  the anti-ghost file is worse than no entry.
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

That makes the accept deadline **later** than the readiness deadline by
construction, which is the relationship the two need and do not have.
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
| `docs/testing/flakes.md` | the `net-harness` paragraphs corrected: not host-dependent-and-unbounded, a deadline started before QEMU. The standing rule about re-runs stays, with this as the case where a re-run was the wrong instrument |
| `docs/kernel-services/network/testing.md` | the harness's two deadlines, and which event each is measured from |
| `README.md` | the Status entry |
| `docs/audit/2026-09-deferred-work-inventory.md` | the §3 row struck, and its "needs the host side instrumented" corrected — the instrument was already reporting |

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
   the README Status entry and the row struck.

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
  believes. It is refuted by the numbers above: the failing runs are not
  runs where loopback was slow, they are runs where the boot was slower
  than a timer that started too early.
