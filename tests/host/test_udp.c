/*
 * test_udp.c - UDP over IPv4 and IPv6 (kernel-services/network/udp.c) on the
 * host: roadmap 2.0 SA6. A digest over seeded histories, recorded from the C,
 * of eight sockets of both families:
 *
 *   - pcbs initialised, bound (given ports, ephemeral ones wrapping at the
 *     top of the range, to our addresses, to the wildcard, to addresses that
 *     are not ours, to the wrong family, twice), connected, unbound with
 *     datagrams queued;
 *   - datagrams out: to the wrong family, port zero, the unspecified
 *     address, past 65,527 bytes, from unbound pcbs (an ephemeral bind),
 *     from wildcard pcbs with and without a source, with allocations failing;
 *   - datagrams in, over IPv4 and IPv6: short, with lengths that lie or pad,
 *     split across mbufs (m_pullup failing), checksums good, absent, wrong or
 *     offloaded; to bound ports (exact beats wildcard, connected pcbs only
 *     from their peer), to no port (a port-unreachable unless broadcast or
 *     IPv6, the prepend failing), quiet ones from the firewall; queues full;
 *     sockets being released;
 *   - ICMP errors notified, the port-in-use question NAT asks, the
 *     /proc/net listing with every capacity, and the statistics.
 *
 * mbuf.c, the checksum, netparse and inet.c's addresses are real; the
 * interfaces, IP output, ICMP and the sockets are stubs that fold each call
 * into the digest. Every mbuf is freed, every wake reference given back and
 * no lock held at the end. Not reached: the ephemeral range exhausted, which
 * takes 16,383 bound sockets.
 *
 * UDP_BENCH=1 times a datagram received and read, and a datagram sent.
 */

#include "harness.h"

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/mbuf.h>
#include <kernel/net/cksum.h>
#include <kernel/net/inet.h>
#include <kernel/net/ip.h>
#include <kernel/net/udp.h>
#include <kernel/netif.h>
#include <kernel/socket.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The digest of the C (udp.c, 2026-11-03), which the CeeSafe port gives. */
#define UDP_DIGEST 0x653e074bd035e785ull
#define ROUNDS 20000u
#define SEED 0xC05F5FA2026B0017ull
#define NPCB 8u

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
    mix32(0x5c00u + which);
    mix64(value);
}

/* mbuf.c's caches over malloc; an allocation fails when g_fail_in counts down to it. */
static int g_fail_in = -1;
static long g_live;

static bool fail_now(void) { return g_fail_in >= 0 && g_fail_in-- == 0; }

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
    if (fail_now())
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

/* ---- the stubs ---- */

static uint64_t g_random;

uint64_t random_u64(void) { return g_random; }

static uint64_t g_policy;   /* bits drawn per round: which stubs refuse */

static bool refuses(unsigned bit) { return (g_policy >> bit) & 1; }

#define A(a, b, c, d) IPV4_ADDR(a, b, c, d)

static const struct in6_addr V6_OURS = { { 0xfe, 0x80, [15] = 1 } };
static const struct in6_addr V6_LOOP = { { [15] = 1 } };
static const struct in6_addr V6_OTHER = { { 0x20, 0x01, 0x0d, 0xb8, [15] = 5 } };
static const struct in6_addr V6_PEER = { { 0x20, 0x01, 0x0d, 0xb8, [15] = 7 } };
static const struct in6_addr V6_PEER2 = { { 0x20, 0x01, 0x0d, 0xb8, [15] = 8 } };

bool netif_owns_ipv4(uint32_t addr)
{
    bool owns = addr == A(10, 0, 2, 15) || addr == A(127, 0, 0, 1);
    note(1, owns);
    return owns;
}

bool netif_owns_ipv6(const struct in6_addr *a)
{
    bool owns = in6_equal(a, &V6_OURS) || in6_equal(a, &V6_LOOP);
    note(2, owns);
    return owns;
}

uint32_t ipv4_source_for(uint32_t dst)
{
    note(3, dst);
    return refuses(0) ? 0 : A(10, 0, 2, 15);
}

