# NEXT SUBSYSTEM — the INPUT chain: what a guest may ask of the host

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This is that report, and
nothing in it is implemented.

**Subsystem: the firewall's second chain. The forwarding firewall decided
which *other machines* a guest may reach; this decides which of the **host's
own services** a guest may reach. A datagram a guest tap delivers to the host
gets a verdict at the local-delivery site in `ipv4_input`, after NAT declines
it and before the transport demux — evaluated against the same per-guest,
first-match rule engine as the FORWARD chain, as a third direction,
`TO_HOST`, needing no new struct, only a new direction value and a third
default policy. Two gaps close at once: (1) host-local delivery has **no
filter today**, so a guest can reach any host listener through its gateway
address (management sockets, debug services, another tenant's helper) — the
default becomes "the services the tap offers, and nothing else", with those
services installed as visible, deletable rules rather than hard-coded holes;
and (2) host-local delivery has **no anti-spoof today** — the strict-source
check lives only in `ipv4_forward` — so a guest can forge its source toward
host services; the same rule now guards the local path. The host's own
exposure to the *uplink* (a host-scoped firewall) and the host's egress (an
OUTPUT chain) are named as the next units.**

## Problem

The forwarding firewall (PR #103) isolated guests from each other and from
the world by policy, and scoped itself to the FORWARD chain, naming an INPUT
chain as the follow-up. That leaves the host itself as the exposed party:

- **Any host listener is reachable from a guest.** A guest sends to its tap
  gateway (`10.0.(3+k).1`) — or to *any* address the host owns, its uplink
  address included — and `ipv4_input` delivers it to whatever UDP/TCP socket
  or ICMP handler is bound there. The guest-facing services (the DNS proxy on
  `gateway:53`, ICMP echo) are meant to be reachable; a host management
  service bound to `0.0.0.0`, a debug listener, a test harness socket, or the
  DNS proxy of *another* guest's tap (bound to *its* gateway, which the host
  also owns) are not — and today there is no way to say so. On a multi-tenant
  host that is the classic escape: the guests cannot see each other, but each
  can see the hypervisor.
- **A guest can lie about who it is to the host.** `ipv4_forward` requires a
  masquerading tap's source to be exactly `<subnet>.15`; `ipv4_input`'s local
  path performs no such check, so a guest may source a datagram to a host
  service as its neighbour, as the uplink, or as the host itself.

## Current implementation

`kernel-services/network/ipv4.c`, `ipv4_input`:

- A datagram not addressed to the host is forwarded if the ingress is
  `NETIF_FORWARD` (`ipv4.c:583`); one that is addressed to the host is offered
  to `nat_in` first (`ipv4.c:599`) — a masqueraded reply, an ICMP error
  quoting one, or an inbound DNAT is rewritten and re-emitted there and never
  reaches host services.
- Everything else addressed to the host (and every broadcast) is trimmed and
  demuxed: `icmp_input` / `udp_input(nif, m, hdr, NULL)` / `tcp_input(...)`
  (the `switch (hdr->proto)` that follows `:599`). **There is no verdict on
  this path and no source check**: the anti-spoof (`ipv4.c:429-441`,
  `fwd_spoofed`) runs only in `ipv4_forward`.

`kernel-services/network/fw.c` (the firewall) knows two directions,
`TO_UPLINK` and `TO_GUEST`, decided by the egress interface in
`ipv4_forward`; its rule engine (per-guest ordered list, first match, a
default per direction), attachment registry, control opcodes and listing are
all direction-parametrised — nothing in them is FORWARD-specific except the
call site and the two-entry policy array.

The guest-facing services and how they are reached:

- **DNS proxy** (`tapsvc.c:474-478`): a ksock bound to *that tap's gateway*
  `:53`, reached through `ipv4_input` → `udp_input`. Must stay reachable.
- **ICMP echo** to the gateway (`icmp_input`): the documented
  `ping 10.0.3.1` reproduction. Must stay reachable.
- **DHCP** (`tapsvc.c:211`, `dhcp_filter` installed by `tap_set_input_filter`
  at `:536`): answered at the *frame* level before the stack, so it never
  enters `ipv4_input` and is unaffected by anything on this path.

Every existing selftest that sends guest→host traffic targets `:53`
(`net-dns`, `nettest.c:3272-3350`); none reaches another host port from a
tap, so a default that admits DNS and echo leaves the suite unchanged.

