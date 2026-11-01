/*
 * test_ether.c - Ethernet framing and dispatch (kernel-services/network/
 * ether.c, kernel/include/kernel/net/ether.h) on the host: roadmap 2.0 SA6.
 * A digest over seeded frames, recorded from the C. ether_input gets frames
 * of every length (shorter than a header too), whole or split across two
 * mbufs, to this interface's MAC, broadcast, multicast or another host, now
 * and then sent by this interface (reflected), with each EtherType; the
 * protocol inputs it calls are stubs that fold in what they receive (the
 * bytes after the header, the flags, the protocol) and free it. ether_output
 * gets payloads in mbufs with and without headroom, with transmit checksum
 * offload or not, short enough to be padded, and allocation failures; the
 * netif_transmit stub folds in the whole frame. Every mbuf is freed at the
 * end (mbuf.c's statistics), and the interface's error counter is folded in.
 * ETHER_BENCH=1 times ether_input to the IPv4 stub and ether_output.
 */

#include "harness.h"

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/mbuf.h>
#include <kernel/net/ether.h>
#include <kernel/net/ip.h>
#include <kernel/netif.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The digest of the C (ether.c, 2026-11-01), which the CeeSafe port gives. */
#define ETHER_DIGEST 0xd21f822ad04f292aull
#define ROUNDS 20000u
#define SEED 0xC05F5FA2026B000Aull

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

/* The protocol inputs and the transmit: fold in what arrives, then free it. */
static bool g_benching;
static unsigned g_delivered;

static void arrived(unsigned which, struct mbuf *m)
{
    g_delivered++;
    if (!g_benching) {
        uint8_t bytes[96];
        uint32_t n = m_length(m);
        uint32_t take = n < sizeof(bytes) ? n : (uint32_t)sizeof(bytes);
        m_copydata(m, 0, take, bytes);
        mix32(which);
        mix32(n);
        mix32(m->pkt.len);
        mix32(m->flags & (M_BCAST | M_MCAST));
        mix(&m->pkt.proto, sizeof(m->pkt.proto));
        mix(bytes, take);
    }
    m_freem(m);
}

void ipv4_input(struct netif *nif, struct mbuf *m) { (void)nif; arrived(1, m); }
void arp_input(struct netif *nif, struct mbuf *m) { (void)nif; arrived(2, m); }
void ipv6_input(struct netif *nif, struct mbuf *m) { (void)nif; arrived(3, m); }

static int g_tx_rc;
static unsigned g_nomem;

int netif_transmit(struct netif *nif, struct mbuf *m)
{
    (void)nif;
    if (!g_benching) {
        uint8_t frame[160];
        uint32_t n = m_length(m);
        uint32_t take = n < sizeof(frame) ? n : (uint32_t)sizeof(frame);
        m_copydata(m, 0, take, frame);
        mix32(n);
        mix32(m->pkt.len);
        mix(&m->pkt.csum_flags, sizeof(m->pkt.csum_flags));
        mix(&m->pkt.csum_start, sizeof(m->pkt.csum_start));
        mix(frame, take);
    }
    m_freem(m);
    return g_tx_rc;
}

static struct netif g_nif;

static const uint8_t g_types[][2] = { { 0x08, 0x00 }, { 0x08, 0x06 }, { 0x86, 0xdd }, { 0x88, 0xcc }, { 0x00, 0x01 } };

/* A destination: this interface, broadcast, a multicast group, or another host. */
static void destination(uint8_t dst[ETH_ALEN])
{
    uint64_t kind = next() % 8;
    for (unsigned i = 0; i < ETH_ALEN; i++)
        dst[i] = (uint8_t)next();
    if (kind < 3)
        memcpy(dst, g_nif.mac, ETH_ALEN);
    else if (kind == 3)
        memset(dst, 0xff, ETH_ALEN);
    else if (kind == 4)
        dst[0] |= 1;    /* multicast */
    else
        dst[0] &= 0xfe; /* unicast, to whom it may be */
}

static void test_input(unsigned r)
{
    uint8_t frame[96];
    size_t len = (size_t)(next() % 8 == 0 ? next() % 15 : 14 + next() % 82);
    for (size_t i = 0; i < len; i++)
        frame[i] = (uint8_t)next();
    if (len >= 14) {
        destination(frame);
        if (next() % 8 == 0)
            memcpy(frame + 6, g_nif.mac, ETH_ALEN);   /* our own frame, reflected */
        unsigned t = (unsigned)(next() % 5);
        frame[12] = g_types[t][0];
        frame[13] = g_types[t][1];
    }
    struct mbuf *m = m_get();
    EXPECT(m != NULL);
    m->flags |= M_PKTHDR;
    size_t split = len;
    if (len > 1 && next() % 3 == 0)
        split = (size_t)(1 + next() % (len - 1));   /* the header across two mbufs: m_pullup's work */
    m_append(m, frame, (uint32_t)split);
    if (split < len) {
        struct mbuf *tail = m_get();
        EXPECT(tail != NULL);
        memcpy(tail->data, frame + split, len - split);
        tail->len = (uint32_t)(len - split);
        m->next = tail;
        m->pkt.len = (uint32_t)len;
    }
    if (r % 97 == 0)
        g_fail_in = 0;   /* m_pullup's allocation fails, when it needs one */
    ether_input(&g_nif, m);
    g_fail_in = -1;
}

