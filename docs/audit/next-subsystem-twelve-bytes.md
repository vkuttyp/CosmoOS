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

**It reproduces**: about one run in three, `make ARCH=x86_64 test`, on
this developer's machine. Every earlier attempt for weeks was on
aarch64, where it did not appear in eleven runs — which is a fact about
where to run the loop, not about the bug, since CI has failed it on both
architectures. The loop is two minutes.

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

### The stack has the counters and the test ignores them

`struct tcp_stats` (`kernel/include/kernel/net/tcp.h:218`) already
carries, per boot: `segs_out`, `retransmits`, `rsts_in`, `rsts_out`,
`timeouts`, `out_refused` ("segments the chain refused"), `out_aborted`,
`out_recorded`, `dropped_no_pcb`. `tcp_get_stats()` reads them, and two
neighbouring tests already use it — `net-lo-tcp` and `net-lo-tcp-loss`
sample it around their exchanges and print the deltas.

`net-harness` samples none of it.

## Why it matters

- **It is the only test that leaves the machine.** Everything else in
  the network suite is loopback, a synthetic interface, or a tap this
  kernel owns both ends of. `net-harness` is the one place a real NIC, a
  real hypervisor backend and a real host stack all appear, so it is
  where a defect in any of them surfaces — and it is currently the least
  instrumented.
- **It costs every unit.** Eight sightings; three separate
  investigations in this session alone; a re-run each time, and each
  re-run a decision about whether the branch is at fault.
- **Something is losing data on an established TCP connection**, which
  is not a small thing to be unable to explain. Either the guest never
  put the segment on the wire, or it did and the segment was refused,
  dropped or lost — and each of those is a different defect in a
  different place.

## Design

### 1. Make both ends say what happened

The guest's message becomes the analogue of the host's:

```c
kprintf("NETTEST: client %s (connect %d, sent %lld, recv %lld, %llu segs out, "
        "%llu retransmits, %llu refused)\n",
        client_ok ? "ok" : "failed", rc, (long long)sent, (long long)got,
        d.segs_out, d.retransmits, d.out_refused);
```

with `sent` and `got` the actual returns of `ksock_sendto` and
`ksock_recvfrom`, and `d` the `tcp_get_stats()` delta across the
exchange — sampled exactly as `net-lo-tcp` already does it, which is why
this is a small change rather than a new facility.

### 2. What one failing run then tells you

The candidates are mutually exclusive on those numbers, which is the
point of choosing them:

| observation | what it means |
| --- | --- |
| `sent` < 12 or negative | the guest never queued the data; the defect is in `ksock_sendto` or above, and the network is innocent |
| `sent` = 12, `segs_out` delta 0 | queued and never transmitted: the defect is between the socket and the device |
| `segs_out` ≥ 1, `out_refused` > 0 | **the firewall refused the guest's own segment** — see below |
| `segs_out` ≥ 1, `retransmits` climbing | it left the guest repeatedly and the host never saw it: slirp, the NIC, or the host |
| `segs_out` = 1, no retransmits, nothing else | it left once, was lost, and the guest never noticed — which would itself be a bug, because TCP must retransmit unacknowledged data |

That last row is worth stating in advance. The host never sent an ACK
for data it never received, so a guest that transmitted and saw no ACK
**must** retransmit. If the counters show one segment out and no
retransmissions across ten seconds, the defect is in the guest's
retransmission timer and not in the wire at all.

### 3. The suspect the counters exist to test

`out_refused` is in `tcp_stats` because the firewall's OUTPUT chain can
refuse a connection's own segments
(`docs/kernel-services/network/design.md`, "A refused segment is the
connection's business"). And `net-harness` runs **last** of the
thirty-seven network self-tests, after `net-firewall`, `net-tcpverdict`,
`net-nat`, `net-dnat`, `net-forward` and the rest — every test that
installs rules, NAT entries or routes. Those tests call `fw_flush()` on
**entry**, not on exit (`nettest.c:4349`, `:4643`, `:5107` are all three
lines into a `selftest_*` function), so the last one to install anything
leaves it installed.

**The intermittency argues against this being the whole story**, and the
report says so rather than leading with the tidy version: a statically
leaked rule would refuse the segment on *every* run, and this fails
about one in three. If `out_refused` moves, the next question is what
makes the refusal intermittent — a conntrack entry that has expired by
the time `net-harness` reaches the end of a thirty-seven-test suite is
the obvious candidate, and it is timing-dependent in exactly the way the
observations are.

If `out_refused` does not move, this suspect is eliminated in one run
and the unit follows whichever row above did move.

### 4. What this unit does not do

It does not promise a fix. It promises that the next failure names its
own cause, the way PR #167's did within an hour — and then follows it
one step. The step after that depends on what the numbers say, and a
report that plans past the measurement would be planning past the only
thing that has produced progress on this defect.

## Affected files

| file | change |
| --- | --- |
| `kernel-services/network/nettest.c` | `selftest_net_harness`: keep `sent` and `got`, sample `tcp_get_stats()` across the exchange, print all of it |
| `docs/kernel-services/network/testing.md` | what the guest's line now says, beside what the host's says |
| `docs/audit/2026-09-deferred-work-inventory.md` | the row: the locus narrowed by whatever the run shows |
| `README.md` | the Status entry |

No change to the stack itself is planned. **If the unit ends up changing
`tcp.c`, `socket.c` or a driver, that is the finding** and the report as
built says which row of the table above sent it there.

## New APIs

None. `tcp_get_stats()` exists and is already used by two tests.

## Migration plan

1. **The instrumentation**, and a local run loop until it fails — about
   one in three on x86-64, so a handful of boots.
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

None. Two counter reads and a longer `kprintf` on a path that runs once
per boot.

## Risks

- **The failure may not reproduce with the instrumentation in.** It is
  about one in three and the change is two counter samples, so this is
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
- **Leave it.** Eight sightings, three investigations, a re-run per
  occurrence on both architectures, and an unexplained loss of data on
  an established TCP connection. The last unit made it legible; stopping
  now would waste that.
