# Networking: API

Every entry follows constitution section 52: purpose, inputs, outputs,
ownership, lifetime, concurrency, blocking, interrupt-context rules,
failure modes, ABI stability. Kernel-side interfaces are **internal**
(they may change with the code); the system-call numbers, constants and
`struct cosmo_sockaddr` in `uapi/cosmo/syscall.h` are **stable**. The
symbols listed under "Module exports" are part of module ABI v1 and
change only with its version.

Common rules unless stated otherwise: an `struct mbuf *` argument is
**taken** (the callee owns it afterwards, including on failure); a
function that returns an `struct mbuf *` may return a different pointer
than it was given, so callers write `m = m_prepend(m, n)`; functions
marked *any context* take only IRQ-safe spinlocks and never sleep;
functions marked *thread* may sleep and must not be called with a
spinlock held or from an interrupt handler.

## Byte order and addresses (`kernel/include/kernel/net/inet.h`)

`htons`, `ntohs`, `htonl`, `ntohl`: inline byte swaps (the kernel is
little-endian only). `struct in6_addr { uint8_t s6_addr[16]; }`.

**`struct netaddr { uint16_t family; uint16_t port; union { uint32_t v4; struct in6_addr v6; }; }`**
The stack's internal address: `family` is `COSMO_AF_INET`,
`COSMO_AF_INET6` or 0 (unspecified), `port` is in **host** order, the
address in **network** order. 20 bytes, plain data, no ownership.

Helpers, all pure and any context: `IPV4_ADDR(a,b,c,d)` (network-order
constant), `INADDR_ANY_N`, `INADDR_BROADCAST_N`, `INADDR_LOOPBACK_N`;
`in6_is_unspecified`, `in6_is_loopback`, `in6_is_linklocal`,
`in6_is_multicast`, `in6_equal`; `netaddr_equal` (family, port and
address), `netaddr_addr_equal` (family and address),
`netaddr_is_unspecified`.

**`int netaddr_from_user_shape(struct netaddr *out, const struct cosmo_sockaddr *in)`**
Converts the UAPI shape: `-EAFNOSUPPORT` (or `-EINVAL`) for a family
other than the two known ones. `flowinfo` and `scope` are ignored in this phase.

**`void netaddr_to_user_shape(struct cosmo_sockaddr *out, const struct netaddr *in)`**
Fills every field (`flowinfo` and `scope` 0, unused address bytes 0).

**`const char *netaddr_str(const struct netaddr *a, char *buf, size_t len)`**
`"a.b.c.d:port"` or `"[hex groups]:port"` into `buf` (64 bytes is
enough); returns `buf`. For logs.

## Packet buffers (`kernel/include/kernel/mbuf.h`, `kernel-services/network/mbuf.c`)

Constants: `MCLBYTES` 2048 (a cluster), `MHLEN` 128 (inline storage),
`NET_HEADROOM` 64 (where `m_getcl` places `data`). Flags: `M_PKTHDR`
(first buffer of a packet, `pkt` valid), `M_EXT` (cluster storage),
`M_BCAST`, `M_MCAST` (link-layer destination class of a received
frame), `M_CSUM_OK` (reserved for drivers that verify transport
checksums; nothing sets it yet).

**`struct mbuf`**: `next` (same packet), `nextpkt` (queue), `data`,
`len`, `flags`, `refcount` (atomic), `buf`/`size` (storage), `pkt`
(`len` of the whole chain, `rcvif`, `rx_ns`, `proto`, `csum_flags`,
`src`: the sender's `netaddr`, set by `udp_input`), `cl` (the cluster
when `M_EXT`), `inl[MHLEN]`. 240 bytes; a `struct mbuf_cluster` is a
4-byte refcount plus 2048 data bytes. Both come from slab caches
(`mbuf`, `mcluster`) whose memory is direct-mapped, so `dma_map`
succeeds on `m->data`.

**`void mbuf_init(void)`** Creates the caches; called once from
`net_init`. Panics without memory.

**`struct mbuf *m_get(void)`** A zeroed mbuf with inline storage,
`data == buf`, `len` 0, no `M_PKTHDR`. NULL when out of memory (counted
in `alloc_failures`). Any context (slab with `0` flags: verify the slab
rules in `docs/kernel/memory/api.md` before calling from an interrupt
handler; the stack calls it from thread context and from virtio
completion callbacks).

**`struct mbuf *m_getcl(void)`** A packet-header mbuf on a fresh cluster:
`M_PKTHDR | M_EXT`, `data = buf + NET_HEADROOM`, `len` 0. NULL when
out of memory. Same context rules.

**`struct mbuf *m_free(struct mbuf *m)`** Drops one reference on one
buffer, freeing it (and its cluster when the cluster's count reaches
zero) and returns `m->next`. NULL in, NULL out. Any context.

**`void m_freem(struct mbuf *m)`** Frees a chain. NULL is fine.

**`struct mbuf *m_ref(struct mbuf *m)`** A new mbuf sharing `m`'s
cluster and describing the same `[data, data+len)`; the cluster count
goes up. NULL for an inline mbuf or without memory. The copy has no
`M_PKTHDR`. Writers must not modify shared bytes.

