# NEXT SUBSYSTEM — reaching beyond the host: IP forwarding and masquerade NAT

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This is that report, and
nothing in it is implemented.

**Subsystem: connected-subnet routing, IP forwarding, and masquerade NAT
in the host stack, so a guest on the tap can reach past the host — through
the host's own interface — and the replies find their way back.**

## Problem

A guest reaches the host and nothing beyond it. The tap unit gave the
guest an IP link to the host's stack: it ARPs `10.0.3.1`, pings the host,
opens a connection to a service the host runs. But `10.0.3.15` cannot
reach `10.0.2.3` on the host's own network, let alone the world, because
the host does not forward: a packet whose destination is not one of the
host's own addresses is dropped (`ipv4_input`'s `rx_not_for_us`). The host
is an endpoint, not a router, and the guest is boxed into the two-machine
network the tap created.

The tap unit named this as its successor — "reaching beyond the host
(NAT, routing, DHCP, DNS)." This unit is the routing and NAT of that: the
part that lets the guest's packets leave, and their replies return. DHCP
(so the guest need not be told its address) and a DNS proxy (so it can
resolve names without a configured resolver) are conveniences on top and
are the unit after.

## Current implementation

**The host does not forward.** `ipv4_input` delivers a packet addressed to
one of the host's own IPs or a broadcast, and drops everything else with
`rx_not_for_us`. There is no path that takes a packet arriving on one
interface and sends it out another.

**Routing is default-only.** `ipv4_route(dst)` returns the loopback for
`127/8` and the host's own addresses, and otherwise `netif_default()` --
the one non-loopback interface that is up. It has no notion of a
*connected route*: it cannot say "`10.0.3.15` is on `tap0`'s subnet, send
it there" or "`10.0.2.0/24` is on the NIC." So even with forwarding, a
reply bound for the guest would be routed to the default interface, not
the tap.

**There is no NAT.** Nothing rewrites addresses or ports, and there is no
connection-tracking table. A guest packet forwarded out the host's NIC
with its `10.0.3.15` source would be answered to `10.0.3.15` -- an address
that means nothing on the far network -- and the reply would never come
back. Masquerade is what makes a private guest reachable through the
host's single public address.

**The pieces that do exist and are reused:** `ipv4_output`/`output_on`
(route and emit a datagram), the ICMP error path (for time-exceeded), the
tap and its interfaces, and the transport checksums the stack already
computes.

## Why it matters

- **A guest that reaches the world.** With forwarding and masquerade, a
  guest configured with the tap as its gateway reaches whatever the host
  can -- the host's LAN, and the internet if the host has it. It is the
  step from "a machine on a two-node network with the host" to "a machine
  on the network."
- **The host becomes a router, once.** Forwarding and NAT are general --
  between any two interfaces, for any private client, not only a VM. A
  container, a second tap, a future bridge all reach out the same way.
  Building it for the guest builds the host's routing.
- **The foundation DHCP and DNS sit on.** Autoconfiguring the guest and
  resolving its names are worth little until its packets can leave; this
  unit is what they complete.

## Proposed design

### 1. Connected-subnet routing

`ipv4_route(dst)` learns a middle case between "our own / loopback" and
"the default interface": a **connected route** -- the interface on whose
subnet `dst` falls (so `10.0.3.15` routes to `tap0`, `10.0.2.x` to the
NIC), before falling back to the default for everything else. Because the
interface registry permits overlapping masks and iterates in registration
order, selection is **longest-prefix**: among the interfaces whose
address and mask contain `dst`, the one with the longest mask wins, not
whichever is encountered first, so a broad subnet registered early cannot
capture traffic meant for a more-specific one. This is what lets a reply
bound for the guest reach the tap, and a guest packet for the host's LAN
reach the NIC, rather than both going to the default. Loopback and
owned-address handling are unchanged.

### 2. IP forwarding

Where `ipv4_input` today drops a packet that is not for the host, it
instead **forwards** it -- but only when the packet arrived on an interface
marked to forward. The gate is **per-ingress-interface**: a `NETIF_FORWARD`
flag on the *receiving* interface, set on the tap and never on the real
NIC. So a guest packet arriving on the tap is forwarded, while a packet
arriving on the NIC for some other host is still dropped as not-for-us --
the host does not become a router for its real link. A forwarded packet is
routed (longest-prefix connected, then default), its TTL decremented (an
ICMP time-exceeded and a drop at zero, RFC 1812), and re-emitted out the
chosen interface with `output_on`; one that would go back out the
interface it arrived on, or that has no route, is dropped (with the
appropriate ICMP where the standard calls for it).

### 3. Masquerade NAT (source NAT)

