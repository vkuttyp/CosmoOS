# NEXT SUBSYSTEM — configuring the guest's network at runtime: a control channel

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This is that report, and
nothing in it is implemented.

**Subsystem: a writable control channel the VM owner uses to configure the
guest's networking while it runs — first, adding and removing port-forwards
(DNAT rules) without a reboot — the deliberate control ABI the whole network
arc deferred, designed once, with the privilege and scope model stated.**

## Problem

Everything the guest's network does is fixed at boot. The tap comes up, the
guest autoconfigures, it reaches out through NAT, and it is reachable through
the port-forwards in `fw_cfg` `opt/cosmo/portforward` — but all of that is
decided before the machine starts and cannot change while it runs. An
operator who wants to expose a new guest service, or take one down, must
stop the machine and change its firmware configuration. There is no way to
say "forward host port 8443 to the guest now" to a running system. Every
network unit so far chose this deliberately — a read-only `fw_cfg` surface
cannot be abused, and it kept those units small — and each named "a writable
control surface / a runtime API" as a later unit. This is that unit.

## Current implementation

**The control surfaces are read-only.** `fw_cfg` (`opt/cosmo/*`) is read
once at boot; `/dev/vmm`'s sysctl view is read-only; `/dev/net/tap` carries
frames, not configuration. Nothing a userland owner can write reconfigures
the network. The port-forward table (`nat.c`) has `nat_pf_add` /
`nat_pf_clear`, called only from `tapsvc` at tap setup with the `fw_cfg`
string; there is no remove, no list, and no path from userland to any of it.

**The pieces that exist and are reused:** `nat.c`'s port-forward table and
its target validation (a rule's guest address must be on a connected subnet);
the `ramfs_mkchr` character-device mechanism `/dev/net/tap` already uses (a
`chrdev_ops` with `read`/`write`, a file mode for permissions); the tap and
its guest; the credential model that gates a privileged device by its mode.

## Why it matters

- **A machine an operator can actually run.** Reconfiguring by rebooting is
  fine for a demo and untenable for anything real. Adding and removing a
  port-forward on a running guest is the difference between a fixed appliance
  and a machine an operator administers.
- **It pays the debt the arc kept naming.** The tap, NAT, DHCP/DNS and DNAT
  units each deferred "a writable control surface" and each stayed smaller
  for it. The debt is now concrete and worth paying deliberately, once,
  rather than bolting a one-off onto each feature.
- **It is the ABI the later network settings will ride.** Runtime forwards
  are the first operation; the same channel is where a later unit turns
  forwarding or masquerade on and off, sets the resolver, or brings the tap
  up and down. Designing the channel now, with one operation, sets the shape
  the rest reuse — the same discipline the hypervisor and storage seams used.

## Design (proposed)

### 1. A privileged control device, `/dev/net/tapctl`

A character device created next to `/dev/net/tap` (in `tap_dev_init`), mode
`0600` so only the owner (root / the VM owner) can open it — the same
privilege gate `/dev/net/tap` uses. `write` submits one control command
(§2); `read` returns the live configuration so an operator can see the state.
It is a control channel, entirely separate from the frame channel
`/dev/net/tap`, so configuration and data never mix on one descriptor.

The `read` reply is as much a versioned ABI as the write command: one
`read` returns an atomic snapshot — a fixed `struct cosmo_netctl_list` header
(the same version field as the command, and a rule count) followed by that
many fixed `struct cosmo_netctl_rule` records (`proto, host_port,
guest_addr, guest_port`). A single `read` yields the whole snapshot into the
caller's buffer or `-EMSGSIZE` if it does not fit (as `/dev/net/tap`'s read
does for a frame); there is no partial read or cursor, so the operator never
sees a half-updated table. The count bounds the records (`NAT_PF_MAX`), and
the version lets an old `vmctl` refuse a reply shape it does not know.

### 2. A versioned, structured control message

A `write` carries one fixed-layout command, not a text line, so the ABI is
explicit and checkable (the lesson of the handle-rights unit: a uapi struct
that grows must carry its version). The uapi header
`kernel/include/uapi/cosmo/netctl.h` defines `struct cosmo_netctl` — a
version, an opcode, and the operation's fields — and the first opcodes are
`NETCTL_FORWARD_ADD` and `NETCTL_FORWARD_DEL`, each carrying
`(proto, host_port, guest_addr, guest_port)`. Unknown opcodes and unknown
versions are refused (`-EINVAL` / `-ENOTSUP`), never guessed. A short write,
or one whose fields are out of range, is refused whole; a command is applied
or it is not, never half.

