/*
 * test_fw.c - The firewall (kernel-services/network/fw.c) on the host: roadmap
 * 2.0 SA6. A digest over seeded histories, recorded from the C, of ten guest
 * addresses competing for the eight guest slots, their taps, an uplink and
 * the world beyond it:
 *
 *   - guests attached (past the slots), re-attached and purged; rules added
 *     (well-formed and not, by every owner, at every index, twice, past the
 *     thirty-two a list holds), deleted, listed; policies set and read; the
 *     guest list;
 *   - the FORWARD chain: datagrams of every protocol, short or with options,
 *     to the world and to another guest, new and replies to a recorded flow
 *     (bare SYNs back, ACKs that establish), floods past a guest's share;
 *   - the INPUT chain from a tap, honest and spoofed;
 *   - the host chain from the uplink, and replies to the flows the host
 *     opened (fw_host_flow_of and fw_host_record, the one-entry cache, the
 *     host's share flooded);
 *   - the OUTPUT chain, with its fast path and with rules scoped to the world
 *     or a guest;
 *   - the flow listing, ageing across every timeout, flushes, the statistics,
 *     and every rule that decides a verdict (the FW_HOST_TEST hook).
 *
 * mbuf.c and netparse are real; the clock and the interfaces are the test's.
 * Every mbuf is freed and no lock held at the end. Not reached: a flow refused
 * for a full table with its initiator's share not spent -- the shares add up
 * to the table, and only attached guests (eight) and the host record flows.
 *
 * FW_BENCH=1 times a guest-to-guest datagram on a recorded flow, and one
 * judged by a guest's full rule list.
 */

#include "harness.h"

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/mbuf.h>
#include <kernel/net/fw.h>
#include <kernel/net/inet.h>
#include <kernel/net/ip.h>
#include <kernel/net/tapsvc.h>
#include <kernel/net/tcp.h>
#include <kernel/netif.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The digest of the C (fw.c, 2026-11-03), which the CeeSafe port gives. */
#define FW_DIGEST 0xc99643fb9523e2f5ull
#define ROUNDS 20000u
#define SEED 0xC05F5FA2026B0019ull
#define NGUESTS 10u

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
    mix32(0x5e00u + which);
    mix64(value);
}

/* mbuf.c's caches over malloc. */
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

static uint64_t g_now = 7000000000ull;

uint64_t clock_now_ns(void) { return g_now; }

/* ---- the network ---- */

#define A(a, b, c, d) IPV4_ADDR(a, b, c, d)

static struct netif g_taps[NGUESTS];
static struct netif g_uplink;

static uint32_t guest_ip(unsigned i) { return A(192, 168, 100 + i, 15); }
static uint32_t gateway_ip(unsigned i) { return A(192, 168, 100 + i, 1); }

static const uint16_t PORTS[] = { 53, 80, 7, 1234, 4000, 0 };

static uint16_t some_port(void) { return PORTS[next() % (sizeof(PORTS) / sizeof(PORTS[0]))]; }

/* An address of one kind: a guest, a gateway, the uplink's, a world peer, somewhere random. */
static uint32_t address(void)
{
    switch (next() % 8) {
    case 0:
    case 1: return guest_ip((unsigned)(next() % NGUESTS));
    case 2: return gateway_ip((unsigned)(next() % NGUESTS));
    case 3: return A(10, 0, 2, 15);
    case 4: return A(10, 0, 2, 2);
    case 5: return A(8, 8, 8, 8);
    case 6: return A(93, 184, 216, 34);
    default: return (uint32_t)next();
    }
}

static uint8_t some_prefix(void)
{
    static const uint8_t P[] = { 0, 0, 8, 16, 24, 32, 32 };
    return next() % 16 == 0 ? (uint8_t)(next() % 40) : P[next() % sizeof(P)];
}

