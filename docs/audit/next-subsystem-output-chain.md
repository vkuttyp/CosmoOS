# NEXT SUBSYSTEM — the OUTPUT chain: what the host itself may send

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This was that report; the unit is
now **implemented** (PR "The OUTPUT chain: what the host itself may send"),
and the design below is as built — see
`docs/kernel-services/network/design.md` ("The OUTPUT chain") for the
shipped description and `docs/kernel-services/network/testing.md`
(`net-output`) for its proofs. Two things came out differently and are
marked where they arise: the ordering this report leaned on turns out **not**
to be what guarantees the property it was chosen for, and the performance
mitigation held in reserve **was** needed — on the evidence of the test
suite's timing, not the benchmark's throughput.

**Subsystem: the firewall's last chain. FORWARD decided which machines a
guest may reach, INPUT which of the host's services a guest may reach, the
host chain which of them the world may reach, and the host-state unit gave
that chain the replies to the host's own flows. The one path still under no
policy is the host's **own egress** — every datagram the machine originates,
to a guest or to the world. `fw_output_verdict` is called from
`ipv4_output` at the point the host-state unit already reads the flow, and
for the first time a DROP is **told to the sender**: `ipv4_output` returns
`-EPERM`, which `udp_sendto` and `icmp_send_echo` already propagate to the
caller, because the party being refused is local and can be given an answer
rather than silence. The verdict runs **before** the flow record, so a
datagram the chain refuses opens no reply state — the two units meet
exactly there. One new direction, `FW_DIR_OUTPUT`, on the host's existing
policy object; rules may name a **source** (which of the machine's addresses
sent), a destination prefix and a selector, and — the thing this chain can
express that no other can — the egress **scope**: whether the datagram is
leaving for the world or going to a guest tap, so that "the host may not
answer guests on this port" and "the host may not call out to the world on
that one" are different rules. The default is **ACCEPT**: the host is the
trusted party, and a default DROP would cut the tap services, the harness's
replies and the machine's own name resolution in one stroke.**

## Problem (the state before this unit)

Before this unit the filter covered three of the four directions traffic can
take through this machine, and the fourth was the one nothing watched:

- **A compromised or buggy host service can call out freely.** Every
  listener the host runs, every module, every in-kernel service (`tapsvc`'s
  DNS relay, the future OUTPUT of anything else) reaches any address the
  routing table can name. The host chain stops the world *reaching in*; it
  says nothing about the host reaching out, which is the direction that
  matters once something inside is misbehaving.
- **The host's replies to guests are unfilterable.** The INPUT chain
  decides which of the host's services a guest may *reach*, but the host's
  answer — and anything else the host sends *to* a guest — leaves through
  `ipv4_output` with no verdict. An operator who wants a guest's tap to
  carry DHCP and DNS but nothing else the host might send has no way to say
  so. Both the INPUT-chain and host-chain reports named this as OUTPUT's
  job ("and, with it, filtering the host's replies to guests").
- **A guest's tap and the world are the same to the sender.** The host has
  a route to every guest subnet (`netif_connected`, the routing unit) and to
  the world. Nothing distinguishes "the host sent this to a guest" from
  "the host sent this to the internet" in any policy the machine can hold.
- **A refused send is silent everywhere else.** The other three chains
  filter *someone else's* traffic, so silence is the right answer (the host
  chain went to some length to make a DROP mean silence). On egress the
  sender is a local socket that already receives errors — `-ENETUNREACH`
  when no route exists, `-EMSGSIZE` when the datagram is too large — and
  can be told `-EPERM` in exactly the same way.

## Implementation before this unit

`kernel-services/network/ipv4.c`, `ipv4_output` (`:175-200`) is the host's
single egress door and, since the host-state unit, already the place where
the firewall reads a flow:

```
    struct netif *nif = ipv4_route(dst);          /* no route: -ENETUNREACH */
    struct fw_host_flow hf;
    bool track = fw_host_flow_of(nif, m, src ?: ipv4_source_for(dst), dst, proto, &hf);
    int rc = output_on(nif, m, src, dst, proto, ttl);   /* MTU, header, ARP, link */
    if (track && rc == 0)
        fw_host_record(&hf);
```

