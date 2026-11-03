/*
 * test_nd.c - Neighbour discovery (kernel-services/network/nd.c,
 * kernel/include/kernel/net/ip.h's ND half) on the host: roadmap 2.0 SA6.
 * A digest over seeded histories, recorded from the C, of three interfaces
 * and a pool of 48 link-local neighbours, more than the table's 32 entries,
 * so resolutions evict: resolutions with and without a packet to park
 * (multicast destinations, up and down interfaces, entries reachable,
 * incomplete or absent), lookups, neighbour solicitations in (for our
 * address or another, from the unspecified address, with and without a
 * source link-layer address option, short, split across two mbufs, with
 * m_pullup's allocation failing), neighbour advertisements in (solicited or
 * not, with and without a target link-layer address option), flushes,
 * ageing passes as time moves (retries, timeouts, expiry), the debug build's
 * hooks, and allocation failures in the messages the table sends (the
 * cluster, the IPv6 header's prepend).
 *
 * mbuf.c, the checksum and netparse are real; ether_output, the clock and
 * the interface references are stubs that fold each call into the digest.
 * Every mbuf is freed at the end.
 *
 * ND_BENCH=1 times a resolution of a reachable neighbour and a solicitation
 * for us, answered.
 */

#include "harness.h"

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/mbuf.h>
#include <kernel/net/ether.h>
#include <kernel/net/ip.h>
#include <kernel/netif.h>
#include <kernel/object.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* nd.cee's counter, which ipv6.c reads for ip_stats.nd_pending_dropped (no header declares it). */
uint64_t nd_pending_count(void);

/* The digest of the C (nd.c, 2026-11-03), which the CeeSafe port gives. */
#define ND_DIGEST 0xc4e59aaf81eb00c3ull
#define ROUNDS 20000u
#define SEED 0xC05F5FA2026B000Full
#define NIFS 3u
#define POOL 48u

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

