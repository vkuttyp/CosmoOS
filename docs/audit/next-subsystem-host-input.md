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
One thing closes without any rule — the **off-link invariant**: a
datagram arriving on a link is for an address *on that link* — the ingress
interface's own, or a broadcast — and anything else is dropped before any
chain. Today `netif_owns_ipv4` treats every interface's address as the
host's and the martian check looks only at the source, so an uplink datagram
addressed to a guest's gateway is delivered into that guest's DNS proxy (an
open resolver per guest), and one addressed to `127.0.0.1` reaches
**loopback-bound services** — both reachable from the real network. Everything else waits for a rule or
a hardened default — under the default ACCEPT nothing else changes — and
when a rule *does* say DROP, the drop is **silent**: the chain does **not**
try to know which TCP segments belong to a connection
— three drafts of that (flags, "any PCB", "eligible states") each left a way
for `tcp_input` to answer a probe. Instead a DROP verdict for TCP/UDP becomes
a **quiet-delivery policy carried on the datagram** (`M_FW_QUIET`): the
transport, which owns acceptability, delivers it only if an existing
connection accepts it by TCP's own checks (sequence, acknowledgment, the
syncache completion of an admitted SYN) or a connected UDP socket names the
sender, **creates no new connection**, and **emits nothing** for a rejected
segment — no SYN-ACK, RST, challenge ACK, window ACK or port-unreachable,
enforced inside `batch_send`, the sole function that transmits a TCP
response, rather than at a list of sites or at any one flush call —
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
2. **The firewall does not decide which segments belong to a connection;
   the transport does — under a policy the firewall hands it.** Three drafts
   of a firewall-side "established" test (TCP flags; any non-listening PCB;
   an eligible-state list) each left `tcp_input` a way to *answer* a probe,
   and the last did not even match this stack: passive half-opens live in
   the listener's **SYN cache** (`tcp.c:1301-1516`, SYN cookies) and the
   child is created directly `ESTABLISHED`; `SYN_RCVD` is simultaneous open
   only (`tcp.c:1675`). So the design inverts. `fw_host_verdict` computes
   the rule/default verdict as before. On **ACCEPT** the datagram is
   delivered normally. On **DROP**, a TCP or UDP datagram is *not* freed at
   the IP layer: it is marked **`M_FW_QUIET`** (an mbuf flag — `tcp_input`
   and `udp_input` take `(nif, m, ip4, ip6)`, so the policy rides on the
   packet with no signature change) and handed to the transport, which
   under that flag:
   - **TCP** decides one thing per segment — **accepted by an existing
     connection, or rejected** — with the checks it already makes, **all of them**: the
     sequence-window test (`tcp.c:1703-1712`) is only the first; a segment
     must also pass the RST-position check (`:1728` — a reset not naming
     `rcv_nxt` draws a challenge), the in-window-SYN check (`:1740` —
     challenge), the missing-ACK check (`:1744`), the RFC 5961 §5 ACK-range
     check (`:1747` — challenge) and the `SYN_RCVD` ACK check (`:1754` —
     reset) before it is **applied** to the connection. Acceptance is that
     application — the first mutation of connection state by the segment:
     the ACK-processing step that advances `snd_una` (`≈:1758`, `≈:1769`),
     from which data delivery, FIN handling and state transitions follow; a
     **valid in-window reset** (`seq == rcv_nxt`, `:1731`), which tears the
     connection down and emits nothing; and the **SYN-cache completion of a
     passive open whose SYN was admitted earlier** (the ACK that creates the
     child, exactly as today). A `SYN_SENT` connection's valid SYN+ACK is
     applied on the same terms. An **in-window SYN on an existing
     connection** is *not* accepted under quiet: it is a connection-open
     attempt, which the policy refuses silently, and RFC 5961's challenge
     exists only to probe the peer. An *accepted* segment is processed exactly as today,
     **its ACKs and window updates included** — they are the connection's
     own traffic, which the policy lets persist. A *rejected* segment, under
     the flag, is freed with **no response and no side effect**, enforced
     **structurally rather than site by site**: every TCP response —
     `build_raw` and `build_segment` alike, so listener SYN-ACKs, resets and
     challenge ACKs (`:570`, `:673`) *and* the ACK to an out-of-window
     segment (`:1712`) — goes through `batch_push` into the per-call
     `struct tcp_batch`, and `batch_send` (`:681-690`) is the **sole
     emitter** — the only `ipv4_output`/`ipv6_output` calls in `tcp.c`. So the
     gate is `batch_send` itself, not any one call to it: the batch carries a
     `quiet` bit, set from the mbuf's `M_FW_QUIET` when `tcp_input` begins
     and **cleared at exactly the three acceptance points above** — the
     ACK-apply step, the valid in-window reset, the SYN-cache completion —
     and nowhere earlier; every `goto out` before them is a rejection whose
     batch stays quiet, so whatever it queued (a challenge ACK, a reset, a
     window ACK) is freed. `batch_send` frees a still-quiet batch instead of
     transmitting it. Clearing at the window test would let the later
     rejections answer; clearing without a named boundary could silence a
     valid connection's own output — hence the three points. Every flush — the early
     no-pcb and listener-rejection flushes at `:1617` and `:1628` that
     return before `out:`, and the final one at `:1882` — passes through
     the same function, so no return path can leak a response. Side
     effects follow the same rule, *consumed at emission, not at decision*:
     `challenge_ack` (`:673`) returns before consulting
     `challenge_allowed()` when its batch is quiet, so a rejected probe
     cannot burn the host-wide RFC 5961 budget (`g_chal_count`, `:667`,
     otherwise incremented before the response is even queued) and starve
     legitimate challenge ACKs. Bookkeeping
     moves after acceptance: `last_rx_ns = now` (`:1684`, today set before
     the acceptability test) is updated only for an accepted segment, so a
     rejected one cannot refresh the connection's keepalive clock — an
     invalid segment should never have. And no SYN-cache entry is allocated
     for a new SYN (`:1516`). Rejections are counted `quiet_dropped`.
   - **UDP** delivers only to a socket **connected** to the sender; a
     datagram to an unconnected or listening socket is freed silently, and
     no ICMP port-unreachable is sent (`udp.c:276`).
   - **ICMP** and anything else: a DROP is a plain drop (there is no
     connection to deliver to).
   The firewall thus never models TCP state, malformed segments are
   rejected by the same validation that protects the connection today —
   only now without a reply — and a DROP means **silence** by construction.
   The host's own outbound connections, its accepted inbound ones, and its
   connected UDP flows keep working under any rule set. Rules therefore gate
   **new** connections and unsolicited datagrams; a connection that exists
   when a DROP rule is added persists until it closes (as FORWARD's flow
   state does) — documented, and the operator who wants it cut closes the
   socket.
