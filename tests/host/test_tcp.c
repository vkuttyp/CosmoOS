/*
 * test_tcp.c - TCP (kernel-services/network/tcp.c) on the host: roadmap 2.0
 * SA6. A digest over seeded histories, recorded from the C, of a host with a
 * listener on :80 and up to six connections to two peers on its link:
 *
 *   - passive opens through the listener: handshakes completed, abandoned,
 *     answered wrongly; SYN floods past the SYN cache, and handshakes
 *     completed by cookie; accepted, and owners that never accept;
 *   - active opens: answered with a SYN-ACK, a RST, a simultaneous SYN, or
 *     nothing (retransmitted SYNs, the give-up);
 *   - the peer's segments on every connection: every flag, sequence and
 *     acknowledgment offset around what the connection expects, windows,
 *     options, payloads (in order, out of order, overlapping), checksums
 *     corrupted; ICMP fragmentation-needed quoting the host's last segment;
 *   - the host's sends, receives, shutdowns and closes, owners letting go,
 *     keepalive and FIN_WAIT_2 settings; time passing and the timers firing
 *     (delayed ACKs, retransmission and its backoff, keepalives, TIME_WAIT);
 *   - the table listing, the port question NAT asks, readiness, the
 *     statistics.
 *
 * The rest of the stack (IPv4, Ethernet, ARP, mbufs, the checksum) runs as
 * the kernel's, over tests/fuzz/shim_net.c, the TCP fuzz target's host:
 * timers, work items, the clock and the random source are the shim's,
 * seeded. Every frame the host transmits, every state change (through
 * TCP_HOST_TEST's hook) and every return is folded into the digest; every
 * transmitted frame has correct checksums; after each history nothing is
 * allocated beyond the baseline and no mbuf is alive (TCP_DUMP=1 lists what
 * is left otherwise, and the history of a pcb that is).
 *
 * Not reached: IPv6 (the shim's link has no IPv6 address), a pcb ended
 * between the lookup and the lock (a second thread), a pullup failing, and
 * the debug builds' test seams the kernel's self-tests drive.
 */

#include <kernel/net/cksum.h>
#include <kernel/net/ether.h>
#include <kernel/net/fw.h>
#include <kernel/net/inet.h>
#include <kernel/net/ip.h>
#include <kernel/net/tcp.h>
#include <kernel/socket.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "harness.h"
#include "netpkt.h"
#include "shim_net.h"

/* The digest of the C (tcp.c, 2026-11-03), which the CeeSafe port gives. */
#define TCP_DIGEST 0x62a476ae9c5d6fbdull
#define HISTORIES 4u
#define OPS 1500u
#define SEED 0xC05F5FA2026B001Cull
#define NCONN 6u

static uint64_t g_rng, g_digest;

