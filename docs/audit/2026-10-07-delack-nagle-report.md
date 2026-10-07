# The delayed acknowledgement against a Nagle peer

Date: 2026-10-07. Branch `delack-nagle-latency` from `main` at
`aea60ce7`. Scope: a measurement, the two tests that keep it measured,
and the comparison tool; no TCP rule changed.

## The question

PR #322 (`b76e4536`) made the delayed acknowledgement fire for the first
time: an odd in-order segment is now acknowledged by the next segment, by
the application's read, or by the 40 ms timer, where before it was
acknowledged at once on the receive path's own output
([tidy-up report](2026-10-07-completion-waits-tidy-up-report.md)). A peer
with Nagle on -- the Linux and macOS default -- that writes a request in
two small parts holds the second until the first is acknowledged; against
a receiver that acknowledges only from the timer, every request costs
40 ms. The read-driven acknowledgement was expected to cover the common
case. This unit measures it.

## What can measure it, and what cannot

**The host harness cannot show the interaction with the guest.** QEMU's
user-mode backend (libslirp) terminates the guest's TCP in its own stack
and writes onward to a host socket, and that stack never holds a small
segment for an acknowledgement:

```c
/* libslirp src/tcp_output.c, 4.9.x -- the Nagle test, disabled */
if ((1 || idle || tp->t_flags & TF_NODELAY) &&
    len + off >= so->so_snd.sb_cc)
    goto send;
```

So no segment toward the guest ever waits on the guest's acknowledgement,
whatever the host client does. And in the other direction the guest has
no Nagle of its own (plan §7 lists it as not implemented), so a guest
client's second write is never held. A host-side Nagle client against a
guest server, through the harness, exercises the *host kernel's* Nagle
against the host kernel's own delayed acknowledgement on the loopback leg
between the harness and QEMU -- a measurement of the host, not of this
stack.

**The peer can be built.** On loopback, a client that sends the first
half of a request, waits until its send buffer is acknowledged (whole
again), and only then sends the second half is a Nagle sender by
construction, and its peer is this stack's TCP with the delayed
acknowledgement in question. That is `net-tcp-nagle-peer`.

## What was built

| Piece | Where |
|---|---|
| `net-tcp-nagle-peer` (kernel self-test, every debug boot) | `kernel-services/network/nettest.c`: `tcp_wwr_server_thread`, `nagle_peer_mode`, `selftest_net_tcp_nagle_peer`; registered in `kernel/core/selftest.c` |
| The write-write-read service on port 8, and the reverse exchange on the back-connection | `kernel-services/network/nettest.c`: `h_wwr_thread`, `h_wwr_client`; `NETTEST: wwr guest-client ...` lines; `wwr_conns` in the done line |
| The host side: port-8 forward, two batches of 50 rounds each way, the latency line, the bounds | `tests/boot/nettest.py` (`WWR_HALF`, `WWR_ROUNDS`, `WWR_BUDGET_S`, `_wwr_client`, `_wwr_host_reply`, `latency`, `failures`, `guest_failures`); `tests/boot/run_boot_test.py` prints the line and judges the guest's two |
| The comparison tool | `tools/delack-nagle-probe.py [--old]`: a clone of HEAD, with `--old` the rule before `b76e4536` (the pure-ACK branch fires on `delack_pending`, the read sets no `ack_now`), booted through the standard harness |
| Invariant N27, the design paragraph, the test descriptions | `docs/kernel-services/network/invariants.md`, `design.md` ("TCP", "The harness protocol"), `testing.md` |
| The harness's own unit test | `tests/boot/test_nettest_deadline.py` records the exchange as a passing run does |

### The shapes

A request is two 16-byte halves written as two sends; the answer is the
request, sent once both halves are in. Fifty rounds a batch.

| Shape | Sender | Receiver | What a 40 ms stall would need |
|---|---|---|---|
| host -> guest, Nagle | harness, default socket | guest port 8 | the host kernel to withhold the ACK of the first half from the harness; the guest is not on that path |
| host -> guest, `TCP_NODELAY` | harness | guest port 8 | nothing: the path's latency, bounded at a 25 ms median |
| guest -> host, host Nagle | guest, on the back-connection | harness, answering in two writes | the host kernel to withhold the ACK of the host's first write from itself (loopback to QEMU) |
| guest -> host, host `TCP_NODELAY` | guest | harness | nothing: the path's latency, bounded at a 25 ms median from the guest's line |
| loopback Nagle peer, server reading at once | the test's client | `tcp_wwr_server_thread` | the guest to withhold the ACK of the first half past the read |
| loopback Nagle peer, server busy 5 ms before each read | the test's client | the same, `param` = 5 | the same, with the read 5 ms late: the ACK must still leave with it, not 40 ms later |