`(proto, host_port)` is the rule's unique key: at most one rule binds a given
protocol and host port. `FORWARD_ADD` of a binding that already exists
(static or runtime) is refused (`-EEXIST`) rather than shadowed — so a listed
rule is always the one that receives traffic, and there is no first-match
ambiguity. `FORWARD_DEL` removes the rule holding a `(proto, host_port)`
whatever its origin (runtime deletion may remove a boot-configured forward:
the operator is privileged and the table is one table); a `DEL` of an absent
binding is `-ENOENT`. Rules carry no static/runtime tag, because the unique
key makes one unnecessary.

### 3. Backed by the existing table, with the same guarantees

`NETCTL_FORWARD_ADD` calls `nat_pf_add` (tightened per §4 to bind only to the
guest tap and to reject a duplicate binding, §2), and refuses a full table;
`NETCTL_FORWARD_DEL` calls a new `nat_pf_del` that removes the rule with the
matching `(proto, host_port)` **and reaps the DNAT conntrack entries that
rule created**, so a removed forward stops immediately — an in-flight flow
included — not after the entries idle out (reaping is mandatory, asserted by
the test, not optional). `read` lists the rules through a new `nat_pf_list`.
Nothing about a rule's meaning changes — this unit is a *path to* the
port-forward table, so a runtime-added forward behaves exactly as a `fw_cfg`
one. The static `fw_cfg` rules remain, read at boot; the control channel adds
and removes on top of them, in the one table.

### 4. Privilege and scope: what it may and may not do

The channel configures the *guest's* networking and nothing of the host's.
A forward's target must be on **the guest tap's own subnet** — not merely
any connected subnet: `netif_connected()` alone would also accept an address
on the uplink or a peer subnet, and a rule pointing there would make the host
relay a public port to another machine on its real network. So `nat_pf_add`
is tightened to validate the target against the guest tap specifically (the
interface the control channel belongs to), rejecting a target that is not on
it. A rule can thus never point at the host's own services, at the uplink, or
off into the default route. The device's `0600` mode confines writing to the
privileged owner; an unprivileged process cannot open it. The channel adds no
operation that touches the host's real interface, its routes, or another
process — it is exactly the port-forward table (and, later, the tap's own
settings), reachable at runtime. It adds **no new system call**: it is a
character device, as the frame channel is.

### 5. The milestone

- **Gated, in the harness:** with no static rule, a command `FORWARD_ADD tcp
  host-port → guest:Q` is submitted through the control device; a client
  connection to that host port is then DNAT'd to the guest (as the existing
  `net-dnat` proves the table behaves). A `FORWARD_DEL` for the same port is
  submitted; the connection is then delivered to the host, not the guest, and
  the rule no longer appears in the `read` listing. A malformed or
  wrong-version command is refused and changes nothing; a forward whose
  target is not on the guest tap's subnet is refused.
- **Demonstrated, reproducible:** `vmctl` gains a `port-forward add|del|list`
  subcommand that writes the control device, and a stock Linux guest's
  service is exposed and then hidden on a running machine under
  `QEMU_MEM=2G`, no reboot.

### 6. Deliberately out of scope

