# NEXT SUBSYSTEM — twelve bytes that never arrive

Date: 2026-09-17. Tree: `main` at a9b9dba (after PR #167, the harness
deadlines). Chosen from
`docs/audit/2026-09-deferred-work-inventory.md` §3.

**Subsystem: the guest's side of a connection both ends agree exists —
made to say which step failed, and then narrowed.**

This report **takes up** the inventory's §3 `net-harness` row, which PR
#167 rewrote from a symptom into a locus. The row is not struck until
the unit lands, and — as with #167 — not necessarily then: this is a
defect nobody has yet explained, and a report that promises to close it
would be promising something its predecessor learned not to.

**Built as PR #169 — step 1 only, deliberately.** The instrument is in;
the numbers are not, because the failure did not come.

**The rate is lower than this report said, and load does not raise it.**
Twenty-one x86-64 boots on this machine: **one failure, and it was the
one that prompted the report.** Six of those twenty-one ran with four
CPU spinners alongside, on the reasoning that every observed failure —
three in CI, one here — had coincided with a loaded machine. All six
passed. Not enough to call load irrelevant; enough to stop treating it
as the lever.

At roughly one boot in twenty and two minutes a boot, hunting locally is
forty minutes per expected failure with wide variance. CI's rate looked
higher earlier the same day, so the instrument ships and the next failure
reports itself — which is exactly how PR #167's four numbers produced an
answer within the hour of being pushed.

**Step 2 remains "read the numbers", and this pull request does not
contain them.** Saying so is the point: a unit that shipped an
instrument and then guessed what it would have shown would be the error
this row has already cost two retractions for.

## What is established

Not "flaky". The failure has one signature, seen on three machines and
two architectures within an hour of the harness being taught to record
its timings:

| | accept | data | gave up | accept budget unspent |
| --- | --- | --- | --- | --- |
| x86-64 CI | 92.0 s | 0 of 12 | 102.0 s | 59.1 s |
| aarch64 CI | 92.6 s | 0 of 12 | 102.6 s | 65.3 s |
| local x86-64 | 76.1 s | 0 of 12 | 86.1 s | 78.7 s |

In every case:

- the guest's `ksock_connect` returned **0**, and the blocking path
  waits for `TCP_ESTABLISHED` (`socket.c:289-300`), so the handshake
  *completed* — "sent before the connection was up" is ruled out;
- the host **accepted** the connection, within a second of the guest
  reporting ready and with a minute or more of its budget to spare;
- **zero of the guest's twelve bytes arrived**, and the host gave up ten
  seconds later on the read budget, not the accept budget;
- and everything else on the same interface in the same run was fine —
  `NETTEST: done tcp_conns=2 udp_pkts=20 quit=1`, a 256 KiB TCP echo and
  twenty UDP datagrams.

So: **twelve bytes, guest to host, on a connection both ends agree
exists, while a quarter-megabyte crosses the same interface in the same
run.**

**It reproduces on x86-64 here, and rarely.** This report first said
"about one run in three", which was one failure in three runs stated as
a rate. **Twenty-one runs in, it is one in twenty-one** — and even that
is a handful of observations, not a measurement. Twenty consecutive
passes at a true one-third rate has a probability under 0.1 %, so the
original figure was not merely imprecise, it was wrong.

Every earlier attempt for weeks was on aarch64, where it did not appear
in eleven runs, which is a fact about where to run the loop rather than
about the bug — CI has failed it on both architectures. Each boot is
about two minutes, so a hunt is tens of minutes rather than the handful
of boots an earlier draft of the plan assumed.

## What nothing can currently say

### The guest reports the wrong step

```c
/* kernel-services/network/nettest.c:907-914 */
int rc = ksock_connect(c, &host);
if (rc == 0 && ksock_sendto(c, "cosmo hello\n", 12, NULL) == 12) {
    int64_t n = ksock_recvfrom(c, buf, sizeof(buf), NULL);
    client_ok = n == 12 && memcmp(buf, "cosmo world\n", 12) == 0;
}
kprintf(client_ok ? "NETTEST: client ok\n" : "NETTEST: client failed (%d)\n", rc);
```

`rc` is the **connect's** result. Every recorded failure says
`client failed (0)`, which reports that the step that *worked* worked.
Whether `ksock_sendto` returned 12, a short count, or an error is not
printed anywhere, and neither is what `ksock_recvfrom` returned.

This is precisely the defect PR #167 removed from the host side of the
same exchange, on the other side of the wire, and it is why a fortnight
of sightings could not say whether the guest had even tried to send.

### And the connection is never asked anything

The socket layer can be interrogated about *this* connection —
`tcp_send_space(pcb)`, `tcp_recv_avail(pcb)`, `tcp_state_of(pcb)` — and
`net-harness` asks none of them. It calls `ksock_sendto`, discards the
return, and prints a variable that holds the result of a different call.

