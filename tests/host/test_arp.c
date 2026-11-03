/*
 * test_arp.c - Address resolution (kernel-services/network/arp.c,
 * kernel/include/kernel/net/ether.h's ARP half) on the host: roadmap 2.0
 * SA6. A digest over seeded histories, recorded from the C, of three
 * interfaces and a pool of 96 neighbour addresses, more than the table's 64
 * entries, so resolutions evict: resolutions with and without a packet to
 * park (broadcast destinations, up and down interfaces, entries reachable,
 * incomplete or absent), lookups, ARP frames in (requests and replies, for
 * us or for another host, from ourselves or from 0.0.0.0, solicited or not,
 * malformed, split across two mbufs, and with m_pullup's allocation
 * failing), flushes, deletions, listings, ageing passes as time moves (the
 * retries, the timeouts, the expiry of reachable entries) and the age work
 * item, with allocation failures in the requests the table sends.
 *
 * mbuf.c and netparse are real; ether_output, the timer, the work queue, the
 * other protocols' ageing and the interface references are stubs that fold
 * each call into the digest. Every mbuf is freed at the end.
 *
 * ARP_BENCH=1 times a resolution of a reachable neighbour and a request for
 * us, answered.
 */

#include "harness.h"

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/mbuf.h>
#include <kernel/net/ether.h>
#include <kernel/net/fw.h>
#include <kernel/net/ip.h>
#include <kernel/net/nat.h>
#include <kernel/net/tapsvc.h>
#include <kernel/netif.h>
#include <kernel/object.h>
#include <kernel/timer.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The digest of the C (arp.c, 2026-11-03), which the CeeSafe port gives. */
#define ARP_DIGEST 0xc5ce56f4e28bdf48ull
#define ROUNDS 20000u
#define SEED 0xC05F5FA2026B000Eull
#define NIFS 3u
#define POOL 96u

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
    mix32(0x5800u + which);
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

/* ---- the kernel services arp.c calls ---- */

static bool g_benching;
static uint64_t g_now = 1000000000ull;
static struct netif g_nifs[NIFS];

