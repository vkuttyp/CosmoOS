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

### 2. Stateful return traffic — which direction actually needs filter state

A correct reading of the reply paths (verified against `nat.c`) splits the
problem in two, and only one half needs the filter's own state:

- **guest→uplink (masqueraded).** The reply is addressed to the host's
  uplink address, so `ipv4_input` hands it to `nat_in`, which un-translates
  it and delivers it to the guest via `nat_forward_to` → `ipv4_output`
  (`nat.c:444`, `nat.c:592`) — it **does not** re-enter `ipv4_forward`, so
  `fw_forward_verdict` never sees it. That is safe and wanted: a masqueraded
  reply exists only because a matching NAT conntrack entry exists, and that
  entry was created only when the guest's outbound NEW flow was **accepted**
  by `fw_forward_verdict` on the way out. **The NAT conntrack entry is the
  stateful evidence for this direction** — no reply rule and no filter flow
  entry are needed; `nat_in` already drops a reply with no matching entry.
  (An earlier draft of this report wrongly claimed the reply re-traverses
  `ipv4_forward`; it does not, and the design does not rely on it.)
- **guest→guest (un-NAT'd).** Both the request and the reply traverse
  `ipv4_forward` (neither is translated). This is the **only** direction in
  which the firewall must carry its own state: an accepted NEW A→B flow
  records a flow entry, and B→A matching the reverse of that entry is
  **ESTABLISHED** and accepted without a reverse rule. Conntrack cannot serve
  here because un-NAT'd guest→guest flows are absent from it.
- **inbound DNAT (client→guest).** Authorized by the port-forward rule, and
  also delivered via `nat_in` → `ipv4_output`, not the FORWARD chain — so the
  FORWARD filter neither gates nor needs to gate it; the pf rule is its
  policy. (Filtering inbound-to-guest beyond the pf rule is a later INPUT-
  style refinement.)

So the filter keeps **its own** bounded flow table used for the guest→guest
direction, with the short/long idle timeouts conntrack already defines,
reclaimed by the same periodic tick as `nat_age`. Flow-key semantics by
protocol:

- **TCP/UDP** — the 5-tuple `(proto, src_ip, src_port, dst_ip, dst_port)`;
  the reply is the swapped tuple. TCP state is coarse (NEW vs
  established-by-ACK, as `nat_entry.tcp_est` already is); no full TCP state
  machine in this unit.
- **ICMP** — only **echo** is stateful, keyed on `(src_ip, dst_ip, echo id)`
  with the request being type 8 and its reply type 0 carrying the **same
  echo identifier** (exactly what NAT already tracks in `orig_port` for ICMP
  echo). A B→A echo *request* is not a reply and never matches A→B echo
  state; non-echo ICMP is not stateful-tracked and is governed by rules /
  default only. This prevents unrelated B→A ICMP being classified
  ESTABLISHED.

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

- `FILTER_ADD` — add a rule. The command struct names the guest explicitly
  in its payload (a `guest_addr` field, exactly as `FORWARD_ADD` already
  does), the direction, proto, dst addr/prefix, dst port, and verdict. The
  **kernel assigns a stable rule id** (a small monotonic counter per guest,
  never reused while the rule lives) and returns it to the caller (the write
  yields the id, and it also appears in the read-listing). Ordering:
  `FILTER_ADD` **appends** by default (evaluation order = insertion order);
  an optional `before_id` field inserts ahead of an existing rule for
  deterministic placement, `0` meaning append.
- `FILTER_DEL` — remove the rule with a given `(guest_addr, rule_id)`.
- `FILTER_POLICY` — set the default policy (`ACCEPT`/`DROP`) for a
  `(guest_addr, direction)`.
- The read snapshot grows a filter section — a second versioned list after
  the port-forward list (selected by a field in the read request) — emitting
  each rule **with its id, in evaluation order**, plus the current default
  policy per direction, so an operator reads back exactly what is installed
  and can delete or re-order by id.

**Guest binding — the same model the forwards use, not a handle-bound one.**
`/dev/net/tapctl` carries no per-open guest state, and `vmctl` opens a fresh,
transient handle per command (so a handle-scoped rule would be discarded the
moment `vmctl` exits). A rule therefore identifies its guest **by address in
the payload**, validated the way `nat_pf_add` validates a forward target —
the address must be a live guest tap (`NETIF_FORWARD`), so a command cannot
install policy for a non-existent or non-guest address, and rules survive the
control handle closing. Teardown keys on that address: the tap's `release`
calls `fw_guest_purge(guest_ip)` beside `nat_guest_purge`, removing only that
guest's rules, policy and flow state. `vmctl` grows `filter add|del|list` and
`filter policy` subcommands mirroring `port-forward`, each taking the guest
address as `port-forward` does.

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
  `tap_ctl_write`; extend the `tap_ctl_read` snapshot; validate each rule's
  payload `guest_addr` against a live guest tap (as `nat_pf_add` does).
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
  `int fw_rule_add(uint32_t guest_ip, uint32_t before_id, const struct fw_rule
  *r);` — **returns the assigned rule id (>0) or -errno**; `before_id == 0`
  appends, else inserts ahead of that id. `int fw_rule_del(uint32_t guest_ip,
  uint32_t rule_id);` `unsigned fw_rule_list(uint32_t guest_ip, struct fw_rule
  *out, unsigned max);` — fills each `fw_rule.id` in evaluation order.
  `int fw_policy_set(uint32_t guest_ip, uint8_t direction, uint8_t verdict);`
  `void fw_age(uint64_t now_ns);` `void fw_flush(void);` `void
  fw_guest_purge(uint32_t guest_ip);` `void fw_get_stats(struct fw_stats *);`
  `struct fw_rule` carries `{ id, direction, proto, dst_ip, dst_prefix,
  dst_port, verdict }`; the id is kernel-assigned (a per-guest monotonic
  counter), so the caller never supplies one on add and always has one to
  delete/re-order by.
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
- **Stateful return (guest→guest, the direction that needs filter state)**:
  B's TCP/UDP reply to an accepted A→B flow reaches A with no reverse rule;
  an *unsolicited* B→A packet (no flow, no rule) is dropped.
- **NAT'd reply needs no filter state**: an accepted guest→uplink flow's
  masqueraded reply reaches the guest (it is delivered by `nat_in`, gated by
  the NAT conntrack entry, not by the FORWARD chain); a reply with no NAT
  entry is dropped by `nat_in` as today.
- **ICMP echo state, not a bare reverse tuple**: an accepted A→B echo request
  (type 8, id X) admits B's echo reply (type 0, id X); a B→A echo *request*
  (type 8) after it is still dropped (it is not a reply), and an echo reply
  with a different id does not match.
- **Rules are bound by payload address, not by handle**: a rule added for
  guest A (named in the payload) filters only A's traffic and **survives the
  control handle closing** (added, handle closed, then the next A→B packet
  still obeys it); `fw_guest_purge` on A's tap release removes only A's
  rules/policy/flows, B's untouched.
- **Rule identity and ordering**: `FILTER_ADD` returns a stable id; the read
  snapshot lists rules with their ids in evaluation order; a second add with
  `before_id` lands ahead of the named rule (a later-listed overlapping
  DROP before an ACCEPT changes the verdict); `FILTER_DEL` by id removes
  exactly that rule and the rest keep their order.
- **Control round-trip and rejection**: `FILTER_POLICY` flips a default and
  the next packet's verdict follows; a short write / wrong version /
  non-guest `guest_addr` / unknown `rule_id` on DEL is rejected and changes
  nothing.

Each assertion bug-proofed by reintroducing the defect (e.g. a verdict that
always ACCEPTs → the inter-guest drop test fails; a state table that never
records a flow → the guest→guest stateful-return test fails; an ICMP match
that ignores the echo id → the wrong-id reply is wrongly admitted).

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
- **Filter/NAT ordering subtleties.** The verdict must run on the
  guest-visible tuple, so it is called in `ipv4_forward` **before** `nat_out`
  rewrites the source; calling it after would make rules see translated
  addresses. The NAT'd reply path is deliberately *not* filtered again (it is
  delivered by `nat_in` → `ipv4_output`, and the NAT conntrack entry already
  proves the flow was accepted outbound); the filter's own flow state is only
  for the guest→guest direction, where both halves traverse `ipv4_forward`.
  A bug-proof asserts a guest→uplink reply is delivered without a filter flow
  entry, and a guest→guest reply is admitted by one.
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
