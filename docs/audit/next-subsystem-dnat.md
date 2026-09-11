# NEXT SUBSYSTEM — reaching the guest from outside: inbound port forwarding (DNAT)

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This is that report, and
nothing in it is implemented.

**Subsystem: destination NAT (port forwarding) in the host stack, so a
connection arriving at the host on a configured port is rewritten to a guest
address and port and forwarded to the guest, and the guest's replies are
rewritten back — the inbound mirror of the masquerade the previous unit
built, and the last half of "a guest that is a machine on the network."**

## Problem

The guest can reach out, but nothing can reach in. Masquerade NAT lets a
guest open connections to the world and get the replies (source rewritten to
the host's address, the reply un-rewritten back). But a service the guest
*runs* — an SSH daemon on `10.0.3.15:22`, a web server on `:80` — is
invisible from outside: a client on the host's network that connects to the
host has its packet delivered to the host's own stack (`10.0.3.15` means
nothing to it), and the host has no rule that says "a connection to *this*
port belongs to the guest." The guest is a client of the network, not yet a
server on it.

## Current implementation

**Everything addressed to the host is delivered to the host.** `ipv4_input`
delivers a unicast whose destination is one of the host's own addresses to
the local transport (`udp_input` / `tcp_input`) — after `nat_in` gets a
first look, but `nat_in` only matches the *reply* to a masqueraded guest flow
or an ICMP error quoting one; a fresh inbound connection matches nothing and
falls through to local delivery. There is no port-forward table and no
inbound rewrite.

**The masquerade would rewrite a guest's reply the wrong way.** A guest
service's reply to an outside client is a forwarded packet leaving the
uplink; `nat_out` would masquerade it — source rewritten to the host's
address and a *lent* port — which is exactly wrong for a forwarded-in
connection, whose reply must carry the port the client originally reached
(`host:P`), not a fresh NAT port. Masquerade and DNAT must be reconciled.

**The pieces that exist and are reused:** the `nat.c` conntrack table and its
incremental checksum fix-up (`csum_patch16/32`); `ipv4_input`'s `nat_in` hook
for packets addressed to the host, and `ipv4_forward` / `nat_out` for
forwarded packets; the tap and its guest; the `fw_cfg` channel the resolver
address already rides.

## Why it matters

- **A guest that serves, not just consumes.** Outbound reach made the guest a
  client; inbound reach makes it a host others connect to — an SSH box, a web
  server, a database a developer runs in a VM and hits from the host. It is
  the half of NAT that a developer notices first (`ssh -p 2222 localhost`).
- **It completes the NAT picture symmetrically.** The masquerade unit
  rewrites outbound source and un-rewrites the reply; this rewrites inbound
  destination and un-rewrites the reply. Together they are what "NAT" means;
  apart, the guest is reachable in only one direction.
- **It is the reused foundation for a firewall.** A port-forward is a DNAT
  rule; a general filtering firewall (a later unit) is the same match-plus-
  action machinery with more actions. Building the DNAT rule builds the shape
  the firewall extends.

## Design (proposed)

### 1. The port-forward table, configured statically

A small table of rules, each `(protocol, host port) → (guest address, guest
port)` — for example "TCP port 2222 → `10.0.3.15:22`." A rule matches a
connection to **any** of the host's own addresses on that port -- a wildcard
host-address bind, as a container's default published port is `0.0.0.0`: the
match in §2 is exactly the existing "addressed to one of our addresses"
test plus the port, so `ssh -p 2222 localhost` on the host and a connection
to the host's uplink address on 2222 both hit the same rule. Binding a
forward to *one* specific host address (loopback only, or the uplink only) is
a later refinement, and the grammar leaves room for it (an optional address
prefix); this unit's rules are the wildcard form. The rules are read once, at
boot / tap activation, from `fw_cfg` `opt/cosmo/portforward` (a comma-
separated list of `proto:hostport:guestaddr:guestport`, e.g.
`tcp:2222:10.0.3.15:22,tcp:8080:10.0.3.15:80` — no host-address field, since
the bind is wildcard); there is **no writable control surface** in this unit,
matching the read-only stance of the tap and NAT units. A runtime API to add and remove forwards (through `vmctl`) is a
named later unit (§Alternatives); the static table is enough to prove the
mechanism and to run the demonstration.

### 2. Inbound DNAT, before local delivery

