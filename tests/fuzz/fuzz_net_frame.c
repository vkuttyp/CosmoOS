/*
 * fuzz_net_frame.c - Ethernet frames into the whole receive path.
 *
 * The real stack (ether.c through tcp.c, nat.c and fw.c) on the host
 * (shim_net.c), with a topology like a booted machine's: an uplink `fz0`
 * (10.0.2.15/24, peer and gateway 10.0.2.2) and a guest tap `fz1`
 * (10.75.0.1/24, forwarding and masquerading for the guest 10.75.0.15, with
 * the firewall's guest attached, one TO_UPLINK drop rule and one TCP port
 * forward). Before each input: a UDP socket on :7, a UDP socket connected
 * to the peer, a TCP listener on :80, a connection the peer opened to it
 * and accepted (ESTABLISHED), and a connection this host is opening to
 * 10.0.2.3:9 (SYN_SENT, its SYN parked on an incomplete ARP entry). And
 * the same over IPv6 on fz0's link-local address: the peer fe80::1 learnt
 * from its neighbour solicitation, a UDP socket on :7, one connected to
 * the peer's :53, a TCP listener on :80 and a connection from the peer,
 * accepted.
 *
 * The input is a sequence of records: a control byte, a little-endian
 * 16-bit length, the frame. Control bits:
 *   0  deliver on fz1 (the guest side) instead of fz0
 *   1  recompute the IPv4 and transport checksums first
 *   2  retarget a TCP frame to the established connection: the peer's
 *      addresses and ports, seq/ack at what the connection expects
 *   3  retarget to the half-open connection this host is opening
 *   4  advance the clock 250 ms first (delayed ACKs, retransmits, ARP
 *      retries fire)
 *   5  after delivery the host acts: sends 100 bytes on the connection,
 *      drains what it received, drains the UDP sockets
 *   6  the host closes the established connection (once)
 *   7  the host shuts its write side (once)
 *
 * A record whose length has its top bit (0x8000) set is a host action, not
 * a frame: the control byte names it (modulo the count), the low 15 bits
 * are the length of its argument bytes. A UDP send (socket: unconnected
 * v4, connected v4, unconnected v6, connected v6, or a fresh one --
 * unbound v4 or v6 (an ephemeral port), bound to a port in use, bound to an
 * address not this host's; destination: one of
 * sixteen -- resolved, unresolved, routed, broadcast, multicast, own,
 * loopback, unroutable -- port and length from the arguments), the
 * transmit rings full or not (-ENOBUFS), a segment from the IPv6 peer
 * at what the connection expects, the host's acts on the IPv6 connection,
 * time passing, a neighbour advertisement or solicitation.
 *
 * Oracles, checked after every frame and at the end of the input: every
 * spinlock released; every frame the stack transmitted carries correct
 * checksums, is at least 60 bytes and at most the MTU plus the Ethernet
 * header; the ARP and ND tables never exceed their sizes; the NAT and
 * firewall flow tables never exceed theirs; every unicast IPv4 or IPv6
 * frame sent goes to the MAC its interface's ARP or ND table holds for
 * the next hop, reachable, on that interface (N25; neighbour discovery's
 * own answers excepted: they go to the asker's option); a UDP send whose
 * datagram the ring refused returns -ENOBUFS, and one that returns
 * -ENOBUFS had its datagram refused. After teardown (sockets
 * closed, every timer run to quiescence): no mbuf alive, the allocator's
 * live count back to its baseline (no pcb, SYN-cache entry or buffer
 * leaked), the neighbour tables empty once each interface is flushed
 * (every entry named a registered interface: N25), the NAT and firewall
 * tables empty once flushed.
 */

#include <kernel/net/cksum.h>
#include <kernel/net/ether.h>
#include <kernel/net/fw.h>
#include <kernel/net/inet.h>
#include <kernel/net/ip.h>
#include <kernel/net/nat.h>
#include <kernel/net/tcp.h>
#include <kernel/net/udp.h>
#include <kernel/socket.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fuzz.h"
#include "harness.h"
#include "netpkt.h"
#include "shim_net.h"

#define CTL_GUEST     (1u << 0)
#define CTL_FIX       (1u << 1)
#define CTL_TO_CONN   (1u << 2)
#define CTL_TO_HALF   (1u << 3)
#define CTL_ADVANCE   (1u << 4)
#define CTL_HOST_ACTS (1u << 5)
#define CTL_CLOSE     (1u << 6)
#define CTL_SHUT_WR   (1u << 7)

static const uint8_t k_mac0[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };   /* fz0 */
static const uint8_t k_mac1[6] = { 0x02, 0xf2, 0x00, 0x00, 0x00, 0x01 };   /* fz1 */
static const uint8_t k_peer_mac[6] = { 0x52, 0x55, 0x0a, 0x00, 0x02, 0x02 };
static const uint8_t k_guest_mac[6] = { 0x02, 0xa1, 0x00, 0x00, 0x00, 0x0f };

#define IP0      IPV4_ADDR(10, 0, 2, 15)
#define PEER     IPV4_ADDR(10, 0, 2, 2)
#define FAR      IPV4_ADDR(10, 0, 2, 3)
#define IP1      IPV4_ADDR(10, 75, 0, 1)
#define GUEST    IPV4_ADDR(10, 75, 0, 15)
#define MASK24   htonl(0xffffff00u)
#define PEER_PORT 40000u
#define OUR_ISN   1000u

static struct fz_netif g_f0, g_f1;
static size_t g_baseline;
static unsigned g_bad_csum_frames, g_bad_len_frames;

static struct udp_pcb g_udp7, g_udpc, g_udp6, g_udpc6;
static struct tcp_pcb *g_listener, *g_conn, *g_half, *g_listener6, *g_conn6;
static bool g_conn_closed;
static struct in6_addr g_ll0, g_ll1, g_peer6;   /* fz0's and fz1's link-local addresses; the IPv6 peer */
static unsigned g_bad_neigh_frames;

/*
 * Every TCP pcb a caller closes has an owner: the socket layer attaches
 * one at creation (ksock_create) and at accept (tcp_accept takes it), and
 * the pcb's end-of-connection paths read it to decide between retiring the
 * pcb under its live socket and killing an ownerless one -- a queued child
 * nobody accepted. A pcb killed by the network and then closed by a caller
 * would be put once too often, so this target owns its pcbs the way the
 * socket layer does: a socket object per pcb, released with it.
 */
static struct socket g_sock_listener, g_sock_conn, g_sock_half, g_sock_listener6, g_sock_conn6;
static const struct kobject_type g_sock_type = { .name = "fz-socket" };

static void own(struct socket *s, struct tcp_pcb *pcb)
{
    memset(s, 0, sizeof(*s));
    kobject_init(&s->obj, &g_sock_type);
    s->family = COSMO_AF_INET;
    s->tcp = pcb;
    pcb->sock = s;
}

