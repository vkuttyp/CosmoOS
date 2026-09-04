# Networking: architecture

Constitution sections 33 (the layered stack over an mbuf abstraction),
34 (mbuf design), 35 (build the ownership and memory abstractions first,
promise no zero copy yet), 36 (avoid global locks in packet processing),
60 (networking testing), invariant 5 (the stack must not depend on a
specific NIC driver), and the Phase 8 roadmap entry: mbuf, Ethernet,
IPv4, IPv6, UDP, TCP, sockets.

## Where it sits

```text
   user process       socket/bind/listen/accept/connect/sendto/recvfrom/shutdown (+ read/write/close)
        │             kernel/syscall/native.c → handles → struct socket (a kobject_io_type)
        ▼
   kernel-services/network/
        socket.c      the socket layer: kernel API (ksock_*) shared by system calls and tests
        tcp.c         state machine, send/receive buffers, retransmission, congestion control
        udp.c         datagram sockets and demultiplexing
        ipv4.c        IPv4, ICMP (echo, unreachable), route and source selection
        ipv6.c        IPv6, ICMPv6, neighbour discovery (ND table)
        arp.c         ARP table and resolution
        ether.c       frame parsing and output with address resolution
        netif.c       interfaces, the receive queue, deferred work and the network worker thread
        mbuf.c        packet buffers: reference counted, chained, external clusters
        cksum.c       the Internet checksum
        inet.c        address helpers and the UAPI address conversion
        loopback.c    the `lo` interface (127.0.0.1, ::1)
        nettest.c     self-tests and the harness-driven echo services
        │
        ▼
   drivers/virtio/virtio_net.c   virtio-net as a boot module: `eth0`
   kernel/core/fwcfg.c + kernel/arch/x86_64/fwcfg.c
                                 QEMU fw_cfg: test parameters and static addresses at boot
```

Everything above the driver is generic; a NIC driver fills in a
`struct netif` (name, MAC, MTU, `struct netif_ops` with `transmit`),
registers it, and calls `netif_rx()` from its completion path. The stack never names a driver
(invariant 5); the loopback interface is a `netif` like any other, which
is what makes the protocol tests deterministic.

## Purpose

Give processes Unix sockets over real IPv4 and IPv6 networks: a packet
buffer model whose ownership is explicit, a layered stack that a NIC
driver plugs into, TCP with retransmission and flow control good enough
to carry data reliably over a lossy path, UDP, and a socket API exposed
through handles.

## Responsibilities

- **mbuf** (`kernel/mbuf.h`): reference-counted buffers with headroom
  and tailroom, chained (`next`) into a packet, packets chained
  (`nextpkt`) into queues, external 2 KiB clusters from a slab that are
  DMA-able, per-packet metadata (interface, protocol, checksum state,
  receive time). Ownership rule: whoever holds the pointer owns one
  reference; passing an mbuf down `netif_transmit` or up `netif_rx`
  transfers it.
- **Interfaces** (`kernel/netif.h`): `struct netif` with name, MAC, MTU,
  IPv4 address/mask/gateway, IPv6 link-local address, operations,
  statistics; registration; the receive queue drained by one network
  worker thread (`netrx`) so protocol code runs in thread context with a
  known locking model; the transmit path is called in the sender's
  context.
- **Ethernet and ARP**: frame validation, dispatch by EtherType, output
  with ARP resolution, an ARP table with pending-packet queues,
  request retransmission and entry ageing.
- **IPv4 and ICMP**: header validation and checksum, local delivery,
  broadcast, a default route, ICMP echo request/reply, port
  unreachable. Fragmentation is not implemented (fragments are counted
  and dropped; sends larger than the MTU fail with `-EMSGSIZE`).
- **IPv6, ICMPv6, ND**: link-local address from the MAC, neighbour
  solicitation/advertisement, ICMPv6 echo, hop-limit handling, `::1`.
- **UDP**: bound ports, per-socket receive queues, `sendto`/`recvfrom`,
  checksum generation and verification (v4 and v6).
- **TCP**: RFC 793 states, three-way handshake, sequence and
  acknowledgement handling, receive window and flow control, a byte
  send buffer with retransmission on an RTO timer (RFC 6298 estimate),
  fast retransmit on three duplicate ACKs, slow start and congestion
  avoidance, delayed ACK, TIME_WAIT, RST handling, listen backlog. No
  SACK, window scaling, timestamps, or urgent data.
