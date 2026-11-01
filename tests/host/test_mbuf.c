/*
 * test_mbuf.c - Packet buffers (kernel-services/network/mbuf.c,
 * kernel/include/kernel/mbuf.h) on the host: roadmap 2.0 SA6. A digest over
 * seeded histories of packets, recorded from the C. Packets are made from
 * plain mbufs or clusters and grown past one cluster; each step prepends,
 * pulls up, trims from the front or the back, copies data out, appends,
 * takes a shared reference to a cluster, copies the packet, or frees part
 * of a chain; queues take, refuse (full, or the mbuf already queued) and
 * give packets back. Allocations fail now and then. After every step the
 * digest takes each buffer of the packet: its length, flags, room before and
 * after, size and bytes, and the packet header's length; and, at the end,
 * the statistics, which must show every mbuf and cluster freed.
 * MBUF_BENCH=1 times the hot operations.
 */

#include "harness.h"

#include <kernel/kmalloc.h>
#include <kernel/mbuf.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The digest of the C (mbuf.c, 2026-11-02), which the CeeSafe port gives. */
#define MBUF_DIGEST 0xfd419bfa31e68b04ull
#define ROUNDS 4000u
#define STEPS 24u
#define SEED 0xC05F5FA2026B000Cull

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

/* Every buffer of a packet: what the C reads of it, and its bytes. */
static void mix_packet(const struct mbuf *m)
{
    mix32(m == NULL);
    for (const struct mbuf *b = m; b != NULL; b = b->next) {
        mix32(b->len);
        mix32(b->flags);
        mix32(b->size);
        mix32(m_leadingspace(b));
        mix32(m_trailingspace(b));
        mix(b->data, b->len);
        mix32(b->refcount);
    }
    if (m != NULL) {
        mix32(m->flags & M_PKTHDR ? m->pkt.len : 0u);
        mix32(m_length(m));
        mix(&m->pkt.proto, sizeof(m->pkt.proto));
        mix(&m->pkt.csum_flags, sizeof(m->pkt.csum_flags));
    }
}

static uint8_t g_bytes[6000];

static void fill(uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        g_bytes[i] = (uint8_t)next();
}

/* A packet: a plain mbuf or a cluster, perhaps with a packet header, with some bytes. */
static struct mbuf *make(void)
{
    struct mbuf *m = next() % 2 ? m_getcl() : m_get();
    if (m == NULL)
        return NULL;
    if (next() % 4 != 0)
        m->flags |= M_PKTHDR;
    m->pkt.proto = (uint16_t)next();
    m->pkt.csum_flags = (uint16_t)(next() % 4);
    m->flags |= (uint32_t)next() & (M_BCAST | M_MCAST | M_CSUM_OK);
    uint32_t n = (uint32_t)(next() % 8 == 0 ? next() % 5000 : next() % 300);
    fill(n);
    int rc = m_append(m, g_bytes, n);
    mix32((uint32_t)rc);
    return m;
}

static void step(struct mbuf **slot)
{
    struct mbuf *m = *slot;
    unsigned op = (unsigned)(next() % 14);
    if (next() % 23 == 0)
        g_fail_in = (int)(next() % 2);
    mix32(op);
    if (m == NULL && op != 0) {
        op = 0;
    }
    switch (op) {
    case 0:
        if (m != NULL)
            m_freem(m);
        m = make();
        break;
    case 1: {
        uint32_t n = (uint32_t)(next() % 6 == 0 ? next() % 300 : next() % 64);
        m = m_prepend(m, n);
        if (m != NULL && m->len >= n) {
            fill(n);
            memcpy(m->data, g_bytes, n);
        }
        break;
    }
    case 2: {
        uint32_t n = (uint32_t)(next() % 8 == 0 ? next() % 2200 : next() % 80);
        m = m_pullup(m, n);
        break;
    }
    case 3: {
        int n = (int)(next() % 200);
        if (next() % 2)
            n = -n;
        if (next() % 16 == 0)
            n = n * 40;
        m_adj(m, n);
        break;
    }
    case 4: {
        uint8_t out[400];
        uint32_t off = (uint32_t)(next() % 600);
        uint32_t len = (uint32_t)(next() % 400);
        memset(out, 0xEE, sizeof(out));
        bool ok = m_copydata(m, off, len, out);
        mix(&ok, sizeof(ok));
        mix(out, ok ? len : 0);
        break;
    }
    case 5: {
        uint32_t n = (uint32_t)(next() % 8 == 0 ? next() % 3000 : next() % 200);
        fill(n);
        int rc = m_append(m, g_bytes, n);
        mix32((uint32_t)rc);
        break;
    }
    case 6: {
        struct mbuf *r = m_ref(m);
        mix_packet(r);
        if (r != NULL)
            m_free(r);
        break;
    }
    case 7: {
        struct mbuf *c = m_copypacket(m);
        mix_packet(c);
        m_freem(c);
        break;
    }
    case 8:
        m = m_free(m);   /* the first buffer: the rest, or NULL */
        break;
    case 9:
        mix32(m_length(m));
        break;
    case 10: {
        /* A reference to the cluster kept alive while the packet goes: the cluster outlives its first owner. */
        struct mbuf *r = m_ref(m);
        m_freem(m);
        m = r;
        break;
    }
    case 11: {
        uint32_t n = (uint32_t)(next() % 64);
        m = m_pullup(m, n);
        if (m != NULL)
            m_adj(m, (int)(n / 2));
        break;
    }
    default: {
        uint32_t n = (uint32_t)(next() % (MCLBYTES + 1u));
        m = m_prepend(m, n);   /* larger than an mbuf's room, up to a cluster (no caller prepends more) */
        break;
    }
    }
    g_fail_in = -1;
    *slot = m;
    mix_packet(m);
}

