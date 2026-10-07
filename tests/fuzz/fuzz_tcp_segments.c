/*
 * fuzz_tcp_segments.c - A sequence of segments into one TCP connection.
 *
 * The real TCP (tcp.c, with the IPv4 and Ethernet layers under it) on the
 * host (shim_net.c). Before each input a peer at 10.0.2.2:40000 opens a
 * connection to this host's listener on :80 and it is accepted, so the
 * input starts on an ESTABLISHED connection. The input is a program over
 * that connection, one byte of opcode then operands:
 *
 *   0  a segment from the peer: flags(1), seq offset(2, signed, from what
 *      the connection expects), ack offset(2, signed, from the host's
 *      snd_nxt), window(2), option length(1, up to 40) and the options,
 *      payload length(2, up to 1400) and the payload (zero past the input)
 *   1  the host sends: length(2, up to 4096)
 *   2  the host reads: length(2)
 *   3  time passes: ms(2) -- delayed ACKs, retransmissions, keepalives,
 *      TIME_WAIT run
 *   4  the host shuts its write side
 *   5  the host closes (the connection is then the network's to end)
 *   6  a segment as in 0 with its checksum then corrupted: the oracle is
 *      that the connection does not move
 *   7  ICMP fragmentation-needed quoting the host's last segment: mtu(2)
 *   8  a raw TCP segment: length(2) and the bytes, addressed to the
 *      connection, checksum fixed, everything else as given
 *   9  the next pending timer fires, whatever the time
 *
 * Oracles: every state change passes through tcp_test_state_change
 * (tcp.c under TCP_HOST_TEST) and must be an arc of the documented machine
 * (RFC 793 figure 6, plus this implementation's completion of a passive
 * open from the SYN cache straight into ESTABLISHED, and any state's abort
 * to CLOSED); a segment with a bad checksum changes nothing (bad_cksum
 * counts it, state and sequence variables stay); after every operation
 * snd_una <= snd_nxt <= snd_max, the receive buffer within its size, the
 * out-of-order queue within its count, every lock released, every
 * transmitted frame well-formed with correct checksums; after teardown no
 * mbuf alive and the allocator back at its baseline.
 */

#include <kernel/net/cksum.h>
#include <kernel/net/ether.h>
#include <kernel/net/inet.h>
#include <kernel/net/ip.h>
#include <kernel/net/tcp.h>
#include <kernel/socket.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fuzz.h"
#include "harness.h"
#include "netpkt.h"
#include "shim_net.h"