static void test_output(unsigned r)
{
    uint8_t payload[80];
    uint32_t len = (uint32_t)(next() % 81);
    for (uint32_t i = 0; i < len; i++)
        payload[i] = (uint8_t)next();
    struct mbuf *m = next() % 2 ? m_getcl() : m_get();   /* with headroom, or without: m_prepend allocates */
    EXPECT(m != NULL);
    m->flags |= M_PKTHDR;
    m_append(m, payload, len);
    m->pkt.csum_flags = (uint16_t)(next() % 4);
    m->pkt.csum_start = (uint16_t)(next() % 64);
    m->pkt.csum_offset = (uint16_t)(next() % 32);
    uint8_t dst[ETH_ALEN];
    destination(dst);
    unsigned t = (unsigned)(next() % 5);
    uint16_t type = (uint16_t)((g_types[t][0] << 8) | g_types[t][1]);
    g_tx_rc = (int)(next() % 3) - 1;
    if (r % 89 == 0)
        g_fail_in = (int)(next() % 2);   /* the prepend's or the padding's allocation fails */
    int rc = ether_output(&g_nif, m, next() % 16 == 0 ? eth_broadcast : dst, type);
    g_fail_in = -1;
    if (rc == -ENOMEM)
        g_nomem++;
    mix32((uint32_t)rc);
}

static void setup(void)
{
    static bool done;
    if (!done) {
        mbuf_init();
        done = true;
    }
    memset(&g_nif, 0, sizeof(g_nif));
    memcpy(g_nif.name, "eth0", 5);
    for (unsigned i = 0; i < ETH_ALEN; i++)
        g_nif.mac[i] = (uint8_t)next();
    g_nif.mac[0] = (uint8_t)((g_nif.mac[0] & 0xfe) | 0x02);   /* a locally administered unicast address */
}

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    setup();
    for (unsigned r = 0; r < ROUNDS; r++) {
        test_input(r);
        test_output(r);
    }
    mix(&g_nif.stats, sizeof(g_nif.stats));
    mix(eth_broadcast, ETH_ALEN);
    printf("  ether digest 0x%016llx over %u rounds: %u frames delivered, %llu receive errors, %u ENOMEM\n",
           (unsigned long long)g_digest, ROUNDS, g_delivered, (unsigned long long)g_nif.stats.rx_errors, g_nomem);
    EXPECT(g_digest == ETHER_DIGEST);
    EXPECT(g_live == 0);
}

/* ETHER_BENCH=1: best of 15 runs, 20,000 packets each way per run. */
static double ns_since(const struct timespec *t0)
{
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (double)(t1.tv_sec - t0->tv_sec) * 1e9 + (double)(t1.tv_nsec - t0->tv_nsec);
}

static void ether_bench(void)
{
    g_rng = SEED;
    setup();
    g_benching = true;
    uint8_t frame[60];
    memcpy(frame, g_nif.mac, ETH_ALEN);
    for (unsigned i = 6; i < sizeof(frame); i++)
        frame[i] = (uint8_t)next();
    frame[12] = 0x08;
    frame[13] = 0x00;
    uint8_t payload[46] = { 0 };
    double best[2] = { 1e18, 1e18 };
    for (unsigned t = 0; t < 15; t++) {
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 20000; i++) {
            struct mbuf *m = m_getcl();
            m_append(m, frame, sizeof(frame));
            ether_input(&g_nif, m);
        }
        double a = ns_since(&t0) / 20000;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 20000; i++) {
            struct mbuf *m = m_getcl();
            m_append(m, payload, sizeof(payload));
            g_tx_rc = 0;
            (void)ether_output(&g_nif, m, eth_broadcast, ETH_P_IP);
        }
        double b = ns_since(&t0) / 20000;
        if (a < best[0]) best[0] = a;
        if (b < best[1]) best[1] = b;
    }
    g_benching = false;
    printf("  bench ether_input %.1f ns, ether_output %.1f ns (with an mbuf's allocation and free)\n", best[0], best[1]);
}

static const struct host_test tests[] = {
    { "ether digest", test_digest },
};

int main(void)
{
    if (getenv("ETHER_BENCH") != NULL)
        ether_bench();
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
