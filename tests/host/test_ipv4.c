/*
 * test_ipv4.c - IPv4, ICMP and the path MTU cache (kernel-services/network/
 * ipv4.c) on the host: roadmap 2.0 SA6. A digest over seeded histories,
 * recorded from the C, of four interfaces -- a loopback, an uplink with a
 * gateway, a guest tap that forwards and masquerades, and a forwarding link
 * without masquerade:
 *
 *   - datagrams in: well-formed or not (versions, header lengths with
 *     options, total lengths that lie, fragments, bad checksums, short,
 *     padded, split across two mbufs, m_pullup failing); from martian,
 *     spoofed and honest sources; to our addresses, broadcasts, other hosts
 *     (forwarded, hairpinned, unroutable, TTL expiring, masqueraded or
 *     dropped by NAT); through the DHCP client, NAT's return path and the
 *     firewall's INPUT, FORWARD and host chains (quiet drops included);
 *     every protocol, ICMP with good and bad checksums, echoes answered or
 *     rate-limited, echo replies to the hook, destination-unreachables and
 *     fragmentation-needed quoting our flows or not;
 *   - datagrams out: from given and unspecified sources, to the loopback,
 *     on-link, through the gateway or with none, broadcast; past the MTU and
 *     past 65,535 bytes; with checksum offload; through ARP's answers; the
 *     firewall's OUTPUT verdict and the host flow it records;
 *   - echo requests sent, unreachables and time-exceededs sent for a quote;
 *   - the path MTU cache: updates past its sixteen entries, expiry, lookups,
 *     flushes; the route list (with kmalloc failing), the route and source
 *     choices, the rate limit across its one-second windows, the debug
 *     build's forwarding hook, and the statistics.
 *
 * mbuf.c, the checksum and netparse are real; the interface registry, ARP,
 * the firewall, NAT, DHCP, raw sockets, UDP, TCP and the transmits are stubs
 * that fold each call into the digest and answer from the seed. Every mbuf is
 * freed and every interface reference given back at the end.
 *
 * IPV4_BENCH=1 times an echo answered and a datagram sent.
 */

#include "harness.h"

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/mbuf.h>
#include <kernel/net/cksum.h>
#include <kernel/net/dhcp.h>
#include <kernel/net/ether.h>
#include <kernel/net/fw.h>
#include <kernel/net/inet.h>
#include <kernel/net/ip.h>
#include <kernel/net/nat.h>
#include <kernel/net/raw.h>
#include <kernel/net/tapsvc.h>
#include <kernel/net/tcp.h>
#include <kernel/net/udp.h>
#include <kernel/netif.h>
#include <kernel/object.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The digest of the C (ipv4.c, 2026-11-03), which the CeeSafe port gives. */
#define IPV4_DIGEST 0x003b7ba636ab8d7aull
#define ROUNDS 20000u
#define SEED 0xC05F5FA2026B0012ull
#define NIFS 4u

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
    mix32(0x5b00u + which);
    mix64(value);
}

/* mbuf.c's caches and kmalloc, over malloc; an allocation fails when g_fail_in counts down to it. */
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

static bool g_kmalloc_fails;

void *kmalloc(size_t size, unsigned flags)
{
    (void)flags;
    if (g_kmalloc_fails)
        return NULL;
    g_live++;
    return malloc(size);
}

void kfree(void *p)
{
    if (p != NULL)
        g_live--;
    free(p);
}

/* ---- the interfaces ---- */

enum { LO, ETH0, TAP0, ETH1 };
static struct netif g_nifs[NIFS];
static long g_refs[NIFS];
static bool g_has_default = true;
static uint64_t g_now = 5000000000ull;

uint64_t clock_now_ns(void) { return g_now; }

static unsigned nif_index(const struct netif *nif) { return (unsigned)(nif - g_nifs); }

void kobject_get(struct kobject *obj) { g_refs[nif_index(container_of(obj, struct netif, obj))]++; }
void kobject_put(struct kobject *obj) { g_refs[nif_index(container_of(obj, struct netif, obj))]--; }

static struct netif *ref(struct netif *nif)
{
    netif_get(nif);
    return nif;
}

struct netif *netif_loopback(void) { return ref(&g_nifs[LO]); }

struct netif *netif_default_ipv4(void)
{
    struct netif *nif = &g_nifs[ETH0];
    return g_has_default && (nif->flags & NETIF_UP) && nif->ip4.addr ? ref(nif) : NULL;
}

