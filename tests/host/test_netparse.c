/*
 * test_netparse.c - Host test of the network receive path's decoders
 * (kernel/net/netparse.h; roadmap 2.0 SA5, the network-frame family).
 * ASan/UBSan.
 *
 * Besides direct checks, a recorded digest: 200,000 inputs from a seeded
 * generator, half of them a valid header of the decoder's layer with a few
 * bytes changed (so the accepting paths are reached, not only the
 * refusals), every decoder run on each with lengths, MACs and option types
 * the generator also picks. Every result is a line, and the lines' FNV-1a
 * digest is recorded below: the differential test of the rule's point 5.
 * The C decoders gave it (2026-10-31), and the CeeSafe ones that replaced
 * them (netparse.cee) give the same. NETPARSE_DUMP=1 prints the lines.
 *
 * The TCP segment family has a digest of its own ("tcp digest"): 200,000
 * headers and option lists, recorded from the C (netparse_tcp.c) the same
 * way, which netparse.cee gives too.
 *
 * The DHCP and DNS family has one too ("dhcp digest"): 100,000 DHCP
 * messages, the frames that carry them to the tap's server, and resolver
 * addresses as text, recorded from the C (netparse_dhcp.c), which
 * netparse.cee gives too.
 *
 * NETPARSE_BENCH=1 times the decoders instead: the same inputs, every
 * decoder on each, best of 9 runs, in nanoseconds per input (the 5% rule).
 */

#include "harness.h"

#include <kernel/net/dhcp.h>
#include <kernel/net/netparse.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* --- direct checks --------------------------------------------------------- */

static const uint8_t OWN[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };

static void eth(uint8_t *f, const uint8_t dst[6], const uint8_t src[6], uint16_t type)
{
    memcpy(f, dst, 6);
    memcpy(f + 6, src, 6);
    f[12] = (uint8_t)(type >> 8);
    f[13] = (uint8_t)type;
}

static void test_ether(void)
{
    static const uint8_t peer[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x02 };
    static const uint8_t other[6] = { 0x52, 0x54, 0x00, 0x99, 0x99, 0x99 };
    static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    static const uint8_t mcast[6] = { 0x33, 0x33, 0x00, 0x00, 0x00, 0x01 };
    uint8_t f[14];
    uint16_t type = 0;
    eth(f, OWN, peer, 0x0800);
    EXPECT(netparse_ether(f, OWN, &type) == NETPARSE_ETH_UNICAST && type == 0x0800);
    eth(f, other, peer, 0x0806);
    EXPECT(netparse_ether(f, OWN, &type) == NETPARSE_ETH_OTHER);
    eth(f, OWN, OWN, 0x0800);
    EXPECT(netparse_ether(f, OWN, &type) == NETPARSE_ETH_OWN);
    eth(f, bcast, OWN, 0x0806);   /* our own broadcast, reflected */
    EXPECT(netparse_ether(f, OWN, &type) == NETPARSE_ETH_OWN);
    eth(f, bcast, peer, 0x0806);
    EXPECT(netparse_ether(f, OWN, &type) == NETPARSE_ETH_BROADCAST && type == 0x0806);
    eth(f, mcast, peer, 0x86dd);
    EXPECT(netparse_ether(f, OWN, &type) == NETPARSE_ETH_MULTICAST && type == 0x86dd);
}

static void arp_pkt(uint8_t *p, uint16_t op)
{
    static const uint8_t pkt[28] = { 0, 1, 8, 0, 6, 4, 0, 1, 0x52, 0x54, 0, 0x12, 0x34, 2, 10, 0, 2, 2,
                                     0, 0, 0, 0, 0, 0, 10, 0, 2, 15 };
    memcpy(p, pkt, sizeof(pkt));
    p[6] = (uint8_t)(op >> 8);
    p[7] = (uint8_t)op;
}

static void test_arp(void)
{
    uint8_t p[28];
    struct netparse_arp a;
    arp_pkt(p, 2);
    EXPECT(netparse_arp(p, &a));
    EXPECT(a.op == 2 && a.sha[5] == 2 && a.spa[0] == 10 && a.spa[3] == 2 && a.tpa[3] == 15);
    for (int i = 0; i < 6; i++) {   /* each of htype, ptype, hlen, plen wrong */
        arp_pkt(p, 1);
        p[i] ^= 0x10;
        EXPECT(!netparse_arp(p, &a));
    }
}

/* A valid IPv4 header of `ihl` bytes in a `total`-byte datagram, checksummed. */
static void ipv4_hdr(uint8_t *h, uint32_t ihl, uint32_t total)
{
    memset(h, 0, ihl);
    h[0] = (uint8_t)(0x40 | ihl / 4);
    h[2] = (uint8_t)(total >> 8);
    h[3] = (uint8_t)total;
    h[6] = 0x40;   /* DF */
    h[8] = 64;
    h[9] = 17;
    h[12] = 10; h[15] = 2;
    h[16] = 10; h[19] = 15;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < ihl; i += 2)
        sum += (uint32_t)(h[i] << 8 | h[i + 1]);
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    sum = ~sum & 0xffff;
    h[10] = (uint8_t)(sum >> 8);
    h[11] = (uint8_t)sum;
}