**`m_leadingspace(m)`, `m_trailingspace(m)`** Inline: bytes before
`data`, bytes after `data + len`, in this buffer only.

**`struct mbuf *m_prepend(struct mbuf *m, uint32_t n)`** Makes `n`
bytes available in front of `data`: in place when headroom allows, else
a new leading buffer (inline for `n <= MHLEN`, a cluster otherwise)
that inherits `M_PKTHDR` and `pkt`. Adjusts `pkt.len`. On failure frees
`m` and returns NULL.

**`struct mbuf *m_pullup(struct mbuf *m, uint32_t n)`** Guarantees the
first `n` bytes of the chain are contiguous in the first buffer, copying
from following buffers into a fresh cluster when necessary (the whole
cluster is usable, so `n <= MCLBYTES`). Returns NULL, having freed the
chain, when `n > MCLBYTES`, when the chain is shorter than `n`, or
without memory. Every header cast in the stack is preceded by it.

**`void m_adj(struct mbuf *m, int n)`** Trims `n > 0` bytes from the
front or `-n` bytes from the back of the chain, clamped to its length;
adjusts `pkt.len`. Buffers are never freed, only emptied.

**`bool m_copydata(const struct mbuf *m, uint32_t off, uint32_t len, void *dst)`**
Copies out of the chain; `false` (partial copy possible) when the chain
is shorter than `off + len`.

**`int m_append(struct mbuf *m, const void *src, uint32_t len)`**
Appends to the last buffer, adding clusters (with `data = buf`) as
needed; adjusts `pkt.len`. `0` or `-ENOMEM` (bytes already appended
stay).

**`uint32_t m_length(const struct mbuf *m)`** Sum of `len` over the
chain (not `pkt.len`).

**`struct mbuf *m_copypacket(const struct mbuf *m)`** Linearises a
chain into one fresh cluster-backed packet (copying `pkt` and the
`M_BCAST/M_MCAST/M_CSUM_OK` flags). NULL when longer than
`MCLBYTES - NET_HEADROOM` (1984 bytes) or without memory. The source
is **borrowed**.

**Queues**: `struct mbufq { head, tail, len, maxlen, lock }`.
`mbufq_init(q, maxlen, name)`; `bool mbufq_enqueue(q, m)` takes the
packet and returns `false` **after freeing it** when the queue is full;
`mbufq_dequeue` returns NULL when empty; `mbufq_drain` frees everything;
`mbufq_len`. All any context (IRQ-safe spinlock).

**`void mbuf_get_stats(struct mbuf_stats *out)`** `mbufs_alive`,
`clusters_alive`, `allocs`, `frees`, `alloc_failures`. Any context.

## Checksums (`kernel/include/kernel/net/cksum.h`, `cksum.c`)

Pure functions, any context. `uint32_t cksum_partial(data, len, sum)`
accumulates 16-bit words (an odd trailing byte is taken as the high
byte of a word, RFC 1071); `uint16_t cksum_fold(sum)` folds and
complements, returning the value to store in a header in network order;
`uint16_t in_cksum(data, len)` is both; `uint32_t m_cksum_partial(m, off,
len, sum)` accumulates over a chain range, handling odd-length buffer
boundaries; `cksum_pseudo4(src, dst, proto, len)` and
`cksum_pseudo6(src, dst, proto, len)` return the pseudo-header sum to
seed a transport checksum with. Verification is
`cksum_fold(m_cksum_partial(m, 0, len, pseudo)) == 0`.

## Interfaces and the worker (`kernel/include/kernel/netif.h`, `netif.c`, `loopback.c`)

**`struct netif_ops { int (*transmit)(struct netif *nif, struct mbuf *m); }`**
Called in thread context with no stack lock held; takes the packet
(frees it on error too) and returns 0 or `-errno`. `m->data` is at the
Ethernet header; the chain may have several buffers.

**`struct netif`**: `name[8]`, `index` (assigned at registration),
`mac[6]`, `mtu`, `flags` (`NETIF_UP`, `NETIF_LOOPBACK`, `NETIF_NOARP`),
`ip4 { addr, mask, gateway }` (network order, 0 = unset), `ip6_ll`
(derived from the MAC at registration), `ops`, `priv`, `stats`
(`rx_packets/bytes/dropped/errors`, `tx_*`), `link`, `lock` (spinlock
for addresses and flags). Storage belongs to the driver (or is static,
as for `lo`) and must outlive the registration.

**`void net_init(void)`** Once, from `kernel_main` after
`ramfs_populate_boot` and before `module_load_boot` (drivers register
interfaces during module init). Creates the mbuf caches, initialises
ARP, ND, UDP, TCP and sockets, starts the `netrx` thread (priority 40)
and registers `lo`. Logs `net: stack ready`.

**`int netif_register(struct netif *nif)`** Adds an interface prepared
by the driver (`name`, `mac`, `mtu`, `ops`, `priv`, `flags`): assigns
`index`, the IPv6 link-local address, and for non-loopback interfaces
the IPv4 configuration from fw_cfg `opt/cosmo/ipv4`
(`"a.b.c.d/prefix,gateway"`) or the QEMU user-mode defaults
(`10.0.2.15/24`, gateway `10.0.2.2`). Logs `net: <name> registered
(<mac>, mtu <n>)`. `-EINVAL` (empty name, no `transmit`, MTU below
68), `-EEXIST` for a duplicate name. Thread context (a mutex guards the
list).

