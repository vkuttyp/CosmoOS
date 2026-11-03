/*
 * test_ipv6.c - IPv6 input and output and ICMPv6 (kernel-services/network/
 * ipv6.c) on the host: roadmap 2.0 SA6. A digest over seeded histories,
 * recorded from the C, of a loopback and an Ethernet interface: datagrams in
 * (well-formed or not, every next header, padded, short, split across two
 * mbufs, with m_pullup's allocation failing; to us, to all-nodes, to our
 * solicited-node group or another group, to another host; from a loopback or
 * our own address; ICMPv6 with a good or a bad checksum, echoes that are
 * answered or rate-limited, solicitations and advertisements with and without
 * the hop limit ND requires), datagrams out (to the loopback, to our own
 * address, link-local and multicast, unroutable; from a given, an unspecified
 * or no source; up to and past the MTU; with checksum offload; through ND's
 * answers: resolved, pending, refused; with and without the headroom the
 * header needs), the routing and source choices, and the statistics.
 *
 * mbuf.c, the checksum and netparse are real; the interface registry, ND,
 * UDP, TCP, the transmits and the rate limit are stubs that fold each call
 * into the digest. Every mbuf is freed and every interface reference given
 * back at the end.
 *
 * IPV6_BENCH=1 times an echo answered and a datagram sent.
 */

#include "harness.h"

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/mbuf.h>
#include <kernel/net/cksum.h>
#include <kernel/net/ether.h>
#include <kernel/net/ip.h>
#include <kernel/net/tcp.h>
#include <kernel/net/udp.h>
#include <kernel/netif.h>
#include <kernel/object.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The digest of the C (ipv6.c, 2026-11-03), which the CeeSafe port gives. */
#define IPV6_DIGEST 0x58f5b59e3be23bccull
#define ROUNDS 20000u
#define SEED 0xC05F5FA2026B0011ull

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

/* A call to a stub: which, and a value. */
static void note(uint32_t which, uint64_t value)
{
    if (g_benching)
        return;
    mix32(0x5a00u + which);
    mix64(value);
}

/* mbuf.c's caches, over malloc; an allocation fails when g_fail_in counts down to it. */
static int g_fail_in = -1;
static long g_live;

struct kmem_cache *kmem_cache_create(const char *name, size_t size, size_t align)
{
    (void)align;
    struct kmem_cache *c = calloc(1, sizeof(*c));
    c->name = name;
    c->object_size = size;
    return c;
}

void *kmem_cache_alloc(struct kmem_cache *c, unsigned flags)
{
    (void)flags;
    if (g_fail_in >= 0 && g_fail_in-- == 0)
        return NULL;
    g_live++;
    return malloc(c->object_size);
}

void kmem_cache_free(struct kmem_cache *c, void *p)
{
    (void)c;
    if (p != NULL)
        g_live--;
    free(p);
}

/* ---- the kernel services ipv6.c calls ---- */

static struct netif g_lo, g_eth;
static struct netif *g_default;   /* netif_default's answer: g_eth, or NULL now and then */
static long g_refs[2];

static unsigned nif_index(const struct netif *nif) { return nif == &g_lo ? 0 : 1; }

void kobject_get(struct kobject *obj) { g_refs[nif_index(container_of(obj, struct netif, obj))]++; }
void kobject_put(struct kobject *obj) { g_refs[nif_index(container_of(obj, struct netif, obj))]--; }

struct netif *netif_loopback(void)
{
    netif_get(&g_lo);
    return &g_lo;
}

struct netif *netif_default(void)
{
    if (g_default != NULL)
        netif_get(g_default);
    return g_default;
}

bool netif_owns_ipv6(const struct in6_addr *a)
{
    bool owns = memcmp(a, &g_eth.ip6_ll, 16) == 0;
    note(1, owns);
    return owns;
}

/* What leaves: the interface, the bytes, the packet header's fields. */
static void sent(uint32_t which, const struct netif *nif, struct mbuf *m)
{
    if (!g_benching) {
        uint8_t bytes[96];
        uint32_t n = m_length(m);
        uint32_t take = n < sizeof(bytes) ? n : (uint32_t)sizeof(bytes);
        m_copydata(m, 0, take, bytes);
        note(which, nif_index(nif));
        mix32(n);
        mix32(m->pkt.len);
        mix32(m->flags & (M_BCAST | M_MCAST));
        mix(&m->pkt.proto, sizeof(m->pkt.proto));
        mix(&m->pkt.csum_flags, sizeof(m->pkt.csum_flags));
        mix(&m->pkt.csum_start, sizeof(m->pkt.csum_start));
        mix(bytes, take);
    }
    m_freem(m);
}

static int g_tx_rc;