static void test_ipv4(void)
{
    uint8_t h[60];
    uint32_t ihl = 0, total = 0;
    ipv4_hdr(h, 20, 48);
    EXPECT(netparse_ipv4_outer(h, 48, &ihl, &total) == NETPARSE_OK && ihl == 20 && total == 48);
    EXPECT(netparse_ipv4_outer(h, 47, &ihl, &total) == NETPARSE_BAD);   /* claims more than arrived */
    EXPECT(netparse_ipv4_header(h, 20) == NETPARSE_OK);
    h[8] = 63;   /* TTL changed under the checksum */
    EXPECT(netparse_ipv4_header(h, 20) == NETPARSE_BAD_CKSUM);
    ipv4_hdr(h, 60, 60);
    EXPECT(netparse_ipv4_outer(h, 1500, &ihl, &total) == NETPARSE_OK && ihl == 60 && total == 60);
    EXPECT(netparse_ipv4_header(h, 60) == NETPARSE_OK);
    ipv4_hdr(h, 20, 19);   /* total below the header */
    EXPECT(netparse_ipv4_outer(h, 100, &ihl, &total) == NETPARSE_BAD);
    ipv4_hdr(h, 20, 40);
    h[0] = 0x44;   /* ihl 16 */
    EXPECT(netparse_ipv4_outer(h, 100, &ihl, &total) == NETPARSE_BAD);
    h[0] = 0x65;   /* version 6 */
    EXPECT(netparse_ipv4_outer(h, 100, &ihl, &total) == NETPARSE_BAD);
    /* A fragment with a correct checksum: refused as a fragment. */
    ipv4_hdr(h, 20, 40);
    h[6] = 0x20;   /* MF */
    h[10] = h[11] = 0;
    uint32_t sum = 0;
    for (int i = 0; i < 20; i += 2)
        sum += (uint32_t)(h[i] << 8 | h[i + 1]);
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    sum = ~sum & 0xffff;
    h[10] = (uint8_t)(sum >> 8);
    h[11] = (uint8_t)sum;
    EXPECT(netparse_ipv4_header(h, 20) == NETPARSE_FRAGMENT);
    /* A quote: version and length only. */
    ipv4_hdr(h, 24, 2);
    EXPECT(netparse_ipv4_quote(h, &ihl) && ihl == 24);
    h[0] = 0x43;
    EXPECT(!netparse_ipv4_quote(h, &ihl));
}

static void test_ipv6_nd(void)
{
    uint8_t h[40] = { 0x60, 0, 0, 0, 0, 32, 58, 255 };
    uint32_t plen = 0;
    EXPECT(netparse_ipv6(h, 72, &plen) == NETPARSE_OK && plen == 32);
    EXPECT(netparse_ipv6(h, 71, &plen) == NETPARSE_BAD);
    h[0] = 0x40;
    EXPECT(netparse_ipv6(h, 72, &plen) == NETPARSE_BAD);

    uint8_t msg[32] = { 135, 0, 0, 0, 0, 0, 0, 0, 0xfe, 0x80 };
    msg[23] = 1;
    msg[24] = 1;
    msg[25] = 1;
    memcpy(msg + 26, OWN, 6);
    struct netparse_nd nd;
    netparse_nd(msg, 1, &nd);
    EXPECT(nd.has_mac && nd.target[0] == 0xfe && nd.target[15] == 1 && memcmp(nd.mac, OWN, 6) == 0);
    netparse_nd(msg, 2, &nd);   /* a solicitation's option is not a target address */
    EXPECT(!nd.has_mac && nd.target[0] == 0xfe && nd.mac[0] == 0);
    msg[25] = 2;
    netparse_nd(msg, 1, &nd);
    EXPECT(!nd.has_mac);
}

static void test_transports(void)
{
    uint8_t u[8] = { 0x30, 0x39, 0, 53, 0, 20, 0, 0 };
    struct netparse_udp d;
    EXPECT(netparse_udp(u, 20, &d) == NETPARSE_OK && d.len == 20 && d.sport == 12345 && d.dport == 53 && !d.has_cksum);
    EXPECT(netparse_udp(u, 19, &d) == NETPARSE_BAD);
    u[7] = 1;
    EXPECT(netparse_udp(u, 30, &d) == NETPARSE_OK && d.has_cksum);
    u[5] = 7;
    EXPECT(netparse_udp(u, 30, &d) == NETPARSE_BAD);

    uint8_t t[14] = { 0x30, 0x39, 0, 80, 0x01, 0x02, 0x03, 0x04, 0, 0, 0, 0, 0x50, 0x12 };
    struct netparse_l4 l;
    netparse_l4(t, 14, &l);
    EXPECT(l.sport == 12345 && l.dport == 80 && l.seq == 0x01020304 && l.tcp_flags == 0x12);
    EXPECT(l.icmp_type == 0x30 && l.icmp_id == 0x0102);
    netparse_l4(t, 8, &l);
    EXPECT(l.tcp_flags == 0);
}