A forwarded packet leaving an interface whose subnet does not contain its
source -- a guest packet from `10.0.3.15` going out the NIC -- has its
**source rewritten** to the outbound interface's address, and its
TCP/UDP source port (or ICMP echo id) rewritten to a free value the NAT
owns. The mapping (original source, port, destination, protocol) ↔
(rewritten port) is recorded in a **connection-tracking table**. A packet
arriving for the interface's address on a NAT-owned port is matched in the
table, its destination rewritten back to the original guest address and
port, and forwarded to the guest. The transport checksum is fixed up
incrementally (the pseudo-header changed); the IP checksum is recomputed.

The table is bounded and its entries expire (a short timeout for
completed or idle flows, longer for established TCP); a full table drops
new flows rather than growing. Because forwarding is enabled only on the
tap (one guest), the table's flows are that guest's, so a guest that opens
endless flows only drops its own new ones -- there is no other forwarding
client to starve. A per-client quota is what keeps them isolated once
forwarding serves more than one client (a container, a second tap), and is
the concern of that unit, not this one. ICMP errors
that quote a NAT'd packet are themselves translated so path-MTU and
unreachables reach the guest.

### 4. What crosses, and what does not

IP crosses, rewritten; the host does not proxy or terminate the guest's
connections (that would be a userspace NAT, §Alternatives). The guest is
statically configured for this unit -- address `10.0.3.15`, gateway
`10.0.3.1`, and a resolver it can reach through the NAT -- so it needs no
DHCP or DNS help yet. Only IPv4 is masqueraded; IPv6 forwarding and NAT,
and inbound port-forwarding (DNAT) to reach a guest service from outside,
are later units.

### 5. The milestone

- **Gated, in the harness:** a two-interface forward-and-NAT round trip
  with no external network. A guest-side tap (`10.0.3.0/24`) and an
  uplink-side tap (`10.0.2.0/24`); forwarding and masquerade on. A UDP (and
  a TCP-SYN, and an ICMP echo) datagram injected on the guest tap from
  `10.0.3.15` to a `10.0.2.0/24` address is read back on the uplink tap
  with its source rewritten to the uplink's address and a NAT port; a
  reply injected on the uplink to that address and port is read back on the
  guest tap with its destination rewritten to `10.0.3.15` and the original
  port -- the flow tracked both ways. Table exhaustion drops rather than
  grows; an entry expires.
- **Demonstrated, reproducible:** a stock Linux guest with the tap as its
  gateway reaches the host's network and, where the host has it, the
  internet -- `ping` and a `curl` of an external address -- the
  `QEMU_MEM=2G` reproduction, not a CI gate.

### 6. Deliberately out of scope

- **DHCP and DNS.** Autoconfiguring the guest's address and resolving its
  names are the next unit; here the guest is configured by hand and uses a
  resolver reachable through the NAT.
- **Inbound port forwarding (DNAT)** to reach a guest service from
  outside, and a general filtering firewall. This unit forwards and
  masquerades outbound flows and their replies; policy and inbound
  redirection are their own units.
- **IPv6 forwarding and NAT.** IPv4 masquerade first; IPv6 (where NAT is
  the exception, not the rule) is separate.
- **A userspace NAT in the owner.** Rejected in the tap unit and again
  here (§Alternatives): the host already has the stack; routing through it
  is smaller than reimplementing it in `vmctl`.

## Affected files

- `kernel-services/network/ipv4.c` — connected-route selection in
  `ipv4_route`; the forwarding path at the `rx_not_for_us` point; the ICMP
  time-exceeded.
- `kernel-services/network/nat.c` (new), `kernel/include/kernel/net/nat.h`
  — the connection-tracking table and the rewrite (out and back), with the
  incremental checksum fix-up.
- `kernel-services/network/netif.c` / `netif.h` — the `NETIF_FORWARD`
  (forward packets arriving here) and masquerade flags, set on the tap.
- `kernel-services/network/nettest.c`, `kernel/core/selftest.c`,
  `selftest.h` — the two-tap forward-and-NAT selftest.
- `docs/kernel-services/network/`, `docs/kernel-services/virtualization/`,
  `README.md` — the design and the Status entry.

## New APIs

No new system call, and no new control-plane ABI: forwarding and NAT are
internal to the stack, turned on by the tap setup setting `NETIF_FORWARD`
and the masquerade flag on the tap when a VM attaches (the `/dev/vmm`
sysctl surface is read-only, and this unit does not add a writable one).
The router-facing surface is `ipv4_route` gaining longest-prefix connected
routes, a forwarding hook in `ipv4_input` gated on the ingress
interface's flag, and `nat.c`'s translate-out / translate-back over the
conntrack table.

## Migration plan