void ipv6_source_for(const struct in6_addr *dst, struct in6_addr *src)
{
    if (!g_benching)
        mix(dst, sizeof(*dst));
    note(4, 0);
    if (refuses(0))
        memset(src, 0, sizeof(*src));
    else
        *src = V6_OURS;
}

static void sent(uint32_t which, struct mbuf *m)
{
    if (!g_benching) {
        uint8_t bytes[96];
        uint32_t n = m_length(m);
        uint32_t take = n < sizeof(bytes) ? n : (uint32_t)sizeof(bytes);
        m_copydata(m, 0, take, bytes);
        note(which, n);
        mix32(m->pkt.len);
        mix(bytes, take);
    }
    m_freem(m);
}

static int g_out_rc;

int ipv4_output(struct mbuf *m, uint32_t src, uint32_t dst, uint8_t proto, uint8_t ttl)
{
    note(5, ((uint64_t)src << 32) | dst);
    note(6, ((uint64_t)proto << 8) | ttl);
    sent(7, m);
    return g_out_rc;
}

int ipv6_output(struct mbuf *m, const struct in6_addr *src, const struct in6_addr *dst, uint8_t proto, uint8_t hoplimit)
{
    if (!g_benching) {
        mix(src, sizeof(*src));
        mix(dst, sizeof(*dst));
    }
    note(8, ((uint64_t)proto << 8) | hoplimit);
    sent(9, m);
    return g_out_rc;
}

void icmp_send_unreach(struct mbuf *orig, const struct ipv4_hdr *iph, uint8_t code)
{
    uint8_t bytes[96];
    uint32_t n = m_length(orig);
    uint32_t take = n < sizeof(bytes) ? n : (uint32_t)sizeof(bytes);
    m_copydata(orig, 0, take, bytes);
    note(10, ((uint64_t)n << 8) | code);
    mix(bytes, take);
    mix(iph, sizeof(*iph));
}

/* The sockets: one per pcb, some being released (a tryget fails). */
static struct socket g_socks[NPCB];
static bool g_dying[NPCB];
static long g_wake_refs;

static unsigned sock_index(const struct socket *s) { return (unsigned)(s - g_socks); }

struct socket *sock_wake_ref(struct socket *s)
{
    if (s == NULL) {
        note(11, NPCB);
        return NULL;
    }
    unsigned i = sock_index(s);
    note(11, ((uint64_t)g_dying[i] << 8) | i);
    if (g_dying[i])
        return NULL;
    g_wake_refs++;
    return s;
}

void sock_wake_unref(struct socket *s)
{
    note(12, sock_index(s));
    g_wake_refs--;
}

void sock_wake(struct socket *s) { note(13, sock_index(s)); }

void sock_set_error(struct socket *s, int err) { note(14, ((uint64_t)sock_index(s) << 32) | (uint32_t)err); }

/* ---- the histories ---- */

static struct udp_pcb *pcb(unsigned i) { return &g_socks[i].udp; }

static const uint16_t PORTS[] = { 0, 53, 68, 5353, 49152, 49153, 65535, 7 };

static uint16_t some_port(void)
{
    if (next() % 4 == 0) {
        struct udp_pcb *p = pcb((unsigned)(next() % NPCB));
        if (p->local.port != 0)
            return p->local.port;
    }
    return PORTS[next() % (sizeof(PORTS) / sizeof(PORTS[0]))];
}

/* An address of `family`: ours, the loopback, someone else's, a peer, another peer, the wildcard. */
static void address(struct netaddr *a, uint16_t family, uint16_t port)
{
    memset(a, 0, sizeof(*a));
    a->family = family;
    a->port = port;
    unsigned k = (unsigned)(next() % 6);
    if (family == COSMO_AF_INET) {
        const uint32_t v4[6] = { A(10, 0, 2, 15), A(127, 0, 0, 1), A(10, 0, 2, 99), A(10, 0, 2, 7),
                                        A(10, 0, 2, 8), 0 };
        a->v4 = v4[k];
    } else {
        const struct in6_addr *v6[6] = { &V6_OURS, &V6_LOOP, &V6_OTHER, &V6_PEER, &V6_PEER2, NULL };
        if (v6[k] != NULL)
            a->v6 = *v6[k];
    }
}

static uint16_t some_family(void) { return next() % 2 ? COSMO_AF_INET : COSMO_AF_INET6; }

