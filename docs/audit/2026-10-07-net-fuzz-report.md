# Network packet-parser fuzzing

Date: 2026-10-07. Branch `net-fuzz` from `main` at `abf63098` (the
epoll-callback merge, PR #325). Plan §12, "missing fuzz/property coverage":
the network packet parsers, named as a gap in the network testing notes
since the stack was written ("no host tests yet: the checksum and header
parsers should get a bit-flip fuzz loop") and in the inventory (§2.10).

## 1. The survey: what parses the wire, and what the host can build

Every function that reads bytes a peer chose, and whether it compiles on
the host with the shims this unit adds (`tests/fuzz/shim_net.c`):

| Layer | Functions | Host build |
|---|---|---|
| Ethernet | `ether_input` (`ether.c`) | yes, unchanged |
| ARP | `arp_input`, the table (`arp.c`) | yes, unchanged |
| IPv4 | `ipv4_input` (header, options length, fragments refused), `ipv4_forward`, `icmp_input`, `icmp_needfrag` and the quoted-flow reader, the path-MTU cache (`ipv4.c`) | yes, unchanged |
| IPv6 | `ipv6_input`, `icmpv6_input`, `nd_input_ns`/`nd_input_na` and their one option (`ipv6.c`); extension headers are not parsed (the datagram is dropped, `rx_unknown_proto`) | yes, unchanged |
| UDP | `udp_input` (`udp.c`) | yes, unchanged |
| TCP | `tcp_input` (header, options via `parse_mss`, the SYN cache and cookies, every state), the timers' work (`tcp.c`) | yes, unchanged; `TCP_HOST_TEST` adds a state-change hook |
| NAT | `nat_out`, `nat_in` and the ICMP-error quote reader (`nat.c`) | yes, unchanged |
| Firewall | the four verdicts and `l4_read` (`fw.c`) | yes, unchanged |
| DHCP server | `dhcp_filter`, `dhcp_opt`, `dhcp_reply` (`tapsvc.c`) | yes; `TAPSVC_HOST_TEST` adds doors to the filter and the proxy's two loop bodies, which were refactored into `dns_guest_one`/`dns_up_one` |
| DNS proxy | the id table (`dns_alloc`, `dns_take`), `dns_servfail`, the resolver address parser `dns_parse_ip` (`tapsvc.c`) | yes, as above |
| tap | `tap_inject` (a frame from the far end), the tapctl command parser (`tap.c`) | no: the device-file layer (vnodes, handles, the readiness queue) |
| virtio-net | the receive descriptor and `virtio_net_hdr` (`drivers/virtio/virtio_net.c`) | no: shaped by the device model (the virtqueue is fuzzed on its own, `fuzz_virtq`) |
| e1000e | the receive descriptor ring (`drivers/net/e1000e.c`) | no: the same |
| netif | `netif_rx` and the steering hash `net_flow_hash` (`netif.c`) | not built: the worker threads, per-CPU queues and quiescence; the shim delivers a frame as the worker would, straight into `ether_input` |

The shim supplies what the protocol layers need from the rest of the
kernel, in a shape one thread controls: the interface registry
(`netif_default`, `netif_connected`, `netif_owns_ipv4`/`ipv6`,
`netif_loopback`, `netif_transmit` -- a fake interface captures every frame
and runs the target's observer), the worker's work queue (run when the
target says), timers over a clock the target advances (every timer due by
the new time fires in expiry order, with queued work run after each, as the
kernel's timer interrupt hands off to the network worker), `kmalloc` and the
slab caches over `malloc` with a live-object count (so ASan sees every
object and a leak is a count), plain-count kobjects, `random_u64` reseeded
per input (an input replays), `sock_wake`/`sock_set_error` as counters. The
network sources compile with the host test flags and no change but the two
hooks. One host-shim fix: `tests/host/shim/arch/cpu.h` included
`<stdlib.h>`, which on Darwin brings `htons` in as a macro and broke
`inet.h`'s inline in any file that included the shim first (`arp.c`,
`ipv6.c`); it declares `abort` itself now, and the targets include the
kernel's headers before the host's.

## 2. The targets

`tests/fuzz/netpkt.h` builds frames (Ethernet, IPv4 with its checksum, UDP
and TCP with the pseudo-header checksum, ARP, ICMP, IPv6, neighbour
discovery) for the seeds and the handshakes, and checks them: `np_fix_checksums`
recomputes a mutated frame's checksums so the input reaches past the gates,
`np_check_checksums` is the output oracle.

**`fuzz_net_frame`: Ethernet frames into the whole receive path.** The
topology of a booted machine: an uplink `fz0` (10.0.2.15/24, peer and
gateway 10.0.2.2) and a guest tap `fz1` (10.75.0.1/24, forwarding and
masquerading for the guest 10.75.0.15, the firewall's guest attached with
its seeded rules plus one TO_UPLINK drop, one TCP port forward). Before
each input: a UDP socket on :7, a UDP socket connected to the peer, a TCP
listener on :80, a connection the peer opened and the host accepted
(ESTABLISHED, through a real handshake read off the fake interface), and
a connection the host is opening to 10.0.2.3:9 (SYN_SENT, its SYN parked
on an incomplete ARP entry). The input is a sequence of records -- a
control byte, a 16-bit length, the frame -- and the control bits choose
the interface, recompute checksums, retarget a TCP frame to the
established or the half-open connection (addresses, ports, seq and ack at
what the connection expects, so a mutated segment is the connection's),
advance the clock 250 ms, have the host act (send, drain), shut its write
side or close. Up to 64 frames an input.

**`fuzz_tcp_segments`: a sequence of segments into one connection.** A
listener and an accepted connection as above; the input is a program of
operations over it: a segment relative to the connection's expectations
(flags, seq and ack offsets, window, options, payload), the host sends,
reads, shuts its write side, closes, time passes, a segment with its
checksum then corrupted, an ICMP fragmentation-needed quoting the host's
last segment, a raw segment, the next timer fires whatever the time. Up to
256 operations.

**`fuzz_dhcp_dns`: the tap services' two parsers.** A `tapsvc` instance
over a bare interface, no tap, sockets or threads (`tapsvc_test_new`); the
DHCP filter fed a frame as the far end would inject it, each side of the
DNS proxy fed one datagram as its thread would read it, time passing, the
resolver string parser, the upstream configured or not. Replies are
captured: a DHCP reply as the frame out of the interface, a DNS relay as
the datagram sent through the stubbed socket.

Every target owns its pcbs the way the socket layer does -- a `struct
socket` per pcb, attached at creation and at accept -- because a pcb's end
paths read the owner to decide between retiring the pcb under its live
socket and killing an ownerless one (a queued child nobody accepted). The
first draft passed `NULL` to `tcp_accept` and found a use-after-free that
was its own: the network killed the ownerless pcb, the target then closed
it, and `tcp_close` put it twice. The kernel never does this
(`ksock_accept` creates the socket before dequeuing, as its comment says),
so it is recorded here and not changed.

## 3. The oracles

Checked after every frame or operation and at the end of each input:

- every spinlock released (`harness_locks_held`, the host shim's held-lock
  stack: lock balance where the shim can see it);
- every frame the stack transmits carries correct checksums (IPv4 header;
  UDP, TCP and ICMP over IPv4; UDP, TCP and ICMPv6 over IPv6) and is well
  formed, at least 60 bytes and at most the MTU plus the Ethernet header
  -- except a frame the stack *forwards*, which is the guest's datagram
  relayed and is held to the IP header checksum alone (§5);
- the ARP table never exceeds `ARP_TABLE_SIZE`, the NAT and firewall flow
  tables never exceed theirs;
- TCP: `snd_una <= snd_nxt <= snd_max`, the buffers within their sizes, the
  out-of-order queue within its count; every state change is an arc of the
  documented machine (RFC 793 figure 6, plus this implementation's
  completion of a passive open from the SYN cache straight to ESTABLISHED
  and any state's abort to CLOSED), through `tcp_test_state_change`; a
  segment with a bad checksum changes nothing (`bad_cksum` counts it, the
  state and sequence variables stay, nothing is sent);
- DHCP: a reply is a well-formed BOOTP reply (op 2, the request's xid, the
  magic cookie, a message type that is OFFER, ACK or NAK, options ended)
  in a correct IPv4/UDP frame from 67 to 68, and only to a frame the
  filter claimed; DNS: a query with the upstream configured is relayed
  once, upstream, byte for byte but its id, and SERVFAIL'd without the
  upstream; an answer is relayed only from the configured upstream with a
  lent id not yet consumed, then with the guest's id restored to the guest
  that asked; live pending queries never exceed `DNS_PENDING_MAX` and go to
  zero as time passes.

After teardown (sockets closed, every timer run to quiescence, at most
fifteen minutes of simulated time): no mbuf alive, the allocator's live
count back at its baseline -- no pcb, SYN-cache entry or buffer leaked --
the neighbour tables empty once each interface is flushed (every entry
named a registered interface, N25), the NAT and firewall tables empty once
flushed.

## 4. Corpora

Programmatic seeds (19, 16 and 7): the peer's ARP request and a reply that
completes the half-open connection's entry, a ping, UDP to the echo socket,
a SYN to the listener, data, FIN and RST on the established connection, the
guest's UDP and TCP to the world (masqueraded), the world dialing the port
forward, a neighbour solicitation and an IPv6 ping, the guest to a dropped
port and to the host's own socket, fragmentation-needed quoting the
connection, a runt broadcast, a SYN-ACK completing the half-open
connection; for the segments target in-order data, a gap filled, the
peer's close, the host's close, resets in and out of window, a bad
checksum, options, a closed window and the probe, retransmissions to the
limit, fragmentation needed, simultaneous close, keepalive, a raw segment
with every flag, a shutdown; for DHCP and DNS the lease conversation, a NAK
and a second client, a query and its answer from the upstream and from a
stranger, SERVFAIL, expiry, resolver strings, a frame that is not DHCP, a
table filled.

A checked-in corpus from a real boot: `QEMU_PCAP` on an x86-64 debug boot
of this tree (11 015 frames), reduced by `tools/pcap-to-seeds.py` to the
frames the guest *received* (its MAC or broadcast; its own frames are
dropped as reflections) and one file per frame shape: 8 files, 32 KiB, in
`tests/fuzz/corpus/net_frame` -- the gateway's ARP request and reply, a
UDP datagram to port 7, and TCP segments with SYN, SYN-ACK, ACK, PSH-ACK
and FIN-ACK from QEMU's user-mode stack. The TCP ones carry the retarget
bit, so they land on the established connection at run time.

## 5. Runs and findings

TBD-RUNS

**Finding: a FIN that acknowledges the last segment in flight strands a
closed connection (fixed).** `fuzz_tcp_segments`, 16 inputs into a
50 000-mutation run: after teardown three objects stayed allocated -- a
pcb and its two 64 KiB buffers -- with one timer pending (keepalive, two
hours out) and the pcb in CLOSING, in the table, 1176 bytes unsent,
`fin_queued` and not `fin_sent`, nothing in flight. The program: the host
sends more than the peer's window takes, the host closes (FIN_WAIT_1, its
FIN queued behind the unsent data), the peer sends FIN with an ACK of
everything in flight. The FIN branch of `tcp_input` built a bare
acknowledgement and returned without running the output: the one path
that advances `snd_una` without the output, and once the host has closed
no later event runs it -- no send, no acknowledgement left to come, the
retransmit timer finding nothing in flight (its condition is "something in
flight, or a zero window with data waiting"; the window was open). The
data and the FIN stay queued, the pcb with its 128 KiB and its table slot
stays for ever (the keepalive fires once in CLOSING and does nothing), and
the peer waits for a FIN that never comes. Reachable from the wire by any
peer that closes while the host still has window-limited data to send --
a client that shuts its write side mid-response. The fix: the FIN branch
sets `ack_now` and runs `tcp_output_locked`, which sends what the window
now allows, the FIN behind it, the acknowledgement on the first segment
or alone, and arms the retransmit timer. `net-fin-acks-last-data` is the
regression test (a peer with a window of 20, 60 bytes sent, the host
closes, the peer's FIN acknowledges the 20: the 40 and the host's FIN
must follow); `tools/net-fuzz-probe.py --old fin-output` puts the old
branch back and the test fails at its stream check. The saved input
replays clean with the fix, and so does the run that found it.

**Recorded, not changed:**

- *The forwarder relays a guest's malformed transport header.* A UDP
  datagram from the guest whose length field disagrees with the IP length
  is masqueraded and forwarded as it came (the output oracle first flagged
  it as the stack's own malformed frame). A router does not police
  transport headers; Linux's conntrack marks such a datagram INVALID and
  skips NAT for it. Here it is translated and the NAT entry made. No
  memory is read by the length field (the translation is an incremental
  checksum update), and the private source cannot leak (it is rewritten),
  so the difference is a policy, recorded.
- *Forwarding re-stamps the IP identification and drops IP options.*
  `ipv4_forward` trims the original header and `output_on` builds a fresh
  one with the next `g_ip_id`. Fragments are refused on input anyway, so
  no reassembly depends on the id; options are not forwarded. Known shape,
  not a defect.
- *`tcp_close` on a pcb the network already ended and nobody owns puts it
  twice* (§2). Unreachable from the kernel's socket layer.
- *The keepalive timer fires in CLOSING and does nothing.* With the fix
  above a CLOSING pcb always has its FIN in flight and the retransmit
  timer ends it; the keepalive's silence in CLOSING is harmless.
- *IPv6 extension headers are not parsed* -- known since the IPv6 phase;
  the datagram is dropped and counted.

## 6. What could not be fuzzed on the host, and why

- The NIC receive descriptors (`virtio_net.c`, `e1000e.c`): their input is
  a device model's ring and header, not a wire format a peer controls; the
  split virtqueue has `fuzz_virtq`, and a device-model fuzzer would need
  the DMA and interrupt shims the drivers' host tests do not have.
- `tap.c`: the far end's frames come through a device file (vnode, handle,
  readiness queue); `tap_inject` is a copy into an mbuf and `netif_rx`.
  The tapctl command parser reads a string from a handle write.
- `netif.c`: the worker threads, per-CPU queues, steering hash and
  quiescence; the shim replaces it. `net_flow_hash` reads 96 bytes of a
  frame and could be host-built alone; it decides a queue, not a parse.
- `loopback.c`: no wire.
- Lock balance is checked where the shim sees it (the spinlock shim's
  held-lock stack); mutexes and the worker's wait queues are not in the
  host build.
- Concurrency: one thread. The races the self-tests force on the target
  (the park hooks) are out of a fuzzer's reach by construction.

## 7. Validation

TBD-VALIDATION
