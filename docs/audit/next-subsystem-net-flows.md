# NEXT SUBSYSTEM — the operator cannot see why a guest's flows are refused

> Constitution §68 report. Takes up the deferred-work inventory's row
> "a listing of live flows for the operator" (§1.1, the network arc),
> which three units have named and deferred since the host-state unit
> ("observability, not policy"). Measured first: when a guest's flows are
> refused, **nothing an operator can read changes** -- not the control
> plane's listing, not the kernel log, not sysctl -- while the kernel
> counts every refusal in counters that only the self-tests read.
>
> **Built (PR #243).** As designed, with these differences:
> - **The DNAT listing is checked in `net-tapctl`**, not `net-flows-nat`.
>   That test already creates a DNAT flow through a port-forward and reads
>   the device. It now requires that flow listed as the client's
>   (40001) to the guest (:80) through the dialled port (uplink:8080), and
>   a read one byte short to be `-EMSGSIZE`.
> - **`net-flows-nat` also provokes the table cause.** A masquerading tap
>   forwards only its one guest (`.15`; the anti-spoof rule in
>   `ipv4_forward`), so a ninth source takes a ninth tap: eight more
>   masquerading taps send a share each. Seven fill the table, and the
>   eighth's 32 are refused as `out_drop_table`, none as `out_drop_share`.
>   The firewall's table cause is counted but no test provokes it.
> - **The flow section's scratch arrays are allocated per read**
>   (`tap_ctl_flows`), so a failed allocation is `-ENOMEM`. The listing
>   takes no counters: they are read after it with `nat_get_stats` and
>   `fw_get_stats`.
> - **`NAT_KIND_MASQ`/`NAT_KIND_DNAT` moved to `nat.h`**, where the listing
>   names them.
> - **A port-forwarded TCP flow can now be established** (found in
>   review). DNAT entries never set `tcp_est`: the inbound path said "no est
>   upgrade", and the reply path skipped it. So the listing reported every
>   port-forwarded connection as half-open, and the table kept one for only
>   the half-open 30 s rather than the established 300 s. DNAT now follows
>   masquerade's rule from the other side: the client's ACK without SYN, or
>   any TCP segment the guest sends back, marks the flow established.
>   `net-dnat` checks the reply path: half-open after the SYN, established
>   after the guest's SYN-ACK. `net-tapctl` checks the inbound path: the
>   client's ACK alone makes it established, listed with more than the
>   half-open timeout left.
> - **`vmctl flows` accepts `host`** as well as a guest address. The
>   existing `list` commands needed no change: they compare against the
>   version macro and size their buffers from `COSMO_NETCTL_SNAPSHOT_MAX`,
>   so they read version 6 and stop before the flow section.
> - **Every test that reads the snapshot now reads into a full-size
>   buffer.** `net-tapctl`'s last read, a dead store `gmake analyze` had
>   always flagged, now checks its length.
>
> | mutation | caught by |
> | --- | --- |
> | `nat_flow_list` ignores `expires_ns` | `net-flows-nat`: the listing past the timeout is not empty |
> | `fw_flow_list` ignores `expires_ns` | `net-flows-fw`: the listing an hour ahead is not empty |
> | the NAT table cause counted as the share's | `net-flows-nat`: `out_drop_table` did not rise by 32 |
> | an ambiguous DNAT counted as the share's | `net-dnat`: `dnat_drop_ambiguous` not +1 |
> | the firewall's share and table causes swapped | `net-flows-fw`: `flow_drop_share` did not rise |
> | DNAT's `src`/`dst` not swapped | `net-tapctl`: the flow not seen as the client's |
> | a host flow listed under its opener's address | `net-flows-fw`: `dev_host` |
> | `est` not carried | `net-flows-fw`: `tcp_est` |
> | the flow section's room check removed | `net-tapctl`: the one-byte-short read not `-EMSGSIZE` |
> | `vmctl flows` refusing version 6 | the shell harness: the counter lines missing |
> | DNAT's inbound ACK not marking the flow established | `net-tapctl`: (3c) not established |
> | DNAT's reply not marking the flow established | `net-dnat`: not established after the SYN-ACK |
>
> Each mutation (twelve) ran alone on an x86-64 debug boot, with the boot
> confirmed. Where the failing test returns early (`net-tapctl`,
> `net-flows-fw`), later network tests also fail on the state it left
> behind, as they do for the existing tests. The first failure is the one
> named above. Both architectures pass in debug. Both release boots run
> `vmctl flows` in the harness and pass. `gmake host-test` passes, and
> `gmake analyze` is clean on both architectures.

## Problem

A guest gets a share of two bounded tables: NAT's conntrack
(`NAT_QUOTA_PER_GUEST`, 32 of 256 entries) and the firewall's flow table
(`FW_FLOW_QUOTA_PER_GUEST`, 32 of the guests' 256, plus the host's own
64). A guest that fills its share has every new flow of that kind
dropped. That is the design working: a flood starves only itself. But
the operator of the machine is the one who hears "my guest can't reach
anything", and today they have no way to find out that the share is
full, which flows fill it, or when those flows expire.

### Measured

`tools/net-visibility-probe.py` patches `net-nat`'s quota step, which
already floods one test guest with 264 distinct UDP flows
(`NAT_TABLE_SIZE + 8`), so that it reads, before and after the flood:

- the control plane's listing, `/dev/net/tapctl`, through the device's
  own read routine (`tap_ctl_read`);
- the kernel log, from a marker line written just before the flood to
  the end;
- the kernel's own NAT counters, for contrast.

The probe's `apply` command also lists the names the native sysctl table
serves, read from `kernel/syscall/native.c`. Result,
identical on **x86-64 and AArch64** (one debug boot each; the step is
deterministic):

```
applied; sysctl serves 33 names, 1 under net.: net.steer
NVPROBE nat: entries 0 -> 32 (quota 32), out_drop_full +232
NVPROBE tapctl: snapshot 24 bytes before, 24 after, identical: yes
NVPROBE dmesg: 0 lines between the mark and the end, 0 naming nat
```

The guest's share filled (32 entries) and **232 flows were refused**.
The operator's listing is **byte-identical** before and after. The log
gained **no line**, and sysctl's only network name is `net.steer`.

**The counters exist and nobody outside the tests reads them.** The
stack has eight statistics getters -- `arp_get_stats`, `fw_get_stats`,
`ipv4_get_stats`, `ipv6_get_stats`, `nat_get_stats`, `tapsvc_get_stats`,
`tcp_get_stats`, `udp_get_stats`. Each is called only from its own file
and from `kernel-services/network/nettest.c`. No syscall, device or file
reaches them.

**Nor would the counters answer the question if they could be read.**
They lump different causes together:
- `nat_stats.out_drop_full` counts the refusals above, where the guest's
  share was full (`nat.c:399`). It also counts the whole table being
  full (`nat_alloc`, `nat.c:278`), which is a different diagnosis: one
  guest against every guest.
- `dnat_drop_full` counts three causes in `nat_dnat_create`: a new
  inbound flow whose reverse key already exists (`nat.c:242`, refused
  because the reply could not be told apart), the target guest's full
  share (`:249`) and a full table (`:254`).
- `fw_stats.flow_drop_full` counts both of the reasons `flow_slot`
  returns NULL (`fw.c:470`): the initiator's share full, and no free
  slot anywhere in the table.
- `fw_stats.hin_flow_drop_full` is not a refusal at all. When the host's
  share is spent, the host's datagram still goes out; only its flow goes
  unrecorded, so the reply is judged by the host chain's rules
  (`fw.c:706`). `userland/networking/` holds only a README ("Network configuration
and diagnostic tools").

### Why it matters

- **A refusal the operator cannot see looks like a broken network.**
  The quota is a deliberate isolation boundary (the multi-guest unit,
  #100). Its failure mode is silence: no log line, no error the guest
  can read as "your share is full", and nothing on the host side either.
- **The control plane can change what it cannot show.** `vmctl` can add
  port-forwards and firewall rules, and a rule change can end a flow or
  keep one alive, but there is no view of the flows the rules act on.
- **Three units named this gap and deferred it**: host-state
  ("a `vmctl` view of live flows is a later unit if ever wanted"), the
  tcp-verdict report, and the network design's own "Named and deferred".

## Current implementation

- **NAT** (`kernel-services/network/nat.c`): `g_nat[NAT_TABLE_SIZE]`
  under `g_nat_lock`. Each `struct nat_entry` has a kind (`MASQ`
  outbound, `DNAT` inbound), proto, `orig_ip:orig_port` (the guest),
  `nat_ip:nat_port` (the uplink identity lent for masquerade, or the
  host address and port a client dialled for DNAT),
  `peer_ip:peer_port`, `tcp_est`, and `expires_ns`. **An entry is live
  when `in_use && now < expires_ns`.** An expired entry stays `in_use`
  until `nat_age` or an allocation reclaims it, and
  `nat_guest_count(guest, now)` -- the quota check -- counts only live
  entries.
- **Firewall** (`kernel-services/network/fw.c`): `g_flows[FW_FLOW_MAX]`
  under `g_fw_lock`. Each `struct fw_flow` records the initiator `a`,
  the responder `b`, proto, `est`, `guest_ip` (whose share it counts
  against; `FW_HOST_GUEST_IP` (0) for a flow the host opened), and
  `expires_ns`. The liveness rule is the same.
- **The control channel** (`kernel-services/network/tap.c`,
  `tap_ctl_read`): one read returns one versioned snapshot (ABI version
  5): the port-forward list, then the filter section (policies and
  rules). Its limit is `COSMO_NETCTL_SNAPSHOT_MAX` (7 224 bytes today).
  The read goes through the syscall bounce, which offers an object up to
  64 KiB (`IO_BOUNCE_MAX`). The device is `0600`.
- **`vmctl port-forward list` and `vmctl filter list`** read that
  snapshot into a `COSMO_NETCTL_SNAPSHOT_MAX` buffer and check the
  version.

## Design

### 1. A third section in the snapshot: the flows

`COSMO_NETCTL_VERSION` 5 → 6. After the filter section, the snapshot
carries a flow section:

```c
struct cosmo_netctl_flow_counters {     /* one cause each; machine-wide, never per guest */
    uint64_t masq_new;
    uint64_t masq_drop_share;           /* the guest's share was full */
    uint64_t masq_drop_table;           /* the whole table was full */
    uint64_t masq_drop_noport;          /* no NAT identifier free */
    uint64_t dnat_drop_share;           /* the target guest's share was full */
    uint64_t dnat_drop_table;           /* the whole table was full */
    uint64_t dnat_drop_ambiguous;       /* the reply's reverse key was already in use */
    uint64_t nat_expired;
    uint64_t fw_new;                    /* guest-to-guest flows */
    uint64_t fw_drop_share;             /* refused: the initiator's share was full */
    uint64_t fw_drop_table;             /* refused: no free slot in the table */
    uint64_t fw_host_new;
    uint64_t fw_host_unrecorded;        /* the host's share was full: the datagram was sent, its reply takes the rules */
    uint64_t fw_expired;
};

struct cosmo_netctl_flow_list {     /* version 6 and later */
    uint16_t version;
    uint16_t count;                 /* struct cosmo_netctl_flow following */
    uint16_t nat_quota;             /* NAT_QUOTA_PER_GUEST */
    uint16_t fw_quota;              /* FW_FLOW_QUOTA_PER_GUEST */
    uint16_t fw_host_quota;         /* FW_FLOW_QUOTA_HOST */
    uint16_t reserved;
    struct cosmo_netctl_flow_counters counters;
};

struct cosmo_netctl_flow {
    uint8_t  table;                 /* COSMO_NETCTL_FLOW_NAT / _FW */
    uint8_t  kind;                  /* NAT: MASQ / DNAT; FW: GUEST / HOST */
    uint8_t  proto;                 /* COSMO_NETCTL_PROTO_* */
    uint8_t  flags;                 /* COSMO_NETCTL_FLOW_ESTABLISHED */
    uint32_t guest_addr;            /* whose share it counts against; 0 = the host */
    uint32_t src_addr, dst_addr;    /* the initiator and the responder */
    uint16_t src_port, dst_port;    /* ICMP: the echo id, 0 */
    uint32_t nat_addr;              /* NAT: the uplink identity (MASQ) or the dialled host address (DNAT); FW: 0 */
    uint16_t nat_port;
    uint16_t reserved;
    uint32_t expires_ms;            /* from the read's `now`, rounded up: never 0 for a listed flow */
};
```

The fields are filled from the tables as follows:
- **MASQ**: `src` = guest `orig`, `dst` = `peer`, `nat` = the lent
  uplink address and port.
- **DNAT**: `src` = the client (`peer`), `dst` = the guest (`orig`),
  `nat` = the host address and port the client dialled.
- **Firewall flows**: `src` = `a`, `dst` = `b`, `nat` = 0.

Every flow in both directions is written as "who started it" to "whom".

### 2. The listing agrees with the quota (invariant N24)

**A flow is listed exactly when the quota counts it**: `in_use && now <
expires_ns`, with the same `now` for the whole table. An expired entry
that has not been reaped is not listed, because it no longer holds a
share, and listing it would contradict the refusal the operator is
trying to explain. Each table gets one listing function that takes
`now` explicitly, as `nat_age` and `fw_age` already do:

```c
unsigned nat_flow_list(struct nat_flow *out, unsigned max, uint64_t now);
unsigned fw_flow_list(struct fw_flow_info *out, unsigned max, uint64_t now);
```

Each copies its table under its own lock, so **a table's flows are one
instant**. Across the two tables the snapshot is two instants, NAT's
then the firewall's, the same as today's port-forward and filter
sections. The design says so rather than taking both locks.

**The counters are not the same instant, and the listing does not
pretend they are.** Several are incremented just after the table lock
is released: the share refusals (`nat.c:399`, `fw.c:566`) and the
creations `out_new` (`nat.c:419`) and `hin_flow_new` (`fw.c:712`). So a
snapshot can hold a flow whose creation is not yet counted, or a
refusal counted against occupancy read a moment earlier. The difference
is at most the refusals and creations in flight, one per CPU. The
diagnosis the operator needs does not depend on it: the share line and
the flows are exact under the lock, and a count that is still rising is
still rising. The counters are read with `nat_get_stats` and
`fw_get_stats`, as the tests read them today, and the ABI comment says
they are cumulative and approximate to the instant.

### 3. Counters with one cause each

Today's counters cannot be passed on as they are, because each of three
lumps together causes that call for different diagnoses (see
"Measured"). The unit splits them in the kernel, so each counter the
snapshot carries has one cause:

| today | becomes | cause |
| --- | --- | --- |
| `nat_stats.out_drop_full` | `out_drop_share` (`nat.c:399`) and `out_drop_table` (`nat_alloc`, `:278`) | the guest's share full, or the whole table full |
| `nat_stats.dnat_drop_full` | `dnat_drop_ambiguous` (`:242`), `dnat_drop_share` (`:249`), `dnat_drop_table` (`:254`) | a reverse key in use, the target's share full, the table full |
| `fw_stats.flow_drop_full` | `flow_drop_share` and `flow_drop_table` (`flow_slot` says which) | the initiator's share full, or no free slot |
| `fw_stats.hin_flow_drop_full` | `hin_flow_unrecorded` | not a refusal: the datagram was sent, its flow not recorded |

`flow_slot` today returns NULL for both of its reasons. It gains an out
parameter that says which one. The tests that read the old names
(`net-nat`, `net-dnat`, `net-hoststate`) move to the new ones.

`tap_ctl_read` gets `now` once and writes the section after the filter
section, with the room checks the other sections use. It returns
`-EMSGSIZE` if the buffer is too small. `COSMO_NETCTL_SNAPSHOT_MAX`
grows by the flow list header plus `(NAT_TABLE_SIZE + FW_FLOW_MAX)`
flows: a 128-byte header and 576 × 32 = 18 432 bytes of flows, so
25 784 bytes in all, under the 64 KiB bounce.

### 4. `vmctl flows`

`vmctl flows [GUESTADDR|host]` reads the snapshot and prints, first, one
line per share, then the machine-wide refusal counters. Together they
answer "why is my guest refused":

```
nat   10.0.3.15   32/32
fw    10.0.3.15    0/32
fw    host         3/64
refused: masquerade 232 (share) 0 (table) 0 (no port); port-forward 0 (share) 0 (table) 0 (ambiguous); guest-to-guest 0 (share) 0 (table)
host flows not recorded: 0
```

The counters are the kernel's, which count machine-wide, not per guest,
and each has one cause (§3). A full share next to a rising `share` count
is the diagnosis. A rising `table` count with no share full says the
guests together have filled the table. The kernel does not attribute
each drop to a guest, and this unit does not add that.

Then one line per flow: table, kind, proto, `src -> dst`, the NAT
identity, `est`, and seconds to expiry. Occupancy is counted from the
listing itself, so the share line and the flow lines cannot disagree.
`port-forward list` and `filter list` accept version 6, grow their
buffers to the new maximum, and skip the flow section.

### 5. What is deliberately not in this unit

- **The stack's other counters** (ARP, IPv4/6, TCP, UDP, the tap
  services) and **interfaces, addresses and routes**: a `net.*` sysctl
  family or an `ifconfig`/`netstat` is its own unit. This one answers
  the refusal a quota makes, which the operator cannot find any other
  way.
- **A log line on a refusal**: a flood would turn it into a log flood.
  The counter plus the listing tell the whole story on demand, and a
  rate-limited "share full" notice can be added later if wanted.
- **Killing a flow from the control plane**: the listing makes that
  possible to design (it names each flow). It is not needed to see.

### 6. The §70 gate

**Correctness.** One liveness rule for the quota and the listing (N24),
applied with one `now` per table. One cause per counter (§3).

**Concurrency.** Each table is copied under the lock that its writers
already take. The copy is bounded by the table size (at most 256 or 320
entries) into kernel memory (the bounce buffer), with no fault possible.
There is no new lock and no new order: the two table locks are taken
one after the other, never nested. The counters are atomics read outside
those holds and may lag by the refusals and creations in flight (§2).
The snapshot says so, and nothing the operator concludes depends on the
same instant.

**Ownership and lifetime.** None: values are copied, no pointers.

**Security.** The flows name every guest's peers, so they are listed
only on the `0600` control device, never through sysctl, which any
process can read. A guest cannot read the device.

**Failure.** `-EMSGSIZE` for a short buffer, as today. The bounce's
1 KiB fallback under memory pressure already fails today's larger
snapshots the same way. `vmctl` reports it.

**Performance.** Holding the NAT lock for 256 copies, or the firewall
lock for 320, is on the order of the hold `nat_age` already takes over
the same table. A listing is operator-driven, not per packet.

## Affected files

| file | change |
| --- | --- |
| kernel/include/uapi/cosmo/netctl.h | version 6; the flow section's three structs, kinds, flag, `SNAPSHOT_MAX` |
| kernel-services/network/nat.c, kernel/include/kernel/net/nat.h | `nat_flow_list(out, max, now)`; `out_drop_full` split into `out_drop_share`/`out_drop_table`, `dnat_drop_full` into `dnat_drop_ambiguous`/`_share`/`_table` |
| kernel-services/network/fw.c, kernel/include/kernel/net/fw.h | `fw_flow_list(out, max, now)`; `flow_slot` says which reason; `flow_drop_full` split into `flow_drop_share`/`flow_drop_table`; `hin_flow_drop_full` renamed `hin_flow_unrecorded` |
| kernel-services/network/tap.c | `tap_ctl_read` writes the flow section |
| userland/system/vmctl.c | `vmctl flows [GUESTADDR|host]`; the two existing listings read version 6 unchanged |
| kernel-services/network/nettest.c | the new tests; `netctl_snapshot_len` counts the flow section; `net-nat`, `net-dnat`, `net-hoststate` read the split counters |
| docs | network design (the control channel's snapshot), invariants (N24), testing; `docs/userland/api.md` (`vmctl flows`); the inventory row struck; README Status |

## APIs

- **Userland ABI**: `COSMO_NETCTL_VERSION` 6, a third section in the
  `/dev/net/tapctl` read snapshot. Readers of version 5 are all in this
  tree (`vmctl`) and move with it.
- **In the kernel**: `nat_flow_list` and `fw_flow_list`, each with a
  small value struct for its entries (`struct nat_flow`,
  `struct fw_flow_info`). The split counters in `struct nat_stats` and
  `struct fw_stats` (§3); nothing outside the network stack and its tests
  reads those structs.
- **`vmctl flows [GUESTADDR|host]`**.

## Migration plan

One PR: the ABI, the two listing functions, the snapshot section,
`vmctl`, the tests, the documents.

## Tests

| test | checks | mutation it must catch |
| --- | --- | --- |
| `net-flows-nat` (new) | flood one guest past its NAT share as `net-nat` does. The listing then shows exactly `NAT_QUOTA_PER_GUEST` MASQ flows for that guest, with the flooded source ports and a `nat_port` in the NAT range. `masq_drop_share` rose by the number refused (264 − 32 = 232) and `masq_drop_table` did not rise. Listed at `now + NAT_TIMEOUT_UDP_NS` with nothing aged, the listing is empty while the entries are still `in_use`. A DNAT flow through a port-forward is listed with the client as `src` and the dialled port as `nat_port` | the listing ignores `expires_ns` (lists what the quota no longer counts); the share and table refusals counted in one counter again; DNAT's `src`/`dst` swapped |
| `net-flows-fw` (new) | a guest-to-guest flow (two test taps, FORWARD accept) and a flow the host opened are listed as FW `GUEST` and `HOST`, with their initiators as `src` and `est` set once an ACK passes. A host flow counts against `guest_addr` 0 | the host flow listed under a guest's share; `est` not carried |
| `net-tapctl` (existing) | the snapshot through the device: version 6, three sections, length from its own counts. A buffer one byte short of the whole is `-EMSGSIZE` | the flow section's room check missing |
| boot harness | `vmctl flows` runs in the shell harness and prints its share lines | `vmctl` rejecting version 6 |

## Benchmarks

None: the listing is operator-driven. The NAT table's copy under its lock
is bounded by 256 entries.

## Risks

- **A snapshot that grows with occupancy**: the maximum is fixed by the
  table sizes and fits the bounce with room to spare. The header's
  counts are what was written, as for the other sections.
- **Privacy**: the flow list is the guests' traffic endpoints, so it
  lives on the `0600` device only. The network design's section on the
  control channel says so.

## Alternatives considered

- **`net.*` sysctl names for the counters**: readable by any process,
  and a counter alone says "something was refused", not by whom or for
  what. That is a good later unit for the stack-wide counters, but it is
  not this answer.
- **A log line per refusal, rate-limited**: tells the operator that it
  happened, but not which flows hold the share, and a flood still costs
  log space.
- **A separate `/dev/net/flows` device**: a second privileged node and a
  second ABI for what the control channel's versioned snapshot was
  designed to carry ("the ABI the later network settings will ride",
  the netctl report).
