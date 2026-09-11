# NEXT SUBSYSTEM — the host chain: what the world may ask of the host

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This is that report, and
nothing in it is implemented.

**Subsystem: the firewall's third chain, and the first that is not about a
guest. FORWARD decided which machines a guest may reach; INPUT decided which
of the host's services a guest may reach; this decides which of the host's
services the **world** — anything arriving on the uplink — may reach. A
datagram from a real, non-guest link addressed to the host gets a verdict in
`ipv4_input` after `nat_in` declines it, against a **host-scoped** rule list
and default (the sentinel `guest_addr == 0` on the existing control channel,
refused by every op today), with rules that can finally name a **source**
(`src_ip/src_prefix`, the field a world-facing chain cannot do without). The
default is **ACCEPT** — today's behaviour, since the host runs services meant
to be reached and the host's own connections must keep working — and the
operator drops by source, protocol and port or flips to a hardened default.
Two things close without any rule. First, an **off-link invariant**: a
datagram arriving on a link is for an address *on that link* — the ingress
interface's own, or a broadcast — and anything else is dropped before any
chain. Today `netif_owns_ipv4` treats every interface's address as the
host's and the martian check looks only at the source, so an uplink datagram
addressed to a guest's gateway is delivered into that guest's DNS proxy (an
open resolver per guest), and one addressed to `127.0.0.1` reaches
**loopback-bound services** — both reachable from the real network. Second,
the chain's "established" bypass is keyed on **actual connection state in
an eligible state** — a connection that is established or actively closing,
a half-open child whose SYN already passed the rules, or the very SYN+ACK the
host solicited; a connected UDP socket's peer — never on flags alone and
never on a mere transient tuple (`TIME_WAIT`, an unsolicited segment to
`SYN_SENT`), which `tcp_input` would answer —
so a DROP rule means silence, not a RST that confirms the host is there. And
the chain sits where the last unit proved it could not yet be observed:
after `nat_in`, so a DNAT'd inbound connection is never re-gated — now
provable, since DNAT'd traffic does arrive on this ingress.**

## Problem