int netif_transmit(struct netif *nif, struct mbuf *m)
{
    sent(2, nif, m);
    return g_tx_rc;
}

int ether_output(struct netif *nif, struct mbuf *m, const uint8_t *dst, uint16_t type)
{
    if (!g_benching) {
        mix(dst, ETH_ALEN);
        mix32(type);
    }
    sent(3, nif, m);
    return g_tx_rc;
}

/* ND's answer: resolved (0, a MAC), pending (it keeps the packet), or refused (it frees it). */
static int g_nd_answer;

int nd_resolve(struct netif *nif, const struct in6_addr *ip, uint8_t *mac, struct mbuf *m)
{
    note(4, nif_index(nif));
    if (!g_benching)
        mix(ip, 16);
    if (g_nd_answer == 0) {
        memset(mac, 0x42, ETH_ALEN);
        return 0;
    }
    if (g_nd_answer == -EINPROGRESS)
        sent(5, nif, m);   /* parked: it is ND's now */
    else
        m_freem(m);
    return g_nd_answer;
}

void nd_input_ns(struct netif *nif, struct mbuf *m, const struct ipv6_hdr *ip6)
{
    if (!g_benching)
        mix(&ip6->src, 16);
    sent(6, nif, m);
}

void nd_input_na(struct netif *nif, struct mbuf *m, const struct ipv6_hdr *ip6)
{
    if (!g_benching)
        mix(&ip6->dst, 16);
    sent(7, nif, m);
}

uint64_t nd_pending_count(void) { return 7; }

void udp_input(struct netif *nif, struct mbuf *m, const struct ipv4_hdr *ip4, const struct ipv6_hdr *ip6)
{
    note(8, ip4 == NULL);
    if (!g_benching)
        mix(ip6, sizeof(*ip6));
    sent(9, nif, m);
}

void tcp_input(struct netif *nif, struct mbuf *m, const struct ipv4_hdr *ip4, const struct ipv6_hdr *ip6)
{
    note(10, ip4 == NULL);
    if (!g_benching)
        mix(ip6, sizeof(*ip6));
    sent(11, nif, m);
}

static bool g_rate_ok = true;

bool icmp_ratelimit_allow(void)
{
    note(12, g_rate_ok);
    return g_rate_ok;
}

/* ---- the histories ---- */

static const struct in6_addr g_all_nodes = { { 0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 } };
static const struct in6_addr g_loopback = { { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 } };

/* An address of one kind: ours, the loopback, all-nodes, our solicited-node group, another group, link-local, global,
 * unspecified. */
static struct in6_addr address(void)
{
    struct in6_addr a;
    for (unsigned i = 0; i < 16; i++)
        a.s6_addr[i] = (uint8_t)next();
    switch (next() % 9) {
    case 0: return g_eth.ip6_ll;
    case 1: return g_loopback;
    case 2: return g_all_nodes;
    case 3:
        memcpy(a.s6_addr, (const uint8_t[]){ 0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0xff }, 13);
        memcpy(a.s6_addr + 13, g_eth.ip6_ll.s6_addr + 13, 3);
        uint64_t near = next() % 6;
        if (near == 0)
            a.s6_addr[11] = 2;   /* almost: not the solicited-node prefix */
        else if (near == 1)
            a.s6_addr[13 + next() % 3] ^= 1;   /* another node's group: one of the last three bytes differs */
        return a;
    case 4: a.s6_addr[0] = 0xff; return a;
    case 5: a.s6_addr[0] = 0xfe; a.s6_addr[1] = 0x80; return a;
    case 6: memset(&a, 0, sizeof(a)); return a;
    default: a.s6_addr[0] = 0x20; return a;
    }
}

/* An mbuf holding `bytes`, in one buffer or split across two (at `at`, or anywhere when 0), with or without a
 * cluster's headroom. */
static struct mbuf *packet_at(const uint8_t *bytes, uint32_t len, uint32_t at)
{
    struct mbuf *m = next() % 2 ? m_getcl() : m_get();
    EXPECT(m != NULL);
    m->flags |= M_PKTHDR;
    uint32_t split = at != 0 && at < len ? at : len > 1 && next() % 4 == 0 ? (uint32_t)(1 + next() % (len - 1)) : len;
    m_append(m, bytes, split);
    if (split < len) {
        struct mbuf *tail = m_getcl();   /* room for the rest of a 200-byte datagram */
        EXPECT(tail != NULL);
        memcpy(tail->data, bytes + split, len - split);
        tail->len = len - split;
        struct mbuf *last = m;
        while (last->next != NULL)
            last = last->next;   /* m_append may have chained more than one mbuf already */
        last->next = tail;
        m->pkt.len = len;
    }
    return m;
}

static struct mbuf *packet(const uint8_t *bytes, uint32_t len) { return packet_at(bytes, len, 0); }

