/*
 * tcp.h - TCP (RFC 793 with RFC 6298 timers and RFC 5681 congestion
 * control basics). One lock covers the pcb table and every pcb in this
 * phase; see docs/kernel-services/network/design.md.
 */

#ifndef KERNEL_NET_TCP_H
#define KERNEL_NET_TCP_H

#include <kernel/list.h>
#include <kernel/mbuf.h>
#include <kernel/net/inet.h>
#include <kernel/net/ip.h>
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

/* A byte ring. */
struct netbuf {
    uint8_t *data;
    uint32_t size, head, len;   /* head = first byte, len = bytes stored */
};

struct socket;

struct tcp_pcb {
    enum tcp_state state;
    struct netaddr local, remote;
    /* send side */
    uint32_t iss, snd_una, snd_nxt, snd_wnd, snd_wl1, snd_wl2, snd_max;
    uint16_t mss;
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
    /* timers (interrupt context) hand off to the worker */
    struct timer rexmit, delack, timewait;
    unsigned rexmit_count;
    bool delack_pending, fin_queued, fin_sent, fin_rcvd;
    /* passive open */
    struct tcp_pcb *listener;
    struct list_node accept_link;   /* in listener->accept_queue */
    struct list_node accept_queue;
    unsigned backlog, nr_queued;
    /* bookkeeping */
    struct socket *sock;
    struct list_node link;
    int error;
    uint64_t retransmits, segs_in, segs_out;
    struct net_work work;
    unsigned work_flags;
};

void tcp_init(void);
struct tcp_pcb *tcp_pcb_new(uint16_t family);
/* Release the pcb from the socket side: FIN when connected, RST when
 * data is unread, immediate free when closed. The pcb may linger in
 * TIME_WAIT and free itself. */
void tcp_close(struct tcp_pcb *pcb);
int tcp_bind(struct tcp_pcb *pcb, const struct netaddr *local);
int tcp_listen(struct tcp_pcb *pcb, unsigned backlog);
/* Take an established child off the accept queue, or NULL. */
struct tcp_pcb *tcp_accept(struct tcp_pcb *pcb);
int tcp_connect(struct tcp_pcb *pcb, const struct netaddr *remote);   /* sends SYN; completion via sock_wake */
/* Copy into the send buffer and transmit; returns bytes taken (may be
 * fewer than len when the buffer is full, 0 when it is full). */
int64_t tcp_send(struct tcp_pcb *pcb, const void *data, size_t len);
/* Copy out of the receive buffer; 0 with peer_closed set at EOF. */
int64_t tcp_recv(struct tcp_pcb *pcb, void *data, size_t len, bool *peer_closed);
int tcp_shutdown_write(struct tcp_pcb *pcb);
uint32_t tcp_send_space(struct tcp_pcb *pcb);
/* Accessors for the socket layer's wait conditions (lock-free reads). */
bool tcp_accept_ready(struct tcp_pcb *pcb);
enum tcp_state tcp_state_of(struct tcp_pcb *pcb);
void tcp_attach_socket(struct tcp_pcb *pcb, struct socket *sock);
uint32_t tcp_recv_avail(struct tcp_pcb *pcb);

void tcp_input(struct netif *nif, struct mbuf *m, const struct ipv4_hdr *ip4, const struct ipv6_hdr *ip6);

struct tcp_stats {
    uint64_t segs_in, segs_out, retransmits, bad_cksum, rsts_in, rsts_out, conns_active, conns_passive,
        conns_established, dropped_no_pcb, out_of_order, timeouts;
};
void tcp_get_stats(struct tcp_stats *out);
const char *tcp_state_name(enum tcp_state s);

#endif /* KERNEL_NET_TCP_H */