- **Every host-originated datagram passes here**: `udp_sendto`
  (`udp.c:156`, returning the result to the caller), `icmp_send_echo`
  (`ipv4.c:434`, likewise), the host's ICMP errors and echo replies
  (`:280`, `:308`, `:396`) and TCP's `batch_send` (`tcp.c:705`).
- **So does one kind of traffic that is not the host's own**: `nat_in`'s
  delivery of a masqueraded reply or a DNAT'd connection to a guest
  (`nat.c:454`) re-emits through `ipv4_output`. **`ipv4_forward`'s own
  transmit path does not** — it calls `output_on` directly (`:531`) — so
  the datagrams a guest forwards are FORWARD's business, while the ones NAT
  re-emits on a guest's behalf will reach this chain as `FW_SCOPE_GUEST`
  traffic. That is intended and is the sharp edge of this unit (see §3 and
  Risks); it is stated here because "no forwarded datagram passes this
  door" would be the wrong summary.
- **Two senders already propagate the return value** (`udp_sendto`,
  `icmp_send_echo`); TCP's `batch_send` ignores it, as it ignores
  `-ENETUNREACH` today.
- **`fw.c`** holds the host as a policy object (`FW_HOST_GUEST_IP`), with
  `policy[FW_DIR_COUNT]` slots and rules whose direction is one of
  `TO_UPLINK`, `TO_GUEST`, `TO_HOST`, `FROM_UPLINK` (and the forwarding
  wildcard `ANY`). `rule_matches` already matches a source prefix, a
  destination prefix, the protocol and the selector; only the *direction*
  values and the call site are missing for egress.
- **No egress counter exists**: `ip_stats` has `tx` and `tx_no_route`.

## Why it matters

- **It completes the filter.** With OUTPUT, every ingress→egress pair the
  stack serves has a chain: guest→guest, guest→world, guest→host,
  world→host, host→anywhere. The unit after this one is a *refinement*
  (per-interface chains, targets, IPv6), not a missing direction.
- **It is the direction that contains a compromise.** Ingress policy keeps
  attackers out; egress policy limits what a foothold can do. A host
  firewall without it is half a firewall.
- **It gives the tap's policy its other half.** The INPUT chain and the
  tap's seeded rules decide what a guest may ask of the host; OUTPUT
  decides what the host may say to a guest — including saying nothing.
- **The place to put it is already prepared and already proven.** The
  host-state unit established that `ipv4_output` is the one door, tested
  that forwarded and loopback traffic do not pass it, and left the hook
  there. This unit adds the verdict beside the read.

## Design (as built)

### 1. One direction, with an egress scope in the rule

`enum fw_dir` gains **`FW_DIR_OUTPUT`** (`COSMO_NETCTL_DIR_OUTPUT = 5`),
valid on the **host object only** (`-EINVAL` for a guest, as `FROM_UPLINK`
is). It is the host's fifth policy slot (`FW_DIR_COUNT` 4→5).

A rule's existing fields carry most of the match: `src_ip/src_prefix`
(which of the machine's addresses is sending — meaningful now that a host
has a NIC and up to eight tap gateways), `dst_ip/dst_prefix`, `proto` and
the selector (destination port, or ICMP type). What they cannot express is
the *kind* of link the datagram is leaving by, which is exactly the
distinction the Problem section names, so the rule gains one byte:

```c
uint8_t scope;   /* FW_SCOPE_ANY / FW_SCOPE_WORLD / FW_SCOPE_GUEST */
```

- `FW_SCOPE_WORLD` — the egress is a real, non-guest link (`nif->flags` has
  neither `NETIF_MASQUERADE` nor `NETIF_LOOPBACK`): the host is talking to
  the world.
- `FW_SCOPE_GUEST` — the egress is a guest tap (`NETIF_MASQUERADE`): the
  host is talking to a guest, replies included.