Three of the four parties the network stack serves are now under policy:
guest→guest and guest→world (FORWARD, PR #103), guest→host (INPUT, PR #105).
The fourth is the world→host path, and it is wide open:

- **Any host listener is reachable from the uplink.** A datagram arriving on
  the NIC addressed to any address the host owns is delivered to whatever
  socket or handler is bound there (`ipv4_input`: martian check on the
  *source* at `:573-575`, "not for us" only when the destination is owned by
  *no* interface at `:583`, `nat_in` at `:599`, then the transport demux).
  There is no way to say "this management port only from this subnet", or
  "nothing from the world but what DNAT forwards".
- **Every guest's DNS proxy is exposed to the world.** `tapsvc` binds each
  tap's proxy to *that tap's gateway* (`10.0.(3+k).1:53`) precisely so a proxy
  on `0.0.0.0` would not answer on the real interface. But `netif_owns_ipv4`
  (`netif.c`) returns true for **any** interface's address, so a datagram from
  the uplink to `10.0.3.1:53` is "ours", passes the source-only martian check,
  and is delivered into the proxy — an open resolver per guest, reachable
  from the real network, usable for amplification. The INPUT chain does not
  see it (it is gated on guest-tap ingress); nothing does.
- **Loopback-bound services are exposed to the world too.** `netif_owns_ipv4`
  answers true for `127/8`, the martian check examines only the source, and
  the TCP/UDP listener lookup does not restrict a match by ingress interface
  — so an uplink datagram addressed to `127.0.0.1` passes every check and is
  delivered to a socket bound to loopback, the very binding a service uses to
  mean "local callers only". The same is true of a guest tap's datagram to
  `127.0.0.1` (today caught only by INPUT's default DROP, which an operator
  rule with an `any` destination would reopen).
- **Rules cannot name a source.** `struct fw_rule` matches direction, proto,
  destination and selector. For a guest that was right: the source was the
  guest. For the world it is meaningless — the one thing a host firewall rule
  says is *who* may reach a port.

## Current implementation

`kernel-services/network/ipv4.c`, `ipv4_input`, for a datagram arriving on
the NIC (a `netif` that is neither `NETIF_MASQUERADE` nor `NETIF_LOOPBACK`):

- `:573-575` drops a *source* that is loopback or one of our own addresses;
  `:583` drops or forwards a destination owned by no interface; `:599` offers
  an owned destination to `nat_in` — a masqueraded reply, an ICMP error
  quoting one, or an inbound DNAT is rewritten and re-emitted there and never
  reaches host services. Everything else is delivered. The INPUT chain's
  `fw_input_verdict` runs only when `nif->flags & NETIF_MASQUERADE` — a guest
  tap — so the uplink is not consulted, by design (PR #105 named this unit).
- `netif_owns_ipv4(addr)` (`netif.c`) answers "does *any* interface hold this
  address" and is the only notion of "ours" on the input path; there is no
  "is this address on the *ingress* link" check.

`kernel-services/network/fw.c`: per-guest rule lists keyed by the guest's
address; `struct fw_rule { direction, proto, dst_prefix, verdict, dst_ip,
dst_port }` — no source. Directions `TO_UPLINK`, `TO_GUEST`, `TO_HOST`, all
describing *a guest's* datagram; `ANY` is forwarding-only. The control
channel (`tap.c` `tap_ctl_filter`, `tap.c:306`) and the forward op
(`tap.c:277`) both refuse `guest_addr == 0`.

`kernel-services/network/nat.c`: `nat_in` claims a masquerade reply or a
DNAT'd inbound before local delivery; a DNAT is authorized by its port-forward
rule. The INPUT-chain unit could not prove its "after `nat_in`" placement
mattered, because nothing `nat_in` claims arrives on a guest tap; DNAT'd
traffic arrives on the uplink — this chain's ingress.

What reaches the host from the uplink today and must keep working: the
boot-test harness's echo listeners (`tests/boot/nettest.py` connects through
QEMU `hostfwd` to ksock listeners on the NIC address), DNAT'd connections to
guests (claimed by `nat_in` first), replies to the host's own outbound flows
(TCP segments of its connections, UDP replies to its sockets), and ICMP echo
to the host. The NIC's address is a static QEMU default (`netif.c`
`netif_autoconfig`; "DHCP is a later unit"), so no DHCP client needs a hole.

## Why it matters

- **It completes the firewall's coverage.** Every ingress→egress pair the
  stack serves — guest→guest, guest→world, guest→host, world→host — then has
  a chain; the remaining unfiltered path is the host's own egress (OUTPUT),
  which is the trusted party.
- **It closes a real exposure with no operator action.** The per-guest open
  resolvers reachable from the real network are a topology error, not a
  policy choice; the built-in "tap-side addresses are reachable only from
  their tap" check removes them and protects whatever else a guest-facing
  service is bound to.
- **It gives the rule model its missing dimension.** A source match is what
  every later policy feature (management-subnet allow-lists, per-tenant
  ingress) needs; adding it once, versioned, is the leverage.
- **It makes the deferred proof runnable.** The INPUT unit documented that
  "verdict after `nat_in`" was unobservable; here a DNAT'd SYN arrives on the
  gated ingress, and a host DROP-all rule must not touch it.

## Proposed design

### 1. A host-scoped policy object, on the same engine

The engine's rule lists are keyed by guest address; the host is one more
key. **`guest_addr == 0` is the host** — a distinguished, always-present
policy object (no attach, no purge), holding one ordered rule list and one
default. It is addressed through the existing control channel exactly as a
guest is (`FILTER_ADD`/`FILTER_DEL`/`FILTER_POLICY` with `guest_addr = 0`),
and listed as a record with `guest_addr 0`. Every tapctl op refuses `0`
today, so the sentinel collides with nothing; a host-scoped rule or policy
may name only the new direction below (any other direction with
`guest_addr 0` is `-EINVAL`), and a guest-scoped rule may not name it (the
world does not send "as a guest").

### 2. `FROM_UPLINK`: the fourth direction

`enum fw_dir` gains `FW_DIR_FROM_UPLINK` (`COSMO_NETCTL_DIR_FROM_UPLINK = 4`):
a datagram arriving on a **real, non-guest link** — `nif->flags` has neither
`NETIF_MASQUERADE` nor `NETIF_LOOPBACK` — addressed to the host (an owned
unicast after `nat_in` declines it, or a broadcast). Today that is the NIC;
any future real link takes the same chain (per-interface host chains are a
later refinement). `ANY` keeps its forwarding-only meaning and never matches
it (the lesson of PR #105 round 1: a wildcard must not grow into a new,
more sensitive scope).