- **Sockets** (`kernel/socket.h`): `struct socket` as a kobject with the
  io type (read/write on connected sockets), the kernel API `ksock_*`
  used by both the system calls and the tests, blocking semantics
  through wait queues, `shutdown`. System calls 23 to 31 with
  `struct cosmo_sockaddr` in the UAPI.
- **Drivers**: `virtio_net.ko` (feature negotiation, 32 receive
  clusters posted to the device, transmit completions, MAC from the
  device configuration, interrupt-driven queues through the `virtio`
  module) and `lo`.
- **Boot parameters**: a fw_cfg reader gives the kernel `opt/cosmo/*`
  strings QEMU is started with: static IPv4 configuration and the test
  harness's ports. Without them, `eth0` takes QEMU's default user-mode
  address and the harness-driven tests skip.

## Non-responsibilities

- Zero-copy paths and user mappings of packet buffers (section 35: the
  ownership model comes first; copies happen at the socket boundary).
- DHCP, DNS, routing beyond one default route, multicast, IGMP/MLD,
  IPv4 fragmentation and reassembly, path MTU discovery, TCP options
  beyond MSS, SACK, ECN, keepalive, `SO_*` socket options beyond what
  the tests need, Unix domain sockets, raw sockets, netfilter-style
  hooks, per-CPU queues and RCU routing (section 36: one worker thread
  and fine-grained locks for now; the queue and table shapes admit the
  upgrade).
- Any other NIC. Section 60's fuzzing of parsers is started with host
  tests over the pure parsers and is a standing item, not finished here.

## Interfaces at a glance

| Interface | Header | Used by |
|---|---|---|
| `m_get`, `m_getcl`, `m_free`, `m_freem`, `m_prepend`, `m_pullup`, `m_adj`, `m_copydata`, `m_append`, `m_length`, `m_copypacket`, `m_ref`, `mbufq_*` | `kernel/mbuf.h` | every layer, drivers |
| `netif_register/unregister`, `netif_rx`, `netif_transmit`, `netif_find/default/loopback`, `netif_set_ipv4`, `netif_set_up`, `net_work_init/queue`, `loopback_set_filter` | `kernel/netif.h` | drivers, protocols, timers, tests |
| `struct netaddr`, `htons` .., `netaddr_*` | `kernel/net/inet.h` | every layer, system calls |
| `in_cksum`, `cksum_partial/fold`, `m_cksum_partial`, `cksum_pseudo4/6` | `kernel/net/cksum.h` | IP, ICMP, UDP, TCP |
| `ether_input/output`, `arp_resolve`, `arp_input`, `arp_lookup`, `arp_age` | `kernel/net/ether.h` | netif worker, IP output |
| `ipv4_input/output`, `ipv6_input/output`, `icmp_*`, `icmpv6_input`, `nd_*` | `kernel/net/ip.h` | Ethernet dispatch, UDP, TCP |
| `udp_*`, `tcp_*` | `kernel/net/udp.h`, `tcp.h` | socket layer |
| `ksock_create/bind/listen/accept/connect/sendto/recvfrom/shutdown/getsockname/getpeername/get/put` | `kernel/socket.h` | system calls, tests |
| `SYS_socket` … `SYS_getsockname` (23–31), `struct cosmo_sockaddr`, `COSMO_AF_*`, `COSMO_SOCK_*`, `COSMO_SHUT_*` | `uapi/cosmo/syscall.h` | user programs |
| `fwcfg_get_string` | `kernel/fwcfg.h` | netif configuration, tests |

Tests (`testing.md`): self-tests over `lo` for mbufs, checksums, ARP
table logic, UDP v4 and v6, TCP connect/transfer/close, TCP under
injected loss (a loopback test hook drops every seventh data segment),
listen backlog and RST; a harness-driven test where the QEMU host
connects to the guest's TCP and UDP echo services over port forwarding
and the guest connects back to a host service; and init's user-mode
socket test over loopback. Host tests with a bit-flipping fuzz loop
over the parsers are a recorded gap, not yet written.
