# NEXT SUBSYSTEM — the connection the harness accepted

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This report is a design, not
an as-built.

**Subsystem: a back-connection the harness can identify, and a failure
line that names the connection it got instead.** `tests/boot/nettest.py`
listens on the back-connection port with a backlog of one
(`nettest.py:47`) and then accepts exactly once, blindly
(`nettest.py:72`). It assumes the first connection to arrive is the
guest's. It never checks, and when the assumption is false it reports
`TimeoutError`, which names nothing. That is the whole of what
`net-harness` has said for two weeks: twenty-one sightings across twelve
entries (`docs/testing/flakes.md`, *The count*), on both architectures,
on CI and locally, several of them on branches that change no code at
all.

On 2026-09-18 the failure was reproduced deliberately, and the
reproduction is the reason to build this now: **occupying that single
backlog slot before QEMU starts reproduces the `net-harness` signature
on the first boot**, and the packet capture shows the guest behaving
correctly from first SYN to final reset. The inventory row
(`docs/audit/2026-09-deferred-work-inventory.md` §3) ends "Why slirp
resets it is not established and is outside this kernel". That is still
true. What this unit changes is that the harness stops reporting an
unnamed timeout and starts reporting *which* connection it accepted and
what that connection did.

## Problem

The harness owns three host ports and holds the back-connection port for
the whole boot:

```python
self.back_port = free_port()
self.listener = socket.socket()
self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
self.listener.bind(("127.0.0.1", self.back_port))
self.listener.listen(1)
```

and later, once the guest has printed `NETTEST: ready`:

```python
conn, _ = self.listener.accept()
self.results["back_accept_s"] = time.monotonic() - self.t0
conn.settimeout(BACK_RECV_S)
while not data.endswith(b"\n") and len(data) < 64:
    chunk = conn.recv(64)
    ...
```

Two decisions in that code are load-bearing and neither is checked:

1. **The backlog is one.** Measured on this host: with one unaccepted
   connection queued, a second connect **hangs silently** — the SYN is
   dropped, `connect` does not refuse, and a client with a two-second
   timeout simply times out. It does not fail fast, so nothing upstream
   learns that the queue was full.
2. **The accept is blind and single.** Whatever connection is dequeued
   first becomes "the guest's". If it is not, the guest's own connection
   is still sitting in the queue — or, with a backlog of one, was never
   allowed in — and the harness spends its ten-second receive budget
   reading a socket that will never carry `cosmo hello\n`.

Together these convert "something else reached this port" into a
ten-second timeout attributed to the guest's network stack.

## What the reproduction established

A deliberate adversary — a single silent connection opened to
`back_port` before QEMU starts, so the slot is occupied when the guest
dials back — was added behind an environment variable and one aarch64
debug boot was run. It failed on the first attempt, with the signature
the tally has been collecting:

| | wild sightings | induced, first boot |
| --- | --- | --- |
| host `accept` | succeeds | succeeds at 76.6 s |
| host bytes | 0 of 12 | 0 of 12, `b''` |
| host verdict | `TimeoutError` | `TimeoutError` |
| guest `connect` | `0` in six of seven | `0`, but **217 ms** |
| guest outcome | reset, `rsts_in +1` | `recv -104`, `rsts_in +1` |

The guest's line in full:

```
NETTEST: client failed: connect 0 in 217 ms, sent 12 in 0 ms,
  recv -104 in 9966 ms, pending error -104,
  sndbuf free 65536 before, 65524 after send, 65536 after read
  (outstanding 12 then 0), state 0,
  segs_out +6 retransmits +2 refused +0 rsts_in +1
```

and the wire, from the capture taken during that boot:

```
418.239235  10.0.2.15.50546 > 10.0.2.2.51821  [S]                    SYN
418.456149  10.0.2.2.51821 > 10.0.2.15.50546  [S.]                   SYN-ACK, 217 ms later
418.456851  10.0.2.15.50546 > 10.0.2.2.51821  [P.] seq 1:13, len 12  the twelve bytes
418.456873  10.0.2.2.51821 > 10.0.2.15.50546  [.]  ack 13            slirp acknowledges them
428.422331  10.0.2.2.51821 > 10.0.2.15.50546  [R.]                   RST, ten seconds later
```