- `FW_SCOPE_ANY` — either.

`scope` is meaningful for `OUTPUT` only and must be `FW_SCOPE_ANY` on every
other direction (`-EINVAL` otherwise), so tuples stay canonical exactly as
the source prefix does. A destination prefix *could* name a guest's subnet
today, but not durably: the pool assigns `10.0.(3+k).0/24` as taps open and
close, so a rule written against a subnet silently follows whichever guest
gets that slot, while a rule written against the *scope* keeps meaning what
it said. Both are available; the scope is the one that does not rot.

**Loopback is exempt**, as it is on every other chain: the host talking to
itself is not traffic this machine polices, and gating it would put a rule
between a process and `127.0.0.1` with no way to notice.

### 2. Where, and in which order

`fw_output_verdict(struct netif *out, struct mbuf *m, uint32_t src,
uint32_t dst, uint8_t proto)` is called from `ipv4_output` **after the
route** (the egress decides the scope) and **before `output_on`** and
before the flow read.

The `src` it receives must be **the address the wire will carry**, and
today that address is computed twice: `ipv4_output` resolves an unspecified
source for the flow read (`src != 0 ? src : ipv4_source_for(dst)`) and
`output_on` resolves it again for the header. A source-prefix rule judging
a raw `0` — which `icmp_send_echo` passes, and any unbound UDP socket — would
match a different address from the one sent, so this unit **resolves it once**
in `ipv4_output` and hands the same value to the verdict, the flow read and
`output_on`, whose own resolution then becomes dead and is removed:

```
    nif = ipv4_route(dst);                       /* -ENETUNREACH as today */
    uint32_t from = src != 0 ? src : ipv4_source_for(dst);   /* once, for all three */
    if (!(nif->flags & NETIF_LOOPBACK) &&
        fw_output_verdict(nif, m, from, dst, proto) == FW_DROP) {
        STAT(tx_filtered);
        m_freem(m);
        netif_put(nif);
        return -EPERM;                           /* the sender is local: tell it */
    }
    hf = fw_host_flow_of(nif, m, from, dst, proto, ...);      /* same value */
    rc = output_on(nif, m, from, dst, proto, ttl);            /* no longer resolves */
    if (track && rc == 0) fw_host_record(&hf);
```

A test asserts the equivalence directly: a source-prefix rule that names the
NIC's own address matches an `icmp_send_echo` (which passes `src == 0`),
which it could not do if the verdict saw the unresolved value.

Two orderings matter, and one of them turned out to matter less than this
report claimed:

- **Before the flow record**, so that a datagram the chain refuses opens no
  reply state — otherwise an OUTPUT DROP would punch a hole in the host chain
  for the reply to a datagram that was never sent. The property holds and the
  test asserts it. **But the ordering is not what guarantees it**, as the
  bug-proof for it discovered: `fw_host_record` is already conditional on
  `output_on` returning 0, so a datagram the verdict drops can never be
  recorded whichever side of the *read* the verdict sits on — the read takes
  no lock and mutates nothing. Moving the verdict after the read leaves the
  whole suite green. The order is kept because it reads better and skips work
  the verdict may waste, and the guarantee is credited to where it actually
  lives (the host-state unit's own bug-proof 13).
- **After the route.** The scope *is* the egress, so the verdict cannot
  precede `ipv4_route`. A datagram with no route keeps returning
  `-ENETUNREACH` and is counted as it is today: no route is not a policy
  decision.

`m` carries the transport header at offset 0 here (the IP header is
prepended inside `output_on`), as `fw_host_flow_of` already relies on, so
the selector is read with the same `l4_read(m, 0, …)`.

### 3. `-EPERM`, and what it does and does not reach

This is the first chain whose DROP is **not** silence, and the reason is
that the party refused is the machine itself:

- **UDP**: `udp_sendto` returns `ipv4_output`'s value, so a `sendto` refused
  by a rule fails with `-EPERM` and the program learns immediately. This is
  what a POSIX host does, and it is the behaviour an operator debugging a
  rule needs.
- **ICMP echo**: `icmp_send_echo` likewise.
- **TCP**: `batch_send` ignores the return, as it ignores every other
  output error today, so a segment a rule refuses is dropped and the
  connection behaves as though the network swallowed it — `connect` times
  out rather than failing fast. **Deliberately not changed here**: making a
  verdict visible to `connect`/`send` means teaching `batch_send` to report
  per-segment failures back into the PCB, which is a TCP change with its
  own failure modes (a rule added mid-connection would then break
  established connections that today merely stall). Named as a later unit,
  with the behaviour documented and asserted.
- **The host's ICMP errors and echo replies** (`ipv4.c:280`, `:308`,
  `:396`) ignore the return too, and that is correct: a refused error
  message has nobody to tell.
