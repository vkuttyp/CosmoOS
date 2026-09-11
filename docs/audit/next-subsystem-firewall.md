# NEXT SUBSYSTEM — a stateful packet-filter firewall for the guest taps

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This is that report, and
nothing in it is implemented.

**Subsystem: a stateful FORWARD-chain firewall over the guest taps. An
ordered rule list and a per-direction default policy decide whether a
forwarded datagram is accepted or dropped; a bounded flow table makes the
filter stateful, so the reply to an accepted flow is accepted without a
reverse rule. The default closes the gap the multi-guest unit deferred in
writing — inter-guest traffic is dropped unless a rule allows it — while
guest→uplink egress stays open as it is today. Rules are set at runtime over
the existing `/dev/net/tapctl` control channel, alongside the port-forward
rules it already carries. This is the multi-guest arc's policy unit: the tap
made many guests reachable to each other; this lets an operator say who may
reach whom.**

## Problem

The multi-guest unit (PR #101) gave every VM its own tap, subnet and NAT
share, and routes guests to each other as adjacent-subnet machines —
"connectivity, not visibility". Its report and the network design doc both
name, as a deferred later unit, **a policy forbidding inter-guest traffic**.
Today there is none: a guest on `10.0.3.15` can reach a guest on
`10.0.4.15` with no restriction, and a guest can reach anything its routes
resolve to. The only packet-level control in the stack is the DNAT
port-forward table (what inbound connections reach a guest); there is no way
to say "this guest may talk to the uplink but not to its neighbours", or
"only DNS and HTTP may leave this guest", or "drop everything by default".
On a host running more than one tenant's guest, that absence is a real
isolation hole, and it is the single most-requested control a container or
VM host exposes (`iptables`/`nftables`, security groups, network policies).

## Current implementation

`kernel-services/network/ipv4.c`:

- `ipv4_input` (`ipv4.c:521`) hands a datagram not addressed to the host to
  `ipv4_forward` when it arrived on a `NETIF_FORWARD` interface
  (`ipv4.c:570`).
- `ipv4_forward` (`ipv4.c:422`) runs the strict anti-spoof (a masquerading
  tap requires source `<subnet>.15`, `ipv4.c:436`), decrements the TTL,
  routes (`ipv4_route`), drops a hairpin/route-less datagram, then either
  masquerades (`nat_out`, guest→uplink, `ipv4.c:491`) or — for a flow
  leaving another forwarding tap (guest→guest) — forwards the source
  unchanged, and finally re-emits with `output_on`. **There is no accept/drop
  decision anywhere on this path**: every routable, non-spoofed datagram is
  forwarded.
- Inbound is gated only by DNAT: `nat_in` (`ipv4.c:585`) rewrites a packet
  matching a port-forward rule to the guest; everything else addressed to the
  host is delivered to host services (`udp_input`/`tcp_input`/`icmp_input`,
  `ipv4.c:599`).

`kernel-services/network/nat.c` keeps a conntrack table (`g_nat`,
`NAT_TABLE_SIZE` = 256) of masquerade and DNAT flows (`struct nat_entry`,
`nat.c:37`), keyed on NAT identifiers, with per-entry timeouts and `nat_age`.
It tracks only NAT'd flows — **guest→guest traffic, being un-NAT'd, is in no
table at all** — and its keys are NAT identifiers, not a direction-tagged
5-tuple, so it is not by itself a filter state table.

`/dev/net/tapctl` (`tap.c:249`, ABI `kernel/include/uapi/cosmo/netctl.h`)
carries exactly two opcodes today — `FORWARD_ADD`/`FORWARD_DEL` — dispatched
in `tap_ctl_write` to `nat_pf_add`/`nat_pf_del`, with a versioned read
snapshot built from `nat_pf_list`. It is the natural place to add filter
rules: a guest-scoped, privileged (`0600`), versioned control surface that
already exists.

## Why it matters

- **It closes a hole the codebase already promised to close.** Two docs name
  inter-guest policy as this unit; until it exists, "multi-guest" means
  "mutually reachable guests", which is not what a multi-tenant host wants.
- **It is the backbone of every later network-policy feature.** Egress
  filtering, per-guest allow-lists, and a DMZ between guests are all rules on
  the same engine; shipping the engine once is the leverage.
- **It is observable and testable end to end.** A forwarded frame either
  arrives on the peer tap or does not; a verdict is a single accept/drop with
  a counter, so each rule and the stateful return path can be bug-proofed by
  injection exactly as `net-forward`/`net-dnat` are.

## Proposed design

### 1. The filter model

A single **FORWARD chain** evaluated in `ipv4_forward`, after anti-spoof and
routing and *before* the NAT/output step. The decision is made on the
datagram as the guest sends it (post-route, pre-masquerade 5-tuple), so rules
are written in guest-visible terms regardless of any later source rewrite.

A rule matches on: **ingress guest** (the tap's guest address, or "any"),
**direction** (`TO_UPLINK` — the egress is not a forwarding tap; `TO_GUEST` —
the egress is another guest tap; `ANY`), **proto** (TCP/UDP/ICMP/any),
**destination address + prefix**, and **destination port** (or "any"; ignored
for ICMP). The verdict is **ACCEPT** or **DROP**. Rules are an ordered list,
first match wins; a datagram matching no rule takes the **default policy** for
its direction.

Default policy, chosen to close the gap without changing today's working
paths:

- `TO_GUEST` (inter-guest) default **DROP** — the deferred policy, now the
  default.
- `TO_UPLINK` default **ACCEPT** — preserves current guest→internet egress.

An operator tightens or loosens either default and inserts rules (e.g.
"guest A → guest B tcp/445 ACCEPT" to punch one hole in the inter-guest
deny, or "guest A → any udp/53 ACCEPT" then "guest A → any DROP" to confine a
guest to DNS).

### 2. Stateful return traffic

The filter is **stateful**: a NEW forwarded flow that a rule (or the default)
accepts records a flow entry keyed on the connected 5-tuple + ingress; its
reply — the reverse 5-tuple arriving on the forward path — matches as
**ESTABLISHED** and is accepted without a reverse rule, the universal
expectation of a stateful firewall. The filter keeps **its own** bounded
flow table (it is not NAT's job, and guest→guest flows are absent from
conntrack); entries carry the short/long idle timeouts conntrack already
defines and are reclaimed by the same periodic `nat_age` tick the network
worker runs. TCP state is coarse (NEW vs established-by-ACK, as `nat_entry.
tcp_est` already is); a first cut need not track full TCP state machines.

Interaction with NAT, made explicit: a masqueraded guest→uplink reply
re-enters through `nat_in`, is un-translated back to the guest 5-tuple, and
then (routed to the tap) traverses `ipv4_forward` again — where the filter
sees the guest-visible reverse tuple and matches the ESTABLISHED entry. The
filter therefore reads the same tuple for a flow's request and its reply, so
one flow entry covers both.

### 3. Where the verdict is enforced

One function, `fw_forward_verdict(in, out, m, iph, ihl)`, called from
`ipv4_forward` immediately after the egress interface is chosen (so direction
is known from `out->flags & NETIF_FORWARD`) and before `nat_out`. On DROP it
bumps a counter and the caller frees `m` and returns (the existing
`fwd_*`-drop shape); on ACCEPT the packet continues unchanged. Scope for this
unit is the **FORWARD chain only** — host-local delivery (an INPUT chain
protecting the host's own services) and egress from the host itself are named
but deferred, keeping the unit to the guest-isolation problem it solves.

### 4. The control plane

Extend `/dev/net/tapctl` and `kernel/include/uapi/cosmo/netctl.h` with, under
a bumped `COSMO_NETCTL_VERSION`:

- `FILTER_ADD` / `FILTER_DEL` — add/remove a rule (a versioned
  `struct cosmo_netctl_filter` carrying direction, proto, dst addr/prefix,
  dst port, verdict, and a stable rule id or an explicit position for
  ordering).
- `FILTER_POLICY` — set the default policy for a direction.
- The read snapshot grows a filter-rule section (a second versioned list
  after the existing port-forward list, or a selector in the request), so an
  operator reads back exactly the installed rules and policy.

Rules are **guest-scoped** as the forwards are: a rule added through one
guest's control handle binds to that guest's tap (its ingress), so one
tenant cannot write another's policy. `vmctl` grows `filter add|del|list`
and `filter policy` subcommands mirroring `port-forward`.

### 5. Deliberately out of scope (named, later units)

- An **INPUT chain** filtering traffic to the host's own services, and an
  **OUTPUT chain** for the host's own egress.
- **L3/L4 richness** beyond first-match accept/drop: rate limits, logging
  targets, connection-count limits, NAT-before-filter ordering knobs.
- **IPv6** filtering (tracks the deferred IPv6 NAT/DNAT units).
- **Full TCP state tracking** (window/sequence validation); this unit's state
  is NEW-vs-established only.

## Affected files

- `kernel/include/uapi/cosmo/netctl.h` — bump `COSMO_NETCTL_VERSION`; add the
  `FILTER_ADD`/`FILTER_DEL`/`FILTER_POLICY` opcodes, `struct
  cosmo_netctl_filter`, and the filter read-listing structs.
- `kernel/include/kernel/net/fw.h` (new) — the in-kernel filter API
  (`fw_forward_verdict`, `fw_rule_add`/`del`/`list`, `fw_policy_set`,
  `fw_age`, a `struct fw_stats`).
- `kernel-services/network/fw.c` (new) — the rule list, the default policy,
  the bounded stateful flow table, and the verdict function.
- `kernel-services/network/ipv4.c` — call `fw_forward_verdict` in
  `ipv4_forward`; a dropped datagram is freed and counted.
- `kernel-services/network/tap.c` — dispatch the new opcodes in
  `tap_ctl_write`; extend the `tap_ctl_read` snapshot; scope rules to the
  opening handle's guest.
- `kernel-services/network/nat.c` / the network worker — call `fw_age` from
  the same periodic sweep as `nat_age`.
- `kernel-services/network/nettest.c`, `kernel/core/selftest.c`,
  `kernel/include/kernel/selftest.h` — the `net-firewall` selftest.
- `bin/vmctl` (or wherever `port-forward` lives) — `filter` subcommands.
- `docs/kernel-services/network/design.md`, `.../testing.md`, `README.md`
  Status — the new "A forwarding firewall" sections and Status bullet.

## New APIs

- In-kernel (`fw.h`): `enum fw_verdict { FW_ACCEPT, FW_DROP };`
  `fw_verdict fw_forward_verdict(struct netif *in, struct netif *out, struct
  mbuf *m, const struct ipv4_hdr *iph, unsigned ihl);`
  `int fw_rule_add(uint32_t guest_ip, const struct fw_rule *r);`
  `int fw_rule_del(uint32_t guest_ip, uint32_t rule_id);`
  `unsigned fw_rule_list(uint32_t guest_ip, struct fw_rule *out, unsigned
  max);` `int fw_policy_set(uint32_t guest_ip, uint8_t direction, uint8_t
  verdict);` `void fw_age(uint64_t now_ns);` `void fw_flush(void);`
  `void fw_guest_purge(uint32_t guest_ip);` `void fw_get_stats(struct fw_stats
  *);`
- UAPI (`netctl.h`): `COSMO_NETCTL_FILTER_ADD/DEL/POLICY`, `struct
  cosmo_netctl_filter`, `struct cosmo_netctl_filter_rule`, a bumped version.
- No new system call — the control channel is the surface, as the netctl and
  multi-guest units established.

## Migration plan

1. `fw.h` + `fw.c`: the rule list, default policy (inter-guest DROP,
   to-uplink ACCEPT), and stateless verdict; wire `fw_forward_verdict` into
   `ipv4_forward`; `fw_guest_purge` called from the tap's `release` beside
   `nat_guest_purge`.
2. The bounded stateful flow table + ESTABLISHED matching; `fw_age` on the
   periodic tick.
3. The `/dev/net/tapctl` opcodes and read-listing (version bump), guest-
   scoped; `vmctl filter` subcommands.
4. `net-firewall` selftest; docs + README Status entry.

Each step builds, boots and passes on both arches before the next. The
default policy change (inter-guest DROP) is the one behaviour change to
existing setups; it is documented and is exactly the deferred policy. A guest
that needs to reach a neighbour gets an explicit rule.

## Tests

`net-firewall` (new), driven by frame injection as `net-forward`/`net-dnat`
are (`tap_inject`/`tap_recv`, `nettest_mk_udp/tcp`, `nettest_wrap`, stats via
`fw_get_stats`/`ipv4_get_stats`):

- **Default inter-guest DROP**: a datagram from guest A to guest B is dropped
  (never read back on B's tap; the drop counter rises) with no rule.
- **An allow rule punches one hole**: "A→B tcp/445 ACCEPT" — that flow
  reaches B; A→B on another port/proto still drops.
- **To-uplink default ACCEPT unchanged**: a guest→uplink flow forwards as it
  does today.
- **Stateful return**: B's reply to an accepted A→B flow reaches A with no
  reverse rule; an *unsolicited* B→A packet (no flow, no rule) is dropped.
- **Guest-scoped rules**: a rule added on A's handle does not filter B's
  traffic; `fw_guest_purge` on close removes only A's rules/flows.
- **Control round-trip**: `FILTER_ADD` then the read snapshot lists exactly
  that rule; `FILTER_DEL` removes it; `FILTER_POLICY` flips a default and the
  next packet's verdict follows; a short write / wrong version / off-guest
  target is rejected and changes nothing.

Each assertion bug-proofed by reintroducing the defect (e.g. a verdict that
always ACCEPTs → the inter-guest drop test fails; a state table that never
records a flow → the stateful-return test fails).

## Benchmarks

`net-nicbench`-style: forwarded-packet throughput with an empty rule list
(the default-policy fast path) and with a modest rule list, to show the
verdict is O(rules) per NEW packet and O(1) (a flow-table hit) for
ESTABLISHED, and that the added per-packet cost is a bounded table walk. The
stateful table's memory is `sizeof(flow) × FW_FLOW_MAX`, a fixed budget like
the NAT table's.

## Risks

- **Breaking existing guest→guest setups.** The default flips inter-guest to
  DROP. Mitigation: it is the documented, deferred policy; guest→uplink (the
  common path) is unchanged; an explicit rule restores any needed
  reachability. Called out in the README and design doc.
- **Stateful-table exhaustion / DoS.** A guest opening endless flows could
  fill the flow table. Mitigation: a per-guest quota as the NAT table has
  (`NAT_QUOTA_PER_GUEST`'s sibling), so one guest starves only itself.
- **Filter/NAT ordering subtleties.** Evaluating on the guest-visible tuple
  (pre-masquerade, post-un-NAT) is what makes request and reply share a flow
  entry; getting the call site wrong (after `nat_out` rewrote the source)
  would make rules see translated addresses. Mitigation: the verdict is
  called before `nat_out`, and the reply is filtered after `nat_in`
  un-translates — a specific bug-proof asserts a masqueraded flow's reply is
  accepted as ESTABLISHED.
- **Lock ordering.** A new `g_fw_lock` joins `g_pf_lock`/`g_nat_lock`; it must
  take a defined place in the order (the filter runs before NAT, so
  `g_fw_lock` → `g_nat_lock`) and be lockdep-clean, as the netctl TOCTOU fix
  taught.

## Alternatives considered

- **Reuse the NAT conntrack as the filter's state table.** Rejected: conntrack
  holds only NAT'd flows (guest→guest is absent) and is keyed on NAT
  identifiers, not a direction-tagged 5-tuple; bolting filter state onto it
  would entangle two concerns and still miss the un-NAT'd flows.
- **A generic Berkeley-packet-filter / eBPF-style program per tap.** Rejected
  for a first unit: far more surface (a bytecode verifier, a VM) than the
  isolation problem needs; a fixed match/verdict rule is enough and is what
  an operator reasons about.
- **Only a boot-time `fw_cfg` rule set, no runtime channel.** Rejected: the
  netctl unit already established a runtime control plane, and a firewall an
  operator cannot change on a running host is half a firewall.
- **A full `nftables`-shaped multi-chain/table engine now.** Rejected as
  over-scoped; the FORWARD chain with a default policy and ordered rules is
  the minimum that closes the deferred gap, and INPUT/OUTPUT chains and
  richer targets are named follow-ups on the same engine.
