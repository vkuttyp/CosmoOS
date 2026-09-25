# NEXT SUBSYSTEM — the operator cannot see why a guest's flows are refused

> Constitution §68 report. Takes up the deferred-work inventory's row
> "a listing of live flows for the operator" (§1.1, the network arc),
> which three units have named and deferred since the host-state unit
> ("observability, not policy"). Measured first: when a guest's flows are
> refused, **nothing an operator can read changes** -- not the control
> plane's listing, not the kernel log, not sysctl -- while the kernel
> counts every refusal in counters that only the self-tests read.

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

`apply` also lists the names the native sysctl table serves. Result,
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
and from `kernel-services/network/nettest.c`. `nat_stats.out_drop_full`,
`dnat_drop_full`, `fw_stats.flow_drop_full` and `hin_flow_drop_full`
count exactly the refusals above, and no syscall, device or file reaches
them. `userland/networking/` holds only a README ("Network configuration
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
struct cosmo_netctl_flow_counters {
    uint64_t nat_new, nat_drop_full, nat_drop_noport;   /* outbound masquerade: out_new, out_drop_full, out_drop_noport */
    uint64_t dnat_drop_full;                            /* inbound port-forwards refused */
    uint64_t nat_expired;
    uint64_t fw_new, fw_drop_full;                      /* guest-to-guest flows */
    uint64_t fw_host_new, fw_host_drop_full;            /* flows the host opened */
    uint64_t fw_expired;
};   /* machine-wide, as the kernel counts them: nat_stats and fw_stats, never per guest */

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
unsigned nat_flow_list(struct nat_flow *out, unsigned max, uint64_t now, struct nat_stats *stats);
unsigned fw_flow_list(struct fw_flow_info *out, unsigned max, uint64_t now, struct fw_stats *stats);
```

Each copies its table under its own lock, **in one hold together with
that table's counters**. So within one table, the flows and the counters
are the same instant: a refusal counted is a refusal the listed
occupancy explains. Across the two tables the snapshot is two instants,
NAT's then the firewall's, the same as today's port-forward and filter
sections. The design says so rather than taking both locks.

`tap_ctl_read` gets `now` once and writes the section after the filter
section, with the room checks the other sections use. It returns
`-EMSGSIZE` if the buffer is too small. `COSMO_NETCTL_SNAPSHOT_MAX`
grows by the flow list header plus `(NAT_TABLE_SIZE + FW_FLOW_MAX)`
flows: a 96-byte header and 576 × 32 = 18 432 bytes of flows, so 25 752
bytes in all, under the 64 KiB bounce.

### 3. `vmctl flows`

`vmctl flows [GUESTADDR]` reads the snapshot and prints, first, one
line per share, then the machine-wide refusal counters. Together they
answer "why is my guest refused":

```
nat   10.0.3.15   32/32
fw    10.0.3.15    0/32
fw    host         3/64
refused: nat 232 (masquerade), 0 (no port), 0 (port-forward); fw 0 (guests), 0 (host)
```

The counters are the kernel's, which count refusals machine-wide, not per
guest. A full share next to a rising count is the diagnosis; the kernel
does not attribute each drop, and this unit does not add that.

Then one line per flow: table, kind, proto, `src -> dst`, the NAT
identity, `est`, and seconds to expiry. Occupancy is counted from the
listing itself, so the share line and the flow lines cannot disagree.
`port-forward list` and `filter list` accept version 6, grow their
buffers to the new maximum, and skip the flow section.

### 4. What is deliberately not in this unit

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

### 5. The §70 gate

**Correctness.** One liveness rule for the quota and the listing (N24),
applied with one `now` per table.

**Concurrency.** Each table is copied under the lock that its writers
already take. The copy is bounded by the table size (at most 256 or 320
entries) into kernel memory (the bounce buffer), with no fault possible.
There is no new lock and no new order: the two table locks are taken
one after the other, never nested.

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
| kernel-services/network/nat.c, kernel/include/kernel/net/nat.h | `nat_flow_list(out, max, now, stats)` |
| kernel-services/network/fw.c, kernel/include/kernel/net/fw.h | `fw_flow_list(out, max, now, stats)` |
| kernel-services/network/tap.c | `tap_ctl_read` writes the flow section |
| userland/system/vmctl.c | `vmctl flows [GUESTADDR]`; the two existing listings accept version 6 |
| kernel-services/network/nettest.c | the new tests; `netctl_snapshot_len` counts the flow section |
| docs | network design (the control channel's snapshot), invariants (N24), testing; `docs/userland/api.md` (`vmctl flows`); the inventory row struck; README Status |

## APIs

- **Userland ABI**: `COSMO_NETCTL_VERSION` 6, a third section in the
  `/dev/net/tapctl` read snapshot. Readers of version 5 are all in this
  tree (`vmctl`) and move with it.
- **In the kernel**: `nat_flow_list` and `fw_flow_list`, each with a
  small value struct for its entries (`struct nat_flow`,
  `struct fw_flow_info`).
- **`vmctl flows [GUESTADDR]`**.

## Migration plan

One PR: the ABI, the two listing functions, the snapshot section,
`vmctl`, the tests, the documents.

## Tests

| test | checks | mutation it must catch |
| --- | --- | --- |
| `net-flows-nat` (new) | flood one guest past its NAT share as `net-nat` does. The listing then shows exactly `NAT_QUOTA_PER_GUEST` MASQ flows for that guest, with the flooded source ports and a `nat_port` in the NAT range. `nat_drop_full` rose by the number refused (264 − 32 = 232). Listed at `now + NAT_TIMEOUT_UDP_NS` with nothing aged, the listing is empty while the entries are still `in_use`. A DNAT flow through a port-forward is listed with the client as `src` and the dialled port as `nat_port` | the listing ignores `expires_ns` (lists what the quota no longer counts); the counters copied outside the table's lock hold (the drop count and the occupancy from different instants); DNAT's `src`/`dst` swapped |
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