Where `ipv4_input` hands a packet addressed to the host to `nat_in`, `nat_in`
gains a second job: if the packet is *not* a reply to an existing flow but it
is addressed to one of the host's own addresses (the wildcard bind, §1) and
its `(proto, dport)` matches a port-forward rule, it is a connection **into**
the guest. `nat_in` records a conntrack entry (the client's
address and port, the matched rule, the guest target), rewrites the
destination to the guest's address and port (transport checksum fixed
incrementally, as the masquerade does), and **forwards it to the guest** out
the tap — reusing the forwarding emit path. This forward is authorized by the
*rule*, not by the ingress interface's `NETIF_FORWARD` flag: a DNAT'd packet
arrives on the uplink (which is not a forwarding interface), so the rule is
what lets it cross to the guest. For TCP the entry is created on the first
segment (a SYN); for UDP on the first datagram.

### 3. The guest's reply, un-DNAT'd

The guest answers the client from `guest:Q`; that reply is a forwarded packet
leaving the uplink, so it reaches `nat_out`. Before masquerading, `nat_out`
consults the DNAT conntrack: a forwarded packet that matches the reverse
direction of a DNAT entry has its **source** rewritten to the address and
port the client originally reached (`host:P`), not a masquerade port, and its
checksum fixed the same way. So the client sees replies from exactly the
`host:P` it connected to, and the connection works end to end. DNAT entries
therefore take **precedence** over masquerade in `nat_out`: a flow that is
half of a port-forward is never also masqueraded.

### 4. One conntrack table, two rule kinds

The DNAT entries live in the same bounded, expiring `nat.c` table as the
masquerade entries, distinguished by a kind flag; the same "bounded, expires,
drops when full" discipline applies, so inbound state a remote client can
create is as bounded as outbound state a guest can create. A port-forward
rule is matched at most once per new flow (then the conntrack entry carries
it), and an inbound SYN flood creates entries no faster than the table's
bound allows, dropping beyond it — the same reasoning as the masquerade
table, now facing the outside rather than the guest.

### 5. The milestone

- **Gated, in the harness:** two taps (a guest side `10.0.3.0/24` and an
  uplink side), a port-forward rule "TCP port `P` → `10.0.3.15:Q`" (a
  wildcard host-address bind), and a synthetic outside client on the uplink.
  The client sends a TCP SYN to the host's uplink address, port `P`; it is read back on the guest tap with its
  destination rewritten to `10.0.3.15:Q` (checksum valid) and its source
  intact. The guest answers (a SYN-ACK from `10.0.3.15:Q`); it is read back
  on the uplink with its source rewritten to the uplink address port `P`
  (checksum valid), so the client sees a reply from the address it dialed. A
  UDP request/response round trip through the same rule. A packet to a port
  with no rule is delivered to the host, not forwarded; the table is bounded
  (an inbound flood drops rather than grows) and its entries expire.
- **Demonstrated, reproducible:** a stock Linux guest running a service (an
  SSH or HTTP daemon) reached from the host through a port-forward, under
  `QEMU_MEM=2G`, the same reproduction shape as the tap and NAT units.

### 6. Deliberately out of scope

- **A writable control surface** (adding/removing forwards at runtime through
  `vmctl` or a syscall) — this unit's rules are static `fw_cfg`; runtime
  configuration is a later unit.
- **A general filtering firewall** (accept/drop/reject policy, chains, more
  match fields) — the DNAT rule is one action; a firewall is its own unit.
- **Hairpin / NAT-reflection** (a guest reaching its own forwarded service
  through the host address), **IPv6 DNAT**, and **many-to-one or
  load-balancing forwards** — named, later.

## Affected files

- `kernel-services/network/nat.c`, `kernel/include/kernel/net/nat.h` — the
  port-forward table, DNAT on the inbound path, un-DNAT in `nat_out` with
  precedence over masquerade, the conntrack entry's kind flag.
- `kernel-services/network/ipv4.c` — `nat_in` gains the DNAT-forward path
  (a matched inbound packet is rewritten and forwarded to the guest); the
  `nat_out` call already exists.
- `kernel-services/network/tapsvc.c` or `tap.c` — read the `fw_cfg`
  port-forward rules when the tap is set up.
- `kernel-services/network/nettest.c`, `kernel/core/selftest.c`,
  `selftest.h` — the `net-dnat` self-test.
- `docs/kernel-services/network/`, `docs/kernel-services/virtualization/`,
  `README.md` — the design and the Status entry.

## New APIs

No new system call and no new control-plane ABI: the port-forward rules are
read from a read-only `fw_cfg` value, and DNAT is internal to the stack
(`nat.c` gaining an inbound match and a reverse rewrite). The router-facing
surface is the existing `nat_in` / `nat_out`, extended.