bool netif_owns_ipv4(uint32_t addr)
{
    bool owns = false;
    for (unsigned i = 0; i < NIFS; i++)
        owns = owns || (addr != 0 && g_nifs[i].ip4.addr == addr);
    note(1, owns);
    return owns;
}

/* The up, non-loopback interface whose subnet holds dst, the longest prefix. */
struct netif *netif_connected(uint32_t dst)
{
    struct netif *best = NULL;
    for (unsigned i = 1; i < NIFS; i++) {
        struct netif *n = &g_nifs[i];
        if (!(n->flags & NETIF_UP) || n->ip4.addr == 0 || n->ip4.mask == 0 || ((dst ^ n->ip4.addr) & n->ip4.mask))
            continue;
        if (best == NULL || ntohl(n->ip4.mask) > ntohl(best->ip4.mask))
            best = n;
    }
    note(2, best == NULL ? NIFS : nif_index(best));
    return best == NULL ? NULL : ref(best);
}

static bool g_list_overflows;   /* the registry holds more than the caller's array: zeros past ours */

unsigned netif_list(struct netif_info *out, unsigned max)
{
    if (g_list_overflows) {
        memset(out, 0, max * sizeof(*out));
        for (unsigned i = 0; i < NIFS && i < max; i++) {
            memcpy(out[i].name, g_nifs[i].name, sizeof(out[i].name));
            out[i].index = g_nifs[i].index;
        }
        return max + 1;
    }
    for (unsigned i = 0; i < NIFS && i < max; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        memcpy(out[i].name, g_nifs[i].name, sizeof(out[i].name));
        out[i].index = g_nifs[i].index;
        out[i].flags = g_nifs[i].flags;
        out[i].mtu = g_nifs[i].mtu;
        out[i].addr = g_nifs[i].ip4.addr;
        out[i].mask = g_nifs[i].ip4.mask;
        out[i].gateway = g_nifs[i].ip4.gateway;
    }
    return NIFS;
}

/* ---- what leaves, and the protocols around IPv4 ---- */

static void sent(uint32_t which, const struct netif *nif, struct mbuf *m)
{
    if (!g_benching) {
        uint8_t bytes[96];
        uint32_t n = m_length(m);
        uint32_t take = n < sizeof(bytes) ? n : (uint32_t)sizeof(bytes);
        m_copydata(m, 0, take, bytes);
        note(which, nif == NULL ? NIFS : nif_index(nif));
        mix32(n);
        mix32(m->pkt.len);
        mix32(m->flags & (M_BCAST | M_MCAST | M_FW_QUIET));
        mix(&m->pkt.proto, sizeof(m->pkt.proto));
        mix(&m->pkt.csum_flags, sizeof(m->pkt.csum_flags));
        mix(&m->pkt.csum_start, sizeof(m->pkt.csum_start));
        mix(bytes, take);
    }
    m_freem(m);
}

static int g_tx_rc, g_arp_answer;

int netif_transmit(struct netif *nif, struct mbuf *m)
{
    sent(3, nif, m);
    return g_tx_rc;
}

int ether_output(struct netif *nif, struct mbuf *m, const uint8_t *dst, uint16_t type)
{
    if (!g_benching) {
        mix(dst, ETH_ALEN);
        mix32(type);
    }
    sent(4, nif, m);
    return g_tx_rc;
}

int arp_resolve(struct netif *nif, uint32_t ip, uint8_t *mac, struct mbuf *m)
{
    note(5, ((uint64_t)nif_index(nif) << 32) | ip);
    if (g_arp_answer == 0) {
        memset(mac, 0x24, ETH_ALEN);
        return 0;
    }
    if (g_arp_answer == -EINPROGRESS)
        sent(6, nif, m);   /* parked: ARP's now */
    else
        m_freem(m);
    return g_arp_answer;
}

/* The firewall, NAT and DHCP: verdicts from the seed, each call folded in. */
static uint64_t g_policy;   /* bits drawn per round: which hooks refuse */

static bool refuses(unsigned bit) { return (g_policy >> bit) & 1; }

enum fw_verdict fw_output_verdict(struct netif *out, unsigned out_flags, struct mbuf *m, uint32_t src, uint32_t dst,
                                  uint8_t proto)
{
    (void)m;
    note(10, ((uint64_t)nif_index(out) << 40) | ((uint64_t)out_flags << 32) | src);
    note(11, ((uint64_t)proto << 32) | dst);
    return refuses(0) ? FW_DROP : FW_ACCEPT;
}

