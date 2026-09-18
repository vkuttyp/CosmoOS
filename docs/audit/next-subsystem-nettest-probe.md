# NEXT SUBSYSTEM — what the accepted connection was doing

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This report is a design, not
an as-built.

**Subsystem: the state of slirp's own host-side connection at the moment
the exchange fails.** The accept unit (PR #177) answered *which*
connection the harness got. It has now answered the same way six times
running: **one connection, carrying nothing**. That is the only thing
about this defect that has never varied, and it is where every remaining
question now points. What the harness still cannot say is anything at
all about that connection — whether slirp was holding it open and simply
never forwarding, whether slirp had closed its end, or whether slirp was
responsive at that moment at all.

## What is established

Twenty-eight sightings (`docs/testing/flakes.md`, *The count*). The
guest's side is **fully accounted for** and this kernel is not at fault:

- The guest's `ksock_connect` returns 0 after **597 ms to 1510 ms**,
  against a captured healthy baseline of **150 microseconds**.
- A reset then arrives (`rsts_in +1`), sometimes before the guest can
  write and sometimes after twelve bytes are on the wire unacknowledged.
- The host accepts exactly **one** connection and reads **zero** bytes
  from it, six times out of six.

Two hypotheses are dead. A foreign or stale connection is not the
trigger — that is what "one connection" rules out, and the deliberate
stale-slot reproduction reproduced the *symptom* without being the
cause. And a SYN-retransmission mechanism has been proposed and
withdrawn **twice**, most recently when two of six sightings came back
`retransmits +0` and the connect times spread from 597 ms to 1510 ms.

## The problem

`roster()` records, for each connection, its peer, when it was accepted,
how many bytes were read and a preview of them. For the failing
connection those read: `127.0.0.1:40046`, `accepted at 83.5s`,
`0 byte(s)`, `b''`. Every one of those is about what the *harness* did.
**Nothing is recorded about the connection's own state**, and the
harness closes it in a `finally` without ever asking.

So the question the last six sightings all raise — *slirp connected to
us and then what?* — has no instrument behind it, which is the same
shape of gap the accept unit closed one layer up.

## The avenue that is blocked, recorded so it is not planned around

The obvious next measurement is a packet capture of the **host's**
loopback, which would show slirp's host-side connection directly: its
handshake, whether a twelve-byte segment ever leaves slirp, and when the
connection ends. On this machine that needs root — `/dev/bpf0` is
`crw------- root:wheel` and `tcpdump -i lo0` refuses — so it is not
available to an unprivileged run and cannot be part of an automated
test. It remains the right measurement for a human with `sudo` to take,
and this report does not pretend otherwise; everything below is what can
be learned **without privileges**, from the socket the harness already
holds.

## Design

Three probes, taken on the failure path only, before the sockets are
closed. Each one discriminates between causes that are otherwise
indistinguishable in the log.

1. **The accepted connection's TCP state.** On Linux — which is what CI
   runs (`debian:trixie`) — byte 0 of `TCP_INFO` is `tcpi_state`. The
   distinction that matters is **`ESTABLISHED` versus `CLOSE_WAIT`**:
   the first says slirp is holding an open host socket and has simply
   never forwarded the guest's bytes; the second says slirp closed its
   end without sending anything. Those are different defects in
   different parts of slirp. Also recorded: `SO_ERROR`.

2. **A write to it.** If a write of one byte succeeds, slirp's end is
   alive and reading; `EPIPE` or `ECONNRESET` says it is gone. This is
   the cheap confirmation of probe 1 and it does not depend on
   `TCP_INFO` being available, so the pair degrades gracefully on macOS.

3. **A liveness probe through slirp, and this is the one that tests the
   only mechanism still standing.** Connect to `127.0.0.1:tcp_port` —
   the guest's echo service, forwarded by the same slirp instance — and
   time the connect and a one-byte echo. This asks whether **slirp was
   responsive at the moment of failure at all**, which is what the
   guest's 597–1510 ms connect and the missing forward would both
   follow from if QEMU's main loop had stopped servicing slirp for a
   while.

**The predictions, written down before the measurement**, because the
value of a discriminating instrument is lost if the reading is
interpreted after the fact:

| probe 3 | accepted socket | reading |
| --- | --- | --- |
| slow or fails | either | slirp was not being serviced; the locus is QEMU's main loop, outside this kernel, and the tally can say so and stop |
| fast | `ESTABLISHED`, writable, 0 bytes | slirp was healthy and *this connection's* forwarding failed — a per-connection defect, and the first evidence pointing at one |
| fast | `CLOSE_WAIT` or write fails | slirp closed its end without forwarding; the question becomes why it gave up |

Any of the three is a better answer than the tally has now, and the
second and third would be new.

## Affected files

| file | change |
| --- | --- |
| `tests/boot/nettest.py` | the three probes on the failure path; the results carried into the roster line |
| `tests/boot/test_nettest_deadline.py` | the cases below |
| `docs/kernel-services/network/design.md` | what the harness records on failure, beside the identification rule the accept unit added |
| `docs/testing/flakes.md` | what the next sighting is expected to say, and what each reading would mean |

No kernel change. Host-side only, as the accept unit was.

## Tests

Host tests, driving `NetTest` against fake peers, no boot:

| test | asserts |
| --- | --- |
| `probe_established_peer` | a peer that connects, sends nothing and stays open: the state is recorded as established and the write succeeds |
| `probe_closed_peer` | a peer that connects and closes: the state or the write records that it is gone, and the two disagree with the case above |
| `probe_records_on_failure_only` | a successful exchange takes no probes and adds no latency to the healthy path |
| `probe_survives_no_tcp_info` | with `TCP_INFO` unavailable the probes degrade to the write result rather than raising, because macOS is where this is developed and CI is where it runs |

**The bug-proof.** `probe_established_peer` and `probe_closed_peer` must
produce *different* recorded readings; a probe that reports the same
thing for an open peer and a closed one is not a probe. That is the
check that this instrument is not the tally's third confidently wrong
answer — a counter that cannot distinguish the cases it was built to
distinguish is worse than no counter, because it reads like evidence.

## Risks

- **The probe can perturb what it measures.** Writing to the accepted
  socket puts bytes into slirp's buffer. Mitigated by doing it only on
  the failure path, after the exchange has already failed and the run is
  lost anyway.
- **It may find nothing new.** If probe 3 is fast and the socket is
  ESTABLISHED, that is still a result — the first that points inside
  slirp rather than at it — but it names no line of code, and the report
  should not promise one.
- **`TCP_INFO`'s layout is kernel-specific.** Only byte 0 is read, and
  only on Linux, with the write probe as the portable fallback. Reading
  more of that struct would be borrowing trouble for no gain.

## Alternatives considered

- **A host-side packet capture.** The right measurement, and blocked
  without root, as above. Worth asking a human to run once by hand
  rather than designing an automated test around a privilege the test
  does not have.
- **Patch or rebuild QEMU with slirp tracing.** Decisive and far out of
  proportion: it changes the thing under test, and the defect is already
  known to be outside this kernel.
- **Keep re-running and collect more sightings.** Twenty-eight have not
  answered it, and six consecutive rosters have now said the same thing.
  Another sighting of the same shape adds nothing; an instrument that
  asks a new question does.
- **Give up and mark it expected.** Rejected for the same reason as
  before: the tally exists because this project does not do that.