static void test_tcp(void)
{
    uint8_t h[20] = { 0x30, 0x39, 0, 80, 1, 2, 3, 4, 5, 6, 7, 8, 0x60, 0x12, 0xff, 0xff };
    struct netparse_tcp d;
    EXPECT(netparse_tcp(h, 24, &d) == NETPARSE_OK && d.hlen == 24 && d.sport == 12345 && d.dport == 80);
    EXPECT(d.seq == 0x01020304 && d.ack == 0x05060708 && d.flags == 0x12 && d.win == 0xffff);
    EXPECT(netparse_tcp(h, 23, &d) == NETPARSE_BAD);   /* options beyond the segment */
    h[12] = 0x40;   /* 16 bytes */
    EXPECT(netparse_tcp(h, 100, &d) == NETPARSE_BAD);
    h[12] = 0xf0;
    EXPECT(netparse_tcp(h, 60, &d) == NETPARSE_OK && d.hlen == 60);

    static const uint8_t mss[] = { 2, 4, 0x05, 0xb4 };
    EXPECT(netparse_tcp_mss(mss, 4, 1220) == 1460);
    static const uint8_t low[] = { 1, 1, 2, 4, 0x01, 0x00 };   /* no-ops, then an MSS of 256 */
    EXPECT(netparse_tcp_mss(low, 6, 1220) == 536);
    static const uint8_t after_end[] = { 0, 2, 4, 0x05, 0xb4 };
    EXPECT(netparse_tcp_mss(after_end, 5, 1220) == 1220);
    static const uint8_t sack_then[] = { 4, 2, 3, 3, 7, 2, 4, 0x05, 0x00 };
    EXPECT(netparse_tcp_mss(sack_then, 9, 1220) == 1280);
    static const uint8_t bad_len[] = { 3, 1, 2, 4, 0x05, 0xb4 };   /* a length below 2 ends the walk */
    EXPECT(netparse_tcp_mss(bad_len, 6, 1220) == 1220);
    static const uint8_t past[] = { 8, 10, 2, 4, 0x05, 0xb4 };     /* runs past the options */
    EXPECT(netparse_tcp_mss(past, 6, 1220) == 1220);
    static const uint8_t cut[] = { 1, 2 };                          /* a kind with no length byte */
    EXPECT(netparse_tcp_mss(cut, 2, 1220) == 1220);
    EXPECT(netparse_tcp_mss(mss, 0, 777) == 777);
}

/* A DHCP message: the BOOTP header, the cookie, then `opts` (n bytes). */
static uint32_t dhcp_msg(uint8_t *dh, uint8_t op, const uint8_t *opts, uint32_t n)
{
    memset(dh, 0, DHCP_FIXED);
    dh[BOOTP_OFF_OP] = op;
    dh[BOOTP_OFF_HTYPE] = DHCP_HTYPE_ETHER;
    dh[BOOTP_OFF_HLEN] = 6;
    dh[BOOTP_OFF_XID] = 0xde; dh[BOOTP_OFF_XID + 3] = 0x01;
    dh[BOOTP_OFF_FLAGS] = 0x80;
    dh[BOOTP_OFF_YIADDR] = 10; dh[BOOTP_OFF_YIADDR + 3] = 15;
    memcpy(dh + BOOTP_OFF_CHADDR, OWN, 6);
    dh[BOOTP_LEN] = 0x63; dh[BOOTP_LEN + 1] = 0x82; dh[BOOTP_LEN + 2] = 0x53; dh[BOOTP_LEN + 3] = 0x63;
    memcpy(dh + DHCP_FIXED, opts, n);
    return DHCP_FIXED + n;
}

