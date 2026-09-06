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
  that CPU's worker; a work item is on at most one list (`queued`). The
  TCP timers' `pcb_work` may therefore run on a different CPU than the
  connection's input; the per-pcb lock (milestone 8) already serialises
  the two, and `pcb_work` never assumed the input thread's context.
- **Barrier.** `netif_unregister` purges every CPU's queue and posts a
  barrier work item to every worker, waiting for all: after that no
  `input_one` of the interface is running anywhere. The lifetime rules
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