3. **Rules, first match, else the host default.** A rule matches
   `FROM_UPLINK`, proto, **source prefix**, destination prefix and selector
   (port, or ICMP type as before).

On `FW_DROP`, a TCP or UDP datagram is marked `M_FW_QUIET` and continues to
the demux (`ip_stats.hin_quiet`); anything else is freed
(`ip_stats.hin_filtered`).

**What quiet delivery cannot recognise, and why that is acceptable here.** A
reply to an *unconnected* UDP socket (a client that `sendto`s without
connecting) and every ICMP message have no connection or connected peer to
deliver to under `M_FW_QUIET`, so a DROP rule that matches them drops them.
With the default ACCEPT this costs nothing; an operator who writes a broad
UDP DROP (`udp any any`) would drop replies to the host's unconnected UDP
sockets — documented, with the guidance that UDP rules name listener ports
(or a source prefix), and with reply state for unconnected UDP and for ICMP
named as a later unit. ICMP echo to the host is gated by type as before (type
8); the host's own ping replies (type 0) pass the default.

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

- **Reply state for unconnected UDP and for ICMP** (quiet delivery reaches a
  *connected* UDP socket; an unconnected client socket's replies and ICMP
  replies still take the rules, which the default ACCEPT admits).
- **Per-interface host chains** (all real links share `FROM_UPLINK` here).
- **An OUTPUT chain** for the host's egress; rate-limit/log targets; IPv6.
- **DHCP client protection** — moot until the host has a DHCP client.