static const uint8_t k_mac0[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
static const uint8_t k_peer_mac[6] = { 0x52, 0x55, 0x0a, 0x00, 0x02, 0x02 };
#define IP0      IPV4_ADDR(10, 0, 2, 15)
#define PEER     IPV4_ADDR(10, 0, 2, 2)
#define MASK24   htonl(0xffffff00u)
#define PEER_PORT 40000u
#define PEER_ISN  5000u

static struct fz_netif g_f0;
static size_t g_baseline;
static unsigned g_bad_csum_frames, g_bad_len_frames;
static struct tcp_pcb *g_listener, *g_conn;
static bool g_closed;
/* What the peer should send next, read from the pcb while the host owns it
 * and kept from the last reading once it is closed (the network may free a
 * closed pcb at any time: the host never looks at it again). */
static uint32_t g_peer_seq, g_peer_ack;

static void note_expectations(void)
{
    if (g_conn && !g_closed) {
        g_peer_seq = g_conn->rcv_nxt;
        g_peer_ack = g_conn->snd_nxt;
    }
}
static struct socket g_sock_listener, g_sock_conn;
static const struct kobject_type g_sock_type = { .name = "fz-socket" };

/* The host's last transmitted segment, for the fragmentation-needed quote. */
static uint8_t g_last_tx[FZ_CAPTURE_MAX];
static uint32_t g_last_tx_len;

/* --- the state-machine oracle ---------------------------------------------- */

static const bool k_arc[TCP_TIME_WAIT + 1][TCP_TIME_WAIT + 1] = {
    /* from CLOSED: a listen, an active open, and the SYN cache's completion of a passive open */
    [TCP_CLOSED] = { [TCP_LISTEN] = true, [TCP_SYN_SENT] = true, [TCP_ESTABLISHED] = true },
    [TCP_LISTEN] = { [TCP_CLOSED] = true },
    [TCP_SYN_SENT] = { [TCP_SYN_RCVD] = true, [TCP_ESTABLISHED] = true, [TCP_CLOSED] = true },
    [TCP_SYN_RCVD] = { [TCP_ESTABLISHED] = true, [TCP_FIN_WAIT_1] = true, [TCP_CLOSED] = true },
    [TCP_ESTABLISHED] = { [TCP_FIN_WAIT_1] = true, [TCP_CLOSE_WAIT] = true, [TCP_CLOSED] = true },
    [TCP_FIN_WAIT_1] = { [TCP_FIN_WAIT_2] = true, [TCP_CLOSING] = true, [TCP_TIME_WAIT] = true, [TCP_CLOSED] = true },
    [TCP_FIN_WAIT_2] = { [TCP_TIME_WAIT] = true, [TCP_CLOSED] = true },
    [TCP_CLOSE_WAIT] = { [TCP_LAST_ACK] = true, [TCP_CLOSED] = true },
    [TCP_CLOSING] = { [TCP_TIME_WAIT] = true, [TCP_CLOSED] = true },
    [TCP_LAST_ACK] = { [TCP_CLOSED] = true },
    [TCP_TIME_WAIT] = { [TCP_CLOSED] = true },
};

static struct { uint8_t from, to; } g_log[64];
static unsigned g_nlog;

void tcp_test_state_change(struct tcp_pcb *pcb, enum tcp_state from, enum tcp_state to)
{
    (void)pcb;
    if (g_nlog < 64) {
        g_log[g_nlog].from = (uint8_t)from;
        g_log[g_nlog].to = (uint8_t)to;
        g_nlog++;
    }
    if (from == to || ((unsigned)from <= TCP_TIME_WAIT && (unsigned)to <= TCP_TIME_WAIT && k_arc[from][to]))
        return;
    fprintf(stderr, "fuzz_tcp_segments: a state change off the machine: %s -> %s; the connection's history:\n",
            tcp_state_name(from), tcp_state_name(to));
    for (unsigned i = 0; i < g_nlog; i++)
        fprintf(stderr, "  %s -> %s\n", tcp_state_name((enum tcp_state)g_log[i].from),
                tcp_state_name((enum tcp_state)g_log[i].to));
    FUZZ_ASSERT(!"TCP state change off the documented machine");
}

/* --- the frame observer -------------------------------------------------- */

static void on_frame(struct fz_netif *f, const uint8_t *frame, uint32_t len)
{
    (void)f;
    int rc = np_check_checksums(frame, len);
    if (rc == 4)
        g_bad_len_frames++;
    else if (rc != 0)
        g_bad_csum_frames++;
    if (len >= NP_ETH + NP_IPV4 + NP_TCP && np_get16(frame + 12) == ETH_P_IP && frame[NP_ETH + 9] == IPPROTO_TCP) {
        g_last_tx_len = len;
        memcpy(g_last_tx, frame, len);
    }
}

static bool seq_leq(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) <= 0;
}

static void check_invariants(void)
{
    FUZZ_ASSERT(harness_locks_held() == 0);
    FUZZ_ASSERT(g_bad_csum_frames == 0);
    FUZZ_ASSERT(g_bad_len_frames == 0);
    FUZZ_ASSERT(g_f0.oversize == 0 && g_f0.runts == 0);
    if (g_conn != NULL && !g_closed) {
        FUZZ_ASSERT(seq_leq(g_conn->snd_una, g_conn->snd_nxt));
        FUZZ_ASSERT(seq_leq(g_conn->snd_nxt, g_conn->snd_max));
        FUZZ_ASSERT(g_conn->rcvbuf.len <= TCP_RCVBUF);
        FUZZ_ASSERT(g_conn->sndbuf.len <= TCP_SNDBUF);
        FUZZ_ASSERT(g_conn->ooo_n <= TCP_OOO_MAX);
    }
}

/* --- setup and teardown -------------------------------------------------- */

static void deliver(const uint8_t *frame, size_t len)
{
    uint8_t buf[FZ_CAPTURE_MAX];
    if (len > sizeof(buf))
        len = sizeof(buf);
    memcpy(buf, frame, len);
    fz_deliver(&g_f0, buf, (uint32_t)len);
}