**`void netif_unregister(struct netif *nif)`** Removes the interface and
flushes its ARP entries (pending packets freed). Packets already on the
receive queue with `rcvif == nif` are still processed by the worker;
drivers must call `netif_set_up(nif, false)` first and stop delivering.
Thread context.

**`struct netif *netif_find(const char *name)`**, **`netif_default(void)`**
(the first non-loopback interface or NULL), **`netif_loopback(void)`**:
borrowed pointers, valid while the interface stays registered. Any
context.

**`void netif_set_ipv4(nif, addr, mask, gateway)`** Sets the addresses
(network order) and logs them. **`void netif_set_up(nif, bool up)`**
Sets or clears `NETIF_UP`; output through a down interface fails with
`-ENETUNREACH`. Any context.

**`bool netif_owns_ipv4(uint32_t addr)`, `bool netif_owns_ipv6(const struct in6_addr *a)`**
True when the address belongs to a registered interface (loopback
addresses included). Any context.

**`void netif_rx(struct netif *nif, struct mbuf *m)`** Driver to stack:
stamps `pkt.rcvif` and `pkt.rx_ns`, enqueues on the 512-packet receive
queue and wakes the worker. Takes the packet; drops (freed, counted in
`rx_dropped`) when the queue is full. **Any context**, including
interrupt handlers and virtio callbacks. `m->data` must point at the
Ethernet header and `pkt.len` must be set; for `NETIF_LOOPBACK`
interfaces `m->data` is at the IP header and `pkt.proto` carries the
EtherType (`ETH_P_IP`/`ETH_P_IPV6`) that selects the input function.

**`int netif_transmit(struct netif *nif, struct mbuf *m)`** Stack to
driver: counts, checks `NETIF_UP` (`-ENETUNREACH` otherwise, packet
freed) and calls `ops->transmit`. Takes the packet. Thread context, no
spinlock held.

**Deferred work**: `struct net_work { link, fn, arg, queued }`;
`net_work_init(w, fn, arg)`; `net_work_queue(w)` schedules `fn(arg)` on
the worker thread once (idempotent while queued). Any context; this is
how timers reach protocol code (they never send from interrupt
context).

**`void loopback_set_filter(lo_filter_fn fn, void *arg)`** Test hook:
`fn(m, arg)` runs on the sender's thread for every packet transmitted
through `lo`; returning `false` drops it (freed by the caller). Pass
NULL to clear. **`void loopback_init(void)`** registers `lo` (MTU
65535, `NETIF_LOOPBACK | NETIF_NOARP | NETIF_UP`, `127.0.0.1/8`).

**`void netif_dump(void)`** Logs every interface with counters.

## Ethernet and ARP (`kernel/include/kernel/net/ether.h`, `ether.c`, `arp.c`)

Constants: `ETH_ALEN` 6, `ETH_HLEN` 14, `ETH_ZLEN` 60 (minimum frame
without FCS), `ETH_P_IP`, `ETH_P_ARP`, `ETH_P_IPV6`; `struct eth_hdr`;
`eth_broadcast`.

**`void ether_input(struct netif *nif, struct mbuf *m)`** Worker thread
only. Validates the frame, sets `M_BCAST/M_MCAST`, strips the header
and dispatches by EtherType to `arp_input`, `ipv4_input`,
`ipv6_input`; anything else is counted in `rx_errors` and freed.

**`int ether_output(struct netif *nif, struct mbuf *m, const uint8_t dst[6], uint16_t type)`**
Prepends the header (`type` in host order), pads to `ETH_ZLEN` (QEMU's
user-mode backend drops runt frames) and calls `netif_transmit`.
Thread context. Takes the packet.

**ARP**: `ARP_TABLE_SIZE` 64 static entries under one spinlock.
**`int arp_resolve(nif, ip, mac, m)`** returns `0` with `mac` filled
when the address is known; `-EINPROGRESS` when it queued `m` on the
entry (replacing and freeing an older pending packet, `pending_dropped`)
and took ownership, sending a request when the entry is new (an
incomplete entry already has one in flight). No other value is
returned today; when the table is full the least recently updated
reachable entry (else the oldest incomplete one) is evicted with its
pending packet. Broadcast destinations
resolve to `ff:ff:ff:ff:ff:ff` at once. Reachable entries age out after 20 minutes;
incomplete ones retry every second and give up after 3 requests
(pending packet freed, `timeouts` counted). **`arp_input`** (worker)
answers requests for our address and records the asker (without
evicting: a full table learns nothing), completes an incomplete entry
from a reply addressed to us and transmits its pending packet, and
ignores everything else: unsolicited replies (`unsolicited`) and
requests for other hosts never create or change an entry. **`bool arp_lookup(ip,
mac)`** reads the table without sending. **`void arp_flush(nif)`**
drops that interface's entries. **`void arp_age(uint64_t now_ns)`**
runs the ageing pass as if `now_ns` were the current time (the 1 s
timer queues it on the worker together with `nd_age`; tests call it
directly). **`arp_get_stats`**: `requests_sent`, `replies_sent`,
`requests_rcvd`, `replies_rcvd`, `entries`, `pending_dropped`, `unsolicited`,
`timeouts`. Everything but `arp_input`/`arp_age` is any context.