static void trace(const char *fmt, ...)
{
    static int on = -1;
    if (on < 0)
        on = getenv("FZ_TRACE") != NULL;
    if (!on)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void hexdump(const char *what, const uint8_t *p, uint32_t n)
{
    fprintf(stderr, "fuzz_net_frame: %s (%u bytes):", what, n);
    for (uint32_t i = 0; i < n && i < 96; i++)
        fprintf(stderr, "%s%02x", i % 16 == 0 ? "\n  " : " ", p[i]);
    fprintf(stderr, "\n");
}

/*
 * Recent deliveries, for telling a forwarded frame from one the stack built:
 * a forwarded datagram is the guest's, relayed with its TTL down and
 * (masqueraded) its source rewritten -- and possibly later, once ARP has
 * resolved the next hop -- and its transport header is whatever the guest
 * sent. A UDP length field disagreeing with the IP length is relayed as it
 * came: a router does not police transport headers (Linux's conntrack marks
 * such a datagram INVALID and skips NAT; this stack masquerades it; the
 * report records the difference). The oracle holds a forwarded frame to the
 * IP header checksum alone; the stack's own frames must be well-formed
 * throughout. A delivery is remembered by its destination, protocol, payload
 * length and destination port, none of which forwarding changes (the IP
 * total does: the forwarder writes a fresh 20-byte header, so a datagram
 * that arrived with options leaves shorter by their length).
 */
struct seen_ip {
    uint32_t dst;
    uint16_t payload, dport;
    uint8_t proto;
    bool valid;
};
static struct seen_ip g_in[16];
static unsigned g_in_next;

static bool ip_key(const uint8_t *frame, uint32_t len, struct seen_ip *k)
{
    if (len < NP_ETH + NP_IPV4 || np_get16(frame + 12) != ETH_P_IP)
        return false;
    const uint8_t *ip = frame + NP_ETH;
    unsigned ihl = (unsigned)(ip[0] & 0xf) * 4u;
    if ((ip[0] >> 4) != 4 || ihl < NP_IPV4 || NP_ETH + ihl + 4 > len)
        return false;
    memcpy(&k->dst, ip + 16, 4);
    uint16_t total = np_get16(ip + 2);
    k->payload = total >= ihl ? (uint16_t)(total - ihl) : 0;
    k->proto = ip[9];
    k->dport = np_get16(ip + ihl + 2);
    k->valid = true;
    return true;
}

static bool forwarded(const uint8_t *frame, uint32_t len)
{
    struct seen_ip k;
    if (!ip_key(frame, len, &k))
        return false;
    for (unsigned i = 0; i < 16; i++)
        if (g_in[i].valid && g_in[i].dst == k.dst && g_in[i].payload == k.payload && g_in[i].proto == k.proto &&
            g_in[i].dport == k.dport)
            return true;
    return false;
}

/*
 * The frame went to the neighbour its interface knows (N25): a unicast
 * IPv4 frame to the MAC ARP holds on that interface for the next hop (the
 * destination on the link, the gateway off it), an IPv6 one to the MAC ND
 * holds there for the destination (link-local only: no global routing).
 * Neighbour discovery's NS and NA, and ARP itself, are addressed from the
 * message they answer and are not held to it.
 */
static void check_neighbour(struct fz_netif *f, const uint8_t *frame, uint32_t len)
{
    if (len < NP_ETH || (frame[0] & 1))
        return;   /* broadcast or multicast */
    uint8_t mac[6];
    const char *what = NULL;
    uint16_t type = np_get16(frame + 12);
    if (type == ETH_P_IPV6 && len >= NP_ETH + NP_IPV6) {
        const uint8_t *ip = frame + NP_ETH;
        if (ip[6] == IPPROTO_ICMPV6 && len > NP_ETH + NP_IPV6 &&
            (ip[NP_IPV6] == ICMPV6_NS || ip[NP_IPV6] == ICMPV6_NA))
            return;
        struct in6_addr dst;
        memcpy(dst.s6_addr, ip + 24, 16);
        if (!nd_lookup(&f->nif, &dst, mac))
            what = "an IPv6 frame to a neighbour its interface's ND table does not hold reachable";
        else if (memcmp(mac, frame, 6) != 0)
            what = "an IPv6 frame to a MAC other than its interface's ND entry";
    } else if (type == ETH_P_IP && len >= NP_ETH + NP_IPV4) {
        uint32_t dst;
        memcpy(&dst, frame + NP_ETH + 16, 4);
        uint32_t hop = ((dst ^ f->nif.ip4.addr) & f->nif.ip4.mask) == 0 ? dst : f->nif.ip4.gateway;
        if (!arp_lookup(&f->nif, hop, mac))
            what = "an IPv4 frame to a next hop its interface's ARP table does not hold reachable";
        else if (memcmp(mac, frame, 6) != 0)
            what = "an IPv4 frame to a MAC other than its interface's ARP entry";
    }
    if (what == NULL)
        return;
    hexdump(what, frame, len);
    fprintf(stderr, "  out of %s\n", f->nif.name);
    g_bad_neigh_frames++;
}

static void on_frame(struct fz_netif *f, const uint8_t *frame, uint32_t len)
{
    check_neighbour(f, frame, len);
    int rc = np_check_checksums(frame, len);
    if (rc == 0)
        return;
    if (forwarded(frame, len)) {
        /* The guest's datagram relayed: only the IP header is the stack's. */
        if (rc == 1)
            g_bad_csum_frames++;
        return;
    }
    hexdump(rc == 4 ? "a malformed frame transmitted" : "a frame transmitted with a bad checksum", frame, len);
    fprintf(stderr, "  out of %s, checker code %d\n", f->nif.name, rc);
    if (rc == 4)
        g_bad_len_frames++;
    else
        g_bad_csum_frames++;
}

static void check_tables(void)
{
    struct arp_stats a;
    arp_get_stats(&a);
    FUZZ_ASSERT(a.entries <= ARP_TABLE_SIZE);
    static struct nat_flow nf[NAT_TABLE_SIZE + 1];
    FUZZ_ASSERT(nat_flow_list(nf, NAT_TABLE_SIZE + 1, fz_now()) <= NAT_TABLE_SIZE);
    static struct fw_flow_info ff[FW_FLOW_MAX + 1];
    FUZZ_ASSERT(fw_flow_list(ff, FW_FLOW_MAX + 1, fz_now()) <= FW_FLOW_MAX);
}

static void check_invariants(void)
{
    FUZZ_ASSERT(harness_locks_held() == 0);
    FUZZ_ASSERT(fz_sock_wake_refs() == 0);   /* every wake reference put after its wake */
    FUZZ_ASSERT(g_bad_csum_frames == 0);
    FUZZ_ASSERT(g_bad_len_frames == 0);
    FUZZ_ASSERT(g_bad_neigh_frames == 0);
    FUZZ_ASSERT(g_f0.oversize == 0 && g_f1.oversize == 0);
    FUZZ_ASSERT(g_f0.runts == 0 && g_f1.runts == 0);
    check_tables();
}

static void deliver(struct fz_netif *f, const uint8_t *frame, size_t len)
{
    uint8_t buf[FZ_CAPTURE_MAX];
    if (len > sizeof(buf))
        len = sizeof(buf);
    memcpy(buf, frame, len);
    if (f == &g_f1 && ip_key(buf, (uint32_t)len, &g_in[g_in_next % 16]))
        g_in_next++;   /* a guest-side datagram the stack may forward */
    fz_deliver(f, buf, (uint32_t)len);
}

/* The stack's counters, when the setup does not go as a booted machine's would. */
static void dump_stats(const char *what)
{
    struct tcp_stats t;
    struct ip_stats ip;
    struct arp_stats a;
    tcp_get_stats(&t);
    ipv4_get_stats(&ip);
    arp_get_stats(&a);
    fprintf(stderr, "fuzz_net_frame: %s\n  tcp: segs_in %llu bad_cksum %llu no_pcb %llu syn_cached %llu cookies %llu refused %llu\n"
                    "  ipv4: rx %llu bad_header %llu bad_cksum %llu not_for_us %llu offlink %llu tx %llu no_route %llu filtered %llu hin_quiet %llu\n"
                    "  arp: entries %llu requests_rcvd %llu replies_sent %llu; fz0 transmits %u\n",
            what, (unsigned long long)t.segs_in, (unsigned long long)t.bad_cksum, (unsigned long long)t.dropped_no_pcb,
            (unsigned long long)t.syn_cached, (unsigned long long)t.syn_cookies_sent, (unsigned long long)t.out_refused,
            (unsigned long long)ip.rx, (unsigned long long)ip.rx_bad_header, (unsigned long long)ip.rx_bad_cksum,
            (unsigned long long)ip.rx_not_for_us, (unsigned long long)ip.rx_offlink, (unsigned long long)ip.tx,
            (unsigned long long)ip.tx_no_route, (unsigned long long)ip.tx_filtered, (unsigned long long)ip.hin_quiet,
            (unsigned long long)a.entries, (unsigned long long)a.requests_rcvd, (unsigned long long)a.replies_sent,
            g_f0.transmits);
}

/* The peer's ARP request for us: learnt, so our replies to it go straight out. */
static void peer_arp(void)
{
    uint8_t f[64];
    size_t n = np_frame_arp(f, eth_broadcast, k_peer_mac, 1, k_peer_mac, PEER, eth_broadcast, IP0);
    deliver(&g_f0, f, n);
}

/* The three-way handshake from the peer, then accept: the established connection. */
static void establish(void)
{
    uint8_t f[128];
    uint8_t mss[4] = { 2, 4, 0x05, 0xb4 };
    size_t n = np_frame_tcp4(f, k_mac0, k_peer_mac, PEER, IP0, PEER_PORT, 80, OUR_ISN, 0, TH_SYN, 65535, mss, 4, NULL, 0);
    unsigned before = g_f0.transmits;
    deliver(&g_f0, f, n);
    if (g_f0.transmits != before + 1)
        dump_stats("the SYN drew no SYN-ACK");
    FUZZ_ASSERT(g_f0.transmits == before + 1);   /* the SYN-ACK, straight out: the peer's MAC is known */
    const uint8_t *sa = g_f0.last;
    FUZZ_ASSERT(g_f0.last_len >= NP_ETH + NP_IPV4 + NP_TCP);
    FUZZ_ASSERT((sa[NP_ETH + NP_IPV4 + 13] & (TH_SYN | TH_ACK)) == (TH_SYN | TH_ACK));
    uint32_t their_seq = np_get32(sa + NP_ETH + NP_IPV4 + 4);
    n = np_frame_tcp4(f, k_mac0, k_peer_mac, PEER, IP0, PEER_PORT, 80, OUR_ISN + 1, their_seq + 1, TH_ACK, 65535,
                      NULL, 0, NULL, 0);
    deliver(&g_f0, f, n);
    memset(&g_sock_conn, 0, sizeof(g_sock_conn));
    kobject_init(&g_sock_conn.obj, &g_sock_type);
    g_sock_conn.family = COSMO_AF_INET;
    g_conn = tcp_accept(g_listener, &g_sock_conn);   /* the socket exists first, as ksock_accept has it */
    FUZZ_ASSERT(g_conn != NULL);
    g_sock_conn.tcp = g_conn;
    FUZZ_ASSERT(tcp_state_of(g_conn) == TCP_ESTABLISHED);
    trace("accepted: refs %u listener refs %u\n", g_conn->refs, g_listener->refs);
}

static void link_local(const uint8_t mac[6], struct in6_addr *a)
{
    memset(a, 0, sizeof(*a));
    a->s6_addr[0] = 0xfe;
    a->s6_addr[1] = 0x80;
    a->s6_addr[8] = mac[0] ^ 0x02;
    a->s6_addr[9] = mac[1];
    a->s6_addr[10] = mac[2];
    a->s6_addr[11] = 0xff;
    a->s6_addr[12] = 0xfe;
    a->s6_addr[13] = mac[3];
    a->s6_addr[14] = mac[4];
    a->s6_addr[15] = mac[5];
}

/* The peer's solicitation for fz0's link-local address, carrying its own
 * MAC: learnt, so the host's IPv6 replies to it go straight out. */
static void peer_ns(void)
{
    struct in6_addr sn;
    memset(&sn, 0, sizeof(sn));
    sn.s6_addr[0] = 0xff;
    sn.s6_addr[1] = 0x02;
    sn.s6_addr[11] = 0x01;
    sn.s6_addr[12] = 0xff;
    memcpy(sn.s6_addr + 13, g_ll0.s6_addr + 13, 3);
    uint8_t sn_mac[6] = { 0x33, 0x33, 0xff, sn.s6_addr[13], sn.s6_addr[14], sn.s6_addr[15] };
    uint8_t f[128];
    size_t n = np_frame_nd(f, sn_mac, k_peer_mac, &g_peer6, &sn, ICMPV6_NS, &g_ll0, k_peer_mac);
    deliver(&g_f0, f, n);
}

/* The handshake from the IPv6 peer, then accept. */
static void establish6(void)
{
    uint8_t f[128];
    uint8_t mss[4] = { 2, 4, 0x05, 0xa0 };
    size_t n = np_frame_tcp6(f, k_mac0, k_peer_mac, &g_peer6, &g_ll0, PEER_PORT, 80, OUR_ISN, 0, TH_SYN, 65535, mss,
                             4, NULL, 0);
    unsigned before = g_f0.transmits;
    deliver(&g_f0, f, n);
    if (g_f0.transmits != before + 1)
        dump_stats("the IPv6 SYN drew no SYN-ACK");
    FUZZ_ASSERT(g_f0.transmits == before + 1);
    const uint8_t *sa = g_f0.last;
    FUZZ_ASSERT(g_f0.last_len >= NP_ETH + NP_IPV6 + NP_TCP && np_get16(sa + 12) == ETH_P_IPV6);
    FUZZ_ASSERT((sa[NP_ETH + NP_IPV6 + 13] & (TH_SYN | TH_ACK)) == (TH_SYN | TH_ACK));
    uint32_t their_seq = np_get32(sa + NP_ETH + NP_IPV6 + 4);
    n = np_frame_tcp6(f, k_mac0, k_peer_mac, &g_peer6, &g_ll0, PEER_PORT, 80, OUR_ISN + 1, their_seq + 1, TH_ACK,
                      65535, NULL, 0, NULL, 0);
    deliver(&g_f0, f, n);
    memset(&g_sock_conn6, 0, sizeof(g_sock_conn6));
    kobject_init(&g_sock_conn6.obj, &g_sock_type);
    g_sock_conn6.family = COSMO_AF_INET6;
    g_conn6 = tcp_accept(g_listener6, &g_sock_conn6);
    FUZZ_ASSERT(g_conn6 != NULL);
    g_sock_conn6.tcp = g_conn6;
    FUZZ_ASSERT(tcp_state_of(g_conn6) == TCP_ESTABLISHED);
}

static void setup(void)
{
    fz_random_seed(0x5eed);
    g_bad_csum_frames = g_bad_len_frames = g_bad_neigh_frames = 0;
    link_local(k_mac0, &g_ll0);
    link_local(k_mac1, &g_ll1);
    memset(&g_peer6, 0, sizeof(g_peer6));
    g_peer6.s6_addr[0] = 0xfe;
    g_peer6.s6_addr[1] = 0x80;
    g_peer6.s6_addr[15] = 0x01;
    memset(g_in, 0, sizeof(g_in));
    fz_netif_register(&g_f0, "fz0", k_mac0, IP0, MASK24, PEER, 0);
    fz_netif_register(&g_f1, "fz1", k_mac1, IP1, MASK24, 0, NETIF_NODEFAULT | NETIF_FORWARD | NETIF_MASQUERADE);
    g_f0.on_frame = g_f1.on_frame = on_frame;
    fw_guest_attach(GUEST, IP1);
    struct fw_rule drop25 = { .direction = FW_DIR_TO_UPLINK, .proto = IPPROTO_UDP, .dst_prefix = 0,
                              .verdict = FW_DROP, .dst_ip = 0, .dst_port = 25 };
    FUZZ_ASSERT(fw_rule_add(GUEST, 0, &drop25) == 0);
    FUZZ_ASSERT(nat_pf_add(IPPROTO_TCP, 8080, GUEST, 80) == 0);

    struct netaddr a;
    memset(&a, 0, sizeof(a));
    a.family = COSMO_AF_INET;
    a.port = 7;
    FUZZ_ASSERT(udp_pcb_init(&g_udp7, COSMO_AF_INET) == 0);
    FUZZ_ASSERT(udp_bind(&g_udp7, &a) == 0);
    a.port = 5353;
    FUZZ_ASSERT(udp_pcb_init(&g_udpc, COSMO_AF_INET) == 0);
    FUZZ_ASSERT(udp_bind(&g_udpc, &a) == 0);
    g_udpc.remote.family = COSMO_AF_INET;   /* connected to the peer's resolver, as ksock_connect records it */
    g_udpc.remote.port = 53;
    g_udpc.remote.v4 = PEER;

    g_listener = tcp_pcb_new(COSMO_AF_INET);
    FUZZ_ASSERT(g_listener != NULL);
    own(&g_sock_listener, g_listener);
    a.port = 80;
    a.v4 = 0;
    FUZZ_ASSERT(tcp_bind(g_listener, &a) == 0);
    FUZZ_ASSERT(tcp_listen(g_listener, 4) == 0);
    peer_arp();
    establish();
    g_conn_closed = false;

    g_half = tcp_pcb_new(COSMO_AF_INET);
    FUZZ_ASSERT(g_half != NULL);
    own(&g_sock_half, g_half);
    struct netaddr far;
    memset(&far, 0, sizeof(far));
    far.family = COSMO_AF_INET;
    far.port = 9;
    far.v4 = FAR;
    FUZZ_ASSERT(tcp_connect(g_half, &far) == 0);
    FUZZ_ASSERT(tcp_state_of(g_half) == TCP_SYN_SENT);

    struct netaddr a6;
    memset(&a6, 0, sizeof(a6));
    a6.family = COSMO_AF_INET6;
    a6.port = 7;
    FUZZ_ASSERT(udp_pcb_init(&g_udp6, COSMO_AF_INET6) == 0);
    FUZZ_ASSERT(udp_bind(&g_udp6, &a6) == 0);
    a6.port = 5353;
    FUZZ_ASSERT(udp_pcb_init(&g_udpc6, COSMO_AF_INET6) == 0);
    FUZZ_ASSERT(udp_bind(&g_udpc6, &a6) == 0);
    g_udpc6.remote.family = COSMO_AF_INET6;
    g_udpc6.remote.port = 53;
    g_udpc6.remote.v6 = g_peer6;
    g_listener6 = tcp_pcb_new(COSMO_AF_INET6);
    FUZZ_ASSERT(g_listener6 != NULL);
    own(&g_sock_listener6, g_listener6);
    g_sock_listener6.family = COSMO_AF_INET6;
    a6.port = 80;
    FUZZ_ASSERT(tcp_bind(g_listener6, &a6) == 0);
    FUZZ_ASSERT(tcp_listen(g_listener6, 4) == 0);
    peer_ns();
    establish6();
}

static void teardown(void)
{
    /* g_conn is NULL once the host has closed it: from then on the pcb is
     * the network's to end and free, and nothing here reads it. */
    trace("teardown: conn %p refs %u state %s; half refs %u state %s; listener refs %u queued %u\n", (void *)g_conn,
          g_conn ? g_conn->refs : 0, g_conn ? tcp_state_name(g_conn->state) : "-", g_half->refs,
          tcp_state_name(g_half->state), g_listener->refs, g_listener->nr_queued);
    if (g_conn)
        tcp_close(g_conn);
    g_conn = NULL;
    if (g_conn6)
        tcp_close(g_conn6);
    g_conn6 = NULL;
    tcp_close(g_listener6);
    g_listener6 = NULL;
    g_f0.ring_full = g_f1.ring_full = false;
    tcp_close(g_half);
    g_half = NULL;
    tcp_close(g_listener);
    g_listener = NULL;
    struct udp_pcb *udps[] = { &g_udp7, &g_udpc, &g_udp6, &g_udpc6 };
    for (unsigned i = 0; i < 4; i++) {
        udp_unbind(udps[i]);
        struct mbuf *m;
        while ((m = udp_recv(udps[i])) != NULL)
            m_freem(m);
    }
    /* Let every connection run down: FIN retransmissions give up, TIME_WAIT
     * expires, SYN-cache entries age out. The ARP sweep fires once a second
     * for ever, so "no timer pending" is never the stopping point: stop
     * when the allocator is back at its baseline, or after 15 minutes. */
    for (unsigned i = 0; i < 900 && fz_allocs_live() != g_baseline; i++)
        fz_clock_advance(1000000000ull);
    nat_flush();
    nat_pf_clear();
    fw_guest_purge(GUEST);
    fw_flush();
    fz_netif_unregister_all();
    check_invariants();
    struct mbuf_stats ms;
    mbuf_get_stats(&ms);
    FUZZ_ASSERT(ms.mbufs_alive == 0);
    FUZZ_ASSERT(ms.clusters_alive == 0);
    struct arp_stats as;
    arp_get_stats(&as);
    FUZZ_ASSERT(as.entries == 0);
    static struct nat_flow nf[1];
    FUZZ_ASSERT(nat_flow_list(nf, 1, fz_now()) == 0);
    static struct fw_flow_info ff[1];
    FUZZ_ASSERT(fw_flow_list(ff, 1, fz_now()) == 0);
    if (fz_allocs_live() != g_baseline) {
        fprintf(stderr, "fuzz_net_frame: %zu object(s) still allocated after teardown (baseline %zu)\n",
                fz_allocs_live(), g_baseline);
        FUZZ_ASSERT(fz_allocs_live() == g_baseline);
    }
}

/* Rewrite a TCP-over-IPv4 frame so it belongs to `pcb` as its peer would
 * send it: MACs, addresses, ports, and seq/ack at the expected values. */
static void retarget(uint8_t *f, size_t len, const struct tcp_pcb *pcb, const uint8_t *smac, const uint8_t *dmac)
{
    if (len < NP_ETH + NP_IPV4 + NP_TCP || np_get16(f + 12) != ETH_P_IP)
        return;
    uint8_t *ip = f + NP_ETH;
    unsigned ihl = (unsigned)(ip[0] & 0xf) * 4u;
    if ((ip[0] >> 4) != 4 || ihl < NP_IPV4 || NP_ETH + ihl + NP_TCP > len || ip[9] != IPPROTO_TCP)
        return;
    memcpy(f, dmac, 6);
    memcpy(f + 6, smac, 6);
    memcpy(ip + 12, &pcb->remote.v4, 4);
    memcpy(ip + 16, &pcb->local.v4, 4);
    uint8_t *th = ip + ihl;
    np_put16(th, pcb->remote.port);
    np_put16(th + 2, pcb->local.port);
    np_put32(th + 4, pcb->rcv_nxt);
    np_put32(th + 8, pcb->snd_nxt);
}

static void host_acts(void)
{
    if (g_conn) {
        static const uint8_t payload[100] = { 'h' };
        tcp_send(g_conn, payload, sizeof(payload));
        uint8_t buf[512];
        bool eof = false;
        while (tcp_recv(g_conn, buf, sizeof(buf), &eof) > 0)
            ;
    }
    struct mbuf *m;
    while ((m = udp_recv(&g_udp7)) != NULL)
        m_freem(m);
    while ((m = udp_recv(&g_udpc)) != NULL)
        m_freem(m);
    while ((m = udp_recv(&g_udp6)) != NULL)
        m_freem(m);
    while ((m = udp_recv(&g_udpc6)) != NULL)
        m_freem(m);
    fz_run_work();
}

/* --- host actions ------------------------------------------------------------ */

enum { ACT_UDP_SEND, ACT_RING, ACT_V6_SEG, ACT_V6_HOST, ACT_CLOCK, ACT_ND, ACT_COUNT };
#define ACT_RECORD 0x8000u

static void dest_of(unsigned i, struct netaddr *a)
{
    memset(a, 0, sizeof(*a));
    a->family = COSMO_AF_INET;
    switch (i & 15) {
    case 0: a->v4 = PEER; break;                           /* resolved */
    case 1: a->v4 = FAR; break;                            /* on the link, unresolved (the half-open's entry) */
    case 2: a->v4 = IPV4_ADDR(8, 8, 8, 8); break;          /* routed through the gateway */
    case 3: a->v4 = GUEST; break;                          /* the guest's side, unresolved there */
    case 4: a->v4 = IP0; break;                            /* this host */
    case 5: a->v4 = IPV4_ADDR(10, 0, 2, 255); break;       /* the uplink's broadcast */
    case 6: a->v4 = 0xffffffffu; break;                    /* the limited broadcast */
    case 7: a->v4 = INADDR_LOOPBACK_N; break;
    case 8: a->family = COSMO_AF_INET6; a->v6 = g_peer6; break;               /* resolved */
    case 9: a->family = COSMO_AF_INET6; a->v6 = g_peer6; a->v6.s6_addr[15] = 0x99; break;   /* unresolved */
    case 10: a->family = COSMO_AF_INET6; a->v6.s6_addr[0] = 0xff; a->v6.s6_addr[1] = 0x02; a->v6.s6_addr[15] = 1; break;
    case 11: a->family = COSMO_AF_INET6; a->v6.s6_addr[0] = 0x20; a->v6.s6_addr[1] = 0x01; a->v6.s6_addr[15] = 1; break;
    case 12: a->family = COSMO_AF_INET6; a->v6.s6_addr[15] = 1; break;      /* ::1 */
    case 13: a->v4 = 0; break;                             /* unspecified: -EINVAL */
    case 14: a->v4 = IPV4_ADDR(10, 75, 0, 99); break;      /* the guest subnet, nobody there */
    default: a->family = COSMO_AF_INET6; a->v6 = g_ll1; break;              /* this host's other link-local */
    }
}

/* Did the ring refuse a UDP datagram since `before` -- the datagram, not the
 * ARP request or neighbour solicitation its resolution sent? */
static bool refused_udp(const struct fz_netif *f, unsigned before)
{
    if (f->refused == before)
        return false;
    const uint8_t *r = f->last_refused;
    uint32_t n = f->last_refused_len;
    if (n >= NP_ETH + NP_IPV4 && np_get16(r + 12) == ETH_P_IP)
        return r[NP_ETH + 9] == IPPROTO_UDP;
    if (n >= NP_ETH + NP_IPV6 && np_get16(r + 12) == ETH_P_IPV6)
        return r[NP_ETH + 6] == IPPROTO_UDP;
    return false;
}

static void act_udp_send(const uint8_t *arg, size_t n)
{
    static const uint8_t payload[4096] = { 'u' };
    uint8_t sel = n > 0 ? arg[0] : 0;
    struct udp_pcb *pcbs[] = { &g_udp7, &g_udpc, &g_udp6, &g_udpc6 };
    unsigned which = (sel >> 4) & 7;
    static struct udp_pcb fresh;
    struct udp_pcb *pcb = which < 4 ? pcbs[which] : &fresh;
    if (which >= 4) {
        FUZZ_ASSERT(udp_pcb_init(&fresh, which == 5 ? COSMO_AF_INET6 : COSMO_AF_INET) == 0);
        struct netaddr local;
        memset(&local, 0, sizeof(local));
        local.family = fresh.local.family;
        if (which == 6) {
            local.port = 7;   /* g_udp7's */
            FUZZ_ASSERT(udp_bind(&fresh, &local) == -EADDRINUSE);
        } else if (which == 7) {
            local.port = 7000;
            local.v4 = IPV4_ADDR(192, 0, 2, 1);
            FUZZ_ASSERT(udp_bind(&fresh, &local) == -EADDRNOTAVAIL);
        }
    }
    struct netaddr to;
    if (pcb == &g_udpc || pcb == &g_udpc6) {
        to = pcb->remote;   /* a connected send: the socket layer passes the recorded peer */
    } else {
        dest_of(sel, &to);
        to.port = n > 2 ? np_get16(arg + 1) : 9;
    }
    size_t len = n > 3 ? (size_t)arg[3] * 8u : 16u;
    if (n > 4)
        len += (size_t)(arg[4] & 15) * 256u;
    if (len > sizeof(payload))
        len = sizeof(payload);
    unsigned r0 = g_f0.refused, r1 = g_f1.refused;
    int rc = udp_sendto(pcb, payload, len, &to);
    bool refused = refused_udp(&g_f0, r0) || refused_udp(&g_f1, r1);
    trace("udp send sel %#x len %zu: %d%s\n", sel, len, rc, refused ? " (refused)" : "");
    if (refused != (rc == -ENOBUFS)) {
        fprintf(stderr, "fuzz_net_frame: a UDP send returned %d and its datagram was %srefused by the ring\n", rc,
                refused ? "" : "not ");
        FUZZ_ASSERT(refused == (rc == -ENOBUFS));
    }
    if (pcb == &fresh) {
        struct mbuf *m;
        udp_unbind(&fresh);
        while ((m = udp_recv(&fresh)) != NULL)
            m_freem(m);
    }
}

/* A segment from the IPv6 peer, at the sequence numbers the connection expects. */
static void act_v6_seg(const uint8_t *arg, size_t n)
{
    if (g_conn6 == NULL)
        return;
    uint8_t flags = n > 0 ? arg[0] : TH_ACK;
    uint16_t win = n > 2 ? np_get16(arg + 1) : 65535;
    const uint8_t *data = n > 3 ? arg + 3 : NULL;
    size_t dlen = n > 3 ? n - 3 : 0;
    if (dlen > 1200)
        dlen = 1200;
    uint8_t f[1400];
    size_t len = np_frame_tcp6(f, k_mac0, k_peer_mac, &g_conn6->remote.v6, &g_conn6->local.v6, g_conn6->remote.port,
                               g_conn6->local.port, g_conn6->rcv_nxt, g_conn6->snd_nxt, flags, win, NULL, 0, data,
                               dlen);
    deliver(&g_f0, f, len);
}

static void act_v6_host(const uint8_t *arg, size_t n)
{
    uint8_t b = n > 0 ? arg[0] : 3;
    if (g_conn6) {
        if (b & 1) {
            static const uint8_t payload[200] = { '6' };
            tcp_send(g_conn6, payload, (size_t)(n > 1 ? arg[1] : 100) % sizeof(payload) + 1);
        }
        if (b & 2) {
            uint8_t buf[512];
            bool eof = false;
            while (tcp_recv(g_conn6, buf, sizeof(buf), &eof) > 0)
                ;
        }
        if (b & 4)
            tcp_shutdown_write(g_conn6);
        if (b & 8) {
            tcp_close(g_conn6);
            g_conn6 = NULL;
        }
    }
    struct mbuf *m;
    while ((m = udp_recv(&g_udp6)) != NULL)
        m_freem(m);
    while ((m = udp_recv(&g_udpc6)) != NULL)
        m_freem(m);
    fz_run_work();
}

/* A neighbour advertisement or solicitation, made well-formed: the peer's
 * address, the unresolved one the sends park on, or this host's own, from
 * the peer's MAC or another, on either interface. */
static void act_nd(const uint8_t *arg, size_t n)
{
    uint8_t b = n > 0 ? arg[0] : 0;
    struct in6_addr target = g_peer6;
    if (b & 1)
        target.s6_addr[15] = 0x99;
    uint8_t mac[6];
    memcpy(mac, k_peer_mac, 6);
    if (b & 2)
        mac[5] ^= 0x5a;
    struct fz_netif *f = (b & 4) ? &g_f1 : &g_f0;
    const uint8_t *ours = (b & 4) ? k_mac1 : k_mac0;
    const struct in6_addr *own = (b & 4) ? &g_ll1 : &g_ll0;
    uint8_t fr[128];
    size_t len;
    if (b & 8)   /* a solicitation for our address from `target`, carrying `mac` */
        len = np_frame_nd(fr, ours, mac, &target, own, ICMPV6_NS, own, mac);
    else         /* an advertisement of `target` at `mac` */
        len = np_frame_nd(fr, ours, mac, &target, own, ICMPV6_NA, &target, mac);
    deliver(f, fr, len);
}

static void host_action(uint8_t op, const uint8_t *arg, size_t n)
{
    switch (op % ACT_COUNT) {
    case ACT_UDP_SEND:
        act_udp_send(arg, n);
        break;
    case ACT_RING:
        g_f0.ring_full = n > 0 && (arg[0] & 1);
        g_f1.ring_full = n > 0 && (arg[0] & 2);
        break;
    case ACT_V6_SEG:
        act_v6_seg(arg, n);
        break;
    case ACT_V6_HOST:
        act_v6_host(arg, n);
        break;
    case ACT_CLOCK:
        fz_clock_advance(((uint64_t)(n > 0 ? arg[0] : 0) + 1) * 100ull * 1000000ull);
        break;
    default:
        act_nd(arg, n);
        break;
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fz_net_init();
    if (g_baseline == 0)
        g_baseline = fz_allocs_live();
    setup();
    size_t off = 0;
    unsigned frames = 0;
    while (off + 3 <= size && frames < 64) {
        uint8_t ctl = data[off];
        size_t len = data[off + 1] | ((size_t)data[off + 2] << 8);
        off += 3;
        bool action = (len & ACT_RECORD) != 0;
        len &= ~(size_t)ACT_RECORD;
        if (len > size - off)
            len = size - off;
        if (action) {
            host_action(ctl, data + off, len);
            off += len;
            frames++;
            check_invariants();
            continue;
        }
        if (len > FZ_CAPTURE_MAX)
            len = FZ_CAPTURE_MAX;
        uint8_t frame[FZ_CAPTURE_MAX];
        memcpy(frame, data + off, len);
        off += len;
        frames++;
        if (ctl & CTL_ADVANCE)
            fz_clock_advance(250ull * 1000000ull);
        if ((ctl & CTL_TO_CONN) && g_conn)
            retarget(frame, len, g_conn, k_peer_mac, k_mac0);
        else if (ctl & CTL_TO_HALF)
            retarget(frame, len, g_half, k_peer_mac, k_mac0);
        if (ctl & CTL_FIX)
            np_fix_checksums(frame, len);
        if (len > 0)
            deliver((ctl & CTL_GUEST) ? &g_f1 : &g_f0, frame, len);
        if (ctl & CTL_HOST_ACTS)
            host_acts();
        if ((ctl & CTL_SHUT_WR) && g_conn)
            tcp_shutdown_write(g_conn);
        if ((ctl & CTL_CLOSE) && g_conn) {
            tcp_close(g_conn);
            g_conn = NULL;   /* the network's now; it may be freed any time */
            g_conn_closed = true;
        }
        check_invariants();
    }
    teardown();
    return 0;
}

size_t fuzz_max_len(void)
{
    return 8192;
}

/* --- seeds ---------------------------------------------------------------- */

static size_t record(uint8_t *buf, size_t cap, uint8_t ctl, const uint8_t *frame, size_t n)
{
    if (3 + n > cap)
        return 0;
    buf[0] = ctl;
    buf[1] = (uint8_t)n;
    buf[2] = (uint8_t)(n >> 8);
    memcpy(buf + 3, frame, n);
    return 3 + n;
}

/* A host-action record: `op`, then its arguments. */
static size_t action(uint8_t *buf, size_t cap, uint8_t op, const uint8_t *arg, size_t n)
{
    if (3 + n > cap)
        return 0;
    buf[0] = op;
    buf[1] = (uint8_t)n;
    buf[2] = (uint8_t)((n >> 8) | (ACT_RECORD >> 8));
    memcpy(buf + 3, arg, n);
    return 3 + n;
}

/* `udp_sendto` from socket `sock` (0-3) to destination `dest` (0-15). */
static size_t udp_action(uint8_t *buf, size_t cap, unsigned sock, unsigned dest, uint8_t len8)
{
    uint8_t a[5] = { (uint8_t)((sock << 4) | dest), 0, 9, len8, 0 };
    return action(buf, cap, ACT_UDP_SEND, a, sizeof(a));
}

size_t fuzz_seed(unsigned i, uint8_t *buf, size_t cap)
{
    uint8_t f[600];
    static const uint8_t payload[32] = "the quick brown fox jumps over";
    struct in6_addr ll0, sn0, peer6;
    memset(&ll0, 0, sizeof(ll0));
    ll0.s6_addr[0] = 0xfe;
    ll0.s6_addr[1] = 0x80;
    ll0.s6_addr[8] = k_mac0[0] ^ 0x02;
    ll0.s6_addr[9] = k_mac0[1];
    ll0.s6_addr[10] = k_mac0[2];
    ll0.s6_addr[11] = 0xff;
    ll0.s6_addr[12] = 0xfe;
    ll0.s6_addr[13] = k_mac0[3];
    ll0.s6_addr[14] = k_mac0[4];
    ll0.s6_addr[15] = k_mac0[5];
    memset(&sn0, 0, sizeof(sn0));
    sn0.s6_addr[0] = 0xff;
    sn0.s6_addr[1] = 0x02;
    sn0.s6_addr[11] = 0x01;
    sn0.s6_addr[12] = 0xff;
    memcpy(sn0.s6_addr + 13, ll0.s6_addr + 13, 3);
    memset(&peer6, 0, sizeof(peer6));
    peer6.s6_addr[0] = 0xfe;
    peer6.s6_addr[1] = 0x80;
    peer6.s6_addr[15] = 0x01;
    uint8_t sn_mac[6] = { 0x33, 0x33, 0xff, sn0.s6_addr[13], sn0.s6_addr[14], sn0.s6_addr[15] };
    uint8_t mss[4] = { 2, 4, 0x05, 0xb4 };
    size_t n;
    switch (i) {
    case 0:   /* the peer asks for us */
        n = np_frame_arp(f, eth_broadcast, k_peer_mac, 1, k_peer_mac, PEER, eth_broadcast, IP0);
        return record(buf, cap, 0, f, n);
    case 1:   /* a reply completes the half-open connection's ARP entry: its SYN goes out */
        n = np_frame_arp(f, k_mac0, k_peer_mac, 2, k_peer_mac, FAR, k_mac0, IP0);
        return record(buf, cap, 0, f, n);
    case 2:   /* ping */
        n = np_frame_icmp4(f, k_mac0, k_peer_mac, PEER, IP0, ICMP_ECHO, 0, 7, 1, payload, sizeof(payload));
        return record(buf, cap, CTL_FIX, f, n);
    case 3:   /* UDP to the echo socket */
        n = np_frame_udp4(f, k_mac0, k_peer_mac, PEER, IP0, 5555, 7, payload, sizeof(payload));
        return record(buf, cap, CTL_FIX | CTL_HOST_ACTS, f, n);
    case 4:   /* a SYN to the listener */
        n = np_frame_tcp4(f, k_mac0, k_peer_mac, PEER, IP0, 40001, 80, 77, 0, TH_SYN, 65535, mss, 4, NULL, 0);
        return record(buf, cap, CTL_FIX, f, n);
    case 5:   /* data on the established connection, then the host answers */
        n = np_frame_tcp4(f, k_mac0, k_peer_mac, PEER, IP0, PEER_PORT, 80, 0, 0, TH_PSH | TH_ACK, 65535, NULL, 0,
                          payload, sizeof(payload));
        return record(buf, cap, CTL_FIX | CTL_TO_CONN | CTL_HOST_ACTS, f, n);
    case 6:   /* the peer closes */
        n = np_frame_tcp4(f, k_mac0, k_peer_mac, PEER, IP0, PEER_PORT, 80, 0, 0, TH_FIN | TH_ACK, 65535, NULL, 0, NULL, 0);
        return record(buf, cap, CTL_FIX | CTL_TO_CONN | CTL_CLOSE, f, n);
    case 7:   /* a reset */
        n = np_frame_tcp4(f, k_mac0, k_peer_mac, PEER, IP0, PEER_PORT, 80, 0, 0, TH_RST, 0, NULL, 0, NULL, 0);
        return record(buf, cap, CTL_FIX | CTL_TO_CONN, f, n);
    case 8:   /* the guest sends UDP to the world: masqueraded */
        n = np_frame_udp4(f, k_mac1, k_guest_mac, GUEST, IPV4_ADDR(8, 8, 8, 8), 5000, 53, payload, sizeof(payload));
        return record(buf, cap, CTL_GUEST | CTL_FIX, f, n);
    case 9:   /* the guest opens a TCP connection to the world */
        n = np_frame_tcp4(f, k_mac1, k_guest_mac, GUEST, IPV4_ADDR(1, 2, 3, 4), 50000, 80, 5, 0, TH_SYN, 65535, mss, 4,
                          NULL, 0);
        return record(buf, cap, CTL_GUEST | CTL_FIX, f, n);
    case 10:  /* the world dials the port forward */
        n = np_frame_tcp4(f, k_mac0, k_peer_mac, PEER, IP0, 41000, 8080, 9, 0, TH_SYN, 65535, mss, 4, NULL, 0);
        return record(buf, cap, CTL_FIX, f, n);
    case 11:  /* neighbour solicitation for our link-local address */
        n = np_frame_nd(f, sn_mac, k_peer_mac, &peer6, &sn0, ICMPV6_NS, &ll0, k_peer_mac);
        return record(buf, cap, 0, f, n);
    case 12:  /* IPv6 ping */
        n = np_frame_echo6(f, k_mac0, k_peer_mac, &peer6, &ll0, payload, sizeof(payload));
        return record(buf, cap, 0, f, n);
    case 13:  /* the guest to a dropped port on the uplink */
        n = np_frame_udp4(f, k_mac1, k_guest_mac, GUEST, PEER, 5001, 25, payload, 8);
        return record(buf, cap, CTL_GUEST | CTL_FIX, f, n);
    case 14:  /* the guest to the host's own echo socket (the INPUT chain) */
        n = np_frame_udp4(f, k_mac1, k_guest_mac, GUEST, IP1, 5002, 7, payload, 8);
        return record(buf, cap, CTL_GUEST | CTL_FIX | CTL_HOST_ACTS, f, n);
    case 15: { /* fragmentation needed, quoting a segment of the connection (the quote is retargeted by hand) */
        uint8_t inner[NP_IPV4 + 8];
        np_ipv4(inner, IP0, PEER, IPPROTO_TCP, 8, 64);
        np_put16(inner + NP_IPV4, 80);
        np_put16(inner + NP_IPV4 + 2, PEER_PORT);
        np_put32(inner + NP_IPV4 + 4, OUR_ISN + 1);
        n = np_frame_icmp4(f, k_mac0, k_peer_mac, PEER, IP0, ICMP_DEST_UNREACH, ICMP_UNREACH_NEEDFRAG, 0, 576, inner,
                           sizeof(inner));
        return record(buf, cap, CTL_FIX, f, n);
    }
    case 16: { /* time passes, then data, then the host shuts its write side */
        size_t a = record(buf, cap, CTL_ADVANCE | CTL_TO_CONN | CTL_FIX, f,
                          np_frame_tcp4(f, k_mac0, k_peer_mac, PEER, IP0, PEER_PORT, 80, 0, 0, TH_ACK, 65535, NULL,
                                        0, payload, 16));
        size_t b = record(buf + a, cap - a, CTL_ADVANCE | CTL_TO_CONN | CTL_FIX | CTL_SHUT_WR | CTL_HOST_ACTS, f,
                          np_frame_tcp4(f, k_mac0, k_peer_mac, PEER, IP0, PEER_PORT, 80, 0, 0, TH_ACK, 65535, NULL,
                                        0, payload, 16));
        return a + b;
    }
    case 17:  /* a runt and a broadcast */
        memset(f, 0, 60);
        np_eth(f, eth_broadcast, k_peer_mac, ETH_P_IP);
        return record(buf, cap, 0, f, 20);
    case 18:  /* a SYN-ACK to the half-open connection: it completes */
        n = np_frame_tcp4(f, k_mac0, k_peer_mac, FAR, IP0, 9, 0, 300, 0, TH_SYN | TH_ACK, 65535, mss, 4, NULL, 0);
        return record(buf, cap, CTL_FIX | CTL_TO_HALF | CTL_HOST_ACTS, f, n);
    case 19:  /* UDP to the resolved peer, then to the unresolved neighbour, then time for the ARP retries */
        n = udp_action(buf, cap, 0, 0, 4);
        n += udp_action(buf + n, cap - n, 0, 1, 4);
        return n + action(buf + n, cap - n, ACT_CLOCK, (const uint8_t[]){ 30 }, 1);
    case 20:  /* a connected send, a routed one, a broadcast, a guest-side one */
        n = udp_action(buf, cap, 1, 0, 2);
        n += udp_action(buf + n, cap - n, 0, 2, 2);
        n += udp_action(buf + n, cap - n, 0, 5, 2);
        return n + udp_action(buf + n, cap - n, 0, 3, 2);
    case 21:  /* IPv6: to the peer, connected, multicast */
        n = udp_action(buf, cap, 2, 8, 4);
        n += udp_action(buf + n, cap - n, 3, 0, 4);
        return n + udp_action(buf + n, cap - n, 2, 10, 4);
    case 22:  /* IPv6 to an unresolved neighbour, then its advertisement completes it */
        n = udp_action(buf, cap, 2, 9, 4);
        return n + action(buf + n, cap - n, ACT_ND, (const uint8_t[]){ 1 }, 1);
    case 23:  /* the ring full: a resolved send fails -ENOBUFS; then empty again */
        n = action(buf, cap, ACT_RING, (const uint8_t[]){ 3 }, 1);
        n += udp_action(buf + n, cap - n, 0, 0, 4);
        n += udp_action(buf + n, cap - n, 2, 8, 4);
        n += action(buf + n, cap - n, ACT_RING, (const uint8_t[]){ 0 }, 1);
        return n + udp_action(buf + n, cap - n, 0, 0, 4);
    case 24:  /* the IPv6 peer sends data, the host answers and closes */
        n = action(buf, cap, ACT_V6_SEG, (const uint8_t[]){ TH_PSH | TH_ACK, 0xff, 0xff, 'h', 'i', '6' }, 6);
        return n + action(buf + n, cap - n, ACT_V6_HOST, (const uint8_t[]){ 1 | 2 | 8, 40 }, 2);
    case 25:  /* the IPv6 peer closes */
        n = action(buf, cap, ACT_V6_SEG, (const uint8_t[]){ TH_FIN | TH_ACK, 0xff, 0xff }, 3);
        return n + action(buf + n, cap - n, ACT_V6_HOST, (const uint8_t[]){ 2 | 8 }, 1);
    case 27: { /* port unreachable quoting the connected socket's flow: its pending error (udp_error_notify) */
        uint8_t inner[NP_IPV4 + 8];
        np_ipv4(inner, IP0, PEER, IPPROTO_UDP, 8, 64);
        np_put16(inner + NP_IPV4, 5353);
        np_put16(inner + NP_IPV4 + 2, 53);
        np_put16(inner + NP_IPV4 + 4, 8);
        np_put16(inner + NP_IPV4 + 6, 0);
        n = np_frame_icmp4(f, k_mac0, k_peer_mac, PEER, IP0, ICMP_DEST_UNREACH, ICMP_UNREACH_PORT, 0, 0, inner,
                           sizeof(inner));
        size_t a = udp_action(buf, cap, 1, 0, 2);
        return a + record(buf + a, cap - a, CTL_FIX, f, n);
    }
    case 28:  /* fresh sockets: an ephemeral port each family, a port in use, an address not ours */
        n = udp_action(buf, cap, 4, 0, 2);
        n += udp_action(buf + n, cap - n, 5, 8, 2);
        n += udp_action(buf + n, cap - n, 6, 0, 2);
        return n + udp_action(buf + n, cap - n, 7, 0, 2);
    case 26:  /* an oversized datagram, own and loopback destinations, an unroutable one */
        n = udp_action(buf, cap, 0, 0, 255);
        n += udp_action(buf + n, cap - n, 0, 4, 2);
        n += udp_action(buf + n, cap - n, 0, 7, 2);
        return n + udp_action(buf + n, cap - n, 2, 11, 2);
    default:
        return 0;
    }
}