## Migration plan

1. **The port-forward table and its `fw_cfg` parse**, with a test that a
   well-formed rule string is parsed and a malformed one rejected.
2. **Inbound DNAT**: `nat_in` matches a rule, records the entry, rewrites the
   destination and forwards to the guest. Proved by a client SYN read back on
   the guest tap with the destination rewritten (checksum valid) and a packet
   to an unruled port delivered locally.
3. **The reply un-DNAT'd in `nat_out`**, with precedence over masquerade.
   Proved by the guest's SYN-ACK read back on the uplink with its source
   rewritten to `host:P` (checksum valid), for TCP and UDP.
4. **Bounded and expiring**, proved by an inbound flood dropping rather than
   growing and by aging; each bound bug-proved by reintroducing its bug.
5. **The Linux demonstration**, documented and reproducible under
   `QEMU_MEM=2G`; then docs and the Status entry, and the full chain.

## Tests

- `net-dnat` (host): two taps and a static port-forward rule. A client SYN to
  `host:P` is read back on the guest tap rewritten to `guest:Q` (checksum
  valid, source intact); the guest's SYN-ACK is read back on the uplink
  rewritten to source `host:P` (checksum valid); a UDP round trip through the
  rule; a packet to an unruled port is delivered to the host, not forwarded;
  the table is bounded (a flood drops) and expires. Each behaviour and bound
  proved by reintroducing its bug.
- The existing net, forwarding and NAT tests and a net-less boot stay green;
  DNAT does nothing without a rule.

## Benchmarks

Added latency of a DNAT'd connection setup versus a directly-delivered one,
between two taps, recorded so a later regression (or the firewall unit) has a
baseline. No absolute target.

## Risks

- **Inbound state a remote can create.** Unlike masquerade, whose flows the
  one guest creates, DNAT flows are created by outside clients. The table is
  bounded and expiring and drops when full — the same discipline — so an
  inbound SYN flood fills the table and no more, and never grows it; the
  reply path holds no unbounded state.
- **Forwarding a packet the ingress did not authorize.** A DNAT'd packet
  crosses from the uplink to the guest though the uplink is not a
  `NETIF_FORWARD` interface. This is deliberate and gated by the *rule*: only
  a packet matching a configured port-forward crosses, and only to that
  rule's guest target; everything else on the uplink is delivered or dropped
  as before. The host does not become a general router for its uplink.
- **A rewrite that leaves a wrong checksum.** As with masquerade, a DNAT that
  rewrites an address or port must fix the transport checksum or the receiver
  drops the packet silently; the test checks the round-tripped connection,
  not merely that bytes moved.
- **DNAT and masquerade disagreeing on a flow.** A guest reply that is half
  of a port-forward must be un-DNAT'd, not masqueraded; getting the
  precedence wrong sends the client a reply from a NAT port it never dialed.
  The test asserts the reply's source is exactly `host:P`.
- **Overselling reach.** The gated test forwards between two taps, not from a
  real external host; the unit says plainly that the real-world reach is the
  `QEMU_MEM=2G` reproduction, as the tap and NAT units said of theirs.

## Alternatives considered

- **A writable control surface for port-forwards** (a `vmctl` command or a
  syscall that adds a rule at runtime). The natural long-term shape — an
  operator forwards a port without rebooting — but it introduces a writable
  control ABI this arc has so far avoided, and the static `fw_cfg` table
  proves the mechanism and runs the demo without it. Deferred to its own
  unit, where the ABI can be designed deliberately (and shared with the
  firewall's rules).
- **Terminating connections in the owner (a userspace proxy).** `vmctl` could
  listen on the host port and open a matching connection to the guest,
  reaching the service with no kernel DNAT — but it is a second connection
  and a second copy of the flow, and does not preserve the client's address
  to the guest. Rejected, as the userspace-NAT alternative was in the NAT
  unit: routing through the real stack is smaller and truthful about the
  client.
- **A general firewall now.** Match-and-action with accept/drop/reject and
  chains would subsume port-forwarding, but it is a far larger unit than
  "reach the guest's service," and most of it (filtering policy) is not what
  the arc needs next. This unit builds the one action — DNAT — that the
  firewall will later extend.
- **Nothing more (the guest stays outbound-only).** Defensible — many guests
  only need to reach out. Rejected because a guest that cannot be reached is
  half a machine, and the tap and NAT units both named inbound port
  forwarding as the next step.