### 3. Rules gain a source

`struct fw_rule` gains `src_ip`/`src_prefix` (`0/0` = any), matched like the
destination. It is required by this chain and permitted nowhere else: a
guest-scoped rule must carry `0/0` (its source is the guest; `-EINVAL`
otherwise), so tuples stay canonical. The identity of a rule remains its
whole tuple, now including the source. UAPI: `struct cosmo_netctl_filter`
grows `src_addr` (u32) and `src_prefix` (u8, + 3 reserved) — 20→28 bytes;
`struct cosmo_netctl_filter_rule` likewise 16→24; `COSMO_NETCTL_VERSION` →
4. The dispatcher's exact-size-per-op discipline refuses a v3 writer with
`-EINVAL`/`-ENOTSUP` rather than misread it; `COSMO_NETCTL_SNAPSHOT_MAX`
grows with the rule record and is static-asserted as before.

### 4. Enforcement: after `nat_in`, gated on the uplink, before the demux

`fw_host_verdict(nif, m, iph, ihl)` is called in `ipv4_input` at the same
point as the INPUT chain's verdict but for the complementary ingress:
`!(nif->flags & (NETIF_MASQUERADE | NETIF_LOOPBACK))`. Order, and what each
step protects:

1. **The off-link invariant: a datagram arriving on a link is for an address
   on that link.** Before either chain, and for **every non-loopback
   ingress** (the uplink *and* the guest taps), a locally-delivered
   destination must be the ingress interface's own address (`nif->ip4.addr`
   — a `netif` holds one IPv4 address, so no new lookup is needed) or a
   broadcast; anything else is dropped and counted `rx_offlink`. This is a
   fact of the topology, not a policy, and it closes three exposures at once
   with no configuration: an uplink datagram to a guest's gateway (the open
   resolvers), an uplink datagram to `127/8` (loopback-bound services —
   *not* a martian today; the martian check is source-only), and a guest's
   datagram to `127/8` or to another interface's address (today gated only
   by INPUT's default DROP, which an `any`-destination rule would reopen).
   Loopback ingress is exempt (the host talking to itself); DNAT'd and
   masqueraded traffic is addressed to the uplink's own address and is on
   link. INPUT's existing "guest → the host's uplink address" case moves from
   a default drop to this invariant (its test adjusts).
2. **"Established" means a connection the host actually has, in a state
   that expects this segment — never a flag, and never merely "some PCB".**
   The state filter lives in the TCP layer, which owns the states:
   `tcp_conn_accepts(local, lport, remote, rport, flags)` answers true only
   when a connection for the four-tuple is in an **eligible state** —
   `ESTABLISHED`, `FIN_WAIT_1/2`, `CLOSE_WAIT`, `CLOSING`, `LAST_ACK` (a peer
   whose segments are expected); `SYN_RCVD` (a half-open child whose SYN
   already passed the rules, so its completing ACK must be admitted); and
   `SYN_SENT` **only for a segment carrying SYN+ACK or RST** (the handshake
   reply the host itself solicited). `TIME_WAIT`, `LISTEN`, `CLOSED` and any
   other segment to a transient tuple are **not** eligible and take the
   rules — because `tcp_input` answers an invalid segment in those states
   with a RST or a challenge ACK, and a DROP that answers is not a DROP. A
   UDP datagram bypasses only if a *connected* UDP socket names that peer
   (`udp_conn_exists`). Everything else — a SYN, an ACK-only or SYN+ACK probe
   with no eligible connection behind it, a datagram to a listener — is
   unsolicited and takes the rules. So the host's own outbound connections
   and its connected UDP flows keep working under any rule set, and a DROP
   answers a probe with **silence**: the flag-only bypass the draft first
   proposed would have let an ACK probe through to `tcp_input`, and the
   "any non-listening PCB" form that replaced it would still have let one
   through when it happened to match a `SYN_SENT` or `TIME_WAIT` tuple.