static void init_pcb(unsigned i, uint16_t family)
{
    mix32((uint32_t)udp_pcb_init(pcb(i), family));
    pcb(i)->sock = i == NPCB - 1 ? NULL : &g_socks[i];   /* the last one's socket is gone */
}

static void op_init(void)
{
    unsigned i = (unsigned)(next() % NPCB);
    if (pcb(i)->local.port == 0 && mbufq_len(&pcb(i)->rxq) == 0)
        init_pcb(i, some_family());
}

static void op_bind(void)
{
    unsigned i = (unsigned)(next() % NPCB);
    struct netaddr a;
    uint16_t family = next() % 8 == 0 ? some_family() : pcb(i)->local.family;
    uint16_t port = some_port();
    address(&a, family, port);
    if (next() % 3 == 0)
        memset(&a.v6, 0, sizeof(a.v6));   /* the wildcard, more often */
    int rc = udp_bind(pcb(i), &a);
    note(20, ((uint64_t)i << 32) | (uint32_t)rc);
    mix32(pcb(i)->local.port);
}

static void op_unbind(void)
{
    unsigned i = (unsigned)(next() % NPCB);
    udp_unbind(pcb(i));
    note(21, i);
}

static void op_connect(void)
{
    unsigned i = (unsigned)(next() % NPCB);
    struct netaddr a;
    uint16_t port = next() % 4 == 0 ? 0 : (next() % 2 ? 1234 : 53);
    address(&a, pcb(i)->local.family, port);
    udp_set_remote(pcb(i), &a);
    note(22, i);
}

static uint8_t g_data[4096];   /* past a cluster: m_append chains, and can fail */

static void op_send(void)
{
    unsigned i = (unsigned)(next() % NPCB);
    struct netaddr to;
    uint16_t family = next() % 8 == 0 ? some_family() : pcb(i)->local.family;
    uint16_t port = next() % 8 == 0 ? 0 : (next() % 2 ? 1234 : 53);
    address(&to, family, port);
    size_t len;
    switch (next() % 8) {
    case 0: len = 65535 - 8 + next() % 2; break;   /* the largest, and one past it */
    case 1: len = sizeof(g_data); break;
    case 2: len = 0; break;
    default: len = next() % 200; break;
    }
    if (len > sizeof(g_data) && len <= 65535 - 8)
        len = sizeof(g_data);   /* only the size check needs the real 65,527 */
    for (size_t k = 0; k < len && k < 64; k++)
        g_data[k] = (uint8_t)next();
    g_policy = next();
    if (pcb(i)->local.port != 0 && to.family == pcb(i)->local.family && to.port != 0 && next() % 8 == 0) {
        /* Two bytes that make the sum come out zero: sent as 0xffff. */
        struct netaddr src = pcb(i)->local;
        if (netaddr_is_unspecified(&src)) {
            src.v4 = A(10, 0, 2, 15);
            src.v6 = V6_OURS;
        }
        uint8_t h[8] = { (uint8_t)(src.port >> 8), (uint8_t)src.port, (uint8_t)(to.port >> 8), (uint8_t)to.port, 0, 10 };
        uint32_t sum = to.family == COSMO_AF_INET ? cksum_pseudo4(src.v4, to.v4, IPPROTO_UDP, 10)
                                                  : cksum_pseudo6(&src.v6, &to.v6, IPPROTO_UDP, 10);
        uint16_t w = cksum_fold(cksum_partial(h, sizeof(h), sum));   /* the complement of the sum so far */
        memcpy(g_data, &w, 2);
        len = 2;
        g_policy &= ~1ull;   /* a source */
    }
    g_out_rc = refuses(1) ? -ENOBUFS : 0;
    g_fail_in = next() % 8 == 0 ? (int)(next() % 3) : -1;
    int rc = udp_sendto(pcb(i), g_data, len, &to);
    g_fail_in = -1;
    note(23, ((uint64_t)i << 32) | (uint32_t)rc);
    mix32(pcb(i)->local.port);
}

static void op_recv(void)
{
    unsigned i = (unsigned)(next() % NPCB);
    struct mbuf *m = udp_recv(pcb(i));
    if (m == NULL) {
        note(24, i);
        return;
    }
    mix(&m->pkt.src, sizeof(m->pkt.src));
    sent(25, m);
}