## IP, ICMP and neighbour discovery (`kernel/include/kernel/net/ip.h`, `ipv4.c`, `ipv6.c`)

Constants: `IPPROTO_ICMP/TCP/UDP/ICMPV6`, `IP_DEFAULT_TTL` 64, `struct
ipv4_hdr`, `IPV4_HDR_LEN(h)`, `struct ipv6_hdr`, `struct icmp_hdr`, the
ICMP and ICMPv6 type and code numbers used.

**`void ipv4_input(nif, m)`, `void ipv6_input(nif, m)`** Worker thread
only, `m->data` at the IP header. Validate version, header length,
total length against the chain, the v4 header checksum, and the
destination (ours, or v4 broadcast); drop fragments (`rx_fragments`),
martians (a loopback or own source address arriving on a real
interface) and packets not for us (`rx_not_for_us`); dispatch on the
protocol to `icmp_input`/`icmpv6_input`, `udp_input`, `tcp_input`
(the transport gets `m->data` at its header plus a pointer to the IP
header still in the buffer); an unknown protocol on a unicast v4
packet earns "protocol unreachable". IPv6 extension headers are not
parsed (`rx_unknown_proto`).

**`int ipv4_output(m, src, dst, proto, ttl)`**, **`int ipv6_output(m, src, dst, proto, hoplimit)`**
Thread context, `m->data` at the transport header; takes the packet.
Chooses the interface (`lo` for our own and loopback addresses,
otherwise `netif_default`), fills the header (v4: DF set, `id` from a
counter, checksum), resolves the next hop (the destination when on the
subnet or link-local/multicast, else the gateway) through
`arp_resolve`/`nd_resolve` and hands the frame to `ether_output`.
`-ENETUNREACH` (no interface, interface down, no gateway),
`-EMSGSIZE` (larger than the MTU: no fragmentation), `-ENOMEM`; a
pending resolution is reported as `0` (the packet is queued).

**`uint32_t ipv4_source_for(dst)`, `void ipv6_source_for(dst, src)`**
Source address selection: the loopback address for loopback
destinations, the default interface's address (link-local for v6)
otherwise, 0/unspecified when there is none. **`ipv4_route(dst)`,
`ipv6_route(dst)`**: the interface a destination leaves through, or
NULL. Any context.

**`void icmp_input(nif, m, iph)`** Worker only: verifies the checksum;
echo requests are answered in place (the mbuf is turned into the
reply); other types are counted and dropped. **`void icmp_send_unreach(orig, iph, code)`**
Sends destination unreachable with `code` quoting the first 8 bytes of
the original datagram; `orig` is **borrowed**. Not sent for
broadcast/multicast. **`icmp_set_echo_reply_hook(fn)`** registers an
observer `fn(src, id, seq)` for incoming echo replies (tests);
**`int icmp_send_echo(dst, id, seq, payload, len)`** sends a request
(`-EMSGSIZE` above 1400 payload bytes). Thread context.

**`void icmpv6_input(nif, m, ip6)`** Worker only: checksum (mandatory),
echo request/reply, NS/NA (hop limit must be 255) to `nd_input_ns` /
`nd_input_na`.

**Neighbour discovery**: `nd_init`, **`int nd_resolve(nif, ip, mac, m)`**
(same contract as `arp_resolve`: multicast destinations resolve to
`33:33:xx:xx:xx:xx` immediately; a solicitation goes to the
solicited-node multicast group; 32-entry table, 1 s retries, 3
tries, 20 min ageing), `nd_input_ns`, `nd_input_na`, `nd_age(now_ns)`.

**`ipv4_get_stats`, `ipv6_get_stats`** (`struct ip_stats`: `rx`,
`rx_bad_header`, `rx_bad_cksum`, `rx_not_for_us`, `rx_fragments`,
`rx_unknown_proto`, `tx`, `tx_no_route`, `icmp_echo_rcvd`,
`icmp_echo_replied`, `icmp_unreach_sent`). Any context.

## UDP (`kernel/include/kernel/net/udp.h`, `udp.c`)

Constants: `struct udp_hdr`, `UDP_RXQ_MAX` 64 datagrams per pcb,
`NET_EPHEMERAL_LO` 49152, `NET_EPHEMERAL_HI` 65535 (shared with TCP).

**`struct udp_pcb`**: `local` (family always set, port 0 = unbound),
`remote` (connected peer or unspecified), `rxq`, `sock` (the owning
socket, woken on arrival), `link`, `rx_dropped`. Embedded in
`struct socket`; the caller owns the storage.

**`void udp_init(void)`**; **`int udp_pcb_init(pcb, family)`** (zeroes
the pcb and its queue; `-EAFNOSUPPORT`).

**`int udp_bind(pcb, local)`** Registers (address, port) in the global
pcb list under the UDP spinlock; port 0 picks an ephemeral port.
`-EINVAL` (already bound, wrong family), `-EADDRINUSE` (the same port
with an equal or wildcard address), `-EADDRNOTAVAIL` (an address we
do not own). Any context. **`void udp_unbind(pcb)`** removes it and
drains the queue.