static void mix_stats(void)
{
    struct mbuf_stats s;
    mbuf_get_stats(&s);
    mix(&s, sizeof(s));
}

static void test_queues(void)
{
    struct mbufq q;
    mbufq_init(&q, 5, "test-q");
    struct mbuf *held[8] = { 0 };
    for (unsigned r = 0; r < 400; r++) {
        unsigned op = (unsigned)(next() % 5);
        unsigned k = (unsigned)(next() % 8);
        mix32(op);
        if (op <= 1) {
            if (held[k] == NULL)
                held[k] = make();
            if (held[k] != NULL) {
                bool queued = held[k]->flags & M_QUEUED;
                bool ok = mbufq_enqueue(&q, held[k]);
                mix(&ok, sizeof(ok));
                if (!queued)
                    held[k] = NULL;   /* the queue has it, or freed it when full */
            }
        } else if (op == 2) {
            struct mbuf *m = mbufq_dequeue(&q);
            mix_packet(m);
            m_freem(m);
        } else if (op == 3) {
            mix32(mbufq_len(&q));
        } else {
            /* the same mbuf twice: the second enqueue is refused while it is queued */
            struct mbuf *m = make();
            if (m != NULL) {
                bool first = mbufq_enqueue(&q, m);
                mix(&first, sizeof(first));
                if (first) {
                    bool second = mbufq_enqueue(&q, m);
                    mix(&second, sizeof(second));
                }
            }
        }
        mix32(mbufq_len(&q));
    }
    mbufq_drain(&q);
    mix32(mbufq_len(&q));
    for (unsigned k = 0; k < 8; k++)
        m_freem(held[k]);
}

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    mbuf_init();
    struct mbuf *slots[4] = { 0 };
    for (unsigned r = 0; r < ROUNDS; r++) {
        for (unsigned s = 0; s < STEPS; s++)
            step(&slots[next() % 4]);
        if (r % 500 == 0)
            mix_stats();
    }
    for (unsigned k = 0; k < 4; k++)
        m_freem(slots[k]);
    test_queues();
    mix_stats();
    struct mbuf_stats s;
    mbuf_get_stats(&s);
    printf("  mbuf digest 0x%016llx over %u rounds: %llu allocations, %llu failed, %llu double enqueues\n",
           (unsigned long long)g_digest, ROUNDS, (unsigned long long)s.allocs, (unsigned long long)s.alloc_failures,
           (unsigned long long)s.double_enqueues);
    EXPECT(g_digest == MBUF_DIGEST);
    EXPECT(s.mbufs_alive == 0 && s.clusters_alive == 0);
    EXPECT(g_live == 0);
}

/* MBUF_BENCH=1: best of 15 runs of each path. */
static double ns_since(const struct timespec *t0)
{
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (double)(t1.tv_sec - t0->tv_sec) * 1e9 + (double)(t1.tv_nsec - t0->tv_nsec);
}

static void mbuf_bench(void)
{
    mbuf_init();
    static uint8_t payload[1500];
    double best[4] = { 1e18, 1e18, 1e18, 1e18 };
    uint64_t sink = 0;
    struct mbufq q;
    mbufq_init(&q, 1024, "bench-q");
    for (unsigned t = 0; t < 15; t++) {
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 20000; i++) {   /* a received frame: cluster, fill, header off, free */
            struct mbuf *m = m_getcl();
            m_append(m, payload, 1500);
            m = m_pullup(m, 54);
            m_adj(m, 14);
            sink += m_length(m);
            m_freem(m);
        }
        double a = ns_since(&t0) / 20000;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 20000; i++) {   /* a sent segment: headers prepended three times */
            struct mbuf *m = m_getcl();
            m_append(m, payload, 1000);
            m = m_prepend(m, 20);
            m = m_prepend(m, 20);
            m = m_prepend(m, 14);
            sink += m->pkt.len;
            m_freem(m);
        }
        double b = ns_since(&t0) / 20000;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 20000; i++) {   /* a chain read out */
            static struct mbuf *chain;
            if (chain == NULL) {
                chain = m_getcl();
                m_append(chain, payload, 1500);
                m_append(chain, payload, 1500);
                m_append(chain, payload, 1500);
            }
            uint8_t out[4096];
            sink += m_copydata(chain, (uint32_t)(i & 255), 4000, out);
        }
        double c = ns_since(&t0) / 20000;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 20000; i++) {   /* a queue: in and out */
            struct mbuf *m = m_get();
            mbufq_enqueue(&q, m);
            m = mbufq_dequeue(&q);
            m_freem(m);
        }
        double d = ns_since(&t0) / 20000;
        if (a < best[0]) best[0] = a;
        if (b < best[1]) best[1] = b;
        if (c < best[2]) best[2] = c;
        if (d < best[3]) best[3] = d;
    }
    printf("  bench receive %.1f ns, send %.1f ns, copydata %.1f ns, queue %.1f ns (sink %llu)\n", best[0], best[1],
           best[2], best[3], (unsigned long long)sink);
}

static const struct host_test tests[] = {
    { "mbuf digest", test_digest },
};

int main(void)
{
    if (getenv("MBUF_BENCH") != NULL) {
        mbuf_bench();
        return 0;
    }
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