/* An mbuf holding `bytes`, sometimes split across two (the tail at the end of the chain). */
static struct mbuf *packet(const uint8_t *bytes, uint32_t len)
{
    struct mbuf *m = next() % 2 ? m_getcl() : m_get();
    EXPECT(m != NULL);
    m->flags |= M_PKTHDR;
    uint32_t split = len > 1 && next() % 4 == 0 ? (uint32_t)(1 + next() % (len - 1)) : len;
    m_append(m, bytes, split);
    if (split < len) {
        struct mbuf *tail = m_getcl();
        EXPECT(tail != NULL);
        memcpy(tail->data, bytes + split, len - split);
        tail->len = len - split;
        struct mbuf *last = m;
        while (last->next != NULL)
            last = last->next;
        last->next = tail;
        m->pkt.len = len;
    }
    return m;
}

static void op_input(void)
{
    uint16_t family = some_family();
    struct netaddr src, dst;
    address(&src, family, next() % 2 ? 1234 : 53);
    address(&dst, family, some_port());
    if (family == COSMO_AF_INET && next() % 8 == 0)
        dst.v4 = INADDR_BROADCAST_N;

    uint32_t payload = next() % 8 == 0 ? 1200 + (uint32_t)(next() % 200) : (uint32_t)(next() % 100);
    uint32_t ulen = 8 + payload;
    uint32_t len = ulen;
    uint32_t lie = (uint32_t)(next() % 8);
    if (lie == 0)
        len = ulen + 1 + (uint32_t)(next() % 16);   /* padded: trimmed */
    static uint8_t d[1500];
    for (uint32_t k = 8; k < len; k++)
        d[k] = (uint8_t)next();
    uint16_t sport = src.port, dport = dst.port;
    d[0] = (uint8_t)(sport >> 8);
    d[1] = (uint8_t)sport;
    d[2] = (uint8_t)(dport >> 8);
    d[3] = (uint8_t)dport;
    uint32_t field = lie == 1 ? (uint32_t)(next() % 8) : lie == 2 ? len + 1 + (uint32_t)(next() % 4) : ulen;
    d[4] = (uint8_t)(field >> 8);
    d[5] = (uint8_t)field;
    d[6] = d[7] = 0;

    uint8_t ip4b[60];
    struct ipv4_hdr *ip4 = (struct ipv4_hdr *)ip4b;
    struct ipv6_hdr ip6;
    uint32_t sum;
    if (family == COSMO_AF_INET) {
        unsigned ihl = next() % 4 == 0 ? 20 + 4 * (unsigned)(next() % 11) : 20;
        for (unsigned k = 0; k < ihl; k++)
            ip4b[k] = (uint8_t)next();
        ip4->vhl = (uint8_t)(0x40 | ihl / 4);
        ip4->proto = IPPROTO_UDP;
        ip4->src = src.v4;
        ip4->dst = dst.v4;
        sum = cksum_pseudo4(src.v4, dst.v4, IPPROTO_UDP, (uint16_t)ulen);
    } else {
        memset(&ip6, 0, sizeof(ip6));
        ip6.nexthdr = IPPROTO_UDP;
        ip6.src = src.v6;
        ip6.dst = dst.v6;
        sum = cksum_pseudo6(&src.v6, &dst.v6, IPPROTO_UDP, ulen);
    }
    unsigned ck = (unsigned)(next() % 8);   /* 0: none, 1: wrong, else right */
    if (ck != 0 && ulen <= len) {
        uint16_t c = cksum_fold(cksum_partial(d, ulen, sum));
        if (c == 0)
            c = 0xffff;
        if (ck == 1)
            c ^= (uint16_t)(1 + next() % 0xfffe);
        d[6] = (uint8_t)(c >> 8);
        d[7] = (uint8_t)c;
    }
    uint32_t total = next() % 16 == 0 ? (uint32_t)(next() % 8) : len;   /* shorter than a header */
    struct mbuf *m = packet(d, total);
    uint32_t flags = (uint32_t)next();
    m->flags |= flags & (M_BCAST | M_CSUM_OK | M_FW_QUIET);
    if (next() % 4 != 0)
        m->flags &= ~M_FW_QUIET;
    for (unsigned i = 0; i < NPCB; i++)
        g_dying[i] = next() % 8 == 0;
    g_fail_in = next() % 8 == 0 ? (int)(next() % 3) : -1;
    note(26, ((uint64_t)family << 32) | total);
    udp_input(NULL, m, family == COSMO_AF_INET ? ip4 : NULL, family == COSMO_AF_INET ? NULL : &ip6);
    g_fail_in = -1;
    for (unsigned i = 0; i < NPCB; i++)
        g_dying[i] = false;
}