- **`nat_in`'s delivery to a guest** (`nat.c:454`) passes through
  `ipv4_output` with a guest-tap egress, so it is `FW_SCOPE_GUEST` traffic
  and *is* subject to the chain. That is intended — "what the host may send
  to a guest" includes what it forwards on a guest's behalf — but it is also
  the one place where a rule can break a guest's connectivity from the host
  side, and the Risks section says so.

### 4. The default, and what must keep working

`OUTPUT` default **ACCEPT**, and this is the chain where that is least
negotiable. A default DROP would, in one stroke: stop `tapsvc`'s DNS
replies to every guest (`ksock_sendto` → `ipv4_output`, guest-tap egress),
stop the harness's TCP replies on the NIC (so the boot test fails), stop
the host's own name resolution, and stop `nat_in` from delivering
masqueraded replies and DNAT'd connections to guests. The operator hardens
by rule; a hardened *default* would need seeds for all four, and seeding
"everything the machine currently does" is not policy, it is a copy of the
implementation. Documented as the reason, with the alternative rejected
explicitly below.

### 5. Control plane and `vmctl`

ABI **version 5**: `COSMO_NETCTL_DIR_OUTPUT`; a `scope` byte in
`struct cosmo_netctl_filter` and `cosmo_netctl_filter_rule`, each of which
carries `reserved[3]` today — version 5 spends one of those three bytes on
`scope` and leaves `reserved[2]`, so **those two do not change size**; and
`policy_output` in the per-guest record, which **does** grow. That record is
`{u32 guest_addr; u8 policy_to_uplink, policy_to_guest, policy_to_host,
policy_from_uplink}` — version 4 spent its last spare byte, so a fifth
policy makes it 9 bytes and, padded to its `u32` alignment, **8 → 12**.
`COSMO_NETCTL_SNAPSHOT_MAX` therefore grows by 4 bytes per policy object
(36 for nine objects) and is recomputed and static-asserted as before, and
the three tapctl-reading tests carry the new expected lengths. Alternatives
were to pack a fifth verdict into two bits of an existing byte, or to keep
the host's OUTPUT default outside the per-guest record: both were rejected
for the reason the record exists — one byte per direction, read the same way
for every object, is what makes the listing legible and the reader's walk
trivial.
`vmctl filter add|del host out PROTO SRC[/PREFIX] DST[/PREFIX] PORT VERDICT
[world|guest|any] [INDEX]`, `policy host out accept|drop`, and `list`
printing the scope for a host `out` rule. The exact-size-per-op dispatch
refuses a version-4 writer as before.

### 6. Deliberately out of scope (named, later units)

- **A verdict visible to TCP's callers** (`connect`/`send` failing with
  `-EPERM` instead of stalling): a `batch_send` error path into the PCB.
  Designed since, in `next-subsystem-tcp-verdict.md`, and awaiting the
  instruction to build.
- **Per-interface chains** (a rule naming `eth1` rather than a scope).
- **Rate-limit and logging targets**, IPv6 filtering, full TCP state.
- **Filtering the host's *forwarded* traffic here**: that is FORWARD's, and
  `ipv4_forward` deliberately does not pass this door.