## The measurements

All figures are from the validation chain below (debug builds,
`LOCKDEP=1`, QEMU TCG on the macOS host, one QEMU at a time at the
default priority) and from `tools/delack-nagle-probe.py`, whose `--old`
tree is this branch with `b76e4536`'s change to `tcp.c` reverse-applied --
the rule before it, and nothing else different. Times in microseconds,
min/median/max of 20 (the loopback peer) or min/p50/p90/max of 50 (the
harness).

### The Nagle peer on loopback (`net-tcp-nagle-peer`)

| Tree | Arch | Server reading at once: first half acknowledged | ... exchange | Server busy 5 ms before each read: first half acknowledged | ... exchange |
|---|---|---|---|---|---|
| before `b76e4536` (probe `--old`) | x86-64 | 459/650/2420 | 1350/1948/5239 | 470/745/1047 | 2457/7925/10961 |
| before `b76e4536` (probe `--old`) | AArch64 | 294/414/832 | 928/1224/2452 | 330/606/979 | 1358/7702/9178 |
| this branch (probe) | x86-64 | 400/688/1251 | 1058/1753/2859 | 785/7095/8623 | 1900/7585/9091 |
| this branch (probe) | AArch64 | 504/738/1410 | 1412/1940/3460 | 888/7098/8396 | 2154/7635/9509 |
| this branch, 4 CPUs | x86-64 | 499/613/1079 | 1268/1542/2584 | 839/7102/8334 | 1917/7591/8823 |
| this branch, 4 CPUs | AArch64 | 532/832/1541 | 1497/2007/3703 | 873/7121/8658 | 2215/7632/10299 |
| this branch, 2 CPUs | x86-64 | 556/800/1990 | 1511/2074/5550 | 1705/7208/8567 | 3628/7747/9334 |
| this branch, 2 CPUs | AArch64 | 702/967/2961 | 1728/2378/6109 | 924/7088/8345 | 2373/7612/8853 |
| this branch, 1 CPU | x86-64 | 879/1089/2343 | 2005/2429/5845 | 1267/6918/8462 | 2986/7590/9010 |
| this branch, 1 CPU | AArch64 | 704/882/2184 | 1482/2046/4528 | 947/7174/8924 | 2342/7578/9326 |
| this branch, chaos migrator | x86-64 | 759/907/2125 | 2004/2325/4319 | 1732/7059/8339 | 3597/7619/8907 |
| this branch, chaos migrator | AArch64 | 498/881/1682 | 1320/2136/3502 | 920/7148/8538 | 2165/7654/9040 |

What the two trees differ in is one column: with a busy server, the
old rule acknowledged the first half at once (0.6-0.7 ms, from the receive
path) and this branch acknowledges it when the server reads (7.1 ms: the
5 ms sleep, quantised by the 4 ms tick to about 7). The exchange is the
same on both (7.6-7.9 ms), because a request that needs its second half
cannot be answered before the read either way -- the Nagle peer's second
half arrives at the read, which is exactly when the server wants it. With
a reading server both trees acknowledge within a millisecond. Nowhere is
there a 40 ms term: that would need the acknowledgement left to the
timer, and the read never leaves it (N27). The median bound (20 ms, half
the timer) has a margin of about 13 ms on the busy case.

### Through the harness (QEMU user-mode networking)

Host to guest, the harness's socket against the guest's port-8 service;
guest to host, the guest against the harness answering in two writes.
Milliseconds for the host side, microseconds for the guest's.

