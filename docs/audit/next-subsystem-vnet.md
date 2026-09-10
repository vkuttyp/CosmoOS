# NEXT SUBSYSTEM — a guest network interface: virtio-net, so the transport carries a second device

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This is that report, and
nothing in it is implemented.

**Subsystem: a virtio-net device for the guest — a second device on the
virtio-mmio transport, with its receive and transmit queues, so a guest
has a network interface and the transport is shown to carry more than one
kind of device.**

## Problem

A guest now has a CPU, interrupts, a timer, a console, a device tree, a
feature model, and a disk it can read and write. What a machine still
lacks is a network interface. Every device the guest has is a register
file the owner completes in place, or — since the root-filesystem unit —
one block device with a data plane. There is no way for a guest to send
or receive a packet, and so nothing a guest reaches userspace to do can
involve the network: no `ping`, no `ssh` in, no package fetched, no
service answered.

There is a second, quieter problem. The virtio-mmio transport, the
device-side virtqueue walk, and the hardened ring disciplines were all
built for one device — block. `drivers/network/README.md`'s Invariant 5
says the host's network stack does not depend on a specific NIC; the
guest side has the mirror claim to earn — that the *transport* does not
depend on a specific device. A transport that has only ever carried one
device is a guess about what a transport should be. A second device is
what tests the seam.

## Current implementation

**There is no guest network device of any kind.** The device tree
advertises one `virtio_mmio` node (block, `COSMO_HVM_VIRTIO0_*`); there is
no second node, no second transport window, and no net device model in
`vmctl`.

**The device-side virtqueue walk is block-specific.** `userland/system/vblk.c`'s
`serve_one` reads a `virtio_blk_req_hdr`, branches on `T_IN`/`T_OUT`/`T_FLUSH`,
and writes a status byte — the shape of a *block request*. A network
device has no requests and no status byte: its queues carry frames, each
prefixed by a `virtio_net_hdr`, and the direction is fixed by the queue
(receive or transmit), not read from a header.

**What is already built and directly reusable** is everything below the
per-request semantics: the split-virtqueue layout and the descriptor
walk, the used ring, the SPI the owner raises through the guest's
distributor (`cosmo_vm_raise_spi`), and — earned over seven review rounds
on the block device — the ring disciplines: every index bounded by the
queue size, a looping chain refused, a buffer outside guest RAM faulted
through the callback, the available-ring backlog bounded, a per-request
length cap and a per-notification work ceiling, the atomic publish
(advance `used_idx`/`last_avail` only after both used-ring writes land),
and a fault reported as failure, never as partial progress. These live
above the block/net distinction and must be shared, not reimplemented.

**The transport model handles one queue.** `vmctl`'s `vio_reg` addresses a
single queue (its `QueueNum`/`QueueReady`/notify all act on one ring). A
net device has two queues — receive and transmit — so the transport gains
`QueueSel` and per-queue state.

## Why it matters

- **A guest that can talk.** A network interface is the difference between
  a machine that computes in isolation and one that participates. Even
  the loopback core of this unit lets a guest's `ip`, `ifconfig` and
  `ping 127.0.0.1`-of-its-own-address work; the follow-up that bridges to
  the host's network lets it reach the world.
- **The transport, generalised and proved.** Block was the first device
  because a root filesystem is what userspace needs; net is the second
  because it is the device that most differs from block — two queues,
  no request/response, a header instead of a status — so building it is
  what turns "a transport that carries block" into "a transport." The
  ring disciplines the block device earned are exactly what a second
  device should inherit for free; this unit is partly a test that they
  were built at the right layer.
- **The mirror of an established invariant.** The host stack claims NIC
  independence and now has two NIC drivers (virtio and the Intel unit) to
  test it; the guest transport should be able to say the same, and cannot
  until a second device rides it.

## Proposed design

### 1. A second virtio-mmio node, and the transport that carries two

The machine layout gains `COSMO_HVM_VIRTIO1_*` — a second transport window
at the next virtio-mmio slot (`0x0A000200`, the bank `virt` uses) and its
own SPI. The device tree writer adds the node (`compatible "virtio,mmio"`,
its reg and interrupt), the way the block node was added. Nothing forces
a guest to use it: with no `--net`, the transport reports `DeviceID` 0 and
the guest's driver skips the node, exactly as the disk-less block node
does, so the boot test is unchanged.