static void setup(void)
{
    fz_random_seed(0x7cb);
    g_bad_csum_frames = g_bad_len_frames = 0;
    g_nlog = 0;
    g_last_tx_len = 0;
    fz_netif_register(&g_f0, "fz0", k_mac0, IP0, MASK24, PEER, 0);
    g_f0.on_frame = on_frame;
    uint8_t f[128];
    size_t n = np_frame_arp(f, eth_broadcast, k_peer_mac, 1, k_peer_mac, PEER, eth_broadcast, IP0);
    deliver(f, n);

    g_listener = tcp_pcb_new(COSMO_AF_INET);
    FUZZ_ASSERT(g_listener != NULL);
    memset(&g_sock_listener, 0, sizeof(g_sock_listener));
    kobject_init(&g_sock_listener.obj, &g_sock_type);
    g_sock_listener.tcp = g_listener;
    g_listener->sock = &g_sock_listener;
    struct netaddr a;
    memset(&a, 0, sizeof(a));
    a.family = COSMO_AF_INET;
    a.port = 80;
    FUZZ_ASSERT(tcp_bind(g_listener, &a) == 0);
    FUZZ_ASSERT(tcp_listen(g_listener, 4) == 0);

    uint8_t mss[4] = { 2, 4, 0x05, 0xb4 };
    n = np_frame_tcp4(f, k_mac0, k_peer_mac, PEER, IP0, PEER_PORT, 80, PEER_ISN, 0, TH_SYN, 65535, mss, 4, NULL, 0);
    deliver(f, n);
    FUZZ_ASSERT(g_last_tx_len >= NP_ETH + NP_IPV4 + NP_TCP);
    FUZZ_ASSERT((g_last_tx[NP_ETH + NP_IPV4 + 13] & (TH_SYN | TH_ACK)) == (TH_SYN | TH_ACK));
    uint32_t their_seq = np_get32(g_last_tx + NP_ETH + NP_IPV4 + 4);
    n = np_frame_tcp4(f, k_mac0, k_peer_mac, PEER, IP0, PEER_PORT, 80, PEER_ISN + 1, their_seq + 1, TH_ACK, 65535, NULL,
                      0, NULL, 0);
    deliver(f, n);
    memset(&g_sock_conn, 0, sizeof(g_sock_conn));
    kobject_init(&g_sock_conn.obj, &g_sock_type);
    g_conn = tcp_accept(g_listener, &g_sock_conn);
    FUZZ_ASSERT(g_conn != NULL);
    g_sock_conn.tcp = g_conn;
    FUZZ_ASSERT(tcp_state_of(g_conn) == TCP_ESTABLISHED);
    g_closed = false;
    note_expectations();
}

static void teardown(void)
{
    if (!g_closed)
        tcp_close(g_conn);
    g_conn = NULL;
    tcp_close(g_listener);
    g_listener = NULL;
    for (unsigned i = 0; i < 900 && fz_allocs_live() != g_baseline; i++)
        fz_clock_advance(1000000000ull);
    fz_netif_unregister_all();
    check_invariants();
    struct mbuf_stats ms;
    mbuf_get_stats(&ms);
    FUZZ_ASSERT(ms.mbufs_alive == 0 && ms.clusters_alive == 0);
    if (fz_allocs_live() != g_baseline) {
        fprintf(stderr, "fuzz_tcp_segments: %zu object(s) still allocated after teardown (baseline %zu); %u timer(s) pending;"
                        " the connection's history:\n", fz_allocs_live(), g_baseline, fz_timers_pending());
        for (unsigned i = 0; i < g_nlog; i++)
            fprintf(stderr, "  %s -> %s\n", tcp_state_name((enum tcp_state)g_log[i].from),
                    tcp_state_name((enum tcp_state)g_log[i].to));
        fz_dump_live();
        fz_dump_timers();
        struct tcp_pcb *leaked = fz_live_object_of_size(sizeof(struct tcp_pcb));
        if (leaked) {
            fprintf(stderr, "  the pcb: state %s refs %u in table %d; snd_una %u snd_nxt %u snd_max %u sndbuf %u; rcv_nxt %u"
                            " rcvbuf %u; fin_queued %d fin_sent %d fin_rcvd %d; rexmit_count %u rto %llu ms; work_flags %#x;"
                            " timers rexmit %p delack %p timewait %p keep %p\n",
                    tcp_state_name(leaked->state), leaked->refs, !list_empty(&leaked->hash_link), leaked->snd_una,
                    leaked->snd_nxt, leaked->snd_max, leaked->sndbuf.len, leaked->rcv_nxt, leaked->rcvbuf.len,
                    leaked->fin_queued, leaked->fin_sent, leaked->fin_rcvd, leaked->rexmit_count,
                    (unsigned long long)(leaked->rto_ns / 1000000ull), leaked->work_flags, (void *)&leaked->rexmit,
                    (void *)&leaked->delack, (void *)&leaked->timewait, (void *)&leaked->keep);
        }
        FUZZ_ASSERT(fz_allocs_live() == g_baseline);
    }
}