static void put16(uint8_t *b, uint16_t v)
{
    b[0] = (uint8_t)(v >> 8);
    b[1] = (uint8_t)v;
}

/* An ICMPv6 message's checksum over the pseudo-header, as a sender computes it. */
static void icmp_checksum(uint8_t *msg, uint32_t len, const struct in6_addr *src, const struct in6_addr *dst)
{
    msg[2] = msg[3] = 0;
    uint32_t sum = cksum_pseudo6(src, dst, IPPROTO_ICMPV6, len);
    uint16_t c = cksum_fold(cksum_partial(msg, len, sum));
    memcpy(msg + 2, &c, 2);
}

static void op_input(unsigned r)
{
    uint8_t d[200];
    uint32_t plen = (uint32_t)(next() % 120);
    for (unsigned i = 0; i < sizeof(d); i++)
        d[i] = (uint8_t)next();
    d[0] = (uint8_t)(next() % 16 == 0 ? next() : 0x60);
    d[1] = d[2] = d[3] = 0;
    uint64_t nk = next() % 8;
    d[6] = nk < 4 ? IPPROTO_ICMPV6 : nk < 5 ? IPPROTO_UDP : nk < 6 ? IPPROTO_TCP : (uint8_t)next();
    d[7] = next() % 4 ? 255 : (uint8_t)next();
    struct in6_addr src = address(), dst = address();
    memcpy(d + 8, &src, 16);
    memcpy(d + 24, &dst, 16);
    if (d[6] == IPPROTO_ICMPV6 && plen >= 8) {
        uint64_t t = next() % 6;
        d[40] = t < 2 ? ICMPV6_ECHO : t < 3 ? ICMPV6_NS : t < 4 ? ICMPV6_NA : (uint8_t)next();
        d[41] = 0;
        if (next() % 8)
            icmp_checksum(d + 40, plen, &src, &dst);
    }
    uint64_t lk = next() % 16;
    uint32_t pad = lk < 3 ? (uint32_t)(next() % 32) : 0;                  /* a padded frame */
    put16(d + 4, (uint16_t)(lk == 3 ? plen + 1 + next() % 8 : plen));    /* or a payload length that lies */
    uint32_t len = lk == 4 ? (uint32_t)(next() % 40) : 40 + plen + pad;  /* or a header cut short */
    if (len > sizeof(d))
        len = sizeof(d);
    bool icmp_split = r % 53 == 0 && d[6] == IPPROTO_ICMPV6 && plen >= 8;
    struct mbuf *m = packet_at(d, len, icmp_split ? 41 + (uint32_t)(next() % 7) : 0);   /* the ICMPv6 header across two */
    struct netif *nif = next() % 6 == 0 ? &g_lo : &g_eth;
    g_rate_ok = next() % 5 != 0;
    g_tx_rc = 0;
    g_nd_answer = 0;
    if (icmp_split)
        g_fail_in = 0;                   /* the ICMPv6 header's m_pullup */
    else if (r % 71 == 0)
        g_fail_in = (int)(next() % 3);   /* m_pullup's, or the echo reply's prepend */
    ipv6_input(nif, m);
    g_fail_in = -1;
}

static void op_output(unsigned r)
{
    struct in6_addr dst = address(), src = address();
    uint32_t len = next() % 8 == 0 ? (uint32_t)(1400 + next() % 200) : (uint32_t)(next() % 300);
    if (r % 211 == 0)
        len = 65536 + (uint32_t)(next() % 64);   /* past what a payload length can say */
    static uint8_t payload[65600];
    for (uint32_t i = 0; i < len; i++)
        payload[i] = (uint8_t)next();
    struct mbuf *m = next() % 4 ? m_getcl() : m_get();   /* without headroom, m_prepend allocates */
    EXPECT(m != NULL);
    m->flags |= M_PKTHDR;
    m_append(m, payload, len);
    m->pkt.csum_flags = (uint16_t)(next() % 4);
    m->pkt.csum_start = (uint16_t)(next() % 64);
    uint64_t sk = next() % 4;
    const struct in6_addr *sp = sk == 0 ? NULL : &src;
    uint64_t na = next() % 4;
    g_nd_answer = na < 2 ? 0 : na == 2 ? -EINPROGRESS : -ENETUNREACH;
    g_tx_rc = (int)(next() % 3) - 1;
    g_default = next() % 8 == 0 ? NULL : &g_eth;
    if (r % 67 == 0)
        g_fail_in = 0;                   /* the header's prepend, when it needs an allocation */
    uint8_t proto = (uint8_t)(next() % 2 ? IPPROTO_UDP : IPPROTO_TCP);   /* drawn one after the other */
    uint8_t hops = (uint8_t)next();
    int rc = ipv6_output(m, sp, &dst, proto, hops);
    g_fail_in = -1;
    g_default = &g_eth;
    note(20, (uint64_t)(int64_t)rc);
}