/* A rule for an owner: well formed most of the time, every field sometimes wrong. */
static void some_rule(struct fw_rule *r, bool host)
{
    memset(r, 0, sizeof(*r));
    if (next() % 16 == 0)
        r->direction = (uint8_t)(next() % 8);
    else if (host)
        r->direction = next() % 2 ? FW_DIR_FROM_UPLINK : FW_DIR_OUTPUT;
    else
        r->direction = (uint8_t)(next() % 4);   /* ANY, TO_UPLINK, TO_GUEST, TO_HOST */
    static const uint8_t PROTOS[] = { 0, IPPROTO_TCP, IPPROTO_UDP, IPPROTO_ICMP };
    r->proto = next() % 16 == 0 ? (uint8_t)next() : PROTOS[next() % 4];
    r->dst_prefix = some_prefix();
    if (r->dst_prefix != 0 || next() % 32 == 0) {
        uint32_t a = address();
        r->dst_ip = a;
    }
    r->verdict = next() % 32 == 0 ? 2 : (uint8_t)(next() % 2);
    if (r->proto == IPPROTO_ICMP) {
        static const uint16_t T[] = { ICMP_ECHO, ICMP_ECHO_REPLY, 3, FW_ICMP_TYPE_ANY, 300 };
        r->dst_port = T[next() % 5];
    } else {
        r->dst_port = some_port();
    }
    if ((host && next() % 2) || next() % 32 == 0) {
        r->src_prefix = some_prefix();
        if (r->src_prefix != 0 || next() % 32 == 0) {
            uint32_t a = address();
            r->src_ip = a;
        }
    }
    if (r->direction == FW_DIR_OUTPUT)
        r->scope = (uint8_t)(next() % 3);
    else if (next() % 32 == 0)
        r->scope = (uint8_t)(next() % 4);
}

/* An owner: the host object, a guest address, or one never attached. */
static uint32_t some_owner(void)
{
    switch (next() % 4) {
    case 0: return FW_HOST_GUEST_IP;
    case 1: return A(172, 16, 0, 9);
    default: return guest_ip((unsigned)(next() % NGUESTS));
    }
}

/* ---- packets ---- */

struct l4 {
    uint8_t proto;
    uint16_t sport, dport;   /* TCP/UDP */
    uint8_t tcp_flags;
    uint8_t icmp_type;
    uint16_t icmp_id;
};

/* The transport header's bytes into b; how many. */
static unsigned l4_bytes(const struct l4 *t, uint8_t *b)
{
    memset(b, 0, 20);
    for (unsigned k = 0; k < 20; k++)
        b[k] = (uint8_t)next();
    if (t->proto == IPPROTO_ICMP) {
        b[0] = t->icmp_type;
        b[4] = (uint8_t)(t->icmp_id >> 8);
        b[5] = (uint8_t)t->icmp_id;
        return 8;
    }
    b[0] = (uint8_t)(t->sport >> 8);
    b[1] = (uint8_t)t->sport;
    b[2] = (uint8_t)(t->dport >> 8);
    b[3] = (uint8_t)t->dport;
    if (t->proto == IPPROTO_TCP) {
        b[12] = 0x50;
        b[13] = t->tcp_flags;
        return 20;
    }
    return 8;
}

/* An mbuf holding `bytes`, sometimes split across two. */
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

/* A datagram with its IPv4 header (in `hdr` too, as the callers hand it); `*ihl` its length. Sometimes short. */
static struct mbuf *datagram(const struct l4 *t, uint32_t src, uint32_t dst, uint8_t *hdr, unsigned *ihl)
{
    unsigned h = next() % 4 == 0 ? 20 + 4 * (unsigned)(next() % 11) : 20;
    uint8_t bytes[80];
    for (unsigned k = 0; k < h; k++)
        bytes[k] = (uint8_t)next();
    struct ipv4_hdr *iph = (struct ipv4_hdr *)bytes;
    iph->vhl = (uint8_t)(0x40 | h / 4);
    iph->proto = t->proto;
    iph->src = src;
    iph->dst = dst;
    unsigned n = l4_bytes(t, bytes + h);
    if (next() % 16 == 0)
        n = (unsigned)(next() % n);   /* short: the transport header cut */
    memcpy(hdr, bytes, h);
    *ihl = h;
    return packet(bytes, h + n);
}

static void some_l4(struct l4 *t)
{
    memset(t, 0, sizeof(*t));
    static const uint8_t PROTOS[] = { IPPROTO_TCP, IPPROTO_UDP, IPPROTO_UDP, IPPROTO_ICMP, 47 };
    t->proto = PROTOS[next() % 5];
    uint16_t sport = some_port();
    t->sport = sport;
    t->dport = some_port();
    static const uint8_t FLAGS[] = { TH_SYN, TH_SYN | TH_ACK, TH_ACK, TH_ACK | 0x08, 0x01 | TH_ACK };
    t->tcp_flags = FLAGS[next() % 5];
    static const uint8_t TYPES[] = { ICMP_ECHO, ICMP_ECHO_REPLY, 3, 11 };
    t->icmp_type = TYPES[next() % 4];
    t->icmp_id = (uint16_t)(1 + next() % 3);
}