**`int udp_sendto(pcb, data, len, to)`** Builds a datagram (binding an
ephemeral port first when unbound), computes the checksum (generated
for v4, mandatory for v6, 0 mapped to 0xffff) and calls the IP output.
`-EAFNOSUPPORT` (family mismatch), `-EINVAL` (port 0 or unspecified
destination), `-EMSGSIZE` (above 65507 bytes, or above the interface
MTU from the IP output), `-EADDRNOTAVAIL`, `-ENOMEM`, IP output
errors. Thread context (allocates, transmits).

**`struct mbuf *udp_recv(pcb)`** Dequeues one datagram whose `pkt.src`
is the sender and `pkt.len` its payload length, or NULL. Any context.
The caller frees it.

**`void udp_input(nif, m, ip4, ip6)`** Worker only: length and
checksum checks (`rx_bad_len`, `rx_bad_cksum`), lookup by destination
port and address (exact, then wildcard), enqueue (`rx_queue_full`) and
wake the socket; no listener earns an ICMP port unreachable on v4
unicast (`rx_no_port`). **`udp_get_stats`**: `rx`, `rx_bad_len`,
`rx_bad_cksum`, `rx_no_port`, `rx_queue_full`, `tx`.

## TCP (`kernel/include/kernel/net/tcp.h`, `tcp.c`)

Constants: `struct tcp_hdr`, `TH_FIN/SYN/RST/PSH/ACK`, `TCP_HDR_LEN`,
`enum tcp_state` (the eleven RFC 793 states), `TCP_SNDBUF` and
`TCP_RCVBUF` 65536, `TCP_MSS_V4` 1460, `TCP_MSS_V6` 1440, `TCP_MSS_LO`
16384, `TCP_MAX_REXMIT` 8, RTO bounds 200 ms .. 60 s (initial 1 s),
`TCP_DELACK_NS` 40 ms, `TCP_TIMEWAIT_NS` 2 s, `TCP_MAX_BACKLOG` 16.

**`struct tcp_pcb`**: state; `local`/`remote`; send variables `iss`,
`snd_una`, `snd_nxt`, `snd_wnd`, `snd_wl1`, `snd_wl2`, `snd_max`,
`mss`, `sndbuf` (a `struct netbuf` byte ring holding bytes from
`snd_una`); receive variables `irs`, `rcv_nxt`, `rcv_wnd`, `rcvbuf`;
congestion and RTT state (`cwnd`, `ssthresh`, `dupacks`, `srtt_ns`,
`rttvar_ns`, `rto_ns`, one timed sequence number); timers `rexmit`,
`delack`, `timewait` with `rexmit_count`; flags `delack_pending`,
`fin_queued`, `fin_sent`, `fin_rcvd`; passive-open fields `listener`,
`accept_link`, `accept_queue`, `backlog`, `nr_queued`; `sock`, `link`,
`error` (the asynchronous error the socket layer reports),
`retransmits`, `segs_in`, `segs_out`, `work`/`work_flags` (the timers'
hand-off to the worker). Allocated by `tcp_pcb_new`; freed by
`tcp_close` or, when the connection lingers (TIME_WAIT, a FIN in
flight), by the pcb itself on the worker once the state machine ends.
After `tcp_close` the caller must not touch it.

All TCP functions take the single TCP spinlock internally; none may be
called with it held. Segments are built under the lock into a
`struct tcp_batch` (16 at most) and transmitted after it is released,
so no function holds a spinlock across a driver.

**`void tcp_init(void)`** Seeds the ephemeral port counter from
`random_u64`.

**`struct tcp_pcb *tcp_pcb_new(uint16_t family)`** A CLOSED pcb with
allocated 64 KiB send and receive rings, `mss` by family, timers
initialised. NULL without memory. Thread context (allocates).

**`void tcp_close(struct tcp_pcb *pcb)`** Detaches the socket
(`pcb->sock = NULL`) and: frees a CLOSED or SYN_SENT pcb; for a
listener, resets and frees every unaccepted child and frees itself;
for SYN_RCVD/ESTABLISHED/CLOSE_WAIT sends RST and frees when unread
data remains (RFC 2525 2.17), otherwise queues a FIN after the
buffered data (FIN_WAIT_1 or LAST_ACK) and lets the pcb finish and
free itself; in the other closing states it does nothing more. Thread
context.

**`int tcp_bind(pcb, local)`** Records the local address and port
(ephemeral when 0). `-EAFNOSUPPORT` (family), `-EINVAL` (not CLOSED),
`-EADDRINUSE` (a pcb other than a TIME_WAIT one uses the port with an
overlapping address), `-EADDRNOTAVAIL`.

**`int tcp_listen(pcb, backlog)`** LISTEN with `backlog` clamped to
`1..TCP_MAX_BACKLOG`. `-EINVAL` unless bound and CLOSED.

**`struct tcp_pcb *tcp_accept(pcb)`** Removes and returns one
ESTABLISHED (or CLOSE_WAIT) child from the accept queue, or NULL. The
child's `sock` is NULL until `tcp_attach_socket`.