## Affected files

- `kernel/include/kernel/net/fw.h` — `FW_DIR_OUTPUT`, `FW_DIR_COUNT` 5,
  `FW_SCOPE_*`, `scope` in `struct fw_rule` (its single `reserved` byte,
  which leaves the kernel rule with no spare: the next field a rule needs
  will grow that struct, and the unit that needs it should expect to);
  `enum fw_verdict fw_output_verdict(struct netif *out, struct mbuf *m,
  uint32_t src, uint32_t dst, uint8_t proto);` `fw_stats` gains
  `out_accept_rule/out_drop_rule/out_accept_default/out_drop_default`.
- `kernel-services/network/fw.c` — `fw_output_verdict` (the fast path first,
  then the scope from the egress flags, then the host's `OUTPUT` rules
  first-match, else its default); `g_out_fast` with `out_fast_update` called
  under the lock from `fw_rule_add`, `fw_rule_del`, `fw_policy_set` and
  `host_reset`; `rule_valid` (scope only with `OUTPUT`, and `OUTPUT` only on
  the host object, beside `FROM_UPLINK`); `rule_same` includes the scope;
  `rule_matches` takes the datagram's egress scope (`FW_SCOPE_ANY` from the
  three chains that have none).
- `kernel-services/network/ipv4.c` — the call in `ipv4_output` after the
  route, before the flow read; `-EPERM` and `ip_stats.tx_filtered`.
- `kernel/include/kernel/net/ip.h` — `tx_filtered`.
- `kernel/include/uapi/cosmo/netctl.h` — version 5: `DIR_OUTPUT`, the
  `scope` byte in the filter command and rule records (one of their three
  reserved bytes each, leaving `reserved[2]`; no size change),
  `policy_output` in the per-guest record
  (8 → 12 bytes), the recomputed `SNAPSHOT_MAX` and its static asserts.
- `kernel-services/network/tap.c` — the v5 dispatch, the scope carried both
  ways, `policy_output` in the listing.
- `userland/system/vmctl.c` — the `out` direction word, the optional scope
  word, the scope in `list`.
- `kernel-services/network/nettest.c`, `kernel/core/selftest.c`,
  `kernel/include/kernel/selftest.h` — `net-output`.