/* A call to a stub: which, and a value. */
static void note(uint32_t which, uint64_t value)
{
    mix32(0x5900u + which);
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

/* ---- the kernel services nd.c calls ---- */

static bool g_benching;
static uint64_t g_now = 1000000000ull;
static struct netif g_nifs[NIFS];

uint64_t clock_now_ns(void) { return g_now; }

static unsigned nif_index(const struct netif *nif) { return (unsigned)(nif - g_nifs); }

/* The frame as it leaves: the interface, the destination, the type and the bytes (the IPv6 header, the message). */
int ether_output(struct netif *nif, struct mbuf *m, const uint8_t *dst, uint16_t type)
{
    if (!g_benching) {
        uint8_t bytes[96];
        uint32_t n = m_length(m);
        uint32_t take = n < sizeof(bytes) ? n : (uint32_t)sizeof(bytes);
        m_copydata(m, 0, take, bytes);
        note(1, nif_index(nif));
        mix(dst, ETH_ALEN);
        mix32(type);
        mix32(n);
        mix32(m->pkt.len);
        mix(&m->pkt.proto, sizeof(m->pkt.proto));
        mix(bytes, take);
    }
    m_freem(m);
    return 0;
}

static long g_refs[NIFS];

void kobject_get(struct kobject *obj) { g_refs[nif_index(container_of(obj, struct netif, obj))]++; }
void kobject_put(struct kobject *obj) { g_refs[nif_index(container_of(obj, struct netif, obj))]--; }

/* ---- the histories ---- */

static struct in6_addr g_pool[POOL];

static struct in6_addr neighbour(void) { return g_pool[next() % POOL]; }

static struct netif *some_nif(void) { return &g_nifs[next() % NIFS]; }

/* A packet to park: an IPv6 payload whose bytes say which one it is. */
static struct mbuf *packet(void)
{
    struct mbuf *m = m_get();
    EXPECT(m != NULL);
    m->flags |= M_PKTHDR;
    uint8_t body[24];
    for (unsigned i = 0; i < sizeof(body); i++)
        body[i] = (uint8_t)next();
    m_append(m, body, sizeof(body));
    return m;
}

static void op_resolve(unsigned r)
{
    struct netif *nif = some_nif();
    struct in6_addr ip = neighbour();
    if (next() % 12 == 0) {
        ip.s6_addr[0] = 0xff;           /* multicast: its MAC is computed, nothing is asked */
        ip.s6_addr[1] = (uint8_t)next();
    }
    struct mbuf *m = next() % 3 ? packet() : NULL;
    uint8_t mac[ETH_ALEN];
    memset(mac, 0xa5, sizeof(mac));
    if (r % 37 == 0)
        g_fail_in = (int)(next() % 2);  /* the solicitation's cluster, or its IPv6 header's prepend */
    int rc = nd_resolve(nif, &ip, mac, m);
    g_fail_in = -1;
    note(10, (uint64_t)(int64_t)rc);
    if (rc == 0) {
        mix(mac, ETH_ALEN);
        m_freem(m);   /* resolved at once: the packet stays the caller's, to send */
    }
}

static void op_lookup(void)
{
    struct netif *nif = some_nif();   /* drawn one after the other: argument order is the compiler's */
    struct in6_addr ip = neighbour();
    uint8_t mac[ETH_ALEN];
    memset(mac, 0x5a, sizeof(mac));
    bool ok = nd_lookup(nif, &ip, mac);
    note(11, ok);
    mix(mac, ETH_ALEN);
}

/* An ND message in, as icmpv6_input hands it over (hop limit 255 checked there): type, code, checksum, flags, the
 * target, and an option. */
static void op_message(unsigned r, bool solicitation)
{
    struct netif *nif = some_nif();
    uint8_t msg[32];
    for (unsigned i = 0; i < sizeof(msg); i++)
        msg[i] = (uint8_t)next();
    msg[0] = solicitation ? 135 : 136;
    msg[1] = 0;
    struct in6_addr target = next() % 3 ? (solicitation ? nif->ip6_ll : neighbour()) : neighbour();
    memcpy(msg + 8, target.s6_addr, 16);
    uint64_t ok = next() % 8;
    msg[24] = ok < 6 ? (solicitation ? 1 : 2) : (uint8_t)(next() % 4);   /* the option's type, or another */
    msg[25] = ok < 7 ? 1 : (uint8_t)(next() % 3);                        /* its length in 8-byte units */
    struct ipv6_hdr ip6;
    memset(&ip6, 0, sizeof(ip6));
    ip6.hoplimit = 255;
    ip6.src = next() % 16 == 0 ? (struct in6_addr){ { 0 } } : neighbour();
    ip6.dst = nif->ip6_ll;
    uint32_t len = next() % 32 == 0 ? (uint32_t)(next() % 32) : 32;
    struct mbuf *m = m_get();
    EXPECT(m != NULL);
    m->flags |= M_PKTHDR;
    uint32_t split = len > 1 && next() % 4 == 0 ? (uint32_t)(1 + next() % (len - 1)) : len;
    m_append(m, msg, split);
    if (split < len) {
        struct mbuf *tail = m_get();
        EXPECT(tail != NULL);
        memcpy(tail->data, msg + split, len - split);
        tail->len = len - split;
        m->next = tail;
        m->pkt.len = len;
    }
    if (r % 83 == 0)
        g_fail_in = (int)(next() % 3);  /* m_pullup's allocation, or the advertisement's cluster or prepend */
    if (solicitation)
        nd_input_ns(nif, m, &ip6);
    else
        nd_input_na(nif, m, &ip6);
    g_fail_in = -1;
}

static void op_stats(void) { mix64(nd_pending_count()); }

static void op_updown(void)
{
    struct netif *nif = some_nif();
    if (nif->flags & NETIF_UP) {
        nif->flags &= ~NETIF_UP;
        nd_flush(nif);                  /* netif_set_up(false)'s order: the flag, then the flush */
    } else {
        nif->flags |= NETIF_UP;
    }
    note(13, nif->flags);
}

/* Many resolutions at once, before any ageing: the table fills and evicts, and a solicitation finds no room. */
static void op_burst(unsigned r)
{
    unsigned n = 20u + (unsigned)(next() % 30u);
    for (unsigned i = 0; i < n; i++) {
        uint8_t mac[ETH_ALEN];
        g_now += 1000;
        struct in6_addr ip = g_pool[(i * 7u + r) % POOL];
        int rc = nd_resolve(&g_nifs[(i + r) % NIFS], &ip, mac, NULL);
        note(14, (uint64_t)(int64_t)rc);
    }
}

/* The debug build's hooks (CONFIG_DEBUG), single-threaded: each is released before the call that parks on it, so the
 * call passes through the park at once. Armed for another interface, a caller does not park. */
static void op_hooks(unsigned r)
{
    struct netif *nif = some_nif();
    struct netif *other = &g_nifs[(nif_index(nif) + 1u) % NIFS];
    uint8_t mac[ETH_ALEN];
    nd_test_hold_lock_entry(next() % 2 ? nif : other);
    note(16, nd_test_lock_entry_parked());
    nd_test_release_lock_entry();
    struct in6_addr ip = neighbour();
    int rc = nd_resolve(nif, &ip, mac, NULL);
    note(17, (uint64_t)(int64_t)rc);
    note(18, nd_test_lock_entry_parked());
    nd_test_hold_lock_entry(NULL);
    nd_test_hold_retry(r % 2 == 0);
    note(19, nd_test_retry_parked());
    nd_test_release_retry();
    g_now += 1100000000ull;
    nd_age(g_now);
    note(20, nd_test_retry_parked());
    nd_test_hold_retry(false);
}

static void op_age(unsigned r)
{
    uint64_t step = next() % 8;
    if (step < 5)
        g_now += next() % 1500000000ull;                      /* within the retry interval, or past it */
    else if (step < 7)
        g_now += 20ull * 60 * 1000000000ull + next() % 1000;  /* past a reachable entry's life */
    if (r % 7 == 0)
        g_fail_in = (int)(next() % 4);  /* a retry's cluster or prepend */
    nd_age(g_now);
    g_fail_in = -1;
}

static void setup(void)
{
    static bool done;
    if (!done) {
        mbuf_init();
        done = true;
    }
    nd_init();
    memset(g_nifs, 0, sizeof(g_nifs));
    for (unsigned n = 0; n < NIFS; n++) {
        struct netif *nif = &g_nifs[n];
        snprintf(nif->name, sizeof(nif->name), "eth%u", n);
        for (unsigned i = 0; i < ETH_ALEN; i++)
            nif->mac[i] = (uint8_t)next();
        nif->mac[0] = (uint8_t)((nif->mac[0] & 0xfe) | 0x02);
        nif->flags = NETIF_UP;
        nif->ip6_ll.s6_addr[0] = 0xfe;                                /* fe80::/64, then the EUI-64 of the MAC */
        nif->ip6_ll.s6_addr[1] = 0x80;
        nif->ip6_ll.s6_addr[8] = (uint8_t)(nif->mac[0] ^ 2);
        nif->ip6_ll.s6_addr[9] = nif->mac[1];
        nif->ip6_ll.s6_addr[10] = nif->mac[2];
        nif->ip6_ll.s6_addr[11] = 0xff;
        nif->ip6_ll.s6_addr[12] = 0xfe;
        nif->ip6_ll.s6_addr[13] = nif->mac[3];
        nif->ip6_ll.s6_addr[14] = nif->mac[4];
        nif->ip6_ll.s6_addr[15] = nif->mac[5];
    }
    for (unsigned i = 0; i < POOL; i++) {
        memset(&g_pool[i], 0, sizeof(g_pool[i]));
        g_pool[i].s6_addr[0] = 0xfe;
        g_pool[i].s6_addr[1] = 0x80;
        for (unsigned k = 8; k < 16; k++)
            g_pool[i].s6_addr[k] = (uint8_t)next();
    }
}

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    setup();
    for (unsigned r = 0; r < ROUNDS; r++) {
        uint64_t k = next() % 32;
        if (k < 10)
            op_resolve(r);
        else if (k < 13)
            op_lookup();
        else if (k < 18)
            op_message(r, true);
        else if (k < 23)
            op_message(r, false);
        else if (k < 24)
            op_stats();
        else if (k < 25)
            op_updown();
        else if (k < 26)
            nd_flush(some_nif());
        else if (k < 27)
            op_burst(r);
        else if (k < 28 && r % 5 == 0)
            op_hooks(r);
        else
            op_age(r);
    }
    op_stats();
    for (unsigned n = 0; n < NIFS; n++) {
        nd_flush(&g_nifs[n]);
        mix64((uint64_t)g_refs[n]);
    }
    op_stats();
    printf("  nd digest 0x%016llx over %u rounds: %llu dropped while pending\n", (unsigned long long)g_digest, ROUNDS,
           (unsigned long long)nd_pending_count());
    EXPECT(g_digest == ND_DIGEST);
    EXPECT(g_live == 0);
    for (unsigned n = 0; n < NIFS; n++)
        EXPECT(g_refs[n] == 0);
}

