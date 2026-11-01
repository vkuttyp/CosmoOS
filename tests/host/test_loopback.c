/*
 * test_loopback.c - The loopback interface (kernel-services/network/
 * loopback.c, kernel/include/kernel/netif.h) on the host: roadmap 2.0 SA6.
 * A digest recorded from the C: the interface loopback_init registers
 * (every field C reads: name, MTU, flags, capabilities, addresses, ops), and
 * its transmit over seeded packets, with and without a filter that drops or
 * keeps them, folding in what netif_rx receives (the flags and checksum
 * flags transmit changes) and what is freed. mbuf.c runs on the host over a
 * malloc-backed cache; every mbuf is freed at the end.
 */

#include "harness.h"

#include <kernel/kmalloc.h>
#include <kernel/mbuf.h>
#include <kernel/net/ether.h>
#include <kernel/net/ip.h>
#include <kernel/netif.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The digest of the C (loopback.c, 2026-11-01), which the CeeSafe port gives. */
#define LOOPBACK_DIGEST 0x92930b5a63e79528ull
#define ROUNDS 20000u
#define SEED 0xC05F5FA2026B000Bull

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

/* mbuf.c's caches, over malloc. */
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

/* The interface layer: registration records the interface, receive folds in the packet and frees it. */
static struct netif *g_registered;
static unsigned g_released, g_received;

int netif_register(struct netif *nif)
{
    g_registered = nif;
    return 0;
}

void netif_release_static(struct netif *nif)
{
    (void)nif;
    g_released++;
}

void netif_rx(struct netif *nif, struct mbuf *m)
{
    g_received++;
    mix32(nif == g_registered);
    mix32(m->flags);
    mix(&m->pkt.csum_flags, sizeof(m->pkt.csum_flags));
    mix32(m->pkt.len);
    m_freem(m);
}

/* A filter: keeps a packet when its tag (its first byte) is below the threshold its argument points to. */
static unsigned g_filtered;

static bool keep_below(struct mbuf *m, void *arg)
{
    g_filtered++;
    return m->data[0] < *(const uint8_t *)arg;
}

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    mbuf_init();
    loopback_init();
    struct netif *lo = g_registered;
    EXPECT(lo != NULL);
    mix(lo->name, sizeof(lo->name));
    mix32(lo->mtu);
    mix32(lo->flags);
    mix32(lo->caps);
    mix32(lo->ip4.addr);
    mix32(lo->ip4.mask);
    mix32(lo->ip4.gateway);
    mix(&lo->ip6_ll, sizeof(lo->ip6_ll));
    mix32(lo->ops != NULL && lo->ops->transmit != NULL && lo->ops->release == netif_release_static &&
          lo->ops->tx_pending == NULL);
    mix32(lo->index);
    mix(lo->mac, sizeof(lo->mac));
    uint8_t threshold = 0;
    for (unsigned r = 0; r < ROUNDS; r++) {
        if (r % 1000 == 0) {
            if (next() % 2) {
                threshold = (uint8_t)next();
                loopback_set_filter(keep_below, &threshold);
            } else {
                loopback_set_filter(NULL, NULL);
            }
        }
        struct mbuf *m = m_get();
        EXPECT(m != NULL);
        m->flags |= M_PKTHDR | ((uint32_t)next() & (M_BCAST | M_MCAST | M_CSUM_OK));
        uint8_t bytes[16];
        for (unsigned i = 0; i < sizeof(bytes); i++)
            bytes[i] = (uint8_t)next();
        m_append(m, bytes, sizeof(bytes));
        m->pkt.csum_flags = (uint16_t)next();
        int rc = lo->ops->transmit(lo, m);
        mix32((uint32_t)rc);
    }
    loopback_set_filter(NULL, NULL);
    lo->ops->release(lo);
    mix32(g_released);
    printf("  loopback digest 0x%016llx over %u rounds: %u received, %u filtered\n", (unsigned long long)g_digest,
           ROUNDS, g_received, g_filtered);
    EXPECT(g_digest == LOOPBACK_DIGEST);
    EXPECT(g_live == 0);
}

static const struct host_test tests[] = {
    { "loopback digest", test_digest },
};

int main(void)
{
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