There are also global counters (`struct tcp_stats`,
`kernel/include/kernel/net/tcp.h:218` — `segs_out`, `retransmits`,
`out_refused` and more), which `net-lo-tcp` and `net-lo-tcp-loss` sample
two tests away. They are worth printing beside the per-connection
answers and they cannot substitute for them: they are per boot, so they
include this connection's own handshake and every other socket's
traffic. Design §1 says why that matters and what to use instead.

## Why it matters

- **It is the only test that leaves the machine.** Everything else in
  the network suite is loopback, a synthetic interface, or a tap this
  kernel owns both ends of. `net-harness` is the one place a real NIC, a
  real hypervisor backend and a real host stack all appear, so it is
  where a defect in any of them surfaces — and it is currently the least
  instrumented.
- **It costs every unit.** Eight sightings to the date above
  (`docs/testing/flakes.md`, "The count", is the tally of record and
  the only place it is maintained); three separate
  investigations in this session alone; a re-run each time, and each
  re-run a decision about whether the branch is at fault.
- **Something is losing data on an established TCP connection**, which
  is not a small thing to be unable to explain. Either the guest never
  put the segment on the wire, or it did and the segment was refused,
  dropped or lost — and each of those is a different defect in a
  different place.

## Design

### 1. Ask the connection, not the counters

An earlier draft of this section proposed sampling `tcp_get_stats()`
across the exchange and reading `segs_out`. **That does not work, and
the reason is worth keeping**: those counters are global and per boot.
`segs_out` counts the SYN and the ACK that `ksock_connect` itself sends,
so a zero delta is impossible after a handshake the evidence says
completed; a positive delta says nothing about the twelve bytes; and
`out_refused` may belong to another connection entirely. A table of
"mutually exclusive outcomes" built on them was not exclusive and not
about this payload.

The connection can be asked directly. `tcp_send_space(pcb)` is
`netbuf_space(&pcb->sndbuf)` (`tcp.c:1354`), and **data stays in the
send buffer until it is acknowledged**. So:

```c
uint32_t space0 = tcp_send_space(c->tcp);         /* before */
int64_t sent = ksock_sendto(c, "cosmo hello\n", 12, NULL);
uint32_t space1 = tcp_send_space(c->tcp);         /* queued */
... the recv, which fails ...
uint32_t space2 = tcp_send_space(c->tcp);         /* drained, or not */
enum tcp_state st = tcp_state_of(c->tcp);
```

`space0 - space1` says the bytes were queued. `space2` says whether they
were ever acknowledged. Both are this connection's, which is what the
global counters could not be.

### 2. And then the paradox resolves one way or the other

QEMU's user-mode networking is **a proxy, not a wire**. It terminates
the guest's TCP connection in its own stack and opens a *separate*
socket to `127.0.0.1:back_port`; that is what `hostfwd` and outbound
connections are. So slirp acknowledges the guest's data as soon as it
takes it into its own buffer, and only then writes it to the host
socket.

That is how "both ends agree the connection exists and is healthy" can
coexist with twelve bytes vanishing, which is otherwise close to
impossible for TCP. It also means the answer is one of two shapes, and
`space2` separates them:

| observation | meaning |
| --- | --- |
| `sent` < 12 or negative | never queued: the defect is in `ksock_sendto` or above, and the network is innocent |
| `space2` still short by 12, `retransmits` climbing | the guest sent and was never acknowledged — the loss is between the guest and slirp: the transmit path, virtio, the NIC model |
| `space2` still short by 12, `retransmits` flat | the guest queued it, was never acknowledged, and **never retried** — a bug in its own retransmission timer, and nothing to do with the wire |
| `space2` recovered — the data was **acknowledged** — and the host still saw nothing | slirp took the bytes, acknowledged them, and did not deliver them. That is outside this kernel, and saying so with evidence is a result |

The third row is the one worth naming in advance, as the previous unit
named its own: an unacknowledged segment *must* be retransmitted, so
flat retransmits with undrained send buffer is a defect in this
repository regardless of what slirp does.

The fourth is the one a reader should expect to be most likely, given
what slirp is — and it is also the one that would have been invisible to
the global counters, because the guest's stack would look perfect in
every one of them.

### 3. The firewall suspect, demoted

`out_refused` is global and cannot be attributed to this connection, so
it is corroboration and not a discriminator. It also does not need to
be: a refused segment is never acknowledged, so it lands in the second
or third row above by the same measurement. The counter is worth
printing beside them, and worth nothing on its own.

