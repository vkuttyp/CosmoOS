/*
 * test_nat.c - Masquerade NAT and port forwarding (kernel-services/network/
 * nat.c) on the host: roadmap 2.0 SA6. A digest over seeded histories,
 * recorded from the C, of ten guests on their taps (more than the table's
 * eight shares hold), an uplink and the world beyond it:
 *
 *   - datagrams out: UDP, TCP and ICMP echo masqueraded (checksums with and
 *     without, the guest's own port wanted, ports lent by host sockets),
 *     other ICMP and protocols refused, sources on the egress subnet and
 *     flows toward another tap left alone, DNAT replies rewritten back;
 *     floods past a guest's share and past the table, every port lent;
 *   - datagrams in: replies to masqueraded flows (TTLs that expire, padding
 *     past the total, split across mbufs, m_pullup failing), ICMP errors
 *     quoting them (bad checksums, short quotes), echo replies, and
 *     connections to forwarded ports (handshakes that establish and ones
 *     that do not, ambiguous replies, shares and the table full);
 *   - port-forward rules added (every refusal), deleted, cleared, listed,
 *     and parsed from configuration strings well and badly formed; guests
 *     purged; ageing, flushes, the flow listing and the statistics.
 *
 * mbuf.c, the checksum and netparse are real; the clock, the interfaces,
 * the host sockets and IPv4 output are the test's, the last folding each
 * datagram it is handed -- with whether its transport checksum verifies --
 * into the digest. Every mbuf is freed and every interface reference given
 * back at the end.
 *
 * NAT_BENCH=1 times a datagram masqueraded on an existing flow, and its reply.
 */

#include "harness.h"

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/mbuf.h>
#include <kernel/net/cksum.h>
#include <kernel/net/inet.h>
#include <kernel/net/ip.h>
#include <kernel/net/nat.h>
#include <kernel/net/tcp.h>
#include <kernel/net/udp.h>
#include <kernel/netif.h>
#include <kernel/object.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The digest of the C (nat.c, 2026-11-03), which the CeeSafe port gives. */
#define NAT_DIGEST 0xea7a64d4cbe6636aull
#define ROUNDS 20000u
#define SEED 0xC05F5FA2026B001Aull
#define NGUESTS 10u
#define NIFS (NGUESTS + 1u)
#define UPLINK NGUESTS

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
    mix32(0x5f00u + which);
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

static uint64_t g_now = 9000000000ull;

uint64_t clock_now_ns(void) { return g_now; }

/* ---- the network ---- */

#define A(a, b, c, d) IPV4_ADDR(a, b, c, d)

static struct netif g_nifs[NIFS];
static long g_refs[NIFS];

static uint32_t guest_ip(unsigned i) { return A(192, 168, 100 + i, 15); }
static uint32_t uplink_ip(void) { return A(10, 0, 2, 15); }

static unsigned nif_index(const struct netif *nif) { return (unsigned)(nif - g_nifs); }

void kobject_get(struct kobject *obj) { g_refs[nif_index(container_of(obj, struct netif, obj))]++; }
void kobject_put(struct kobject *obj) { g_refs[nif_index(container_of(obj, struct netif, obj))]--; }

/* The up interface whose subnet holds dst. */
struct netif *netif_connected(uint32_t dst)
{
    for (unsigned i = 0; i < NIFS; i++) {
        struct netif *n = &g_nifs[i];
        if (n->ip4.addr != 0 && ((dst ^ n->ip4.addr) & n->ip4.mask) == 0) {
            netif_get(n);
            note(1, i);
            return n;
        }
    }
    note(1, NIFS);
    return NULL;
}

/* The host's sockets: a port is held when the policy says so (or every port, to exhaust the range). */
static uint64_t g_policy;
static bool g_all_ports_taken;

static bool refuses(unsigned bit) { return (g_policy >> bit) & 1; }

static bool port_held(uint16_t port) { return g_all_ports_taken || (refuses(0) && port % 7 == 0); }

bool udp_port_in_use(uint16_t family, uint16_t port, const struct netaddr *addr)
{
    (void)family;
    (void)addr;
    return port_held(port);
}

bool tcp_port_in_use(uint16_t family, uint16_t port, const struct netaddr *addr)
{
    (void)family;
    (void)addr;
    return port_held((uint16_t)(port + 1));
}