bool fw_host_flow_of(struct netif *out, unsigned out_flags, struct mbuf *m, uint32_t src, uint32_t dst, uint8_t proto,
                     struct fw_host_flow *f)
{
    (void)m;
    note(12, ((uint64_t)nif_index(out) << 40) | out_flags);
    memset(f, 0, sizeof(*f));
    f->src = src;
    f->dst = dst;
    f->proto = proto;
    f->a_port = (uint16_t)g_policy;
    return !refuses(1);
}

void fw_host_record(const struct fw_host_flow *f) { note(13, ((uint64_t)f->src << 32) | f->dst); }

enum fw_verdict fw_forward_verdict(struct netif *in, struct netif *out, unsigned out_flags, struct mbuf *m,
                                   const struct ipv4_hdr *iph, unsigned ihl)
{
    (void)m;
    note(14, ((uint64_t)nif_index(in) << 48) | ((uint64_t)nif_index(out) << 40) | ((uint64_t)out_flags << 8) | ihl);
    mix(iph, sizeof(*iph));
    return refuses(2) ? FW_DROP : FW_ACCEPT;
}

enum fw_verdict fw_input_verdict(struct netif *nif, struct mbuf *m, const struct ipv4_hdr *iph, unsigned ihl)
{
    (void)m;
    note(15, ((uint64_t)nif_index(nif) << 8) | ihl);
    mix(iph, sizeof(*iph));
    return refuses(3) ? FW_DROP : FW_ACCEPT;
}

enum fw_verdict fw_host_verdict(struct netif *nif, struct mbuf *m, const struct ipv4_hdr *iph, unsigned ihl)
{
    (void)m;
    note(16, ((uint64_t)nif_index(nif) << 8) | ihl);
    mix(iph, sizeof(*iph));
    return refuses(4) ? FW_DROP : FW_ACCEPT;
}

int nat_out(struct netif *in, unsigned in_flags, struct netif *out, unsigned out_flags, struct mbuf *m,
            const struct ipv4_hdr *iph, unsigned ihl, uint32_t *new_src)
{
    note(17, ((uint64_t)nif_index(in) << 48) | ((uint64_t)in_flags << 32) | ((uint64_t)nif_index(out) << 24) | ihl);
    note(18, out_flags);
    mix(iph, sizeof(*iph));
    mix32(m->pkt.len);
    if (refuses(5))
        return -1;
    *new_src = out->ip4.addr;
    return 0;
}

bool nat_in(struct netif *nif, struct mbuf *m, const struct ipv4_hdr *iph, unsigned ihl, uint16_t total)
{
    note(19, ((uint64_t)nif_index(nif) << 32) | ((uint64_t)ihl << 16) | total);
    mix(iph, sizeof(*iph));
    if (!refuses(6))
        return false;
    sent(20, nif, m);   /* a masqueraded flow's reply: NAT's now */
    return true;
}

bool dhcp_client_input(struct netif *nif, struct mbuf *m, unsigned ihl)
{
    note(21, ((uint64_t)nif_index(nif) << 8) | ihl);
    if (!refuses(7))
        return false;
    sent(22, nif, m);
    return true;
}

void raw_icmp_input(const struct mbuf *m, const struct ipv4_hdr *iph)
{
    note(23, m->pkt.len);
    mix(iph, sizeof(*iph));
}

bool udp_error_notify(const struct netaddr *local, const struct netaddr *remote, int err)
{
    mix(local, sizeof(*local));
    mix(remote, sizeof(*remote));
    note(24, (uint64_t)(int64_t)err);
    return !refuses(8);
}

bool tcp_pmtu_notify(const struct netaddr *local, const struct netaddr *remote, uint32_t seq, uint16_t mtu)
{
    mix(local, sizeof(*local));
    mix(remote, sizeof(*remote));
    note(25, ((uint64_t)seq << 16) | mtu);
    return !refuses(9);
}

void udp_input(struct netif *nif, struct mbuf *m, const struct ipv4_hdr *ip4, const struct ipv6_hdr *ip6)
{
    note(26, ip6 == NULL);
    if (!g_benching)
        mix(ip4, sizeof(*ip4));
    sent(27, nif, m);
}

void tcp_input(struct netif *nif, struct mbuf *m, const struct ipv4_hdr *ip4, const struct ipv6_hdr *ip6)
{
    note(28, ip6 == NULL);
    if (!g_benching)
        mix(ip4, sizeof(*ip4));
    sent(29, nif, m);
}