/* Seventy datagrams for one bound pcb, past its queue of 64. */
static void op_flood(void)
{
    unsigned i = (unsigned)(next() % NPCB);
    struct udp_pcb *p = pcb(i);
    if (p->local.port == 0)
        return;
    struct netaddr dst = p->local, src = p->remote;
    if (netaddr_is_unspecified(&dst)) {
        dst.v4 = A(10, 0, 2, 15);
        dst.v6 = V6_OURS;
    }
    if (src.port == 0) {
        src.family = p->local.family;
        src.port = 1234;
        src.v4 = A(10, 0, 2, 7);
        src.v6 = V6_PEER;
    }
    uint8_t d[8 + 16] = { (uint8_t)(src.port >> 8), (uint8_t)src.port, (uint8_t)(dst.port >> 8), (uint8_t)dst.port, 0,
                          sizeof(d), 0x12, 0x34 };   /* a checksum the interface verified */
    uint8_t ip4b[20] = { 0x45 };
    struct ipv4_hdr *ip4 = (struct ipv4_hdr *)ip4b;
    ip4->proto = IPPROTO_UDP;
    ip4->src = src.v4;
    ip4->dst = dst.v4;
    struct ipv6_hdr ip6;
    memset(&ip6, 0, sizeof(ip6));
    ip6.src = src.v6;
    ip6.dst = dst.v6;
    for (unsigned n = 0; n < 70; n++) {
        d[8] = (uint8_t)n;
        struct mbuf *m = m_get();
        m->flags |= M_PKTHDR | M_CSUM_OK;
        m_append(m, d, sizeof(d));
        udp_input(NULL, m, p->local.family == COSMO_AF_INET ? ip4 : NULL, p->local.family == COSMO_AF_INET ? NULL : &ip6);
    }
    note(30, ((uint64_t)i << 32) | mbufq_len(&p->rxq));
}

static void op_error(void)
{
    struct netaddr local, remote;
    unsigned i = (unsigned)(next() % NPCB);
    if (next() % 2) {
        local = pcb(i)->local;
        remote = pcb(i)->remote;
        if (netaddr_is_unspecified(&local))
            address(&local, local.family, local.port);
    } else {
        uint16_t family = some_family();
        address(&local, family, some_port());
        address(&remote, family, next() % 2 ? 1234 : 53);
    }
    g_dying[i] = next() % 4 == 0;
    bool took = udp_error_notify(&local, &remote, -(int)(1 + next() % 120));
    g_dying[i] = false;
    note(27, took);
}

static void op_in_use(void)
{
    struct netaddr a;
    uint16_t family = some_family();
    address(&a, family, some_port());
    note(28, udp_port_in_use(a.family, a.port, &a));
}

static void op_list(void)
{
    struct udp_sock_info out[NPCB + 1];
    memset(out, 0x5a, sizeof(out));
    unsigned max = (unsigned)(next() % (NPCB + 2));
    unsigned n = udp_list(out, max, some_family());
    note(29, ((uint64_t)max << 32) | n);
    for (unsigned k = 0; k < n && k < max; k++) {
        mix(&out[k].local, sizeof(out[k].local));
        mix(&out[k].remote, sizeof(out[k].remote));
        mix32(out[k].rxq);
        mix64(out[k].drops);
    }
}

static void op_stats(void)
{
    struct udp_stats s;
    memset(&s, 0x33, sizeof(s));
    udp_get_stats(&s);
    mix(&s, sizeof(s));
}