/* --- the program --------------------------------------------------------- */

struct cursor {
    const uint8_t *p;
    size_t n;
};

static uint8_t take8(struct cursor *c)
{
    if (c->n == 0)
        return 0;
    c->n--;
    return *c->p++;
}

static uint16_t take16(struct cursor *c)
{
    uint16_t lo = take8(c);
    return (uint16_t)(lo | (take8(c) << 8));
}

static size_t take_bytes(struct cursor *c, uint8_t *dst, size_t want)
{
    size_t have = want < c->n ? want : c->n;
    memcpy(dst, c->p, have);
    memset(dst + have, 0, want - have);
    c->p += have;
    c->n -= have;
    return want;
}

/* A segment from the peer, relative to the connection's expectations. */
static void peer_segment(struct cursor *c, bool corrupt)
{
    uint8_t flags = take8(c);
    int16_t seqoff = (int16_t)take16(c);
    int16_t ackoff = (int16_t)take16(c);
    uint16_t win = take16(c);
    uint8_t optlen = take8(c);
    if (optlen > 40)
        optlen = 40;
    uint8_t opts[40];
    take_bytes(c, opts, optlen);
    uint16_t paylen = take16(c);
    if (paylen > 1400)
        paylen = 1400;
    uint8_t payload[1400];
    take_bytes(c, payload, paylen);
    note_expectations();
    uint32_t seq = g_peer_seq + (uint32_t)(int32_t)seqoff;
    uint32_t ack = g_peer_ack + (uint32_t)(int32_t)ackoff;
    uint8_t f[NP_ETH + NP_IPV4 + 60 + 1400];
    size_t n = np_frame_tcp4(f, k_mac0, k_peer_mac, PEER, IP0, PEER_PORT, 80, seq, ack, flags, win, opts, optlen, payload,
                             paylen);
    if (!corrupt || g_closed) {
        deliver(f, n);
        return;
    }
    struct tcp_stats before, after;
    tcp_get_stats(&before);
    enum tcp_state st = g_conn->state;
    uint32_t rcv_nxt = g_conn->rcv_nxt, snd_una = g_conn->snd_una, snd_nxt = g_conn->snd_nxt;
    uint32_t rcvlen = g_conn->rcvbuf.len;
    unsigned ooo = g_conn->ooo_n;
    f[NP_ETH + NP_IPV4 + 16] ^= 0x5a;   /* the checksum field */
    unsigned tx = g_f0.transmits;
    deliver(f, n);
    tcp_get_stats(&after);
    FUZZ_ASSERT(after.bad_cksum == before.bad_cksum + 1);
    FUZZ_ASSERT(g_conn->state == st);
    FUZZ_ASSERT(g_conn->rcv_nxt == rcv_nxt && g_conn->snd_una == snd_una && g_conn->snd_nxt == snd_nxt);
    FUZZ_ASSERT(g_conn->rcvbuf.len == rcvlen && g_conn->ooo_n == ooo);
    FUZZ_ASSERT(g_f0.transmits == tx);   /* and it answers nothing */
}

static void needfrag(struct cursor *c)
{
    uint16_t mtu = take16(c);
    if (g_last_tx_len < NP_ETH + NP_IPV4 + 8)
        return;
    /* Quote the host's last segment: its IP header and the first 8 bytes. */
    uint8_t inner[60 + 8];
    const uint8_t *ip = g_last_tx + NP_ETH;
    unsigned ihl = (unsigned)(ip[0] & 0xf) * 4u;
    if (ihl < NP_IPV4 || NP_ETH + ihl + 8 > g_last_tx_len)
        return;
    memcpy(inner, ip, ihl + 8);
    uint8_t f[NP_ETH + NP_IPV4 + 8 + 68];
    size_t n = np_frame_icmp4(f, k_mac0, k_peer_mac, PEER, IP0, ICMP_DEST_UNREACH, ICMP_UNREACH_NEEDFRAG, 0, mtu, inner,
                              ihl + 8);
    deliver(f, n);
}

