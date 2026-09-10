# NEXT SUBSYSTEM — a host bridge: a tap interface and a frame channel, so the guest reaches the host's network

Constitution §68: after the audit, name the next subsystem in this shape
and wait for the instruction to build it. This is that report, and
nothing in it is implemented.

**Subsystem: a tap interface in the host network stack and a userland
frame channel the owner pumps it through, so a guest's virtio-net frames
reach the host's own stack and back — the guest can ARP, ping and open a
TCP connection to the host, over a real interface rather than a
loopback.**

## Problem

The guest has a network interface, but it is plugged into a loopback: a
frame it transmits comes back to it and reaches nothing. The virtio-net
unit built the device and proved it against a wire it fully controls, and
named the wire's other end as the next unit — "bridging the guest's
frames to the host's stack ... where the host userland's raw-frame API
has to be designed." This is that unit.

The gap is specific. The host stack (`kernel-services/network/`) has
Ethernet, ARP, IPv4/IPv6, TCP and UDP, and interfaces (`netif`,
`loopback`), but the only way userland reaches the network is a
`SOCK_STREAM` or `SOCK_DGRAM` socket — a *connection*, not a *frame*.
`vmctl`, the owner, holds the guest's frames (through the virtio-net
device's `wire_tx`/`wire_rx` callbacks) and has nowhere to put them: no
raw socket, no packet socket, no tap. A guest that has done everything
right — negotiated the device, brought `eth0` up, ARPed for its gateway —
is talking into a wire whose far end is a mirror.

## Current implementation

**The wire is a loopback.** `vmctl`'s virtio-net device
(`next-subsystem-vnet.md`) has a `wire_tx` that enqueues a transmitted
frame into a small FIFO and a `wire_rx` that dequeues it back onto the
receive queue. It proves the device; it connects the guest to nothing.

**The host stack has interfaces, but userland cannot be one.** `netif.c`
registers interfaces (`netif_register`), each with `ops->transmit` (the
stack sends a frame *out* the interface) and `netif_rx` (a frame arrives
*in* from it); `loopback.c` is the minimal example — its `transmit` calls
`netif_rx` straight back. A driver is kernel code. There is no way for a
userland process to *be* the far end of an interface: to receive the
frames the stack transmits and to inject the frames it should receive.

**Userland networking is connections, not frames.** `socket.c` offers
`AF_INET` `SOCK_STREAM`/`SOCK_DGRAM`. There is no `SOCK_RAW`, no
`AF_PACKET`, no tap device — nothing that moves an Ethernet frame between
userland and the stack. That absence is exactly what stands between the
guest and the host.

## Why it matters

- **A guest that reaches a real stack.** Even without the internet, a
  guest that can reach the *host* is a real machine on a real network of
  two: it can `ping` the host, resolve nothing but connect to a service
  the host runs, be connected to from the host. It is the difference
  between a NIC that loops back and a NIC plugged into something.
- **The foundation the external network needs.** Routing the guest's
  traffic out to the world (NAT, DHCP, DNS) is a policy on top of a
  working guest-to-host link; it cannot be built until the link exists.
  This unit builds the link and stops there, so the NAT unit has
  something to route.
- **A raw-frame API the host has lacked.** A tap interface is useful
  beyond VMs — it is how any userland program participates in the stack
  at layer 2 (a bridge, a packet capture, a userspace protocol). Building
  it for the guest builds it for the host.

## Proposed design

### 1. A tap interface in the host stack (`tap.c`)

A `tap` is a `netif` whose far end is a userland process rather than a
wire. Its `ops->transmit` — the stack sending a frame out the interface —
does not put the frame on a wire; it enqueues it on a per-tap ring for the
owner to read. A frame the owner injects becomes a `netif_rx` into the
stack, exactly as a real driver's completion would. So the stack treats
the tap as an ordinary interface: it ARPs on it, routes to its subnet,
answers pings addressed to its IP, accepts connections that arrive on it —
all the existing code, over a new interface whose driver is in userland.

The tap is a point-to-point link between the host and one guest, brought
up with a fixed configuration for this unit: the host end at a chosen
private address (say `10.0.2.1/24`), so the guest can be given
`10.0.2.15` and reach `10.0.2.1`. (Making the address configurable, and
serving it to the guest by DHCP, is the NAT unit's, not this one's.)

### 2. The frame channel: a handle, read and write frames

The owner reaches the tap through a **handle**, opened the way `/dev/vmm`
is — a character device (`/dev/net/tap`, or a `SYS_tap_open` returning a
handle; the device-node form reuses `open`/`read`/`write`/`close` and
needs no new call). The contract is one frame per read and per write:

- `read` returns the next frame the stack transmitted out the tap (a
  frame bound for the guest), or blocks / returns `EAGAIN` when none
  waits.