**`int tcp_connect(pcb, remote)`** Picks the source address and an
ephemeral port when unbound, chooses `mss` by route (`TCP_MSS_LO` over
`lo`), sends a SYN with the MSS option and enters SYN_SENT. Completion
is asynchronous: the worker moves the pcb to ESTABLISHED (or sets
`error` to `-ECONNREFUSED`/`-ETIMEDOUT` and CLOSED) and calls
`sock_wake`. `-EAFNOSUPPORT` (family), `-EINVAL` (unspecified peer),
`-EALREADY` (SYN already sent), `-EISCONN` (not CLOSED otherwise),
`-ENETUNREACH` (no route or interface down), `-EADDRINUSE` (no free
port), output errors. Thread context.

**`int64_t tcp_send(pcb, data, len)`** Copies into `sndbuf` and
transmits what the windows allow. Returns the bytes taken, which may
be fewer than `len` or 0 when the ring is full (the caller waits on
`tcp_send_space`); `-EAGAIN` while the handshake is in progress,
`-EPIPE` after `tcp_shutdown_write` or in a state that cannot send,
`pcb->error` when set. Thread context (`data` is kernel memory).

**`int64_t tcp_recv(pcb, data, len, bool *peer_closed)`** Copies out of
`rcvbuf` and recomputes `rcv_wnd` (sending a window update when the
window was below one MSS and is now at least one MSS, so a stalled
peer resumes). Returns the bytes copied; 0 with
`*peer_closed = true` at EOF (FIN received and buffer drained), 0
with `false` when nothing is available yet; `pcb->error` when set.

**`int tcp_shutdown_write(pcb)`** Queues a FIN after the buffered data
(ESTABLISHED → FIN_WAIT_1, CLOSE_WAIT → LAST_ACK). `-ENOTCONN`
otherwise.

**`uint32_t tcp_send_space(pcb)`, `uint32_t tcp_recv_avail(pcb)`,
`bool tcp_accept_ready(pcb)`, `enum tcp_state tcp_state_of(pcb)`**
Lock-free reads for wait conditions; the value may be stale by the
time it is used, which the callers tolerate by re-checking under the
protocol call. Any context.

**`void tcp_attach_socket(pcb, sock)`** Sets `pcb->sock` under the
lock (accepted children).

**`void tcp_input(nif, m, ip4, ip6)`** Worker only, `m->data` at the
TCP header. Verifies the checksum (`bad_cksum`), looks up the exact
four-tuple then a listener on the local port (a segment for no pcb
earns a RST unless it carries one, `dropped_no_pcb`), and runs the
state machine: SYN to a listener creates a child when `nr_queued <
backlog` (otherwise the SYN is dropped and the client retransmits);
ACK processing frees `sndbuf`, updates the window, samples the RTT
(RFC 6298), grows `cwnd` (slow start, then one MSS per RTT), counts
duplicate ACKs (three: halve `ssthresh`, retransmit one segment); RST
sets `error` (`-ECONNREFUSED` in SYN_RCVD, `-ECONNRESET` otherwise)
and closes; in-order data is appended to `rcvbuf` and acknowledged
immediately every second segment or after 40 ms, out-of-order data is
acknowledged with `rcv_nxt` and dropped (`out_of_order`); FIN moves
through CLOSE_WAIT / TIME_WAIT (2 s) and wakes the socket. When
TIME_WAIT ends the pcb is freed if its socket is gone; otherwise it
becomes CLOSED and waits for `tcp_close`.

**`tcp_get_stats`** (`segs_in`, `segs_out`, `retransmits`,
`bad_cksum`, `rsts_in`, `rsts_out`, `conns_active`, `conns_passive`,
`conns_established`, `dropped_no_pcb`, `out_of_order`, `timeouts`);
**`const char *tcp_state_name(s)`**.

## Sockets (`kernel/include/kernel/socket.h`, `socket.c`)

**`struct socket`**: `obj` (a `kobject` of the `socket` io type:
`read` is `ksock_recvfrom(s, buf, len, NULL)`, `write` is
`ksock_sendto(s, buf, len, NULL)`), `family`, `type`, `state`
(`SS_UNCONNECTED`, `SS_BOUND`, `SS_LISTENING`, `SS_CONNECTING`,
`SS_CONNECTED`, `SS_CLOSED`), `udp` (embedded pcb, datagram sockets),
`tcp` (allocated pcb, stream sockets), `wait`, `error` (consumed by
the next call that reports it), `shut` (1 = read, 2 = write), `lock`
(a mutex serialising callers; protocol spinlocks nest inside), `uid`
(the creator, for privileged ports). `SOCK_IO_CHUNK` 4096 is the
system-call bounce-buffer size. Lifetime: a reference-counted kobject
(`ksock_get`/`ksock_put`); the last put runs `socket_release`, which
unbinds the UDP pcb or calls `tcp_close`, then frees.

Every `ksock_*` function runs in thread context and may block on
`s->wait`; none may be called from interrupt context or with a
spinlock held.

**`int ksock_create(family, type, uid, out)`** `-EAFNOSUPPORT` (family
not INET/INET6), `-EINVAL` (type not STREAM/DGRAM), `-ENOMEM`. The
socket is returned with one reference.