3. **Rules, first match, else the host default.** A rule matches
   `FROM_UPLINK`, proto, **source prefix**, destination prefix and selector
   (port, or ICMP type as before).

On `FW_DROP` the datagram is freed and `ip_stats.hin_filtered` counted.

**What remains stateless and why that is acceptable here.** A datagram to
an *unconnected* UDP socket (a listener, or a client socket that `sendto`s
without connecting) and every ICMP message have no connection to key on, so
a rule applies to each such datagram. With the default ACCEPT this costs
nothing; an operator who writes a broad UDP DROP (`udp any any`) would drop
replies to the host's unconnected UDP sockets — documented, with the
guidance that UDP rules name listener ports (or a source prefix), and with
reply state for unconnected UDP and for ICMP named as a later unit. ICMP echo
to the host is gated by type as before (type 8); the host's own ping replies
(type 0) pass the default.

### 5. The default: ACCEPT, and why

`FROM_UPLINK` default **ACCEPT**. This is the first chain where default DROP
would break the machine rather than a guest: the harness's listeners, DNAT'd
connections' host-side handling, ICMP echo, and — with reply state only for
connected UDP sockets — every reply to an unconnected UDP socket of the host
itself. A host firewall's first unit ships the
mechanism and the one topology fix that needs no policy; the operator
hardens with `FROM_UPLINK … DROP` rules by source/port, or flips the default
to DROP and allows explicitly (a hardened host adds `tcp any :22 ACCEPT from
mgmt/24`, `icmp type 8 ACCEPT`, and lets DNAT'd ports through untouched since
`nat_in` claims them first). The INPUT chain's seeds have no analogue here:
there is no small, known set of "services the uplink is offered".

### 6. Control plane and `vmctl`

`vmctl filter add host world PROTO SRC[/PREFIX]|any DST[/PREFIX]|any PORT
VERDICT [INDEX]` — `host` as the guest word writes `guest_addr 0`; `world`
as the direction word writes `FROM_UPLINK`; the **`SRC` argument exists only
for host rules** (guest rules keep their present shape and write `0/0`).
`del` takes the same tuple; `policy host world accept|drop`; `list` prints
the host record (`policy_from_uplink` — the guest record's last reserved
byte, size unchanged) and host rules with their source. `list` continues to
refuse a snapshot whose version is not the one it speaks.

### 7. Deliberately out of scope (named, later units)

- **Reply state for unconnected UDP and for ICMP** (a connected UDP socket
  is covered by `udp_conn_exists`; an unconnected client socket's replies
  and ICMP replies still take the rules, which the default ACCEPT admits).
- **Per-interface host chains** (all real links share `FROM_UPLINK` here).
- **An OUTPUT chain** for the host's egress; rate-limit/log targets; IPv6.
- **DHCP client protection** — moot until the host has a DHCP client.

## Affected files

- `kernel/include/kernel/net/fw.h` — `FW_DIR_FROM_UPLINK`; `src_ip/src_prefix`
  in `struct fw_rule`; `FW_HOST_GUEST_IP 0`; `enum fw_verdict
  fw_host_verdict(struct netif *nif, struct mbuf *m, const struct ipv4_hdr
  *iph, unsigned ihl);` host default in a separate policy slot;
  `fw_stats` gains `hin_accept_rule/hin_drop_rule/hin_accept_default/
  hin_drop_default/hin_bypass_conn` (the off-link drop is an IP-layer
  count, `ip_stats.rx_offlink`, since it precedes both chains).
- `kernel-services/network/fw.c` — the host object (a permanent `fw_guest`
  slot with `ip == 0`, never attached/purged, `fw_flush` resets it);
  `rule_valid` (source `0/0` unless host-scoped; `FROM_UPLINK` only with
  host scope); `rule_matches` gains the source prefix; `fw_host_verdict`.
- `kernel-services/network/tcp.c` / `kernel/include/kernel/net/tcp.h` —
  `bool tcp_conn_accepts(uint32_t local, uint16_t lport, uint32_t remote,
  uint16_t rport, uint8_t flags)`: does a connection for this four-tuple
  exist in a state that expects this segment — `ESTABLISHED`, `FIN_WAIT_1/2`,
  `CLOSE_WAIT`, `CLOSING`, `LAST_ACK`, `SYN_RCVD`, or `SYN_SENT` for a
  SYN+ACK/RST — over the table `tcp_input` demuxes with; no state change;
  the state filter lives here because the states do.
