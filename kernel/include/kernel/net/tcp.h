/*
 * tcp.h - TCP (RFC 793 with RFC 6298 timers, RFC 5681 congestion control
 * basics, RFC 5961 blind-attack protection, a SYN cache with cookies,
 * keepalive and out-of-order reassembly). One spinlock per pcb and one for
 * the hashed table; see docs/kernel-services/network/design.md,
 * "Hardening and per-connection locking".
 */

#ifndef KERNEL_NET_TCP_H
#define KERNEL_NET_TCP_H

#include <kernel/list.h>
#include <kernel/mbuf.h>
#include <kernel/net/inet.h>
#include <kernel/net/ip.h>
#include <kernel/spinlock.h>
#include <kernel/timer.h>

struct tcp_hdr {
    uint16_t sport;
    uint16_t dport;
    uint32_t seq;
    uint32_t ack;
    uint8_t  doff;       /* data offset in words << 4 */
    uint8_t  flags;
    uint16_t win;
    uint16_t cksum;
    uint16_t urg;
} __packed;
#define TH_FIN 0x01
#define TH_SYN 0x02
#define TH_RST 0x04
#define TH_PSH 0x08
#define TH_ACK 0x10
#define TCP_HDR_LEN(h) ((unsigned)((h)->doff >> 4) * 4u)

enum tcp_state {
    TCP_CLOSED,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RCVD,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_LAST_ACK,
    TCP_TIME_WAIT,
};

#define TCP_SNDBUF      65536u
#define TCP_RCVBUF      65536u
#define TCP_MSS_V4      1460u
#define TCP_MSS_V6      1440u
#define TCP_MSS_LO      16384u
#define TCP_MAX_REXMIT  8u
#define TCP_RTO_MIN_NS  (200ull * 1000000ull)
#define TCP_RTO_MAX_NS  (60ull * 1000000000ull)
#define TCP_RTO_INIT_NS (1000ull * 1000000ull)
#define TCP_DELACK_NS   (40ull * 1000000ull)
#define TCP_TIMEWAIT_NS (2ull * 1000000000ull)
#define TCP_MAX_BACKLOG 16u
#define TCP_MAX_WINDOW  65535u                   /* no window scaling: the largest window either end can offer */
#define TCP_HASH_SIZE   256u                     /* table buckets, keyed by local port */
#define TCP_SYNCACHE_SIZE   64u                  /* half-open connections remembered per listener */
#define TCP_SYNCACHE_TTL_NS (8ull * 1000000000ull)
#define TCP_FIN_WAIT2_NS    (60ull * 1000000000ull)   /* an orphaned FIN_WAIT_2 ends after this */
#define TCP_KEEPIDLE_NS     (7200ull * 1000000000ull)
#define TCP_KEEPINTVL_NS    (75ull * 1000000000ull)
#define TCP_KEEPCNT         9u
#define TCP_OOO_MAX         32u                  /* out-of-order segments kept per connection */
#define TCP_CHALLENGE_PER_SEC 100u               /* RFC 5961 challenge ACKs, host-wide */

/* A byte ring. */
struct netbuf {
    uint8_t *data;
    uint32_t size, head, len;   /* head = first byte, len = bytes stored */
};

struct socket;

/* One half-open connection a listener remembers (design.md, "Passive open"). */
struct tcp_syn_entry {
    struct netaddr remote, local;
    uint32_t iss, irs;
    uint16_t peer_mss, path_mss;
    uint64_t ts_ns;                /* 0 = free */
};

struct tcp_syncache {
    struct tcp_syn_entry e[TCP_SYNCACHE_SIZE];
};

/* A segment held for reassembly: [seq, seq + len) beyond rcv_nxt. */
struct tcp_ooo_seg {
    uint32_t seq, len;
    struct mbuf *m;
    bool fin;                      /* the segment carried FIN */
};