static void test_dhcp(void)
{
    uint8_t dh[DHCP_FIXED + 96];
    static const uint8_t ack[] = { 53, 1, 5, 54, 4, 10, 0, 2, 2, 51, 4, 0, 0, 0x0e, 0x10, 1, 4, 255, 255, 255, 0,
                                   3, 8, 10, 0, 2, 2, 10, 0, 2, 3, 6, 4, 10, 0, 2, 3, 12, 5, 'c', 'o', 's', 'm', 'o',
                                   0, 0, 58, 4, 0, 0, 7, 8, 255, 99, 1, 7 };
    uint32_t n = dhcp_msg(dh, BOOTP_REPLY, ack, sizeof(ack));
    struct netparse_dhcp m;
    EXPECT(netparse_dhcp(dh, n, BOOTP_REPLY, &m) == NETPARSE_OK && m.msg_type == DHCP_ACK && m.has_server);
    EXPECT(m.server[3] == 2 && m.flags == 0x8000 && m.xid[0] == 0xde && m.yiaddr[3] == 15 && !m.has_reqip);
    EXPECT(netparse_dhcp(dh, n, BOOTP_REQUEST, &m) == NETPARSE_BAD);   /* the wrong direction */
    EXPECT(netparse_dhcp(dh, DHCP_FIXED - 1, BOOTP_REPLY, &m) == NETPARSE_BAD);
    struct netparse_dhcp_lease ls;
    EXPECT(netparse_dhcp_lease(dh, n, &ls) && ls.lease == 3600 && ls.has_mask && ls.mask[2] == 255);
    EXPECT(ls.has_router && ls.router[3] == 2 && ls.ndns == 1 && ls.dns[3] == 3);
    EXPECT(strcmp((const char *)ls.hostname, "cosmo") == 0 && ls.t1 == 0x708 && ls.t2 == 0);
    uint8_t l = 0;
    EXPECT(netparse_dhcp_opt(dh + DHCP_FIXED, n - DHCP_FIXED, 99, &l) == -1);   /* after the end option */
    EXPECT(netparse_dhcp_opt(dh + DHCP_FIXED, n - DHCP_FIXED, 51, &l) == 11 && l == 4);

    static const uint8_t bad_name[] = { 53, 1, 5, 51, 4, 0, 0, 0, 60, 12, 3, 'a', '/', 'b', 255 };
    n = dhcp_msg(dh, BOOTP_REPLY, bad_name, sizeof(bad_name));
    EXPECT(netparse_dhcp_lease(dh, n, &ls) && ls.hostname[0] == 0 && !ls.has_mask && !ls.has_router);
    static const uint8_t no_lease[] = { 53, 1, 5, 51, 2, 0, 1, 255 };
    n = dhcp_msg(dh, BOOTP_REPLY, no_lease, sizeof(no_lease));
    EXPECT(!netparse_dhcp_lease(dh, n, &ls));
    static const uint8_t runs_past[] = { 53, 1, 1, 50, 9, 10, 0 };   /* option 50's length runs past the end */
    n = dhcp_msg(dh, BOOTP_REQUEST, runs_past, sizeof(runs_past));
    EXPECT(netparse_dhcp(dh, n, BOOTP_REQUEST, &m) == NETPARSE_OK && m.msg_type == DHCP_DISCOVER && !m.has_reqip);
    static const uint8_t no_type[] = { 0, 0, 255, 53, 1, 1 };
    n = dhcp_msg(dh, BOOTP_REQUEST, no_type, sizeof(no_type));
    EXPECT(netparse_dhcp(dh, n, BOOTP_REQUEST, &m) == NETPARSE_BAD);

    uint8_t f[14 + 24 + 8 + 4] = { 0 };
    f[12] = 0x08; f[14] = 0x46; f[14 + 9] = 17;   /* IPv4, a 24-byte header, UDP */
    f[14 + 24 + 2] = 0; f[14 + 24 + 3] = 67;
    uint32_t off = 0;
    EXPECT(netparse_dhcp_frame(f, sizeof(f), &off) && off == 14 + 24 + 8);
    EXPECT(!netparse_dhcp_frame(f, 14 + 24 + 7, &off));
    f[14 + 24 + 3] = 68;
    EXPECT(!netparse_dhcp_frame(f, sizeof(f), &off));

    uint8_t a[4];
    EXPECT(netparse_ipv4_text("10.0.2.3", 8, a) && a[0] == 10 && a[3] == 3);
    EXPECT(!netparse_ipv4_text("10.0.2.256", 10, a) && !netparse_ipv4_text("10.0..3", 7, a));
    EXPECT(!netparse_ipv4_text("10.0.2", 6, a) && !netparse_ipv4_text("1.2.3.4.5", 9, a) && !netparse_ipv4_text("", 0, a));
    EXPECT(netparse_ipv4_text("10.0.2.3 x", 8, a));   /* only `len` characters count */
    static const uint8_t q[12] = { 0xab, 0xcd };
    EXPECT(netparse_dns_id(q, sizeof(q)) == 0xabcd);
}

/* --- the recorded digest --------------------------------------------------- */

#define ROUNDS 200000
#define DIGEST_SEED 0x5A5E7F4A3E202610ull
/* The digest of the C decoders (2026-10-31), which netparse.cee gives too.
 * A change that disagrees changes what the stack accepts: find the first
 * differing line with NETPARSE_DUMP=1 against the commit before it. */
#define NETPARSE_DIGEST 0xb332eb59d8cf9149ull

struct input {
    uint8_t b[64];
    uint8_t own[6];
    uint32_t len_a, len_b;   /* packet lengths the length checks are given */
    uint8_t opt_type;
    uint32_t avail;
};

static uint64_t g_rng;