/* A live flow, as listed, or false. */
static bool some_flow(struct fw_flow_info *out)
{
    static struct fw_flow_info flows[FW_FLOW_MAX];
    unsigned n = fw_flow_list(flows, FW_FLOW_MAX, g_now);
    if (n == 0)
        return false;
    *out = flows[next() % n];
    return true;
}

/* The reply to a flow (or, sometimes, the flow again): its tuple reversed, of the flow's protocol. */
static void reply_to(const struct fw_flow_info *f, struct l4 *t, uint32_t *src, uint32_t *dst)
{
    some_l4(t);
    t->proto = f->proto;
    bool again = next() % 4 == 0;
    *src = again ? f->a_ip : f->b_ip;
    *dst = again ? f->b_ip : f->a_ip;
    t->sport = again ? f->a_port : f->b_port;
    t->dport = again ? f->b_port : f->a_port;
    t->icmp_type = again ? ICMP_ECHO : ICMP_ECHO_REPLY;
    t->icmp_id = f->a_port;
}

static void on_rule_matched(uint32_t owner_ip, const struct fw_rule *r)
{
    if (g_benching)
        return;
    note(1, owner_ip);
    mix(r, sizeof(*r));
}

/* ---- the histories ---- */

static void op_attach(void)
{
    unsigned i = (unsigned)(next() % NGUESTS);
    fw_guest_attach(guest_ip(i), gateway_ip(i));
    note(10, i);
}

static void op_purge(void)
{
    unsigned i = (unsigned)(next() % NGUESTS);
    fw_guest_purge(guest_ip(i));
    note(11, i);
}

static void op_rule_add(void)
{
    uint32_t owner = some_owner();
    struct fw_rule r;
    some_rule(&r, owner == FW_HOST_GUEST_IP ? next() % 16 != 0 : next() % 16 == 0);
    unsigned at = (unsigned)(next() % 40);
    int rc = fw_rule_add(owner, at, &r);
    note(12, ((uint64_t)owner << 32) | (uint32_t)rc);
    mix32(fw_test_out_fast());
}

static void op_rule_del(void)
{
    uint32_t owner = some_owner();
    struct fw_rule rules[FW_RULES_PER_GUEST];
    unsigned n = fw_rule_list(owner, rules, FW_RULES_PER_GUEST);
    struct fw_rule r;
    if (n > 0 && next() % 4 != 0)
        r = rules[next() % n];
    else
        some_rule(&r, owner == FW_HOST_GUEST_IP);
    int rc = fw_rule_del(owner, &r);
    note(13, ((uint64_t)owner << 32) | (uint32_t)rc);
    mix32(fw_test_out_fast());
}

static void op_rule_list(void)
{
    uint32_t owner = some_owner();
    struct fw_rule rules[FW_RULES_PER_GUEST];
    unsigned max = (unsigned)(next() % (FW_RULES_PER_GUEST + 1));
    unsigned n = fw_rule_list(owner, rules, max);
    note(14, ((uint64_t)owner << 32) | n);
    mix(rules, n * sizeof(rules[0]));
}

static void op_policy(void)
{
    uint32_t owner = some_owner();
    uint8_t dir = (uint8_t)(next() % 7);
    uint8_t verdict = next() % 16 == 0 ? 2 : (uint8_t)(next() % 2);
    int rc = fw_policy_set(owner, dir, verdict);
    note(15, ((uint64_t)owner << 32) | ((uint64_t)dir << 24) | ((uint64_t)verdict << 16) | (uint16_t)rc);
    uint8_t p[5];
    memset(p, 0x77, sizeof(p));
    rc = fw_policy_get(some_owner(), &p[0], &p[1], &p[2], &p[3], &p[4]);
    note(16, (uint32_t)rc);
    mix(p, sizeof(p));
    mix32(fw_test_out_fast());
}

static void op_guest_list(void)
{
    uint32_t out[FW_MAX_GUESTS];
    unsigned max = (unsigned)(next() % (FW_MAX_GUESTS + 1));
    unsigned n = fw_guest_list(out, max);
    note(17, n);
    mix(out, n * sizeof(out[0]));
}

