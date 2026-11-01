/*
 * test_inet.c - The address helpers (kernel-services/network/inet.c,
 * kernel/include/kernel/net/inet.h) on the host: roadmap 2.0 SA6. A digest
 * over seeded addresses, recorded from the C: equality of addresses and of
 * address-and-port across families (equal and nearly equal pairs), the
 * unspecified test, the conversions from and to the user's sockaddr shape
 * (every byte of the result, unsupported families too), and netaddr_str
 * into buffers of every length, with the bytes it leaves. INET_BENCH=1
 * times netaddr_equal (the socket lookups' comparison) and the user-shape
 * conversions.
 */

#include "harness.h"

#include <kernel/errno.h>
#include <kernel/net/inet.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The digest of the C (inet.c, 2026-11-01), which the CeeSafe port gives. */
#define INET_DIGEST 0x5bff0d7e00d6e5dfull
#define ROUNDS 20000u
#define SEED 0xC05F5FA2026B0009ull

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

/* A family: mostly IPv4 and IPv6, now and then unspecified or unknown. */
static uint16_t family(void)
{
    uint64_t r = next() % 16;
    if (r < 7)
        return COSMO_AF_INET;
    if (r < 14)
        return COSMO_AF_INET6;
    if (r == 14)
        return 0;
    return (uint16_t)next();
}

/* An address of a family, its bytes often zero (unspecified) or few. */
static void address(struct netaddr *a)
{
    memset(a, 0, sizeof(*a));
    a->family = family();
    a->port = (uint16_t)(next() % 4 == 0 ? 0 : next());
    unsigned style = (unsigned)(next() % 4);
    for (unsigned i = 0; i < 16; i++) {
        uint8_t b = (uint8_t)next();
        a->v6.s6_addr[i] = style == 0 ? 0 : style == 1 ? (uint8_t)(b & 1) : b;
    }
}

/* b: a copy of a, then perhaps one field or byte changed. */
static void nearly(struct netaddr *b, const struct netaddr *a)
{
    *b = *a;
    switch (next() % 6) {
    case 0: b->port = (uint16_t)(b->port + 1); break;
    case 1: b->family = b->family == COSMO_AF_INET ? COSMO_AF_INET6 : COSMO_AF_INET; break;
    case 2: {
        unsigned at = (unsigned)(next() % 16);   /* each draw in its own statement: argument order differs */
        b->v6.s6_addr[at] ^= (uint8_t)(1u << (next() % 8));
        break;
    }
    case 3: b->v6.s6_addr[4 + next() % 12] ^= 0x80; break;   /* past an IPv4 address's bytes */
    default: break;
    }
}

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    for (unsigned r = 0; r < ROUNDS; r++) {
        struct netaddr a, b;
        address(&a);
        if (next() % 2)
            nearly(&b, &a);
        else
            address(&b);
        bool eq = netaddr_equal(&a, &b);
        bool aeq = netaddr_addr_equal(&a, &b);
        bool un = netaddr_is_unspecified(&a);
        mix(&eq, sizeof(eq));
        mix(&aeq, sizeof(aeq));
        mix(&un, sizeof(un));

        /* From the user's shape: every byte of the result, and the error. */
        struct cosmo_sockaddr in;
        uint8_t *raw = (uint8_t *)&in;
        for (size_t i = 0; i < sizeof(in); i++)
            raw[i] = (uint8_t)next();
        in.family = family();
        struct netaddr out;
        memset(&out, 0xA5, sizeof(out));
        int rc = netaddr_from_user_shape(&out, &in);
        mix32((uint32_t)rc);
        mix(&out, sizeof(out));

        /* To the user's shape. */
        struct cosmo_sockaddr back;
        memset(&back, 0x5A, sizeof(back));
        netaddr_to_user_shape(&back, &a);
        mix(&back, sizeof(back));

        /* netaddr_str into a buffer of any length: what it returns, and every byte it leaves. */
        char buf[72];
        memset(buf, 0x77, sizeof(buf));
        size_t len = (size_t)(next() % 64);
        if (next() % 4 == 0)
            len = sizeof(buf);
        const char *s = netaddr_str(&a, buf, len);
        bool same = s == buf;
        mix(&same, sizeof(same));
        mix(buf, sizeof(buf));
    }
    printf("  inet digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, ROUNDS);
    EXPECT(g_digest == INET_DIGEST);
}

/* A few fixed strings, so a reader sees the format. */
static void test_strings(void)
{
    struct netaddr a;
    char buf[64];
    memset(&a, 0, sizeof(a));
    a.family = COSMO_AF_INET;
    a.v4 = IPV4_ADDR(10, 0, 2, 15);
    a.port = 8080;
    EXPECT(strcmp(netaddr_str(&a, buf, sizeof(buf)), "10.0.2.15:8080") == 0);
    memset(&a, 0, sizeof(a));
    a.family = COSMO_AF_INET6;
    a.v6.s6_addr[0] = 0xfe;
    a.v6.s6_addr[1] = 0x80;
    a.v6.s6_addr[15] = 1;
    a.port = 22;
    EXPECT(strcmp(netaddr_str(&a, buf, sizeof(buf)), "[fe80:0:0:0:0:0:0:1]:22") == 0);
    a.family = 0;
    EXPECT(strcmp(netaddr_str(&a, buf, sizeof(buf)), "<unspec>:22") == 0);
    EXPECT(strcmp(netaddr_str(&a, buf, 4), "<un") == 0);
}

/* INET_BENCH=1: best of 15 runs of each path, over 1024 addresses. */
static double ns_since(const struct timespec *t0)
{
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (double)(t1.tv_sec - t0->tv_sec) * 1e9 + (double)(t1.tv_nsec - t0->tv_nsec);
}

static void inet_bench(void)
{
    static struct netaddr as[1024];
    static struct cosmo_sockaddr us[1024];
    g_rng = SEED;
    for (unsigned i = 0; i < 1024; i++) {
        address(&as[i]);
        as[i].family = i % 2 ? COSMO_AF_INET6 : COSMO_AF_INET;
        netaddr_to_user_shape(&us[i], &as[i]);
    }
    double best[2] = {1e18, 1e18};
    uint64_t sink = 0;
    for (unsigned t = 0; t < 15; t++) {
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned r = 0; r < 200; r++)
            for (unsigned i = 0; i < 1024; i++)
                sink += netaddr_equal(&as[i], &as[(i + r) & 1023]);
        double a = ns_since(&t0) / (200.0 * 1024);
        clock_gettime(CLOCK_MONOTONIC, &t0);
        struct netaddr out;
        for (unsigned r = 0; r < 200; r++)
            for (unsigned i = 0; i < 1024; i++) {
                sink += (uint64_t)netaddr_from_user_shape(&out, &us[(i + r) & 1023]) + out.port;
                netaddr_to_user_shape(&us[i], &out);
            }
        double b = ns_since(&t0) / (200.0 * 1024);
        if (a < best[0]) best[0] = a;
        if (b < best[1]) best[1] = b;
    }
    printf("  bench netaddr_equal %.2f ns, user shape round trip %.2f ns (sink %llu)\n", best[0], best[1],
           (unsigned long long)sink);
}

static const struct host_test tests[] = {
    { "inet digest", test_digest },
    { "inet strings", test_strings },
};

int main(void)
{
    if (getenv("INET_BENCH") != NULL)
        inet_bench();
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