static uint64_t next(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static void make_input(struct input *in)
{
    for (size_t i = 0; i < sizeof(in->b); i += 8) {
        uint64_t r = next();
        memcpy(in->b + i, &r, 8);
    }
    switch (next() % 8) {   /* a valid start for one layer, or (0-3) none */
    case 4: {
        uint32_t ihl = 20 + 4 * (uint32_t)(next() % 11);
        ipv4_hdr(in->b, ihl, ihl + (uint32_t)(next() % 64));
        break;
    }
    case 5:
        arp_pkt(in->b, (uint16_t)(1 + next() % 3));
        break;
    case 6:
        in->b[0] = 0x60;
        in->b[4] = 0;
        in->b[5] = (uint8_t)(next() % 64);
        in->b[24] = (uint8_t)(1 + next() % 2);
        in->b[25] = 1;
        break;
    case 7:
        in->b[4] = 0;
        in->b[5] = (uint8_t)(next() % 32);
        break;
    default:
        break;
    }
    unsigned flips = (unsigned)(next() % 4);   /* then a few bytes changed */
    for (unsigned i = 0; i < flips; i++)
        in->b[next() % 64] ^= (uint8_t)(1u << (next() % 8));
    switch (next() % 3) {   /* the interface's MAC: the frame's destination, its source, or another */
    case 0: memcpy(in->own, in->b, 6); break;
    case 1: memcpy(in->own, in->b + 6, 6); break;
    default: memcpy(in->own, OWN, 6); break;
    }
    uint32_t claimed = (uint32_t)(in->b[2] << 8 | in->b[3]);
    in->len_a = next() % 2 ? claimed + (uint32_t)(next() % 3) - 1 : (uint32_t)(next() % 2000);
    in->len_b = next() % 2 ? (uint32_t)(in->b[4] << 8 | in->b[5]) + (uint32_t)(next() % 3) + 39 : (uint32_t)(next() % 200);
    in->opt_type = (uint8_t)(1 + next() % 2);
    in->avail = next() % 2 ? 14 : 8;
}

/* Every decoder on one input, its results as a line; `accepted` counts, per
 * layer, the inputs a decoder accepted (ipv4, arp, ipv6, udp). */
static int run_all(const struct input *in, char *line, size_t size, unsigned accepted[4])
{
    uint16_t type = 0;
    uint32_t eth = netparse_ether(in->b, in->own, &type);

    struct netparse_arp a;
    memset(&a, 0, sizeof(a));
    bool arp = netparse_arp(in->b, &a);

    uint32_t ihl = 0, total = 0, hdr = 9;
    uint32_t outer = netparse_ipv4_outer(in->b, in->len_a, &ihl, &total);
    if (outer == NETPARSE_OK)
        hdr = netparse_ipv4_header(in->b, ihl);
    uint32_t qihl = 0;
    bool quote = netparse_ipv4_quote(in->b, &qihl);

    uint32_t plen = 0;
    uint32_t v6 = netparse_ipv6(in->b, in->len_b, &plen);
    struct netparse_nd nd;
    memset(&nd, 0, sizeof(nd));
    netparse_nd(in->b, in->opt_type, &nd);

    struct netparse_udp u;
    memset(&u, 0, sizeof(u));
    uint32_t udp = netparse_udp(in->b, in->len_a, &u);
    struct netparse_l4 l;
    memset(&l, 0, sizeof(l));
    netparse_l4(in->b, in->avail, &l);

    accepted[0] += outer == NETPARSE_OK && hdr == NETPARSE_OK;
    accepted[1] += arp;
    accepted[2] += v6 == NETPARSE_OK;
    accepted[3] += udp == NETPARSE_OK;
    return snprintf(line, size,
                    "e%u:%04x a%d:%u:%02x%02x%02x%02x%02x%02x:%u.%u.%u.%u:%u.%u.%u.%u "
                    "4%u:%u:%u:%u q%d:%u 6%u:%u n%d:%02x%02x:%02x%02x%02x%02x%02x%02x "
                    "u%u:%u:%u:%u:%d l%u:%u:%08x:%u:%u:%u",
                    eth, type, arp, a.op, a.sha[0], a.sha[1], a.sha[2], a.sha[3], a.sha[4], a.sha[5], a.spa[0],
                    a.spa[1], a.spa[2], a.spa[3], a.tpa[0], a.tpa[1], a.tpa[2], a.tpa[3], outer, ihl, total, hdr,
                    quote, qihl, v6, plen, nd.has_mac, nd.target[0], nd.target[15], nd.mac[0], nd.mac[1], nd.mac[2],
                    nd.mac[3], nd.mac[4], nd.mac[5], udp, u.len, u.sport, u.dport, u.has_cksum, l.sport, l.dport,
                    l.seq, l.icmp_type, l.icmp_id, l.tcp_flags);
}

static void test_recorded_digest(void)
{
    bool dump = getenv("NETPARSE_DUMP") != NULL;
    uint64_t digest = 0xcbf29ce484222325ull;
    g_rng = DIGEST_SEED;
    unsigned accepted[4] = { 0, 0, 0, 0 };
    for (unsigned r = 0; r < ROUNDS; r++) {
        struct input in;
        make_input(&in);
        char line[512];
        int n = run_all(&in, line, sizeof(line), accepted);
        EXPECT(n > 0 && (size_t)n < sizeof(line));
        for (int i = 0; i < n; i++) {
            digest ^= (uint8_t)line[i];
            digest *= 0x100000001b3ull;
        }
        digest ^= '\n';
        digest *= 0x100000001b3ull;
        if (dump)
            printf("%s\n", line);
    }
    /* The generator reaches the accepting paths, not only the refusals. */
    EXPECT(accepted[0] > 1000 && accepted[1] > 1000 && accepted[2] > 1000 && accepted[3] > 1000);
    printf("  netparse digest 0x%016llx over %u inputs (accepted: ipv4 %u, arp %u, ipv6 %u, udp %u)\n",
           (unsigned long long)digest, ROUNDS, accepted[0], accepted[1], accepted[2], accepted[3]);
    EXPECT(digest == NETPARSE_DIGEST);
}

/* The TCP segment family's digest: the C's (2026-10-31), which netparse.cee gives too. */
#define TCP_ROUNDS 200000
#define TCP_SEED 0x7C9A5E6D2026B31Full
#define TCP_DIGEST 0x35875a1cf041750bull

struct tcp_input {
    uint8_t h[20];
    uint8_t opts[40];
    uint32_t optlen;
    uint32_t seg_len;
    uint16_t dflt;
};

/* A header and an option list: options built from the kinds a SYN carries
 * (end, no-op, MSS, window scale, SACK permitted, timestamps) and others,
 * with lengths right or wrong, then a few bytes changed. */
static void make_tcp(struct tcp_input *in)
{
    for (size_t i = 0; i < sizeof(in->h); i += 4) {
        uint32_t r = (uint32_t)next();
        memcpy(in->h + i, &r, 4);
    }
    uint32_t words = 5 + (uint32_t)(next() % 11);
    in->h[12] = (uint8_t)(next() % 8 ? words << 4 : next());
    uint32_t n = 0;
    while (n < sizeof(in->opts)) {
        static const uint8_t kinds[] = { 0, 1, 1, 2, 2, 2, 3, 4, 8, 5, 30 };
        uint8_t kind = kinds[next() % sizeof(kinds)];
        uint8_t len = kind == 2 ? 4 : kind == 3 ? 3 : kind == 4 ? 2 : kind == 8 ? 10 : (uint8_t)(2 + next() % 8);
        if (next() % 8 == 0)
            len = (uint8_t)(next() % 12);
        in->opts[n++] = kind;
        if (kind <= 1)
            continue;
        if (n < sizeof(in->opts))
            in->opts[n++] = len;
        for (uint8_t k = 2; k < len && n < sizeof(in->opts); k++)
            in->opts[n++] = (uint8_t)next();
    }
    unsigned flips = (unsigned)(next() % 3);
    for (unsigned i = 0; i < flips; i++)
        in->opts[next() % sizeof(in->opts)] ^= (uint8_t)(1u << (next() % 8));
    in->optlen = (uint32_t)(next() % 41);
    uint32_t hlen = (uint32_t)(in->h[12] >> 4) * 4;
    in->seg_len = next() % 2 ? hlen + (uint32_t)(next() % 3) - 1 : (uint32_t)(next() % 100);
    in->dflt = (uint16_t)(next() % 2 ? 1220 : 1460);
}

static void test_tcp_digest(void)
{
    bool dump = getenv("NETPARSE_DUMP") != NULL;
    uint64_t digest = 0xcbf29ce484222325ull;
    g_rng = TCP_SEED;
    unsigned ok = 0, found = 0;
    for (unsigned r = 0; r < TCP_ROUNDS; r++) {
        struct tcp_input in;
        make_tcp(&in);
        struct netparse_tcp d;
        memset(&d, 0, sizeof(d));
        uint32_t rc = netparse_tcp(in.h, in.seg_len, &d);
        uint16_t mss = netparse_tcp_mss(in.opts, in.optlen, in.dflt);
        ok += rc == NETPARSE_OK;
        found += mss != in.dflt;
        char line[160];
        int n = snprintf(line, sizeof(line), "t%u:%u:%u:%u:%08x:%08x:%02x:%u m%u", rc, d.hlen, d.sport, d.dport, d.seq,
                         d.ack, d.flags, d.win, mss);
        EXPECT(n > 0 && (size_t)n < sizeof(line));
        for (int i = 0; i < n; i++) {
            digest ^= (uint8_t)line[i];
            digest *= 0x100000001b3ull;
        }
        digest ^= '\n';
        digest *= 0x100000001b3ull;
        if (dump)
            printf("%s\n", line);
    }
    EXPECT(ok > 1000 && found > 1000);
    printf("  tcp digest 0x%016llx over %u inputs (headers accepted %u, an MSS found %u)\n",
           (unsigned long long)digest, TCP_ROUNDS, ok, found);
    EXPECT(digest == TCP_DIGEST);
}

/* The DHCP and DNS family's digest: the C's (2026-10-31), which netparse.cee gives too. */
#define DHCP_ROUNDS 100000
#define DHCP_SEED 0xD4C9D45E2026A11Full
#define DHCP_DIGEST 0x27c8c7e8ec420cddull

struct dhcp_input {
    uint8_t frame[14 + 60 + 8 + DHCP_FIXED + 64];
    uint32_t frame_len;
    uint32_t dh_off;
    uint32_t dhlen;
    uint8_t op;
    uint8_t code;
    char text[20];
    uint32_t text_len;
};

/* A frame carrying a DHCP message to the server port, its options built
 * from the codes both ends read (with lengths right or wrong, host names
 * good and bad), then a few bytes changed anywhere; and a resolver address
 * as text, a dotted quad or close to one. */
static void make_dhcp(struct dhcp_input *in)
{
    memset(in->frame, 0, sizeof(in->frame));
    uint8_t *f = in->frame;
    f[12] = 0x08;
    uint32_t ihl = next() % 8 ? 20 : 20 + 4 * (uint32_t)(next() % 11);
    f[14] = (uint8_t)(0x40 | ihl / 4);
    f[14 + 9] = 17;
    f[14 + ihl + 2] = 0;
    f[14 + ihl + 3] = next() % 8 ? 67 : 68;
    in->dh_off = 14 + ihl + 8;
    uint8_t *dh = f + in->dh_off;
    uint8_t opts[64];
    uint32_t n = 0;
    while (n < sizeof(opts)) {
        static const uint8_t codes[] = { 0, 255, 53, 53, 54, 50, 51, 51, 1, 3, 6, 12, 12, 58, 59, 61, 99 };
        uint8_t code = codes[next() % sizeof(codes)];
        opts[n++] = code;
        if (code == 0 || code == 255)
            continue;
        uint8_t len = code == 53 ? 1 : code == 54 || code == 50 || code == 51 || code == 1 || code == 58 || code == 59 ? 4
                    : code == 3 || code == 6 ? (uint8_t)(4 * (1 + next() % 4)) : (uint8_t)(1 + next() % 10);
        if (next() % 8 == 0)
            len = (uint8_t)(next() % 16);
        if (n < sizeof(opts))
            opts[n++] = len;
        for (uint8_t k = 0; k < len && n < sizeof(opts); k++) {
            uint8_t v = (uint8_t)next();
            if (code == 12 && next() % 4)
                v = (uint8_t)("abcdefghij-.0123"[next() % 16]);
            if (code == 53)
                v = (uint8_t)(1 + next() % 7);
            opts[n++] = v;
        }
    }
    in->op = (uint8_t)(1 + next() % 2);
    uint32_t used = (uint32_t)(next() % (sizeof(opts) + 1));
    in->dhlen = dhcp_msg(dh, (uint8_t)(next() % 4 ? in->op : 3 - in->op), opts, used);
    if (next() % 16 == 0)
        in->dhlen = (uint32_t)(next() % in->dhlen);
    in->frame_len = in->dh_off + in->dhlen;
    unsigned flips = (unsigned)(next() % 3);
    for (unsigned i = 0; i < flips; i++)
        f[next() % in->frame_len] ^= (uint8_t)(1u << (next() % 8));
    in->code = (uint8_t)(next() % 64);
    uint32_t t = 0;
    for (int part = 0; part < 4 + (int)(next() % 8 == 0); part++) {
        if (part)
            in->text[t++] = '.';
        uint32_t v = next() % 8 ? (uint32_t)(next() % 256) : (uint32_t)(next() % 1000);
        t += (uint32_t)snprintf(in->text + t, sizeof(in->text) - t, "%u", v);
    }
    if (next() % 8 == 0)
        in->text[next() % t] = (char)(next() % 128);
    in->text_len = next() % 8 ? t : (uint32_t)(next() % (t + 1));
}

static void test_dhcp_digest(void)
{
    bool dump = getenv("NETPARSE_DUMP") != NULL;
    uint64_t digest = 0xcbf29ce484222325ull;
    g_rng = DHCP_SEED;
    unsigned frames = 0, msgs = 0, leases = 0, texts = 0;
    for (unsigned r = 0; r < DHCP_ROUNDS; r++) {
        struct dhcp_input in;
        make_dhcp(&in);
        uint32_t off = 0;
        bool frame = netparse_dhcp_frame(in.frame, in.frame_len, &off);
        const uint8_t *dh = in.frame + in.dh_off;
        struct netparse_dhcp m;
        memset(&m, 0, sizeof(m));
        uint32_t rc = netparse_dhcp(dh, in.dhlen, in.op, &m);
        struct netparse_dhcp_lease ls;
        memset(&ls, 0, sizeof(ls));
        bool lease = in.dhlen >= DHCP_FIXED && netparse_dhcp_lease(dh, in.dhlen, &ls);
        uint8_t ol = 0;
        int32_t at = in.dhlen >= DHCP_FIXED ? netparse_dhcp_opt(dh + DHCP_FIXED, in.dhlen - DHCP_FIXED, in.code, &ol) : -2;
        uint8_t a[4] = { 0, 0, 0, 0 };
        bool text = netparse_ipv4_text(in.text, in.text_len, a);
        uint16_t id = in.dhlen >= 12 ? netparse_dns_id(dh, in.dhlen) : 0;
        frames += frame;
        msgs += rc == NETPARSE_OK;
        leases += lease;
        texts += text;
        char line[400];
        int n = snprintf(line, sizeof(line),
                         "f%d:%u d%u:%u:%02x%02x%02x%02x:%04x:%u.%u:%u.%u:%02x%02x:%d%u:%d%u "
                         "l%d:%u:%u:%d%u:%d%u:%u:%u.%u.%u:%s:%u:%u o%d:%u t%d:%u.%u.%u.%u i%04x",
                         frame, off, rc, m.msg_type, m.xid[0], m.xid[1], m.xid[2], m.xid[3], m.flags, m.ciaddr[0],
                         m.ciaddr[3], m.yiaddr[0], m.yiaddr[3], m.chaddr[0], m.chaddr[5], m.has_server, m.server[3],
                         m.has_reqip, m.reqip[3], lease, ls.addr[3], ls.lease, ls.has_mask, ls.mask[3], ls.has_router,
                         ls.router[3], ls.ndns, ls.dns[3], ls.dns[7], ls.dns[11], (const char *)ls.hostname, ls.t1,
                         ls.t2, at, ol, text, a[0], a[1], a[2], a[3], id);
        EXPECT(n > 0 && (size_t)n < sizeof(line));
        for (int i = 0; i < n; i++) {
            digest ^= (uint8_t)line[i];
            digest *= 0x100000001b3ull;
        }
        digest ^= '\n';
        digest *= 0x100000001b3ull;
        if (dump)
            printf("%s\n", line);
    }
    EXPECT(frames > 1000 && msgs > 1000 && leases > 1000 && texts > 1000);
    printf("  dhcp digest 0x%016llx over %u inputs (frames %u, messages %u, leases %u, addresses %u)\n",
           (unsigned long long)digest, DHCP_ROUNDS, frames, msgs, leases, texts);
    EXPECT(digest == DHCP_DIGEST);
}

/* --- the benchmark ----------------------------------------------------------- */

static volatile uint64_t g_sink;

static int bench(void)
{
    struct input *ins = malloc(sizeof(*ins) * ROUNDS);
    if (ins == NULL)
        return 1;
    g_rng = DIGEST_SEED;
    for (unsigned r = 0; r < ROUNDS; r++)
        make_input(&ins[r]);
    double best = 1e30;
    for (int run = 0; run < 9; run++) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        uint64_t acc = 0;
        for (unsigned r = 0; r < ROUNDS; r++) {
            const struct input *in = &ins[r];
            uint16_t type;
            acc += netparse_ether(in->b, in->own, &type) + type;
            struct netparse_arp a;
            acc += netparse_arp(in->b, &a) ? a.op : 0;
            uint32_t ihl, total;
            if (netparse_ipv4_outer(in->b, in->len_a, &ihl, &total) == NETPARSE_OK)
                acc += netparse_ipv4_header(in->b, ihl) + total;
            acc += netparse_ipv4_quote(in->b, &ihl);
            uint32_t plen;
            acc += netparse_ipv6(in->b, in->len_b, &plen);
            struct netparse_nd nd;
            netparse_nd(in->b, in->opt_type, &nd);
            acc += nd.has_mac;
            struct netparse_udp u;
            acc += netparse_udp(in->b, in->len_a, &u);
            struct netparse_l4 l;
            netparse_l4(in->b, in->avail, &l);
            acc += l.seq;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        g_sink += acc;
        double ns = (double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec);
        if (ns < best)
            best = ns;
    }
    printf("netparse bench: %.2f ns per input (best of 9 runs of %u)\n", best / ROUNDS, ROUNDS);
    free(ins);

    struct tcp_input *tins = malloc(sizeof(*tins) * TCP_ROUNDS);
    if (tins == NULL)
        return 1;
    g_rng = TCP_SEED;
    for (unsigned r = 0; r < TCP_ROUNDS; r++)
        make_tcp(&tins[r]);
    best = 1e30;
    for (int run = 0; run < 9; run++) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        uint64_t acc = 0;
        for (unsigned r = 0; r < TCP_ROUNDS; r++) {
            const struct tcp_input *in = &tins[r];
            struct netparse_tcp d;
            if (netparse_tcp(in->h, in->seg_len, &d) == NETPARSE_OK)
                acc += d.seq + d.hlen;
            acc += netparse_tcp_mss(in->opts, in->optlen, in->dflt);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        g_sink += acc;
        double ns = (double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec);
        if (ns < best)
            best = ns;
    }
    printf("tcp bench: %.2f ns per input (best of 9 runs of %u)\n", best / TCP_ROUNDS, TCP_ROUNDS);
    free(tins);

    struct dhcp_input *dins = malloc(sizeof(*dins) * DHCP_ROUNDS);
    if (dins == NULL)
        return 1;
    g_rng = DHCP_SEED;
    for (unsigned r = 0; r < DHCP_ROUNDS; r++)
        make_dhcp(&dins[r]);
    best = 1e30;
    for (int run = 0; run < 9; run++) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        uint64_t acc = 0;
        for (unsigned r = 0; r < DHCP_ROUNDS; r++) {
            const struct dhcp_input *in = &dins[r];
            uint32_t off;
            acc += netparse_dhcp_frame(in->frame, in->frame_len, &off);
            const uint8_t *dh = in->frame + in->dh_off;
            struct netparse_dhcp m;
            if (netparse_dhcp(dh, in->dhlen, in->op, &m) == NETPARSE_OK)
                acc += m.msg_type;
            struct netparse_dhcp_lease ls;
            if (in->dhlen >= DHCP_FIXED && netparse_dhcp_lease(dh, in->dhlen, &ls))
                acc += ls.lease + ls.ndns;
            uint8_t a[4];
            acc += netparse_ipv4_text(in->text, in->text_len, a);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        g_sink += acc;
        double ns = (double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec);
        if (ns < best)
            best = ns;
    }
    printf("dhcp bench: %.2f ns per input (best of 9 runs of %u)\n", best / DHCP_ROUNDS, DHCP_ROUNDS);
    free(dins);
    return 0;
}

static const struct host_test tests[] = {
    { "ethernet", test_ether },
    { "arp", test_arp },
    { "ipv4", test_ipv4 },
    { "ipv6 and neighbour discovery", test_ipv6_nd },
    { "udp and transport ids", test_transports },
    { "tcp", test_tcp },
    { "recorded digest", test_recorded_digest },
    { "tcp digest", test_tcp_digest },
    { "dhcp and dns", test_dhcp },
    { "dhcp digest", test_dhcp_digest },
};

int main(void)
{
    if (getenv("NETPARSE_BENCH") != NULL)
        return bench();
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