static void echo_reply_hook(uint32_t src, uint16_t id, uint16_t seq) { note(30, ((uint64_t)src << 32) | ((uint32_t)id << 16) | seq); }

/* ---- the histories ---- */

#define A(a, b, c, d) IPV4_ADDR(a, b, c, d)

static struct netif *some_nif(void) { return &g_nifs[next() % NIFS]; }

/* An address of one kind: one of ours, a broadcast, the loopback net, a guest, an uplink host, a forwarding-link host,
 * somewhere else, zero. */
static uint32_t address(void)
{
    switch (next() % 12) {
    case 0: return g_nifs[1 + next() % 3].ip4.addr;
    case 1: return INADDR_BROADCAST_N;
    case 2: return A(10, 0, 2, 255);
    case 3: return A(127, 0, 0, (uint8_t)(1 + next() % 3));
    case 4: return A(192, 168, 100, 15);
    case 5: return A(192, 168, 100, (uint8_t)next());
    case 6: case 7: return A(10, 0, 2, (uint8_t)(2 + next() % 30));
    case 8: {
        uint8_t c = (uint8_t)next();   /* each byte drawn in turn: an expression's operand order is the compiler's */
        uint8_t d = (uint8_t)next();
        return A(172, 16, c, d);
    }
    case 9: return 0;
    default: {
        uint8_t b = (uint8_t)next();
        uint8_t c = (uint8_t)next();
        uint8_t d = (uint8_t)next();
        return A(93, b, c, d);
    }
    }
}

static void put16(uint8_t *b, uint16_t v)
{
    b[0] = (uint8_t)(v >> 8);
    b[1] = (uint8_t)v;
}

/* An mbuf holding `bytes`, in one buffer or split across two, with or without a cluster's headroom. */
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
            last = last->next;   /* m_append may have chained more than one mbuf already */
        last->next = tail;
        m->pkt.len = len;
    }
    return m;
}

/* An IPv4 header at `h`, `ihl` bytes, with its checksum. */
static void header(uint8_t *h, unsigned ihl, uint16_t total, uint8_t ttl, uint8_t proto, uint32_t src, uint32_t dst)
{
    for (unsigned i = 20; i < ihl; i++)
        h[i] = (uint8_t)next();   /* options, unparsed */
    h[0] = (uint8_t)(0x40 | (ihl / 4));
    h[1] = 0;
    put16(h + 2, total);
    put16(h + 4, (uint16_t)next());
    put16(h + 6, next() % 8 == 0 ? (uint16_t)(next() % 2 ? 0x2000 : next() & 0x1fff) : 0x4000);   /* MF or offset */
    h[8] = ttl;
    h[9] = proto;
    h[10] = h[11] = 0;
    memcpy(h + 12, &src, 4);
    memcpy(h + 16, &dst, 4);
    uint16_t c = in_cksum(h, ihl);
    memcpy(h + 10, &c, 2);
}

/* An ICMP message at `b` (its type, code, the rest of its header, and a quote of a datagram we may have sent). */
static uint32_t icmp_message(uint8_t *b, uint32_t room)
{
    uint64_t k = next() % 8;
    uint8_t type = k < 3 ? ICMP_ECHO : k < 4 ? ICMP_ECHO_REPLY : k < 7 ? ICMP_DEST_UNREACH : (uint8_t)next();
    uint8_t code = type == ICMP_DEST_UNREACH ? (uint8_t)(next() % 2 ? ICMP_UNREACH_NEEDFRAG : next() % 6)
                   : next() % 8 ? 0 : (uint8_t)next();
    uint32_t len = 8;
    for (uint32_t i = 0; i < room; i++)
        b[i] = (uint8_t)next();
    b[0] = type;
    b[1] = code;
    if (type == ICMP_DEST_UNREACH && room >= 8 + 28) {
        put16(b + 6, (uint16_t)(next() % 3 == 0 ? 0 : 296 + next() % 1400));   /* the next-hop MTU, or none */
        uint32_t qsrc = next() % 4 ? g_nifs[1 + next() % 3].ip4.addr : address();   /* a datagram of ours, or not */
        uint32_t qdst = address();
        uint8_t qproto = (uint8_t)(next() % 3 == 0 ? IPPROTO_UDP : next() % 2 ? IPPROTO_TCP : next());
        unsigned qihl = next() % 6 == 0 ? 24 : 20;   /* a quoted header with options, now and then */
        uint16_t qtotal = (uint16_t)(next() % 4 == 0 ? 28 + next() % 48 : 20 + next() % 1500);   /* small: the plateaus' floor */
        header(b + 8, qihl, qtotal, 64, qproto, qsrc, qdst);
        if (next() % 8 == 0)
            b[8] = 0x46;   /* a quote that says it has options it may not have */
        len = 8 + qihl + 8 - (next() % (qihl > 20 ? 2 : 6) == 0 ? (uint32_t)(1 + next() % 8) : 0);   /* the transport bytes, or fewer */
    } else {
        len = 8 + (uint32_t)(next() % 48);
    }
    if (len > room)
        len = room;
    if (next() % 8) {
        b[2] = b[3] = 0;
        uint16_t c = in_cksum(b, len);
        memcpy(b + 2, &c, 2);
    }
    return len;
}

