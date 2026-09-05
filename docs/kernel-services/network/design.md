# Networking: design

## Data structures

### mbuf (`kernel/include/kernel/mbuf.h`)

```c
#define MCLBYTES 2048u                      /* one cluster: a DMA-able slab object */
#define MHLEN    128u                       /* inline storage of a small mbuf */

struct mbuf {
    struct mbuf *next;                      /* next buffer of the same packet */
    struct mbuf *nextpkt;                   /* next packet in a queue */
    uint8_t *data;                          /* first valid byte */
    uint32_t len;                           /* valid bytes at data */
    uint32_t flags;                         /* M_PKTHDR, M_EXT, M_BCAST, M_CSUM_OK ... */
    uint32_t refcount;                      /* atomic; > 1 only for shared clusters */
    /* storage */
    uint8_t *buf;                           /* start of storage (inline or cluster) */
    uint32_t size;                          /* storage size */
    /* packet header (first mbuf of a packet only, M_PKTHDR) */
    struct {
        uint32_t len;                       /* total bytes in the chain */
        struct netif *rcvif;
        uint64_t rx_ns;
        uint16_t proto;                     /* EtherType / next header, set as layers parse */
        uint16_t csum_flags;
        struct netaddr src;                 /* UDP: the sender, for recvfrom */
    } pkt;
    struct mbuf_cluster *cl;                /* M_EXT: { uint32_t refcount; uint8_t data[MCLBYTES]; } */
    uint8_t inl[MHLEN];
};
```

Ownership: an mbuf pointer carries exactly one reference. `m_free(m)`
drops one reference on one buffer (returns `m->next`); `m_freem(m)`
frees a whole chain. A cluster-backed mbuf whose cluster is shared
(`m_ref`) keeps the cluster until the last reference goes. Functions
that take an mbuf and return one (`m_prepend`, `m_pullup`) may return
a different pointer and consume the argument on failure (returning
NULL), so callers write `m = m_prepend(m, n)`. Layers going down
(`netif_transmit`) and up (`netif_rx`, protocol input) take ownership
of the packet they are handed. Headroom: `m_getcl()` for transmit
places `data` `NET_HEADROOM` (64) bytes in so headers can be prepended
without allocation. `m_pullup(m, n)` guarantees the first `n` bytes are
contiguous in the first buffer (copying from following buffers when
needed) so header casts are safe; every header access in the stack is
preceded by it. Memory comes from two slab caches (`mbuf_cache`,
`mcluster_cache`); clusters are kmalloc-style direct-map memory, so
`dma_map` succeeds on them.

`struct mbufq { struct mbuf *head, *tail; unsigned len, maxlen; spinlock_t lock; }`
with `mbufq_enqueue` (frees the packet and returns false when full),
`mbufq_dequeue`, `mbufq_drain`, `mbufq_len`; IRQ-safe.

### Interfaces (`kernel/include/kernel/netif.h`)

```c
struct netif_ops {
    int  (*transmit)(struct netif *nif, struct mbuf *m);     /* takes the packet */
};
struct netif {
    char name[8];                            /* "lo", "eth0" */
    unsigned index;
    uint8_t mac[6];
    uint32_t mtu;                            /* 1500; lo 65535 */
    unsigned flags;                          /* NETIF_UP, NETIF_LOOPBACK, NETIF_NOARP */
    struct { uint32_t addr, mask, gateway; } ip4;   /* network byte order, 0 = unset */
    struct in6_addr ip6_ll;                  /* link-local */
    const struct netif_ops *ops;
    void *priv;
    struct netif_stats stats;               /* rx/tx packets, bytes, drops, errors */
    struct list_node link;
    spinlock_t lock;                         /* addresses and flags */
};
```