static void op_forward(void)
{
    struct l4 t;
    uint32_t src, dst;
    struct fw_flow_info f;
    if (next() % 3 == 0 && some_flow(&f)) {
        reply_to(&f, &t, &src, &dst);
    } else {
        some_l4(&t);
        src = next() % 8 == 0 ? address() : guest_ip((unsigned)(next() % NGUESTS));
        dst = next() % 2 ? guest_ip((unsigned)(next() % NGUESTS)) : address();
    }
    unsigned to_guest = (dst & htonl(0xffff0000u)) == A(192, 168, 0, 0);
    unsigned out_flags = to_guest ? (NETIF_UP | NETIF_FORWARD | NETIF_MASQUERADE) : NETIF_UP;
    uint8_t hdr[60];
    unsigned ihl;
    struct mbuf *m = datagram(&t, src, dst, hdr, &ihl);
    unsigned in = (unsigned)(next() % NGUESTS);
    enum fw_verdict v = fw_forward_verdict(&g_taps[in], to_guest ? &g_taps[0] : &g_uplink, out_flags, m,
                                           (const struct ipv4_hdr *)hdr, ihl);
    note(18, ((uint64_t)v << 32) | ((uint64_t)t.proto << 8) | to_guest);
    m_freem(m);
}

/* Forty new flows from one guest to another, past its share of thirty-two. */
static void op_flood(void)
{
    unsigned i = (unsigned)(next() % NGUESTS), j = (unsigned)(next() % NGUESTS);
    if (next() % 2)
        (void)fw_policy_set(guest_ip(i), FW_DIR_TO_GUEST, FW_ACCEPT);
    for (unsigned k = 0; k < 40; k++) {
        struct l4 t;
        memset(&t, 0, sizeof(t));
        t.proto = IPPROTO_UDP;
        t.sport = (uint16_t)(20000 + k);
        t.dport = 53;
        uint8_t hdr[60];
        unsigned ihl;
        struct mbuf *m = datagram(&t, guest_ip(i), guest_ip(j), hdr, &ihl);
        enum fw_verdict v = fw_forward_verdict(&g_taps[i], &g_taps[j], NETIF_UP | NETIF_FORWARD, m,
                                               (const struct ipv4_hdr *)hdr, ihl);
        mix32(v);
        m_freem(m);
    }
    note(19, ((uint64_t)i << 8) | j);
}

static void op_input(void)
{
    unsigned i = (unsigned)(next() % NGUESTS);
    struct l4 t;
    some_l4(&t);
    uint32_t src = next() % 8 == 0 ? address() : guest_ip(i);
    uint32_t dst = next() % 4 == 0 ? A(255, 255, 255, 255) : gateway_ip(i);
    uint8_t hdr[60];
    unsigned ihl;
    struct mbuf *m = datagram(&t, src, dst, hdr, &ihl);
    struct netif *nif = &g_taps[i];
    uint32_t saved = nif->ip4.addr;
    if (next() % 16 == 0)
        nif->ip4.addr = 0;   /* an unconfigured tap: no anti-spoof */
    enum fw_verdict v = fw_input_verdict(nif, m, (const struct ipv4_hdr *)hdr, ihl);
    nif->ip4.addr = saved;
    note(20, ((uint64_t)v << 32) | ((uint64_t)t.proto << 8) | i);
    m_freem(m);
}

static void op_host_in(void)
{
    struct l4 t;
    uint32_t src, dst;
    struct fw_flow_info f;
    if (next() % 2 && some_flow(&f)) {
        reply_to(&f, &t, &src, &dst);
    } else {
        some_l4(&t);
        src = address();
        dst = next() % 8 == 0 ? A(10, 0, 2, 255) : A(10, 0, 2, 15);
    }
    uint8_t hdr[60];
    unsigned ihl;
    struct mbuf *m = datagram(&t, src, dst, hdr, &ihl);
    enum fw_verdict v = fw_host_verdict(&g_uplink, m, (const struct ipv4_hdr *)hdr, ihl);
    note(21, ((uint64_t)v << 32) | t.proto);
    m_freem(m);
}

/* The host sends: the flow it would open, recorded (mostly) once sent. */
static void host_send(const struct l4 *t, uint32_t dst, unsigned out_flags)
{
    uint8_t b[20];
    unsigned n = l4_bytes(t, b);
    if (next() % 16 == 0)
        n = (unsigned)(next() % n);
    struct mbuf *m = packet(b, n);
    struct fw_host_flow f;
    memset(&f, 0x5a, sizeof(f));
    uint32_t src = out_flags & NETIF_MASQUERADE ? gateway_ip(0) : A(10, 0, 2, 15);
    bool opens = fw_host_flow_of(out_flags & NETIF_MASQUERADE ? &g_taps[0] : &g_uplink, out_flags, m, src, dst,
                                 t->proto, &f);
    note(22, opens);
    if (opens) {
        mix32(f.src);
        mix32(f.dst);
        mix32(f.a_port);
        mix32(f.b_port);
        mix32(f.proto);
        if (next() % 8 != 0)
            fw_host_record(&f);
    }
    m_freem(m);
}

