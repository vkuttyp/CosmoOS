# NEXT SUBSYSTEM — the host's own flows: reply state for the host chain

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This is that report, and
nothing in it is implemented.

**Subsystem: state for the host chain, so that `policy host world drop` is
a configuration a host can run rather than one that cuts its own throat.
The host chain (PR #107) shipped with default ACCEPT because it had no reply
state: a hardened default, or a broad UDP or ICMP DROP, would drop the
replies to the host's own unconnected UDP sockets — first among them the
DNS proxy's upstream socket, which is unconnected *by design* so it can
authenticate the sender — and every ICMP the host is owed: echo replies to
its own requests and the Need-Fragmentation errors its TCP path-MTU
discovery lives on. This unit gives the chain the state those need, in the
shape the stack already has: the FORWARD chain's flow table (`fw.c`)
records a NEW flow at the point it is accepted and admits the reply by the
reverse tuple; the host's flows are recorded at the one place every
host-originated datagram passes, `ipv4_output`, when the egress is a real
link — UDP by its ports, ICMP echo by its identifier — and
`fw_host_verdict` consults that state **before** its rules, as
`fw_forward_verdict` does. ICMP *errors* are not modelled by the firewall
at all: the only one the host consumes, Need-Fragmentation, is already
accepted only when TCP confirms the quoted segment is one of its own in
flight (RFC 5927), so under a DROP verdict an ICMP error is **delivered
quiet** — `M_FW_QUIET`, the mechanism the host chain built — and
`icmp_input` under the flag runs that confirmation and nothing else. The
transport owns acceptability; the firewall adds only silence. No ABI
change, no new syscall: state is not configured, it is earned by what the
host sends.**

## Problem

The host chain's own Risks section put this first: *"A broad UDP/ICMP DROP
breaks the host's own replies. No reply state in this unit."* Concretely,
with `vmctl filter policy host world drop` — the configuration the unit's
design calls "a hardened host" — or with `filter add host world udp any any
any drop`:

- **Every guest loses DNS.** `tapsvc.c`'s proxy relays a guest's query
  upstream from `svc->usock` with `ksock_sendto` (`tapsvc.c:409`) and reads
  the answer from the same socket, which is **unconnected on purpose**
  (`:423-426`: "an unconnected socket accepts packets from anyone, and
  matching on the id alone would let a reachable attacker race a forged
  response into the guest" — the sender is checked by address instead).
  Quiet delivery (`udp.c` `lookup(..., connected_only)`) admits a DROP'd
  datagram only into a socket *connected* to the sender, so the upstream's
  answer is freed (`quiet_dropped`) and the guest's query times out. The
  chain that was built to protect the host's services silences the one
  service every guest depends on.
- **The host cannot ping.** `icmp_send_echo` (`ipv4.c:393-411`) sends an
  echo request; its reply is an ICMP type 0 that matches no rule the
  operator wrote for *inbound* service and is freed at the IP layer
  (`hin_filtered`), so the reply hook (`icmp_echo_reply_hook`, `:387-391`)
  never fires.
- **The host's TCP path-MTU discovery goes blind.** `output_on` sets DF
  (`:205`); a router on the path answers an oversized segment with ICMP
  Need-Fragmentation, which `icmp_input` (`:355-359`) hands to
  `icmp_needfrag` (`:305-336`) — the *only* ICMP error the host consumes,
  and one it consumes carefully: the quoted segment must be one of ours
  (`netif_owns_ipv4(q->src)`, `:313`) *and* `tcp_pmtu_notify` (`:334`,
  `tcp.c:1936`) must find a live connection with that segment in flight
  before `ipv4_pmtu_update` records anything. Under a DROP verdict the
  error is freed before `icmp_input` sees it; the connection's large
  segments are then blackholed, retransmitted at the same size, and time
  out. Silent, and the kind of failure that looks like "the network is
  flaky".
- **Any host client that does not `connect`** — a UDP `sendto` to a peer,
  NTP-shaped or DNS-shaped — never sees its reply.

The FORWARD chain never had this problem because it was built stateful
from the start: `fw_forward_verdict` records an accepted guest-to-guest
NEW flow (`fw.c:461-495`) and admits the reverse tuple as ESTABLISHED
before any rule (`:369-386`), ICMP echo by identifier (`flow_find`,
`:374-397`). The INPUT chain needs none (the host's reply passes no
filter). The host chain is the one chain whose *replies* arrive through
it — and it is the one with no state.

## Current implementation

- **Host egress has one door.** Every datagram the host originates —
  `udp_send` (`udp.c:156`), TCP's `batch_send`, `icmp_send_echo`
  (`ipv4.c:410`), the ICMP errors the host itself sends (`:266`, `:294`,
  `:372`) — calls `ipv4_output(m, src, dst, proto, ttl)` (`ipv4.c:175-186`),
  which routes (`ipv4_route`) and hands the datagram to `output_on`
  (`:188-237`), which builds the header (resolving a zero `src` through
  `ipv4_source_for`, `:190-191`) and transmits. **Forwarded** datagrams do
  not pass `ipv4_output`: `ipv4_forward` calls `output_on` directly
  (`:531`). `nat_in`'s delivery of a masqueraded reply or a DNAT to the
  guest does use `ipv4_output` (`nat.c:454`), but its egress is the guest's
  tap, a `NETIF_MASQUERADE` interface.
- **`fw_host_verdict`** (`fw.c:553`) is stateless: first-match over the
  host's rules, else the host default; on DROP, TCP/UDP is marked
  `M_FW_QUIET` and delivered, anything else — every ICMP — is freed
  (`ipv4.c:639-648`).
- **`icmp_input`** (`ipv4.c:338-378`) consumes exactly two things: Need-
  Fragmentation (to `icmp_needfrag`, TCP-confirmed) and echo requests (it
  answers them, `:360-374`); an echo reply fires the hook (`:375-376`);
  every other message is freed (`:377`). It does not honour `M_FW_QUIET`
  (nothing sets it on ICMP today).
- **The flow table** (`fw.c:65`, `struct fw_flow`): `FW_FLOW_MAX` 256
  entries, keyed by initiator address for a per-guest quota
  (`FW_FLOW_QUOTA_PER_GUEST = 256 / 8 = 32`), so the table is exactly the
  guests' shares with no room for anyone else; `flow_find` matches forward
  and reverse on (proto, a, b, a_port, b_port), ICMP echo request forward /
  reply reverse on the identifier; timeouts are NAT's (`NAT_TIMEOUT_UDP_NS`
  30 s, `_ICMP_NS` 30 s, `_TCP_NS` 30 s / `_TCPEST_NS` 300 s); `fw_age`
  reclaims (`:586`); `fw_guest_purge` drops flows naming a departing
  guest's address; `fw_flush` clears all.
- **Quiet delivery** (PR #107): `M_FW_QUIET` on TCP/UDP; TCP admits only
  what an existing connection accepts, UDP only a socket connected to the
  sender. ICMP has no quiet path.

## Why it matters

- **It makes the host chain usable as a firewall.** A chain whose hardened
  default breaks the host is a chain that stays at ACCEPT; the design that
  shipped promised the operator "drops by source, protocol and port or
  flips to a hardened default", and the flip is not honest until the
  host's own flows survive it.
- **It closes the DNS regression before anyone hits it.** The proxy's
  unconnected upstream socket is a deliberate, documented security choice
  (PR #95); the fix must not be "connect it".
- **It keeps PMTU discovery alive under policy** without teaching the
  firewall TCP — the same principle the host chain settled on after three
  rejected drafts: the transport decides, the firewall adds silence.
- **It reuses what exists.** One flow table, one `flow_find`, one aging
  tick; the host is one more initiator with its own share.

## Proposed design

### 1. The host's flows are recorded where the host's datagrams leave

`fw_host_record(struct netif *out, struct mbuf *m, uint32_t src, uint32_t
dst, uint8_t proto)` is called from **`ipv4_output`** (`ipv4.c:175`), after
`ipv4_route` and before `output_on`, when the egress is a **real,
non-guest link** — `!(out->flags & (NETIF_MASQUERADE | NETIF_LOOPBACK))`,
the complement of the guest taps and the mirror of `fw_host_verdict`'s
ingress test. `ipv4_output` is the door every host-originated datagram
passes and no forwarded one does (`ipv4_forward` transmits through
`output_on`, `:531`); `nat_in`'s deliveries pass it but leave on a tap and
are excluded by the egress test. `m` carries the transport header at
`m->data` (the IP header is built later, in `output_on`), so `l4_read`'s
offset is 0; a zero `src` is resolved as `output_on` resolves it
(`ipv4_source_for(dst)`), so the recorded tuple is what the wire will
carry.

What is recorded, and only this:

- **UDP**: (src, dst, sport, dport) — every send, connected or not; a send
  on a recorded flow refreshes its expiry. Nothing the host sends is
  refused: an unrecordable flow (the host's share is full) is still sent —
  the send path never fails for the firewall's sake — and only its *reply*
  then depends on the rules. Counted `hin_flow_new` / `hin_flow_drop_full`.
- **ICMP echo request** (type 8): (src, dst, identifier), as the FORWARD
  chain records a guest's echo. A reply is admitted by identifier; the
  hook (`icmp_echo_reply_hook`) then fires as today.
- **Nothing else.** TCP is *not* recorded: its segments are already admitted
  by the connection itself under quiet delivery, and recording every
  host TCP send would put a `g_fw_lock` hold and a table scan on the
  uplink's hottest path for no reader (see §3 for why ICMP errors do not
  need it). ICMP errors the host sends, and the host's own ICMP echo
  *replies* (`:372`), are not flows.

The host's flows live in the same table under the initiator key
`FW_HOST_GUEST_IP` (0), so `flow_find`, `fw_age` and `fw_flush` apply
unchanged; `fw_guest_purge` cannot touch them (it matches the departing
guest's address on either side, and the host's addresses are never a
guest's). **The table grows** to make room: `FW_FLOW_MAX` 256 → 320, the
guests' shares unchanged (8 × 32) and the host's `FW_FLOW_QUOTA_HOST` 64
— sized for the DNS proxy's pattern (one upstream socket, one upstream
address: one flow, refreshed per query, however many guests query) with
room for the host's other clients; a flood of distinct host UDP flows
starves only the host's own share, exactly as a guest's flood starves the
guest's.

### 2. `fw_host_verdict` consults state before rules

The order becomes: **(a) state, (b) rules, (c) default.** A datagram that
is the reverse of a recorded host flow — a UDP reply to a recorded (src,
dst, sport, dport), an echo *reply* whose identifier matches a recorded
echo request — is `FW_ACCEPT` outright (`hin_accept_established`), its
flow refreshed; no rule is read. This is the FORWARD chain's rule
(`fw.c:369-386`: "is this half of a flow already accepted?") applied to
the host, and the ordering is the same for the same reason: a reply is
not a new request and no rule was written about it.

What this admits, precisely, is **one tuple**: a UDP datagram *from the
peer address and port the host sent to, to the port the host sent from* —
the reverse of the recorded flow, the same tuple a connected socket would
insist on, now available to an unconnected one for the life of the flow
(30 s idle, refreshed by the host's sends). The firewall cannot know
whether the peer *meant* it as a reply: any datagram on that exact tuple
inside the window is admitted, a second and a third included — that is
what "the host opened this flow" means, and the socket's own validation
(the DNS id, the proxy's sender check) is the second line (see Risks).
It does not admit a datagram from another port at that peer (the proxy's
sender check, `tapsvc.c:426`, is a second line the firewall does not
replace), nor from another host, nor to another local port; those match no
flow and take the rules. The only other match `flow_find` can report is
the *forward* direction — the host's own (src, dst, sport, dport) arriving
inbound — which on a real link can only be a datagram spoofing the host's
address, and `ipv4_input`'s martian check drops it before any chain
(`ipv4.c:573-579`); the state step therefore admits on `reverse` alone,
and a forward match, should the check ever be reached, takes the rules.

### 3. ICMP errors are delivered quiet, and the transport decides

Under a DROP verdict, the IP layer today frees every ICMP. The design
splits ICMP by what the host does with it:

- An **echo reply** is admitted by state (§2) or takes the rules.
- An **echo request** is a *request*, gated by the rules as today (the
  `icmp type 8` selector exists for it).
- An **ICMP error** — Destination Unreachable (3), Time Exceeded (11),
  Parameter Problem (12) — under DROP is marked **`M_FW_QUIET`** and
  delivered, like TCP/UDP (`hin_quiet`); `icmp_input` under the flag runs
  only its Need-Fragmentation path, and `icmp_needfrag` accepts only what
  TCP confirms (`tcp_pmtu_notify`: a live connection, the quoted sequence
  in flight — RFC 5927, already there, `:334`). A quiet error that is not
  Need-Fragmentation, or one TCP does not confirm, is freed
  (`icmp quiet_dropped`) — which is what happens to it today without the
  flag, since the host consumes no other ICMP error (`:377`). Nothing is
  answered in either case (ICMP errors never are).

So the firewall keeps **no TCP state and no ICMP-error model**: it does not
parse the quote, match it against a table, or decide relatedness. It asks
the layer that already validates ICMP errors against its own connections,
and adds only the silence the DROP promised. The same treatment would
extend to any future consumer of ICMP errors (a UDP unreachable notifier)
by that consumer applying its own acceptability under the flag — the
contract `M_FW_QUIET` already states in `mbuf.h`.

### 4. What does not change

Quiet delivery for TCP and UDP is untouched: TCP data is still admitted by
the connection; a UDP datagram to a *connected* socket is still admitted
by the socket; the new state is *additional* admission for the
unconnected case. A UDP listener that never sent is still closed under a
DROP (no flow exists — `net-hostinput`'s `:7000` case holds). DNAT and
masquerade are untouched (`nat_in` claims first; their flows are NAT's,
their egress is a tap). The FORWARD chain's state is untouched. The ABI is
untouched: `COSMO_NETCTL_VERSION` stays 4; there is nothing to configure
(state is not policy) and nothing new to list — the flow table has never
been listed, and a `vmctl` view of live flows is a later unit if ever
wanted. The seeded defaults are untouched.

### 5. Deliberately out of scope (named, later units)

- **A `vmctl` listing of live flows** (guest and host) — observability, not
  policy.
- **ICMP-error admission for UDP flows** (an unreachable that names a
  recorded UDP flow) — there is no consumer in the stack; when a UDP
  unreachable notifier exists it applies its own check under the flag.
- **An OUTPUT chain** — rules on the host's egress. The record hook sits
  where that chain will sit; this unit adds no rule there.
- **Per-interface host chains**, rate-limit/log targets, IPv6 (as before).

## Affected files

- `kernel/include/kernel/net/fw.h` — `FW_FLOW_MAX` 320,
  `FW_FLOW_QUOTA_HOST` 64 (the guests' `FW_FLOW_QUOTA_PER_GUEST` stays
  `256 / FW_MAX_GUESTS`, computed from a new `FW_FLOW_GUEST_POOL` 256 rather
  than from `FW_FLOW_MAX`); `void fw_host_record(struct netif *out, struct
  mbuf *m, uint32_t src, uint32_t dst, uint8_t proto);` `fw_stats` gains
  `hin_accept_established`, `hin_flow_new`, `hin_flow_drop_full`.
- `kernel-services/network/fw.c` — `fw_host_record` (UDP and echo-request
  only; the host's share counted under initiator `FW_HOST_GUEST_IP`; a
  send on a live flow refreshes it); `fw_host_verdict` gains the state
  step before the rule walk (reverse match only); the quota logic factored
  so both chains share one "find a slot within this initiator's share"
  helper.
- `kernel-services/network/ipv4.c` — `ipv4_output` calls
  `fw_host_record` for a real-link egress (after `ipv4_route`, before
  `output_on`, with `src` resolved as `output_on` would); the host-chain
  DROP branch marks ICMP errors (types 3, 11, 12) `M_FW_QUIET` and
  delivers them, freeing the rest as today; `icmp_input` honours
  `M_FW_QUIET` (Need-Fragmentation to `icmp_needfrag` only; everything else
  freed, counted). `ip_stats` gains `icmp_quiet_dropped`.
- `kernel/include/kernel/mbuf.h` — the `M_FW_QUIET` comment names ICMP
  errors among what the flag covers.
- `kernel-services/network/nettest.c`, `kernel/core/selftest.c`,
  `kernel/include/kernel/selftest.h` — `net-hoststate`; `net-firewall`'s
  quota arithmetic if it names `FW_FLOW_MAX`.
- `docs/kernel-services/network/design.md` (the host chain section gains
  its state), `testing.md`, `README.md` Status; this report as built.

## New APIs

- In-kernel: `fw_host_record(...)`, called from `ipv4_output`;
  `FW_FLOW_QUOTA_HOST`; `M_FW_QUIET` honoured by `icmp_input`. No
  firewall-side TCP query, no ICMP-error model.
- UAPI: **none**. `COSMO_NETCTL_VERSION` stays 4.

## Migration plan

1. `fw.c`: the host share and `fw_host_record`; `ipv4.c`: the call in
   `ipv4_output`; `fw_host_verdict`'s state step. Both arches boot; every
   existing test green (under the default ACCEPT nothing observable
   changes; `net-hostinput`'s connected/unconnected UDP cases still hold —
   neither socket ever *sent*).
2. ICMP errors quiet: the DROP branch in `ipv4_input`, `icmp_input` under
   the flag.
3. `net-hoststate`; docs; README; the report as built.

The behaviour changes: none under the default ACCEPT. Under a DROP
verdict, (a) replies to the host's own UDP sends and echo requests are
admitted, (b) Need-Fragmentation for a live host TCP connection is
consumed; nothing else.

## Tests

`net-hoststate` (new): the uplink tap `u` as `net-hostinput` builds one,
world addresses ARP-seeded; `policy host world drop` for the whole test
except where noted; verdicts awaited on the worker.

- **An unconnected UDP client's reply survives DROP**: the host `sendto`s
  from an unbound-then-autobound socket to `world:5300`; the datagram is
  read back on the tap (the record is taken on the way out:
  `hin_flow_new` rises); the world's reply from `5300` to the host's port
  is delivered (`hin_accept_established` rises, no rule installed); a
  datagram from the *same peer, another port* to that socket is freed
  (`quiet_dropped`, no state); one from *another host* likewise; one to
  *another local port* likewise.
- **The DNS proxy end to end under DROP**: guest A queries its gateway; the
  proxy relays upstream (`tapsvc_test_set_upstream` pointed at a world
  address on the tap — a new use of the hook, off loopback); the test
  answers from that address; A reads the answer with its original id —
  through a host default of DROP, with the proxy's unconnected socket
  unchanged.
- **The host can ping under DROP**: `icmp_send_echo` to a world address; the
  request is read back; a reply with the same id fires the reply hook
  (`hin_accept_established`); a reply with another id is freed
  (`hin_drop_default`); an echo *request* from the world is still dropped
  (a request is not a reply).
- **Path-MTU discovery under DROP**: a host TCP connection to a world peer
  (the `net-hostinput` outbound pattern); the host sends a segment; a
  Need-Fragmentation quoting that segment (MTU 576) is delivered quiet and
  consumed: `pmtu_updates` rises and the retransmission is read back
  smaller. A Need-Fragmentation quoting a tuple with no connection is
  freed (`icmp_quiet_dropped`); one quoting the live tuple with a sequence
  not in flight is freed by TCP's own check (`pmtu_updates` unchanged) —
  the firewall admitted it to the layer that could tell; a Destination
  Unreachable (port) quoting the connection is freed (no consumer).
- **One tuple, and no notion of intent**: after the host's `sendto` to
  `world:5300`, a *second* unsolicited datagram from `world:5300` to the
  host's port inside the window is admitted too (the tuple is open, and
  the socket's validation is the second line); the world *initiating* to a
  host port the host never sent from takes the rules and drops; a
  datagram carrying the host's own source address on that tuple, arriving
  on the tap, is dropped as a martian (`rx_bad_header`) before the chain
  and no `hin_*` counter moves.
- **State refresh and expiry**: with the flow recorded, `fw_age(now + 31 s)`
  reclaims it and the next reply drops; a host send re-records it and the
  reply is admitted again; a send every few seconds keeps it alive across
  a 31 s window (refresh, not re-creation: `hin_flow_new` does not rise
  again).
- **The host's share**: 64 distinct UDP flows record; the 65th does not
  (`hin_flow_drop_full`), its send still leaves (read back), and only its
  reply drops; the guests' shares are untouched (a guest-to-guest flow
  still records with the host's share full).
- **Only host-originated, real-link egress records**: a masqueraded
  guest→world UDP flow (through `ipv4_forward` → `output_on`) records
  nothing in the host's share (`hin_flow_new` unchanged) and its reply is
  claimed by `nat_in` as before; a loopback send records nothing; a DNAT'd
  inbound delivered to a guest records nothing.
- **Quiet delivery unchanged**: `net-hostinput`'s cases run green: the
  connected socket still admits by socket, the listener that never sent is
  still closed, TCP data is still admitted by the connection.
- **Regression**: `net-firewall` (guest flow state and quota unchanged),
  `net-dns`, `net-nat`, `net-dnat`, the harness's echo round trip.

Bug-proofs: the state step removed from `fw_host_verdict` (the reply to the
unconnected socket then drops and the proxy's answer never reaches A); the
port match loosened to the peer address alone (the same-peer-other-port
datagram is then admitted); the local-port match dropped (the datagram to
another local port is then admitted); the record hook placed in `output_on` instead of
`ipv4_output` (the masqueraded guest flow then occupies the host's share:
`hin_flow_new` rises for it); the egress test dropped (a loopback send
then records); the host's share unbounded (the 65th flow records and the
guests' table space shrinks — a guest flow then fails `flow_drop_full`);
refresh treated as creation (`hin_flow_new` rises on every send and the
share fills from one client); TCP recorded after all (the uplink send path
then holds `g_fw_lock` per segment — observed as `hin_flow_new` rising for
a TCP send); ICMP errors freed instead of quiet-delivered (the
Need-Fragmentation then never reaches TCP and `pmtu_updates` stays flat);
`icmp_input` ignoring the flag (a quiet echo *request* is then answered
under DROP); echo replies admitted by type rather than by identifier (the
wrong-id reply then fires the hook).

## Benchmarks

The record hook is on the uplink's UDP send path, the one `net-nicbench`
measures directly (UDP sends/s per NIC; PR #107's tree: ~17.4k/s aarch64
under TCG): a `g_fw_lock` hold and a scan of the flow table per send. The
benchmark before and after, on both arches, is the unit's number; the
expected cost is one lock and a 320-entry compare loop hitting the same
refreshed entry (the benchmark sends from one socket to one destination),
well under the ~57 µs a send costs today. If the scan shows, the host's
flows get a small hash index before the unit ships; it is not designed in
up front. The TCP send path is untouched by construction (TCP is not
recorded) and the receive path gains one `flow_find` under a DROP verdict
only.

## Risks

- **State bypasses rules.** A reply admitted by state is admitted whatever
  the rules say — the FORWARD chain's semantics, and every stateful
  firewall's. A flow the host initiated *is* the operator's intent; a
  30 s idle expiry bounds how long a closed socket's tuple stays open, and
  a rule cannot be written that a reply defeats, because a reply is not a
  request. Documented.
- **The proxy's tuple is long-lived.** One upstream socket, refreshed by
  every guest query, keeps one (host:port → upstream:53) flow open as long
  as any guest resolves names — which is the point; the proxy's own
  sender check still gates what it accepts.
- **A forged reply to a recorded tuple.** An attacker who knows the host's
  ephemeral port and the peer can deliver a datagram — as they can to any
  unconnected socket on any host; the socket's own validation (the DNS
  id, the proxy's sender check) is unchanged and is the second line.
- **The share and the DNS pattern.** If the proxy ever used a socket per
  query the share would be exhausted by eight busy guests; it does not,
  and the test pins the one-flow behaviour.
- **Hot-path cost.** Measured, with a mitigation named (an index) if the
  number demands it.
- **A future ICMP-error consumer that forgets the flag** would act on a
  quiet error. The contract is in `mbuf.h` and `icmp_input` is the single
  dispatch point; the test that answers no echo request under quiet is
  the pattern a new consumer's test must follow.

## Alternatives considered

- **Connect the proxy's upstream socket.** Rejected: the socket is
  unconnected so the proxy can check the sender itself (PR #95's design);
  connecting it would also serve only that one client — every other
  unconnected host client stays broken.
- **Seed permanent ACCEPT rules for the host's replies** (`udp from
  upstream:53`, `icmp type 0`, `icmp type 3`). Rejected: they open the
  tuple to anyone at that address for all time, not to the peer of a flow
  the host opened for 30 s; and they cannot express "the reply to *my*
  request" at all. The INPUT chain seeded rules because a guest's access to
  *the tap's services* is policy; a reply is not.
- **Record host flows at the socket layer** (`udp_send`, `icmp_send_echo`).
  Rejected: two sites instead of one, and the egress interface — the fact
  that decides whether a flow is the world's business — is known in
  `ipv4_output`, not in the socket.
- **A separate host flow table.** Rejected: `flow_find`, aging, flush and
  the quota logic exist; the host is one more initiator.
- **Record TCP flows too**, so ICMP errors can be matched by the firewall
  (a RELATED state as netfilter has). Rejected: it puts a lock and a scan
  on every uplink TCP send for a reader that already exists in a better
  place — `icmp_needfrag` validates against the live connection *and* the
  in-flight sequence, which no firewall table can. Quiet delivery of the
  error to that validator is the same design decision the host chain made
  for TCP data, applied to the one ICMP the host consumes.
- **Admit all ICMP errors under DROP** (they are "harmless"). Rejected: an
  unconsumed error is a probe's answer confirming the host is there, and a
  future consumer would inherit an open door. Quiet delivery admits exactly
  what a consumer confirms.
- **Default the host chain to DROP now that it has state.** Rejected for
  this unit: the change is behavioural for every deployment and belongs to
  its own decision once the state has proven itself; the unit's purpose is
  to make the flip safe, not to make it.