const uint8_t eth_broadcast[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

uint64_t clock_now_ns(void) { return g_now; }

static unsigned nif_index(const struct netif *nif) { return (unsigned)(nif - g_nifs); }

/* The frame as it leaves: the interface, the destination, the type and the payload. */
int ether_output(struct netif *nif, struct mbuf *m, const uint8_t *dst, uint16_t type)
{
    if (!g_benching) {
        uint8_t bytes[64];
        uint32_t n = m_length(m);
        uint32_t take = n < sizeof(bytes) ? n : (uint32_t)sizeof(bytes);
        m_copydata(m, 0, take, bytes);
        note(1, nif_index(nif));
        mix(dst, ETH_ALEN);
        mix32(type);
        mix32(n);
        mix(bytes, take);
    }
    m_freem(m);
    return 0;
}

static timer_fn g_timer_fn;
static net_work_fn g_work_fn;
static struct net_work *g_work;

void timer_setup(struct timer *t, timer_fn fn, void *arg) { (void)t; (void)arg; g_timer_fn = fn; note(2, 0); }
void timer_start(struct timer *t, uint64_t delay_ns) { (void)t; note(3, delay_ns); }
void net_work_init(struct net_work *w, net_work_fn fn, void *arg) { (void)arg; g_work = w; g_work_fn = fn; note(4, 0); }
bool net_work_queue(struct net_work *w) { note(5, w == g_work); return true; }

void nd_age(uint64_t now_ns) { note(6, now_ns); }
void nat_age(uint64_t now_ns) { note(7, now_ns); }
void fw_age(uint64_t now_ns) { note(8, now_ns); }
void tapsvc_dns_age(uint64_t now_ns) { note(9, now_ns); }

static long g_refs[NIFS];

void kobject_get(struct kobject *obj) { g_refs[nif_index(container_of(obj, struct netif, obj))]++; }
void kobject_put(struct kobject *obj) { g_refs[nif_index(container_of(obj, struct netif, obj))]--; }

/* ---- the histories ---- */

static uint32_t g_pool[POOL];   /* network order */

static uint32_t neighbour(void) { return g_pool[next() % POOL]; }

static struct netif *some_nif(void) { return &g_nifs[next() % NIFS]; }

/* A packet to park: an IPv4 payload whose bytes say which one it is. */
static struct mbuf *packet(void)
{
    struct mbuf *m = m_get();
    EXPECT(m != NULL);
    m->flags |= M_PKTHDR;
    uint8_t body[20];
    for (unsigned i = 0; i < sizeof(body); i++)
        body[i] = (uint8_t)next();
    m_append(m, body, sizeof(body));
    return m;
}

static void op_resolve(void)
{
    struct netif *nif = some_nif();
    uint64_t kind = next() % 16;
    uint32_t ip = neighbour();
    if (kind == 0)
        ip = 0xffffffffu;                                  /* the limited broadcast */
    else if (kind == 1)
        ip = nif->ip4.addr | ~nif->ip4.mask;               /* the subnet's broadcast, when there is a mask */
    struct mbuf *m = next() % 3 ? packet() : NULL;
    uint8_t mac[ETH_ALEN];
    memset(mac, 0xa5, sizeof(mac));
    if (next() % 41 == 0)
        g_fail_in = 0;   /* the request's cluster */
    int rc = arp_resolve(nif, ip, mac, m);
    g_fail_in = -1;
    note(10, (uint64_t)(int64_t)rc);
    if (rc == 0) {
        mix(mac, ETH_ALEN);
        m_freem(m);   /* resolved at once: the packet stays the caller's, to send */
    }
}

static void op_lookup(void)
{
    uint8_t mac[ETH_ALEN];
    memset(mac, 0x5a, sizeof(mac));
    struct netif *nif = some_nif();   /* drawn one after the other: argument order is the compiler's */
    uint32_t ip = neighbour();
    bool ok = arp_lookup(nif, ip, mac);
    note(11, ok);
    mix(mac, ETH_ALEN);
}

/* An ARP frame in: what a neighbour sends, or something like it. */
static void op_input(unsigned r)
{
    struct netif *nif = some_nif();
    uint8_t a[28];
    for (unsigned i = 0; i < sizeof(a); i++)
        a[i] = (uint8_t)next();
    a[0] = 0; a[1] = 1;                 /* Ethernet */
    a[2] = 0x08; a[3] = 0x00;           /* IPv4 */
    a[4] = 6; a[5] = 4;
    uint64_t opk = next() % 8;
    a[6] = 0;
    a[7] = (uint8_t)(opk < 4 ? 1 : opk < 7 ? 2 : 3);   /* request, reply, or another op */
    bool unconfigured = next() % 32 == 0;   /* the interface has no address yet: nothing is for us */
    uint32_t spa = neighbour(), tpa = unconfigured ? 0 : next() % 3 ? nif->ip4.addr : neighbour();
    uint64_t sk = next() % 16;
    if (sk == 0)
        spa = 0;                        /* a probe */
    else if (sk == 1)
        spa = nif->ip4.addr;            /* our own address */
    memcpy(a + 14, &spa, 4);
    memcpy(a + 24, &tpa, 4);
    if (next() % 32 == 0)
        a[4] = 8;                       /* malformed: no Ethernet hardware address */
    uint32_t len = next() % 32 == 0 ? (uint32_t)(next() % 28) : 28;
    struct mbuf *m = m_get();
    EXPECT(m != NULL);
    m->flags |= M_PKTHDR;
    uint32_t split = len > 1 && next() % 4 == 0 ? (uint32_t)(1 + next() % (len - 1)) : len;
    m_append(m, a, split);
    if (split < len) {
        struct mbuf *tail = m_get();
        EXPECT(tail != NULL);
        memcpy(tail->data, a + split, len - split);
        tail->len = len - split;
        m->next = tail;
        m->pkt.len = len;
    }
    if (r % 89 == 0)
        g_fail_in = (int)(next() % 2);  /* m_pullup's allocation, or the reply's */
    uint32_t addr = nif->ip4.addr;
    if (unconfigured)
        nif->ip4.addr = 0;
    arp_input(nif, m);
    nif->ip4.addr = addr;
    g_fail_in = -1;
}

static void op_delete(void)
{
    struct netif *nif = some_nif();   /* drawn one after the other: argument order is the compiler's */
    arp_delete(nif, neighbour());
}

static void op_list(void)
{
    struct arp_info out[ARP_TABLE_SIZE];
    unsigned max = (unsigned)(next() % (ARP_TABLE_SIZE + 1));
    memset(out, 0x77, sizeof(out));
    unsigned n = arp_list(out, max);
    note(12, n);
    for (unsigned i = 0; i < n && i < max; i++) {
        mix32(out[i].ip);
        mix(out[i].mac, ETH_ALEN);
        mix32(out[i].complete);
        mix(out[i].ifname, sizeof(out[i].ifname));
    }
}

static void op_stats(void)
{
    struct arp_stats s;
    arp_get_stats(&s);
    mix(&s, sizeof(s));
}

static void op_updown(void)
{
    struct netif *nif = some_nif();
    if (nif->flags & NETIF_UP) {
        nif->flags &= ~NETIF_UP;
        arp_flush(nif);                 /* netif_set_up(false)'s order: the flag, then the flush */
    } else {
        nif->flags |= NETIF_UP;
    }
    note(13, nif->flags);
}

/* Many resolutions at once, before any ageing: the table fills and evicts (the least recently updated reachable
 * entry, else the oldest incomplete one), and a request for us finds no room. */
static void op_burst(unsigned r)
{
    struct netif *nif = some_nif();
    unsigned n = 40u + (unsigned)(next() % 40u);
    for (unsigned i = 0; i < n; i++) {
        uint8_t mac[ETH_ALEN];
        g_now += 1000;
        int rc = arp_resolve(&g_nifs[(i + r) % NIFS], g_pool[(i * 7u + r) % POOL], mac, NULL);
        note(14, (uint64_t)(int64_t)rc);
    }
    note(15, nif_index(nif));
}

/* The debug build's hooks (CONFIG_DEBUG), single-threaded: each is released before the call that parks on it, so the
 * call passes through the park at once. Armed for another interface, a caller does not park. */
static void op_hooks(unsigned r)
{
    struct netif *nif = some_nif();
    struct netif *other = &g_nifs[(nif_index(nif) + 1u) % NIFS];
    uint8_t mac[ETH_ALEN];
    arp_test_hold_lock_entry(next() % 2 ? nif : other);
    note(16, arp_test_lock_entry_parked());
    arp_test_release_lock_entry();
    int rc = arp_resolve(nif, neighbour(), mac, NULL);
    note(17, (uint64_t)(int64_t)rc);
    note(18, arp_test_lock_entry_parked());
    arp_test_hold_lock_entry(NULL);
    arp_test_hold_retry(r % 2 == 0);
    note(19, arp_test_retry_parked());
    arp_test_release_retry();
    g_now += 1100000000ull;
    arp_age(g_now);
    note(20, arp_test_retry_parked());
    arp_test_hold_retry(false);
}

static void op_age(unsigned r)
{
    uint64_t step = next() % 8;
    if (step < 5)
        g_now += next() % 1500000000ull;                      /* within the retry interval, or past it */
    else if (step < 7)
        g_now += 20ull * 60 * 1000000000ull + next() % 1000;  /* past a reachable entry's life */
    if (r % 7 == 0)
        g_fail_in = (int)(next() % 3);  /* a retry's request */
    if (next() % 4 == 0) {
        g_timer_fn(NULL, NULL);         /* the timer queues the work item */
        g_work_fn(NULL);                /* the worker runs it: arp_age and the others, the timer again */
    } else {
        arp_age(g_now);
    }
    g_fail_in = -1;
}

static void setup(void)
{
    static bool done;
    if (!done) {
        mbuf_init();
        arp_init();
        done = true;
    }
    memset(g_nifs, 0, sizeof(g_nifs));
    for (unsigned n = 0; n < NIFS; n++) {
        struct netif *nif = &g_nifs[n];
        snprintf(nif->name, sizeof(nif->name), "eth%u", n);
        for (unsigned i = 0; i < ETH_ALEN; i++)
            nif->mac[i] = (uint8_t)next();
        nif->mac[0] = (uint8_t)((nif->mac[0] & 0xfe) | 0x02);
        nif->flags = NETIF_UP;
        nif->ip4.addr = htonl(0x0a000002u + (n << 8));          /* 10.0.n.2 */
        nif->ip4.mask = n == 2 ? 0 : htonl(0xffffff00u);         /* eth2 has no mask */
    }
    for (unsigned i = 0; i < POOL; i++)
        g_pool[i] = htonl(0x0a000000u + ((i % NIFS) << 8) + 3 + i / NIFS);
}

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    setup();
    for (unsigned r = 0; r < ROUNDS; r++) {
        uint64_t k = next() % 32;
        if (k < 10)
            op_resolve();
        else if (k < 13)
            op_lookup();
        else if (k < 23)
            op_input(r);
        else if (k < 24)
            op_list();
        else if (k < 25)
            op_stats();
        else if (k < 26)
            op_updown();
        else if (k < 27)
            op_delete();
        else if (k < 28)
            arp_flush(some_nif());
        else if (k < 29)
            op_burst(r);
        else if (k < 30 && r % 5 == 0)
            op_hooks(r);
        else
            op_age(r);
    }
    op_list();
    op_stats();
    for (unsigned n = 0; n < NIFS; n++) {
        arp_flush(&g_nifs[n]);
        mix64((uint64_t)g_refs[n]);
    }
    op_stats();
    struct arp_stats s;
    arp_get_stats(&s);
    printf("  arp digest 0x%016llx over %u rounds: %llu requests sent, %llu replies sent, %llu timeouts, "
           "%llu dropped while pending, %llu unsolicited\n",
           (unsigned long long)g_digest, ROUNDS, (unsigned long long)s.requests_sent,
           (unsigned long long)s.replies_sent, (unsigned long long)s.timeouts,
           (unsigned long long)s.pending_dropped, (unsigned long long)s.unsolicited);
    EXPECT(g_digest == ARP_DIGEST);
    EXPECT(s.entries == 0);
    EXPECT(g_live == 0);
    for (unsigned n = 0; n < NIFS; n++)
        EXPECT(g_refs[n] == 0);
}