## Why it matters

- **It is the other half of the firewall.** Guest↔guest and guest↔world are
  governed; guest↔host is not. A policy engine that lets a tenant reach the
  hypervisor's sockets has not finished its job.
- **It closes a spoofing hole the forwarding path already closed.** The same
  rule applied in the same shape — no new mechanism, one more call site.
- **It is cheap on the engine that exists.** A third direction value, a third
  policy slot, one verdict call, two seeded rules; the control ABI grows by a
  value and a byte.

## Proposed design

### 1. `TO_HOST`: the INPUT chain as a third direction

The FORWARD chain's direction is decided by the egress (`TO_GUEST` when `out`
is another forwarding tap, else `TO_UPLINK`). A datagram delivered locally
has a third destination: **the host**. `enum fw_dir` gains `FW_DIR_TO_HOST`
(and the UAPI `COSMO_NETCTL_DIR_TO_HOST = 3`); a guest's policy array gains a
third slot. A rule with `direction == TO_HOST` (or `ANY`) is evaluated at the
local-delivery site; `TO_UPLINK`/`TO_GUEST` rules are evaluated in
`ipv4_forward` as now. One list per guest, one first-match walk, one
identity (the match tuple), one `at_index` ordering — the INPUT chain is not
a second engine, it is the same engine consulted from a second place.

Match fields keep their meaning: `proto`; `dst_ip/dst_prefix` constrains
*which host address* (the gateway `/32`, the uplink address, or any);
`dst_port` the host service. A datagram to *any* address the host owns takes
the `TO_HOST` path — a guest reaching the host's uplink address is as much
"the host" as its gateway.

### 2. Enforcement: one call in `ipv4_input`, after NAT declines

`fw_input_verdict(nif, m, iph, ihl)` is called for a datagram that arrived on
a **guest tap** (`nif->flags & NETIF_MASQUERADE`) and is about to be delivered
locally — unicast to one of our addresses *after* `nat_in` has declined it,
or a broadcast — immediately before the trim-and-demux. Loopback and uplink
ingress are not consulted (see §5). On `FW_DROP` the datagram is freed and
`ip_stats.in_filtered` counted.

The verdict does two things, in order:

1. **Anti-spoof, the forwarding rule applied locally.** A guest tap's source
   must be exactly `<subnet>.15` (`TAPSVC_GUEST_HOST`); anything else is
   dropped and counted `in_spoofed` before any rule is consulted. This is the
   check `ipv4_forward` already makes (`ipv4.c:436-441`), now made on both
   paths a tap datagram can take. (A tap datagram that is neither forwarded
   nor for the host is already dropped as unroutable.)
2. **The guest's `TO_HOST` rules, first match, else its `TO_HOST` default.**

No flow state is needed for this chain: the host's *reply* to a guest leaves
by `ipv4_output` → `output_on` → the tap and passes no filter (an OUTPUT
chain is a later unit), and a guest's later segments on an accepted
connection (its ACKs, its data) match the same `TO_HOST` rule by destination
port, so per-datagram rule matching is sufficient and stateless. A guest
sending a bare ACK to an unruled host port is dropped, as it should be.

### 3. The default: what the tap offers, as rules, not holes

`TO_HOST` default policy is **DROP**. So that a stock guest keeps working
without operator action, `fw_guest_attach` **seeds two rules** in the
guest's list — visible in the listing, ordered, deletable, identical in
shape to any operator rule:

- `TO_HOST udp dst <gateway>/32 port 53 ACCEPT` — the tap's DNS proxy;
- `TO_HOST icmp dst <gateway>/32 ACCEPT` — echo to the gateway.

(DHCP needs nothing: it is frame-level.) Seeding rules rather than
hard-coding exceptions keeps the model honest — an operator who wants to
lock a guest out of the resolver deletes the rule by tuple, and the listing
tells the truth about what is open. `fw_flush` re-seeds them (it resets a
guest to "as attached"). A guest reaching the host's *uplink* address, or any
other host port, is dropped by default; a rule opens it.

### 4. The control plane: one value, one byte, version 3