| Tree | Arch | Boot | host -> guest, Nagle | host -> guest, `TCP_NODELAY` | guest -> host, host Nagle | guest -> host, host `TCP_NODELAY` |
|---|---|---|---|---|---|---|
| before `b76e4536` | x86-64 | probe `--old` | 0.5/0.8/0.9/1.1 | 0.5/0.5/0.6/1.1 | 936/1413/2213/3524 | 554/686/726/744 |
| before `b76e4536` | AArch64 | probe `--old` | 0.4/0.5/0.7/0.9 | 0.3/0.5/0.5/1.2 | 793/1178/1616/2418 | 378/512/606/696 |
| this branch | x86-64 | probe | 0.4/0.5/0.5/1.0 | 0.4/0.5/0.5/1.0 | 543/883/1206/1352 | 484/551/581/644 |
| this branch | AArch64 | probe | 0.4/0.6/0.7/1.1 | 0.4/0.5/0.6/1.0 | 722/1030/1704/2304 | 499/605/684/750 |
| this branch | x86-64 | 4 CPUs | 0.4/0.5/0.7/1.0 | 0.3/0.5/0.5/1.1 | 537/726/921/1063 | 470/550/591/639 |
| this branch | AArch64 | 4 CPUs | 0.5/0.7/0.7/1.1 | 0.5/0.6/0.7/1.2 | 788/1191/1935/2682 | 540/663/740/2450 |
| this branch | x86-64 | 2 CPUs | 0.4/0.4/0.6/1.1 | 0.4/0.5/0.6/1.3 | 599/1058/1581/5954 | 540/647/739/1483 |
| this branch | AArch64 | 2 CPUs | 0.4/0.6/0.7/1.2 | 0.6/0.6/0.6/1.4 | 572/675/763/908 | 588/718/768/802 |
| this branch | x86-64 | 1 CPU | 0.6/0.7/0.8/1.3 | 0.6/0.6/0.7/1.8 | 831/992/1646/3748 | 683/833/1114/3860 |
| this branch | AArch64 | 1 CPU | 0.5/0.6/1.0/**30.7** | 0.4/0.5/0.5/1.6 | 558/747/954/1192 | 440/598/618/661 |
| this branch | x86-64 | chaos | 0.5/0.5/0.6/1.1 | 0.5/0.6/0.6/1.5 | 738/1225/2066/2536 | 574/711/767/1367 |
| this branch | AArch64 | chaos | 0.5/0.7/0.8/1.2 | 0.4/0.5/0.6/1.0 | 828/1126/1423/1750 | 683/804/862/912 |

The two trees read alike in every column, as §"What can measure it"
predicts: nothing on this path waits for the guest's acknowledgement.
The one Nagle-shaped reading on the macOS host is in the guest-to-host
column, where the host's second write waits for the host kernel's own
acknowledgement of its first on the loopback leg: with Nagle on, the
median is 0.2-0.5 ms higher than with `TCP_NODELAY` and the tail longer
(macOS acknowledges the loopback segment quickly; one round of fifty in
one boot took 30.7 ms, the single stall in 1,200 Nagle rounds, and a
host-side one).

**The Linux host shows the stall in full.** The same branch on the CI
runner (Debian trixie container, PR #323, run 37617295102; every x86-64
debug boot alike):

| Shape | Linux host (CI) |
|---|---|
| host -> guest, Nagle | min 1.0 ms, **p50 41.0 ms**, p90 41.1 ms, max 41.4 ms |
| host -> guest, `TCP_NODELAY` | 0.3 / 0.4 / 0.5 / 1.5 ms |
| guest -> host, host Nagle | min 712 us, **p50 41181 us**, p90 41380, max 42270 |
| guest -> host, host `TCP_NODELAY` | 439 / 528 / 608 / 699 us |
| the same four, AArch64 job | Nagle 41.0-41.2 ms medians both ways; `TCP_NODELAY` 0.8-1.0 ms host -> guest |
| loopback Nagle peer, server reading at once (ack / exchange medians) | 324 / 701 us |
| loopback Nagle peer, server busy 5 ms (ack / exchange medians) | 7640 / 7992 us |

The 41 ms is Linux's: the first rounds of a connection are acknowledged
at once (quick-ACK mode, 1 ms), then the connection is in pingpong mode
and the harness's -- or QEMU's -- kernel socket withholds the
acknowledgement of a lone small segment for its delayed-ACK timer
(`TCP_DELACK_MIN`, 40 ms), and the Nagle sender on the other side of the
loopback waits for it. It appears in both directions because both have a
Linux sender with Nagle on talking to a Linux receiver over loopback, and
it is identical on every boot of the branch because the guest is not on
that path. The guest's own figures on the runner (the loopback peer: 0.3
ms acknowledged by a reading server, 7.6 ms by a busy one, no 40 ms
anywhere) are the measurement this unit is about, and they agree with
the macOS ones.


## Linux, for comparison

Linux's receiver acknowledges at once in three cases that matter here
(`net/ipv4/tcp_input.c`, `__tcp_ack_snd_check`): more than one full
segment is unacknowledged; quick-ACK mode, which a connection enters for
its first segments (`tcp_incr_quickack`, up to 16) and leaves once it is
"pingpong" -- interactive, the socket having sent data within an ACK
timeout of receiving some; and out-of-order data. Otherwise it arms the
delayed-ACK timer (`TCP_DELACK_MIN`, 40 ms, adapting downward with the
measured inter-arrival time). On a read, `tcp_cleanup_rbuf` sends the
owed acknowledgement only when the receive queue was emptied, a small
pushed segment was queued, *and* the connection is not in pingpong mode
(or two pushed segments were). A request-response server is in pingpong
mode by definition, so against a Nagle client doing write-write-read
Linux withholds the acknowledgement on the read, expecting the reply to
carry it -- and the reply cannot be written until the second half arrives.
That is the classic stall, and it is why the usual advice is
`TCP_NODELAY` on the client.

This stack's rule is simpler and, for this shape, stronger: the read
always sends the owed acknowledgement (N27). The cost is one pure
acknowledgement per request in interactive traffic where Linux would
have let the reply carry it (the reply then carries nothing new). The
two remedies the question named were considered against the
measurements: quick-ACK at connection start would help only a peer whose
first request arrives before the application reads, and only for the
first segments; ACK-on-PSH when nothing is queued to send would
acknowledge every small segment at once, which is the rule before
`b76e4536` -- the one that never let the timer fire. Neither addresses a
stall the measurements show, so neither was added.

## Validation

On the branch head, one chain, one QEMU at a time at the default
priority (no zsh `&`; `pri 31 nice 0`), 434 self-tests in every debug
boot:

| Step | x86-64 | AArch64 |
|---|---|---|
| `make host-test` | PASS | PASS |
| `make analyze` | clean | clean |
| debug boot, 4 CPUs | PASS 135.6 s | PASS 145.1 s |
| debug boot, 2 CPUs | PASS 135.6 s | PASS 142.4 s |
| debug boot, 1 CPU | PASS 134.0 s | PASS 125.4 s |
| `make test-smp2` | PASS 144.9 s | PASS 134.8 s |
| `make test-chaos` | PASS 142.7 s | PASS 144.5 s |
| release build and boot | PASS 16.5 s | PASS 19.8 s |
| `tools/delack-nagle-probe.py` (this branch) | PASS 136.4 s | PASS 139.2 s |
| `tools/delack-nagle-probe.py --old` | FAIL: `net-tcp-delack` alone, as required; everything else passed (145.7 s) | FAIL: `net-tcp-delack` alone, as required (135.2 s) |

Two earlier probe boots were discarded, and the probe corrected, before
the figures above were taken: an `--old` that reverted two of the
commit's six hunks by hand left the handshake's acknowledgement with no
branch to send it, stalled every connection and timed out the boot
(looking, until read, like "the old rule is slow"); and the probe set the
architecture in the environment after importing the harness, which
reads it at import, so its AArch64 boots were judged by x86-64's
markers. The `--old` tree is now the commit's whole `tcp.c` change
reverse-applied with `git apply -R`, and the environment is set first.

No x86-64 boot approached the 180 s budget locally (the longest, the
two-CPU `test-smp2`, 144.9 s). CI (PR #323, run 37617295102): the x86-64
job's debug boots took 119.6-124.2 s against the 180 s budget (release
15.5 s, the panic-path boot 103.6 s); the AArch64 job's 176.1-185.8 s
against its 240 s (release 21.3 s), a few seconds above the previous
unit's 166.7-178.9 s, which is what two hundred write-write-read rounds
and the reverse exchange cost a boot on that runner. No timeout; the flakes record is unchanged. The Linux
runner's Nagle-batch figures are in §"The measurements".


## Plan

Plan §7 carries the measurement as closed; inventory §7.5 and the history
carry the unit. The open TCP items (window scaling, SACK, timestamps,
ECN, Nagle itself) are unchanged.