/* ARP_BENCH=1: the median of nine runs, 200,000 calls each. */
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

static void arp_bench(void)
{
    g_rng = SEED;
    setup();
    g_benching = true;
    struct netif *nif = &g_nifs[0];
    /* A reachable neighbour: asked, then answered. */
    uint32_t ip = g_pool[0];
    uint8_t mac[ETH_ALEN];
    (void)arp_resolve(nif, ip, mac, NULL);
    uint8_t reply[28] = { 0, 1, 0x08, 0x00, 6, 4, 0, 2, 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
    memcpy(reply + 14, &ip, 4);
    memcpy(reply + 24, &nif->ip4.addr, 4);
    struct mbuf *m = m_getcl();
    m_append(m, reply, sizeof(reply));
    arp_input(nif, m);
    EXPECT(arp_resolve(nif, ip, mac, NULL) == 0);
    /* A request for us from that neighbour. */
    uint8_t request[28];
    memcpy(request, reply, sizeof(request));
    request[7] = 1;
    double hit[9], ask[9];
    for (unsigned t = 0; t < 9; t++) {
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 200000; i++)
            (void)arp_resolve(nif, ip, mac, NULL);
        hit[t] = ns_since(&t0) / 200000;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 200000; i++) {
            m = m_getcl();
            m_append(m, request, sizeof(request));
            arp_input(nif, m);
        }
        ask[t] = ns_since(&t0) / 200000;
    }
    qsort(hit, 9, sizeof(double), cmp_double);
    qsort(ask, 9, sizeof(double), cmp_double);
    for (unsigned n = 0; n < NIFS; n++)
        arp_flush(&g_nifs[n]);
    g_benching = false;
    printf("  bench arp_resolve (reachable) %.1f ns, arp_input (a request for us, answered) %.1f ns\n", hit[4], ask[4]);
}

static const struct host_test tests[] = {
    { "arp digest", test_digest },
};

int main(void)
{
    int rc = harness_run(tests, sizeof(tests) / sizeof(tests[0]));
    if (getenv("ARP_BENCH") != NULL)
        arp_bench();   /* after the digest, whose statistics it would change */
    return rc;
}