**`int ksock_bind(s, addr)`** `-EAFNOSUPPORT` (family mismatch),
`-EPERM` (port 1..1023 with `uid != 0`), `-EINVAL` (already bound,
listening or connected), plus `udp_bind`/`tcp_bind` errors.

**`int ksock_listen(s, backlog)`** `-EOPNOTSUPP` (datagram), `-EINVAL`
(not bound, or already listening).

**`int ksock_accept(s, out, peer)`** Blocks until a child is
established; returns a new connected socket (one reference,
`SS_CONNECTED`) and the peer address. `-EOPNOTSUPP`, `-EINVAL` (not
listening, or the listener was shut down for reading while waiting),
the pending error, `-ENOMEM` (the child is closed).

**`int ksock_connect(s, addr)`** Datagram: records the peer, no
traffic. Stream: sends the SYN and blocks until ESTABLISHED
(`SS_CONNECTED`) or failure (`-ECONNREFUSED`, `-ETIMEDOUT` after 8
SYN retransmissions, `-ECONNRESET`); `-EISCONN`, `-EINVAL` (listening),
`-EAFNOSUPPORT`, `tcp_connect` errors.

**`int64_t ksock_sendto(s, buf, len, to)`** `-EPIPE` after a write
shutdown. Datagram: `to` NULL requires a connected socket
(`-ENOTCONN`); returns `len` or the `udp_sendto` error; the socket
becomes `SS_BOUND` on first use. Stream: `to` must be NULL
(`-EISCONN`), the socket connected (`-ENOTCONN`); copies everything,
blocking while the send ring is full; on an error midway returns the
bytes already taken, else the error (`-EPIPE`, `-ECONNRESET`,
`-ETIMEDOUT`).

**`int64_t ksock_recvfrom(s, buf, len, from)`** Datagram: `-EINVAL`
when unbound (nothing can arrive); blocks for one datagram and copies
at most `len` bytes of it (the rest is discarded); `from` gets the
sender. Stream: `-ENOTCONN` unless connected; blocks for data; returns
bytes, 0 at EOF (peer FIN, or read shutdown), `pcb->error` on a
reset. 0 immediately after `shutdown(SHUT_RD)`.

**`int ksock_shutdown(s, how)`** `COSMO_SHUT_RD/WR/RDWR` (`-EINVAL`
otherwise); write shutdown on a connected stream queues a FIN; wakes
every waiter.

**`int ksock_getsockname(s, out)`** Always succeeds (port 0 when
unbound). **`int ksock_getpeername(s, out)`** `-ENOTCONN` unless
connected.

**`struct socket *socket_from_kobject(struct kobject *obj)`** The
socket, or NULL when `obj` has another type (`sock_of` in `native.c`
uses it to answer `-EBADF` for a file or console handle).

**`void sock_wake(s)`, `void sock_set_error(s, err)`** Protocol side,
any context: wake all waiters (and record `err`). **`unsigned
socket_count(void)`** live sockets (tests check for leaks).

## System calls (`kernel/include/uapi/cosmo/syscall.h`, `kernel/syscall/native.c`)

**ABI stability: stable.** Numbers 23–31; `SYS_COUNT` is 32.

```c
#define COSMO_AF_INET  2
#define COSMO_AF_INET6 10
#define COSMO_SOCK_STREAM 1
#define COSMO_SOCK_DGRAM  2
#define COSMO_SHUT_RD 0, COSMO_SHUT_WR 1, COSMO_SHUT_RDWR 2
struct cosmo_sockaddr {
    uint16_t family;    /* COSMO_AF_* */
    uint16_t port;      /* host byte order */
    uint32_t flowinfo;  /* IPv6, ignored today; write 0 */
    uint8_t  addr[16];  /* network byte order; IPv4 uses addr[0..3] */
    uint32_t scope;     /* IPv6 scope id, ignored today; write 0 */
};                      /* 28 bytes */
```

| Nr | Name | Arguments | Result | Errors |
|---|---|---|---|---|
| 23 | `socket` | `int family, int type, int proto` (0) | handle with READ and WRITE rights | `EAFNOSUPPORT`, `EINVAL`, `ENOMEM`, `EMFILE` |
| 24 | `bind` | `int h, const struct cosmo_sockaddr *sa, size_t len` | 0 | `EBADF` (not a socket), `EFAULT`, `EINVAL` (`len` below 28), `EAFNOSUPPORT`, `EPERM`, `EADDRINUSE`, `EADDRNOTAVAIL` |
| 25 | `listen` | `int h, int backlog` | 0 | `EBADF`, `EOPNOTSUPP`, `EINVAL` |
| 26 | `accept` | `int h, struct cosmo_sockaddr *peer, size_t *len` (both may be NULL) | new handle | `EBADF` (needs READ), `EOPNOTSUPP`, `EINVAL`, `EFAULT`, `EMFILE`, the connection's error |
| 27 | `connect` | `int h, const struct cosmo_sockaddr *sa, size_t len` | 0 | `EBADF`, `EFAULT`, `EINVAL`, `EAFNOSUPPORT`, `EISCONN`, `ECONNREFUSED`, `ETIMEDOUT`, `ENETUNREACH`, `EADDRNOTAVAIL` |
| 28 | `sendto` | `int h, const void *buf, size_t len, const struct cosmo_sockaddr *to, size_t tolen` (`to` NULL for connected sockets) | bytes sent | `EBADF` (needs WRITE), `EFAULT`, `EINVAL`, `EMSGSIZE`, `ENOTCONN`, `EISCONN`, `EPIPE`, `ECONNRESET`, `ETIMEDOUT`, `ENOMEM` |
| 29 | `recvfrom` | `int h, void *buf, size_t len, struct cosmo_sockaddr *from, size_t *fromlen` (both may be NULL) | bytes received, 0 at end of stream | `EBADF` (needs READ), `EFAULT`, `EINVAL`, `ENOTCONN`, `ECONNRESET`, `ETIMEDOUT`, `ENOMEM` |
| 30 | `shutdown` | `int h, int how` | 0 | `EBADF`, `EINVAL` |
| 31 | `getsockname` | `int h, struct cosmo_sockaddr *sa, size_t *len` | 0 | `EBADF`, `EFAULT` |