**slirp acknowledged the twelve bytes into its own buffer, never
delivered them, and reset the guest ten seconds later** — while the
harness held a connection that was never the guest's. `nettest.c`
already names this outcome in a comment written before it was ever
observed: a drained send buffer with nothing at the far end "is a real
and expected outcome, and it is the one that says this kernel is not at
fault". The capture is that comment, measured.

The `217 ms` matters on its own. A healthy back-connection, captured
from a passing boot, is answered by slirp in **150 microseconds** and
the whole exchange — SYN, SYN-ACK, twelve bytes each way, FIN with
`seq 13, ack 13` — completes in **52 ms**. There was no such baseline
before this; every sighting in the tally was read without one.

**What this does and does not prove.** It proves that a foreign
connection on that port reproduces `net-harness` exactly, that the
harness cannot tell the difference, and that the guest kernel is correct
throughout. It does **not** prove that a foreign connection is what
happens on CI: the adversary was injected. The wild trigger remains
unnamed, and this report does not guess at one — the last correlation
recorded on this thread, a SYN-retransmission pattern, had to be
withdrawn when a fourth run contradicted it.

## Current implementation

`tests/boot/nettest.py` is the host half; `kernel-services/network/nettest.c`
(the `nettest_client` path, around line 920) is the guest half. The
guest half was instrumented across three previous units and is now the
better-instrumented of the two: it samples `tcp_get_stats` *before* the
connect, times each step, and prints the pcb's own verdict from
`ksock_error` rather than a machine-wide counter
(`docs/audit/next-subsystem-socket-verdict.md`).

The host half records its own timings into `self.results` —
`back_accept_s`, `back_bytes`, `back_data`, `back_done_s` — and prints
them on failure (`nettest.py`, `failures()`). What it does not record is
**anything about the connection it accepted**: not the peer, not whether
more connections were waiting, not whether any other connection arrived
during the ten seconds it spent waiting on the wrong one.

## Why it matters

- **The instrument misattributes.** A failure whose cause is entirely on
  the host side is reported in a form that reads as a guest TCP defect,
  and has been read that way: three of the four framings in the
  inventory row describe symptoms of something that had already
  happened.
- **It reports several times a day now.** The CI rate rose sharply on
  2026-09-17 and has not fallen (`flakes.md`). Every occurrence costs a
  re-run and a read, and delivers one word: `TimeoutError`.
- **It is the last open row of a four-unit arc.** The nettest-deadline,
  twelve-bytes and socket-verdict units each closed an instrument gap on
  the guest side. The remaining gap is on the host side, and it is the
  one that has been answering every time.

## Design

The rule: **the guest's connection is the one that delivers
`cosmo hello\n`. Every other connection is evidence, and evidence is
reported, not discarded silently.**

1. **Raise the backlog.** `listen(8)`. One is not a design choice here,
   it is the default nobody revisited, and it is what turns a foreign
   connection from a nuisance into a stall: with a deeper queue the
   guest's connection is admitted even when something else is ahead of
   it.

2. **Accept every connection until the request arrives or the deadline
   expires.** Not one accept: a loop. Keep each accepted connection open
   and non-blocking, and `select` across all of them plus the listener.
   The first connection to deliver `cosmo hello\n` is the guest's; reply
   `cosmo world\n` on that one.

3. **Record every connection, not just the winner.** For each: its peer
   address and port, when it was accepted relative to `t0`, how many
   bytes it sent, and the first thirty-two bytes of whatever that was.
   This is the part that names the wild trigger the next time it
   happens.

4. **Report them in the failure line.** Today's message ends
   "connection accepted at 76.6s, then 0 of 12 bytes". It should end
   with the roster: how many connections arrived, from which peers, and
   what each sent. A failure where exactly one connection arrived and it
   was silent is a different defect from one where two arrived and the
   second carried the request, and the line must distinguish them.

5. **Keep the two deadlines separate and per-connection.** The accept
   budget stays what remains of the run's budget; the receive budget
   stays `BACK_RECV_S`, but measured from *that connection's* accept
   rather than from the first one, so a late-but-correct guest
   connection is not charged for time spent on an earlier intruder.

6. **A connection that arrives and delivers nothing is still a
   failure.** Raising the backlog must not turn a real guest-side fault
   into a pass. If the deadline expires with no connection having
   delivered the request, the run fails exactly as it does now — with a
   better line.

## Affected files

