# Network fuzzing: the oracles past the receive path

The net-fuzz report (`docs/audit/2026-10-07-net-fuzz-report.md`, §5) named
where coverage stopped: the IPv6 output path (no IPv6 socket), `udp.c`'s
send side, and the half of `fw.c` and `nat.c` that configuration drives.
This unit extends `fuzz_net_frame` over the first two, adds
`fuzz_net_config` for the third, and makes the one recorded `tcp_close`
hazard defensive.

## 1. What was added

**`fuzz_net_frame`.** The topology gains, on fz0's link-local address, a
UDP socket, one connected to the peer fe80::1, a TCP listener, and a
connection from the peer that has been accepted. The peer is learnt from
its neighbour solicitation. A record whose length has its top bit set is
a host action, not a frame:
- UDP sends from eight socket shapes (unconnected and connected, v4 and
  v6; fresh unbound ones of each family; a fresh one bound to a port in
  use, or to an address not ours) to sixteen destinations: resolved,
  unresolved, routed, both broadcasts, multicast, own, loopback,
  unroutable, unspecified.
- The transmit rings full: the shim's transmit frees the packet and
  returns `-ENOBUFS`, as virtio-net's does.
- Segments from the IPv6 peer at the sequence numbers the connection
  expects, and the host's acts on the IPv6 connection.
- Time passing, and neighbour advertisements and solicitations.

Ten new seeds.

New oracles:
- Every unicast IPv4 or IPv6 frame goes to the MAC that its interface's
  ARP or ND table holds, reachable, for the next hop (N25). ND's own NS
  and NA answers are excepted: they are addressed from the message they
  answer. `nd_lookup` is `arp_lookup`'s twin, added for this oracle.
- A UDP send returns `-ENOBUFS` exactly when the ring refused its
  datagram. A refused ARP request or NS is not the datagram.
- An IPv6 UDP checksum is never zero (RFC 8200 §8.1).