/* Whether a datagram's transport checksum verifies: pseudo-header for TCP/UDP (UDP 0: none), plain for ICMP. */
static bool l4_ok(const uint8_t *l4, unsigned len, uint8_t proto, uint32_t src, uint32_t dst)
{
    if (proto == IPPROTO_UDP && len >= 8 && l4[6] == 0 && l4[7] == 0)
        return true;
    uint32_t sum = proto == IPPROTO_ICMP ? 0 : cksum_pseudo4(src, dst, proto, (uint16_t)len);
    return cksum_fold(cksum_partial(l4, len, sum)) == 0;
}

/* IPv4 output: what NAT forwards, folded in. */
int ipv4_output(struct mbuf *m, uint32_t src, uint32_t dst, uint8_t proto, uint8_t ttl)
{
    if (!g_benching) {
        static uint8_t bytes[1600];
        uint32_t n = m_length(m);
        EXPECT(n <= sizeof(bytes));
        m_copydata(m, 0, n, bytes);
        note(2, ((uint64_t)src << 32) | dst);
        note(3, ((uint64_t)n << 16) | ((uint64_t)proto << 8) | ttl);
        mix32(l4_ok(bytes, n, proto, src, dst));
        mix(bytes, n < 96 ? n : 96);
    }
    m_freem(m);
    return 0;
}

/* ---- datagrams ---- */

struct dgram {
    uint8_t proto;
    uint32_t src, dst;
    uint16_t sport, dport;   /* TCP/UDP; for ICMP echo, sport is the id */
    uint8_t tcp_flags;
    uint8_t icmp_type;
    uint8_t ttl;
    unsigned payload;
    unsigned cksum;          /* 0 right, 1 wrong, 2 none (UDP) */
};

static unsigned put_header(uint8_t *b, unsigned ihl, uint16_t total, uint8_t ttl, uint8_t proto, uint32_t src,
                           uint32_t dst)
{
    for (unsigned k = 20; k < ihl; k++)
        b[k] = (uint8_t)next();
    b[0] = (uint8_t)(0x40 | ihl / 4);
    b[1] = 0;
    b[2] = (uint8_t)(total >> 8);
    b[3] = (uint8_t)total;
    b[4] = b[5] = 0;
    b[6] = b[7] = 0;
    b[8] = ttl;
    b[9] = proto;
    b[10] = b[11] = 0;
    memcpy(b + 12, &src, 4);
    memcpy(b + 16, &dst, 4);
    uint16_t c = in_cksum(b, ihl);
    memcpy(b + 10, &c, 2);
    return ihl;
}