static void raw_segment(struct cursor *c)
{
    uint16_t len = take16(c);
    if (len > 1460)
        len = 1460;
    uint8_t seg[1460];
    take_bytes(c, seg, len);
    if (len < NP_TCP)
        return;
    uint8_t f[NP_ETH + NP_IPV4 + 1460];
    size_t o = np_eth(f, k_mac0, k_peer_mac, ETH_P_IP);
    memcpy(f + o + NP_IPV4, seg, len);
    np_put16(f + o + NP_IPV4, PEER_PORT);
    np_put16(f + o + NP_IPV4 + 2, 80);
    np_ipv4(f + o, PEER, IP0, IPPROTO_TCP, len, 64);
    np_l4_cksum4(f + o + NP_IPV4, len, PEER, IP0, IPPROTO_TCP, 16);
    deliver(f, o + NP_IPV4 + len);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fz_net_init();
    if (g_baseline == 0)
        g_baseline = fz_allocs_live();
    setup();
    struct cursor c = { data, size };
    unsigned ops = 0;
    while (c.n > 0 && ops++ < 256) {
        uint8_t op = take8(&c);
        switch (op) {
        case 0:
            peer_segment(&c, false);
            break;
        case 1: {
            uint16_t len = take16(&c);
            if (len > 4096)
                len = 4096;
            static uint8_t buf[4096];
            memset(buf, 'd', sizeof(buf));
            if (!g_closed)
                tcp_send(g_conn, buf, len);
            break;
        }
        case 2: {
            uint16_t len = take16(&c);
            if (len > 4096)
                len = 4096;
            static uint8_t buf[4096];
            bool eof = false;
            if (!g_closed)
                tcp_recv(g_conn, buf, len, &eof);
            break;
        }
        case 3:
            fz_clock_advance((uint64_t)take16(&c) * 1000000ull);
            break;
        case 4:
            if (!g_closed)
                tcp_shutdown_write(g_conn);
            break;
        case 5:
            if (!g_closed) {
                tcp_close(g_conn);
                g_closed = true;
            }
            break;
        case 6:
            peer_segment(&c, true);
            break;
        case 7:
            needfrag(&c);
            break;
        case 8:
            raw_segment(&c);
            break;
        case 9:
            fz_fire_until(600ull * 1000000000ull, 1);
            break;
        default:
            break;   /* an unknown opcode is a no-op: the mutator keeps the rest of the program */
        }
        fz_run_work();
        note_expectations();
        check_invariants();
    }
    teardown();
    return 0;
}

size_t fuzz_max_len(void)
{
    return 4096;
}

/* --- seeds ---------------------------------------------------------------- */

struct prog {
    uint8_t *p;
    size_t n, cap;
};

static void emit8(struct prog *g, uint8_t v)
{
    if (g->n < g->cap)
        g->p[g->n++] = v;
}

static void emit16(struct prog *g, uint16_t v)
{
    emit8(g, (uint8_t)v);
    emit8(g, (uint8_t)(v >> 8));
}

static void seg(struct prog *g, uint8_t op, uint8_t flags, int16_t seqoff, int16_t ackoff, uint16_t win,
                const uint8_t *opts, uint8_t optlen, uint16_t paylen)
{
    emit8(g, op);
    emit8(g, flags);
    emit16(g, (uint16_t)seqoff);
    emit16(g, (uint16_t)ackoff);
    emit16(g, win);
    emit8(g, optlen);
    for (unsigned i = 0; i < optlen; i++)
        emit8(g, opts[i]);
    emit16(g, paylen);
    for (unsigned i = 0; i < paylen; i++)
        emit8(g, (uint8_t)('a' + i % 26));
}