- **The other network settings** (forwarding/masquerade on/off, the resolver,
  the tap's address, bringing the tap up/down) — the channel is designed to
  carry them, but this unit delivers only the port-forward operations; the
  rest are follow-ups on the same ABI.
- **Per-guest or multi-tap control** — the model is still one guest on one
  tap; a control channel that names which guest is the concern of the
  multi-guest unit.
- **A general firewall's rules**, **unprivileged/delegated control**, and a
  **query/subscribe interface** for live statistics — named, later.

## Affected files

- `kernel-services/network/tap.c` — create `/dev/net/tapctl` in
  `tap_dev_init`; the control `chrdev_ops` (`write` applies a command, `read`
  lists rules).
- `kernel-services/network/nat.c`, `kernel/include/kernel/net/nat.h` —
  `nat_pf_del` (remove a rule by `(proto, host_port)` and reap its conntrack
  entries) and `nat_pf_list` (snapshot the rules); `nat_pf_add` tightened to
  bind only to the guest tap's subnet and to reject a duplicate
  `(proto, host_port)`.
- `kernel/include/uapi/cosmo/netctl.h` (new) — `struct cosmo_netctl`, the
  opcodes and the version.
- `userland/system/vmctl.c` — a `port-forward add|del|list` subcommand.
- `kernel-services/network/nettest.c`, `kernel/core/selftest.c`,
  `selftest.h` — the `net-tapctl` self-test.
- `docs/kernel-services/network/`, `docs/kernel-services/virtualization/`,
  `README.md` — the design and the Status entry.

## New APIs

A new **character device** `/dev/net/tapctl` (a control ABI, `struct
cosmo_netctl` in the uapi), not a system call and not a new syscall ABI. Its
kernel surface is `nat_pf_del` / `nat_pf_list` beside the existing
`nat_pf_add`. The device is privileged by its `0600` mode.

## Migration plan

1. **`nat_pf_add` tightened, `nat_pf_del`, `nat_pf_list`** in `nat.c`, with a
   test: a target off the guest tap's subnet is rejected; a duplicate
   `(proto, host_port)` is rejected (`-EEXIST`); a rule added then deleted no
   longer matches, its conntrack entries reaped so an in-flight flow stops;
   `nat_pf_list` snapshots the rules.
2. **The control device and its message**: `/dev/net/tapctl`, the
   `cosmo_netctl` parse (version and opcode checked, fields range-checked, a
   short or unknown command refused whole), and the versioned `read` reply
   (a `cosmo_netctl_list` header + `cosmo_netctl_rule` records, whole snapshot
   or `-EMSGSIZE`).
3. **End to end through the device**: a `FORWARD_ADD` submitted through
   `/dev/net/tapctl` makes a connection DNAT to the guest; a `FORWARD_DEL`
   stops it; proved by `net-tapctl`. Each behaviour bug-proved by
   reintroducing its bug.
4. **`vmctl port-forward`** and the Linux demonstration, documented and
   reproducible under `QEMU_MEM=2G`.
5. **Docs and the Status entry**, and the full verification chain.

## Tests

- `net-tapctl` (host): a `FORWARD_ADD` command applied through the control
  path installs a rule (a subsequent client connection is DNAT'd to the
  guest, as `net-dnat`'s machinery shows); a `read` returns the versioned
  list with that one rule; a `FORWARD_DEL` removes it (the connection then
  stays local, an in-flight flow is dropped by the reap, and the listing is
  empty); a duplicate `(proto, host_port)` `ADD` is `-EEXIST`; a malformed
  command, a wrong version, and a target off the guest tap's subnet are each
  refused and change nothing. Each behaviour and refusal bug-proved by
  reintroducing its bug.
- The existing net, NAT and DNAT tests and a net-less boot stay green; the
  control device does nothing until written.

## Benchmarks

None meaningful — a control operation is rare and not on the packet path.

## Risks

- **A writable surface is an attack surface.** The whole arc avoided one on
  purpose. The mitigations are explicit: the device is privileged (`0600`,
  the owner only); every command is a fixed, version-checked, range-checked
  struct refused whole on any doubt; and the operations are confined to the
  guest's port-forward table with the guest-tap-subnet target check (§4,
  tightened from the connected-subnet check the DNAT unit shipped),
  so nothing a caller writes can reconfigure the host's own network or reach
  another process.
- **A removed forward that keeps working.** Deleting a rule must also stop
  flows it already created, or a "closed" port stays open through live
  conntrack entries; `nat_pf_del` reaps the entries the rule made, and the
  test asserts an in-flight flow stops.
- **ABI drift.** A control struct that grows silently breaks already-built
  callers (the handle-rights lesson). The struct is versioned and the kernel
  refuses a version it does not know, so an old `vmctl` and a new kernel — or
  the reverse — fail cleanly rather than misreading fields.
- **Overselling.** The gated test drives the control device from the harness;
  the `vmctl port-forward` on a running Linux guest is the `QEMU_MEM=2G`
  reproduction, as the arc's other units said of theirs.

## Alternatives considered

- **A new system call** (`net_config(...)`). A first-class ABI, but it adds a
  syscall to the table for what a device write does, and the arc's control
  surfaces have been devices and `fw_cfg`, not calls. Rejected as heavier
  than the problem; the character device reuses the exact mechanism the frame
  channel uses.
- **Extending `/dev/net/tap` with control messages** (an `ioctl`, or a magic
  frame). Rejected: mixing configuration and frame data on one descriptor is
  the ambiguity a separate control channel exists to avoid, and the tap's
  `read`/`write` already mean "a frame."
- **A writable `fw_cfg`-style / sysctl node.** `/dev/vmm`'s surface is
  read-only by design; making a sysctl writable would blur the read-only
  contract the rest of the system relies on. A dedicated control device keeps
  the writable surface named and contained.
- **A line-oriented text command** (`"forward add tcp 8080 …"`). Friendly to
  type, but an unversioned, free-form surface is exactly what drifts; a
  structured, versioned message is the deliberate ABI this unit is for.
  `vmctl` gives the operator the friendly form on top of it.
- **Nothing (static configuration only).** What exists. Rejected because a
  machine reconfigured only by reboot is not one an operator runs, and the
  arc explicitly named this as the next step.
