/*
 * test_cksum.c - The Internet checksum (kernel-services/network/cksum.c,
 * kernel/include/kernel/net/cksum.h) on the host: roadmap 2.0 SA6's first
 * port. A digest over seeded inputs, recorded from the C: cksum_partial and
 * in_cksum over buffers of every length, alignment and starting sum;
 * m_cksum_partial over mbuf chains split at odd and even places, from every
 * offset; the IPv4 and IPv6 pseudo-headers; and m_csum_complete, with the
 * bytes it writes and the offsets it refuses. CKSUM_BENCH=1 times the hot
 * paths: a 1500-byte buffer, a 64 KiB one, and a 64 KiB chain.
 */

#include "harness.h"

#include <kernel/mbuf.h>
#include <kernel/net/cksum.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The digest of the C (cksum.c, 2026-11-01). */
#define CKSUM_DIGEST 0xee64aae0b44b750full
#define ROUNDS 20000u
#define SEED 0xC05F5FA2026B0008ull

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

static uint8_t g_buf[70000];

/* A chain of `n` mbufs over consecutive pieces of g_buf, the last piece's
 * end at `total`; pieces of length 0 too. */
static unsigned make_chain(struct mbuf *ms, unsigned n, size_t total)
{
    size_t at = 0;
    for (unsigned k = 0; k < n; k++) {
        size_t left = total - at;
        size_t len = k + 1 == n ? left : (size_t)(next() % (left + 1));
        memset(&ms[k], 0, sizeof(ms[k]));
        ms[k].data = g_buf + at;
        ms[k].len = (uint32_t)len;
        ms[k].next = k + 1 < n ? &ms[k + 1] : NULL;
        at += len;
    }
    return n;
}

static void test_digest(void)
{
    static struct mbuf ms[8];
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    for (unsigned r = 0; r < ROUNDS; r++) {
        /* A buffer: its length (often odd, now and then large), where it starts, the sum it continues. */
        size_t len = next() % 16 == 0 ? (size_t)(next() % 66000) : (size_t)(next() % 1600);
        size_t start = (size_t)(next() % 8);
        for (size_t i = 0; i < len; i++)
            g_buf[start + i] = (uint8_t)next();
        uint32_t sum = next() % 4 == 0 ? (uint32_t)next() : 0u;
        mix32(cksum_partial(g_buf + start, len, sum));
        uint16_t c = in_cksum(g_buf + start, len);
        mix(&c, sizeof(c));
        uint32_t big = (uint32_t)next();
        c = cksum_fold(big);
        mix(&c, sizeof(c));

        /* A chain over it, and a window into the chain from any offset. */
        unsigned n = 1 + (unsigned)(next() % 8);
        size_t total = len < sizeof(g_buf) ? len : sizeof(g_buf);
        make_chain(ms, n, total);
        uint32_t off = (uint32_t)(next() % (total + 2));
        uint32_t want = (uint32_t)(next() % (total + 3));
        mix32(m_cksum_partial(ms, off, want, sum));

        /* The pseudo-headers. */
        uint32_t src = (uint32_t)next(), dst = (uint32_t)next();
        uint8_t proto = (uint8_t)next();
        uint16_t plen = (uint16_t)next();
        mix32(cksum_pseudo4(src, dst, proto, plen));
        struct in6_addr a, b;
        for (unsigned i = 0; i < 16; i++) {
            a.s6_addr[i] = (uint8_t)next();
            b.s6_addr[i] = (uint8_t)next();
        }
        uint32_t len6 = (uint32_t)next();
        mix32(cksum_pseudo6(&a, &b, proto, len6));

        /* m_csum_complete on the chain as a packet: the field it fills, and offsets it refuses. */
        make_chain(ms, n, total);
        ms[0].pkt.len = (uint32_t)(next() % 4 == 0 ? next() % (total + 4) : total);
        ms[0].pkt.csum_start = (uint16_t)(next() % (ms[0].len + 3));
        ms[0].pkt.csum_offset = (uint16_t)(next() % 24);
        ms[0].pkt.csum_flags = (uint16_t)(next() % 4);
        if (r % 64 == 0) {   /* a UDP checksum that comes to zero, which is sent as 0xffff */
            memset(g_buf, 0, 16);
            g_buf[0] = 0xff;
            g_buf[1] = 0xff;
            total = 16;
            make_chain(ms, 1, total);
            ms[0].pkt.len = 16;
            ms[0].pkt.csum_start = 0;
            ms[0].pkt.csum_offset = 2;
            ms[0].pkt.csum_flags = NET_CSUM_UDP;
        }
        bool done = m_csum_complete(&ms[0]);
        mix(&done, sizeof(done));
        mix(&ms[0].pkt.csum_flags, sizeof(ms[0].pkt.csum_flags));
        mix(g_buf, total < 64 ? total : 64);   /* the field it wrote lies in the first buffer */
    }
    printf("  cksum digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, ROUNDS);
    EXPECT(g_digest == CKSUM_DIGEST);
}

/* CKSUM_BENCH=1: best of 15 runs of each path. */
static double ns_since(const struct timespec *t0)
{
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (double)(t1.tv_sec - t0->tv_sec) * 1e9 + (double)(t1.tv_nsec - t0->tv_nsec);
}

static void cksum_bench(void)
{
    static struct mbuf ms[45];
    g_rng = SEED;
    for (size_t i = 0; i < sizeof(g_buf); i++)
        g_buf[i] = (uint8_t)next();
    for (unsigned k = 0; k < 45; k++) {   /* 64 KiB as 44 buffers of 1,490 and a last of 0 */
        memset(&ms[k], 0, sizeof(ms[k]));
        ms[k].data = g_buf + (size_t)k * 1490u;
        ms[k].len = k < 44 ? 1490u : 0u;
        ms[k].next = k < 44 ? &ms[k + 1] : NULL;
    }
    double best[3] = {1e18, 1e18, 1e18};
    uint64_t sink = 0;
    for (unsigned t = 0; t < 15; t++) {
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 20000; i++)
            sink += in_cksum(g_buf + (i & 7), 1500);
        double a = ns_since(&t0) / 20000;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 500; i++)
            sink += in_cksum(g_buf + (i & 7), 65536);
        double b = ns_since(&t0) / 500;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 500; i++)
            sink += m_cksum_partial(ms, i & 7, 65000, 0);
        double c = ns_since(&t0) / 500;
        if (a < best[0]) best[0] = a;
        if (b < best[1]) best[1] = b;
        if (c < best[2]) best[2] = c;
    }
    printf("  bench in_cksum 1500 B %.1f ns, 64 KiB %.1f ns, m_cksum_partial 64 KiB chain %.1f ns (sink %llu)\n",
           best[0], best[1], best[2], (unsigned long long)sink);
}

static const struct host_test tests[] = {
    { "cksum digest", test_digest },
};

int main(void)
{
    if (getenv("CKSUM_BENCH") != NULL)
        cksum_bench();
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