**`fuzz_net_config`** (new; in `make fuzz` and CI's bounded job). The
topology is an uplink and two masquerading guest taps. The operations:
- firewall rules added and deleted, for the host object, either guest,
  or a guest nobody attached; policies set; the firewall flushed;
- port forwards added, deleted and cleared;
- a guest released (its interface down, then the purges) and attached
  again; NAT flushed; time;
- host sends through the OUTPUT chain;
- frames built from templates: uplink to host, uplink to a forwarded
  port, guest to world, guest to host, guest to the other guest, and
  replies to translations the table holds.

The target keeps a model of what it configured. After every record:
- every listing equals the model, in order, and so do the policies and
  the attached guests;
- the tables stay within capacity;
- every return code is the one the model predicts (`-EEXIST` for a
  duplicate of an installed rule, `-ENOSPC` for a full list, `-ENOENT`
  for a missing owner, `-EINVAL` for a forward to an address on no
  forwarding tap);
- the OUTPUT fast path agrees with the host's rules and OUTPUT default;
- every DNAT translation belongs to a forward the model holds;
- nothing is a released guest's.

Every verdict is accounted for. The hook tallies the rules that decide,
by chain and verdict, and the firewall's per-chain by-rule counters must
agree with the tally after every record. Where a frame's template names
the chain and owner, a default decision must be the model's default. A
host send returns `-EPERM` exactly when the OUTPUT chain dropped it. (All
three were added in review.)

As a frame is decided, every rule that decides it must be installed, for
that owner, at that moment. This uses fw.c's `FW_HOST_TEST` hook, which
reports each deciding rule, so a removed rule never matches again. A
stateful flow that a rule accepted outlives the rule, as conntrack's do.
The flow, not the rule, passes such a frame.

**`fuzz_tcp_segments`.** Opcode 10 detaches the owner without closing,
which is the ownerless pcb `tcp_accept(listener, NULL)` makes. Seed 16
then has the peer reset it and the teardown close it.

**Wake references.** Since the census-wake-ref unit, the shim counts
`sock_wake_ref`/`sock_wake_unref`, and both stack targets assert that
none is held whenever the stack returns to them.

**Each oracle checked against a mutation of what it watches**, in a
clone, at 20 000 mutations or on the seeds:

| oracle | mutation | caught |
|---|---|---|
| neighbour (IPv6) | one bit of the resolved MAC flipped in `ipv6.c`'s output | seed, iteration 0 |
| neighbour (IPv4) | the same in `ipv4.c` | seed, iteration 0 |
| wake references | `sock_wake_after` puts with `ksock_put` | seed, iteration 0 |
| listing round-trip | `fw_rule_add` ignores its index | iteration 0 |
| fast path | `fw_rule_del` without `out_fast_update` | iteration 237 |
| deciding rule installed | the forward chain reads one past the list's end | iteration 1 |
| DNAT has a forward | `nat_pf_del` does not reap | iteration 1 |
| idempotent kill | the guard taken out | seed 16: ASan heap-use-after-free |
| verdict tally | the OUTPUT chain turns a rule's DROP into ACCEPT | at once |
| send result | the send path returns 0 for a dropped datagram | at once |
| fast path, policy | `fw_policy_set` without `out_fast_update` | at once |

## 2. Findings

**A port-forward clear kept the cleared rules' translations (fixed).**
`fuzz_net_config`, on its own seed: after `nat_pf_clear` the table still
held `tcp 8080 -> 10.75.0.15:80`, and the client's next segment reached
the guest through a port the clear had closed. `nat_pf_del` reaps its
one rule's entries; the clear reaped none. Only the self-tests and the
unused `nat_portforward_config` call it today. It now reaps every DNAT
entry under `g_pf_lock -> g_nat_lock`, as the delete does.
Regression test: `net-pf-clear`. `tools/net-fuzz-probe.py --old pf-clear`
puts the old clear back, and the test fails at "its translation went with
it".

**A tap's release purged its guest before the tap was gone (fixed).**
`fuzz_net_config`, at 50 inputs: a masquerade entry named a released
guest. The target delivered a frame on the guest's interface after the
release's purges. That is what `tap_chr_release` allowed:
1. It purged the guest's NAT and firewall state, then destroyed the tap.
2. A frame the guest wrote before its last close, still on a worker's
   queue, could be masqueraded in between.
3. Its translation outlived the tap, and the next tap given the subnet
   inherited it: replies meant for the old guest were translated to the
   new one for up to 300 s.

The tap now goes first. Once `tap_destroy` returns, no frame of the
interface is queued or in flight (`netif_unregister`'s barrier), so the
purge clears everything there will be. The fuzz model releases in the
fixed order: the interface goes down, then the purges.

Regression test: `net-tap-release-order`. A CONFIG_DEBUG seam just after
the purges offers the guest's frame through the tap's interface and waits
out the workers. A control shows the same frame, sent before the close,
does make state. `--old tap-release-order` puts the old order back, and
the test fails at `left == 0`.

**`tcp_close` on an ownerless pcb the network had ended put it twice
(made defensive).** This was recorded, not changed, in the net-fuzz
report. `pcb_kill_locked` now runs once; a second call hands its caller
nothing. The socket layer never makes such a pcb, so no kernel-suite test
can reach it; seed 16 above is the regression.

**Checked and not a defect.** After a release, the host's own send to the
former guest's address records a host flow: with the tap down, the
address routes out the uplink and is the world's. The oracle holds a
released guest to "no flow is its own", plus "none names it" at the
release itself.

## 3. Coverage

Lines of `kernel-services/network/*.c` executed by the seeds, the corpus
and 100 000 mutations (seed 1), measured by `tools/fuzz-coverage.py`
(clang source-based coverage, Apple M-series).

| file | `fuzz_net_frame` before | after | the four targets' union before | after |
|---|---|---|---|---|
| `ipv6.c` | 67.8 % | 83.8 % | 67.8 % | 83.8 % |
| `udp.c` | 49.0 % | 77.2 % | 49.0 % | 79.2 % |
| `fw.c` | 45.3 % | 65.4 % | 45.3 % | 91.7 % (`fuzz_net_config` alone) |
| `nat.c` | 59.5 % | 58.7 % | 59.5 % | 80.2 % |
| `ipv4.c` | 68.4 % | 71.3 % | 74.3 % | 79.3 % |
| `tcp.c` | 67.8 % | 70.3 % | 77.6 % | 79.3 % |
| `arp.c` | 76.8 % | 79.9 % | 78.2 % | 81.7 % |
| `mbuf.c` | 63.9 % | 66.5 % | 63.9 % | 66.5 % |
| `inet.c` | 19.3 % | 26.3 % | 19.3 % | 26.3 % |
| total | 63.6 % | 70.2 % | 68.2 % (three targets) | 78.7 % |

`fuzz_net_config` alone reaches `fw.c` 91.7 % and `nat.c` 74.4 %. The
union's `fw.c` comes from that target's binary: its `FW_HOST_TEST` build
changes `fw.c`'s functions, and `llvm-cov` keeps the first object's
mapping when the hashes differ, which put the merged figure at 82.7 %.
What the targets still miss: `inet.c`'s address formatting, the
allocation-failure branches, and the IPv6 forwarding the stack does not
have.

Runs: `fuzz_net_frame` 100 000 mutations with seeds 1, 2 and 3, and
`fuzz_net_config` with seeds 1, 2 and 3, all clean after the fixes. CI
runs each target with `FUZZ_RUNS=50000`.
