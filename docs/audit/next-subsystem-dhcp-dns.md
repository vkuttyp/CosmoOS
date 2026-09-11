# NEXT SUBSYSTEM — autoconfiguring the guest: DHCP and a DNS proxy

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it.

> **Status: implemented (PR #95).** This report was the plan; it was built
> as described and is now in `kernel-services/network/tapsvc.c`. The design
> below is retained as the rationale of record; the built system is
> documented in `docs/kernel-services/network/design.md` ("Autoconfiguring
> the guest") and its tests in that directory's `testing.md`. Where the two
> differ, the design doc is authoritative.

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
(`kernel-services/network/tapsvc.c`), started when `tap0` is activated (the
VM attaches, alongside forwarding and masquerade). It lives in the kernel,
not a userland daemon, for three reasons: the tap and its addressing are
already in-kernel (the service's whole configuration — the guest's address,
the gateway, the lease subnet — *is* `tap0`'s config, derived, not chosen);
the guest's DHCP and DNS packets land in the host stack, where the service
receives them with no new plumbing; and it is directly testable by injecting
frames on a tap and reading the answers back, the same harness shape as
`net-nat`. The policy it carries is minimal and bounded (one guest, one
address, one upstream), so the usual "policy belongs in userland" argument is
weak here; a general DHCP server with pools and reservations would be a
userland daemon, and this report says so in §Alternatives.

**The two halves reach the guest by different paths, and the difference is
the crux of the design.** DHCP happens before the guest has an address, and
its replies are the *limited* broadcast `255.255.255.255`; a routed UDP
socket cannot carry that to the tap — `ipv4_route` sends the limited
broadcast to `netif_default`, and `tap0` is explicitly `NETIF_NODEFAULT`, so
a socketed reply would leave the *physical NIC*, never the guest (and a
wildcard `:67` socket would also *receive* broadcasts from every interface,
not only the tap). So **DHCP is handled at the frame level, scoped to
`tap0`** (§2): the tap hands the service each inbound frame before the stack
sees it, and the service replies by building a frame and transmitting it out
`tap0` — addressed as the client's broadcast flag dictates (§2), never
through IP routing or a wildcard socket, so it is pinned to the tap on both
ends by construction. DNS,
by contrast, happens *after* the guest is configured: its query is a unicast
to `10.0.3.1` (an address the host owns) and the answer is a unicast to
`10.0.3.15` (which `ipv4_route` sends out `tap0` by the connected-subnet
route the NAT unit added), so **DNS is an ordinary in-kernel `ksock` UDP
service** (§3) with no interface-scoping problem.

### 2. The DHCP server (RFC 2131), at the frame level on `tap0`

The tap gives the service each frame the guest injects, before the stack
handles it (a tap-local receive filter — the tap owns `tapsvc`, so it calls
into it directly, not a machine-wide hook); the service claims the ones that
are a UDP datagram to port 67 and lets everything else through unchanged. It
answers the one guest:

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
- **Reaching a client with no address, out `tap0`, addressed the way RFC
  2131 §4.1 requires.** The client does not own `10.0.3.15` when the OFFER
  or ACK is sent, so the reply's destination follows the client's broadcast
  flag, at *both* layers:
  - **Broadcast flag set** (a fresh client, e.g. Linux `dhclient`): the reply
    is the *limited broadcast* — IP destination `255.255.255.255`, UDP port
    68, Ethernet destination the link broadcast `ff:ff:ff:ff:ff:ff`. Using
    `10.0.3.15` as the IP destination here would let the guest's IPv4 input
    drop the datagram before its DHCP client sees it (it owns no such
    address yet), which is the failure this rule exists to avoid.
  - **Broadcast flag clear:** IP destination `yiaddr` (`10.0.3.15`), UDP
    port 68, Ethernet destination the client's hardware address (`chaddr`) —
    a link-unicast to a client that has told us it will accept a datagram for
    an address it does not yet own.

  Either way the service builds the whole reply frame (source `10.0.3.1:67`)
  and transmits it out `tap0` with `ether_output` — an interface-scoped send,
  never `ipv4_output`, so it never consults the route table and never leaks
  to the NIC. Because ingress is the tap's own filter and egress is an
  explicit `ether_output(tap0, …)`, both directions are scoped to the tap by
  construction; no wildcard socket and no routed broadcast is involved, so
  `NETIF_NODEFAULT` and the missing interface scope on sockets are not in the
  path.

### 3. The DNS proxy: a UDP relay with one transaction model

Bound to `10.0.3.1:53` (an in-kernel `ksock` UDP socket), it forwards the
guest's queries to a real upstream resolver on a single host socket and
relays the answers back. The one thing it rewrites is the transaction ID;
everything else is relayed byte for byte.

- **One transaction model, stated once.** A guest query arriving on
  `10.0.3.1:53` is entered in a pending table as (guest source address,
  guest source port, the guest's 16-bit query ID), and the proxy **allocates
  a new 16-bit ID unique among the outstanding entries** and forwards the
  query — its ID field replaced by that allocated ID, every other byte
  unchanged — to the upstream from the proxy's single upstream socket. When
  an answer arrives on that socket, its ID is the key: the proxy finds the
  entry, **restores the guest's original ID**, and sends the answer (again
  byte-for-byte apart from the ID) to the recorded guest address and port.
  So the ID is rewritten going out and restored coming back; the payload
  (names, questions, records) is never parsed, so `A`, `AAAA`, `TXT`,
  anything all pass through. The query leaves as the *host's* own traffic
  (source: the host's egress interface), so it needs no NAT and reaches
  whatever the host can reach.
- **Collisions, defined.** Because the proxy owns the upstream ID space, two
  guest queries that happen to share an ID (different source ports, or an ID
  reused before the first answered) get *distinct* allocated IDs, so their
  answers are never ambiguous. When no ID is free (the table is full) the new
  query is dropped, not forwarded — the same bound as everywhere else.
- **A bounded, expiring pending table.** Entries expire (a query with no
  answer within a short timeout is reclaimed, its ID freed), and a full table
  drops new queries — the same "bounded, expiring, drops-when-full"
  discipline as the NAT conntrack table, for the same reason: guest-driven
  state must not grow without limit. The reader supplies nothing the proxy
  trusts: an upstream answer whose ID matches no live entry is dropped.
- **The upstream.** Configured through `fw_cfg` `opt/cosmo/resolver` (an
  IPv4 address); the harness points it at a test-controlled responder (a
  loopback socket), the `QEMU_MEM=2G` demo at a real resolver the host can
  reach. Absent configuration, the proxy answers `SERVFAIL` rather than
  guessing an upstream.
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
  network). It injects a `DHCPDISCOVER` on the tap with the broadcast flag set; the
  reply read back off the tap is a `DHCPOFFER` sent as the limited broadcast
  at both layers (IP `255.255.255.255`, Ethernet `ff:ff:ff:ff:ff:ff`), never
  routed off another interface, carrying `10.0.3.15`, mask `/24`, router and
  DNS `10.0.3.1`, and a lease. It injects a `DHCPREQUEST` for that
  address and reads back a `DHCPACK`; a `REQUEST` for a different address
  reads back a `DHCPNAK`. Then, configured, it sends a DNS query to
  `10.0.3.1:53`; the proxy forwards it (with a rewritten ID) to a test
  upstream (a loopback responder the test controls) and the answer read back
  off the tap carries the guest's *original* ID and the upstream's records.
  Two queries sharing an ID get distinct upstream IDs and unambiguous
  answers; the pending table is bounded (a flood of unanswered queries drops
  rather than grows) and its entries expire.
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
  — the frame-level DHCP responder, the DNS-proxy service thread and its
  sockets, the pending table.
- `kernel-services/network/tap.c` / `tap.h` — start the service on
  `tap_dev_activate` and stop it on teardown; the tap-local receive filter
  that hands the service each inbound frame before the stack sees it, and the
  `ether_output`-based reply path out the tap.
- `kernel-services/network/nettest.c`, `kernel/core/selftest.c`,
  `selftest.h` — the `net-dhcp` and `net-dns` self-tests.
- `kernel/core/fwcfg.c` / `kernel/fwcfg.h` (or the existing fw_cfg reader)
  — the `opt/cosmo/resolver` upstream address.
- `docs/kernel-services/network/`, `docs/kernel-services/virtualization/`,
  `README.md` — the design and the Status entry.

## New APIs

No new system call and no new control-plane ABI: the DHCP half rides a
tap-local receive filter and `ether_output`, the DNS half binds an in-kernel
`ksock` socket and reads one read-only `fw_cfg` value. The surface is
internal — `tapsvc_start(struct netif *tap)` / `tapsvc_stop()`, called from
the tap's activation and teardown, plus the tap-local receive-filter hook the
tap already needs for this.

## Migration plan

1. **The tap-local receive filter**: `tap.c` hands the service each inbound
   frame before `netif_rx`, and an `ether_output`-based reply path sends a
   frame back out the tap to a given hardware address. A test proves a frame
   the service emits is read back off the tap and a frame it does not claim
   still reaches the stack.
2. **The DHCP server** (frame-level): DISCOVER/OFFER/REQUEST/ACK/NAK/RELEASE
   for the one guest slot, replies built and sent out `tap0` to the
   destination the client's broadcast flag selects (§2: the limited broadcast
   at both layers when set, a link-unicast to `chaddr` with IP `yiaddr` when
   clear). Proved by `net-dhcp` (a synthetic guest completes DORA and the
   offer carries the right fields; a REQUEST for a wrong address is NAK'd; a
   second hardware address is refused). Each behavior bug-proved by
   reintroducing its bug.
3. **The DNS proxy**: the socket on `10.0.3.1:53`, the single upstream
   socket, the ID-rewriting relay, the bounded expiring pending table. Proved
   by `net-dns` (a guest query is relayed with a rewritten ID and its answer
   returned with the guest's original ID and the upstream's records; two
   queries sharing an ID stay unambiguous; an unconfigured upstream yields
   SERVFAIL; the table is bounded and expires). Each bound bug-proved.
4. **Enabling on the tap** and **the Linux demonstration**, documented and
   reproducible under `QEMU_MEM=2G`.
5. **Docs and the Status entry**, and the full verification chain.

## Tests

- `net-dhcp` (host): a synthetic guest injects DISCOVER on a tap with the
  broadcast flag set; the reply read back off the tap is an OFFER sent as the
  limited broadcast at both layers (IP `255.255.255.255`, Ethernet
  `ff:ff:ff:ff:ff:ff`) carrying `10.0.3.15`, `/24`, router/DNS `10.0.3.1`, a
  lease; REQUEST → ACK; a REQUEST for another address → NAK; a second
  hardware address is offered nothing. A separate DISCOVER with the flag
  *clear* is answered by a link-unicast to `chaddr` with IP `yiaddr`. The
  reply leaves only the tap (bug-proofs: an IP destination of `10.0.3.15`
  under the flag, which the guest would drop; and routing the reply instead
  of `ether_output`, which sends it off the default
  interface so the tap read-back is empty).
- `net-dns` (host): a guest query to `10.0.3.1:53` is forwarded to a
  test-controlled upstream (a loopback responder) with a rewritten ID, and
  the answer read back off the tap carries the guest's *original* ID and the
  upstream's records; two queries sharing an ID get distinct upstream IDs and
  unambiguous answers; an unconfigured upstream yields SERVFAIL; the pending
  table drops when full and reclaims on expiry.
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
- **Untrusted packets parsed by a new service.** The DHCP responder parses a
  guest-crafted BOOTP/DHCP frame and the DNS proxy reads a guest-crafted
  query header (the ID, and enough to route the answer); both validate
  lengths and treat the input as hostile, and the DNS proxy rewrites only the
  ID rather than interpreting names, so its parse surface is a fixed-size
  header, not the whole message.
- **Reaching a client with no address, and only it, addressed correctly.** A
  DHCP reply must reach a client that owns no address yet, over the tap and
  not the physical NIC. Two things have to be right: the *interface* (built
  as a frame and sent with `ether_output` out `tap0`, never IP-routed —
  `tap0` is `NETIF_NODEFAULT`, so a routed reply would leave the wrong
  interface) and the *destination* (RFC 2131 §4.1: with the broadcast flag
  set, the limited broadcast at both IP and Ethernet layers; clear, a
  link-unicast to `chaddr` with IP `yiaddr`). Getting the IP destination
  wrong under the flag — using `yiaddr` — lets the guest drop the reply
  before its DHCP client sees it. The test asserts both.
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
