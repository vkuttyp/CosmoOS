# NEXT SUBSYSTEM — autoconfiguring the guest: DHCP and a DNS proxy

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This is that report, and
nothing in it is implemented.

**Subsystem: a DHCP server and a DNS proxy on the tap, so a guest that
boots with a stock network configuration (DHCP client on) gets its address,
gateway and resolver from the host and can resolve names — the guest needs
to know nothing about the host's network, and no `ip addr add` by hand.**

## Problem

The guest reaches the world but only if it is configured by hand. Forwarding
and masquerade (the previous unit) let a guest on `10.0.3.0/24` reach past
the host, but only after someone brings its interface up with a static
address, a static gateway, and a static resolver: `ip addr add
10.0.3.15/24`, `ip route add default via 10.0.3.1`, `echo nameserver … >
/etc/resolv.conf`. A stock Linux (or any off-the-shelf) guest does none of
that on its own — it boots a DHCP client and expects an answer, and it
resolves names through whatever resolver DHCP hands it. Until the host
answers DHCP, the guest sits with an unconfigured interface; until it can
resolve a name, "reaching the world" means reaching an IP address a human
typed, not `example.com`.

## Current implementation

**Nothing answers DHCP.** The guest's `DHCPDISCOVER` is a UDP broadcast to
`255.255.255.255:67`. It arrives on `tap0` in the host stack, finds no
socket bound to port 67, and is dropped (`udp_input`'s `rx_no_port`, silent
for broadcast — no ICMP). There is no address pool, no lease, no server.

**Nothing resolves names.** The stack has UDP and TCP and the in-kernel
`ksock_*` API, but no resolver and no DNS anything. A guest pointed at a
resolver it can reach (through the NAT) could resolve on its own, but it has
no way to *learn* a resolver's address, and the host offers none.

**The pieces that exist and are reused:** the tap and its `10.0.3.0/24`
configuration (`tap.c`, brought up with forwarding and masquerade when the
VM attaches); the in-kernel socket API (`ksock_create`/`bind`/`sendto`/
`recvfrom`, already used by the network self-tests to run servers on a
kernel thread); `udp_input`'s delivery to a bound pcb; `ether_output` and
the ARP the stack already does; the `fw_cfg` channel (`opt/cosmo/*`) the
NIC address and the fault-injection and the encryption key already ride.

## Why it matters

- **A guest that boots and works, unmodified.** The whole point of the tap
  and NAT arc was a stock guest reaching the world. A stock guest expects
  DHCP; giving it DHCP is the difference between "a machine on the network"
  and "a machine on the network once an operator configures it by hand."
- **Names, not addresses.** Reaching `93.184.216.34` is a demo; reaching
  `example.com` is networking. The resolver is the last piece the guest
  needs before it behaves like a normal host.
- **The gateway is the one thing the guest must know.** With DHCP, the guest
  learns its address, its route, and its resolver from one broadcast; with
  the DNS proxy on the gateway, the resolver it learns is just the gateway
  again. The guest is told exactly one address (`10.0.3.1`, learned, not
  configured) and everything else follows.

## Proposed design

### 1. Where it lives: an in-kernel service tied to the tap