static void setup(void)
{
    static bool done;
    if (!done) {
        mbuf_init();
        done = true;
    }
    g_random = NET_EPHEMERAL_HI - NET_EPHEMERAL_LO - 3;   /* the ephemeral ports start four from the top */
    udp_init();
    for (unsigned i = 0; i < NPCB; i++)
        init_pcb(i, i % 2 ? COSMO_AF_INET6 : COSMO_AF_INET);
}

static void teardown(void)
{
    for (unsigned i = 0; i < NPCB; i++)
        udp_unbind(pcb(i));
}

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    setup();
    for (unsigned r = 0; r < ROUNDS; r++) {
        g_policy = 0;
        g_out_rc = 0;
        uint64_t k = next() % 32;
        if (k < 3)
            op_init();
        else if (k < 8)
            op_bind();
        else if (k < 10)
            op_unbind();
        else if (k < 12)
            op_connect();
        else if (k < 16)
            op_send();
        else if (k < 19)
            op_recv();
        else if (k < 27)
            op_input();
        else if (k < 29)
            op_error();
        else if (k < 30)
            op_in_use();
        else if (k < 31)
            op_list();
        else if (r % 2)
            op_flood();
        else
            op_stats();
        EXPECT(harness_locks_held() == 0);
    }
    op_stats();
    teardown();
    printf("  udp digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, ROUNDS);
    EXPECT(g_digest == UDP_DIGEST);
    EXPECT(g_live == 0);
    EXPECT(g_wake_refs == 0);
}

/* UDP_BENCH=1: the median of nine runs, 100,000 datagrams each. */
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

static void udp_bench(void)
{
    g_rng = SEED;
    setup();
    g_benching = true;
    g_policy = 0;
    g_out_rc = 0;
    /* A wildcard listener and a socket connected to the sender, on another port, ahead of it. */
    struct netaddr a = { .family = COSMO_AF_INET, .port = 5353 };
    EXPECT(udp_bind(pcb(0), &a) == 0);
    a.port = 9999;
    a.v4 = A(10, 0, 2, 15);
    EXPECT(udp_bind(pcb(2), &a) == 0);
    struct netaddr peer = { .family = COSMO_AF_INET, .port = 1234, .v4 = A(10, 0, 2, 7) };
    udp_set_remote(pcb(2), &peer);
    uint8_t d[8 + 512] = { 0 };
    d[0] = 1234 >> 8;
    d[1] = 1234 & 0xff;
    d[2] = 5353 >> 8;
    d[3] = 5353 & 0xff;
    d[4] = (uint8_t)(sizeof(d) >> 8);
    d[5] = (uint8_t)sizeof(d);
    uint16_t c = cksum_fold(cksum_partial(d, sizeof(d), cksum_pseudo4(peer.v4, a.v4, IPPROTO_UDP, sizeof(d))));
    d[6] = (uint8_t)(c >> 8);
    d[7] = (uint8_t)c;
    uint8_t ip4b[20] = { 0x45 };
    struct ipv4_hdr *ip4 = (struct ipv4_hdr *)ip4b;
    ip4->proto = IPPROTO_UDP;
    ip4->src = peer.v4;
    ip4->dst = a.v4;
    struct netaddr to = peer;
    double in[9], out[9];
    for (unsigned t = 0; t < 9; t++) {
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 100000; i++) {
            struct mbuf *m = m_getcl();
            m_append(m, d, sizeof(d));
            udp_input(NULL, m, ip4, NULL);
            m_freem(udp_recv(pcb(0)));
        }
        in[t] = ns_since(&t0) / 100000;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 100000; i++)
            (void)udp_sendto(pcb(0), g_data, 512, &to);
        out[t] = ns_since(&t0) / 100000;
    }
    qsort(in, 9, sizeof(double), cmp_double);
    qsort(out, 9, sizeof(double), cmp_double);
    teardown();
    g_benching = false;
    printf("  bench udp_input+udp_recv (512 bytes) %.1f ns, udp_sendto (512 bytes) %.1f ns\n", in[4], out[4]);
}

static const struct host_test tests[] = {
    { "udp digest", test_digest },
};

int main(void)
{
    int rc = harness_run(tests, sizeof(tests) / sizeof(tests[0]));
    if (getenv("UDP_BENCH") != NULL)
        udp_bench();
    return rc;
}