static void op_route(void)
{
    struct in6_addr dst = address(), src;
    g_default = next() % 8 == 0 ? NULL : &g_eth;
    memset(&src, 0xee, sizeof(src));
    ipv6_source_for(&dst, &src);
    mix(&src, sizeof(src));
    struct netif *nif = ipv6_route(&dst);
    note(21, nif == NULL ? 2 : nif_index(nif));
    if (nif != NULL)
        netif_put(nif);
    g_default = &g_eth;
}

static void op_stats(void)
{
    struct ip_stats s;
    memset(&s, 0x33, sizeof(s));
    ipv6_get_stats(&s);
    mix(&s, sizeof(s));
}

static void setup(void)
{
    static bool done;
    if (!done) {
        mbuf_init();
        done = true;
    }
    memset(&g_lo, 0, sizeof(g_lo));
    memset(&g_eth, 0, sizeof(g_eth));
    memcpy(g_lo.name, "lo", 3);
    memcpy(g_eth.name, "eth0", 5);
    g_lo.flags = NETIF_UP | NETIF_LOOPBACK;
    g_lo.mtu = 65536;
    g_eth.flags = NETIF_UP;
    g_eth.mtu = 1500;
    for (unsigned i = 0; i < ETH_ALEN; i++)
        g_eth.mac[i] = (uint8_t)next();
    g_eth.ip6_ll.s6_addr[0] = 0xfe;
    g_eth.ip6_ll.s6_addr[1] = 0x80;
    for (unsigned i = 8; i < 16; i++)
        g_eth.ip6_ll.s6_addr[i] = (uint8_t)next();
    g_lo.ip6_ll = g_loopback;
    g_default = &g_eth;
}

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    setup();
    for (unsigned r = 0; r < ROUNDS; r++) {
        uint64_t k = next() % 16;
        if (k < 8)
            op_input(r);
        else if (k < 13)
            op_output(r);
        else if (k < 15)
            op_route();
        else
            op_stats();
    }
    op_stats();
    mix64((uint64_t)g_refs[0]);
    mix64((uint64_t)g_refs[1]);
    printf("  ipv6 digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, ROUNDS);
    EXPECT(g_digest == IPV6_DIGEST);
    EXPECT(g_live == 0);
    EXPECT(g_refs[0] == 0 && g_refs[1] == 0);
}

/* IPV6_BENCH=1: the median of nine runs, 100,000 packets each. */
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

static void ipv6_bench(void)
{
    g_rng = SEED;
    setup();
    g_benching = true;
    g_rate_ok = true;
    g_nd_answer = 0;
    g_tx_rc = 0;
    /* An echo request to our address, from a link-local neighbour. */
    uint8_t echo[40 + 64];
    memset(echo, 0, sizeof(echo));
    echo[0] = 0x60;
    put16(echo + 4, 64);
    echo[6] = IPPROTO_ICMPV6;
    echo[7] = 255;
    struct in6_addr peer = g_eth.ip6_ll;
    peer.s6_addr[15] ^= 1;
    memcpy(echo + 8, &peer, 16);
    memcpy(echo + 24, &g_eth.ip6_ll, 16);
    echo[40] = ICMPV6_ECHO;
    icmp_checksum(echo + 40, 64, &peer, &g_eth.ip6_ll);
    uint8_t payload[512] = { 0 };
    double in[9], out[9];
    for (unsigned t = 0; t < 9; t++) {
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 100000; i++) {
            struct mbuf *m = m_getcl();
            m_append(m, echo, sizeof(echo));
            ipv6_input(&g_eth, m);
        }
        in[t] = ns_since(&t0) / 100000;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 100000; i++) {
            struct mbuf *m = m_getcl();
            m_append(m, payload, sizeof(payload));
            (void)ipv6_output(m, &g_eth.ip6_ll, &peer, IPPROTO_UDP, 64);
        }
        out[t] = ns_since(&t0) / 100000;
    }
    qsort(in, 9, sizeof(double), cmp_double);
    qsort(out, 9, sizeof(double), cmp_double);
    g_benching = false;
    printf("  bench ipv6_input (an echo answered) %.1f ns, ipv6_output (512 bytes, resolved) %.1f ns\n", in[4], out[4]);
}

static const struct host_test tests[] = {
    { "ipv6 digest", test_digest },
};

int main(void)
{
    int rc = harness_run(tests, sizeof(tests) / sizeof(tests[0]));
    if (getenv("IPV6_BENCH") != NULL)
        ipv6_bench();
    return rc;
}