- `docs/kernel-services/network/design.md` (the firewall's fourth chain),
  `testing.md`, `README.md` Status; this report as built.

## New APIs

- In-kernel: `fw_output_verdict(...)`; `FW_DIR_OUTPUT`; `FW_SCOPE_*` and
  `struct fw_rule.scope` (its one reserved byte, leaving the kernel rule
  with no spare); `fw_policy_get` reports a fifth slot. `ipv4_output`
  resolves the source once and returns `-EPERM` for a refused datagram.
- UAPI (version 5): `COSMO_NETCTL_DIR_OUTPUT`, `COSMO_NETCTL_SCOPE_*`, the
  `scope` byte, `policy_output`. No new opcode and no new syscall; the
  per-guest policy record grows by four bytes, the other two do not.

## Migration (done, in the planned order)

1. `fw.h`/`fw.c`: the direction, the scope, the verdict; `ipv4.c`: the call
   site, `-EPERM`, `tx_filtered`. Both arches boot with the default ACCEPT
   and every existing test green (nothing in the tree writes an OUTPUT
   rule, and the default changes nothing).
2. UAPI v5, `tap.c`, `vmctl`, and the version/size expectations in the
   three tapctl-reading tests.
3. `net-output`; docs; README; the report as built.

The behaviour that changes: nothing until an operator writes a rule. With
one, the refused datagram is dropped before it reaches the link, its sender
is told `-EPERM` where the sender reads errors, and no reply state is
opened for it.

## Tests

`net-output` (new): the uplink tap and a guest tap as `net-hoststate`
builds them, host sockets, and the world ARP-seeded.

- **Default ACCEPT changes nothing**: with no rule, the host's UDP to the
  world leaves as before (`out_accept_default` rises — and, as built, that
  counter is incremented by the fast path too, so it still means "what this
  chain let out by default").
- **A rule refuses a send, and the sender is told**: `OUTPUT udp any
  10.77.9.0/24 :5300 DROP` makes `ksock_sendto` return **`-EPERM`**,
  nothing is read back on the tap, and `ip_stats.tx_filtered` and
  `out_drop_rule` rise. An echo request under an `icmp type 8` OUTPUT rule
  likewise returns `-EPERM` from `icmp_send_echo`.
- **The scope distinguishes the two egresses**: a rule with
  `FW_SCOPE_GUEST` drops the host's UDP to a guest's address while the same
  datagram to a world address still leaves; `FW_SCOPE_WORLD` is the mirror;
  `FW_SCOPE_ANY` drops both. A rule that names only a destination prefix
  keeps working, so the scope adds a dimension rather than replacing one.
- **The host's reply to a guest is filterable**: guest A's DNS query
  reaches the proxy (INPUT's seed) and the proxy's relay leaves for the
  upstream, but with an `OUTPUT udp scope guest :4444 DROP` rule the
  proxy's *answer* to A never reaches A's tap — the case both earlier
  reports named, now expressible.
- **A refused datagram opens no reply state**: with a DROP rule on its port
  and the host chain closed for that world, the host's `sendto` fails,
  `hin_flow_new` does **not** rise, and the world's reply is dropped for want
  of a flow; delete the rule and the same send records its flow and the same
  reply arrives. The step needs a destination port of its own — as built, the
  first step's successful send leaves a live flow that would have admitted
  the reply regardless, which the first run of this test discovered.
- **A failure that is not a verdict is not counted as one**: as built this is
  the oversized datagram (`-EMSGSIZE` from `output_on`), which the chain
  accepts and the link refuses, leaving `tx_filtered` and the flow count
  alone. The planned no-route case is **not reachable** from a socket here —
  the NIC carries a default route, so every address routes somewhere — and
  the substitute makes the same point deterministically.
- **Loopback is exempt**: a rule that would match by every other field does
  not touch a `127.0.0.1` send.
- **TCP stalls rather than failing** (the documented limit): a *nonblocking*
  `connect` to a refused port returns `-EINPROGRESS`, not `-EPERM`, while
  `out_drop_rule` rises and no SYN is read back — nonblocking as built, so
  the test does not park a thread on a connect that will only time out.
  Asserted so the later unit that changes it has a test to change — which
  is `next-subsystem-tcp-verdict.md`, whose first step reverses this
  assertion.
- **`nat_in`'s delivery is scope-guest traffic**: with a port-forward to A
  and an `OUTPUT scope guest DROP` rule, the DNAT'd SYN is dropped on its
  way to A (`tx_filtered`), and without the rule it arrives — the
  interaction the Risks section warns about, asserted rather than assumed.
- **Scope discipline**: a scope on a `TO_HOST` or `FROM_UPLINK` rule is
  `-EINVAL`; an `OUTPUT` rule on a guest is `-EINVAL`; a scope value out of
  range is `-EINVAL`; `policy <guest> out` is `-EINVAL`.
- **The scope is part of a rule's identity** (added in the build, because the
  proof for it was otherwise unobservable): two rules alike but for the
  scope both install, both drop their own egress, and each deletes by its own
  tuple.
- **The hardened default** (added in the build): flipping the OUTPUT policy
  to DROP with no rule at all refuses the next send and counts
  `out_drop_default`, and ACCEPT restores it. The verdicts are taken into
  locals and the default restored *before* any assertion — a failure here
  would otherwise leave the machine unable to send for every test that
  follows, the lesson the host-state unit paid for — and this is also what
  keeps the fast path honest: a policy flip with no rules must invalidate it.
- **Control round trip**: an `OUTPUT` rule with a scope written through
  `/dev/net/tapctl` is listed with it, beside `policy_output`; a version-4
  writer is refused **by version**, since the command's size did not change
  (the report's "refused by size" was wrong about which check fires); a guest
  naming `OUTPUT` is refused there too; and the policy record's version-5
  width is asserted by a static assert in the test.
- **Regression**: `net-hoststate`, `net-hostinput`, `net-input`,
  `net-firewall`, `net-dnat`, `net-dns`, `net-nat`, the harness's echo
  round trip.

Bug-proofs — **nine run**, each reintroduced, observed failing for the stated
reason, the source restored byte-identical; and **one named that is not
observable**, which is the more interesting entry:

- the gate counting the verdict but not stopping the datagram (the refused
  `sendto` then returns success and the datagram reaches the link — as built
  this is how "the verdict must precede the link" is proved, the report's
  "verdict after `output_on`" construction having crashed the boot by handing
  `fw_output_verdict` a datagram that no longer existed);
- the loopback exemption removed (the `127.0.0.1` send then fails);
- the scope ignored in `rule_matches` (the guest-scoped rule then drops the
  host's world traffic too);
- the scope left out of `rule_same` (the second of two rules alike but for it
  is refused `-EEXIST`);
- `-EPERM` replaced by a silent success (the failed `sendto` then returns the
  byte count and the caller cannot tell);
- the source resolved *after* the verdict (the source-prefix rule then judges
  `0` and misses the echo it should refuse — and `net-hoststate` fails with
  it, since the flow key takes the same unresolved value);
- `OUTPUT` allowed on a guest's object (the guest's add then succeeds);
- a new rule not invalidating the fast path (the first rule then never binds:
  the refused send succeeds);
- a policy flip not invalidating it (the hardened default then lets
  everything out).

**Not observable: the verdict placed after the flow read.** The suite stays
green, because `fw_host_record` is already conditional on `output_on`
succeeding — so the ordering cannot be what keeps a refused send from opening
reply state, and the claim above is corrected rather than defended. Two of
the nine proofs also had to be rebuilt before they *were* observable (the
scope-identity case needed two rules alike but for the scope; the
source-resolution case needed a source-prefix rule in this unit's own test,
which the report promised and the first draft omitted) — a reminder that a
bug-proof tests the test as much as the code.

## Benchmarks

This is the host's send path, so the cost is one flag test (loopback), one
`g_fw_lock` hold and a walk of the host's `OUTPUT` rules — paid by every
datagram the machine sends to a non-loopback egress, TCP's segments
included.

**`net-nicbench`'s UDP loop is the instrument for throughput** — it sends to
`nif->ip4.gateway` on the NIC ten thousand times, a real egress, so it
exercises this path on every send — and it could not resolve the cost, again.
Three runs of `main` (72ef489) and three of this tree, same session, aarch64:

| UDP sends/s | run 1 | run 2 | run 3 | median |
| --- | --- | --- | --- | --- |
| `main` | 21470 | 15413 | 15177 | 15413 |
| this unit | 20878 | 16893 | 20626 | 20626 |

The chain measures *faster* on the median, and the spread within one build is
40%. `net-lo-tcp` was dropped from the plan: loopback is exempt from this
chain, so those transfers never reach the verdict and citing them would have
measured nothing.

**The cost showed where the last unit's did: in the suite's timing.** One
aarch64 run in four failed three tap-driven tests at once — `net-hostinput`'s
window-update read-back, `net-hoststate`'s masqueraded forward and
`net-output`'s own proxy answer — the signature the host-state unit taught
us to read as a slower send path, with x86 and the GIC variant green in the
same batch. So **the mitigation this report held in reserve is implemented**:
`g_out_fast`, a flag meaning "no rule of the host's names OUTPUT and its
OUTPUT default is ACCEPT", maintained under `g_fw_lock` at every change to
the host object and read with one relaxed load on the send path. When it is
set — every configuration but a deliberately filtered one, and the state the
machine boots in — a send skips the transport-header copy, the key, the lock
and the walk, and is still counted, so the statistic keeps its meaning.
Three consecutive aarch64 runs, x86_64, both GIC boots and the reproducible
check are green with it, where the version without it failed one run in four.

Two honest caveats. The mitigation's necessity rests on a 1-in-4 failure
against a 3-in-3 recovery, which is suggestive rather than conclusive; it is
also *free* in the case that matters, so the decision does not hang on the
statistics. And the flag is read without the lock, so a rule added
concurrently with a send in flight may not bind that datagram — the same
"rules take effect now-ish" property every chain already has, since a
datagram past its verdict is never re-judged.

## Risks

- **A rule can cut the machine off, or cut a guest off.** OUTPUT is the
  chain where a mistake stops the host from answering anything, and where a
  `scope guest` rule can silently break a guest's DNAT'd or masqueraded
  traffic (`nat_in` delivers through this door). Mitigation: default
  ACCEPT; the scope so that "to guests" and "to the world" are separable;
  the interaction asserted in the tests; documented plainly.
- **`-EPERM` is a new error from `sendto`.** Callers that treat any error
  as fatal will now fail where they previously succeeded — which is the
  point, but it is a new failure mode in userland. No in-tree caller
  changes behaviour under the default.
- **TCP's asymmetry.** UDP and ICMP learn; TCP stalls. Documented, tested,
  and named as the next refinement rather than half-built here — and that
  refinement is now the report `next-subsystem-tcp-verdict.md`, which keeps
  the asymmetry only where it is honest: an opening connection is told at
  once, a synchronized one on its next call, since by then its bytes are
  already queued.
- **The cost is on every send.** Measured as above; the mitigation is
  implemented rather than merely named, because the suite's timing said it
  was needed. What remains is one relaxed load per host-originated datagram
  while no OUTPUT rule exists.
- **ABI v5 grows the per-guest record.** The filter command and rule
  records spend one of their three reserved bytes each (`reserved[3]` →
  `scope` + `reserved[2]`) and keep their size; the policy record has none
  left and goes 8 → 12, so `SNAPSHOT_MAX` grows and every reader's expected
  length moves with it. A v4 writer is refused by version, and a
  v4 *reader* refuses a v5 snapshot by version rather than misreading the
  wider record — the property the version gate exists for, exercised in the
  tests.

## Alternatives considered

- **Express "to a guest" with a destination prefix instead of a scope.**
  Rejected as the *only* mechanism: the tap pool reassigns
  `10.0.(3+k).0/24` as guests come and go, so a prefix rule follows whoever
  inherits the subnet, while the scope keeps meaning what it said. The
  prefix stays available for naming one particular guest.
- **Put the chain in `output_on` instead of `ipv4_output`.** Rejected for
  the reason the host-state unit proved: `ipv4_forward` transmits through
  `output_on`, so the chain would silently filter *forwarded* traffic too
  and duplicate FORWARD's job with a different rule set.
- **Silence instead of `-EPERM`, for consistency with the other chains.**
  Rejected: the other chains hide the host from strangers, where silence is
  the security property. Here the refused party is a local socket that
  already distinguishes `-ENETUNREACH` from success, and an operator
  debugging an egress rule needs the error. Silence would also make a
  refused `sendto` indistinguishable from a delivered one.
- **Default DROP with seeded rules, as the INPUT chain did.** Rejected: the
  seeds would have to cover `tapsvc`'s replies, the harness's replies,
  `nat_in`'s deliveries and the host's own resolution — "everything the
  machine currently does", which is a copy of the implementation rather
  than a policy. ACCEPT-then-harden, as the host chain argued.
- **Make TCP report the verdict to `connect`/`send` now.** Rejected for
  scope: `batch_send` would need a per-segment error path into the PCB, and
  the semantics of a rule added *mid-connection* (fail the established
  connection, or merely stop it?) is a design question of its own.
- **A fifth chain for "host → guest" separate from "host → world".**
  Rejected: one chain with a scope in the rule is the same expressiveness
  with one rule list, one default and one call site; two chains would need
  two of each and a rule to decide which sees a datagram first.