static void op_input(unsigned r)
{
    static uint8_t d[400];
    unsigned ihl = next() % 6 == 0 ? 20 + 4 * (unsigned)(1 + next() % 2) : 20;
    uint64_t pk = next() % 8;
    uint8_t proto = pk < 4 ? IPPROTO_ICMP : pk < 6 ? IPPROTO_UDP : pk < 7 ? IPPROTO_TCP : (uint8_t)next();
    uint32_t plen;
    if (proto == IPPROTO_ICMP) {
        plen = icmp_message(d + ihl, 120);
    } else {
        plen = (uint32_t)(next() % 64);
        for (uint32_t i = 0; i < plen; i++)
            d[ihl + i] = (uint8_t)next();
    }
    uint32_t src = address(), dst = address();
    uint8_t ttl = (uint8_t)(next() % 4 == 0 ? next() % 3 : 64);
    uint16_t total = (uint16_t)(ihl + plen);
    if (next() % 16 == 0)
        total = (uint16_t)(total + 1 + next() % 8);   /* a total length that lies */
    header(d, ihl, total, ttl, proto, src, dst);
    if (next() % 16 == 0)
        d[10] ^= 0x40;   /* a bad header checksum */
    if (next() % 32 == 0)
        d[0] = (uint8_t)next();   /* not a version-4 header */
    uint32_t len = ihl + plen + (next() % 4 == 0 ? (uint32_t)(next() % 24) : 0);   /* link padding */
    if (next() % 32 == 0)
        len = (uint32_t)(next() % 24);   /* cut short */
    bool icmp_split = r % 47 == 0 && proto == IPPROTO_ICMP && len >= ihl + 8;
    struct mbuf *m;
    if (icmp_split) {
        m = m_getcl();   /* the IP header whole, the ICMP header across two mbufs: icmp_input's own pullup */
        EXPECT(m != NULL);
        m->flags |= M_PKTHDR;
        uint32_t at = ihl + 1 + (uint32_t)(next() % 6);
        m_append(m, d, at);
        struct mbuf *tail = m_getcl();
        EXPECT(tail != NULL);
        memcpy(tail->data, d + at, len - at);
        tail->len = len - at;
        m->next = tail;
        m->pkt.len = len;
    } else {
        m = packet(d, len);
    }
    if (next() % 8 == 0)
        m->flags |= M_BCAST;
    struct netif *nif = some_nif();
    unsigned saved = nif->flags;
    uint32_t saved_addr = nif->ip4.addr;
    if (nif != &g_nifs[LO] && next() % 8 == 0)
        nif->flags ^= NETIF_FORWARD | (next() % 2 ? NETIF_MASQUERADE : 0);   /* a toggle, for this packet */
    if ((nif == &g_nifs[TAP0] || nif == &g_nifs[ETH1]) && next() % 16 == 0)
        nif->ip4.addr = 0;   /* a forwarder with no address of its own: no anti-spoof */
    uint32_t eth0_addr = g_nifs[ETH0].ip4.addr, eth0_mask = g_nifs[ETH0].ip4.mask;
    if (next() % 16 == 0)
        g_nifs[ETH0].ip4.mask = 0;   /* the egress without a subnet */
    else if (next() % 16 == 0)
        g_nifs[ETH0].ip4.addr = 0;   /* or without an address */
    g_has_default = next() % 8 != 0;
    g_policy = next() & next();   /* each hook refuses about a quarter of the time */
    g_arp_answer = next() % 4 == 0 ? -EINPROGRESS : 0;
    g_tx_rc = 0;
    if (icmp_split)
        g_fail_in = 0;   /* icmp_input's pullup */
    else if (r % 61 == 0)
        g_fail_in = (int)(next() % 4);   /* a pullup, NAT's pullup, an ICMP reply's cluster or prepend */
    ipv4_input(nif, m);
    g_fail_in = -1;
    nif->flags = saved;
    nif->ip4.addr = saved_addr;
    g_nifs[ETH0].ip4.addr = eth0_addr;
    g_nifs[ETH0].ip4.mask = eth0_mask;
    g_has_default = true;
}