`netif_rx(nif, m)` runs in any context (a driver's interrupt handler):
it stamps `m->pkt.rcvif`, enqueues on the global receive queue
(`mbufq`, IRQ-safe spinlock, 512 packets) and wakes the worker.
`netrx` (one kernel thread, priority 40) dequeues up to 64 packets
per wake and calls `ether_input` (or `ipv4_input`/`ipv6_input`
directly for `NETIF_LOOPBACK` interfaces, which have no link layer and
leave the EtherType in `pkt.proto`), then runs the deferred work list
(`struct net_work`, queued by `net_work_queue` from timers). All protocol *input* processing therefore runs on one
thread; protocol *output* runs on the caller's thread. Protocol tables
are protected by spinlocks that are taken from both sides; no lock is
held across a `transmit`. Timers (ARP/ND ageing, TCP) run in interrupt context
and only queue a `struct net_work` for the worker; they never send
directly.

### Address resolution (`arp.c`, `nd.c`)

```c
struct arp_entry { uint32_t ip; uint8_t mac[6]; enum { ARP_INCOMPLETE, ARP_REACHABLE } state;
                   uint64_t updated_ns; struct mbuf *pending; unsigned tries; struct netif *nif; };
```

A fixed table (`ARP_TABLE_SIZE` 64, spinlock). When our own
resolution finds the table full it evicts the least recently updated
reachable entry (an incomplete one has a resolution in flight);
learning from received traffic never evicts and simply learns nothing
when the table is full. `arp_resolve(nif, ip, mac_out, m)`
returns the MAC when known (broadcast addresses immediately);
otherwise it queues `m` on the entry (replacing an older pending
packet, which is freed), sends a request for a new entry and returns
`-EINPROGRESS`; the reply's `arp_input` fills the
entry and transmits the pending packet. Entries age out after 20 min
(`REACHABLE`) or 3 unanswered requests at 1 s (`INCOMPLETE`, pending
packet freed), driven by a 1 s timer that hands the work to the worker.
ARP carries no authentication, so the table learns only what RFC 826
requires: a request addressed to us records or refreshes the asker, a
reply addressed to us completes an entry we are resolving; unsolicited
replies (counted, `unsolicited`) and requests for other hosts change
nothing. A neighbour that changes its MAC is relearned from its next
request to us or when its entry ages out. ND (`ipv6.c`, `ND_TABLE_SIZE` 32) mirrors this for IPv6 with
neighbour solicitation/advertisement over ICMPv6 (hop limit 255
required) and the solicited-node multicast MAC (`33:33:ff:xx:xx:xx`).

### IP (`ipv4.c`, `ipv6.c`; ICMP lives in `ipv4.c`, ICMPv6 and ND in `ipv6.c`)

Input validates version, header length, total length against the mbuf,
header checksum (v4), hop limit, and destination (our address,
broadcast, `::1`/`127.0.0.1`). Delivery by protocol number: ICMP, UDP,
TCP; anything else counted and dropped (v4 replies "protocol
unreachable" only for unicast). Output: `ipv4_output(m, src, dst, proto,
ttl)` / `ipv6_output(...)` chooses the interface (loopback for local destinations, else the
single `eth`-class interface), fills the header, and for v4 hands the
packet to `ether_output` with the next hop (destination if on-link
else the gateway). Route selection is a function, not a table, in this
phase; a routing table with RCU is where section 36 points later.

### UDP (`udp.c`)

`struct udp_pcb` lives inside the socket; a global list under a
spinlock indexes bound (local address, port). Receive queue: `mbufq`
of 64 datagrams per socket, each mbuf carrying the sender's address in
its packet header (`pkt.src`). Ephemeral ports 49152 to 65535. Checksum
mandatory on v6, generated on v4.

### TCP (`tcp.c`)

```c
struct tcp_pcb {
    enum tcp_state state;
    struct netaddr local, remote;                   /* family-tagged addresses */
    /* send */  uint32_t iss, snd_una, snd_nxt, snd_wnd, snd_wl1, snd_wl2, snd_max; uint16_t mss;
    struct netbuf sndbuf;                           /* byte ring, TCP_SNDBUF 65536 */
    /* receive */ uint32_t rcv_nxt, rcv_wnd, irs; struct netbuf rcvbuf;   /* TCP_RCVBUF 65536 */
    /* congestion */ uint32_t cwnd, ssthresh; unsigned dupacks;
    /* RTO */ uint64_t srtt_ns, rttvar_ns, rto_ns; uint32_t rtt_seq; uint64_t rtt_start_ns;
    struct timer rexmit;  unsigned rexmit_count;  struct timer delack; struct timer timewait;
    struct net_work work; unsigned work_flags;      /* WORK_REXMIT/DELACK/TIMEWAIT/FREE: timers -> worker */
    struct list_node link;                          /* the pcb table */
    struct socket *sock;                            /* owner; NULL once closed */
    int error;                                      /* -ECONNREFUSED, -ECONNRESET, -ETIMEDOUT */
    struct list_node accept_link, accept_queue;  struct tcp_pcb *listener; unsigned backlog, nr_queued;
    bool fin_queued, fin_sent, fin_rcvd, delack_pending;
};
```

Segments in: `tcp_input(nif, m, ip4, ip6)` looks up the pcb (exact 4-tuple, else
a LISTEN pcb on the local port), validates the checksum and sequence
window, and runs the state machine of RFC 793 section 3.9 with these
simplifications: one SYN option (MSS), no reassembly queue beyond
accepting only in-order data (out-of-order segments are ACKed with
`rcv_nxt` and dropped, which the loss test shows still converges
through retransmission), RST on segments to closed ports, ACK
processing frees acknowledged bytes from `sndbuf`, updates the window,
runs the RTT estimator (RFC 6298, one sample in flight at a time), and
drives congestion control (slow start until `ssthresh`, then one MSS
per RTT; three duplicate ACKs halve `ssthresh` and retransmit; an RTO
sets `cwnd` to one MSS). Segments out: `tcp_output(pcb)` sends as much
of `sndbuf` beyond `snd_nxt` as `min(snd_wnd, cwnd)` allows in MSS-sized
segments, piggybacking ACKs, and arms the retransmit timer; the timer
handler sets `WORK_REXMIT` and queues `pcb->work` on the worker, which
retransmits from `snd_una` with a doubled RTO, giving up after 8
attempts (the connection is closed with `-ETIMEDOUT`). When the peer
advertises a zero window the retransmit timer doubles as the persist
probe. Delayed ACK: a
pure ACK is sent at once when two segments are pending or after 40 ms.
TIME_WAIT lasts 2 s in this phase (a constant, `TCP_TIMEWAIT_NS`).
Closing: `shutdown(SHUT_WR)`/`close` queue a FIN after the data; close
on a socket with unread data sends RST (RFC 2525 2.17); closing a
listener resets its unaccepted children. Listening pcbs hold an accept
queue of established children up to `backlog` (1..16); a SYN beyond it
is dropped and the client retransmits.

Locking: one TCP spinlock (`g_lock` in `tcp.c`) protects the pcb table
and every pcb in this phase; it is taken by input (worker), output
(callers) and the worker's timer handlers, never held across
`transmit` (segments are built under the lock into a `struct tcp_batch`
of at most 16 and sent by `batch_send` after unlock) and never across a
copy to or from user memory (the socket layer copies into a kernel
buffer first). Section 36 asks for finer grain; the single lock is
recorded as the thing to split (per-pcb locks plus a table lock) once
there is a workload.

### Sockets (`kernel/include/kernel/socket.h`)

```c
struct socket {
    struct kobject obj;                      /* socket_type (kobject_io_type: read/write on connected sockets) */
    int family, type;                        /* AF_INET/AF_INET6, SOCK_STREAM/SOCK_DGRAM */
    enum socket_state state;                 /* SS_UNCONNECTED, SS_BOUND, SS_LISTENING, SS_CONNECTING, SS_CONNECTED, SS_CLOSED */
    struct udp_pcb udp;                      /* embedded; SOCK_DGRAM */
    struct tcp_pcb *tcp;                     /* allocated; SOCK_STREAM */
    struct waitqueue wait;                   /* readers, writers, accepters, connectors */
    int error;                               /* pending asynchronous error, consumed by the next call */
    unsigned shut;                           /* 1 = RD, 2 = WR */
    struct mutex lock;                       /* serialises callers; protocol locks nest inside */
    uint32_t uid;                            /* creator; ports below 1024 need 0 */
};
```

`ksock_*` functions implement the blocking semantics with
`wait_event` on `sock->wait`, woken by the protocols (`sock_wake`).
Data copies: `ksock_sendto` copies from the caller's kernel buffer into
the TCP send ring under the TCP lock (or builds a UDP datagram mbuf);
`ksock_recvfrom` copies out of the receive ring/queue. System calls
copy between user memory and a kernel buffer (`SOCK_IO_CHUNK` 4096 per
stream chunk, one allocation of up to 64 KiB for a datagram or a
receive) around these calls, so user faults never happen under a
protocol lock.
Addresses cross the UAPI as `struct cosmo_sockaddr { uint16_t family;
uint16_t port; uint32_t flowinfo; uint8_t addr[16]; uint32_t scope; }`
(one shape for both families, network byte order for `addr`, host byte
order for `port`).

Handle rights: `socket()` installs the socket with READ|WRITE; `accept`
installs the child the same way.

### virtio-net (`drivers/virtio/virtio_net.c`)

Features wanted: `MAC`, `STATUS`; offloads and `MRG_RXBUF` are not
negotiated so every received packet is one buffer with a 12-byte
`virtio_net_hdr` in front. Receive queue: 32 posted mbuf clusters
(`VNET_RX_BUFS`; the whole cluster, `data = buf`, holds header plus
frame); the completion callback (interrupt context, through the
`virtio` module's MSI-X vector) drops runts, sets `len`, strips the
header with `m_adj` and calls `netif_rx`; buffers are re-posted at the
end of each completion pass. Transmit: the zeroed header is prepended
into the mbuf's headroom (`m_prepend`, no separate header pool), a
chain of more than 4 buffers is linearised with `m_copypacket` first,
every buffer is `dma_map`ped as one descriptor, and completions free
the mbufs. Removal takes the interface down, unregisters it, resets the
device and frees the posted buffers. `eth0` gets its IPv4 configuration
in `netif_register` from fw_cfg `opt/cosmo/ipv4` (`addr/prefix,gateway`)
or the QEMU user-mode defaults (`10.0.2.15/24`, `10.0.2.2`).

### fw_cfg (`kernel/arch/x86_64/fwcfg.c`, `kernel/core/fwcfg.c`, `kernel/fwcfg.h`, `arch/fwcfg.h`)

The QEMU firmware configuration device through the traditional I/O
ports (selector 0x510, data 0x511): check the `QEMU` signature, walk
the file directory (`FW_CFG_FILE_DIR`, selector 0x19), find
`opt/cosmo/<name>` and copy its bytes (`arch_fwcfg_read`, under a
spinlock). `fwcfg_get_string` is the architecture-neutral wrapper.
Every call probes; absent on real hardware (`-ENODEV`), which just
means no parameters. Strings: `opt/cosmo/ipv4`, `opt/cosmo/nettest`
(`tcp=<hostport>` the guest connects back to).

## The harness protocol (`tests/boot/run_boot_test.py`, `nettest.py`)

QEMU is started with `-netdev user,id=n0,ipv4=on,ipv6=on,hostfwd=tcp:127.0.0.1:P1-:7,hostfwd=udp:127.0.0.1:P2-:7`,
`-device virtio-net-pci,netdev=n0,mac=52:54:00:c0:5f:05` and `-fw_cfg name=opt/cosmo/nettest,string=tcp=P3`
where P1..P3 are free ports the harness picked; it listens on P3.
(`ipv4=on` is spelled out because QEMU's user-mode backend treats
`ipv6=on` alone as "IPv6 only"; the backend also drops frames shorter
than 60 bytes, which is why `ether_output` pads.) The
kernel self-test `net-harness` starts a TCP echo server and a UDP echo
server on port 7, prints `NETTEST: ready`, connects to `10.0.2.2:P3`,
sends `cosmo hello\n`, expects `cosmo world\n`, prints `NETTEST: client ok`,
then serves echo until a TCP connection delivers `QUIT` (60 s budget),
prints `NETTEST: done tcp_conns=N udp_pkts=N quit=1` and returns. The
harness tails the serial log during the run; on `ready` it echoes 256
KiB through TCP in varying chunk sizes, exchanges 20 UDP datagrams (18
must return), sends `QUIT`, and requires both markers plus its own
verification. Without fw_cfg the self-test skips, so `make run` is
unaffected. The harness timeout is 180 s; release builds create the
harness but only require the `virtio_net` and `eth0` boot markers.

## Ownership and lifetime

mbufs: as above. netifs: kobjects whose storage is freed by
`ops->release` after the last reference; the creator and the registry
each hold one, lookups and routes hand out more, and `netif_unregister`
runs the six-step protocol in `api.md` (flags, registry, grace period,
queue purge and worker barrier, table flush, registry reference) so that
no transmit, receive, queued packet or table entry names the interface
when it returns (`docs/kernel/quiesce/design.md`, "Network interfaces").
pcbs: owned by the socket until close; a pcb's three timers are cancelled
with `timer_cancel_sync` in `pcb_free_locked`, because a callback that
fired on another CPU writes `work_flags` and queues `pcb->work` (the
callbacks take only the work lock, so spinning on them under `g_lock`
cannot deadlock); a TCP child dequeued by `tcp_accept` is attached to its
socket under `g_lock` in the same step, so no reset can free it under
the accepting thread; sockets woken after a protocol lock is dropped are
held with `kobject_tryget` (the release clears `pcb->sock` under that
lock but starts at count zero). a TCP pcb outlives its socket in TIME_WAIT and
is freed by the timer once `sock` is NULL. If the application still
holds the socket when TIME_WAIT ends (shutdown without close), or the
connection ends by reset or timeout, the pcb is *retired*: it becomes
CLOSED (reads return 0 or the error, writes `-EPIPE`), leaves the pcb
table so it reserves no port and matches no segment, and is freed by
the eventual `tcp_close`; a pcb is never freed while a socket points
at it. Sockets: kobjects held by
handles and by in-kernel users; the last put closes the protocol
(`tcp_close` sends FIN or RST as appropriate). Accept-queue children
are owned by the listener until accepted or the listener closes (then
reset).

## Concurrency

Lock order (verified by the debug-build lock-order checker on every
boot, `docs/kernel/lockdep/testing.md`): `sock->lock` (mutex) →
`tcp_lock`/`udp_lock` (spinlock, IRQ-safe) → `arp_lock`/`nd_lock` →
`netif->lock` → driver locks → `mbuf` caches. `rxq.lock` is a leaf taken by drivers in interrupt
context. Timers take `tcp_lock`. The worker thread takes protocol locks
but never `sock->lock`; it wakes waiters through `waitqueue_wake_all`,
which needs no socket lock. Nothing holds a spinlock across
`transmit`, `copy_to/from_user`, or a blocking wait, and nothing under
a protocol spinlock enters a sleeping primitive: the interface registry
(`netif_find`, `netif_default`, `netif_owns_*`) is guarded by a spinlock
of its own and is read only outside the protocol locks, and TCP decides
its path MSS (`tcp_path_mss`, a registry lookup) before taking `g_lock`
and caches it in `pcb->path_mss`, on the active side from the route and
on the passive side from the interface the SYN arrived on. `mutex_lock`
asserts `preempt_count == 0` on entry, so a regression here panics in
the first handshake of `net-lo-tcp`.

## Memory

mbuf 240 bytes (inline 128), cluster object 2052 (refcount + 2048);
caches grow on demand (`kmem_cache`). Receive queue 512 packets; UDP 64
datagrams per socket; TCP 64 KiB send and 64 KiB receive rings per
connection (`kmalloc`); virtio-net posts 32 clusters. ARP (64) and ND
(32) tables are static.

## Error handling

Malformed packets are counted per layer (`netif->stats.rx_errors`,
protocol counters) and dropped without logging above debug level (a
flood must not become a log flood). Socket calls return `-EINVAL`,
`-EADDRINUSE`, `-EADDRNOTAVAIL`, `-ENOTCONN`, `-EISCONN`,
`-ECONNREFUSED`, `-ECONNRESET`, `-ETIMEDOUT`, `-EPIPE`, `-EMSGSIZE`,
`-EAGAIN` (non-blocking is not offered yet; reserved), `-EAFNOSUPPORT`,
`-EOPNOTSUPP`, `-EPERM` for ports below 1024 when uid is not 0.

## Performance

Not a goal beyond "moves 256 KiB through TCP over QEMU user-mode
networking without stalling". One worker thread, one TCP lock, copies
at the socket boundary. The shapes (mbuf chains, a receive queue per
CPU later, pcb hashing) are the ones to optimise.

## Security

Every header field that sizes anything is checked against the mbuf
before use, headers are pulled up before they are cast, and options
are skipped by length with bounds checks. ISNs come from
`random_u64()`. Ports below 1024 require uid 0. Loopback addresses are
never accepted from a real interface (martian filter). The stack drops
packets whose source is one of our own addresses arriving from `eth0`.
Parsers are host-fuzzed (bit flips over valid packets must never crash).

## Testing strategy

Host: none yet; `test_net.c` (checksum vectors incl. odd lengths and
pseudo headers; IPv4/IPv6/UDP/TCP header parsing accept/reject cases;
a bit-flip fuzz loop over parsers) is the recorded gap. Self-tests: `net-mbuf` (alloc/free,
prepend/pullup/adj/copy across chains, refcounts, queue limits),
`net-cksum`, `net-arp` (table insert/lookup/age via the timer hook),
`net-lo-udp` (v4 and v6 sendto/recvfrom over `lo`, port reuse
refused, unbound recv fails), `net-lo-tcp` (server and client threads
over `lo`, 1 MiB IPv4 and 256 KiB IPv6 transfers with verification,
orderly close, RST to a closed port, listen backlog), `net-lo-tcp-loss`
(the loopback drop hook drops every 7th data segment; the transfer
still completes; retransmission counters moved), `net-harness` (as
above, skipped without fw_cfg). Init: a user-mode UDP echo to itself
over loopback, a refused TCP connect, a privileged bind, and the error
paths of the calls. Details in `testing.md`.

## Future extensibility

Per-CPU receive queues and RSS; RCU routing table; per-pcb locks;
SACK/window scaling/timestamps; reassembly; DHCP; DNS resolver in
userland; zero-copy receive via page cache-style mappings; more NICs.
