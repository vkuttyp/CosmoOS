# kernel-services/network

The network stack (Phase 8): a layered design over an mbuf abstraction
(constitution sections 33 to 36). No file here names a NIC driver
(invariant 5); drivers register a `struct netif` and call `netif_rx`.

| File | Content |
|---|---|
| `mbuf.c` | Packet buffers: reference-counted, chained, 2 KiB clusters, queues (`kernel/mbuf.h`) |
| `inet.c` | `struct netaddr` helpers and the UAPI `cosmo_sockaddr` conversion (`kernel/net/inet.h`) |
| `cksum.c` | The Internet checksum over buffers and chains, pseudo headers (`kernel/net/cksum.h`) |
| `netif.c` | Interface registry, the 512-packet receive queue, deferred work, the `netrx` worker thread, fw_cfg address configuration (`kernel/netif.h`) |
| `loopback.c` | The `lo` interface and the test filter hook |
| `ether.c` | Ethernet framing, dispatch by EtherType, runt padding |
| `arp.c` | ARP table (64 entries), resolution with one pending packet, retries and ageing |
| `ipv4.c` | IPv4 input/output, route and source selection, ICMP echo and unreachable |
| `ipv6.c` | IPv6 input/output, link-local addresses, ICMPv6, neighbour discovery (32 entries) |
| `udp.c` | UDP pcbs, binding, checksums, per-socket receive queues (`kernel/net/udp.h`) |
| `tcp.c` | TCP: state machine, send/receive rings, RTO, fast retransmit, congestion control, TIME_WAIT, accept queues (`kernel/net/tcp.h`) |
| `socket.c` | `struct socket` kobjects and the blocking `ksock_*` API used by system calls 23–31 (`kernel/socket.h`) |
| `nettest.c` | Self-tests `net-mbuf` … `net-harness` and the harness echo services |

Driver: `drivers/virtio/virtio_net.c` (`eth0`). System calls:
`kernel/syscall/native.c`. Host harness: `tests/boot/nettest.py`.
Documentation: `docs/kernel-services/network/`.