static void op_output(unsigned r)
{
    uint32_t dst = address(), src = next() % 3 ? 0 : address();
    uint32_t len = next() % 8 == 0 ? (uint32_t)(1400 + next() % 200) : (uint32_t)(next() % 300);
    if (r % 211 == 0) {
        len = 65516 + (uint32_t)(next() % 3);   /* at and past what a total length can say */
        dst = A(127, 0, 0, 1);                    /* on the loopback, whose MTU is larger */
    }
    static uint8_t payload[65600];
    for (uint32_t i = 0; i < len; i++)
        payload[i] = (uint8_t)next();
    struct mbuf *m = next() % 4 ? m_getcl() : m_get();
    EXPECT(m != NULL);
    m->flags |= M_PKTHDR;
    m_append(m, payload, len);
    m->pkt.csum_flags = (uint16_t)(next() % 4);
    m->pkt.csum_start = (uint16_t)(next() % 64);
    g_policy = next() & next();
    uint64_t aa = next() % 4;
    g_arp_answer = aa < 2 ? 0 : aa == 2 ? -EINPROGRESS : -EHOSTUNREACH;
    g_tx_rc = (int)(next() % 3) - 1;
    g_has_default = next() % 8 != 0;
    uint32_t gw = g_nifs[ETH0].ip4.gateway;
    if (next() % 8 == 0)
        g_nifs[ETH0].ip4.gateway = 0;   /* no gateway: nothing off-link */
    uint8_t proto = (uint8_t)(next() % 2 ? IPPROTO_UDP : IPPROTO_TCP);   /* drawn one after the other */
    uint8_t ttl = (uint8_t)next();
    if (r % 67 == 0)
        g_fail_in = 0;   /* the header's prepend, when it needs an allocation */
    int rc = ipv4_output(m, src, dst, proto, ttl);
    g_fail_in = -1;
    g_nifs[ETH0].ip4.gateway = gw;
    g_has_default = true;
    note(40, (uint64_t)(int64_t)rc);
}

static void op_echo(unsigned r)
{
    uint8_t payload[1500];
    size_t len = next() % 16 == 0 ? 1401 + next() % 50 : next() % 200;
    for (size_t i = 0; i < len; i++)
        payload[i] = (uint8_t)next();
    g_policy = next() & next();
    g_arp_answer = 0;
    uint32_t dst = address();
    uint16_t id = (uint16_t)next();   /* drawn one after the other */
    uint16_t seq = (uint16_t)next();
    if (r % 59 == 0)
        g_fail_in = 0;   /* the request's cluster */
    int rc = icmp_send_echo(dst, id, seq, payload, len);
    g_fail_in = -1;
    note(41, (uint64_t)(int64_t)rc);
}

/* An unreachable or a time-exceeded sent for a datagram we received. */
static void op_error(unsigned r)
{
    uint8_t d[120];
    unsigned ihl = next() % 4 == 0 ? 24 : 20;
    uint32_t plen = (uint32_t)(next() % 40);
    for (uint32_t i = 0; i < plen; i++)
        d[ihl + i] = (uint8_t)next();
    uint32_t src = address(), dst = address();
    header(d, ihl, (uint16_t)(ihl + plen), 5, (uint8_t)next(), src, dst);
    struct mbuf *orig = packet(d, ihl + plen);
    g_policy = next() & next();
    g_arp_answer = 0;
    if (r % 7 == 0)
        g_fail_in = 0;   /* the error's cluster */
    if (next() % 2)
        icmp_send_unreach(orig, (const struct ipv4_hdr *)d, (uint8_t)(next() % 4));
    else
        icmp_send_timxceed(orig, (const struct ipv4_hdr *)d);
    g_fail_in = -1;
    m_freem(orig);
}

static uint32_t pmtu_dst(void) { return A(93, 184, 0, (uint8_t)(next() % 24)); }   /* more than the cache's 16 */