`vmctl`'s transport register file learns `QueueSel` and keeps per-queue
state (descriptor/avail/used addresses, size, ready, `used_idx`), because
a net device has two queues: queue 0 is **receive**, queue 1 is
**transmit** (the virtio-net convention). `DeviceID` is 1 (network), and
config space carries the MAC (six bytes) when `VIRTIO_NET_F_MAC` is
offered.

### 2. The device side of a net queue (`userland/system/vnet.c`)

A net queue is walked the same way a block queue is — the difference is
what a served buffer *means*:

- **Transmit (queue 1):** the guest posts a frame as a device-readable
  chain (the `virtio_net_hdr` then the frame bytes). The device reads the
  chain out of guest memory, strips the header, and hands the frame to
  the wire. The used entry is the descriptor consumed; no data returns.
- **Receive (queue 0):** the guest posts empty device-writable buffers.
  When a frame arrives from the wire, the device writes a `virtio_net_hdr`
  (zeroed — no offloads) and the frame into the next posted buffer and
  completes it, the used length being header + frame.

The walk, the bounds and the atomic publish are the block device's,
factored so both devices share them rather than each carrying its own
copy: the review history on `vblk.c` is precisely the argument for not
writing a second, subtly different walk. What `vnet.c` adds is the
frame-shaped `serve` — read-a-frame-out for transmit, write-a-frame-in
for receive — and the per-frame bound (a frame is at most 1514 bytes plus
the header, so a buffer or chain claiming more is a driver error).

### 3. The wire is the owner, and for this unit it is a loopback

A network device is only as useful as what it is plugged into. The owner
is the wire, and this unit gives it the simplest honest one: a
**loopback** — a frame the guest transmits is delivered back to it on the
receive queue. That is enough to prove both queues, the header, and the
interrupt end to end: a guest brings the interface up, posts a receive
buffer, transmits a frame, and receives its own frame back. It also lets
the guest's own stack talk to itself (its address, its loopback), which
is what a freshly-configured interface does first.

Connecting the guest to something real — bridging its frames to the
host's own network stack so it can reach the host, or out through the
host's NIC — is deliberately the *next* unit (§Deliberately out of scope):
it is a policy on top of the wire, not the device, and it is where the
host's userland network API (raw frames to and from the host stack) has
to be designed. This unit builds the device and proves it against a wire
it fully controls; the block unit shipped the same way, with the real
Linux mount as a documented reproduction rather than a gate.

### 4. The interrupt, and receive-buffer starvation

A completed transmit or a delivered receive raises the device's SPI
through the guest's distributor, and the guest's InterruptACK lowers it,
exactly as block does. One net-specific care: a frame can only be
received if the guest has posted a buffer for it. If the receive queue is
empty when a frame arrives, the frame is dropped (a NIC drops when it has
no buffer; it does not stall the wire) — counted, not queued without
bound, so a guest that never posts receive buffers cannot make the owner
hold frames forever.

### 5. Feature negotiation, kept minimal

The device offers `VIRTIO_F_VERSION_1` and `VIRTIO_NET_F_MAC` (a fixed
MAC in config space) and nothing else — no checksum or TSO/USO offloads
(`GUEST_CSUM`/`HOST_TSO*`), no mergeable receive buffers
(`MRG_RXBUF`), no control queue (`CTRL_VQ`), no multiqueue (`MQ`). A
conforming driver then takes the simple path: one receive queue, one
transmit queue, a fixed-size buffer per frame, a zeroed header. Each
omitted feature is a later unit if a guest ever needs it; none is on the
path to "a guest has an interface that sends and receives."

### 6. The milestone

- **Gated, in the harness:** a guest (`guest_vnet.c`) probes the node,
  negotiates, sets up both queues, posts a receive buffer, transmits a
  known frame, and receives it back through the owner's loopback with the
  bytes intact (`el2-virtq-net`); and the device-side queue walk serves
  transmit and receive over an in-memory wire and refuses hostile rings
  with no out-of-bounds access (`test_vnet_dev`). Neither needs a real
  network.