`kernel/include/uapi/cosmo/netctl.h`: `COSMO_NETCTL_DIR_TO_HOST = 3` for
`FILTER_ADD`/`FILTER_DEL`/`FILTER_POLICY`; `struct cosmo_netctl_filter_guest`
spends one byte of its `reserved` pair on `policy_to_host` (layout and size
unchanged); `COSMO_NETCTL_VERSION` → 3 (a v2 reader that ignores the byte
sees zeros where it saw zeros). `vmctl filter` accepts `host` as a direction
word; `vmctl filter list` prints the third policy. `/dev/net/tapctl`'s
dispatcher, exact-size discipline, guest-by-address binding, attachment
coherence and snapshot bound (`COSMO_NETCTL_SNAPSHOT_MAX`, unchanged since the
seeded rules fit within `FW_RULES_PER_GUEST`) all carry over.

### 5. Deliberately out of scope (named, later units)

- **The host's exposure to the uplink** — a host-scoped INPUT chain for
  datagrams arriving on the NIC. It is not per-guest, so it needs a
  host-level policy object and control surface (not `/dev/net/tapctl`), and
  it interacts with DNAT ordering; a unit of its own.
- **An OUTPUT chain** for the host's own egress (and, with it, filtering the
  host's replies to guests).
- **Loopback** is never filtered (the host talking to itself).
- Rate-limit/log targets, IPv6, full TCP state — as before.

## Affected files

- `kernel/include/kernel/net/fw.h` — `FW_DIR_TO_HOST`; policy array to 3;
  `enum fw_verdict fw_input_verdict(struct netif *nif, struct mbuf *m, const
  struct ipv4_hdr *iph, unsigned ihl);` `fw_policy_get` gains `to_host`;
  `fw_stats` gains `in_accept_rule/in_accept_default/in_drop_rule/
  in_drop_default/in_spoofed`.
- `kernel-services/network/fw.c` — the verdict (anti-spoof + `TO_HOST`
  walk), `rule_valid` accepts the new direction, `fw_guest_attach`/
  `fw_flush` seed the two default rules, `fw_policy_set/get` handle the third
  slot, `dir_slot` maps it.
- `kernel-services/network/ipv4.c` — the call after `nat_in` declines, for
  `NETIF_MASQUERADE` ingress; `ip_stats.in_filtered`, `in_spoofed`.
- `kernel/include/kernel/net/ip.h` — the two stats.
- `kernel/include/uapi/cosmo/netctl.h` — `DIR_TO_HOST`, `policy_to_host`,
  version 3.
- `kernel-services/network/tap.c` — the read snapshot emits `policy_to_host`
  (no dispatch change: the direction value flows through).
- `userland/system/vmctl.c` — `host` direction word; list prints the third
  policy.
- `kernel-services/network/nettest.c`, `kernel/core/selftest.c`,
  `kernel/include/kernel/selftest.h` — the `net-input` selftest;
  `net-firewall`'s listing assertions account for the seeded rules
  (`rule_count >= 3` becomes `>= 5`, the rule-count check in step (6) counts
  from the seeded baseline).
- `docs/kernel-services/network/design.md` ("A forwarding firewall" gains the
  INPUT chain; the "guest-to-host is unfiltered" statements swept),
  `testing.md`, `README.md` Status entry; this report converted to as-built.

## New APIs

- In-kernel: `FW_DIR_TO_HOST`; `enum fw_verdict fw_input_verdict(struct netif
  *nif, struct mbuf *m, const struct ipv4_hdr *iph, unsigned ihl);` `int
  fw_policy_get(uint32_t guest_ip, uint8_t *to_uplink, uint8_t *to_guest,
  uint8_t *to_host);` — everything else (`fw_rule_add/del/list`,
  `fw_policy_set`, attach/purge) is unchanged and simply accepts the new
  direction.
- UAPI: `COSMO_NETCTL_DIR_TO_HOST`, `cosmo_netctl_filter_guest.policy_to_host`,
  `COSMO_NETCTL_VERSION 3`. No new opcode, no new struct, no new syscall.

## Migration plan

1. `fw.h`/`fw.c`: the third direction and policy slot; `fw_input_verdict`
   with the anti-spoof and the `TO_HOST` walk; seeding in attach/flush.
2. `ipv4.c`: the call site and the two stats; both arches boot, every
   existing test green (DNS and echo pass by the seeded rules; DHCP is
   untouched).
3. UAPI v3, `tap.c` listing byte, `vmctl` direction word.
4. `net-input` selftest; the `net-firewall` count adjustments; docs and
   README; the report as built.

The behaviour change is the `TO_HOST` default: a guest reaching a host
service other than the gateway's DNS and echo is now dropped. It is the
stated purpose of the unit, documented, and a rule (or a policy flip) opens
whatever an operator needs.