struct tcp_pcb {
    spinlock_t lock;               /* this pcb's state; docs: lock order listener -> child -> table */
    uint32_t refs;                 /* atomic: the state machine, the table, the socket, the accept queue,
                                      each lookup in flight, each queued work item */
    enum tcp_state state;
    struct netaddr local, remote;
    /* send side */
    uint32_t iss, snd_una, snd_nxt, snd_wnd, snd_wl1, snd_wl2, snd_max;
    uint16_t mss;                  /* negotiated: min(peer's option, path_mss) */
    uint16_t path_mss;             /* what this end can send on the path: TCP_MSS_LO when the peer is this
                                      host, else the family default. Fixed when the connection is set up
                                      (tcp_path_mss), so nothing under the TCP lock consults the netif
                                      registry (invariant N5). */
    struct netbuf sndbuf;          /* bytes from snd_una onward */
    /* receive side */
    uint32_t irs, rcv_nxt, rcv_wnd;
    struct netbuf rcvbuf;
    /* congestion and RTT */
    uint32_t cwnd, ssthresh;
    unsigned dupacks;
    uint64_t srtt_ns, rttvar_ns, rto_ns;
    uint32_t rtt_seq;
    uint64_t rtt_start_ns;
    bool rtt_pending;
    /* out-of-order reassembly, sorted by seq */
    struct tcp_ooo_seg ooo[TCP_OOO_MAX];
    unsigned ooo_n;
    uint32_t ooo_bytes;
    /* timers (interrupt context) hand off to the worker */
    struct timer rexmit, delack, timewait, keep;
    unsigned rexmit_count;
    uint64_t last_rx_ns;           /* last acceptable segment; keepalive idles from here */
    unsigned keep_probes;          /* unanswered keepalive probes */
    bool delack_pending, fin_queued, fin_sent, fin_rcvd;
    /* passive open */
    struct tcp_pcb *listener;       /* a queued child's listener; cleared by accept or the listener's close */
    struct list_node accept_link;   /* in listener->accept_queue */
    struct list_node accept_queue;  /* established children waiting for accept */
    struct tcp_syncache *syncache;  /* listeners only */
    unsigned backlog, nr_queued;
    /* bookkeeping */
    struct socket *sock;
    struct list_node hash_link;    /* the table bucket for local.port; empty when not in the table */
    int error;
    uint64_t retransmits, segs_in, segs_out;
    struct net_work work;
    unsigned work_flags;
};

void tcp_init(void);
struct tcp_pcb *tcp_pcb_new(uint16_t family);
/* The largest segment this host can send to `remote`: TCP_MSS_LO when the
 * peer is this host (delivery through `lo`), else the family default. Reads
 * the netif registry; call it before taking the TCP lock, never under it. */
uint16_t tcp_path_mss(uint16_t family, const struct netaddr *remote);
/* Release the pcb from the socket side: FIN when connected, RST when
 * data is unread, immediate free when closed. The pcb may linger in
 * TIME_WAIT and free itself. */
void tcp_close(struct tcp_pcb *pcb);
int tcp_bind(struct tcp_pcb *pcb, const struct netaddr *local);
int tcp_listen(struct tcp_pcb *pcb, unsigned backlog);
/* Take an established child off the accept queue, or NULL. */
/* Dequeue an established child, attached to `owner` under the lock, or NULL. */
struct tcp_pcb *tcp_accept(struct tcp_pcb *pcb, struct socket *owner);
int tcp_connect(struct tcp_pcb *pcb, const struct netaddr *remote);   /* sends SYN; completion via sock_wake */
/* Copy into the send buffer and transmit; returns bytes taken (may be
 * fewer than len when the buffer is full, 0 when it is full), -EAGAIN
 * while the handshake is still running. */
int64_t tcp_send(struct tcp_pcb *pcb, const void *data, size_t len);
/* Copy out of the receive buffer; 0 with peer_closed set at EOF. */
int64_t tcp_recv(struct tcp_pcb *pcb, void *data, size_t len, bool *peer_closed);
int tcp_shutdown_write(struct tcp_pcb *pcb);
uint32_t tcp_send_space(struct tcp_pcb *pcb);
/* Accessors for the socket layer's wait conditions (lock-free reads). */
bool tcp_accept_ready(struct tcp_pcb *pcb);
enum tcp_state tcp_state_of(struct tcp_pcb *pcb);
uint32_t tcp_recv_avail(struct tcp_pcb *pcb);
/* COSMO_IO_* bits for the pcb alone (the socket layer adds its shutdown state). */
unsigned tcp_ready(struct tcp_pcb *pcb);

void tcp_input(struct netif *nif, struct mbuf *m, const struct ipv4_hdr *ip4, const struct ipv6_hdr *ip6);
/* An ICMP "fragmentation needed" quoting a segment of ours: lower the
 * connection's MSS to fit `mtu` and retransmit. Ignored unless the quoted
 * sequence number is in flight (RFC 5927). Called on the network worker. */
void tcp_pmtu_notify(const struct netaddr *local, const struct netaddr *remote, uint32_t seq, uint16_t mtu);
/* Test hooks: keepalive parameters and the orphaned FIN_WAIT_2 timeout (0 = default). */
void tcp_set_keepalive(uint64_t idle_ns, uint64_t intvl_ns, unsigned cnt);
void tcp_set_fin_wait2(uint64_t ns);

struct tcp_stats {
    uint64_t segs_in, segs_out, retransmits, bad_cksum, rsts_in, rsts_out, conns_active, conns_passive,
        conns_established, dropped_no_pcb, out_of_order, timeouts;
    uint64_t syn_cached, syn_cookies_sent, syn_cookies_ok, syn_bad_ack, challenge_acks, ooo_queued, ooo_dropped,
        keepalive_probes, fin_wait2_timeouts, pmtu_updates;
};
void tcp_get_stats(struct tcp_stats *out);
const char *tcp_state_name(enum tcp_state s);

#endif /* KERNEL_NET_TCP_H */