## Affected files

- `kernel/include/kernel/net/fw.h` — `FW_DIR_FROM_UPLINK`; `src_ip/src_prefix`
  in `struct fw_rule`; `FW_HOST_GUEST_IP 0`; `enum fw_verdict
  fw_host_verdict(struct netif *nif, struct mbuf *m, const struct ipv4_hdr
  *iph, unsigned ihl);` host default in a separate policy slot;
  `fw_stats` gains `hin_accept_rule/hin_drop_rule/hin_accept_default/
  hin_drop_default` (the off-link drop is an IP-layer count,
  `ip_stats.rx_offlink`, since it precedes both chains; quiet delivery's
  outcomes are `ip_stats.hin_quiet` and the transports' `quiet_dropped`).
- `kernel-services/network/fw.c` — the host object (a permanent `fw_guest`
  slot with `ip == 0`, never attached/purged, `fw_flush` resets it);
  `rule_valid` (source `0/0` unless host-scoped; `FROM_UPLINK` only with
  host scope); `rule_matches` gains the source prefix; `fw_host_verdict`.
- `kernel/include/kernel/mbuf.h` — the `M_FW_QUIET` packet flag (beside
  `M_BCAST`): "deliver only to an existing connection or connected socket,
  create nothing, answer nothing".
- `kernel-services/network/tcp.c` — honour `M_FW_QUIET` structurally:
  `struct tcp_batch` gains a `quiet` bit, set from the mbuf when `tcp_input`
  begins and cleared at exactly three points — the ACK-apply step
  (`≈:1758`/`:1769`, after the window `:1703`, RST-position `:1728`,
  in-window-SYN `:1740`, missing-ACK `:1744`, ACK-range `:1747` and
  `SYN_RCVD`-ACK `:1754` checks have all passed), a valid in-window reset
  (`:1731`), and a SYN-cache completion; `batch_send` (`:681`, the sole emitter) frees a still-quiet
  batch instead of transmitting — so the early flushes at `:1617`/`:1628`
  and the final one at `:1882` are all gated by one line; `challenge_ack`
  (`:673`) returns before `challenge_allowed()` when its batch is quiet
  (the RFC 5961 budget is consumed only for a response that will be sent);
  `last_rx_ns = now` moves from `:1684` (before the test) to after
  acceptance; no SYN-cache allocation for a new SYN under the flag
  (`:1516`). Everything an accepted segment does — including its ACKs —
  runs unchanged. A `quiet_dropped` stat.
- `kernel-services/network/udp.c` — honour `M_FW_QUIET`: deliver only to a
  socket connected to the sender, free anything else silently, and skip the
  ICMP port-unreachable (`:276`). A `quiet_dropped` stat.
- `kernel-services/network/ipv4.c` — the off-link check for every
  non-loopback ingress (before either chain; `ip_stats.rx_offlink`); the
  second call site (uplink ingress): on DROP a TCP/UDP datagram is marked
  `M_FW_QUIET` and continues to the demux (`ip_stats.hin_quiet`), anything
  else is freed (`ip_stats.hin_filtered`).
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
  { …, src_ip, src_prefix }`; the `M_FW_QUIET` mbuf flag, honoured by
  `tcp_input` and `udp_input` (no signature change — the policy rides on the
  packet). No firewall-side connection query of any kind. `fw_rule_add/del/
  list` and `fw_policy_set/get` accept `guest_ip == 0` for the host object.
- UAPI (version 4): `COSMO_NETCTL_DIR_FROM_UPLINK`, `cosmo_netctl_filter.
  src_addr/src_prefix`, `cosmo_netctl_filter_rule.src_addr/src_prefix`,
  `cosmo_netctl_filter_guest.policy_from_uplink`, `guest_addr 0` = the host.
  No new opcode, no new syscall.

## Migration plan

1. `ipv4.c`: the off-link invariant for every non-loopback ingress
   (`rx_offlink`); the `M_FW_QUIET` flag — in `tcp.c` the accepted/rejected
   disposition, the `quiet` bit on `struct tcp_batch` honoured in
   `batch_send`, `challenge_ack` consulting the budget only when its batch
   is not quiet, `last_rx_ns` moved after acceptance and the SYN-cache
   allocation gate; in `udp.c` the
   connected-socket gate and the suppressed port-unreachable; `fw.c`: the host
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
- **Quiet delivery: the transport decides, and answers nothing**: a world
  SYN accepted by a rule to a host listener completes a handshake (the host's
  SYN-ACK is read back on the uplink tap; the world's ACK creates the child
  via the SYN cache); a DROP rule for that port is then added — the
  *established* connection's next data segment is still delivered
  (`hin_quiet` rises, `tcp quiet_dropped` does not), while an **ACK-only
  probe from a source with no connection** to the same port is freed by TCP
  (`quiet_dropped` rises) and **no RST is read back**; the same for a SYN+ACK
  with no connection. A **new SYN** to that port under the DROP rule draws
  **no SYN-ACK and no RST** (no SYN-cache entry is made). A datagram to a
  *connected* UDP socket's peer is delivered; one to an unconnected listener
  is freed with **no ICMP port-unreachable** read back.
- **TCP's own validation, silenced**: a world SYN is admitted (default
  ACCEPT), a DROP rule for the port is added *before* the world's completing
  ACK arrives — and that ACK still opens the connection (the SYN-cache
  completion runs under `M_FW_QUIET`; the child is created and a listener
  `accept` returns it); a segment with a bad sequence number to the
  established tuple draws **no challenge ACK**, an out-of-window segment
  draws **no window ACK**, and neither touches the connection's state — its
  keepalive clock included (a keepalive probe scheduled before the rejected
  segment still fires on time); the host `connect`s out to a peer on the uplink tap (its
  SYN is read back) under a DROP rule covering the peer, and the peer's valid
  SYN+ACK is admitted and completes the handshake while a bare ACK from that
  peer is freed silently; after the host closes an accepted connection
  (`TIME_WAIT`), a bare ACK from the peer to that tuple draws nothing.
- **Rejected probes consume no shared budget**: under a DROP rule, a burst of
  more than `TCP_CHALLENGE_PER_SEC` bad-sequence probes at a covered port is
  freed silently — and immediately afterwards a legitimate out-of-window
  segment on an *accepted* connection (outside the rule) still draws its RFC
  5961 challenge ACK, proving the host-wide challenge budget was not spent
  on the rejected ones.
- **The acceptance boundary is the apply step, not the window test**: under
  a DROP rule covering the peer of an accepted connection, an **in-window
  SYN** on that connection draws no challenge ACK (a connection-open attempt
  is refused silently), an in-window segment whose ACK is outside the RFC
  5961 §5 range draws no challenge, a reset not naming `rcv_nxt` draws no
  challenge — all three are in-window and all three are rejections; while a
  **valid reset** from the peer (`seq == rcv_nxt`) still tears the connection
  down (the accepted socket reports the reset), because a legitimate teardown
  of an existing connection is applied, and emits nothing anyway.
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
the `quiet` bit not honoured in `batch_send` (a rejected segment's batch
is sent as today: the ACK-only probe from a source with no connection then
draws a RST, the bad-sequence segment a challenge ACK, and the out-of-window
segment a window ACK — one revert, three observed responses); the gate
placed at the final flush only (the no-pcb path's early flush at `:1617`
then sends the RST); `quiet` cleared at the window test instead of the
apply step (the in-window SYN, the out-of-range ACK and the mis-positioned
reset then each draw a challenge ACK); `challenge_ack` consulting the budget before checking
the quiet bit (the probe burst then starves the accepted connection's
legitimate challenge ACK); the SYN-cache
allocation not gated (a new SYN under the DROP rule then draws a SYN-ACK);
`last_rx_ns` left before the acceptability test (a rejected segment then
refreshes the keepalive clock and the scheduled probe fires late); a DROP that
frees TCP at the IP layer instead of marking it quiet (the established
connection's data then stops flowing, and the admitted SYN's completing ACK
never opens the connection); `udp_input` ignoring the flag (the datagram to
the unconnected listener then draws a port-unreachable); the off-link
invariant removed (the world's query then reaches A's proxy and is relayed,
and the world's datagram reaches the loopback-bound listener); the call
site moved before `nat_in` (the DNAT'd SYN then drops under the host DROP
rule — the proof the INPUT unit could not run).

## Benchmarks

The uplink is the hot path: the NIC's receive rate with an empty host rule
list (one flag test and the off-link compare, `iph->dst == nif->ip4.addr`)
and with a short list, measured with `net-nicbench`; and, under a DROP rule,
quiet delivery costs no extra lookup (the demux it would have taken, minus
the responses). The guest-tap path gains exactly the off-link compare (the
INPUT chain is otherwise unchanged); the loopback path is untouched.

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
- **Quiet delivery's cost and semantics.** No extra lookup: a dropped-by-rule
  TCP/UDP datagram takes the demux it would have taken anyway, minus the
  responses — the benchmark measures the uplink receive rate with and
  without a DROP rule. Semantically, rules gate *new* connections and
  unsolicited datagrams; a connection that exists when a DROP rule is added
  persists until it closes (FORWARD's flow state behaves the same way) —
  documented; an operator who wants it cut closes the socket. The
  implementation risk is a response path that escapes the gate — which is
  why the gate is inside `batch_send` — the sole emitter, through which
  every flush passes — not a list of sites (a per-site list missed the
  out-of-window ACK at `:1712` in review) and not any one flush call (the
  final-flush form missed the early flushes at `:1617`/`:1628` in review) —
  and a side effect that precedes emission, of which two were found:
  `last_rx_ns` updated before acceptance, and the RFC 5961 budget consumed
  before the response is queued; each has a bug-proof (one revert of the
  gate draws a RST, a challenge ACK and a window ACK; the clock left early
  delays a keepalive probe).
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
  transient tuple would still be answered.
- **An eligible-state filter (`ESTABLISHED`, closing states, `SYN_RCVD`,
  `SYN_SENT` for SYN+ACK).** The draft's third form; rejected on review for
  two reasons that settle the question: it did not match this stack (passive
  half-opens live in the listener's SYN cache and the child is born
  `ESTABLISHED`, so the completing ACK of an admitted SYN matched nothing
  and a permitted connection could never open), and state-plus-flags still
  admitted malformed segments that `tcp_input` answers. Any firewall-side
  reconstruction of "does this segment belong to a connection" re-derives
  TCP's acceptability test and will lag it; quiet delivery hands the
  question to the layer that already answers it, and adds only silence.
- **Carry the quiet policy as a new `tcp_input`/`udp_input` parameter.**
  Rejected in favour of an mbuf flag: both take `(nif, m, ip4, ip6)` and are
  called from several places; a flag on the packet reaches exactly the
  response and creation sites that must honour it with no signature churn,
  as `M_BCAST` already does for broadcast delivery.
- **Silence by enumerating the emit sites.** The quiet design's first form
  named three (`:570`, `:673`, `:1516`) and review found a fourth — the ACK
  to an out-of-window segment at `:1712` via `build_segment`. Rejected: a
  list of sites is only as complete as the last audit. Every TCP response
  already goes through `batch_push` into the per-call batch, and
  `batch_send` is the sole function that transmits it (the only
  `ipv4_output`/`ipv6_output` calls in `tcp.c`), so the gate is `batch_send` itself,
  the sole emitter — a `quiet` bit on the batch, set at entry and cleared on
  acceptance, makes it free a rejected segment's batch — and not any one
  call to it: the "final flush" form of this idea missed the early flushes
  at `:1617`/`:1628` in review. Any response added in the future is
  silenced with the rest. The same review found bookkeeping
  (`last_rx_ns`) done before acceptance, so "no side effect" is enforced the
  same way — nothing is recorded until the segment is accepted.
- **Drop at the socket layer (refuse a match whose ingress is not the
  bound interface).** Rejected: it scatters the invariant across every
  transport's demux and cannot be listed or counted as one thing; the IP
  layer knows the ingress and the destination together.
- **Per-interface host chains now.** Rejected for scope: one real link
  exists; the chain is written so a second one can be split off later.