/* ND_BENCH=1: the median of nine runs, 200,000 calls each. */
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

static void nd_bench(void)
{
    g_rng = SEED;
    setup();
    g_benching = true;
    struct netif *nif = &g_nifs[0];
    struct in6_addr ip = g_pool[0];
    uint8_t mac[ETH_ALEN];
    /* A reachable neighbour: solicited, then advertised. */
    (void)nd_resolve(nif, &ip, mac, NULL);
    uint8_t na[32] = { 136, 0 };
    memcpy(na + 8, ip.s6_addr, 16);
    na[24] = 2;
    na[25] = 1;
    memcpy(na + 26, "\x02\x11\x22\x33\x44\x55", 6);
    struct ipv6_hdr ip6;
    memset(&ip6, 0, sizeof(ip6));
    ip6.hoplimit = 255;
    ip6.src = ip;
    ip6.dst = nif->ip6_ll;
    struct mbuf *m = m_getcl();
    m_append(m, na, sizeof(na));
    nd_input_na(nif, m, &ip6);
    EXPECT(nd_resolve(nif, &ip, mac, NULL) == 0);
    /* A solicitation for us from that neighbour. */
    uint8_t ns[32];
    memcpy(ns, na, sizeof(ns));
    ns[0] = 135;
    memcpy(ns + 8, nif->ip6_ll.s6_addr, 16);
    ns[24] = 1;
    double hit[9], ask[9];
    for (unsigned t = 0; t < 9; t++) {
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 200000; i++)
            (void)nd_resolve(nif, &ip, mac, NULL);
        hit[t] = ns_since(&t0) / 200000;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 200000; i++) {
            m = m_getcl();
            m_append(m, ns, sizeof(ns));
            nd_input_ns(nif, m, &ip6);
        }
        ask[t] = ns_since(&t0) / 200000;
    }
    qsort(hit, 9, sizeof(double), cmp_double);
    qsort(ask, 9, sizeof(double), cmp_double);
    for (unsigned n = 0; n < NIFS; n++)
        nd_flush(&g_nifs[n]);
    g_benching = false;
    printf("  bench nd_resolve (reachable) %.1f ns, nd_input_ns (a solicitation for us, answered) %.1f ns\n", hit[4],
           ask[4]);
}

static const struct host_test tests[] = {
    { "nd digest", test_digest },
};

int main(void)
{
    int rc = harness_run(tests, sizeof(tests) / sizeof(tests[0]));
    if (getenv("ND_BENCH") != NULL)
        nd_bench();   /* after the digest, whose counter it would change */
    return rc;
}