- `kernel-services/network/udp.c` / `kernel/include/kernel/net/udp.h` —
  `bool udp_conn_exists(...)`: is a *connected* UDP socket bound to this
  four-tuple.
- `kernel-services/network/ipv4.c` — the off-link check for every
  non-loopback ingress (before either chain; `ip_stats.rx_offlink`); the
  second call site (uplink ingress); `ip_stats.hin_filtered`.
- `kernel/include/uapi/cosmo/netctl.h` — version 4: `DIR_FROM_UPLINK`,
  `src_addr/src_prefix` in the filter command and rule records,
  `policy_from_uplink`, the grown `SNAPSHOT_MAX`.
- `kernel-services/network/tap.c` — accept `guest_addr 0` for filter ops;
  list the host record; the v4 exact sizes; static-asserts.
- `userland/system/vmctl.c` — `host`/`world` words, the host-only `SRC`
  argument, listing the source and the fourth policy; version check as
  before.
- `kernel-services/network/nettest.c`, `kernel/core/selftest.c`,
  `kernel/include/kernel/selftest.h` — `net-hostinput`; `net-firewall`/
  `net-input`/`net-tapctl` adjusted for the v4 record sizes; `net-input`'s
  "guest → the host's uplink address" case moves from a default drop to the
  off-link drop (`rx_offlink`).
- `docs/kernel-services/network/design.md` (the firewall section gains the
  host chain; the INPUT section's "unobservable ordering" note becomes the
  proof), `testing.md`, `README.md` Status; this report as built.

## New APIs

- In-kernel: `FW_DIR_FROM_UPLINK`; `fw_host_verdict(...)`; `struct fw_rule
  { …, src_ip, src_prefix }`; `tcp_conn_accepts(local, lport, remote,
  rport, flags)` (the eligible-state query); `udp_conn_exists(...)`. `fw_rule_add/del/
  list` and `fw_policy_set/get` accept `guest_ip == 0` for the host object.
- UAPI (version 4): `COSMO_NETCTL_DIR_FROM_UPLINK`, `cosmo_netctl_filter.
  src_addr/src_prefix`, `cosmo_netctl_filter_rule.src_addr/src_prefix`,
  `cosmo_netctl_filter_guest.policy_from_uplink`, `guest_addr 0` = the host.
  No new opcode, no new syscall.

## Migration plan

1. `ipv4.c`: the off-link invariant for every non-loopback ingress
   (`rx_offlink`); `tcp_conn_accepts` (the eligible-state query) /
   `udp_conn_exists`; `fw.c`: the host
   object, the source fields, the new direction, `fw_host_verdict`
   (connection-state bypass, rules/default); the second `ipv4.c` call site;
   both arches boot with the default ACCEPT and every existing test green
   except `net-input`'s uplink-address case, which moves to the invariant
   (nothing else in the tree sends off-link traffic).
2. UAPI v4 and the size changes; `tap.c`; `vmctl`; the record-size
   adjustments in the three existing tapctl-reading tests.
3. `net-hostinput`; docs; README; the report as built.

The behaviour changes: (a) a datagram delivered locally from any
non-loopback ingress must be for that link's own address or a broadcast —
so an uplink datagram to a tap's gateway or to `127/8`, and a guest's
datagram to `127/8` or to another interface's address, are dropped: the
open-resolver and loopback-exposure fixes, intended, with `net-input`'s
uplink-address case moving to the invariant's counter; (b) nothing else
until an operator adds a rule.

## Tests

`net-hostinput` (new): an "uplink" tap `u` as `net-dnat` builds one (a real,
non-guest link) plus two guests through `/dev/net/tap`; ksock listeners on
the host; verdicts awaited on the worker as in `net-input`.

- **Default ACCEPT**: a SYN from the world to a host TCP listener and a UDP
  datagram to a host UDP listener are delivered (`hin_accept_default`).
- **A DROP rule with a source**: `FROM_UPLINK tcp from 10.0.2.0/24 any :2222
  DROP` drops a SYN from `10.0.2.99` (`hin_drop_rule`) while a SYN from
  `10.0.9.9` (outside the prefix) is still delivered — the source match is
  real.