- **Demonstrated, reproducible:** a stock Linux brings up `eth0` on the
  device and sends to an address on its subnet that is *not* its own, so
  the frame must leave through `eth0` (pinging its own address would take
  the kernel's local route and never touch the device); the loopback wire
  returns the frame, and `eth0`'s transmit and receive counters
  (`ip -s link show eth0`) both advance — an observable that fails if
  transmit, the wire, receive, or the interrupt is broken. Reaching
  anything beyond the guest waits for the host-bridge unit. Same
  `QEMU_MEM=2G` reproduction shape as the block device's Linux
  demonstration, not a CI gate.

### 7. Deliberately out of scope

- **A real network.** Bridging the guest's frames to the host's network
  stack (so it can reach the host, or the outside through the host NIC) is
  the next unit; it needs the host userland's raw-frame API designed, and
  is a policy on the wire, not the device.
- **Offloads and mergeable buffers** (`CSUM`, `GUEST_CSUM`, `HOST_TSO*`,
  `MRG_RXBUF`). The device presents a plain interface: full frames, no
  checksum or segmentation help, one buffer per frame.
- **The control queue and multiqueue** (`CTRL_VQ`, `MQ`, MAC
  programming, RSS). One receive and one transmit queue; the MAC is fixed
  in config space.
- **A PCI transport.** virtio-mmio still; a PCI transport is its own unit
  on either device.

## Affected files

- `kernel/include/uapi/cosmo/hv_machine.h` — `COSMO_HVM_VIRTIO1_BASE`/
  `_SIZE`/`_INTID`.
- `tools/fdt/fdt.c` — the second `virtio_mmio` node; `tests/host/test_fdt.c`
  reads it back.
- `userland/system/vnet.c`/`.h` (new) — the device side of a net queue,
  sharing the ring walk and disciplines with `vblk.c` (the shared walk is
  factored out of `vblk.c` so there is one copy).
- `userland/system/vmctl.c` — the second transport window, `QueueSel` and
  per-queue state, `DeviceID` 1 with a MAC, the loopback wire, `--net loop`.
- `userland/userland.mk` — link `vnet.c` (and the shared walk) into vmctl.
- `tests/host/test_vnet_dev.c` (new), `tests/host/host.mk` — the host
  device-walk test.
- `tests/hv/aarch64/guest_vnet.c` (+ start) and
  `kernel-services/virtualization/hvtest.c` — the guest driver and the
  `el2-virtq-net` end-to-end test with an inline loopback wire.
- `docs/kernel-services/virtualization/design.md`,
  `docs/kernel-services/virtualization/testing.md`, `README.md` — the
  Status entry and test rows.

## New APIs

No new system calls: transmit and receive reach guest memory through the
existing `cosmo_vm_mem_*`, and the interrupt through
`cosmo_vm_raise_spi`/`_lower_spi`. The device-side walk shares the block
device's callback shape; a net `serve` reads or writes a frame rather than
a block.

```c
/* userland/system/vnet.h */
#define VIRTIO_NET_F_MAC     5u    /* config space carries a MAC */
#define VNET_HDR_LEN         12u   /* struct virtio_net_hdr, zeroed (no offloads) */
#define VNET_FRAME_MAX       1514u /* a full Ethernet frame, no jumbo */

struct vnet_io {
    int (*read_guest)(void *ctx, uint64_t gpa, void *buf, uint32_t len);
    int (*write_guest)(void *ctx, uint64_t gpa, const void *buf, uint32_t len);
    /* The wire: take a transmitted frame, and try to fill a receive
     * buffer from the wire. Both return bytes, or <0 / 0 for none. */
    int (*wire_tx)(void *ctx, const void *frame, uint32_t len);
    int (*wire_rx)(void *ctx, void *frame, uint32_t max);
    void *ctx;
    uint64_t max_bytes_per_call;   /* the per-notification work ceiling, as block has */
};
```

`vmctl` gains `--net loop` (the loopback wire). The transmit and receive
queues are served by the shared ring walk with net's per-frame `serve`.

## Migration plan

1. **Factor the ring walk out of `vblk.c`** into a shared device-side
   virtqueue module (the avail/used walk, the bounds, the atomic publish,
   the work ceiling), leaving `vblk.c` as the block `serve` over it. The
   block tests are the check on the refactor — all must stay green with no
   behaviour change before a line of net is written.
2. **The net device side, on the host.** `vnet.c`'s transmit and receive
   `serve` over the shared walk; prove it with `test_vnet_dev` — a frame
   transmitted is handed to the wire, a frame on the wire fills a posted
   receive buffer, an empty receive queue drops rather than stalls, and
   hostile rings (a frame past `VNET_FRAME_MAX`, a receive buffer outside
   guest RAM, the backlog/chain/index cases) are refused — each proved by
   reintroducing the bug.
3. **The transport in `vmctl`.** The second window, `QueueSel` and
   per-queue state, `DeviceID` 1 and the MAC, `--net loop`, and the SPI on
   completion. The block transport and a disk-less boot are unchanged.
4. **The guest test.** `guest_vnet.c` brings the interface up, posts a
   receive buffer, transmits a frame, receives it back; `el2-virtq-net`
   with an inline loopback wire requires the received bytes to match.
5. **The Linux demonstration**, documented and reproducible under
   `QEMU_MEM=2G`: `eth0` up, `ping` of its own address.
6. **Docs and the Status entry**, and the full verification chain.

## Tests

- `test_vnet_dev` (host): a transmit hands the frame to the wire intact; a
  receive fills a posted buffer with a wire frame and a zeroed header, used
  length header+frame; an empty receive queue drops a wire frame (counted,
  no stall); hostile rings — a frame longer than `VNET_FRAME_MAX`, a
  receive buffer outside guest RAM, an index/backlog/looping chain — are
  refused with no out-of-bounds access; and the work ceiling bounds a
  notification as it does for block.
- `el2-virtq-net` (kernel): a real guest driver negotiates the device,
  sets up both queues, transmits a known frame and receives it back
  through the inline loopback wire; the test requires the received frame
  to equal the transmitted one, so a device that dropped or corrupted it
  would not pass.
- The block tests and a disk-less/net-less boot stay green; the shared
  ring walk is exercised by both devices' tests.

## Benchmarks

Not a performance unit, but a loopback round-trip gives a cheap number:
frames per second through transmit → wire → receive, recorded so a later
regression (or the host-bridge unit) has a baseline. No absolute target.

## Risks

- **Reimplementing the ring walk instead of sharing it.** The block
  device's walk cost seven review rounds to harden; a net device that
  copies it will drift and reintroduce the same bugs. The plan factors the
  walk out first, with the block tests as the check, so net inherits the
  disciplines rather than re-earning them.
- **A frame with no receive buffer.** A wire frame arriving at an empty
  receive queue must be dropped, not held — a guest that never posts
  buffers must not make the owner accumulate frames without bound. Counted
  and dropped, like real hardware.
- **The two-queue transport.** Adding `QueueSel` and per-queue state to a
  transport that had one queue can leak block behaviour into net or
  vice-versa if the state is not cleanly per-queue. The block device stays
  a one-queue user of the same code, so its tests catch a regression.
- **A loopback NIC's limited reach, oversold.** This unit's device is real
  but its wire is a loopback; it must be described as the mechanism, with
  external connectivity named as the next unit, exactly as the block unit
  described the Linux mount as a reproduction. The honest framing is the
  deliverable, not just the code.

## Alternatives considered

- **Bridge to the host network now, instead of loopback.** More useful
  immediately (the guest could reach the host and beyond), but it folds
  two designs into one — the device *and* the host userland's raw-frame
  API — and the second is the larger, less settled half. Splitting them
  keeps this unit a tight, fully-testable device and lets the bridge get
  its own design. Rejected for scope, deferred as the named next unit.
- **A different device next (virtio-console dataplane, an RNG, a PCI
  transport).** Each is a real unit, but net is the device that most
  exercises the transport (two queues, frames not requests), so it is the
  one that best earns "the transport carries more than block." The others
  follow.
- **An in-kernel virtio-net.** Rejected for the same reason as in-kernel
  block: the wire is the owner's, and the kernel has no business holding a
  guest's network. The console and distributor are in the kernel because
  their state is kernel state; a guest's packets are not.
- **Interactive console (forwarding the owner's stdin to the guest's
  UART) as the next unit instead.** Genuinely useful — it makes the
  userspace a guest already reaches drivable by a human — and named
  not-done since the machine unit. But it is largely owner-side plumbing
  over a receive path the console already has, where net is a new device
  that tests the transport; net is the more substantial subsystem and the
  better use of a report. The interactive console remains a good small
  unit to pick up separately.