The circumstance that made it interesting stands and is still worth
recording: `net-harness` runs **last** of the thirty-seven network
self-tests, after every test that installs rules, and those call
`fw_flush()` on **entry** rather than exit (`nettest.c:4349`, `:4643`,
`:5107` are each three lines into a `selftest_*` function), so the last
one to install anything leaves it installed. The intermittency argues
against it being the whole story — a statically leaked rule would refuse
the segment on every run, and this fails about one boot in twenty.

### 4. What this unit does not do

It does not promise a fix. It promises that the next failure names its
own cause, the way PR #167's did within an hour — and then follows it
one step. The step after that depends on what the numbers say, and a
report that plans past the measurement would be planning past the only
thing that has produced progress on this defect.

## Affected files

| file | change |
| --- | --- |
| `kernel-services/network/nettest.c` | `selftest_net_harness`: keep `sent` and `got`; sample `tcp_send_space` before the send, after it and after the failed read; `tcp_state_of` at the end; the global counters beside them as corroboration |
| `docs/kernel-services/network/testing.md` | what the guest's line now says, beside what the host's says |
| `docs/audit/2026-09-deferred-work-inventory.md` | the row: the locus narrowed by whatever the run shows |
| `README.md` | the Status entry |

No change to the stack itself is planned. **If the unit ends up changing
`tcp.c`, `socket.c` or a driver, that is the finding** and the report as
built says which row of the table above sent it there.

## New APIs

None. `tcp_send_space`, `tcp_state_of` and `tcp_get_stats` all exist; the first two are already declared in `tcp.h` and the third is used by two neighbouring tests.

## Migration plan

1. **The instrumentation**, and a local run loop until it fails. At
   roughly one boot in twenty that is tens of minutes, not a handful —
   see the rate note above, which corrects this report's own first
   estimate. Worth considering before brute force: the harness accepts
   exactly one back-connection (`listen(1)`, one `accept`), so repeating
   the exchange within a boot would need the host side changed too, and
   that is a larger change than it sounds.
2. **Read the numbers.** One failing run selects a row of the table in
   §2, which selects what step 3 is.
3. **Step 3 depends on step 2** and the report deliberately does not
   pre-write it.
4. Docs, the inventory row narrowed, the README entry.

## Tests

| test | claim | how it fails if the change is reverted |
| --- | --- | --- |
| `net-harness` (existing) | unchanged in what it asserts | — |
| the guest's new line | a failure names the step, the returns and the segment counts | reverted, the log says `client failed (0)` and a reader is back where a fortnight of sightings left them |
| whatever step 3 becomes | — | to be written against the defect the numbers name |

**The instrumentation is not a test and this report does not pretend it
is.** Nothing here asserts a property; it makes a failure legible. The
bug-proof belongs to step 3, and will be written when there is a defect
to prove. Claiming otherwise is how the previous unit nearly shipped a
cause it had not established.

## Benchmarks

None. Three `tcp_send_space` reads, a `tcp_state_of`, one
`tcp_get_stats` pair and a longer `kprintf`, on a path that runs once
per boot. `tcp_send_space` takes the pcb's spinlock and returns a word
(`tcp.c:1354-1360`); measuring it three times around a twelve-byte send
is not a cost worth a number.

## Risks

- **The failure may not reproduce with the instrumentation in.** It is
  about one boot in twenty and the change is three send-buffer reads, a
  state read and a counter pair, so this is
  unlikely; if it happens, that is itself information and the report as
  built should say so rather than quietly running more boots.
- **The numbers may point outside this repository** — at QEMU's user
  networking. That is a real possible outcome, and the honest form of it
  is a row in the inventory naming what was eliminated, not a shrug. The
  elimination is worth the unit either way.
- **`out_refused` moving would implicate the firewall tests' teardown**,
  which is a different defect from the one being chased and should not
  be bundled into this unit's fix without its own report.
- **One measurement is one run.** The distribution matters here as it
  did in #167, and the plan's step 1 says "until it fails" rather than
  "once", but a single failing run showing one row is a lead and not a
  proof.

## Alternatives considered

- **Chase the firewall teardown first**, since `fw_flush()` on entry
  rather than exit is a real smell. Rejected as a starting point: it is
  a hypothesis, the counters test it for free as part of step 1, and
  leading with it is how the last unit spent two rounds retracting a
  cause it liked.
- **Packet capture on the host.** Heavier, and it answers a narrower
  question than the counters: it would show whether the segment reached
  the host, but not whether the guest queued it, transmitted it, or had
  it refused.
- **Widen the host's ten-second read budget.** Treats the symptom, and
  the evidence says the data is absent rather than late — the host
  waited ten seconds for twelve bytes on an idle connection.
- **Leave it.** Eight sightings to the date above, three investigations, a re-run per
  occurrence on both architectures, and an unexplained loss of data on
  an established TCP connection. The last unit made it legible; stopping
  now would waste that.