Details: user buffers are checked with the same window rule as `read`
and `write`, and every copy goes through a kernel buffer so no user
fault happens under a protocol lock. A datagram `sendto` copies the
whole message first (`EMSGSIZE` above 64 KiB); a stream `sendto` copies
`SOCK_IO_CHUNK` (4096) bytes at a time and returns the bytes sent
before a fault or error, or the error when nothing was sent.
`recvfrom` receives at most 64 KiB per call into a kernel buffer, then
copies out. An address length shorter than the structure is `EINVAL`;
a longer one is accepted (only 28 bytes are read). `*len` on output
(`accept`, `recvfrom`, `getsockname`) is set to `sizeof(struct
cosmo_sockaddr)`; on input it is ignored. A socket handle also answers
`read`, `write` and `close` (numbers 2, 1, 10) through the
`kobject_io_type`. New error numbers: `COSMO_EPIPE` 32,
`COSMO_EMSGSIZE` 90, `COSMO_EOPNOTSUPP` 95, `COSMO_EAFNOSUPPORT` 97,
`COSMO_EADDRINUSE` 98, `COSMO_EADDRNOTAVAIL` 99, `COSMO_ENETUNREACH`
101, `COSMO_ECONNRESET` 104, `COSMO_ENOBUFS` 105, `COSMO_EISCONN` 106,
`COSMO_ENOTCONN` 107, `COSMO_ETIMEDOUT` 110, `COSMO_ECONNREFUSED` 111,
`COSMO_EHOSTUNREACH` 113, `COSMO_EALREADY` 114, `COSMO_EINPROGRESS`
115 (the last two are defined for completeness; no call returns them
yet since sockets are blocking).

User-side wrappers (`libc/include/cosmo/syscall.h`, stable names):
`cosmo_syscall5`, `cosmo_socket(family, type, proto)`, `cosmo_bind(h,
sa)`, `cosmo_listen(h, backlog)`, `cosmo_accept(h, peer, len)`,
`cosmo_connect(h, sa)`, `cosmo_sendto(h, buf, len, to)`,
`cosmo_recvfrom(h, buf, len, from, fromlen)`, `cosmo_shutdown(h, how)`,
`cosmo_getsockname(h, sa, len)`; the address wrappers pass
`sizeof(struct cosmo_sockaddr)` (0 for a NULL `to`).

## Boot parameters (`kernel/include/kernel/fwcfg.h`, `kernel/include/arch/fwcfg.h`)

**`bool fwcfg_get_string(const char *key, char *buf, size_t len)`**
Copies the firmware item `opt/cosmo/<key>` as a NUL-terminated string
into `buf` (truncated to `len - 1`); `false` when the item or the
device is absent. Any context (a spinlock around the port accesses);
each call walks the file directory, so it is for boot-time
configuration, not hot paths. Keys used: `ipv4`
(`"a.b.c.d/prefix,gateway"`, read by `netif_register`), `nettest`
(`"tcp=<port>"`, read by the `net-harness` self-test).

**`int arch_fwcfg_read(const char *name, void *buf, size_t len)`**
(`arch/fwcfg.h`, implemented by `kernel/arch/x86_64/fwcfg.c`): copies
at most `len` bytes of the named item and returns its full size (which
may exceed `len`), `-ENOENT`, or `-ENODEV` when the `QEMU` signature
is not at selector 0 (real hardware). The x86-64 implementation uses
the traditional I/O ports 0x510 (selector) and 0x511 (data) and the
`FW_CFG_FILE_DIR` (0x19) directory. Architecture-neutral callers use
`fwcfg_get_string`.

## Module exports

Added to module ABI v1 in this phase (`EXPORT_SYMBOL` at the end of
`mbuf.c` and `netif.c`): `m_get`, `m_getcl`, `m_free`, `m_freem`,
`m_prepend`, `m_pullup`, `m_adj`, `m_copydata`, `m_append`,
`m_length`, `m_copypacket`, `netif_register`, `netif_unregister`,
`netif_rx`, `netif_set_ipv4`, `netif_set_up`. This is the whole
surface a NIC driver module needs; `virtio_net.ko` uses nothing else
from the stack.