- `write` injects one frame from the guest into the stack (`netif_rx`).

It is a `KOBJECT_TYPE_IO` object, so it works with the readiness and
non-blocking machinery the network unit already built (`ioready`,
`setnonblock`, `io_poll`) — the owner polls it beside the vCPU exits
rather than blocking. Opening the handle registers the tap and brings it
up; closing it unregisters the tap and drops its queued frames, so a
crashed owner leaves no interface behind. The queues are bounded and drop
when full — a NIC drops when it has nowhere to put a frame.

### 3. The owner bridges the guest to the channel (`vmctl --net tap`)

`--net tap` replaces the loopback wire with the frame channel. The
device's `wire_tx` (a frame the guest transmitted) becomes a `write` to
the channel; the run loop, alongside draining the virtio queues, `read`s
frames from the channel and hands them to `wire_rx` for the guest's
receive queue. The virtio-net device is unchanged — it already reaches
its wire through callbacks; only the callbacks' far end moves from a FIFO
to the host stack. `--net loop` stays, so the device's own tests keep
their self-contained wire.

### 4. What crosses, and what does not

Layer 2 crosses whole: the owner moves Ethernet frames, and the host
stack does ARP, IP and the transport. The owner does **not** parse or
translate the guest's traffic — it is a wire, not a proxy; the difference
from a userspace NAT (which terminates the guest's IP and reopens it on
host sockets) is deliberate, and it is why this unit needs the tap rather
than the existing sockets. The guest and host must share a subnet (the
tap's), which this unit fixes; giving the guest an address automatically
and reaching beyond the host are the next unit's.

### 5. The milestone

- **Gated, in the harness:** a host self-test brings up a tap, and the
  stack and the tap's userland end exchange frames — a frame the stack
  transmits is read back, a frame injected is received and answered (an
  ARP request for the host's tap IP is injected and the stack's ARP reply
  is read; an ICMP echo is injected and the echo reply is read). And an
  end-to-end kernel test (`el2-tap-host`): a guest brings `eth0` up,
  ARPs for `10.0.2.1`, sends an ICMP echo, and receives the host stack's
  reply — the owner's role played by the test, bridging the guest's
  virtio-net to a real tap. Neither needs an external network.
- **Demonstrated, reproducible:** a stock Linux brings `eth0` up on
  `10.0.2.15`, `ping 10.0.2.1` answers from the host, and a `curl` of a
  host-side listener connects — the `QEMU_MEM=2G` reproduction, not a CI
  gate.

### 6. Deliberately out of scope

- **Reaching beyond the host — NAT, routing, DHCP, DNS.** Routing the
  tap's subnet out through the host's own interface (so the guest reaches
  the world), handing the guest its address, and answering its name
  lookups are the next unit; each is a policy on top of a working
  guest-to-host link.
- **Bridging onto the host's physical LAN** (the guest as a peer on the
  host's real network, its own DHCP lease from a real server). That is L2
  bridging, a different and larger interface-plumbing unit.
- **Raw and packet sockets for general userland.** The tap is a device,
  not a new socket family; `SOCK_RAW`/`AF_PACKET` for arbitrary programs
  is its own unit if a program ever needs it.
- **Offloads across the tap.** Frames cross whole and checksummed; the
  tap does not negotiate `TXCSUM`/`RXCSUM` shortcuts with the stack.
- **More than one guest on one tap, or many taps bridged together.** One
  tap is one point-to-point link to one owner.

## Affected files

- `kernel-services/network/tap.c` (new), `kernel-services/network/netif.h`
  or `kernel/include/kernel/net/tap.h` — the tap netif and its queues.
- The frame channel: a character device under `kernel-services/vfs/ramfs`'s
  chrdev mechanism (as `/dev/vmm` is), or `SYS_tap_open`; a
  `KOBJECT_TYPE_IO` object with `read`/`write`/`ready`/`set_nonblock`.
- `userland/system/vmctl.c` — `--net tap`: open the channel, `wire_tx`
  writes it, the run loop reads it into `wire_rx`.
- `tests/host/test_tap.c` (new), `tests/host/host.mk` — the tap netif and
  channel with no guest.
- `tests/hv/aarch64/guest_vnet.c` (extended) or a new guest, and
  `kernel-services/virtualization/hvtest.c` — `el2-tap-host`, a guest
  reaching a real host tap.
- `docs/kernel-services/network/`, `docs/kernel-services/virtualization/`,
  `README.md` — the design and the Status entry.

## New APIs

The frame channel, either as a device node (no new system call) or:

```c
#define SYS_tap_open  <n>   /* (const char *name) -> handle: a layer-2 endpoint on the host stack */
```

A `KOBJECT_TYPE_IO` handle: `read` one frame the stack transmitted,
`write` one frame into the stack, `ready`/`set_nonblock` as the network
unit defined them. No change to the virtio-net device or `vq.c`.

## Migration plan

1. **The tap netif, on the host.** `tap.c`: a `netif` whose `transmit`
   enqueues to a ring and whose injection is `netif_rx`; register/bring up
   with the point-to-point config; a bounded, dropping queue. Prove it
   with `test_tap` — the stack transmits a frame and it is read back, a
   frame injected is received, an ARP for the tap's IP is answered, queues
   drop when full — with no guest.
2. **The frame channel.** The handle (device node or `SYS_tap_open`)
   over the tap: `read`/`write` move frames, `ready`/`set_nonblock` work,
   close unregisters. Prove it drives the tap from a userland-shaped
   caller in the host test.
3. **vmctl `--net tap`.** Open the channel, bridge `wire_tx`/`wire_rx`,
   read the channel in the run loop. `--net loop` unchanged.
4. **The end-to-end guest test.** `el2-tap-host`: a guest ARPs and pings
   the host's tap address over a real tap, and the host stack replies.
5. **The Linux demonstration**, documented and reproducible under
   `QEMU_MEM=2G`: `eth0` up, `ping 10.0.2.1`, a TCP connection to a host
   listener.
6. **Docs and the Status entry**, and the full verification chain.

## Tests

- `test_tap` (host): the stack transmits out the tap and the frame is read
  from the channel; a frame written to the channel is received by the
  stack; an injected ARP request for the tap's IP produces an ARP reply on
  the channel; an injected ICMP echo produces an echo reply; the queues
  drop when full; a closed channel unregisters the tap. No guest.
- `el2-tap-host` (kernel): a guest driver brings `eth0` up, ARPs for the
  host's tap IP, sends an ICMP echo, and receives the reply — the frames
  crossing a real tap, the host stack answering. So a tap that dropped or
  misrouted a frame would fail.
- The virtio-net tests and a `--net loop`/net-less boot stay green; the
  device is unchanged.

## Benchmarks

A round-trip time and a bulk transfer between guest and host over the tap,
recorded so the NAT unit and later regressions have a baseline. No
absolute target.

## Risks

- **A frame channel that blocks the owner.** If the owner blocks reading
  the channel, the guest's vCPUs stop. It must be non-blocking and polled
  beside the vCPU exits, as the console already is; the readiness
  machinery exists for exactly this.
- **A crashed owner leaving an interface.** A tap registered on open must
  be unregistered on close, and its queued frames dropped, or a dead VM
  leaves a live interface and leaks frames. Close is the contract.
- **Unbounded queues.** The stack can transmit faster than the owner
  reads, and the guest faster than the stack accepts; both queues bound
  and drop, like a NIC, so neither side makes the other grow without
  bound.
- **The stack meeting an untrusted frame.** A frame injected from the
  guest is untrusted, and it now reaches the host's real ARP/IP/TCP code,
  not a loopback. That code already hardened against hostile input over
  the network hardening unit; this unit's care is to add no path around
  those checks — an injected frame takes the same `netif_rx` a driver's
  does, with no shortcut.
- **Overselling a host-only link.** The guest reaches the host, not the
  world; the unit must say so, with NAT named as the next unit, exactly
  as the virtio-net unit called its wire a loopback.

## Alternatives considered

- **A userspace NAT in the owner (QEMU's "user"/SLIRP model).** The owner
  terminates the guest's L2 and IP and reopens its flows on host
  `SOCK_STREAM`/`SOCK_DGRAM` sockets — no new host API, and it reaches the
  world directly. But it is a TCP/IP/ARP/DHCP/DNS implementation *in
  vmctl*, far larger than a tap, and it duplicates the stack the host
  already has. The tap reuses the real stack and is smaller; NAT to the
  world becomes a routing policy on the tap's subnet, a later unit.
  Rejected for size and duplication.
- **Raw/packet sockets instead of a tap device.** A `SOCK_RAW` /
  `AF_PACKET` socket would also move frames, but it is a socket-family
  addition threaded through `socket.c`, and it is the wrong shape for
  *being an interface* (it observes or injects on an existing one). The
  tap is an interface whose driver is userland — the right primitive for
  a VM's NIC, and reusable. A raw socket is deferred to if a program needs
  to sniff or inject on a real interface.
- **An in-kernel bridge from the guest's virtio-net straight to a host
  netif.** It would skip the owner, but it puts a guest's packets in the
  kernel and couples the hypervisor to the network stack — the same
  objection as an in-kernel virtio device. The wire is the owner's; the
  tap is how the owner reaches the stack.
- **Interactive console (owner stdin to the guest UART) as the next unit
  instead.** Still a good small unit and still not done, but the network
  arc has momentum and a named next step; the host bridge is the larger,
  more consequential subsystem and the one the virtio-net unit pointed at.