static void op_host_send(void)
{
    struct l4 t;
    some_l4(&t);
    static const unsigned FLAGS[] = { NETIF_UP, NETIF_UP, NETIF_UP | NETIF_MASQUERADE, NETIF_UP | NETIF_LOOPBACK };
    unsigned flags = FLAGS[next() % 4];
    if (next() % 2)
        t.dport = 53;   /* one peer and port again: the cache */
    host_send(&t, next() % 2 ? A(8, 8, 8, 8) : address(), flags);
}

/* Seventy flows the host opens, past its share of sixty-four. */
static void op_host_flood(void)
{
    for (unsigned k = 0; k < 70; k++) {
        struct l4 t;
        memset(&t, 0, sizeof(t));
        t.proto = IPPROTO_UDP;
        t.sport = (uint16_t)(30000 + k);
        t.dport = 123;
        host_send(&t, A(1, 1, 1, 1), NETIF_UP);
    }
    note(23, 0);
}

static void op_output(void)
{
    struct l4 t;
    some_l4(&t);
    uint8_t b[20];
    unsigned n = l4_bytes(&t, b);
    if (next() % 16 == 0)
        n = (unsigned)(next() % n);
    struct mbuf *m = packet(b, n);
    unsigned flags = next() % 2 ? NETIF_UP | NETIF_MASQUERADE : NETIF_UP;
    uint32_t src = address();
    uint32_t dst = address();
    enum fw_verdict v = fw_output_verdict(flags & NETIF_MASQUERADE ? &g_taps[0] : &g_uplink, flags, m, src, dst,
                                          t.proto);
    note(24, ((uint64_t)v << 32) | ((uint64_t)t.proto << 8) | (flags >> 6));
    m_freem(m);
}

static void op_flows(void)
{
    static struct fw_flow_info flows[FW_FLOW_MAX];
    unsigned max = next() % 4 == 0 ? FW_FLOW_MAX : (unsigned)(next() % 8);
    unsigned n = fw_flow_list(flows, max, g_now);
    note(25, ((uint64_t)max << 32) | n);
    for (unsigned k = 0; k < n; k++) {
        mix32(flows[k].guest_ip);
        mix32(flows[k].a_ip);
        mix32(flows[k].b_ip);
        mix32(flows[k].a_port);
        mix32(flows[k].b_port);
        mix32(flows[k].proto);
        mix32(flows[k].est);
        mix64(flows[k].expires_ns);
    }
}

static void op_stats(void)
{
    struct fw_stats s;
    memset(&s, 0x33, sizeof(s));
    fw_get_stats(&s);
    mix(&s, sizeof(s));
}

static void setup(void)
{
    static bool done;
    if (!done) {
        mbuf_init();
        done = true;
    }
    memset(g_taps, 0, sizeof(g_taps));
    for (unsigned i = 0; i < NGUESTS; i++) {
        g_taps[i].flags = NETIF_UP | NETIF_FORWARD | NETIF_MASQUERADE | NETIF_NODEFAULT;
        g_taps[i].ip4.addr = gateway_ip(i);
        g_taps[i].ip4.mask = A(255, 255, 255, 0);
    }
    memset(&g_uplink, 0, sizeof(g_uplink));
    g_uplink.flags = NETIF_UP;
    g_uplink.ip4.addr = A(10, 0, 2, 15);
    g_uplink.ip4.mask = A(255, 255, 255, 0);
    fw_flush();
    for (unsigned i = 0; i < NGUESTS; i++)
        fw_guest_purge(guest_ip(i));
}

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    setup();
    fw_test_set_rule_matched(on_rule_matched);
    for (unsigned r = 0; r < ROUNDS; r++) {
        uint64_t jump = next() % 64;
        g_now += jump == 0 ? 301000000000ull : jump == 1 ? 31000000000ull : next() % 2000000000ull;
        uint64_t k = next() % 64;
        if (k < 4)
            op_attach();
        else if (k < 6)
            op_purge();
        else if (k < 14)
            op_rule_add();
        else if (k < 18)
            op_rule_del();
        else if (k < 20)
            op_rule_list();
        else if (k < 24)
            op_policy();
        else if (k < 25)
            op_guest_list();
        else if (k < 37)
            op_forward();
        else if (k < 42)
            op_input();
        else if (k < 49)
            op_host_in();
        else if (k < 55)
            op_host_send();
        else if (k < 59)
            op_output();
        else if (k < 60)
            op_flows();
        else if (k < 61) {
            fw_age(g_now);
            note(26, 0);
        } else if (k < 62)
            op_stats();
        else if (r % 2)
            op_flood();
        else if (r % 4 == 0)
            op_host_flood();
        else if (r % 64 == 2) {
            fw_flush();
            note(27, 0);
        } else
            op_flows();
        EXPECT(harness_locks_held() == 0);
    }
    op_stats();
    fw_test_set_rule_matched(NULL);
    printf("  fw digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, ROUNDS);
    EXPECT(g_digest == FW_DIGEST);
    EXPECT(g_live == 0);
}