static void op_pmtu(void)
{
    uint64_t k = next() % 10;
    if (k >= 8) {
        for (unsigned i = 0; i < 20; i++) {   /* more live entries than the cache holds: the oldest goes */
            uint8_t host = (uint8_t)(i + 8 * (unsigned)(next() % 4));   /* drawn one after the other */
            uint32_t mtu = (uint32_t)(576 + next() % 1000);
            ipv4_pmtu_update(A(93, 185, 0, host), mtu);
        }
        return;
    }
    if (k < 4) {
        uint32_t dst = pmtu_dst();
        ipv4_pmtu_update(dst, (uint32_t)(next() % 1600));
    } else if (k < 7) {
        uint32_t dst = next() % 2 ? pmtu_dst() : address();
        g_has_default = next() % 4 != 0;
        note(42, ipv4_path_mtu(dst));
        g_has_default = true;
    } else {
        ipv4_pmtu_flush();
    }
}

static void op_routes(void)
{
    struct ipv4_route_info out[IPV4_ROUTES_MAX];
    memset(out, 0x55, sizeof(out));
    unsigned max = (unsigned)(next() % (IPV4_ROUTES_MAX + 1));
    g_kmalloc_fails = next() % 16 == 0;
    g_has_default = next() % 4 != 0;
    g_list_overflows = next() % 8 == 0;
    struct netif saved[NIFS];
    memcpy(saved, g_nifs, sizeof(saved));
    struct netif *n1 = &g_nifs[1 + next() % 3];
    uint64_t t = next() % 6;
    if (t == 0)
        n1->flags &= ~NETIF_UP;
    else if (t == 1)
        n1->ip4.addr = 0;
    else if (t == 2)
        n1->ip4.mask = 0;
    else if (t == 3)
        n1->ip4.gateway = 0;
    unsigned n = ipv4_route_list(out, max);
    memcpy(g_nifs, saved, sizeof(saved));
    g_kmalloc_fails = false;
    g_has_default = true;
    g_list_overflows = false;
    note(43, n);
    for (unsigned i = 0; i < n; i++)
        mix(&out[i], sizeof(out[i]));
}

static void op_route(void)
{
    uint32_t dst = address();
    g_has_default = next() % 4 != 0;
    note(44, ipv4_source_for(dst));
    struct netif *nif = ipv4_route(dst);
    note(45, nif == NULL ? NIFS : nif_index(nif));
    if (nif != NULL)
        netif_put(nif);
    g_has_default = true;
}

static void op_rate(void)
{
    unsigned n = (unsigned)(next() % 160);
    unsigned allowed = 0;
    for (unsigned i = 0; i < n; i++)
        allowed += icmp_ratelimit_allow();
    note(46, allowed);
}

/* The debug build's forwarding hook (CONFIG_DEBUG), single-threaded: released before the forward that parks on it. */
static void op_hook(void)
{
    ipv4_test_hold_forward(true);
    note(47, ipv4_test_forward_parked());
    ipv4_test_release_forward();
    uint8_t d[60];
    header(d, 20, 40, 64, IPPROTO_UDP, A(172, 16, 9, 9), A(10, 0, 2, 7));
    for (unsigned i = 20; i < 40; i++)
        d[i] = (uint8_t)next();
    g_policy = 0;
    g_arp_answer = 0;
    ipv4_input(&g_nifs[ETH1], packet(d, 40));
    note(48, ipv4_test_forward_parked());
    ipv4_test_hold_forward(false);
}

/* The echo-reply hook unset, then set again, around an echo reply arriving. */
static void op_unhooked(void)
{
    icmp_set_echo_reply_hook(NULL);
    icmp_echo_reply_hook(A(10, 0, 2, 7), 1, 2);
    icmp_set_echo_reply_hook(echo_reply_hook);
    icmp_echo_reply_hook(A(10, 0, 2, 7), 3, 4);
}

static void op_stats(void)
{
    struct ip_stats s;
    memset(&s, 0x33, sizeof(s));
    ipv4_get_stats(&s);
    mix(&s, sizeof(s));
}