- **"Established" is a connection, not a flag**: a world SYN accepted by a
  rule to a host listener completes a handshake (the host's SYN-ACK is read
  back on the uplink tap; the world's ACK is delivered); a DROP rule for that
  port is then added — and the *established* connection's next ACK-carrying
  segment still passes (`hin_bypass_conn` rises), while an **ACK-only probe
  from a source with no connection** to the same port is dropped
  (`hin_drop_rule`) and **no RST is read back** on the uplink tap. The same
  for SYN+ACK with no connection. A datagram to a *connected* UDP socket's
  peer bypasses; one to an unconnected listener takes the rules.
- **Transient states are not "established"**: with a DROP rule for the port,
  the host closes an accepted connection (the four-tuple enters `TIME_WAIT`)
  and a bare ACK from the peer to that tuple is dropped with no RST or
  challenge ACK read back; the host `connect`s out to a peer on the uplink
  tap (its SYN is read back; the tuple is `SYN_SENT`) and a bare ACK from
  that peer is dropped silently while the peer's SYN+ACK is admitted and
  completes the handshake; a half-open `SYN_RCVD` child's completing ACK is
  admitted (its SYN passed the rules).
- **UDP and ICMP are per datagram**: a UDP DROP rule drops every matching
  datagram; an `icmp type 8 DROP` drops an echo request and no reply comes
  back, while a type-0 datagram to the host passes the default.
- **Off-link: a link's datagrams are for that link's address**: with no
  rule installed, a datagram from the world to guest A's gateway `:53` is
  dropped with `rx_offlink` and the proxy sees nothing (a canary query gets
  no relay), while the same query from A's own tap is answered (INPUT's
  seed); a datagram from the world to `127.0.0.1:<port>` of a
  loopback-bound ksock listener is dropped with `rx_offlink` and the
  listener receives nothing (and a loopback-sent one still arrives); a
  datagram from guest A's tap to `127.0.0.1` or to the host's uplink address
  is dropped with `rx_offlink` (INPUT's former default-drop case, now the
  invariant).