size_t fuzz_seed(unsigned i, uint8_t *buf, size_t cap)
{
    struct prog g = { buf, 0, cap };
    static const uint8_t sack_permitted[2] = { 4, 2 };
    static const uint8_t wscale[3] = { 3, 3, 7 };
    static const uint8_t nop_eol[4] = { 1, 1, 0, 0 };
    switch (i) {
    case 0:   /* data in order, the host reads and answers */
        seg(&g, 0, TH_PSH | TH_ACK, 0, 0, 65535, NULL, 0, 100);
        emit8(&g, 2); emit16(&g, 100);
        emit8(&g, 1); emit16(&g, 200);
        seg(&g, 0, TH_ACK, 0, 200, 65535, NULL, 0, 0);
        break;
    case 1:   /* a gap, then the segment that fills it */
        seg(&g, 0, TH_ACK, 100, 0, 65535, NULL, 0, 100);
        seg(&g, 0, TH_ACK, 0, 0, 65535, NULL, 0, 100);
        emit8(&g, 2); emit16(&g, 300);
        break;
    case 2:   /* the peer closes; the host reads EOF and closes */
        seg(&g, 0, TH_FIN | TH_ACK, 0, 0, 65535, NULL, 0, 0);
        emit8(&g, 2); emit16(&g, 10);
        emit8(&g, 5);
        seg(&g, 0, TH_ACK, 1, 1, 65535, NULL, 0, 0);
        emit8(&g, 3); emit16(&g, 5000);
        break;
    case 3:   /* the host closes first; the peer acknowledges and closes */
        emit8(&g, 1); emit16(&g, 50);
        emit8(&g, 5);
        seg(&g, 0, TH_ACK, 0, 51, 65535, NULL, 0, 0);
        seg(&g, 0, TH_FIN | TH_ACK, 0, 51, 65535, NULL, 0, 0);
        emit8(&g, 3); emit16(&g, 3000);
        break;
    case 4:   /* a reset */
        seg(&g, 0, TH_RST, 0, 0, 0, NULL, 0, 0);
        break;
    case 5:   /* a reset out of the window: a challenge ACK, not an end */
        seg(&g, 0, TH_RST, 1000, 0, 0, NULL, 0, 0);
        seg(&g, 0, TH_ACK, 0, 0, 65535, NULL, 0, 10);
        break;
    case 6:   /* a bad checksum changes nothing */
        seg(&g, 6, TH_PSH | TH_ACK, 0, 0, 65535, NULL, 0, 100);
        seg(&g, 6, TH_FIN | TH_ACK, 0, 0, 65535, NULL, 0, 0);
        break;
    case 7:   /* options on a data segment */
        seg(&g, 0, TH_ACK, 0, 0, 65535, sack_permitted, 2, 10);
        seg(&g, 0, TH_ACK, 0, 0, 65535, wscale, 3, 10);
        seg(&g, 0, TH_ACK, 0, 0, 65535, nop_eol, 4, 10);
        break;
    case 8:   /* the window closes, the host has data, the probe timer runs */
        seg(&g, 0, TH_ACK, 0, 0, 0, NULL, 0, 0);
        emit8(&g, 1); emit16(&g, 2000);
        emit8(&g, 3); emit16(&g, 1000);
        emit8(&g, 9);
        emit8(&g, 9);
        seg(&g, 0, TH_ACK, 0, 0, 65535, NULL, 0, 0);
        break;
    case 9:   /* unacknowledged data: retransmissions until the connection gives up */
        emit8(&g, 1); emit16(&g, 1000);
        for (unsigned k = 0; k < 12; k++)
            emit8(&g, 9);
        break;
    case 10:  /* fragmentation needed against the host's segment */
        emit8(&g, 1); emit16(&g, 3000);
        emit8(&g, 7); emit16(&g, 576);
        emit8(&g, 9);
        break;
    case 11:  /* a SYN on the established connection, and a FIN with data beyond the window */
        seg(&g, 0, TH_SYN, 0, 0, 65535, NULL, 0, 0);
        seg(&g, 0, TH_FIN | TH_ACK, 32000, 0, 65535, NULL, 0, 100);
        break;
    case 12:  /* simultaneous close */
        emit8(&g, 5);
        seg(&g, 0, TH_FIN | TH_ACK, 0, 0, 65535, NULL, 0, 0);
        seg(&g, 0, TH_ACK, 1, 1, 65535, NULL, 0, 0);
        emit8(&g, 3); emit16(&g, 5000);
        break;
    case 13:  /* keepalive: a long idle, probes, then the peer answers */
        for (unsigned k = 0; k < 8; k++) {
            emit8(&g, 3); emit16(&g, 60000);
        }
        seg(&g, 0, TH_ACK, 0, 0, 65535, NULL, 0, 0);
        break;
    case 14: { /* a raw segment with every flag */
        emit8(&g, 8);
        emit16(&g, 24);
        uint8_t raw[24] = { 0 };
        raw[12] = 0x60;
        raw[13] = 0xff;
        raw[20] = 2; raw[21] = 4; raw[22] = 2; raw[23] = 0;
        for (unsigned k = 0; k < 24; k++)
            emit8(&g, raw[k]);
        break;
    }
    case 15:  /* the host shuts its write side, the peer keeps sending */
        emit8(&g, 4);
        seg(&g, 0, TH_ACK, 0, 1, 65535, NULL, 0, 100);
        emit8(&g, 2); emit16(&g, 100);
        seg(&g, 0, TH_FIN | TH_ACK, 0, 0, 65535, NULL, 0, 0);
        emit8(&g, 3); emit16(&g, 5000);
        break;
    default:
        return 0;
    }
    return g.n;
}