## Tests

`net-input` (new), two guests through `/dev/net/tap` as `net-firewall` does,
plus ksock listeners on the host to prove delivery or its absence:

- **Seeded services reach the host**: a guest DNS query to `gateway:53` is
  answered (the proxy still works: `net-dns`'s round trip, repeated here);
  a guest echo request to the gateway draws an echo reply.
- **Everything else is closed by default**: a host UDP listener on
  `gateway:7000` (and a TCP listener) receives nothing from the guest
  (`in_drop_default` rises); the guest addressing the host's **uplink**
  address is dropped too.
- **A rule opens a service**: `TO_HOST udp gateway/32 7000 ACCEPT` → the
  listener receives the datagram (`in_accept_rule` rises); a TCP rule admits
  a SYN *and* the guest's subsequent ACK (stateless per-datagram match).
- **The seeds are real rules**: deleting the seeded DNS rule by tuple makes
  the next DNS query drop (and `net-dns`'s behaviour is restored by
  `fw_flush`'s re-seed); the listing shows both seeds with indices 0 and 1
  and `policy_to_host == DROP`.
- **Anti-spoof on the local path**: a datagram from a guest tap sourced as
  `10.0.3.50` (or as the host itself, or as the neighbour) toward
  `gateway:53` is dropped with `in_spoofed`, though a rule would admit that
  port from the real guest.
- **Per-guest**: A's `TO_HOST` rule does not open B's path; purge on release
  removes A's `TO_HOST` rules and policy; a reopened A re-seeds cleanly.
- **Policy flip**: `FILTER_POLICY TO_HOST ACCEPT` admits an unruled port; back
  to DROP closes it. `vmctl`-shaped writes with `host` are accepted;
  `ANY` as a policy direction is rejected.
- **Regression**: `net-dns`, `net-dhcp`, `net-nat`, `net-firewall` unchanged
  in verdict (counts adjusted for the seeds).

Bug-proofs: a verdict that always accepts (the closed port then receives);
a missing anti-spoof (the forged source then reaches the listener); seeds
implemented as hard-coded exceptions (deleting the DNS rule then changes
nothing); the call site placed *before* `nat_in` (a masqueraded reply is
then wrongly subjected to `TO_HOST` and dropped — `net-nat`'s reply fails).

## Benchmarks

The verdict adds one rule walk per host-bound guest datagram (the DNS proxy
path gains ~2 rule comparisons); measured as before with the `net-bench`
style loop over guest→gateway UDP. Nothing on the uplink or loopback paths
changes.

## Risks

- **Guests losing a host service they used.** Only DNS and echo stay open;
  any other guest→host use (none exists in the tree) needs a rule.
  Mitigation: documented as the unit's purpose; a rule or a policy flip is
  one `vmctl filter` command.
- **The anti-spoof breaking an existing test.** Every guest→host injection in
  the suite sources from the guest's `.15`; a survey precedes the change.
- **Call-site ordering.** Placing the verdict before `nat_in` would filter
  NAT'd replies as host-bound traffic (they are not). Mitigation: after
  `nat_in` declines, and a bug-proof asserts it (`net-nat`'s masqueraded
  reply must still reach the guest).
- **Seeded rules and the per-guest cap.** Two of `FW_RULES_PER_GUEST` (32)
  are taken at attach; `-ENOSPC` arrives two rules earlier. Acceptable and
  documented.
- **A v2 reader of the listing.** The third policy lives in a formerly
  reserved byte; a v2 reader ignores it. The version bump signals the
  semantic change.

## Alternatives considered

- **Hard-code DNS and echo as exceptions in the verdict.** Rejected: an
  invisible hole an operator cannot see, list or close; seeded rules keep
  the listing truthful and the model uniform.
- **A separate INPUT engine / rule list.** Rejected: the existing engine is
  direction-parametrised; a third direction value reuses identity, ordering,
  attachment, control and listing unchanged.
- **Filter at the socket layer (per-socket "accept from guests" flag).**
  Rejected: it scatters policy across every service and cannot express
  per-guest or per-address rules; the network layer already has the guest's
  identity (its tap) and the engine.
- **Include the uplink→host chain in this unit.** Rejected for scope: it is
  host-scoped, not per-guest, needs its own control surface, and interacts
  with DNAT; named as the next unit.