/* The transport header and payload at `l4`; its length. */
static unsigned put_l4(uint8_t *l4, const struct dgram *d)
{
    unsigned hl = d->proto == IPPROTO_TCP ? 20 : 8;
    unsigned len = hl + d->payload;
    for (unsigned k = 0; k < len; k++)
        l4[k] = (uint8_t)next();
    if (d->proto == IPPROTO_ICMP) {
        l4[0] = d->icmp_type;
        l4[1] = 0;
        l4[4] = (uint8_t)(d->sport >> 8);
        l4[5] = (uint8_t)d->sport;
    } else {
        l4[0] = (uint8_t)(d->sport >> 8);
        l4[1] = (uint8_t)d->sport;
        l4[2] = (uint8_t)(d->dport >> 8);
        l4[3] = (uint8_t)d->dport;
        if (d->proto == IPPROTO_UDP) {
            l4[4] = (uint8_t)(len >> 8);
            l4[5] = (uint8_t)len;
        } else if (d->proto == IPPROTO_TCP) {
            l4[12] = 0x50;
            l4[13] = d->tcp_flags;
        }
    }
    unsigned coff = d->proto == IPPROTO_TCP ? 16 : d->proto == IPPROTO_UDP ? 6 : 2;
    if (d->proto == IPPROTO_TCP || d->proto == IPPROTO_UDP || d->proto == IPPROTO_ICMP) {
        l4[coff] = l4[coff + 1] = 0;
        if (!(d->cksum == 2 && d->proto == IPPROTO_UDP)) {
            uint32_t sum = d->proto == IPPROTO_ICMP ? 0 : cksum_pseudo4(d->src, d->dst, d->proto, (uint16_t)len);
            uint16_t c = cksum_fold(cksum_partial(l4, len, sum));
            if (c == 0 && d->proto == IPPROTO_UDP)
                c = 0xffff;
            if (d->cksum == 1)
                c ^= 0x0101;
            memcpy(l4 + coff, &c, 2);
        }
    }
    return len;
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

static const uint16_t PORTS[] = { 53, 80, 443, 1234, 40007, 44444, 22 };

static uint16_t some_port(void) { return PORTS[next() % (sizeof(PORTS) / sizeof(PORTS[0]))]; }

static uint32_t world(void)
{
    const uint32_t W[] = { A(8, 8, 8, 8), A(1, 1, 1, 1), A(93, 184, 216, 34), A(10, 0, 2, 7) };
    return W[next() % 4];
}

static void some_dgram(struct dgram *d)
{
    memset(d, 0, sizeof(*d));
    static const uint8_t PROTOS[] = { IPPROTO_UDP, IPPROTO_UDP, IPPROTO_TCP, IPPROTO_TCP, IPPROTO_ICMP, 47 };
    d->proto = PROTOS[next() % 6];
    uint16_t sport = some_port();
    d->sport = next() % 4 == 0 ? (uint16_t)(1 + next() % 3) : sport;
    d->dport = some_port();
    static const uint8_t FLAGS[] = { TH_SYN, TH_SYN | TH_ACK, TH_ACK, TH_ACK | 0x08, TH_RST, TH_ACK | TH_RST };
    d->tcp_flags = FLAGS[next() % 6];
    static const uint8_t TYPES[] = { ICMP_ECHO, ICMP_ECHO, ICMP_ECHO_REPLY, 13 };
    d->icmp_type = TYPES[next() % 4];
    static const uint8_t TTLS[] = { 64, 64, 64, 2, 1, 0 };
    d->ttl = TTLS[next() % 6];
    d->payload = next() % 8 == 0 ? 1000 + (unsigned)(next() % 300) : (unsigned)(next() % 40);
    d->cksum = next() % 8 == 0 ? 1 : next() % 8 == 0 ? 2 : 0;
}

/* A datagram's bytes (header with options sometimes, transport, padding past the total sometimes); its length. */
static unsigned build(const struct dgram *d, uint8_t *b, unsigned *ihl, uint16_t *total)
{
    unsigned h = next() % 4 == 0 ? 20 + 4 * (unsigned)(next() % 11) : 20;
    unsigned l = put_l4(b + h, d);
    *ihl = h;
    *total = (uint16_t)(h + l);
    put_header(b, h, *total, d->ttl, d->proto, d->src, d->dst);
    unsigned pad = next() % 8 == 0 ? (unsigned)(1 + next() % 12) : 0;
    for (unsigned k = 0; k < pad; k++)
        b[h + l + k] = (uint8_t)next();
    return h + l + pad;
}

/* A live entry, as listed, or false. */
static bool some_flow(struct nat_flow *out)
{
    static struct nat_flow flows[NAT_TABLE_SIZE];
    unsigned n = nat_flow_list(flows, NAT_TABLE_SIZE, g_now);
    if (n == 0)
        return false;
    *out = flows[next() % n];
    return true;
}

/* ---- the histories ---- */

/* As ipv4_forward does: the transport minimum pulled up, nat_out, then the datagram sent (here, folded in). */
static void forward_out(const struct dgram *d, unsigned in, unsigned out, unsigned in_flags, unsigned out_flags)
{
    static uint8_t b[1600];
    unsigned ihl;
    uint16_t total;
    unsigned len = build(d, b, &ihl, &total);
    struct mbuf *m = packet(b, len);
    unsigned l4min = d->proto == IPPROTO_TCP ? 20 : 8;
    if (m->pkt.len < ihl + l4min) {
        note(10, m->pkt.len);
        m_freem(m);
        return;
    }
    m = m_pullup(m, ihl + l4min);
    EXPECT(m != NULL);
    uint8_t hdr[60];
    memcpy(hdr, m->data, ihl);
    uint32_t new_src = 0x5a5a5a5a;
    int rc = nat_out(&g_nifs[in], in_flags, &g_nifs[out], out_flags, m, (const struct ipv4_hdr *)hdr, ihl, &new_src);
    note(11, ((uint64_t)(uint32_t)rc << 32) | new_src);
    uint8_t after[1600];
    m_copydata(m, 0, m->pkt.len, after);
    mix32(l4_ok(after + ihl, total - ihl, d->proto, new_src, d->dst));
    mix(after + ihl, l4min);
    m_freem(m);
}

static void op_out(void)
{
    struct dgram d;
    some_dgram(&d);
    unsigned i = (unsigned)(next() % NGUESTS);
    d.src = next() % 16 == 0 ? A(10, 0, 2, 9) : guest_ip(i);   /* sometimes already on the uplink's subnet */
    d.dst = next() % 8 == 0 ? guest_ip((unsigned)(next() % NGUESTS)) : world();
    static const unsigned OUTS[] = { UPLINK, UPLINK, UPLINK, 0 };
    unsigned out = OUTS[next() % 4];
    unsigned in_flags = next() % 8 == 0 ? NETIF_UP | NETIF_FORWARD : g_nifs[i].flags;
    unsigned out_flags = g_nifs[out].flags;
    g_policy = next();
    forward_out(&d, i, out, in_flags, out_flags);
}

/* A guest's reply on a DNAT flow: from its forwarded port back to the client. */
static void op_dnat_reply(void)
{
    struct nat_flow f;
    if (!some_flow(&f))
        return;
    struct dgram d;
    some_dgram(&d);
    if (f.proto == IPPROTO_ICMP)
        return;
    d.proto = f.proto;
    d.src = f.orig_ip;
    d.dst = f.peer_ip;
    d.sport = f.orig_port;
    d.dport = f.peer_port;
    unsigned i = (unsigned)((ntohl(f.orig_ip) >> 8) & 0xff) - 100;
    if (i >= NGUESTS)
        i = 0;
    forward_out(&d, i, UPLINK, g_nifs[i].flags, g_nifs[UPLINK].flags);
}

/* Forty new flows from one guest, past its share. */
static void op_flood(void)
{
    unsigned i = (unsigned)(next() % NGUESTS);
    g_policy = 0;
    for (unsigned k = 0; k < 40; k++) {
        struct dgram d;
        memset(&d, 0, sizeof(d));
        d.proto = IPPROTO_UDP;
        d.src = guest_ip(i);
        d.dst = A(9, 9, 9, (uint8_t)k);
        d.sport = (uint16_t)(30000 + k);
        d.dport = 53;
        d.ttl = 64;
        forward_out(&d, i, UPLINK, g_nifs[i].flags, g_nifs[UPLINK].flags);
    }
    note(12, i);
}

/* As ipv4_input does: the header pulled up, nat_in, and what it declines freed. */
static void deliver_in(const uint8_t *b, unsigned len, unsigned ihl, uint16_t total)
{
    struct mbuf *m = packet(b, len);
    m = m_pullup(m, ihl);
    EXPECT(m != NULL);
    uint8_t hdr[60];
    memcpy(hdr, m->data, ihl);
    g_fail_in = next() % 16 == 0 ? (int)(next() % 2) : -1;
    bool taken = nat_in(&g_nifs[UPLINK], m, (const struct ipv4_hdr *)hdr, ihl, total);
    g_fail_in = -1;
    note(13, taken);
    if (!taken)
        m_freem(m);
}

static void op_in(void)
{
    struct dgram d;
    some_dgram(&d);
    struct nat_flow f;
    if (next() % 4 != 0 && some_flow(&f)) {
        d.proto = f.proto;
        d.src = f.peer_ip;
        d.dst = f.nat_ip;
        if (f.proto == IPPROTO_ICMP) {
            d.icmp_type = next() % 4 == 0 ? ICMP_ECHO : ICMP_ECHO_REPLY;
            d.sport = f.nat_port;
        } else {
            d.sport = f.peer_port;
            d.dport = f.nat_port;
        }
    } else {
        d.src = world();
        d.dst = uplink_ip();
        if (d.proto == IPPROTO_ICMP)
            d.icmp_type = ICMP_ECHO_REPLY;
    }
    if (d.proto == IPPROTO_ICMP && d.icmp_type == ICMP_ECHO_REPLY && next() % 4 == 0)
        d.cksum = 1;
    static uint8_t b[1600];
    unsigned ihl;
    uint16_t total;
    unsigned len = build(&d, b, &ihl, &total);
    if (next() % 32 == 0) {
        len = ihl + (unsigned)(next() % 8);   /* less than a transport header */
        total = (uint16_t)len;
        b[2] = (uint8_t)(total >> 8);
        b[3] = (uint8_t)total;
    }
    deliver_in(b, len, ihl, total);
}

/* An ICMP error from the world quoting a datagram the NAT sent (or not). */
static void op_icmp_error(void)
{
    struct nat_flow f;
    bool have = some_flow(&f);
    if (!have) {
        memset(&f, 0, sizeof(f));
        f.proto = IPPROTO_UDP;
        f.nat_ip = uplink_ip();
        f.peer_ip = world();
        f.nat_port = 40001;
        f.peer_port = 53;
    }
    static uint8_t b[1600];
    unsigned ihl = next() % 4 == 0 ? 24 : 20;
    uint8_t *ic = b + ihl;
    memset(ic, 0, 8);
    ic[0] = next() % 2 ? ICMP_DEST_UNREACH : ICMP_TIME_EXCEEDED;
    ic[1] = 3;
    /* The quote: the datagram as sent (from the NAT's address and lent port), its header with options sometimes. */
    unsigned qihl = next() % 4 == 0 ? 28 : 20;
    uint8_t *q = ic + 8;
    put_header(q, qihl, 200, 1, f.proto, f.nat_ip, f.peer_ip);
    if (next() % 16 == 0)
        q[0] = 0x45 + 0x10;   /* not IPv4: the quote is refused */
    uint8_t *ql4 = q + qihl;
    for (unsigned k = 0; k < 8; k++)
        ql4[k] = (uint8_t)next();
    if (f.proto == IPPROTO_ICMP) {
        ql4[0] = ICMP_ECHO;
        ql4[4] = (uint8_t)(f.nat_port >> 8);
        ql4[5] = (uint8_t)f.nat_port;
    } else {
        ql4[0] = (uint8_t)(f.nat_port >> 8);
        ql4[1] = (uint8_t)f.nat_port;
        ql4[2] = (uint8_t)(f.peer_port >> 8);
        ql4[3] = (uint8_t)f.peer_port;
    }
    unsigned extra = next() % 4 == 0 ? (unsigned)(next() % 400) : 0;   /* more of the quoted datagram */
    for (unsigned k = 0; k < extra; k++)
        ql4[8 + k] = (uint8_t)next();
    unsigned icmp_len = 8 + qihl + 8 + extra;
    if (next() % 16 == 0)
        icmp_len = 8 + (unsigned)(next() % (qihl + 8));   /* a short quote */
    uint16_t c = in_cksum(ic, icmp_len);
    if (next() % 8 == 0)
        c ^= 0x0202;
    memcpy(ic + 2, &c, 2);
    uint16_t total = (uint16_t)(ihl + icmp_len);
    static const uint8_t TTLS[] = { 64, 64, 1 };
    uint8_t ttl = TTLS[next() % 3];
    uint32_t from = world();
    put_header(b, ihl, total, ttl, IPPROTO_ICMP, from, uplink_ip());
    deliver_in(b, total, ihl, total);
}

/* A client connecting to a forwarded port on the host. */
static void op_dnat_in(void)
{
    struct nat_pf_rule rules[NAT_PF_MAX];
    unsigned n = nat_pf_list(rules, NAT_PF_MAX);
    struct dgram d;
    some_dgram(&d);
    d.proto = next() % 2 ? IPPROTO_TCP : IPPROTO_UDP;
    if (n > 0 && next() % 4 != 0) {
        const struct nat_pf_rule *r = &rules[next() % n];
        d.proto = r->proto;
        d.dport = r->host_port;
    }
    d.src = world();
    d.dst = uplink_ip();
    d.sport = (uint16_t)(50000 + next() % 4);
    static uint8_t b[1600];
    unsigned ihl;
    uint16_t total;
    unsigned len = build(&d, b, &ihl, &total);
    deliver_in(b, len, ihl, total);
}

static void op_pf_add(void)
{
    static const uint8_t PROTOS[] = { IPPROTO_TCP, IPPROTO_UDP, IPPROTO_ICMP };
    uint8_t proto = PROTOS[next() % 3];
    uint16_t host_port = next() % 16 == 0 ? 0 : (uint16_t)(8000 + next() % 24);
    uint16_t guest_port = next() % 16 == 0 ? 0 : some_port();
    uint32_t gip;
    switch (next() % 8) {
    case 0: gip = 0; break;
    case 1: gip = A(10, 0, 2, 20); break;          /* on the uplink, not a guest tap */
    case 2: gip = A(172, 16, 0, 1); break;          /* on no interface */
    default: gip = guest_ip((unsigned)(next() % NGUESTS)); break;
    }
    int rc = nat_pf_add(proto, host_port, gip, guest_port);
    note(20, ((uint64_t)(uint32_t)rc << 32) | host_port);
}

static void op_pf_del(void)
{
    uint8_t proto = next() % 2 ? IPPROTO_TCP : IPPROTO_UDP;
    uint16_t host_port = (uint16_t)(8000 + next() % 24);
    note(21, ((uint64_t)nat_pf_del(proto, host_port) << 16) | host_port);
}

static void op_pf_list(void)
{
    struct nat_pf_rule rules[NAT_PF_MAX];
    unsigned max = (unsigned)(next() % (NAT_PF_MAX + 1));
    unsigned n = nat_pf_list(rules, max);
    note(22, n);
    for (unsigned k = 0; k < n; k++) {
        mix32(rules[k].proto);
        mix32(rules[k].host_port);
        mix32(rules[k].guest_port);
        mix32(rules[k].guest_ip);
    }
}

static void op_pf_config(void)
{
    static const char *CFGS[] = {
        "tcp:8080:192.168.100.15:80",
        "udp:8053:192.168.101.15:53,tcp:8022:192.168.102.15:22",
        "tcp:8081:192.168.103.15:80,bogus,udp:8054:192.168.104.15:5353",
        "tcp:99999:192.168.100.15:80",
        "udp:8055:192.168.100:53",
        "tcp:8056:192.168.105.15:",
        "icmp:8057:192.168.100.15:1",
        "tcp:8058:10.0.2.20:80",
        "",
        ",,tcp:8059:192.168.106.15:8080,",
        "tcp:8060:192.168.107.15:80x,udp:8061:192.168.108.15:53",
        "tcp:8062:192.168.300.15:80",
        NULL,
    };
    const char *cfg = CFGS[next() % (sizeof(CFGS) / sizeof(CFGS[0]))];
    bool replace = next() % 2;
    if (replace)
        nat_portforward_config(cfg);
    else
        nat_portforward_apply(cfg);
    note(23, replace);
}

static void op_purge(void)
{
    unsigned i = (unsigned)(next() % NGUESTS);
    nat_guest_purge(guest_ip(i));
    note(24, i);
}

static void op_flows(void)
{
    static struct nat_flow flows[NAT_TABLE_SIZE];
    unsigned max = next() % 4 == 0 ? NAT_TABLE_SIZE : (unsigned)(next() % 8);
    unsigned n = nat_flow_list(flows, max, g_now);
    note(25, ((uint64_t)max << 32) | n);
    for (unsigned k = 0; k < n; k++) {
        mix32(flows[k].kind);
        mix32(flows[k].proto);
        mix32(flows[k].est);
        mix32(flows[k].orig_port);
        mix32(flows[k].nat_port);
        mix32(flows[k].peer_port);
        mix32(flows[k].orig_ip);
        mix32(flows[k].nat_ip);
        mix32(flows[k].peer_ip);
        mix64(flows[k].expires_ns);
    }
}

static void op_stats(void)
{
    struct nat_stats s;
    memset(&s, 0x33, sizeof(s));
    nat_get_stats(&s);
    mix(&s, offsetof(struct nat_stats, entries));
    mix32(s.entries);
}

/* Two forwards to one guest port, and a client dialling both from one port: the second flow's reply could not be told
 * from the first's, so it is refused. */
static void op_ambiguous(void)
{
    unsigned i = (unsigned)(next() % NGUESTS);
    int a = nat_pf_add(IPPROTO_TCP, 8100, guest_ip(i), 80);
    int b = nat_pf_add(IPPROTO_TCP, 8101, guest_ip(i), 80);
    note(29, ((uint64_t)(uint32_t)a << 32) | (uint32_t)b);
    for (unsigned k = 0; k < 2; k++) {
        struct dgram d;
        memset(&d, 0, sizeof(d));
        d.proto = IPPROTO_TCP;
        d.src = A(8, 8, 4, 4);
        d.dst = uplink_ip();
        d.sport = 50009;
        d.dport = (uint16_t)(8100 + k);
        d.tcp_flags = TH_SYN;
        d.ttl = 64;
        static uint8_t buf[200];
        unsigned ihl;
        uint16_t total;
        unsigned len = build(&d, buf, &ihl, &total);
        deliver_in(buf, len, ihl, total);
    }
    (void)nat_pf_del(IPPROTO_TCP, 8100);
    (void)nat_pf_del(IPPROTO_TCP, 8101);
}

/* A connection to a forwarded port, in order: the client's SYN, the guest's SYN-ACK back through nat_out, the client's
 * ACK -- established only then. */
static void op_handshake(void)
{
    unsigned i = (unsigned)(next() % NGUESTS);
    int rc = nat_pf_add(IPPROTO_TCP, 8200, guest_ip(i), 22);
    note(31, (uint32_t)rc);
    static const uint8_t FLAGS[] = { TH_SYN, TH_SYN | TH_ACK, TH_ACK };
    for (unsigned step = 0; step < 3; step++) {
        struct dgram d;
        memset(&d, 0, sizeof(d));
        d.proto = IPPROTO_TCP;
        d.tcp_flags = FLAGS[step];
        d.ttl = 64;
        if (step == 1) {
            d.src = guest_ip(i);
            d.dst = A(8, 8, 4, 4);
            d.sport = 22;
            d.dport = 50011;
            forward_out(&d, i, UPLINK, g_nifs[i].flags, g_nifs[UPLINK].flags);
        } else {
            d.src = A(8, 8, 4, 4);
            d.dst = uplink_ip();
            d.sport = 50011;
            d.dport = 8200;
            static uint8_t buf[200];
            unsigned ihl;
            uint16_t total;
            unsigned len = build(&d, buf, &ihl, &total);
            deliver_in(buf, len, ihl, total);
        }
    }
    op_flows();
    (void)nat_pf_del(IPPROTO_TCP, 8200);
}

/* Every guest's share taken, past the table; a forward more than the rule table holds; then a connection to one. */
static void op_fill(void)
{
    g_policy = 0;
    for (unsigned i = 0; i < NGUESTS; i++)
        for (unsigned k = 0; k < NAT_QUOTA_PER_GUEST; k++) {
            struct dgram d;
            memset(&d, 0, sizeof(d));
            d.proto = IPPROTO_UDP;
            d.src = guest_ip(i);
            d.dst = A(9, 9, (uint8_t)i, (uint8_t)k);
            d.sport = (uint16_t)(20000 + k);
            d.dport = 123;
            d.ttl = 64;
            forward_out(&d, i, UPLINK, g_nifs[i].flags, g_nifs[UPLINK].flags);
        }
    for (unsigned k = 0; k <= NAT_PF_MAX; k++)
        mix32((uint32_t)nat_pf_add(IPPROTO_UDP, (uint16_t)(9000 + k), guest_ip(k % NGUESTS), 53));
    struct dgram d;
    memset(&d, 0, sizeof(d));
    d.proto = IPPROTO_UDP;
    d.src = A(8, 8, 4, 4);
    d.dst = uplink_ip();
    d.sport = 50010;
    d.dport = 9008 + (uint16_t)(next() % 2);   /* guest 8 or 9: under their shares, the table full */
    d.ttl = 64;
    static uint8_t buf[200];
    unsigned ihl;
    uint16_t total;
    unsigned len = build(&d, buf, &ihl, &total);
    deliver_in(buf, len, ihl, total);
    note(30, 0);
}

/* Every lendable port held by a host socket: no identifier is free. */
static void op_exhaust(void)
{
    g_all_ports_taken = true;
    struct dgram d;
    memset(&d, 0, sizeof(d));
    d.proto = IPPROTO_UDP;
    d.src = guest_ip((unsigned)(next() % NGUESTS));
    d.dst = A(4, 4, 4, 4);
    d.sport = (uint16_t)(next() % 1000);
    d.dport = 53;
    d.ttl = 64;
    unsigned i = (unsigned)((ntohl(d.src) >> 8) & 0xff) - 100;
    forward_out(&d, i, UPLINK, g_nifs[i].flags, g_nifs[UPLINK].flags);
    g_all_ports_taken = false;
}

static void setup(void)
{
    static bool done;
    if (!done) {
        mbuf_init();
        done = true;
    }
    memset(g_nifs, 0, sizeof(g_nifs));
    for (unsigned i = 0; i < NGUESTS; i++) {
        g_nifs[i].flags = NETIF_UP | NETIF_FORWARD | NETIF_MASQUERADE | NETIF_NODEFAULT;
        g_nifs[i].ip4.addr = A(192, 168, 100 + i, 1);
        g_nifs[i].ip4.mask = A(255, 255, 255, 0);
    }
    g_nifs[UPLINK].flags = NETIF_UP;
    g_nifs[UPLINK].ip4.addr = uplink_ip();
    g_nifs[UPLINK].ip4.mask = A(255, 255, 255, 0);
    nat_flush();
    nat_pf_clear();
}

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    setup();
    for (unsigned r = 0; r < ROUNDS; r++) {
        uint64_t jump = next() % 64;
        g_now += jump == 0 ? 301000000000ull : jump == 1 ? 31000000000ull : next() % 2000000000ull;
        g_policy = 0;
        uint64_t k = next() % 64;
        if (k < 16)
            op_out();
        else if (k < 20)
            op_dnat_reply();
        else if (k < 32)
            op_in();
        else if (k < 37)
            op_icmp_error();
        else if (k < 43)
            op_dnat_in();
        else if (k < 48)
            op_pf_add();
        else if (k < 50)
            op_pf_del();
        else if (k < 51)
            op_pf_list();
        else if (k < 53)
            op_pf_config();
        else if (k < 54)
            op_purge();
        else if (k < 56)
            op_flows();
        else if (k < 57) {
            nat_age(g_now);
            note(26, 0);
        } else if (k < 58)
            op_stats();
        else if (k < 62)
            op_flood();
        else if (r % 8 == 0)
            op_exhaust();
        else if (r % 64 == 5)
            op_ambiguous();
        else if (r % 64 == 9)
            op_handshake();
        else if (r % 256 == 7)
            op_fill();
        else if (r % 16 == 1) {
            nat_flush();
            note(27, 0);
        } else if (r % 16 == 3) {
            nat_pf_clear();
            note(28, 0);
        } else
            op_flows();
        EXPECT(harness_locks_held() == 0);
    }
    op_stats();
    for (unsigned i = 0; i < NIFS; i++)
        mix64((uint64_t)g_refs[i]);
    printf("  nat digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, ROUNDS);
    EXPECT(g_digest == NAT_DIGEST);
    EXPECT(g_live == 0);
    for (unsigned i = 0; i < NIFS; i++)
        EXPECT(g_refs[i] == 0);
}

/* NAT_BENCH=1: the median of nine runs, 100,000 datagrams each. */
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

static void nat_bench(void)
{
    g_rng = SEED;
    setup();
    g_benching = true;
    g_policy = 0;
    /* Thirty flows from other guests ahead of it in the table, then the one timed. */
    for (unsigned k = 0; k < 30; k++) {
        struct dgram d = { .proto = IPPROTO_UDP, .src = guest_ip(1 + k % 8), .dst = A(9, 9, 9, (uint8_t)k),
                           .sport = (uint16_t)(30000 + k), .dport = 53, .ttl = 64 };
        forward_out(&d, 1 + k % 8, UPLINK, g_nifs[1].flags, g_nifs[UPLINK].flags);
    }
    struct dgram d = { .proto = IPPROTO_UDP, .src = guest_ip(0), .dst = A(8, 8, 8, 8), .sport = 5000, .dport = 53,
                       .ttl = 64, .payload = 64 };
    uint8_t b[200], out_b[200];
    unsigned ihl;
    uint16_t total;
    g_rng = SEED;
    unsigned len = build(&d, b, &ihl, &total);
    (void)len;
    /* One masquerade to learn the lent port, for the reply. */
    struct mbuf *m = m_getcl();
    m_append(m, b, total);
    uint32_t new_src;
    EXPECT(nat_out(&g_nifs[0], g_nifs[0].flags, &g_nifs[UPLINK], g_nifs[UPLINK].flags, m, (const struct ipv4_hdr *)b,
                   ihl, &new_src) == 0);
    m_copydata(m, 0, total, out_b);
    m_freem(m);
    uint16_t lent = (uint16_t)((out_b[ihl] << 8) | out_b[ihl + 1]);
    struct dgram r = { .proto = IPPROTO_UDP, .src = d.dst, .dst = uplink_ip(), .sport = 53, .dport = lent,
                       .ttl = 64, .payload = 64 };
    uint8_t rb[200];
    unsigned rihl;
    uint16_t rtotal;
    build(&r, rb, &rihl, &rtotal);
    double out[9], in[9];
    for (unsigned t = 0; t < 9; t++) {
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 100000; i++) {
            struct mbuf *p = m_getcl();
            m_append(p, b, total);
            (void)nat_out(&g_nifs[0], g_nifs[0].flags, &g_nifs[UPLINK], g_nifs[UPLINK].flags, p,
                          (const struct ipv4_hdr *)b, ihl, &new_src);
            m_freem(p);
        }
        out[t] = ns_since(&t0) / 100000;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 100000; i++) {
            struct mbuf *p = m_getcl();
            m_append(p, rb, rtotal);
            if (!nat_in(&g_nifs[UPLINK], p, (const struct ipv4_hdr *)rb, rihl, rtotal))
                m_freem(p);
        }
        in[t] = ns_since(&t0) / 100000;
    }
    qsort(out, 9, sizeof(double), cmp_double);
    qsort(in, 9, sizeof(double), cmp_double);
    g_benching = false;
    nat_flush();
    printf("  bench nat_out (a flow behind thirty) %.1f ns, nat_in (its reply) %.1f ns\n", out[4], in[4]);
}

static const struct host_test tests[] = {
    { "nat digest", test_digest },
};

int main(void)
{
    int rc = harness_run(tests, sizeof(tests) / sizeof(tests[0]));
    if (getenv("NAT_BENCH") != NULL)
        nat_bench();
    return rc;
}
