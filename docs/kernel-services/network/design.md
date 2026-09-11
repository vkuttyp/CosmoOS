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
    spinlock_t lock; uint32_t refs;                 /* per-pcb lock; references (design: "Hardening") */
    enum tcp_state state;
    struct netaddr local, remote;                   /* family-tagged addresses */
    /* send */  uint32_t iss, snd_una, snd_nxt, snd_wnd, snd_wl1, snd_wl2, snd_max; uint16_t mss;
    struct netbuf sndbuf;                           /* byte ring, TCP_SNDBUF 65536 */
    /* receive */ uint32_t rcv_nxt, rcv_wnd, irs; struct netbuf rcvbuf;   /* TCP_RCVBUF 65536 */
    /* congestion */ uint32_t cwnd, ssthresh; unsigned dupacks;
    /* RTO */ uint64_t srtt_ns, rttvar_ns, rto_ns; uint32_t rtt_seq; uint64_t rtt_start_ns;
    struct tcp_ooo_seg ooo[TCP_OOO_MAX]; unsigned ooo_n; uint32_t ooo_bytes;   /* reassembly queue */
    struct timer rexmit;  unsigned rexmit_count;  struct timer delack; struct timer timewait;
    struct timer keep; uint64_t last_rx_ns; unsigned keep_probes;   /* keepalive and orphaned FIN_WAIT_2 */
    struct net_work work; unsigned work_flags;      /* WORK_REXMIT/DELACK/TIMEWAIT/KEEP: timers -> worker */
    struct list_node hash_link;                     /* the pcb table bucket for local.port */
    struct tcp_syncache *syncache;                  /* listeners: half-open connections */
    struct socket *sock;                            /* owner; NULL once closed */
    int error;                                      /* -ECONNREFUSED, -ECONNRESET, -ETIMEDOUT */
    struct list_node accept_link, accept_queue;  struct tcp_pcb *listener; unsigned backlog, nr_queued;
    bool fin_queued, fin_sent, fin_rcvd, delack_pending;
};
```

Segments in: `tcp_input(nif, m, ip4, ip6)` looks up the pcb (exact 4-tuple, else
a LISTEN pcb on the local port), validates the checksum and sequence
window, and runs the state machine of RFC 793 section 3.9 with these
simplifications: one SYN option (MSS), a bounded reassembly queue
(out-of-order segments inside the window are kept, up to `TCP_OOO_MAX`,
and delivered when the gap closes; each earns an immediate duplicate
ACK), RST on segments to closed ports, RFC 5961 challenge ACKs for
blind resets, SYNs and out-of-range ACKs, ACK
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
TIME_WAIT lasts 2 s in this phase (a constant, `TCP_TIMEWAIT_NS`) and
restarts only for a retransmitted FIN. An orphaned FIN_WAIT_2 ends
after `TCP_FIN_WAIT2_NS`; an idle established connection is probed
(keepalive) and ends after unanswered probes.
Closing: `shutdown(SHUT_WR)`/`close` queue a FIN after the data; close
on a socket with unread data sends RST (RFC 2525 2.17); closing a
listener resets its unaccepted children. Listening pcbs answer SYNs
from a SYN cache or with SYN cookies and allocate a child only for the
ACK that completes the handshake; the accept queue holds established
children up to `backlog` (1..16), and a completing ACK beyond it is
dropped so the client retransmits.

Locking: one spinlock per pcb and one for the hashed table; segments
are built under the pcb lock into a `struct tcp_batch` of at most 16
and sent by `batch_send` after unlock, and nothing is held across a
copy to or from user memory (the socket layer copies into a kernel
buffer first). The rules are in "Hardening and per-connection
locking" below.

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
pcbs: reference counted (the state machine, the table, the socket, the
accept queue, each lookup and each queued work item hold one; the table
in "Hardening and per-connection locking"); a pcb's four timers are
cancelled with `timer_cancel_sync` in `pcb_kill_locked` before the
state machine's reference is dropped, because a callback that fired on
another CPU writes `work_flags` and queues `pcb->work` (the callbacks
take only the work lock and atomics, so spinning on them under the pcb
lock cannot deadlock); a TCP child dequeued by `tcp_accept` is attached
to its socket under the listener's and its own lock in the same step, so
no reset can end it under the accepting thread; sockets woken after a
protocol lock is dropped are held with `kobject_tryget` (the release
clears `pcb->sock` under that lock but starts at count zero). A TCP pcb
outlives its socket in TIME_WAIT and ends when the timer fires with
`sock` NULL. If the application still
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
listener `pcb->lock` → child `pcb->lock` (subclass 1) → `tcp-table` /
`udp_lock` (spinlock, IRQ-safe) → `arp_lock`/`nd_lock` →
`netif->lock` → driver locks → `mbuf` caches. `rxq.lock` is a leaf taken by drivers in interrupt
context. Timers take no pcb lock (they queue work). The worker thread takes protocol locks
but never `sock->lock`; it wakes waiters through `waitqueue_wake_all`,
which needs no socket lock. Nothing holds a spinlock across
`transmit`, `copy_to/from_user`, or a blocking wait, and nothing under
a protocol spinlock enters a sleeping primitive: the interface registry
(`netif_find`, `netif_default`, `netif_owns_*`) is guarded by a spinlock
of its own and is read only outside the protocol locks, and TCP decides
its path MSS (`tcp_path_mss`, a registry and path-MTU cache lookup)
before taking the pcb lock and caches it in `pcb->path_mss`, on the
active side from the route and on the passive side from the interface
the SYN arrived on. `mutex_lock`
asserts `preempt_count == 0` on entry, so a regression here panics in
the first handshake of `net-lo-tcp`.

## Hardening and per-connection locking (audit milestone 8)

Milestone 8 of `docs/audit/2026-09-post-roadmap-audit.md` §19 (findings
#9 and #10, and the network items of §9.2 and §9.3). The lifetime pass
already gave `netif` a reference count and closed the accept, timer and
UDP close races; this milestone gives TCP the same shape and closes the
remaining remote-triggerable weaknesses. Everything here is decided in
this section first and implemented in `tcp.c`, `ipv4.c`, `socket.c`,
`pipe.c` and the system-call layers.

### Reference-counted pcbs and per-pcb locks

`struct tcp_pcb` gains `spinlock_t lock`, `uint32_t refs` and
`hash_link`; the single `g_lock` is gone. The lock covers the pcb's own
state; the table (`g_hash[TCP_HASH_SIZE]`, 256 buckets keyed by the
local port, holding listeners and connections alike) has its own
spinlock `g_table_lock` that covers only bucket membership and port
reservation (`port_in_use`, `pick_ephemeral`). Reference holders:

| Holder | Taken | Dropped |
|---|---|---|
| the state machine itself | `tcp_pcb_new` | when the connection ends (`pcb_kill_locked`): timers cancelled synchronously, the pcb unlinked |
| the table | insertion (`bind`, `connect`, passive open) | unlink (`pcb_kill_locked`, retire) |
| the socket | `ksock_create`, `tcp_accept` (transferred from the queue) | `tcp_close` |
| the listener's accept queue | passive open | `tcp_accept` (to the socket), listener close |
| a lookup | `lookup()` under the table lock | end of `tcp_input` |
| a queued work item | the timer callback, `pcb_get` before `net_work_queue` | end of `pcb_work` |

`pcb_put` frees at zero (rings, out-of-order queue, the pcb); it must
never be the last put from a timer callback, and it cannot be: a
callback runs only while the state machine holds its reference, since
the ending path cancels every timer synchronously before dropping it.

Lock order: `sock->lock` (mutex) → listener `pcb->lock` → child
`pcb->lock` (subclass 1, `spin_lock_irqsave_nested`) → `g_table_lock` →
`arp_lock`/`nd_lock` → `netif->lock` → drivers. The table lock is
innermost among TCP's locks so that a pcb holding its own lock may
insert or remove itself; a lookup therefore never takes a pcb lock
under the table lock: it takes the table lock, finds the pcb, takes a
reference, drops the table lock, then locks the pcb. If the pcb ended
in between (state `CLOSED`) the segment is treated as if no pcb matched.
A child never takes its listener's lock; the listener's fields a child
needs (`sock` for the accept wake-up) are read through `sock_ref`'s
`kobject_tryget`, and the listener clears `c->listener` under the
child's lock when it closes. Segments are still built under the pcb
lock into a `struct tcp_batch` and sent after unlock (N5); the batch's
data copy uses the mbuf cluster directly instead of a 1500-byte stack
buffer (§9.2 LOW). Nothing in the worker or in `tcp_input` holds two
connection locks except listener → child on the passive-open and
accept paths.

### Passive open: SYN cache and SYN cookies (#10)

A SYN to a listener no longer allocates a pcb. The listener owns a
`struct tcp_syncache` of `TCP_SYNCACHE_SIZE` (64) entries, each the
peer's address, the local address, `iss`, `irs`, the peer's MSS, the
path MSS and the arrival time, indexed by a hash of the 4-tuple with
linear probing over eight slots. A SYN fills a free or expired
(`TCP_SYNCACHE_TTL_NS`, 8 s) slot and answers SYN-ACK; a repeated SYN
for an entry answers SYN-ACK again (there is no SYN-ACK retransmit
timer: the client's SYN retransmit drives it). When no slot is free the
listener answers with a *SYN cookie* and keeps nothing: `iss = H(secret,
4-tuple, t) & ~7 | mss_index`, `t` the 8-second slot of the clock, `H`
a 32-bit hash keyed by a boot-time secret, `mss_index` into the table
{536, 1220, 1440, 1460, 4096, 8960, 16384}. The completing ACK
(`ack - 1 == iss`) is checked against the cache first, then against the
cookies of the current and previous slot; only then is a pcb allocated,
already `ESTABLISHED`, inserted in the table and queued for `accept`.
`backlog` (1..`TCP_MAX_BACKLOG` 16) now bounds established children
waiting to be accepted; a completing ACK beyond it is dropped (the
client retransmits the ACK, or its data, and the pcb is created when
the queue drains). A flood of SYNs therefore costs the listener at most
64 × 64 bytes and no memory per spoofed source, and a legitimate client
still connects through a cookie. `SYN_RCVD` remains only for the
simultaneous-open path of `SYN_SENT`. New counters: `syn_cached`,
`syn_cookies_sent`, `syn_cookies_ok`, `syn_bad_ack`.

### RFC 5961: blind in-window attacks

- **RST**: accepted only when `seq == rcv_nxt`; a RST elsewhere inside
  the window is answered with a *challenge ACK* (a pure ACK with the
  current numbers) and dropped. Outside the window, dropped.
- **SYN in a synchronized state**: never a reset any more; a challenge
  ACK, and the segment is dropped.
- **ACK**: `ack` outside `[snd_una − TCP_MAX_WINDOW, snd_max]` is a
  challenge ACK and a drop, not processed.

Challenge ACKs are limited to `TCP_CHALLENGE_PER_SEC` (100) across the
host with a token bucket; the counter `challenge_acks` records both
sent and suppressed. TIME_WAIT applies the same rules and restarts its
2 MSL timer only for a retransmitted FIN (RFC 1122 4.2.2.13), not for
any segment, so a peer can no longer pin a pcb (§9.2).

### Timers: FIN_WAIT_2 and keepalive

A fourth timer, `keep`, serves two purposes:

- **Orphaned FIN_WAIT_2.** When the socket is gone (`sock == NULL`) and
  the connection is in `FIN_WAIT_2`, the pcb is ended silently after
  `TCP_FIN_WAIT2_NS` (60 s). A peer that never sends its FIN cannot
  hold the pcb and its 128 KiB of rings for ever. (With a socket still
  open the state may last as long as the application wants, as on every
  other system.)
- **Keepalive.** In `ESTABLISHED` and `CLOSE_WAIT` the timer fires after
  `TCP_KEEPIDLE_NS` (7200 s) without a received segment; the worker then
  sends a probe (an ACK with `seq = snd_nxt − 1`, no data, RFC 1122
  4.2.3.6) every `TCP_KEEPINTVL_NS` (75 s) up to `TCP_KEEPCNT` (9)
  times, after which the connection ends with `-ETIMEDOUT`. Any
  received acceptable segment records `last_rx_ns` and resets the
  count. Keepalive is always on (there is no `SO_KEEPALIVE` yet); the
  parameters are global and `tcp_set_keepalive(idle_ns, intvl_ns, cnt)`
  lets the self-test shorten them.

Both run on the worker through `WORK_KEEP` (timers never send, N4).

### Out-of-order reassembly

A segment inside the window but beyond `rcv_nxt` is queued on the pcb's
`ooo` list (`struct tcp_ooo_seg { seq, len, mbuf }`, sorted, at most
`TCP_OOO_MAX` 32 entries and never more bytes than the receive window)
instead of being dropped. A new segment that overlaps a queued one is
dropped unless it covers it entirely, in which case it replaces it;
overlap with `rcv_nxt` is trimmed on delivery. After in-order data is
stored, the queue is drained while its head is contiguous with
`rcv_nxt`, and the ACK that follows covers everything delivered. A
duplicate ACK still goes out for every out-of-order arrival so the
sender's fast retransmit works. `out_of_order` keeps counting arrivals;
`ooo_queued` counts what was kept and `ooo_dropped` what the bound
refused. The queue is freed with the pcb and flushed on reset.

### ICMP rate limit

`icmp_send_unreach`, the echo reply and the IPv6 echo reply pass a
global token bucket of `ICMP_RATE_PER_SEC` (100, burst 100).
Suppressed messages count in `icmp_ratelimited`. A UDP port scan or an
echo flood thus produces at most 100 replies a second from this host.
While there, the quoted header in an unreachable copies the *whole*
received IP header (options included) from the saved copy, closing the
uninitialised-bytes leak of §9.2 (`ipv4.c` unknown protocol, `udp.c`
port unreachable).

### Path MTU discovery

Every IPv4 datagram carries DF already. An incoming *Fragmentation
Needed* (type 3, code 4) is now honoured: the next-hop MTU from the
message (or, when zero, the next plateau below the quoted total length
from RFC 1191's table) is recorded in a 16-entry per-destination cache
(`ipv4_pmtu_update`, 10-minute expiry, floor 576) that `ipv4_path_mtu`
consults and `tcp_path_mss` derives the MSS from; but only when the quoted
transport header is TCP, `tcp_pmtu_notify(local, remote, mtu)` finds
the connection, checks that the quoted sequence number lies in
`[snd_una, snd_max)` (RFC 5927: a blind message cannot shrink a
connection it cannot see), lowers `path_mss` and `mss` to `mtu − 40`
(never below 256) and retransmits from `snd_una` at the new size. The
cache is written only after that confirmation: a message the quoted
connection does not vouch for changes nothing, so a blind sender
cannot lower the MSS of future connections to a destination of its
choosing for ten minutes. Quotes of other protocols are ignored (no
consumer of the cache exists for them yet).
`pmtu_updates` counts accepted messages. IPv6 keeps its minimum-MTU
behaviour (1280) for now.

### Ephemeral ports

`pick_ephemeral` starts from a random port on every call and probes
upwards, instead of counting from a random base once at boot (§9.2
LOW).

### Non-blocking I/O and readiness

`struct kobject_io_type` gains two optional operations
(`docs/kernel/object/api.md`):

```c
unsigned (*ready)(struct kobject *obj);              /* COSMO_IO_* bits that would not block now */
int (*set_nonblock)(struct kobject *obj, bool on);   /* -EOPNOTSUPP when the object always completes */
```

`COSMO_IO_READABLE` (1), `COSMO_IO_WRITABLE` (2), `COSMO_IO_HANGUP` (4)
and `COSMO_IO_ERROR` (8) are UAPI. A NULL `ready` means always readable
and writable (files); the console reports readable when the tty holds
a complete line. Sockets: a datagram socket is readable with a queued
datagram, a listener when a child waits, a stream when the receive ring
has data, the peer's FIN arrived (`HANGUP` too), the connection ended
or an error is pending (`ERROR`); writable when the send ring has room
in `ESTABLISHED`/`CLOSE_WAIT`, or when a write would fail at once. Pipe
ends: the reader is readable with bytes or no writer left (`HANGUP`),
the writer writable with `PIPE_BUF` free or no reader left (`ERROR`).

Non-blocking mode is a property of the object (one bit in `struct
socket` and one per pipe end), which every handle to it shares; Linux
attaches it to the open file description and CosmoOS has no such layer
between the handle and the object, so the two are the same here. In
non-blocking mode `accept` returns `-EAGAIN` with no child; `connect`
returns `-EINPROGRESS` once the SYN is out (a second call
`-EALREADY`, and once the handshake is over `-EISCONN` or the recorded
error); `recvfrom`/`read` return `-EAGAIN` with nothing to read; `send`
returns what fits and `-EAGAIN` when nothing does; a pipe read is
`-EAGAIN` while empty with a writer, a pipe write `-EAGAIN` when the
buffer cannot take the write (or its first byte for a write larger
than `PIPE_BUF`).

UAPI: `COSMO_SOCK_NONBLOCK` (0x800) may be ORed into `socket`'s type;
`SYS_ioready` (58: `(int h) -> COSMO_IO_* mask`) reports readiness
without waiting; `SYS_setnonblock` (59: `(int h, int on) -> 0`) sets
the mode. Linux: `SOCK_NONBLOCK` on `socket` and `accept4`,
`pipe2(O_NONBLOCK)`, `fcntl(F_SETFL, O_NONBLOCK)` and `F_GETFL`
reporting it, `EINPROGRESS`/`EALREADY`/`EAGAIN` as above. `poll` itself
remains Linux stage 3 (`docs/compat/linux/design.md`); the readiness
operation is the piece it and async I/O need from every object.

## Forwarding and NAT (`ipv4.c`, `nat.c`; audit unit "reaching beyond the host")

The stack was an endpoint: it delivered what was addressed to it and
dropped the rest. This unit lets a guest on a tap reach past the host --
through the host's own interface, its replies NAT'd back -- in three pieces
(`docs/audit/next-subsystem-nat.md`).

**Connected-route selection.** `ipv4_route` tried owned/loopback, then the
single default interface. `netif_connected(dst)` now sits between them: the
up, non-loopback interface on whose subnet `dst` falls, chosen by *longest
prefix* so overlapping masks resolve by specificity, not registration
order. So a reply for the guest routes to the tap (the guest's `/24`), not
the default NIC. Owned and loopback destinations, and anything with no
connected route (which still falls to the default), are unchanged.

**IP forwarding.** Where `ipv4_input` dropped a unicast datagram not for the
host (`rx_not_for_us`), it now **forwards** it -- but only when the packet
arrived on an interface marked `NETIF_FORWARD`. The gate is
*per-ingress-interface*: the flag is the tap's, never the real NIC's, so a
packet arriving on the NIC for some other host is still dropped and the host
is no router for its real link. A forwarded datagram is routed (connected,
then default), its TTL decremented (an ICMP time-exceeded at zero, RFC
1812), and re-emitted with `output_on`; one with no route (ICMP
net-unreachable) or that would hairpin back out its arrival interface is
dropped. The datagram takes the same validated `ipv4_input` path -- header,
checksum, martian checks -- before forwarding, with no shortcut, and a
strict reverse-path check drops any datagram whose source is not on the
ingress interface's own subnet, so a guest cannot forge a source (an
uplink-subnet address, say, which would otherwise make masquerade skip it
and be emitted unchanged). On a masquerading tap — a point-to-point link to
one owner — this is tightened further to the tap's single guest address (see
"Many guests: a tap per open").

**Masquerade NAT (`nat.c`).** A forwarded flow leaving an interface whose
subnet does not hold its source -- a `10.0.3.x` guest going out the uplink
-- has its source rewritten to the egress address and its transport
identifier (TCP/UDP source port, ICMP echo id) rewritten to a value a
bounded conntrack table lends; the reply, arriving for that value, is
rewritten back to the guest and forwarded. `nat_out` runs in the forwarding
path (gated on the ingress `NETIF_MASQUERADE`); `nat_in` runs in
`ipv4_input` for datagrams addressed to us, before host delivery, and
catches both a plain reply and an ICMP error quoting a NAT'd packet
(translated so path-MTU and unreachables reach the guest). The transport
checksum is fixed up incrementally (RFC 1624, `csum_patch16`/`csum_patch32`,
in this codebase's big-endian-word convention); the IP checksum is
recomputed when the header is rebuilt downstream. Transport headers are
read and written by byte offset -- they are `__packed`, so a pointer to a
member could be unaligned. The lent identifiers come from a reserved range (`NAT_PORT_MIN..MAX`) kept
below `NET_EPHEMERAL_LO`, so a host's own outbound flow -- which sources
from an ephemeral port -- never collides with a lent one; and `nat_alloc`
additionally skips any port a host UDP/TCP socket has bound
(`udp_port_in_use`/`tcp_port_in_use`), so a reply for a host service is
never redirected to the guest. An inbound ICMP message is checksum-validated
before translation (the error path recomputes the checksum wholesale, which
would otherwise launder a corrupt message the normal path would drop). The
table is bounded (`NAT_TABLE_SIZE`); entries expire (a short idle timeout,
longer once a TCP flow is established, via `nat_age`, which the network
worker calls from its periodic ARP/ND aging), and a full table drops new
flows. Because forwarding is enabled
only on the one guest's tap, the table's flows are that guest's, so a guest
that opens endless flows starves only itself; once the table is shared among
several guests, a per-guest quota bounds each one's footprint ("Many guests: a
tap per open", below). Only IPv4
UDP, TCP and ICMP echo are masqueraded; a flow that cannot be (an
unsupported protocol, a truncated header, a full table) is dropped rather
than forwarded with the private source exposed.

A guest's tap turns `NETIF_FORWARD` and `NETIF_MASQUERADE` on when its owner
opens `/dev/net/tap` -- a VM attaching is the opt-in (one persistent `tap0`
then; a tap per open now, "Many guests: a tap per open" below). No new system
call and no writable control surface: the flags are internal, set by the tap
setup.
A stock Linux guest with the tap as its gateway reaching the host's network
(and the internet, where the host has it) is the `QEMU_MEM=2G`
reproduction. The filtering firewall over this forwarding path is done ("A
forwarding firewall", below); IPv6 NAT is a later unit; inbound
port-forwarding (DNAT), once next, is done -- the next section.

## Inbound port forwarding: DNAT (`nat.c`; audit unit "reaching the guest from outside")

Masquerade let the guest reach out; DNAT lets the outside reach a service the
guest runs (`docs/audit/next-subsystem-dnat.md`). A **static port-forward
table** (`nat_portforward_config` from `fw_cfg` `opt/cosmo/portforward`, a
list of `proto:hostport:guestaddr:guestport`, read on VM attach) maps a host
port to a guest address and port. The bind is **wildcard on the host
address**: a rule matches a connection to any of the host's own addresses on
that port, so the match is the existing "addressed to one of our addresses"
test plus the port. This unit had no writable control surface; the runtime
API came two units later ("A runtime network control channel", below), and
the firewall's rules ride the same channel.

**Inbound** (`nat_in` → `nat_in_dnat`): a TCP/UDP packet addressed to the host
that is *not* a masquerade reply and whose `(proto, dport)` matches a rule (or
an established DNAT flow) records a conntrack entry, has its destination
rewritten to the guest's address and port (checksum fixed incrementally), and
is forwarded out the tap with `ipv4_output` — authorized by the *rule*, not by
the ingress interface's `NETIF_FORWARD`, so a connection arriving on the
uplink crosses to the guest though the uplink is not a forwarding interface.

**The reply** (`nat_out`): the guest answers from `guest:Q`; that forwarded
packet has its source rewritten back to what the client dialed (`host:P`),
**with precedence over masquerade** — a flow that is half of a port-forward is
never also masqueraded. Because the client sits on the egress subnet in the
two-tap case (so the masquerade `on_egress` gate would otherwise skip
`nat_out`), `ipv4_forward` now calls `nat_out` for every natable forwarded
packet and `nat_out` decides DNAT-reply vs masquerade vs nothing.

DNAT and masquerade entries share the one bounded, expiring `nat.c` table,
told apart by a kind flag (the masquerade lookups filter to their kind), so
inbound state a remote client can create is bounded exactly as outbound state
the guest can. A stock Linux guest running a service reached from the host
through a port-forward is the `QEMU_MEM=2G` reproduction. The writable
control surface and the filtering firewall are done ("A runtime network
control channel" and "A forwarding firewall", below); hairpin/NAT-reflection
and IPv6 DNAT are later units.

## Many guests: a tap per open (`tap.c`, `tapsvc.c`, `nat.c`; audit unit "from one guest to many")

The stack served one guest — a persistent `tap0`, a singleton `tapsvc`, one
NAT table — because the character device had no per-open lifecycle. Now every
open of `/dev/net/tap` is a guest (`docs/audit/next-subsystem-multiguest.md`).

**A tap per open.** `/dev/net/tap`'s `open` (the VFS chrdev lifecycle, below)
takes a free slot from a pool of `TAP_MAX_GUESTS` (8) and creates `tap<k>` on
`10.0.(3+k).0/24` — host `.1`, guest `.15`, the convention `tap0` set, which
is now simply the first — forwarding and masquerade on, with its own `tapsvc`;
`read`/`write` act on that file's tap; the ninth open is `-ENOSPC`. The last
close (`release`) stops the service, purges the guest's NAT state, destroys
the tap and frees the slot — in that order, so the service's threads and DHCP
filter are gone before the tap they point at, and no flow survives for a
reused subnet. A tap exists exactly while an owner holds the channel;
`vmctl`, which opens once per run, is unchanged, and two runs are two guests.

**`tapsvc` per tap.** The singleton is now an instance (`struct tapsvc`)
allocated per tap: its own DHCP binding (a guest slot on its own subnet) and
its own DNS proxy, whose socket is bound to *that tap's gateway* — a proxy on
`0.0.0.0:53` would answer DNS on the host's real interface. The periodic
`nat`/DNS age sweeps every live instance.

**NAT for many.** Four changes keep a shared table fair and honest with
several guests. A **per-guest quota** (`NAT_QUOTA_PER_GUEST = NAT_TABLE_SIZE /
NAT_GUESTS`) caps a guest's whole footprint — the guest is always the
`orig_ip` side, for a masquerade flow it dialled out and a DNAT flow dialled
in to its port-forward alike, so one count (`nat_guest_count`) bounds both;
neither an outbound flood nor an inbound flood against one guest's forwards
starves a peer. **Masquerade is skipped when the egress interface itself
forwards** (a flow between two taps is guest-to-guest), so guests reach each
other with real addresses — and because that leaves the source un-rewritten,
the forwarding anti-spoof is tightened: a masquerading tap is a
point-to-point link to a single owner at `<subnet>.15`, so `ipv4_forward`
requires exactly that source (not merely one on the subnet), or a guest could
forge a same-subnet identity toward a peer. A packet that matches a
port-forward rule but cannot be forwarded (the guest is over quota, or the
table is full) is **dropped, not delivered locally** — it is meant for the
guest, not for a host service that happens to bind the same port. And
**`nat_guest_purge(guest_ip)`** removes every port-forward rule targeting a
guest and every conntrack entry on its guest side — and nothing else — the
guest-scoped teardown a departing tap's `release` calls before its subnet
returns to the pool.

Each guest thus has its own channel (it sees only its own frames), its own
lease and subnet, its own NAT share, and its own port-forwards; guests reach
each other over routed IP as adjacent-subnet machines do — connectivity, not
visibility — *when policy allows it*: the forwarding firewall ("A forwarding
firewall", below) now drops inter-guest traffic by default and a rule opens
it, so this unit's routing is the mechanism and that unit's policy decides
its use. An L2 bridge sharing one subnet and per-guest limits beyond the NAT
quota are later units.

## A runtime network control channel (`tap.c`, `nat.c`; audit unit "configuring the guest's network at runtime")

Everything above was fixed at boot from read-only `fw_cfg`; this adds the one
writable control surface the arc deferred, so an operator changes the guest's
networking on a running machine (`docs/audit/next-subsystem-netctl.md`).

**`/dev/net/tapctl`**, a privileged character device (mode `0600`, beside
`/dev/net/tap`), is the channel — separate from the frame channel, so
configuration and data never share a descriptor. A `write` submits one
fixed-layout, versioned command (`struct cosmo_netctl` in
`uapi/cosmo/netctl.h`: a version, an opcode, and the fields), applied whole or
refused — a short write, an unknown version (`-ENOTSUP`), an unknown opcode or
an out-of-range field (`-EINVAL`) change nothing. The first opcodes are
`FORWARD_ADD` and `FORWARD_DEL`. A `read` returns the live rules as a
versioned snapshot (a `struct cosmo_netctl_list` header — version and count —
then that many `struct cosmo_netctl_rule`), the whole snapshot in one read or
`-EMSGSIZE`, so an operator never sees a half-updated table.

The commands reach the existing port-forward table. `nat_pf_add` is **tightened**
here: a rule's target must be on the **guest tap's own subnet** — a connected
interface that forwards (`NETIF_FORWARD`) — not merely any connected subnet,
which would also match the uplink and let a rule relay a host port to another
machine; and `(proto, host_port)` is a **unique key** (a duplicate `ADD` is
`-EEXIST`, never shadowed, so a listed rule is always the one that gets
traffic). `nat_pf_del` removes the rule holding a `(proto, host_port)` —
whatever its origin, since the privileged operator owns the one table — and
**reaps the DNAT conntrack entries it created**, so a removed forward stops an
in-flight flow at once, not after it idles out. `nat_pf_list` snapshots the
rules for the `read`.

The writable surface is contained by the same reasoning the arc used for the
read-only ones: the device is privileged (`0600`, the owner only); every
command is a versioned, range-checked struct refused whole on any doubt; and
the operations touch only the guest's port-forward table with the
guest-tap-subnet target check, never the host's own interface, routes, or
another process. No new system call. `vmctl port-forward add|del|list` drives
it; exposing and hiding a guest service on a running Linux guest is the
`QEMU_MEM=2G` reproduction. The channel is designed to carry the tap's other
settings (forwarding/masquerade toggles, the resolver) in later units.

## A forwarding firewall (`fw.c`; audit unit "a stateful packet-filter firewall for the guest taps")

The multi-guest unit routed guests to each other — "connectivity, not
visibility" — and deferred *a policy forbidding inter-guest traffic*. This is
that policy (`docs/audit/next-subsystem-firewall.md`): a stateful packet
filter over the guest taps, deciding who may reach whom.

**One chain, one call site.** A single FORWARD chain, evaluated by
`fw_forward_verdict` in `ipv4_forward` **after** the anti-spoof and routing
steps (so the direction is known from the egress: `TO_GUEST` when `out` is
another forwarding tap, `TO_UPLINK` otherwise) and **before** `nat_out` (so
rules see the datagram as the guest sent it, never a translated source). A
`FW_DROP` frees the datagram and counts `fwd_filtered`. Each guest — attached
by address when its tap opens — owns an ordered rule list evaluated
first-match, and a default verdict per direction for a datagram no rule
matches. The **defaults** are the deferred policy made concrete: `TO_GUEST`
**DROP**, `TO_UPLINK` **ACCEPT** — a guest cannot reach its neighbour unless
a rule allows it, and its path to the world is as it was. A rule matches on
direction (or any), protocol (TCP/UDP/ICMP or any), destination address with
a prefix (or any), and a transport selector: for TCP/UDP a destination port
(or any; a port constraint applies to TCP/UDP only and never matches a
datagram without ports), for ICMP the ICMP type (or `FW_ICMP_TYPE_ANY`) —
see "The ICMP selector is a type", below.

**Stateful where it must be — and only there.** Reading the reply paths
exactly: a masqueraded guest→uplink reply is addressed to the host, so
`nat_in` un-translates it and delivers it via `ipv4_output` — it **never
re-enters `ipv4_forward`** and needs no filter state, because it exists only
on the strength of a NAT conntrack entry that was created when this filter
accepted the outbound flow. Conntrack is that direction's state. A
guest→guest flow is un-NAT'd, so *both* halves traverse `ipv4_forward`; that
is the one direction with filter state of its own: an accepted NEW flow is
recorded in a bounded flow table (`FW_FLOW_MAX`, a per-guest share
`FW_FLOW_QUOTA_PER_GUEST` so a flood starves only its owner — and a NEW flow
that cannot be recorded is refused, since state it cannot keep would strand
the reply), and the reverse tuple is **ESTABLISHED** and accepted without a
reverse rule — with one exception that matters because a guest injects
arbitrary flags: a TCP segment with SYN set and ACK clear opens a connection
and is never a reply, so a **reverse-direction bare SYN** on an accepted
flow's ports is the *other* guest starting a flow and takes that guest's
rules and default, not the shortcut. TCP is otherwise coarse (NEW until an
ACK without SYN, then the longer timeout, as conntrack's `tcp_est`); the
timeouts are conntrack's and `fw_age` runs on the same periodic tick as
`nat_age`. **ICMP is stateful for echo
only**, keyed on the echo identifier: a type-8 request records the id, and
only a type-0 reply carrying that id is its reply — a reverse echo *request*
is a new flow (and meets the default), and a reply with another id matches
nothing. Inbound DNAT is authorized by its port-forward rule and delivered by
`nat_in`, not the FORWARD chain, so it is not re-gated here.

**Identity without an id.** The control channel's `write` returns only a
byte count, so a kernel-assigned rule id could not reach the caller. A rule's
identity is therefore its **whole match tuple**, exactly as a port-forward's
is `(proto, host_port)`: `FILTER_DEL` names the tuple `FILTER_ADD` installed,
a duplicate tuple is `-EEXIST`, and ordering is explicit — `FILTER_ADD`
carries an `at_index` (clamped to append) and the listing reports each rule's
current index as a display ordinal, not an identity to round-trip.

**Bound by address, coherent with teardown.** `/dev/net/tapctl` has no
per-open guest state and `vmctl` closes its handle after each command, so a
rule names its guest **by address in the payload**, as a forward names its
target, and survives the handle closing. Attachment is the firewall's own:
`tap_chr_open` calls `fw_guest_attach` last (after every step that can fail),
`tap_chr_release` calls `fw_guest_purge` beside `nat_guest_purge` before the
subnet returns to the pool, and `fw_rule_add`/`fw_policy_set` require the
guest attached — all under one `g_fw_lock`. So an add and a teardown are
strictly ordered: the purge ran first and the add is refused (`-ENOENT`), or
the add ran first and the purge removes it; a reused address inherits no
rule and no flow (purge drops every flow naming the address on either side).
This is the netctl unit's TOCTOU lesson applied by construction, rather than
by re-checking `netif_connected` (which stays "live" until `tap_destroy`, after
the purge, and would leave a window). Lock order: `g_fw_lock` → `g_nat_lock`;
the verdict takes no other lock.

**The control plane** is `/dev/net/tapctl`, now at ABI version 3
(`uapi/cosmo/netctl.h`; version 2 added the filter, version 3 the INPUT
chain's `DIR_TO_HOST`, the per-guest `policy_to_host`, and the ICMP-type
meaning of an ICMP rule's selector): `FILTER_ADD`/`FILTER_DEL`/`FILTER_POLICY` in a
`struct cosmo_netctl_filter` (each op exactly its own struct; the dispatcher
reads the common `(version, op)` first), and the read snapshot gains a filter
section after the port-forward list — a header, the attached guests' default
policies, then every rule in evaluation order with its guest and index. A
reader that stops after the port-forward rules is unaffected. `vmctl filter
add|del|policy|list` drives it. No new system call.

**The INPUT chain: what a guest may ask of the host** (audit unit "the INPUT
chain", `docs/audit/next-subsystem-input-chain.md`). The FORWARD chain
decided which *other machines* a guest may reach; this decides which of the
**host's own services** it may reach. Before it, `ipv4_input` handed
everything `nat_in` declined straight to `icmp_input`/`udp_input`/`tcp_input`
— a guest could reach any host listener through its gateway (or the host's
uplink address), and could forge its source doing so, since the strict `.15`
check lived only in `ipv4_forward`. Now `fw_input_verdict` runs in
`ipv4_input` for a datagram a **guest tap** (`NETIF_MASQUERADE` ingress)
delivers to the host — unicast to one of our addresses *after* `nat_in` has
declined it (a NAT'd reply or DNAT is not host-bound), or a broadcast — and
before the transport demux; a drop frees the datagram and counts
`in_filtered`. It is the same engine consulted from a second place:
**`TO_HOST` is a third direction** (a rule's `direction`, a third default
slot), not a second rule list — and a rule's **`ANY` direction keeps its
version-2 meaning, either *forwarding* direction, never `TO_HOST`**: a
wildcard written to permit forwarding must not, by the INPUT chain's
arrival, silently open a host service, so host traffic needs an explicit
`TO_HOST` rule. The verdict does two things in order: the
**anti-spoof** (the source must be the tap's guest, `<subnet>.15`, else
`in_spoofed` and drop — the forwarding rule made on both paths a tap datagram
can take), then the guest's `TO_HOST` rules first-match, else its `TO_HOST`
default. **No state on this chain**: the host's reply leaves by
`ipv4_output` and passes no filter, and a guest's later segments match the
same rule by destination port, so the verdict is per datagram (a bare ACK to
an unruled port is dropped). The **default is DROP**, and so that a stock
guest keeps working `fw_guest_attach` (now given the tap's gateway too)
**seeds two rules** — `TO_HOST udp gateway/32 :53 ACCEPT` (the DNS proxy) and
`TO_HOST icmp gateway/32 type 8 ACCEPT` (echo *request*) — as ordinary
listed, ordered, deletable rules rather than hard-coded holes; `fw_flush`
re-seeds them; DHCP needs none (frame-level). A guest never attached fails
closed. Loopback and the uplink are not guests and are not consulted; the
uplink has its own chain, the host chain below.

**The ICMP selector is a type.** A default-deny chain guarding host handlers
cannot admit "all ICMP": `icmp_input` dispatches Need-Fragmentation (type
3/4) into `ipv4_pmtu_update` and Echo Reply (type 0) into the echo hook. So
for `proto == ICMP` a rule's `dst_port` field **is the ICMP type** (0..255),
with `FW_ICMP_TYPE_ANY` (`0xffff`) as the wildcard — the field is the
transport selector, a port for TCP/UDP and a type for ICMP, no new field or
size — and the echo seed admits type 8 alone, leaving a guest's echo reply
and need-frag to the default. (Version 2 required `0` there and meant "any";
version 3 refuses a v2 writer by version rather than reinterpret it.) The
FORWARD chain's ICMP flow *state* (echo by id) is unchanged; this is the
rule *match*, and lets FORWARD rules say "echo-request only" too.

**The host chain: what the world may ask of the host** (audit unit "the host
chain", `docs/audit/next-subsystem-host-input.md`). FORWARD decided which
machines a guest may reach and INPUT which of the host's services a guest may
reach; this decides which of the host's services the **world** — anything
arriving on a real, non-guest link (`nif->flags` has neither
`NETIF_MASQUERADE` nor `NETIF_LOOPBACK`; today the NIC) — may reach. It is
the same engine consulted from a third place, with three additions.
**A host-scoped policy object**: `FW_HOST_GUEST_IP` (`0`) names the host —
one ordered rule list and one default, permanent (never attached or purged;
`fw_flush` resets it), kept *outside* the guest table so no datagram's source
can ever name it (`guest_find` searches guests; only the control path's
`policy_find` maps `0` to the host). **A fourth direction**, `FROM_UPLINK`,
which the host object alone may hold (`-EINVAL` for a guest; `-EINVAL` for
any other direction on the host); `ANY` keeps its forwarding-only meaning and
never matches it. **A source prefix** on `struct fw_rule` (`src_ip/src_prefix`,
`0/0` = any), the field a world-facing rule cannot do without and permitted
*only* on a host rule — a guest's rule must say `0/0`, its source being the
guest, so tuples stay canonical. `fw_host_verdict` runs in `ipv4_input` for
the uplink's datagram to the host at the same point as INPUT's verdict —
**after `nat_in` has declined it** (a masqueraded reply or a DNAT is not
host-bound and is never re-gated: with a port-forward to a guest and the host
default DROP, the world's SYN to `host:8080` is still DNAT'd — the proof the
INPUT unit could not run), before the transport demux — and is stateless:
first-match over the host's rules, else the host default, **ACCEPT** (the
host runs services meant to be reached; the harness's echo listeners, DNAT'd
connections' host-side handling, ICMP echo and every reply to the host's own
UDP sockets all arrive here; the operator hardens by rule or flips the
default). Counted `hin_accept_rule/hin_drop_rule/hin_accept_default/
hin_drop_default`.

**The off-link invariant.** One thing closes with no rule: a datagram
arriving on a link is for an address *on that link*. Before `nat_in` and
before either chain, for **every non-loopback ingress** (uplink and guest
taps alike), a locally-owned unicast destination must be the ingress
interface's own address (`iph->dst == nif->ip4.addr`) or a broadcast; else it
is dropped, `rx_offlink`. `netif_owns_ipv4` answered "ours" for *any*
interface's address and the martian check looks only at the source, so until
now an uplink datagram to a guest's gateway `:53` was delivered into that
tap's DNS proxy (an open resolver per guest, reachable from the real network)
and one to `127.0.0.1` reached **loopback-bound services** — the binding a
service uses to mean "local callers only"; a guest's datagram to `127/8` or to
the uplink's address was held back only by INPUT's default DROP, which an
`any`-destination rule would have reopened. A fact of the topology, not a
policy: no rule reopens it. Loopback ingress is the host talking to itself
and is exempt; masqueraded replies and DNAT'd connections are addressed to
the uplink's own address and are on link. `net-input`'s "guest → the host's
uplink address" case moved from INPUT's default drop to this counter — the
one existing-test adjustment and the one unconditional behaviour change.

**A DROP is quiet: the transport decides, and answers nothing.** The chain
does not model TCP state — three drafts of a firewall-side "established" test
(flags; any non-listening PCB; an eligible-state list) each left `tcp_input`
a way to *answer* a probe, and the last did not even match this stack
(passive half-opens live in the listener's SYN cache, the child is born
`ESTABLISHED`, `SYN_RCVD` is simultaneous open only). Instead a DROP verdict
on a TCP or UDP datagram does not free it at the IP layer: it is marked
**`M_FW_QUIET`** (an mbuf flag beside `M_BCAST` — `tcp_input`/`udp_input`
take `(nif, m, ip4, ip6)`, so the policy rides on the packet with no
signature change; `hin_quiet`) and delivered, and the transport, which owns
acceptability, admits it only into an *existing* connection. Anything else —
ICMP included — is freed (`hin_filtered`). In **TCP** the gate is structural,
not a list of sites: every response `tcp.c` builds — listener SYN-ACKs,
resets, challenge ACKs, the ACK to an out-of-window segment — goes through
`batch_push` into the per-call `struct tcp_batch`, and `batch_send` is the
sole function that transmits (the only `ipv4_output`/`ipv6_output` calls in
the file), so the batch carries a `quiet` bit, set from the mbuf when
`tcp_input` begins and **cleared only where the segment is accepted**;
`batch_send` frees a still-quiet batch instead of transmitting it, whichever
flush it reaches (the early no-pcb and listener-rejection flushes, or the
final one), and counts it `quiet_dropped`. The acceptance points, each a path
that queues output or mutates state on its own, were fixed by walking every
`goto out` in `tcp_input` and classifying it: **(1)** the point after the
*last* rejection check in the synchronized states — after the window test,
the reset-position, in-window-SYN, missing-ACK and RFC 5961 §5 ACK-range
checks, and the `SYN_RCVD` ACK check — and *before* the accepted-segment
processing, so an accepted segment's own output is never lost, whether it
advances `snd_una`, is a duplicate ACK whose third arrival builds a fast
retransmission, a pure window update that re-enables a blocked send, data
with an unchanged ACK, or a FIN; **(2)** the active-open `SYN_SENT`
completion — the host's own outbound connection completes under any rule;
**(3)** a valid reset (`seq == rcv_nxt`, or the refused active open), which
tears the connection down and emits nothing; **(4)** the SYN-cache completion
of a SYN admitted earlier (the ACK that creates the child); and **(5)** a
retransmitted FIN in `TIME_WAIT` that names exactly `rcv_nxt` — an
exact-position match on an existing connection, found in the walk (the
design named four), whose ACK the peer needs to finish its own close.
Side effects follow the same rule, *consumed at emission, not at decision*:
`challenge_ack` returns before consulting `challenge_allowed()` when its
batch is quiet, so a burst of rejected probes cannot burn the host-wide
RFC 5961 budget and starve a legitimate connection's challenge; `last_rx_ns`
and `keep_probes` are updated only for an accepted segment, so a rejected one
cannot refresh the keepalive clock; no SYN-cache entry is allocated for a new
SYN under the flag. **UDP** under the flag delivers only to a socket
**connected** to the sender (`lookup(..., connected_only)`), frees anything
else silently and sends no ICMP port-unreachable (`quiet_dropped`). So a
DROP means silence — no SYN-ACK, RST, challenge ACK, window ACK or
port-unreachable confirms the host is there — while the host's own outbound
connections, its accepted inbound ones and its connected UDP flows keep
working under any rule set. Rules therefore gate *new* connections and
unsolicited datagrams; a connection that exists when a DROP rule is added
persists until it closes (as FORWARD's flow state does), and an operator
who wants it cut closes the socket. What quiet delivery cannot recognise —
a reply to an *unconnected* UDP socket, and every ICMP reply — takes the
rules, which the default ACCEPT admits; a broad `udp any any` DROP would drop
the host's own unconnected replies (name listener ports or a source prefix);
reply state for those is a later unit.

**ABI version 4.** `DIR_FROM_UPLINK` (4); `src_addr`/`src_prefix` in the
filter command and rule records (`struct cosmo_netctl_filter` 20→28 bytes,
`struct cosmo_netctl_filter_rule` 16→24 — a version-3 writer is refused by
size, never misread); `policy_from_uplink` in the per-guest record (its
formerly reserved byte; meaningful in the host's record, which the snapshot
lists first under `guest_addr 0`, `COSMO_NETCTL_HOST_ADDR`); `SNAPSHOT_MAX`
recomputed for `MAX_GUESTS + 1` policy objects and static-asserted with the
record sizes. `vmctl filter add|del host world PROTO SRC[/PREFIX]|any
DST[/PREFIX]|any PORT VERDICT [INDEX]`, `policy host world accept|drop`;
`list` prints the host record and its rules with their source, and refuses a
snapshot of another version. No new opcode, no new syscall.

Named and deferred: reply state for unconnected UDP and for ICMP;
per-interface host chains (all real links share `FROM_UPLINK`); an OUTPUT
chain for the host's own egress (and, with it, filtering the host's replies
to guests); rate-limit and logging targets; IPv6 filtering; full TCP state
tracking; DHCP-client protection, moot until the host has a DHCP client.

## Autoconfiguring the guest: DHCP and a DNS proxy (`tapsvc.c`; audit unit "autoconfiguring the guest")

Forwarding and NAT let a guest reach the world, but only after it was
configured by hand. `tapsvc.c` is the tap's autoconfiguration service,
started when the VM attaches (`tap_dev_activate`, alongside forward and
masquerade). It has two halves that reach the guest by different paths
(`docs/audit/next-subsystem-dhcp-dns.md`).

**DHCP, at the frame level (§2).** The guest has no address yet and its
DHCP replies are the limited broadcast; a routed socket cannot carry that to
the tap, since `ipv4_route` sends `255.255.255.255` to `netif_default` and a
tap is `NETIF_NODEFAULT`. So the DHCP server rides a **tap input filter**
(`tap_set_input_filter`): the tap hands it each frame the guest injects
before the stack sees it, and it claims the UDP-to-port-67 ones. It answers
DISCOVER/REQUEST for the tap's single guest slot (`<subnet>.15`) with the
tap's own address as router and DNS (`<subnet>.1`) and a lease; a REQUEST for
any other address is NAK'd, DECLINE/RELEASE free the one binding (keyed by
`chaddr`), a second client is offered nothing. The reply is built as a frame
and sent out the tap with `ether_output`, its destination following the
client's broadcast flag at both layers (RFC 2131 §4.1): flag set (or a NAK)
→ the limited broadcast (IP `255.255.255.255`, Ethernet `ff:ff:ff:ff:ff:ff`);
flag clear → IP `yiaddr` with a link-unicast to `chaddr`. Never IP-routed.

**DNS, an ID-rewriting UDP relay (§3).** Once configured, the guest's query
is a unicast to the gateway `:53` (an address the host owns) and the answer
a unicast back to the guest (which `ipv4_route` sends out the tap by the
connected-subnet route), so the proxy is an ordinary in-kernel `ksock`
service: a socket on `<gateway>:53` and two threads over a bounded, expiring
pending table. A query is recorded as (guest address, guest port, the
guest's 16-bit id); the proxy allocates an id unique among outstanding
entries, rewrites the query's id, and forwards it from one upstream socket to
the `fw_cfg` upstream (`opt/cosmo/resolver`). The answer's id is the key: the
proxy restores the guest's id and relays the answer back unchanged apart from
that id, so two queries sharing an id stay unambiguous. The query leaves as
the host's own traffic (no NAT), and the proxy never parses names, so any
record type passes. No upstream configured → the proxy answers `SERVFAIL`.
The table is bounded (`DNS_PENDING_MAX`) and drops when full; entries expire
(`tapsvc_dns_age`, called from the periodic ARP/ND aging). UDP only, ≤512
bytes; TCP DNS and EDNS0 are later.

No new system call and no writable control surface: the DHCP parameters are
the tap's own configuration and the upstream resolver is a read-only `fw_cfg`
value. A stock Linux guest with its DHCP client on and the tap as its only
network autoconfigures `eth0` and resolves a name — the `QEMU_MEM=2G`
reproduction. A general DHCP server (pools, many clients), a caching or
recursive resolver, DHCPv6/RA, DNS-over-TCP and DNSSEC are later units.

## Receive scaling and offloads (post-audit unit 11)

The audit's plan (`docs/audit/2026-09-post-roadmap-audit.md` §19, "After
these") names multi-queue networking and zero-copy readiness as the unit
after the ten milestones, with the specification's rule (Prompt #2 §21):
*do not introduce complexity unless benchmarks demonstrate benefit*. This
section records what is done, what is measured, and what is deliberately
not done.

### What the audit found (§9.4–9.5)

Nothing is per CPU: one receive queue (`g_rxq`), one worker thread
(`netrx`), one work list; virtio-net has one RX and one TX queue with
its vectors on CPU 0; every received packet is copied three times to the
user and every sent byte four times; `m_pullup` leaves no headroom and
`NET_HEADROOM` (64) is too small for `vnet(12) + eth(14) + IPv6(40) +
TCP(20..60)`, so IPv6 transmit chains start with an extra buffer; no
checksum offload at all (and, as it turned out, none to be had from
QEMU's user-mode backend: it has no `vnet_hdr`, so the device clears
`CSUM` and `GUEST_CSUM`; the loopback is where offload pays here).

### Per-CPU receive queues and workers (RPS)

```text
driver (any CPU, IRQ) ──netif_rx──▶ steer(flow hash) ──▶ rxq[cpu k] ──▶ netrx/k (pinned to CPU k)
loopback (caller)     ──netif_rx──▶ steer(flow hash) ──▶ rxq[cpu j] ──▶ netrx/j
timer (IRQ)           ──net_work_queue──▶ work[this cpu] ──▶ netrx/this cpu
```

- One `struct net_cpu { struct mbufq rxq; struct list_node work;
  spinlock_t work_lock; struct waitqueue wq; struct thread *worker;
  bool ready; }` per online CPU; the worker `netrx/N` is created with
  `thread_create_on(..., CPUMASK_OF(N))` at the same priority as today's
  `netrx`. Every protocol input still runs in thread context with
  interrupts on, one packet at a time per worker.
- **Steering.** `netif_rx` computes a flow hash over the innermost
  addresses and ports it can find without pulling the packet up
  (`net_flow_hash`: Ethernet type → IPv4/IPv6 header → TCP/UDP ports;
  a packet it cannot classify hashes to 0) and picks `cpu = hash %
  cpu_count()`. Packets of one flow therefore always land on one queue
  and are processed in order, whatever CPU the driver or the loopback
  caller ran on; two flows may run in parallel. A driver whose receive
  queue is bound to a CPU (a multi-queue NIC with RSS) calls
  `netif_rx_on(nif, m, cpu)` and skips the hash: the device already
  steered.
- **Work.** `net_work_queue` appends to the calling CPU's list and wakes
  that CPU's worker; a work item is on at most one list: `queued` is
  claimed with an atomic exchange *before* any list lock is taken, since
  two CPUs queueing the same item (two timers of one pcb firing on
  different CPUs) hold different locks, and the worker clears it after
  unlinking. The
  TCP timers' `pcb_work` may therefore run on a different CPU than the
  connection's input; the per-pcb lock (milestone 8) already serialises
  the two, and `pcb_work` never assumed the input thread's context.
- **Barrier.** `netif_unregister` purges every CPU's queue and posts a
  barrier work item to every running worker, waiting for all: after that
  no `input_one` of the interface is running anywhere. The barrier items
  are static and unregister runs under a mutex, so the guarantee never
  depends on an allocation. The lifetime rules
  of `docs/kernel/quiesce/` are unchanged (`netif_rx` and `netif_transmit`
  are read-side sections).
- **What stays global.** The protocol tables and their spinlocks (TCP
  hash and table lock, UDP list, ARP/ND tables, the SYN cache under its
  listener, the ICMP rate limiter, the path MTU cache): all were made
  IRQ-safe and lock-protected in milestone 8, none assumed one input
  thread. `ipv4`'s datagram id is already atomic. Counters are atomic
  adds as before; the per-CPU queues add `struct net_cpu_stats {
  rx_queued, rx_dropped, rx_steered_local, work_runs }` per CPU for
  `netif_dump`.
- **Not done, and why.** No NAPI-style polling or interrupt moderation
  (QEMU's virtio-net delivers one interrupt per completion batch already
  and the boot tests measure nothing that would move); no busy polling;
  no per-CPU protocol tables (the locks are uncontended at the tested
  scale); no XPS beyond the driver's one transmit queue.

### The next mbuf: headroom, flow id, checksum flags

Prompt #2 §20 asks whether the mbuf can carry jumbo frames,
scatter/gather, checksum offload, TSO/LRO, RSS, multiqueue, zero copy
and DMA recycling. Scatter/gather, DMA mapping per buffer (`pkt.dma`)
and shared clusters (`m_ref`) already exist. This unit adds what the
measured paths need and leaves the rest recorded:

- `NET_HEADROOM` becomes 128: every transmit chain on either IP version
  fits its link, network and transport headers in the first cluster.
  `m_pullup` places the pulled-up bytes `NET_HEADROOM` in as well, so a
  later prepend (a reply built on a received packet: ICMP, TCP RST/ACK)
  does not allocate.
- `pkt.flow_hash` (32 bits) is computed once by `netif_rx` and kept for
  the layers above (a future RSS-aware socket table, XPS on a multi-queue
  driver).
- `pkt.csum_flags` gets a defined meaning: on receive `M_CSUM_OK` (the
  interface vouches for the transport checksum: TCP and UDP skip
  `m_cksum_partial`); on transmit `NET_CSUM_TCP` set by TCP's segment
  builder — which does not know the interface yet — with
  `pkt.csum_start` (the transport header's offset from the packet's
  first byte; IP and Ethernet add their header lengths as they prepend)
  and `pkt.csum_offset` (the checksum field's offset within the
  transport header), the checksum field holding the folded,
  not-inverted pseudo-header sum: exactly the `virtio_net_hdr` contract.
  `netif_transmit` is the one place that decides: an interface with
  `NETIF_CAP_TXCSUM` gets the partial form, any other has the sum
  finished in software right there (`m_csum_complete`). UDP keeps its
  software checksum (its zero-means-none rule needs the final value;
  the datagrams in the tests are small). Capabilities are a new
  `nif->caps` word set by the driver before `netif_register`
  (`NETIF_CAP_TXCSUM`, `NETIF_CAP_RXCSUM`). The loopback advertises
  both and marks every packet `M_CSUM_OK`: a transfer between two
  sockets of one machine computes no transport checksum at all, as
  Linux's `lo` does.
- Not done: jumbo frames and `MRG_RXBUF` (MTU stays 1500; QEMU's
  user-mode network never offers more), TSO/LRO (no measured need
  behind a 1500-byte MTU), page-granular or user-mappable clusters and
  mbuf-based TCP buffers (the zero-copy blockers of §9.4; a rewrite of
  the TCP data path, deferred with the copy counts recorded in
  `testing.md`), an XDP-like fast path.

### virtio-net offloads and per-queue CPUs

The driver negotiates `VIRTIO_NET_F_CSUM` and `VIRTIO_NET_F_GUEST_CSUM`.
Receive: a header with `VIRTIO_NET_HDR_F_DATA_VALID` sets `M_CSUM_OK`; a
header with `NEEDS_CSUM` (a partially checksummed frame from another
guest) has its checksum completed in software before input; neither
flag means the stack verifies as before. Transmit: with `NET_CSUM_TCP`
set the driver writes `NEEDS_CSUM`, `csum_start` and `csum_offset` into
the `virtio_net_hdr`; the stack skipped the checksum loop. The
interface advertises `NETIF_CAP_TXCSUM | NETIF_CAP_RXCSUM` accordingly;
without the features the driver advertises nothing and
`netif_transmit` finishes every sum in software, so a device that
offers no offload sees no change. That is the tested configuration:
QEMU's user-mode backend offers neither feature (the boot log says
`checksum offload: tx off, rx off`), so the driver's header writes and
`NEEDS_CSUM` completion are exercised by review only — a recorded gap,
small and isolated; the stack side (the partial form, software
completion, `M_CSUM_OK`) is covered by `net-csum-offload` and by every
loopback test.

`virtq_alloc_on(vdev, index, max, callback, cpu, out)` routes the
queue's MSI-X vector to `cpu` (`virtq_alloc` is the CPU 0 form); the
virtio-pci transport passes it to `pci_msix_request`, which NVMe already
uses per CPU. virtio-net keeps one queue pair: QEMU's user-mode network
backend has a single queue (`-netdev user,queues=N` is refused), so the
device never offers `VIRTIO_NET_F_MQ` here and a multi-queue negotiation
would be untestable code. The receive path is ready for it
(`netif_rx_on`, per-CPU queues, `virtq_alloc_on`); the driver's
`MQ`/`CTRL_VQ` negotiation is the recorded next step when a
multi-queue backend (tap, vhost) is in the test matrix.

### Benchmarks

### The NIC-path benchmark (`net-nicbench`, after the e1000e unit)

`net-bench` drives loopback, so it measures the stack, the scheduler and
the copies and never touches a device. The offload decision for a
driver needs traffic that *leaves the machine*, and until this existed
that decision was being made by assumption (`docs/audit/next-subsystem.md`,
"Benchmarks"). `net-nicbench` runs on every non-loopback interface the
machine has, bringing the default down to reach the second exactly as
`net-second-nic` does, and reports three things per interface:

- **ARP round trips through the driver's rings.** Requests for the
  gateway are built by hand and sent with `ether_output`; the replies
  are counted and consumed by the receive hook at the driver boundary,
  before any protocol layer sees them. The hook's context is one per
  round, on the round's stack frame: clearing the hook waits a grace
  period (`netif_set_rx_hook`), so no worker is still counting into a
  round that has ended, and a reply from one interface's round is never
  credited to the next interface's. QEMU's user-mode backend answers
  ARP in-process, so this is the lightest peer available and the number
  is the driver plus the device model plus the worker hand-off, with
  the IP stack out of the picture. Reported as round trips per second
  and nanoseconds per round trip.
- **UDP transmit through the whole stack and out the NIC.** 1 KiB
  datagrams to the gateway from an in-kernel socket, so the cost is the
  socket, UDP, IPv4, Ethernet, the driver and the device model. Reported
  as sends per second and nanoseconds per send, with how many frames the
  driver actually transmitted beside how many the socket accepted -- a
  driver that drops under load shows up as the gap.
- **The software checksum's share.** `in_cksum` over the same 1 KiB,
  timed by itself, as a percentage of a send. This is the gate: a
  transmit checksum offload can save at most that share, so if it is a
  few percent of a packet that is already dominated by the device model,
  the descriptor machinery it needs is not worth writing.

Reported, not compared, for the same reason as `net-bench`: TCG timing
on a shared host is noisy, and a benchmark that fails on a slow morning
teaches nothing. It does fail if fewer than half the ARP replies come
back, because that is a broken path rather than a slow one.

`net-bench` (debug builds; reports, never fails on timing) measures on
loopback: TCP throughput of a 4 MiB transfer with one flow and with two
concurrent flows, in MiB/s, and the UDP send rate over 10 000
64-byte datagrams (with how many the receiver's 64-entry queue kept),
once with steering disabled (every packet to CPU 0's queue: the old
architecture) and once with steering on, on the boot test's 4 CPUs.
Each round uses fresh ports (the previous round's connections sit in
TIME_WAIT). The numbers are recorded in `testing.md` under "Receive
scaling"; the deciding comparison is throughput with concurrent flows,
which is where one worker serialises. Under TCG the absolute figures
are small and noisy; the ratio between the two modes is the evidence
the rule asks for. `netif_set_steering` selects the mode; the read-only
`net.steer` sysctl reports it.

### Tests

`net-steer`: a fake interface injects packets of eight flows from every
CPU (`smp_call` or pinned threads): each flow's packets arrive at one
worker in order (the fake transport records `(cpu, sequence)` per
flow), the eight flows use more than one queue on 4 CPUs, `netif_rx_on`
lands where told, and `netif_unregister` purges every queue and its
barrier returns with nothing in flight. `net-csum-offload`: a fake
interface with `NETIF_CAP_TXCSUM` receives a hand-built IPv4/TCP packet
in the partial form with `csum_flags`, `csum_start` and `csum_offset`
right, and `m_csum_complete` turns it into a valid packet; the same
interface without the capability gets it finished by `netif_transmit`;
out-of-range offsets are refused (`-EINVAL`, nothing transmitted); a
received packet with a wrong checksum is dropped and counted, and the
same packet marked `M_CSUM_OK` is accepted (the interface's word is
final); `lo` advertises both capabilities.
`net-mbuf` gains the headroom checks (`m_pullup` keeps `NET_HEADROOM`;
an IPv6 TCP header set fits one cluster). `net-lo-tcp` and the harness
echo run unchanged over the steered path; `net-netif-lifetime` covers
the multi-worker barrier.

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
still completes; retransmission counters moved), `net-tcp-syncache` (a
300-SYN flood allocates nothing and a client still connects),
`net-tcp-rfc5961` (blind RST, SYN and ACK are challenged, the exact
reset accepted), `net-tcp-reorder` (delayed segments are queued and
delivered), `net-tcp-keepalive` (probes and timeout, an orphaned
FIN_WAIT_2 reaped), `net-icmp-limit` (echo replies rate limited, a
fragmentation-needed message lowers a connection's MSS),
`net-nonblock` (sockets and pipe ends never block and report
readiness), `net-harness` (as above, skipped without fw_cfg). Init: a
user-mode UDP echo to itself over loopback, a refused TCP connect, a
privileged bind, non-blocking sockets with `ioready`, and the error
paths of the calls. Details in `testing.md`.

## Future extensibility

Per-CPU receive queues and RSS; RCU routing table; SACK/window
scaling/timestamps; `SO_KEEPALIVE` and per-socket keepalive
parameters; IPv6 path MTU discovery; a `poll`/`select` built on the
readiness operation; DHCP; DNS resolver in userland; zero-copy receive
via page cache-style mappings; more NICs.