- **Ordering after `nat_in`, now provable**: with a port-forward
  `tcp:8080 → A:80` and a host rule `FROM_UPLINK tcp any any :8080 DROP`
  (or default DROP), a client SYN to `host:8080` is **still DNAT'd to A**
  (read back on A's tap) — DNAT'd traffic is authorized by the pf rule and
  never re-gated; `hin_*` counters do not move for it.
- **Scope discipline**: `FILTER_ADD` with `guest_addr 0` and direction
  `TO_HOST` is `-EINVAL`; with a guest address and `FROM_UPLINK` is
  `-EINVAL`; a guest rule with a non-zero source is `-EINVAL`; an `ANY`
  rule does not match the world.
- **Not this chain's ingress**: a guest-tap datagram to the host still takes
  the INPUT chain (its seeds work, `hin_*` unchanged); loopback is untouched.
- **Default flip**: `policy host world drop` then an unruled SYN drops and
  an explicit `ACCEPT` rule readmits it; back to ACCEPT.
- **Control round trip**: the listing carries a `guest_addr 0` record with
  `policy_from_uplink` and the host rules with their source; `vmctl`-shaped
  writes with `host`/`world` and a `SRC` land as intended; a v3-sized write
  is refused.
- **Regression**: the harness's `NETTEST` echo round trip, `net-dnat`,
  `net-tapctl`, `net-input`, `net-firewall` unchanged in verdict.

Bug-proofs: a verdict that ignores rules (the sourced DROP then delivers);
a source match that ignores the prefix (the out-of-prefix SYN then drops);
a bypass keyed on flags instead of the connection table (the ACK-only probe
from a source with no connection then reaches `tcp_input` and a RST is read
back on the uplink tap); a state filter that admits every non-listening PCB (a bare ACK to the
host's `SYN_SENT` tuple, or to a `TIME_WAIT` tuple, then reaches
`tcp_input` and a RST or challenge ACK is read back); a bypass that consults
no state at all (the established connection's ACK then drops under the
rule); the off-link
invariant removed (the world's query then reaches A's proxy and is relayed,
and the world's datagram reaches the loopback-bound listener); the call
site moved before `nat_in` (the DNAT'd SYN then drops under the host DROP
rule — the proof the INPUT unit could not run).

## Benchmarks

The uplink is the hot path: the NIC's receive rate with an empty host rule
list (one flag test and the owner lookup on the destination) and with a
short list, measured with `net-nicbench`; the owner lookup is the same
`g_netifs` walk `netif_owns_ipv4` already does once per datagram, and can
share it. Nothing on the tap or loopback paths changes.

## Risks

- **A broad UDP/ICMP DROP breaks the host's own replies.** No reply state
  in this unit. Mitigation: default ACCEPT; documented guidance (name
  listener ports or a source prefix); reply state named as the next unit.
- **The off-link invariant surprises someone reaching a guest gateway, or
  `127.0.0.1`, from the LAN.** There is no legitimate case — a gateway
  address exists for the guest's link only, and a loopback binding *means*
  "local callers only"; the existing anti-spoof already assumes a tap is a
  point-to-point link. The loopback exposure is pre-existing and this closes
  it. Documented as the one unconditional behaviour change (and the one
  existing-test adjustment, in `net-input`).
- **The connection lookup on the hot path.** Every non-SYN TCP segment from
  the uplink to a ruled or default-dropped port now costs a table lookup
  that `tcp_input` would perform anyway a few instructions later; the
  lookup is read-only and shares its table, and the benchmark measures it.
  A segment for an established connection pays one lookup, then bypasses.
- **ABI v4 grows two structs.** Exact-size dispatch refuses a v3 writer; a
  v3 `vmctl list` refuses a v4 snapshot by version; `SNAPSHOT_MAX` is
  recomputed and static-asserted; `vmctl` is built with the kernel.
- **Sentinel `0` for the host.** Refused by every op today, so no existing
  caller means it; documented in the UAPI header.
- **Ordering.** The verdict must stay after `nat_in`; the bug-proof that
  moves it demonstrates the DNAT breakage this unit finally makes visible.

## Alternatives considered

- **Reuse the INPUT chain's `TO_HOST` for uplink ingress too.** Rejected:
  `TO_HOST` rules are per guest (keyed by the guest's address); the world
  has no guest key, the rule needs a source the guest chain forbids, and
  the defaults differ (guest DROP with seeds; world ACCEPT).
- **Default DROP with seeded rules, as INPUT did.** Rejected: the host's
  services from the uplink are not a small known set, and without UDP/ICMP
  reply state a default DROP breaks the host's own flows; ACCEPT-then-harden
  is what every host firewall ships first.
- **A separate control node (`/dev/net/hostctl`).** Rejected: the existing
  channel's shape, dispatch, versioning and listing carry the host object
  unchanged under the `0` sentinel; a second node duplicates all of it.
- **Fix the open resolver by binding the proxies to a tap-only scope
  instead of an off-link check.** Rejected as insufficient: the proxy is one
  service and loopback bindings are another whole class; the off-link
  invariant protects every current and future binding that is not on the
  ingress link, and states the actual fact (a datagram arriving on a link is
  for that link's address).
- **An "established" bypass keyed on TCP flags.** The draft's first form;
  rejected on review: an ACK-only or SYN+ACK probe with no connection behind
  it would pass to `tcp_input`, whose RST confirms the host and its closed
  ports — a DROP rule that still answers is not a DROP. The connection-table
  lookup costs what `tcp_input` was about to spend anyway.
- **A bypass for any non-listening PCB.** The draft's second form; rejected
  on review: the table also holds `SYN_SENT`, `SYN_RCVD`, the closing states
  and `TIME_WAIT`, and `tcp_input` answers an invalid segment in several of
  those with a RST or a challenge ACK — so a probe that happened to match a
  transient tuple would still be answered. The eligible-state filter names
  exactly the states in which a segment from that peer is expected, and
  lives in the TCP layer with the states.
- **Drop at the socket layer (refuse a match whose ingress is not the
  bound interface).** Rejected: it scatters the invariant across every
  transport's demux and cannot be listed or counted as one thing; the IP
  layer knows the ingress and the destination together.
- **Per-interface host chains now.** Rejected for scope: one real link
  exists; the chain is written so a second one can be split off later.