static uint64_t next(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static void mix(const void *bytes, size_t n)
{
    const uint8_t *b = bytes;
    for (size_t i = 0; i < n; i++) {
        g_digest ^= b[i];
        g_digest *= 0x100000001b3ull;
    }
}

static void mix32(uint32_t v) { mix(&v, sizeof(v)); }
static void mix64(uint64_t v) { mix(&v, sizeof(v)); }

static bool g_benching;

static void note(uint32_t which, uint64_t value)
{
    if (g_benching)
        return;
    mix32(0x6100u + which);
    mix64(value);
}

/* shim_net.c's assertions land in the harness. */
void fuzz_fail(const char *file, int line, const char *expr) { harness_fail(file, line, expr); }

/* ---- the network ---- */

static const uint8_t k_mac0[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
static const uint8_t k_peer_mac[2][6] = { { 0x52, 0x55, 0x0a, 0x00, 0x02, 0x02 }, { 0x52, 0x55, 0x0a, 0x00, 0x02, 0x03 } };
#define IP0 IPV4_ADDR(10, 0, 2, 15)
#define MASK24 htonl(0xffffff00u)

static uint32_t peer_ip(unsigned p) { return IPV4_ADDR(10, 0, 2, 2 + p); }

static struct fz_netif g_f0;
static unsigned g_bad_frames;
static uint8_t g_last_tx[FZ_CAPTURE_MAX];
static uint32_t g_last_tx_len;

static void on_frame(struct fz_netif *f, const uint8_t *frame, uint32_t len)
{
    (void)f;
    if (!g_benching && np_check_checksums(frame, len) != 0)
        g_bad_frames++;
    note(1, len);
    if (!g_benching)
        mix(frame, len < 128 ? len : 128);
    if (len >= NP_ETH + NP_IPV4 + NP_TCP && np_get16(frame + 12) == ETH_P_IP && frame[NP_ETH + 9] == IPPROTO_TCP) {
        g_last_tx_len = len;
        memcpy(g_last_tx, frame, len);
    }
}

static struct { const void *pcb; uint8_t from, to; uint16_t op; } g_trace[4096];
static unsigned g_ntrace, g_op;
static uint8_t g_op_kind[8192];

void tcp_test_state_change(struct tcp_pcb *pcb, enum tcp_state from, enum tcp_state to)
{
    note(2, ((uint64_t)from << 8) | to);
    g_trace[g_ntrace % 4096].pcb = pcb;
    g_trace[g_ntrace % 4096].from = (uint8_t)from;
    g_trace[g_ntrace % 4096].to = (uint8_t)to;
    g_trace[g_ntrace % 4096].op = (uint16_t)g_op;
    g_ntrace++;
}

static void deliver(const uint8_t *frame, size_t len)
{
    uint8_t buf[FZ_CAPTURE_MAX];
    if (len > sizeof(buf))
        len = sizeof(buf);
    memcpy(buf, frame, len);
    fz_deliver(&g_f0, buf, (uint32_t)len);
    fz_run_work();
}

/* A segment from peer p's port to the host's. */
static void segment(unsigned p, uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack, uint8_t flags, uint16_t win,
                    const uint8_t *opts, uint8_t optlen, const uint8_t *payload, uint16_t paylen, bool corrupt)
{
    static uint8_t f[NP_ETH + NP_IPV4 + 60 + 1460];
    size_t n = np_frame_tcp4(f, k_mac0, k_peer_mac[p], peer_ip(p), IP0, sport, dport, seq, ack, flags, win, opts, optlen,
                             payload, paylen);
    if (corrupt)
        f[NP_ETH + NP_IPV4 + 16] ^= 0x5a;
    deliver(f, n);
}

/* The host's last segment to (peer p, port), its sequence and acknowledgment, if it was to them. */
static bool last_to(unsigned p, uint16_t port, uint32_t *seq, uint32_t *ack, uint8_t *flags)
{
    if (g_last_tx_len < NP_ETH + NP_IPV4 + NP_TCP)
        return false;
    const uint8_t *ip = g_last_tx + NP_ETH;
    const uint8_t *t = ip + (ip[0] & 0xf) * 4u;
    uint32_t dst;
    memcpy(&dst, ip + 16, 4);
    if (dst != peer_ip(p) || np_get16(t + 2) != port)
        return false;
    *seq = np_get32(t + 4);
    *ack = np_get32(t + 8);
    *flags = t[13];
    return true;
}

/* ---- connections ---- */

static const struct kobject_type g_sock_type = { .name = "test-socket" };

struct conn {
    struct tcp_pcb *pcb;
    struct socket sock;
    bool owned;              /* the host still holds it: not closed */
    bool let_go;             /* its owner let go without closing: an ownerless pcb, as only the fuzz targets make */
    unsigned peer;
    uint16_t peer_port, host_port;
    uint32_t peer_seq, peer_ack;   /* what the peer sends next, read while owned */
};

static struct conn g_conns[NCONN];
static struct tcp_pcb *g_listener;
static struct socket g_listener_sock;
static uint16_t g_next_port;

static void expectations(struct conn *c)
{
    if (c->pcb != NULL && c->owned) {
        c->peer_seq = c->pcb->rcv_nxt;
        c->peer_ack = c->pcb->snd_nxt;
    }
}

static struct conn *free_conn(void)
{
    for (unsigned i = 0; i < NCONN; i++)
        if (g_conns[i].pcb == NULL)
            return &g_conns[i];
    return NULL;
}

static struct conn *some_conn(void)
{
    struct conn *c = &g_conns[next() % NCONN];
    return c->pcb != NULL ? c : NULL;
}

static void own(struct conn *c, struct tcp_pcb *pcb)
{
    memset(&c->sock, 0, sizeof(c->sock));
    kobject_init(&c->sock.obj, &g_sock_type);
    c->sock.tcp = pcb;
    c->pcb = pcb;
    c->owned = true;
    c->let_go = false;
    expectations(c);
}

static void some_options(uint8_t *opts, uint8_t *len)
{
    *len = 0;
    if (next() % 2) {
        static const uint16_t MSS[] = { 1460, 536, 1200, 88, 9000 };
        uint16_t mss = MSS[next() % 5];
        opts[(*len)++] = 2;
        opts[(*len)++] = 4;
        opts[(*len)++] = (uint8_t)(mss >> 8);
        opts[(*len)++] = (uint8_t)mss;
    }
    if (next() % 4 == 0) {   /* window scale, then SACK permitted, then something unknown or broken */
        opts[(*len)++] = 1;
        opts[(*len)++] = 3;
        opts[(*len)++] = 3;
        opts[(*len)++] = (uint8_t)(next() % 16);
    }
    if (next() % 8 == 0) {
        opts[(*len)++] = 4;
        opts[(*len)++] = 2;
    }
    if (next() % 16 == 0) {
        opts[(*len)++] = 99;
        opts[(*len)++] = (uint8_t)(next() % 6);
    }
    while (*len % 4)
        opts[(*len)++] = 1;
}

/* A passive open: the peer's SYN to :80, maybe the completing ACK, maybe the accept. */
static void op_passive(void)
{
    unsigned p = (unsigned)(next() % 2);
    uint16_t port = (uint16_t)(40000 + g_next_port++ % 2000);
    uint32_t isn = (uint32_t)next();
    uint8_t opts[40], optlen;
    some_options(opts, &optlen);
    segment(p, port, 80, isn, 0, TH_SYN, 65535, opts, optlen, NULL, 0, false);
    uint32_t seq, ack;
    uint8_t flags;
    if (!last_to(p, port, &seq, &ack, &flags) || (flags & (TH_SYN | TH_ACK)) != (TH_SYN | TH_ACK)) {
        note(10, 0);
        return;
    }
    switch (next() % 6) {
    case 0: return;   /* abandoned: the SYN cache holds it, then expires it */
    case 1:
        segment(p, port, 80, isn + 1, seq + 7, TH_ACK, 65535, NULL, 0, NULL, 0, false);   /* a wrong ACK: a RST */
        return;
    case 2:
        segment(p, port, 80, isn + 1, seq + 1, TH_RST, 0, NULL, 0, NULL, 0, false);
        return;
    default: break;
    }
    uint8_t hello[32];
    uint16_t n = (uint16_t)(next() % 4 == 0 ? next() % 32 : 0);
    for (uint16_t k = 0; k < n; k++)
        hello[k] = (uint8_t)next();
    segment(p, port, 80, isn + 1, seq + 1, TH_ACK | (n ? TH_PSH : 0), 65535, NULL, 0, hello, n, false);
    struct conn *c = free_conn();
    if (c == NULL || next() % 8 == 0)
        return;   /* never accepted: the listener's queue keeps it until the listener closes */
    memset(&c->sock, 0, sizeof(c->sock));
    kobject_init(&c->sock.obj, &g_sock_type);
    struct tcp_pcb *pcb = tcp_accept(g_listener, &c->sock);
    note(11, pcb != NULL);
    if (pcb == NULL)
        return;
    c->peer = p;
    c->peer_port = port;
    c->host_port = 80;
    own(c, pcb);
}

/* A SYN flood: more half-open handshakes than the SYN cache holds; one completed by cookie. */
static void op_flood(void)
{
    unsigned p = (unsigned)(next() % 2);
    unsigned count = 20 + (unsigned)(next() % 60);
    uint16_t last_port = 0;
    uint32_t last_isn = 0, last_seq = 0;
    bool have = false;
    for (unsigned k = 0; k < count; k++) {
        uint16_t port = (uint16_t)(42000 + g_next_port++ % 4000);
        uint32_t isn = (uint32_t)next();
        segment(p, port, 80, isn, 0, TH_SYN, 65535, NULL, 0, NULL, 0, false);
        uint32_t seq, ack;
        uint8_t flags;
        if (last_to(p, port, &seq, &ack, &flags) && (flags & TH_SYN)) {
            last_port = port;
            last_isn = isn;
            last_seq = seq;
            have = true;
        }
    }
    note(12, count);
    if (have && next() % 2)
        segment(p, last_port, 80, last_isn + 1, last_seq + (next() % 4 == 0 ? 3 : 1), TH_ACK, 65535, NULL, 0, NULL, 0,
                false);
}

/* An active open: the host connects; the peer answers or not. */
static void op_active(void)
{
    struct conn *c = free_conn();
    if (c == NULL)
        return;
    unsigned p = (unsigned)(next() % 2);
    struct tcp_pcb *pcb = tcp_pcb_new(COSMO_AF_INET);
    if (pcb == NULL)
        return;
    c->peer = p;
    c->peer_port = (uint16_t)(7000 + next() % 3);
    own(c, pcb);
    pcb->sock = &c->sock;
    struct netaddr a;
    memset(&a, 0, sizeof(a));
    a.family = COSMO_AF_INET;
    if (next() % 4 == 0)
        a.port = (uint16_t)(30000 + next() % 4);   /* a chosen port, sometimes taken */
    note(20, (uint32_t)tcp_bind(pcb, &a));
    a.port = c->peer_port;
    a.v4 = peer_ip(p);
    int rc = tcp_connect(pcb, &a);
    note(21, (uint32_t)rc);
    c->host_port = pcb->local.port;
    uint32_t seq, ack;
    uint8_t flags;
    if (rc != 0 || !last_to(p, c->peer_port, &seq, &ack, &flags))
        return;
    uint32_t isn = (uint32_t)next();
    uint8_t opts[40], optlen;
    some_options(opts, &optlen);
    switch (next() % 6) {
    case 0: break;   /* silence: the SYN is retransmitted, then the connect gives up */
    case 1: segment(p, c->peer_port, c->host_port, 0, seq + 1, TH_RST | TH_ACK, 0, NULL, 0, NULL, 0, false); break;
    case 2: segment(p, c->peer_port, c->host_port, isn, 0, TH_SYN, 65535, opts, optlen, NULL, 0, false); break;
    default:
        segment(p, c->peer_port, c->host_port, isn, seq + 1, TH_SYN | TH_ACK, 65535, opts, optlen, NULL, 0, false);
        break;
    }
    expectations(c);
}

static uint8_t g_buf[8192];

/* A segment from the connection's peer, relative to what the connection expects. */
static void op_peer(void)
{
    struct conn *c = some_conn();
    if (c == NULL)
        return;
    expectations(c);
    static const uint8_t FLAGS[] = { TH_ACK, TH_ACK, TH_ACK | TH_PSH, TH_ACK | TH_FIN, TH_RST, TH_SYN,
                                     TH_ACK | TH_RST, TH_SYN | TH_ACK, TH_ACK | 0x20, 0 };
    uint8_t flags = FLAGS[next() % 10];
    int32_t seqoff = next() % 4 == 0 ? (int32_t)(next() % 4000) - 2000 : 0;
    int32_t ackoff = next() % 4 == 0 ? (int32_t)(next() % 400) - 200 : 0;
    static const uint16_t WIN[] = { 65535, 65535, 0, 1, 512, 8192 };
    uint16_t win = WIN[next() % 6];
    uint8_t opts[40], optlen = 0;
    if (next() % 8 == 0)
        some_options(opts, &optlen);
    uint16_t paylen = next() % 3 == 0 ? (uint16_t)(next() % 1400) : (uint16_t)(next() % 64);
    for (uint16_t k = 0; k < paylen && k < 64; k++)
        g_buf[k] = (uint8_t)next();
    bool corrupt = next() % 16 == 0;
    segment(c->peer, c->peer_port, c->host_port, c->peer_seq + (uint32_t)seqoff, c->peer_ack + (uint32_t)ackoff, flags, win,
            opts, optlen, g_buf, paylen, corrupt);
}

static void op_host(void)
{
    struct conn *c = some_conn();
    if (c == NULL)
        return;
    switch (next() % 8) {
    case 0:
    case 1:
    case 2: {
        if (!c->owned)
            return;
        size_t len = next() % 4 == 0 ? 6000 : (size_t)(next() % 600);
        for (size_t k = 0; k < len && k < 64; k++)
            g_buf[k] = (uint8_t)next();
        note(30, (uint64_t)tcp_send(c->pcb, g_buf, len));
        break;
    }
    case 3:
    case 4: {
        if (!c->owned)
            return;
        bool eof = false;
        int64_t n = tcp_recv(c->pcb, g_buf, (size_t)(next() % 2000), &eof);
        note(31, ((uint64_t)eof << 32) | (uint32_t)n);
        if (n > 0)
            mix(g_buf, (size_t)n);
        break;
    }
    case 5:
        if (c->owned)
            note(32, (uint32_t)tcp_shutdown_write(c->pcb));
        break;
    case 6:
        if (c->owned) {
            note(33, c->pcb->state);
            tcp_close(c->pcb);
        }
        c->pcb = NULL;
        c->owned = false;
        break;
    default:
        if (c->owned && next() % 4 == 0) {   /* the owner lets go without closing */
            arch_irq_state_t s = spin_lock_irqsave(&c->pcb->lock);
            c->pcb->sock = NULL;
            spin_unlock_irqrestore(&c->pcb->lock, s);
            c->let_go = true;
            note(34, 0);   /* still the host's to close: that close must not put the machine's reference again */
        } else if (c->owned) {
            note(35, ((uint64_t)tcp_send_space(c->pcb) << 32) | tcp_recv_avail(c->pcb));
            note(36, ((uint64_t)tcp_ready(c->pcb) << 8) | tcp_state_of(c->pcb));
        }
        break;
    }
}

/* ICMP fragmentation-needed quoting the host's last segment. */
static void op_needfrag(void)
{
    if (g_last_tx_len < NP_ETH + NP_IPV4 + 8)
        return;
    uint8_t inner[60 + 8];
    const uint8_t *ip = g_last_tx + NP_ETH;
    unsigned ihl = (unsigned)(ip[0] & 0xf) * 4u;
    if (ihl < NP_IPV4 || NP_ETH + ihl + 8 > g_last_tx_len)
        return;
    memcpy(inner, ip, ihl + 8);
    uint32_t dst;
    memcpy(&dst, ip + 16, 4);
    unsigned p = dst == peer_ip(1) ? 1 : 0;
    static const uint16_t MTU[] = { 1400, 576, 68, 1500, 1280, 9000 };
    uint8_t f[NP_ETH + NP_IPV4 + 8 + 68];
    size_t n = np_frame_icmp4(f, k_mac0, k_peer_mac[p], peer_ip(p), IP0, ICMP_DEST_UNREACH, ICMP_UNREACH_NEEDFRAG, 0,
                              MTU[next() % 6], inner, ihl + 8);
    deliver(f, n);
}

/* The peer's data out of order: later segments first, then the gap filled (the out-of-order queue). */
static void op_ooo(void)
{
    struct conn *c = some_conn();
    if (c == NULL || !c->owned)
        return;
    expectations(c);
    unsigned n = 2 + (unsigned)(next() % 6);
    uint16_t len = (uint16_t)(1 + next() % 400);
    unsigned order[8];
    for (unsigned k = 0; k < n; k++)
        order[k] = k;
    for (unsigned k = n - 1; k > 0; k--) {   /* a shuffle, the first one last */
        unsigned j = (unsigned)(next() % (k + 1));
        unsigned t = order[k];
        order[k] = order[j];
        order[j] = t;
    }
    for (unsigned k = 0; k < n; k++) {
        unsigned at = order[k];
        for (uint16_t b = 0; b < len && b < 64; b++)
            g_buf[b] = (uint8_t)(at * 31 + b);
        bool dup = next() % 8 == 0;
        segment(c->peer, c->peer_port, c->host_port, c->peer_seq + at * len - (dup ? len / 2 : 0), c->peer_ack,
                TH_ACK | TH_PSH, 65535, NULL, 0, g_buf, len, false);
    }
    note(13, ((uint64_t)n << 16) | len);
}

/* The host firewall's OUTPUT chain refuses what the host sends to one peer port, then stops. */
static void op_refuse(void)
{
    struct fw_rule rule;
    memset(&rule, 0, sizeof(rule));
    rule.direction = FW_DIR_OUTPUT;
    rule.proto = IPPROTO_TCP;
    rule.dst_prefix = 32;
    rule.dst_ip = peer_ip((unsigned)(next() % 2));
    rule.dst_port = (uint16_t)(next() % 2 ? 7000 + next() % 3 : 0);
    rule.verdict = FW_DROP;
    if (next() % 2)
        note(14, (uint32_t)fw_rule_add(FW_HOST_GUEST_IP, 0, &rule));
    else {
        struct fw_rule rules[FW_RULES_PER_GUEST];
        unsigned n = fw_rule_list(FW_HOST_GUEST_IP, rules, FW_RULES_PER_GUEST);
        for (unsigned k = 0; k < n; k++)
            (void)fw_rule_del(FW_HOST_GUEST_IP, &rules[k]);
        note(15, n);
    }
}

/* ICMP unreachable (port or host) quoting the host's last segment: a SYN-ACK's refused, a flow's error. */
static void op_unreach(void)
{
    if (g_last_tx_len < NP_ETH + NP_IPV4 + 8)
        return;
    uint8_t inner[60 + 8];
    const uint8_t *ip = g_last_tx + NP_ETH;
    unsigned ihl = (unsigned)(ip[0] & 0xf) * 4u;
    if (ihl < NP_IPV4 || NP_ETH + ihl + 8 > g_last_tx_len)
        return;
    memcpy(inner, ip, ihl + 8);
    uint32_t dst;
    memcpy(&dst, ip + 16, 4);
    unsigned p = dst == peer_ip(1) ? 1 : 0;
    uint8_t code = next() % 2 ? ICMP_UNREACH_PORT : ICMP_UNREACH_HOST;
    uint8_t f[NP_ETH + NP_IPV4 + 8 + 68];
    size_t n = np_frame_icmp4(f, k_mac0, k_peer_mac[p], peer_ip(p), IP0, ICMP_DEST_UNREACH, code, 0, 0, inner, ihl + 8);
    deliver(f, n);
}

/* What a connect or a bind may not do. */
static void op_misuse(void)
{
    struct netaddr a;
    memset(&a, 0, sizeof(a));
    a.family = COSMO_AF_INET;
    a.port = 7000;
    a.v4 = peer_ip(0);
    struct conn *c = some_conn();
    switch (next() % 6) {
    case 0:
        /* Connected, connecting, or retired by the network under its owner (a later connect reuses it). Never an
         * ownerless pcb: the network kills one outright, and no socket can connect it again. */
        if (c != NULL && c->owned && !c->let_go)
            note(16, (uint32_t)tcp_connect(c->pcb, &a));
        break;
    case 1:
        note(16, (uint32_t)tcp_connect(g_listener, &a));   /* a listener */
        break;
    case 2:
        if (c != NULL && c->owned && !c->let_go)
            note(17, (uint32_t)tcp_bind(c->pcb, &a));   /* bound already */
        break;
    default: {
        struct tcp_pcb *pcb = tcp_pcb_new(next() % 2 ? COSMO_AF_INET6 : COSMO_AF_INET);
        if (pcb == NULL)
            break;
        struct netaddr b;
        memset(&b, 0, sizeof(b));
        b.family = next() % 2 ? COSMO_AF_INET6 : COSMO_AF_INET;   /* sometimes not the pcb's */
        b.port = 7001;
        if (b.family == COSMO_AF_INET)
            b.v4 = next() % 4 == 0 ? IPV4_ADDR(192, 0, 2, 1) : peer_ip(0);   /* off-link with a gateway, or on it */
        else
            b.v6.s6_addr[0] = next() % 2 ? 0x20 : 0xfe;
        note(18, (uint32_t)tcp_listen(pcb, 1));   /* not bound */
        note(19, (uint32_t)tcp_connect(pcb, &b));
        tcp_close(pcb);
        break;
    }
    }
}

/* Data in flight, then three duplicate ACKs: a fast retransmit. */
static void op_dupacks(void)
{
    struct conn *c = some_conn();
    if (c == NULL || !c->owned || c->pcb->state != TCP_ESTABLISHED)
        return;
    for (size_t k = 0; k < 64; k++)
        g_buf[k] = (uint8_t)next();
    (void)tcp_send(c->pcb, g_buf, 3000);
    fz_run_work();
    expectations(c);
    uint32_t una = c->pcb->snd_una;
    for (unsigned k = 0; k < 4; k++)
        segment(c->peer, c->peer_port, c->host_port, c->peer_seq, una, TH_ACK, 65535, NULL, 0, NULL, 0, false);
    note(22, c->pcb->snd_nxt - una);
}

/* Both sides close at once: the host's FIN, the peer's FIN that does not acknowledge it (CLOSING), then the ACK
 * (TIME_WAIT), then the peer's FIN again (retransmitted into TIME_WAIT). */
static void op_simclose(void)
{
    struct conn *c = some_conn();
    if (c == NULL || !c->owned || c->pcb->state != TCP_ESTABLISHED)
        return;
    (void)tcp_shutdown_write(c->pcb);
    fz_run_work();
    expectations(c);
    uint32_t fin_seq = c->peer_seq, our_fin = c->pcb->snd_max;
    segment(c->peer, c->peer_port, c->host_port, fin_seq, our_fin - 1, TH_ACK | TH_FIN, 65535, NULL, 0, NULL, 0, false);
    note(23, c->pcb->state);
    segment(c->peer, c->peer_port, c->host_port, fin_seq + 1, our_fin, TH_ACK, 65535, NULL, 0, NULL, 0, false);
    note(23, c->pcb->state);
    if (next() % 2)
        segment(c->peer, c->peer_port, c->host_port, fin_seq, our_fin, TH_ACK | TH_FIN, 65535, NULL, 0, NULL, 0, false);
}

/* A segment whose header lies: shorter than a header, or a data offset past the segment or below 20. */
static void op_malformed(void)
{
    struct conn *c = some_conn();
    uint16_t sport = c ? c->peer_port : 40000, dport = c ? c->host_port : 80;
    unsigned p = c ? c->peer : 0;
    uint8_t seg[64];
    for (unsigned k = 0; k < sizeof(seg); k++)
        seg[k] = (uint8_t)next();
    unsigned len = next() % 2 ? (unsigned)(next() % 20) : 20 + (unsigned)(next() % 44);
    if (len >= 13)
        seg[12] = next() % 2 ? 0x40 : 0xf0;   /* doff 4 (below 20) or 15 (60, past a short segment) */
    np_put16(seg, sport);
    if (len >= 4)
        np_put16(seg + 2, dport);
    uint8_t f[NP_ETH + NP_IPV4 + 64];
    size_t o = np_eth(f, k_mac0, k_peer_mac[p], ETH_P_IP);
    memcpy(f + o + NP_IPV4, seg, len);
    np_ipv4(f + o, peer_ip(p), IP0, IPPROTO_TCP, (uint16_t)len, 64);
    if (len >= 18)
        np_l4_cksum4(f + o + NP_IPV4, len, peer_ip(p), IP0, IPPROTO_TCP, 16);
    deliver(f, o + NP_IPV4 + len);
    note(24, len);
}

static void op_time(void)
{
    if (next() % 3 == 0) {
        note(40, fz_fire_until(600ull * 1000000000ull, 1));
    } else {
        static const uint64_t MS[] = { 1, 10, 40, 200, 1000, 3000, 30000, 70000 };
        fz_clock_advance(MS[next() % 8] * 1000000ull);
    }
    fz_run_work();
}

static void op_tables(void)
{
    struct tcp_sock_info info[16];
    memset(info, 0, sizeof(info));
    unsigned max = (unsigned)(next() % 17);
    unsigned n = tcp_list(info, max, COSMO_AF_INET);
    note(50, ((uint64_t)max << 32) | n);
    for (unsigned k = 0; k < n && k < max; k++) {
        mix(&info[k].local, sizeof(info[k].local));
        mix(&info[k].remote, sizeof(info[k].remote));
        mix32(info[k].state);
        mix32(info[k].txq);
        mix32(info[k].rxq);
    }
    struct tcp_stats st;
    memset(&st, 0, sizeof(st));
    tcp_get_stats(&st);
    mix(&st, sizeof(st));
    struct netaddr a;
    memset(&a, 0, sizeof(a));
    a.family = COSMO_AF_INET;
    a.v4 = IP0;
    note(51, ((uint64_t)tcp_port_in_use(COSMO_AF_INET, 80, &a) << 1) | tcp_accept_ready(g_listener));
}

static void op_settings(void)
{
    if (next() % 2) {
        uint64_t idle = (1 + next() % 60) * 1000000000ull;
        uint64_t intvl = (1 + next() % 10) * 1000000000ull;
        unsigned cnt = (unsigned)(1 + next() % 5);
        tcp_set_keepalive(idle, intvl, cnt);
    } else
        tcp_set_fin_wait2((1 + next() % 60) * 1000000000ull);
    note(52, 0);
}

/* ---- the histories ---- */

static void setup(unsigned h)
{
    fz_random_seed(SEED + h);
    fw_flush();   /* no OUTPUT rule left from the last history */
    g_bad_frames = 0;
    g_last_tx_len = 0;
    memset(g_conns, 0, sizeof(g_conns));
    fz_netif_register(&g_f0, "fz0", k_mac0, IP0, MASK24, peer_ip(0), 0);
    g_f0.on_frame = on_frame;
    for (unsigned p = 0; p < 2; p++) {
        uint8_t f[128];
        size_t n = np_frame_arp(f, eth_broadcast, k_peer_mac[p], 1, k_peer_mac[p], peer_ip(p), eth_broadcast, IP0);
        deliver(f, n);
    }
    g_listener = tcp_pcb_new(COSMO_AF_INET);
    EXPECT(g_listener != NULL);
    memset(&g_listener_sock, 0, sizeof(g_listener_sock));
    kobject_init(&g_listener_sock.obj, &g_sock_type);
    g_listener_sock.tcp = g_listener;
    g_listener->sock = &g_listener_sock;
    struct netaddr a;
    memset(&a, 0, sizeof(a));
    a.family = COSMO_AF_INET;
    a.port = 80;
    EXPECT(tcp_bind(g_listener, &a) == 0);
    EXPECT(tcp_listen(g_listener, (unsigned)(2 + h)) == 0);
}

static void teardown(size_t baseline)
{
    for (unsigned i = 0; i < NCONN; i++)
        if (g_conns[i].pcb != NULL && g_conns[i].owned)
            tcp_close(g_conns[i].pcb);
    memset(g_conns, 0, sizeof(g_conns));
    tcp_close(g_listener);
    g_listener = NULL;
    for (unsigned i = 0; i < 900 && fz_allocs_live() != baseline; i++) {
        fz_clock_advance(1000000000ull);
        fz_run_work();
    }
    fz_netif_unregister_all();
    struct mbuf_stats ms;
    mbuf_get_stats(&ms);
    EXPECT(ms.mbufs_alive == 0 && ms.clusters_alive == 0);
    if (fz_allocs_live() != baseline && getenv("TCP_DUMP")) {
        fprintf(stderr, "live %zu baseline %zu timers %u; tcp_pcb is %zu bytes\n", fz_allocs_live(), baseline,
                fz_timers_pending(), sizeof(struct tcp_pcb));
        fz_dump_live();
        fz_dump_timers();
        struct tcp_pcb *x = fz_live_object_of_size(sizeof(struct tcp_pcb));
        if (x) {
            fprintf(stderr, "  pcb %p: state %s refs %u sock %p local %u remote %u in table %d error %d work %#x\n", (void *)x,
                    tcp_state_name(x->state), x->refs, (void *)x->sock, x->local.port, x->remote.port,
                    !list_empty(&x->hash_link), x->error, x->work_flags);
            for (unsigned i = 0; i < g_ntrace && i < 4096; i++)
                if (g_trace[i].pcb == x)
                    fprintf(stderr, "    op %u (%u): %s -> %s\n", g_trace[i].op, g_op_kind[g_trace[i].op % 8192],
                            tcp_state_name((enum tcp_state)g_trace[i].from), tcp_state_name((enum tcp_state)g_trace[i].to));
        }
    }
    EXPECT(fz_allocs_live() == baseline);
    EXPECT(fz_sock_wake_refs() == 0);
}

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    fz_net_init();
    size_t baseline = fz_allocs_live();
    for (unsigned h = 0; h < HISTORIES; h++) {
        setup(h);
        for (unsigned r = 0; r < OPS; r++) {
            uint64_t k = next() % 32;
            g_op = h * OPS + r;
            g_op_kind[g_op % 8192] = (uint8_t)k;
            if (k < 4)
                op_passive();
            else if (k < 5)
                op_flood();
            else if (k < 8)
                op_active();
            else if (k < 18)
                op_peer();
            else if (k < 24)
                op_host();
            else if (k < 25)
                op_needfrag();
            else if (r % 4 == 0 && (k == 25 || k == 26))
                op_ooo();
            else if (r % 4 == 1 && (k == 25 || k == 26))
                op_refuse();
            else if (r % 4 == 2 && (k == 25 || k == 26))
                op_unreach();
            else if (k == 25 || k == 26)
                op_misuse();
            else if (k == 27 && r % 3 == 0)
                op_dupacks();
            else if (k == 27 && r % 3 == 1)
                op_simclose();
            else if (k == 27)
                op_malformed();
            else if (k < 30)
                op_time();
            else if (k < 31)
                op_tables();
            else
                op_settings();
            fz_run_work();
            EXPECT(harness_locks_held() == 0);
            EXPECT(fz_sock_wake_refs() == 0);
        }
        teardown(baseline);
        note(60, h);
    }
    EXPECT(g_bad_frames == 0);
    printf("  tcp digest 0x%016llx over %u histories of %u operations\n", (unsigned long long)g_digest, HISTORIES, OPS);
    EXPECT(g_digest == TCP_DIGEST);
}

/* TCP_BENCH=1: the median of nine runs of 20,000 rounds on one established connection -- an in-order 1,400-byte
 * segment from the peer and the host's read of it; the host's 1,400-byte send and the peer's ACK of it. Through the
 * whole host stack, as the C's was. */
static double ns_since(const struct timespec *t0)
{
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (double)(t1.tv_sec - t0->tv_sec) * 1e9 + (double)(t1.tv_nsec - t0->tv_nsec);
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static void tcp_bench(void)
{
    g_rng = SEED;
    fz_net_init();
    size_t baseline = fz_allocs_live();
    setup(0);
    g_benching = true;
    struct conn *c = &g_conns[0];
    struct tcp_pcb *pcb = tcp_pcb_new(COSMO_AF_INET);
    c->peer = 0;
    c->peer_port = 7000;
    own(c, pcb);
    pcb->sock = &c->sock;
    struct netaddr a;
    memset(&a, 0, sizeof(a));
    a.family = COSMO_AF_INET;
    EXPECT(tcp_bind(pcb, &a) == 0);
    a.port = 7000;
    a.v4 = peer_ip(0);
    EXPECT(tcp_connect(pcb, &a) == 0);
    c->host_port = pcb->local.port;
    uint32_t seq, ack;
    uint8_t flags;
    EXPECT(last_to(0, 7000, &seq, &ack, &flags));
    segment(0, 7000, c->host_port, 1000, seq + 1, TH_SYN | TH_ACK, 65535, NULL, 0, NULL, 0, false);
    EXPECT(tcp_state_of(pcb) == TCP_ESTABLISHED);
    static uint8_t data[1400];
    memset(data, 'd', sizeof(data));
    double in[9], out[9];
    for (unsigned t = 0; t < 9; t++) {
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 20000; i++) {
            segment(0, 7000, c->host_port, pcb->rcv_nxt, pcb->snd_nxt, TH_ACK | TH_PSH, 65535, NULL, 0, data, sizeof(data),
                    false);
            bool eof = false;
            (void)tcp_recv(pcb, g_buf, sizeof(g_buf), &eof);
            fz_run_work();
        }
        in[t] = ns_since(&t0) / 20000;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 20000; i++) {
            (void)tcp_send(pcb, data, sizeof(data));
            segment(0, 7000, c->host_port, pcb->rcv_nxt, pcb->snd_nxt, TH_ACK, 65535, NULL, 0, NULL, 0, false);
            fz_run_work();
        }
        out[t] = ns_since(&t0) / 20000;
    }
    qsort(in, 9, sizeof(double), cmp_double);
    qsort(out, 9, sizeof(double), cmp_double);
    teardown(baseline);
    g_benching = false;
    printf("  bench a segment received and read %.1f ns, a segment sent and acknowledged %.1f ns\n", in[4], out[4]);
}

static const struct host_test tests[] = {
    { "tcp digest", test_digest },
};

int main(void)
{
    int rc = harness_run(tests, sizeof(tests) / sizeof(tests[0]));
    if (getenv("TCP_BENCH") != NULL)
        tcp_bench();
    return rc;
}
