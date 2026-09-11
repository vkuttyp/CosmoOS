# NEXT SUBSYSTEM — from one guest to many: a tap per guest

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This is that report, and
nothing in it is implemented.

**Subsystem: concurrent guests. Every VM owner that opens `/dev/net/tap`
gets a tap of its own, on a subnet of its own, with its own DHCP/DNS
service, its own share of the NAT table and its own port-forwards — so two
(or eight) stock guests run at once, each a full machine on the network,
neither able to starve the other nor to see the other's frames — guests
reach each other only as two machines on adjacent subnets do, over routed
IP, and a policy forbidding even that is a later unit. The first unit of the
multi-guest arc, and the one that removes the "one guest" assumption every
network unit has carried.**

## Problem

The hypervisor can run many machines; the network can serve one. The EL2
backend hands out up to sixteen VMIDs and nothing stops two `vmctl` runs
from creating two VMs — but both would open the same `/dev/net/tap`, which is
one persistent `tap0` on one subnet with one DHCP lease, and their frames
would interleave on one channel. The tap unit chose that deliberately ("one
persistent tap is enough for one guest") because the ramfs character device
has no open/close lifecycle to hang a per-owner tap on, and every later unit
inherited the assumption: `tapsvc` is a singleton with one DHCP binding and
one DNS proxy; the NAT report noted that with one guest "the table's flows are
that guest's, so it starves only itself" and deferred a per-client quota "to
the unit that forwards for more than one client." This is that unit. The
guest is a full machine on the network; the host should be able to run more
than one.

## Current implementation

**One tap, created at boot, never destroyed.** `tap_dev_init` creates `tap0`
(`10.0.3.0/24`, host `.1`, guest `.15`) and `/dev/net/tap`; `tap_dev_activate`
brings it up on first use and starts `tapsvc` on it. A second owner opening
`/dev/net/tap` shares the same tap: its frames and the first owner's mix on
one queue and one subnet.

**No per-open state in the VFS.** `struct chrdev_ops` has `read` and `write`
only; `struct file` (the per-open object) carries a vnode, a position and
flags, and no driver-private slot. A device cannot tell two opens apart, so
it cannot give each its own instance or tear one down when its owner closes.

**Singletons downstream.** `tapsvc` keeps one `g_svc` (one DHCP binding, one
guest slot) and one `g_dns` (one gateway socket, one thread pair, one pending
table); `tapsvc_start` refuses a second instance. `nat.c`'s conntrack table is
one bounded table with no per-guest cap. `nat_out` masquerades any forwarded
flow whose source is off the egress subnet — which, with two taps, would also
masquerade guest-to-guest traffic to a tap gateway address.

**What already generalises.** Connected longest-prefix routing sends a
reply to whichever tap holds its subnet; masquerade and DNAT key their flows
by guest address; `nat_pf_add` validates a target against *a* forwarding tap;
`netctl` names the guest by address; the hypervisor's VMIDs; `vmctl` opens
the tap once per run — so one open per guest is exactly the seam.

## Why it matters

- **A host, not a demo.** One VM is a demonstration; several, each networked
  and each isolated, is what a machine that hosts guests *is*. It is the step
  from "a guest" to "guests."
- **It retires the assumption instead of patching it.** Every network unit
  carried "one guest" as a note in its risks. Making the tap per-owner removes
  the note at the source; the downstream units then generalise or gain the one
  bound (a per-guest NAT quota) they each said they would need.
- **A per-open device lifecycle is worth more than taps.** `/dev/net/tap` is
  the first device that needs "an instance per open, destroyed on last
  close" — the same shape a future per-open VM control channel or a
  per-owner console would need. Building the hook once, in the VFS, is the
  reusable half of this unit.

## Proposed design

### 1. A per-open lifecycle for character devices

`struct chrdev_ops` gains `open` and `release`, and `struct file` gains a
driver-private pointer the device sets in `open` and receives in every
`read`/`write` and in `release`. `open` runs on each open of the node and may
refuse (`-EBUSY`, `-ENOSPC`); `release` runs when the last reference to that
`struct file` drops — the file is a `kobject`, so "last close" is already a
defined moment. Devices without the hooks behave exactly as today (the frame
channel, `/dev/net/tapctl`, `/dev/vmm` are untouched by default). This is a
small VFS addition with one new invariant: a device's per-open state lives
and dies with its `struct file`, never with the vnode.

### 2. A tap per open of `/dev/net/tap`

`/dev/net/tap`'s `open` creates a tap for that owner — `tap<k>` on the next
free subnet from a pool `10.0.(3+k).0/24` (host `.1`, guest `.15`, the same
convention as today's `tap0`, which becomes simply the first), up to a cap
`TAP_MAX_GUESTS` (8, comfortably under the sixteen VMIDs) — marks it
`NETIF_FORWARD | NETIF_MASQUERADE | NETIF_NODEFAULT`, and starts a `tapsvc`
instance for it. `read`/`write` act on *that* file's tap. `release` stops its
service, destroys the tap (unregister, drop the reference) and returns the
subnet to the pool. So `vmctl` — which opens `/dev/net/tap` once per run —
gets a private, isolated channel with no change to itself, and two `vmctl`
runs are two guests. A ninth open is refused (`-ENOSPC`). The
boot-time persistent `tap0` and its "no close hook" rationale go away; a tap
exists exactly while an owner holds the channel.

### 3. `tapsvc` per tap

The singleton becomes an instance per tap: each tap carries its own DHCP
binding (one guest slot on *its* subnet — the address handed out is derived
from the tap's own subnet, as today) and its own DNS proxy (a socket bound to
*its* gateway address on port 53, its own pending table, its own thread
pair). Per-tap gateway binding is deliberate — a single proxy on
`0.0.0.0:53` would also answer DNS on the host's real interface, exposing a
resolver to the LAN. Threads are bounded by the guest cap (two per tap).
`tapsvc_start(t)` / `tapsvc_stop(t)` take the tap; the state hangs off it.

### 4. NAT for many guests: a quota, no masquerade between taps, a guest-scoped purge

Three changes in `nat.c`; the first two named by earlier units, the third
forced by guests that now come and go:

- **A per-guest quota.** The conntrack table stays one bounded table, but no
  single guest (source address) may hold more than `NAT_TABLE_SIZE /
  TAP_MAX_GUESTS` masquerade entries; a guest at its quota has its *new*
  flows dropped while every other guest's continue. This is the per-client
  fairness the NAT report deferred to exactly this unit: one guest's flood
  starves only itself, as before, now with more than one guest present.
- **Masquerade only toward the uplink.** A flow forwarded from one tap out
  *another tap* (guest-to-guest, which connected routing now makes reachable)
  is not masqueraded — the egress is a forwarding tap, not the uplink — so
  guests see each other's real addresses. Masquerade applies when the egress
  is the uplink (not `NETIF_FORWARD`), as intended.
- **A guest-scoped purge, `nat_guest_purge(guest_ip)`.** Today NAT state can
  only be aged or flushed *globally*, a DNAT rule removed only by its own
  `(proto, host_port)` key, and unregistering an interface clears neither —
  so a departing guest would leave stale flows and rules behind, and a later
  guest handed the same subnet would inherit them. `nat_guest_purge` removes
  every DNAT rule whose target is the guest and every conntrack entry
  (masquerade or DNAT) whose guest side is that address, and nothing else;
  the tap's `release` calls it before the subnet returns to the pool. Other
  guests' state is untouched — this is what lets a shared table be safely
  reused without flushing it under the guests that remain.

DNAT and `netctl` keep their ABI unchanged: a rule names its guest by
address, and `nat_pf_add` already requires the target to be on a forwarding
tap — now any of them. The purge is the only new NAT entry point.

### 5. Isolation between guests

The boundary this unit draws is exact, and it is the one the subsystem
statement makes: each guest has its **own channel** (it sees only its own
frames — a per-open tap), its **own lease and identity** (its own subnet and
DHCP binding), its **own share** (the NAT quota), and is reachable from
outside only through rules that name *its* address. What it does **not**
draw is network isolation between guests: guest-to-guest traffic is routed by
the host (both taps forward) with real addresses, exactly as two machines on
adjacent subnets reach each other. That is connectivity, not visibility — a
guest cannot read another's frames or take its lease, but it can address its
services. A policy that forbids inter-guest traffic is the later firewall
unit; this unit gives guests distinct, addressable identities and leaves
reachability on.

### 6. The milestone

- **Gated, in the harness:** two opens of `/dev/net/tap` (through the VFS,
  as `vmctl` does) yield two taps on two distinct subnets; each synthetic
  guest DHCPs its own address from its own service; each reaches an uplink
  peer through masquerade (distinct lent ports, distinct entries); each is
  reachable through its own DNAT rule; a flood from guest A fills A's quota
  and A's new flows drop while a flow from guest B still passes; A and B can
  reach each other with real (un-masqueraded) addresses; closing A's channel
  destroys A's tap and service and frees its subnet, while B keeps working;
  a ninth open is refused.
- **Demonstrated, reproducible:** two stock Linux guests under two `vmctl`
  runs at once, each autoconfigured, each reaching the world, each with a
  forwarded service — under `QEMU_MEM=2G` each, the same reproduction shape
  as the arc's other units.

### 7. Deliberately out of scope

- **An L2 bridge** (guests on one shared subnet seeing each other at link
  layer). This unit gives each guest its own routed subnet; a bridge is a
  later unit if shared-L2 semantics are ever needed.
- **Guest-to-guest policy** (a firewall forbidding or allowing inter-guest
  traffic) — named, later; this unit makes guests addressable.
- **Per-guest resource limits beyond the NAT quota** (CPU, memory quotas
  across VMs) and **a VM/guest registry or naming** (`vmctl` addressing a
  guest by name) — later units on this foundation.

## Affected files

- `kernel/include/kernel/vfs.h`, `kernel-services/vfs/vfs.c`,
  `kernel-services/vfs/ramfs.c` — `chrdev_ops.open`/`release`, the per-open
  private slot on `struct file`, `release` on the file's last reference.
- `kernel-services/network/tap.c` / `tap.h` — a tap per open (subnet pool,
  `TAP_MAX_GUESTS`, create on `open`, destroy on `release`), the end of the
  persistent `tap0`.
- `kernel-services/network/tapsvc.c` / `tapsvc.h` — per-tap instances
  (DHCP binding and DNS proxy state, threads and sockets per tap).
- `kernel-services/network/nat.c` / `nat.h` — the per-guest quota;
  `nat_guest_purge` (rules and conntrack of one guest); masquerade
  only toward a non-forwarding egress.
- `kernel-services/network/nettest.c`, `kernel/core/selftest.c`,
  `selftest.h` — the `net-multiguest` self-test (and adjustments to the
  existing tap/DHCP/DNS/NAT tests that assumed a singleton `tapsvc`).
- `docs/kernel-services/network/`, `docs/kernel-services/virtualization/`,
  `docs/kernel-services/vfs/`, `README.md` — the design and the Status entry.

## New APIs

No new system call and no new control ABI: `vmctl` and the guest are
unchanged (an open of `/dev/net/tap` now yields a private tap). The kernel
gains `chrdev_ops.open`/`release` and a per-open private pointer on `struct
file` — a VFS contract for devices with per-open instances — and `tapsvc_start`
/ `tapsvc_stop` take the tap they serve.

## Migration plan

1. **The per-open chrdev lifecycle** in the VFS: `open`/`release` hooks and
   the per-file private slot, with a test (a synthetic device counts opens
   and releases; two opens see distinct private state; `release` fires on the
   last close and not before).
2. **A tap per open**: `/dev/net/tap` creates a tap from the subnet pool on
   `open`, destroys it on `release` — stopping its service and purging its
   NAT/DNAT state (step 4's `nat_guest_purge`) *before* the subnet is freed —
   and refuses past the cap; `tap0` at boot goes away. Proved by two opens
   yielding two taps on distinct subnets, the ninth refused, a close
   destroying only its own with its state gone and the other's intact.
3. **`tapsvc` per tap**: DHCP and DNS state per instance; each guest gets its
   own lease and resolver. Proved by two guests each completing DHCP and a
   DNS round trip through their own service.
4. **NAT for many**: the per-guest quota, uplink-only masquerade, and
   `nat_guest_purge`. Proved by A's flood dropping only A's new flows while
   B's pass, by guest-to-guest traffic arriving un-masqueraded, and by a
   purge of A removing A's rules and flows while B's remain. Each bound
   bug-proved.
5. **The two-guest Linux demonstration**, documented and reproducible under
   `QEMU_MEM=2G`; then docs and the Status entry, and the full chain.

## Tests

- `vfs-chrdev-open` (host): a synthetic character device's `open` runs per
  open with distinct private state, `read`/`write` see their own file's
  state, `release` runs exactly once on the last close.
- `net-multiguest` (host): two opens of `/dev/net/tap` → two taps on distinct
  subnets; per-tap DHCP and DNS round trips; masquerade with distinct entries;
  per-guest DNAT; the quota (A's flood drops only A's new flows, B's flow
  passes); un-masqueraded guest-to-guest reachability; closing A tears down
  only A — its tap, its service, its DNAT rules and its conntrack entries
  purged, while B's flows and rules survive; the ninth open refused. Each
  behaviour and bound proved by reintroducing its bug.
- The existing tap, DHCP, DNS, NAT, DNAT and control-channel tests stay
  green, adjusted where they assumed the singleton.

## Benchmarks

Per-guest forwarded throughput with one and with two guests active, recorded
so a later regression (or the bridge unit) has a baseline. No absolute
target.

## Risks

- **A per-open lifecycle is a new VFS contract.** `release` must fire exactly
  once, on the last reference, and never while a `read`/`write` on that file
  is in flight; the file is a `kobject`, so its refcount is the arbiter, and
  the test asserts once-and-only-once.
- **A tap destroyed under a running service.** `release` must stop the tap's
  `tapsvc` (join its DNS threads, drop its DHCP filter) *before* the tap is
  unregistered, or a thread wakes on a freed socket; the order is stop, then
  destroy, as the singleton's `tapsvc_stop` already does.
- **A subnet reused too early.** Returning a subnet to the pool while a
  guest's NAT/DNAT entries still name its old address could let a new guest
  inherit flows; `release` calls `nat_guest_purge` — the guest-scoped
  removal §4 adds, since nothing existing can clear one guest's state without
  flushing everyone's — before the subnet is freed.
- **Fairness that is only a cap.** The per-guest quota bounds each guest's
  share of the table; it is not a scheduler. It guarantees B is never starved
  by A, not equal throughput. The report says so.
- **Overselling.** The gated test runs two synthetic guests on two taps; two
  real Linux guests at once is the `QEMU_MEM=2G` reproduction, as the arc's
  other units said of theirs.

## Alternatives considered

- **`/dev/net/tap0`, `/dev/net/tap1`, … as separate boot-time nodes.** No VFS
  change, but a fixed set of always-present taps with no lifecycle — an owner
  must be told which to open, a departing guest leaves its tap and lease up,
  and the count is baked in. Rejected: the per-open instance is the honest
  model (a tap exists while an owner holds it) and the VFS hook is reusable.
- **One shared subnet behind an in-kernel L2 bridge.** Guests see each other
  at link layer, one DHCP pool serves all. Larger (a bridge, MAC learning, one
  pool with leases), and it removes the per-guest isolation a routed design
  gives for free. Deferred as a later unit; the routed per-subnet model is the
  smaller first step and composes with it.
- **A user-space switch in `vmctl`** (one owner process multiplexing several
  guests over one tap). Rejected as the userland-NAT alternative was: it
  re-implements the stack's job in the owner and keeps the kernel at one
  guest.
- **Only the NAT quota, keeping one tap.** Fixes the starvation note but not
  the actual limit; two owners would still share one channel. Rejected.