1. **Connected-route selection** in `ipv4_route`, with its own test (a
   destination on an interface's subnet routes to that interface, not the
   default; the default still catches the rest). The existing net tests
   are the check that owned/loopback routing is unchanged.
2. **IP forwarding** at the `rx_not_for_us` point, gated on the ingress
   interface's `NETIF_FORWARD`: a packet not for the host that arrived on a
   forwarding interface is routed and re-emitted, the TTL decremented,
   time-exceeded at zero, a routeless or hairpin packet dropped; a packet
   that arrived on a non-forwarding interface is still dropped. Proved by
   forwarding between two taps with no NAT (a packet injected on one
   appears on the other, TTL down by one; TTL 1 yields an ICMP
   time-exceeded read back), and by a packet injected on a non-forwarding
   interface never appearing on the other.
3. **Masquerade NAT** (`nat.c`): the conntrack table, the out/back rewrite
   with the checksum fix-up, bounded with expiry. Proved by the two-tap
   round trip (UDP, TCP-SYN, ICMP echo), table exhaustion, and expiry --
   each translation checked and each bound proved by reintroducing its bug.
4. **The Linux demonstration**, documented and reproducible under
   `QEMU_MEM=2G`: the guest reaches the host's network and the internet.
5. **Docs and the Status entry**, and the full verification chain.

## Tests

- `net-route` (host): a destination on an interface's connected subnet
  routes to that interface; the default catches the rest; loopback and
  owned addresses are unchanged.
- `net-forward` (host): two taps, one marked `NETIF_FORWARD`. A packet
  injected on the forwarding tap is read back on the other with the TTL
  decremented; a TTL-1 packet yields an ICMP time-exceeded; a packet with
  no route or that would hairpin is dropped. And the gate: a packet
  injected on the *non-forwarding* interface is never forwarded (dropped as
  not-for-us) -- so real-NIC ingress stays disabled.
- `net-nat` (host): the two-tap masquerade round trip for UDP, a TCP SYN,
  and an ICMP echo -- the source rewritten out, the reply un-rewritten
  back, the checksums valid; the table bounded (a flood drops new flows,
  does not grow) and its entries expiring; an ICMP error quoting a NAT'd
  packet translated back to the guest.
- The existing net tests and a net-less boot stay green; forwarding is off
  unless enabled.

## Benchmarks

Forwarded throughput and added latency between two taps, recorded so a
later regression (or the DHCP/DNS unit) has a baseline. No absolute target.

## Risks

- **Becoming a router by accident.** The gate is per-ingress-interface
  (`NETIF_FORWARD` on the tap, never the NIC), so only packets that arrive
  on the tap are forwarded; a packet arriving on the real NIC for another
  host is still dropped as not-for-us. The host does not become a router
  for its real link.
- **A conntrack table a guest can exhaust.** Bounded with expiry; a full
  table drops new flows rather than grows. With forwarding on only for the
  tap, the table's flows are the one guest's, so it starves only itself;
  when a later unit forwards for more than one client, a per-client quota
  keeps them isolated.
- **A checksum left wrong by a rewrite.** A NAT that rewrites an address or
  port must fix the transport checksum, or every translated packet is
  silently dropped by the receiver. The incremental fix-up is tested by
  checking the round-tripped payload arrives, not merely that bytes moved.
- **The untrusted packet reaching more of the stack.** A forwarded guest
  packet now traverses routing and NAT, not just delivery. It takes the
  same validated `ipv4_input` path (header checks, martians) before
  forwarding, with no shortcut, and the NAT reads only the fields it
  rewrites.
- **Overselling reach.** The gated test forwards between two taps, not to
  the internet; the unit must say the real-world reach is the QEMU_MEM=2G
  reproduction, as the tap unit said of its Linux ping.

## Alternatives considered

- **A userspace NAT/SLIRP in the owner.** `vmctl` could terminate the
  guest's flows and reopen them on host sockets, reaching the world with no
  kernel forwarding. But it is a second TCP/IP/NAT implementation in
  userland, duplicating the stack the host has; routing through the real
  stack is smaller and reuses the transport, the checksums, the timers.
  Rejected, as in the tap unit.
- **Always-on forwarding (a router by default).** Simple, but it makes the
  host forward for every interface the moment it has two -- a policy the
  operator should choose, not a default. Rejected for the opt-in.
- **Full conntrack/firewall (a netfilter).** A general framework with
  filtering, DNAT, and hooks is far larger than "let the guest out." This
  unit does masquerade and its replies; a firewall is its own unit if the
  host ever needs to filter.
- **Interactive console instead.** Still a good small unit and still
  not-done, but the network arc has a named next step and the guest
  reaching the world is the consequential one; the console remains a
  separate pickup.