/* FW_BENCH=1: the median of nine runs, 100,000 datagrams each. */
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

static void fw_bench(void)
{
    g_rng = SEED;
    setup();
    g_benching = true;
    fw_guest_attach(guest_ip(0), gateway_ip(0));
    fw_guest_attach(guest_ip(1), gateway_ip(1));
    /* Guest 0's list full of rules that do not match, then its default for the other guest: ACCEPT. */
    for (unsigned k = 0; k < FW_RULES_PER_GUEST - 2; k++) {
        struct fw_rule r = { .direction = FW_DIR_TO_GUEST, .proto = IPPROTO_TCP, .dst_prefix = 32,
                             .verdict = FW_DROP, .dst_ip = guest_ip(1), .dst_port = (uint16_t)(1000 + k) };
        EXPECT(fw_rule_add(guest_ip(0), 99, &r) == 0);
    }
    EXPECT(fw_policy_set(guest_ip(0), FW_DIR_TO_GUEST, FW_ACCEPT) == 0);
    struct l4 t = { .proto = IPPROTO_UDP, .sport = 5000, .dport = 53 };
    uint8_t hdr[60], first[60];
    unsigned ihl;
    struct mbuf *m = datagram(&t, guest_ip(0), guest_ip(1), first, &ihl);
    m_freem(m);
    /* One fixed datagram: its bytes again, unsplit. */
    uint8_t bytes[28];
    memset(bytes, 0, sizeof(bytes));
    bytes[0] = 0x45;
    bytes[9] = IPPROTO_UDP;
    uint32_t s = guest_ip(0), d = guest_ip(1);
    memcpy(bytes + 12, &s, 4);
    memcpy(bytes + 16, &d, 4);
    bytes[20] = 5000 >> 8;
    bytes[21] = 5000 & 0xff;
    bytes[23] = 53;
    memcpy(hdr, bytes, 20);
    double est[9], rules[9];
    for (unsigned r = 0; r < 9; r++) {
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 100000; i++) {
            struct mbuf *p = m_get();
            m_append(p, bytes, sizeof(bytes));
            (void)fw_forward_verdict(&g_taps[0], &g_taps[1], NETIF_UP | NETIF_FORWARD, p,
                                     (const struct ipv4_hdr *)hdr, 20);
            m_freem(p);
        }
        est[r] = ns_since(&t0) / 100000;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 100000; i++) {
            struct mbuf *p = m_get();
            m_append(p, bytes, sizeof(bytes));
            (void)fw_input_verdict(&g_taps[0], p, (const struct ipv4_hdr *)hdr, 20);
            m_freem(p);
        }
        rules[r] = ns_since(&t0) / 100000;
    }
    qsort(est, 9, sizeof(double), cmp_double);
    qsort(rules, 9, sizeof(double), cmp_double);
    g_benching = false;
    fw_flush();
    printf("  bench fw_forward_verdict (a recorded flow) %.1f ns, fw_input_verdict (a list of %u) %.1f ns\n", est[4],
           FW_RULES_PER_GUEST, rules[4]);
}

static const struct host_test tests[] = {
    { "fw digest", test_digest },
};

int main(void)
{
    int rc = harness_run(tests, sizeof(tests) / sizeof(tests[0]));
    if (getenv("FW_BENCH") != NULL)
        fw_bench();
    return rc;
}