The DHCP server and the DNS proxy are a small in-kernel service
(`kernel-services/network/tapsvc.c`) that binds `ksock` UDP sockets and runs
on a kernel thread, started when `tap0` is activated (the VM attaches,
alongside forwarding and masquerade). It lives in the kernel, not a userland
daemon, for three reasons: the tap and its addressing are already in-kernel
(the service's whole configuration — the guest's address, the gateway, the
lease subnet — *is* `tap0`'s config, derived, not chosen); the guest's DHCP
and DNS packets land in the host stack, where an in-kernel socket receives
them with no new plumbing; and it is directly testable by injecting frames
on a tap and reading the answers back, the same harness shape as `net-nat`.
The policy it carries is minimal and bounded (one guest, one address, one
upstream), so the usual "policy belongs in userland" argument is weak here;
a general DHCP server with pools and reservations would be a userland daemon,
and this report says so in §Alternatives.

### 2. The DHCP server (RFC 2131)

Bound to `0.0.0.0:67`, it answers the one guest on `tap0`:

- **DISCOVER → OFFER, REQUEST → ACK.** The offered address is the tap's
  single guest slot (`10.0.3.15`, the address the tap unit already
  documents), with subnet mask `255.255.255.0`, router and DNS server both
  `10.0.3.1` (the tap's host address — the gateway *is* the resolver, §3),
  and a finite lease. A `REQUEST` for that address is ACK'd; a `REQUEST` for
  any other is NAK'd. `DECLINE`/`RELEASE` free the binding.
- **One binding.** The tap serves one guest, so the "pool" is one address
  keyed by the client's hardware address (chaddr); a second client is
  offered nothing (logged), not a second address. A per-client pool is the
  concern of the userland-daemon unit, not this one.
- **Replying to a client with no address.** The client cannot yet receive a
  unicast to `yiaddr`, so the reply is broadcast (or unicast to the
  hardware address per the broadcast flag, RFC 2131 §4.1) out `tap0` —
  built and sent through the bound socket to `255.255.255.255:68`, the stack
  broadcasting it out the tap. (Delivering a broadcast `DHCPDISCOVER` *to*
  the server also requires `udp_input` to hand a broadcast datagram to a
  wildcard-bound socket; if it does not today, that is a one-line fix with
  its own check.)

### 3. The DNS proxy (a UDP relay)

Bound to `10.0.3.1:53`, it forwards the guest's queries to a real upstream
resolver and relays the answers:

- **A byte relay, not a resolver.** A guest query arriving on `10.0.3.1:53`
  is forwarded verbatim from a host socket to the upstream resolver; the
  answer that comes back is relayed verbatim to the guest. The proxy does
  not parse names or record types, so `A`, `AAAA`, `TXT`, anything, all
  work — and the query leaves as the *host's* own traffic (source: the
  host's egress interface), so it needs no NAT and reaches whatever the host
  can reach.
- **The upstream.** Configured through `fw_cfg` `opt/cosmo/resolver` (an
  IPv4 address); the harness points it at a test-controlled responder (a
  loopback socket), the `QEMU_MEM=2G` demo at a real resolver the host can
  reach. Absent configuration, the proxy answers `SERVFAIL` rather than
  guessing an upstream.
- **A bounded pending table.** Each forwarded query records (guest address,
  guest port, guest txid) → (upstream txid) so the answer routes back to the
  right guest socket; entries are bounded and expire (a query with no answer
  is dropped, not remembered forever), a full table drops new queries. This
  is the same "bounded, expiring, drops-when-full" discipline as the NAT
  conntrack table, for the same reason: guest-driven state must not grow
  without limit.
- **UDP only, ≤512 bytes for now.** TCP DNS and EDNS0 large responses
  (truncation/`TC` handling, fallback to TCP) are a later refinement; the
  proxy sets nothing it cannot honor.

### 4. Enabling it

`tap0` starts the service the moment the owner first uses `/dev/net/tap` —
the same "a VM has attached" signal that turns on forwarding and masquerade
(`tap_dev_activate`). No new system call and no writable control surface:
the DHCP parameters are the tap's own configuration, and the upstream
resolver is a read-only `fw_cfg` value. The service is torn down when the
tap goes away.

### 5. The milestone

- **Gated, in the harness:** a synthetic guest on a tap (no external
  network). It broadcasts a `DHCPDISCOVER`; the host answers a `DHCPOFFER`
  carrying `10.0.3.15`, mask `/24`, router and DNS `10.0.3.1`, and a lease.
  It `DHCPREQUEST`s that address and gets a `DHCPACK`; a `REQUEST` for a
  different address gets a `DHCPNAK`. It sends a DNS query for a name to
  `10.0.3.1:53`; the proxy forwards it to a test upstream (a loopback
  responder the test controls) and relays the response back, the answer
  matching what the upstream returned. The pending table is bounded (a flood
  of unanswered queries drops rather than grows) and its entries expire.
- **Demonstrated, reproducible:** a stock Linux guest booted with its DHCP
  client on and the tap as its only network — it autoconfigures `eth0`
  (address, default route, resolver) from the host and resolves a name,
  under `QEMU_MEM=2G`, the same reproduction shape as the tap and NAT units.

### 6. Deliberately out of scope

- **A general DHCP server** (pools, reservations, multiple clients, options
  beyond the handful a guest needs) — a userland daemon's job (§Alternatives).
- **A caching or recursive resolver** — the proxy relays; it does not cache
  answers or walk the DNS tree. Caching is a later refinement if it earns
  its keep.
- **DHCPv6 and IPv6 RA/DNS**, **DNS over TCP/EDNS0 large answers**, and
  **DNSSEC validation** — named, later.

## Affected files

- `kernel-services/network/tapsvc.c` (new), `kernel/include/kernel/net/tapsvc.h`
  — the DHCP server, the DNS proxy, the service thread and its sockets.
- `kernel-services/network/tap.c` — start the service on `tap_dev_activate`,
  stop it on teardown.
- `kernel-services/network/udp.c` — deliver a broadcast UDP datagram to a
  wildcard-bound socket if it does not already (with its own check).
- `kernel-services/network/nettest.c`, `kernel/core/selftest.c`,
  `selftest.h` — the `net-dhcp` and `net-dns` self-tests.
- `kernel/core/fwcfg.c` / `kernel/fwcfg.h` (or the existing fw_cfg reader)
  — the `opt/cosmo/resolver` upstream address.
- `docs/kernel-services/network/`, `docs/kernel-services/virtualization/`,
  `README.md` — the design and the Status entry.

## New APIs

No new system call and no new control-plane ABI: the service binds in-kernel
sockets and reads one read-only `fw_cfg` value. Its surface is internal —
`tapsvc_start(struct netif *tap)` / `tapsvc_stop()`, called from the tap's
activation and teardown.

## Migration plan

1. **Broadcast UDP delivery**: ensure `udp_input` hands a broadcast datagram
   to a wildcard-bound socket, with a test (a socket bound to `0.0.0.0:port`
   receives a datagram sent to the subnet broadcast). The DHCP server needs
   this; it may already hold, in which case the test records that it does.
2. **The DHCP server**: the service thread, the socket on `:67`, DISCOVER/
   OFFER/REQUEST/ACK/NAK/RELEASE for the one guest slot, broadcast replies.
   Proved by `net-dhcp` (a synthetic guest completes DORA and is offered the
   right fields; a REQUEST for a wrong address is NAK'd; a second client is
   refused). Each behavior bug-proved by reintroducing its bug.
3. **The DNS proxy**: the socket on `10.0.3.1:53`, the byte relay to the
   `fw_cfg` upstream, the bounded expiring pending table. Proved by
   `net-dns` (a guest query is relayed to a test upstream and its answer
   relayed back verbatim; an unconfigured upstream yields SERVFAIL; the
   table is bounded and expires). Each bound bug-proved.
4. **Enabling on the tap** and **the Linux demonstration**, documented and
   reproducible under `QEMU_MEM=2G`.
5. **Docs and the Status entry**, and the full verification chain.

## Tests

- `net-dhcp` (host): a synthetic guest on a tap broadcasts DISCOVER and gets
  an OFFER with `10.0.3.15`, `/24`, router/DNS `10.0.3.1`, a lease; REQUEST
  → ACK; a REQUEST for another address → NAK; a second hardware address is
  offered nothing. Replies observed on the tap are broadcast (the client has
  no address yet).
- `net-dns` (host): a guest query to `10.0.3.1:53` is relayed to a
  test-controlled upstream (a loopback responder) and the response relayed
  back, matching the upstream's answer for its txid and question; an
  unconfigured upstream yields SERVFAIL; the pending table drops when full
  and reclaims on expiry.
- The existing net, forwarding and NAT tests and a net-less boot stay green;
  the service is off until the tap is activated.

## Benchmarks

DHCP round-trip latency and DNS relay added latency between the guest tap
and a loopback upstream, recorded so a later regression (caching, TCP DNS)
has a baseline. No absolute target.

## Risks

- **A guest that floods the server or the resolver.** The DHCP binding is
  one address keyed by hardware address (a second client gets nothing, not
  unbounded state); the DNS pending table is bounded and expiring and drops
  when full — the same discipline as the NAT conntrack table, for the same
  reason (guest-driven state must not grow without limit).
- **Untrusted packets parsed by a new service.** The DHCP server parses a
  guest-crafted BOOTP/DHCP packet and the DNS proxy reads a guest-crafted
  query header (txid, and enough to route the answer); both validate lengths
  and treat the input as hostile, and the DNS proxy relays bytes rather than
  interpreting names, so its parse surface is a fixed-size header, not the
  whole message.
- **Replying to a client with no address.** A DHCP reply must reach a client
  that cannot yet receive a unicast to the offered address; getting the
  broadcast/hardware-address path wrong means the guest never sees the
  offer. The test asserts the reply is observed on the tap as a broadcast.
- **Overselling reach.** The gated test resolves through a loopback upstream,
  not the internet; the unit says plainly that real-world resolution is the
  `QEMU_MEM=2G` reproduction, as the tap and NAT units said of theirs.
- **A resolver the guest is told to trust.** The proxy hands the guest the
  gateway as its resolver and forwards to an upstream the *host* chose
  (fw_cfg), never one the guest names — a guest cannot point the host at an
  arbitrary upstream.

## Alternatives considered

- **A userland DHCP/DNS daemon.** A separate process binding `:67`/`:53`
  through the socket syscalls, with the address pool and upstream in a config
  file — the right shape for a *general* server with pools, reservations and
  many clients. Rejected for this unit: the tap serves one guest, the
  configuration is `tap0`'s own, and an in-kernel service is directly
  driveable by the frame-injection harness that already tests the tap and
  NAT; a userland daemon would need a second machine's worth of test
  scaffolding to prove. The general daemon is a later unit if the host ever
  serves more than one guest.
- **DHCP option 6 points the guest straight at a real upstream (no proxy).**
  Simpler — DHCP hands the guest a real resolver reachable through the NAT,
  and the guest resolves directly. Rejected as the primary design because it
  makes the guest depend on knowing a real upstream and on the NAT for every
  query, and forecloses the host mediating (local names, a future cache);
  the proxy costs a small relay and keeps "the gateway is the one address
  the guest must know" true. (The proxy can still be configured to point at
  any upstream, so the simple case is a subset.)
- **A full recursive/caching resolver in the host.** Far larger than "let
  the guest resolve": root hints, cache eviction, DNSSEC. This unit relays;
  a resolver is its own unit if the host ever needs to answer without an
  upstream.
- **Static configuration only (no DHCP).** What exists today — document the
  `ip addr`/`ip route`/`resolv.conf` a guest needs. Rejected: the point of
  the arc is a *stock* guest, and a stock guest speaks DHCP.