static void setup(void)
{
    static bool done;
    if (!done) {
        mbuf_init();
        icmp_set_echo_reply_hook(echo_reply_hook);
        done = true;
    }
    memset(g_nifs, 0, sizeof(g_nifs));
    const char *names[NIFS] = { "lo", "eth0", "tap0", "eth1" };
    for (unsigned i = 0; i < NIFS; i++) {
        struct netif *n = &g_nifs[i];
        memcpy(n->name, names[i], strlen(names[i]) + 1);
        n->index = i + 1;
        n->flags = NETIF_UP;
        n->mtu = 1500;
        for (unsigned k = 0; k < ETH_ALEN; k++)
            n->mac[k] = (uint8_t)next();
    }
    g_nifs[LO].flags |= NETIF_LOOPBACK;
    g_nifs[LO].mtu = 65536;
    g_nifs[LO].ip4.addr = A(127, 0, 0, 1);
    g_nifs[LO].ip4.mask = A(255, 0, 0, 0);
    g_nifs[ETH0].ip4.addr = A(10, 0, 2, 15);
    g_nifs[ETH0].ip4.mask = A(255, 255, 255, 0);
    g_nifs[ETH0].ip4.gateway = A(10, 0, 2, 2);
    g_nifs[TAP0].flags |= NETIF_FORWARD | NETIF_MASQUERADE;
    g_nifs[TAP0].ip4.addr = A(192, 168, 100, 1);
    g_nifs[TAP0].ip4.mask = A(255, 255, 255, 0);
    g_nifs[ETH1].flags |= NETIF_FORWARD;
    g_nifs[ETH1].ip4.addr = A(172, 16, 0, 1);
    g_nifs[ETH1].ip4.mask = A(255, 255, 0, 0);
}

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    setup();
    for (unsigned r = 0; r < ROUNDS; r++) {
        g_now += next() % 16 == 0 ? 1000000000ull + next() % 700000000000ull : next() % 30000000ull;   /* windows, expiry */
        uint64_t k = next() % 32;
        if (k < 14)
            op_input(r);
        else if (k < 20)
            op_output(r);
        else if (k < 22)
            op_echo(r);
        else if (k < 24)
            op_error(r);
        else if (k < 27)
            op_pmtu();
        else if (k < 28)
            op_routes();
        else if (k < 30)
            op_route();
        else if (k < 31)
            op_rate();
        else if (r % 3 == 0)
            op_hook();
        else if (r % 3 == 1)
            op_unhooked();
        else
            op_stats();
    }
    op_stats();
    for (unsigned i = 0; i < NIFS; i++)
        mix64((uint64_t)g_refs[i]);
    printf("  ipv4 digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, ROUNDS);
    EXPECT(g_digest == IPV4_DIGEST);
    EXPECT(g_live == 0);
    for (unsigned i = 0; i < NIFS; i++)
        EXPECT(g_refs[i] == 0);
}

/* IPV4_BENCH=1: the median of nine runs, 100,000 packets each. */
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

static void ipv4_bench(void)
{
    g_rng = SEED;
    setup();
    g_benching = true;
    g_policy = 0;
    g_arp_answer = 0;
    g_tx_rc = 0;
    /* An echo request to the uplink's address from a host on its link. */
    uint8_t echo[20 + 64];
    memset(echo, 0, sizeof(echo));
    echo[20] = ICMP_ECHO;
    uint16_t c = in_cksum(echo + 20, 64);
    memcpy(echo + 22, &c, 2);
    header(echo, 20, sizeof(echo), 64, IPPROTO_ICMP, A(10, 0, 2, 7), A(10, 0, 2, 15));
    uint8_t payload[512] = { 0 };
    double in[9], out[9];
    for (unsigned t = 0; t < 9; t++) {
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 100000; i++) {
            g_now += 20000000ull;   /* 50 a second: under the rate limit */
            struct mbuf *m = m_getcl();
            m_append(m, echo, sizeof(echo));
            ipv4_input(&g_nifs[ETH0], m);
        }
        in[t] = ns_since(&t0) / 100000;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 100000; i++) {
            struct mbuf *m = m_getcl();
            m_append(m, payload, sizeof(payload));
            (void)ipv4_output(m, 0, A(10, 0, 2, 7), IPPROTO_UDP, 64);
        }
        out[t] = ns_since(&t0) / 100000;
    }
    qsort(in, 9, sizeof(double), cmp_double);
    qsort(out, 9, sizeof(double), cmp_double);
    g_benching = false;
    printf("  bench ipv4_input (an echo answered) %.1f ns, ipv4_output (512 bytes, resolved) %.1f ns\n", in[4], out[4]);
}

static const struct host_test tests[] = {
    { "ipv4 digest", test_digest },
};

int main(void)
{
    int rc = harness_run(tests, sizeof(tests) / sizeof(tests[0]));
    if (getenv("IPV4_BENCH") != NULL)
        ipv4_bench();
    return rc;
}
