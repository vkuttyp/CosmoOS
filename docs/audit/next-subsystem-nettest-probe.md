# NEXT SUBSYSTEM — what the accepted connection was doing

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. **This report is as built**
(PR #182), and the banner below records where the build differed from
it.

**What the build changed, found by building rather than reading:**

1. **`TCP_INFO` had to be restricted to Linux explicitly, and the test
   caught it doing the exact thing this unit exists to prevent.** The
   design said "Linux only"; the first implementation just tried the
   option and took whatever came back. macOS defines a `TCP_INFO` whose
   struct is not Linux's, so byte 0 is not `tcpi_state` there — it read
   **`FIN_WAIT1` for a plainly established connection**. A confident
   wrong answer from a probe built to stop confident wrong answers.
   `test_probe_open_peer` failed on it; the platform check is now
   explicit rather than implied.
2. **A fourth end-class, `wrong-data`.** The design named three. The
   loop already had a fourth path — a peer that answers with something
   that is not the request — and leaving it unlabelled would have put a
   connection into the roster with `still open`, which is not what
   happened to it.
3. **The reset test asserts `error`, and the bug-proof checks all three
   pairs.** A first version hedged — "error or closed, in case a FIN
   raced the reset" — against a case `SO_LINGER` with a zero timeout
   cannot produce: it sends an RST and never a FIN, measured as `error`
   four times out of four. Worse, that hedge let the bug-proof *say*
   three distinct readings while only checking two pairs, leaving out
   **closed versus reset** — which is the pair the old loop actually got
   wrong, since `except OSError: chunk = b""` made them the same
   reading. Both are asserted now.

**Subsystem: the state of slirp's own host-side connection at the moment
the exchange fails.** The accept unit (PR #177) answered *which*
connection the harness got. It has now answered the same way **every
time it has been asked**: *one connection, carrying nothing*
(`docs/testing/flakes.md`, *The count*, which owns the number so this
does not repeat it). That is the only thing
about this defect that has never varied, and it is where every remaining
question now points. What the harness still cannot say is anything at
all about that connection — whether slirp was holding it open and simply
never forwarding, whether slirp had closed its end, or whether slirp was
responsive at that moment at all.

## What is established

The tally is `docs/testing/flakes.md`, *The count*. The guest's side is
**fully accounted for** and this kernel is not at fault:

- The guest's `ksock_connect` returns 0 after **529 ms to 1510 ms**,
  against a captured healthy baseline of **150 microseconds**. (This
  report said 597 at the bottom, which was the fastest then recorded;
  sighting thirty-six went below it. The band has widened at both ends
  every time it has been tested -- `docs/testing/flakes.md`, *The
  count*, owns the running figures.)
- A reset then arrives (`rsts_in +1`), sometimes before the guest can
  write and sometimes after twelve bytes are on the wire unacknowledged.
- The host accepts exactly **one** connection and reads **zero** bytes
  from it, **every instrumented sighting since the roster existed**.

Two hypotheses are dead. A foreign or stale connection is not the
trigger — that is what "one connection" rules out, and the deliberate
stale-slot reproduction reproduced the *symptom* without being the
cause. And a SYN-retransmission mechanism has been proposed and
withdrawn **twice**, and sightings keep arriving with `retransmits +0`
while the connect times spread from 529 ms to 1510 ms.

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

2. **How the connection ended, in four classes — which the harness
   nearly knows already and throws away.** Both outcomes reach the
   roster as `0 byte(s)` today, so no sighting so far can say whether
   slirp closed its end or the harness merely timed out. That is the
   single most valuable bit the roster lacks.

   It must be **more than two**, and this is where the first version of
   this probe was wrong in the case that matters. The loop
   reads

   ```python
   try:
       chunk = sock.recv(64)
   except OSError:
       chunk = b""
   if not chunk:          # treated as "the peer closed"
   ```

   so a **reset is converted into an empty read and recorded as an
   orderly close**. The failure under investigation involves a reset,
   so a two-class probe would confidently give the wrong diagnosis
   exactly when it is finally asked the question. The classes are:

   - **`closed`** — `recv` returned `b""`: an orderly FIN from slirp.
   - **`error`** — `recv` raised, and **the errno is recorded**:
     `ECONNRESET` is a different event from a FIN and must not be
     flattened into one.
   - **`deadline`** — the receive budget expired with the connection
     still open and silent.
   - **`wrong-data`** — the peer answered with something that is not the
     request. The design first named only the three above; the loop
     already had this path, and leaving it unlabelled would have put a
     connection into the roster as "still open" when it was not (as
     built; see the banner).

   No syscall, works on both platforms, and it is the portable half of
   probe 1.

   **No write probe.** The first version of this report proposed one as
   the portable fallback and it does not work: a one-byte write to a
   socket in `CLOSE_WAIT` *succeeds*, because the local send buffer
   accepts it and `EPIPE` arrives only on a later write. An open peer
   and a closed one would both record "write succeeded". The classes
   above and probe 1 cover everything it was meant to cover, so
   **the implementation does not write to the accepted socket at
   all** — which also keeps the probe from perturbing what it measures.

3. **A timed probe through the same slirp**, to the guest's echo
   service on `127.0.0.1:tcp_port`, recording the connect and the echo
   round-trip **separately**.

   **Its value is asymmetric, and the first version of this report got
   that wrong.** The echo traverses slirp *and* the guest — which must
   accept the forwarded connection, schedule its echo thread and reply —
   so a **slow** reading does not single out QEMU's main loop; guest-side
   delay produces the same number, and reading it as "slirp was starved"
   would send the next investigation to the wrong subsystem. A **fast**
   reading is the informative one: it rules out the whole path having
   stalled, which is the only mechanism still standing after the
   foreign-connection and retransmission theories died.

**The predictions, written down before the measurement**, because the
value of a discriminating instrument is lost if the reading is
interpreted after the fact:

| probe 3 | how it ended / state | reading |
| --- | --- | --- |
| **fast** | `deadline`, `ESTABLISHED`, 0 bytes | the path was alive and *this connection's* forwarding failed — a per-connection defect, and the first evidence pointing at one |
| **fast** | `closed` (FIN), or `CLOSE_WAIT` | slirp closed its end without forwarding; the question becomes why it gave up |
| **fast** | `error` with `ECONNRESET` | slirp **reset** the host side too, not just the guest's — which would tie the two halves of the failure together for the first time |
| **slow** | either | the path stalled — **slirp or the guest**, and this probe cannot say which. Separating them needs the guest's own timestamp for the same window, which is a follow-on and is named here so the reading is not over-claimed |

The fast rows are the ones worth having. The slow row is recorded
honestly as ambiguous rather than dressed up, because a reading that
points confidently at the wrong subsystem is worse than one that admits
it points at two.

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
| `probe_open_peer` | a peer that connects, sends nothing and **stays open**: recorded as ending at the **deadline**, and (on Linux) `ESTABLISHED` |
| `probe_closed_peer` | a peer that connects and **closes**: recorded as **`closed`**, and the reading differs from the case above |
| `probe_reset_peer` | a peer that **resets** (`SO_LINGER` zero-timeout close): recorded as **`error`** with `ECONNRESET`, and differs from *both* — the case a two-class probe would have flattened into `closed` |
| `probe_records_on_failure_only` | a successful exchange takes no probes and adds no latency to the healthy path |
| `probe_survives_no_tcp_info` | with `TCP_INFO` unavailable the probes degrade to **how the connection ended** rather than raising — macOS is where this is developed and Linux is where it runs, and EOF-versus-deadline is available on both |

**The bug-proof.** The three peer cases must produce **three different**
recorded readings. A probe that reports the same thing for an open peer,
a closed one and a reset is not a probe. **Neither half of this is
hypothetical**: the write probe this report first proposed fails it
because a write into `CLOSE_WAIT` succeeds, and the two-class version
fails it because `except OSError: chunk = b""` records a reset as an
orderly close. That is the
check that this instrument is not the tally's third confidently wrong
answer — a counter that cannot distinguish the cases it was built to
distinguish is worse than no counter, because it reads like evidence.

## Risks

- **A probe can perturb what it measures.** This one does not: nothing
  is written to the accepted socket, and the four end-classes are
  recorded from reads the loop already performs. The echo probe opens
  its own connection and is taken only on the failure path, after the
  exchange has already failed and the run is lost anyway.
- **It may find nothing new.** If probe 3 is fast and the socket is
  ESTABLISHED, that is still a result — the first that points inside
  slirp rather than at it — but it names no line of code, and the report
  should not promise one.
- **`TCP_INFO`'s layout is kernel-specific.** Only byte 0 is read, and
  only on Linux, with the **four end-classes** as the portable fallback
  — `closed`, `error` with its errno, `deadline`, `wrong-data`. Not a
  write probe,
  which cannot tell an open peer from a closed one. Reading more of that
  struct would be borrowing trouble for no gain.

## Alternatives considered

- **A host-side packet capture.** The right measurement, and blocked
  without root, as above. Worth asking a human to run once by hand
  rather than designing an automated test around a privilege the test
  does not have.
- **Patch or rebuild QEMU with slirp tracing.** Decisive and far out of
  proportion: it changes the thing under test, and the defect is already
  known to be outside this kernel.
- **Keep re-running and collect more sightings.** The whole tally has
  not answered it, and every roster since the accept unit landed has
  said the same thing.
  Another sighting of the same shape adds nothing; an instrument that
  asks a new question does.
- **Give up and mark it expected.** Rejected for the same reason as
  before: the tally exists because this project does not do that.