| file | change |
| --- | --- |
| `tests/boot/nettest.py` | `listen(8)`; `_back_server` becomes a select loop over the listener and all accepted connections; per-connection records; the failure line carries the roster |
| `tests/boot/test_nettest_deadline.py` | the new host-side cases below, including the adversary as a permanent regression test |
| `docs/testing/flakes.md` | *The count*: record the reproduction, the 150 µs / 52 ms baseline, and what the harness now reports |
| `docs/audit/2026-09-deferred-work-inventory.md` | strike the §3 `net-harness` row down to what remains open: the wild trigger |
| `docs/kernel-services/network/design.md` | the harness's identification rule, stated where the harness is described |

No kernel change. The guest half is already correct and already
instrumented; this unit is entirely host-side.

## New APIs

None. Nothing in the kernel changes, no syscall, no selftest registry
entry. The host harness grows internal structure only.

## Migration plan

Single commit, no staging: the harness is test-only and has no
consumers beyond `gmake test`. The existing `nettest-deadline` host test
is the guard that the change does not regress the deadline behaviour the
earlier unit established, and it runs in CI already (`host-test`).

## Tests

`tests/boot/test_nettest_deadline.py` is a host test — it drives
`NetTest` directly with fake guests, no QEMU — so every case below runs
in about a second and needs no boot.

| test | asserts |
| --- | --- |
| `back_stale_slot` | a silent connection opened before the guest's: the harness still finds the guest's request, and the run passes |
| `back_stale_named` | that same run records the intruder — peer, accept time, zero bytes — and a failing variant names it in the failure line |
| `back_wrong_data` | a connection that sends something other than `cosmo hello\n` does not become the guest's, and is recorded |
| `back_two_connections` | when the request arrives on the *second* connection, the reply goes out on that one and the exchange succeeds |
| `back_none_delivers` | deadline expires with connections accepted but no request: still a failure, roster reported |
| existing cases | unchanged and still passing — the deadline semantics from the earlier unit are not altered |

**The bug-proof.** With the fix reverted, `back_stale_slot` must fail
with exactly today's symptom: `accept` succeeds, zero of twelve bytes,
`TimeoutError`, nothing named. That is the check that the test is not
vacuous — the failure mode it guards against has been demonstrated to
occur, so a version of the test that passes against the old code is
testing nothing.

The boot-level adversary used for the reproduction
(`COSMO_NETTEST_STALE_BACKLOG=1`) is kept out of the committed harness;
its job is done and the host tests cover the same ground in a second
instead of two minutes.

## Benchmarks

None. The healthy exchange is 52 ms end to end and this changes nothing
on that path — the select loop exits on the first connection that
delivers the request, which in a healthy run is the first connection
accepted.

## Risks

- **This names the trigger; it does not fix it.** If the wild cause is
  something other than a foreign connection, these tests will pass and
  `net-harness` will keep happening — with a failure line that finally
  distinguishes the two cases. That is the point of the unit and it
  should not be oversold as a fix.
- **A deeper backlog could mask a real fault.** Mitigated by design
  point 6: the pass condition is still that the guest's request arrived,
  not that some connection arrived.
- **The local rate is x86-64's.** The tally measures one boot in
  twenty-one on x86-64 and records that aarch64 did not reproduce it in
  eleven runs. A twenty-two-boot aarch64 hunt on 2026-09-18 found
  nothing, which is consistent with that and is not evidence about the
  defect — noted here so the next person does not spend the same hour.
  Any boot-level verification of this unit belongs on x86-64.

## Alternatives considered

- **Raise the backlog and nothing else.** Cheapest, and it would very
  likely have prevented the induced failure. Rejected as the whole unit:
  it leaves the harness unable to say what happened, which is the
  property that has cost twenty-one sightings. The backlog change is
  design point 1 precisely because it is necessary and insufficient.
- **Verify the peer instead of the payload.** Check that the connection
  comes from QEMU's process. Rejected: every connection arrives from
  `127.0.0.1` through slirp's own socket, so the peer does not identify
  the guest; the payload does, and the payload is already defined.
- **A fixed, well-known back port.** Would remove `free_port`'s
  ephemeral-range allocation from the picture. Rejected: it trades a
  rare collision for a guaranteed one across concurrent runs, and the
  ephemeral allocator was measured clean — zero collisions in three
  thousand triples over the observed range 49152–65535.
- **Mark `net-harness` a known flake and stop failing on it.** Rejected
  outright. The tally exists because this project does not do that, and
  the one sighting that broke the pattern (`sent 12`, a segment on the
  wire) is exactly what would have been lost.
