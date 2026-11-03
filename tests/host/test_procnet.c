/*
 * test_procnet.c - /proc/net (kernel-services/network/procnet.c) on the
 * host: roadmap 2.0 SA6. A digest over seeded rounds, recorded from the C,
 * of every file rendered from listings of every size -- none, a few, more
 * than the file holds -- with addresses, ports, states, queue lengths and
 * counters of every magnitude, IPv4 and IPv6 sockets, interfaces with and
 * without a link-local address, routes, complete and incomplete neighbours,
 * and the allocator failing at the first, second or a later allocation
 * (the text's growth among them).
 *
 * The code under test is procnet.c alone, formatting through the kernel's
 * own kvsnprintf and ksnprintf (kernel/core/printf.c, linked here, not the
 * host's): the listings (interfaces, routes, ARP, TCP and UDP sockets) and
 * the allocator are this file's, seeded. Every return and every byte of every text is in the digest. The
 * text's form is checked as it is produced: whole lines, each ending in a
 * newline; a tcp line 150 characters, a udp or route line 128 (Linux's
 * padding), the dev and arp headers as Linux writes them; no text longer
 * than PROCNET_TEXT_MAX. After the rounds nothing is allocated.
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/net/ether.h>
#include <kernel/net/inet.h>
#include <kernel/net/ip.h>
#include <kernel/net/procnet.h>
#include <kernel/net/tcp.h>
#include <kernel/net/udp.h>
#include <kernel/netif.h>
#include <kernel/printf.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "harness.h"

/* The values procnet.cee writes as its own constants, each the header's. */
_Static_assert(PROCNET_TEXT_MAX == 64u * 1024u - 64u && PROCNET_DEV == 0 && PROCNET_ROUTE == 1 && PROCNET_ARP == 2 &&
               PROCNET_TCP == 3 && PROCNET_TCP6 == 4 && PROCNET_UDP == 5 && PROCNET_UDP6 == 6 &&
               PROCNET_IF_INET6 == 7 && PROCNET_COUNT == 8, "procnet.cee's procnet.h values");
_Static_assert(NETIF_MAX == 32 && NETIF_LOOPBACK == 2 && IPV4_ROUTES_MAX == 33 && ARP_TABLE_SIZE == 64 &&
               COSMO_AF_INET == 2 && COSMO_AF_INET6 == 10 && ENOENT == 2 && ENOMEM == 12,
               "procnet.cee's interface, route, ARP, family and errno values");
_Static_assert(TCP_CLOSED == 0 && TCP_LISTEN == 1 && TCP_SYN_SENT == 2 && TCP_SYN_RCVD == 3 && TCP_ESTABLISHED == 4 &&
               TCP_FIN_WAIT_1 == 5 && TCP_FIN_WAIT_2 == 6 && TCP_CLOSE_WAIT == 7 && TCP_CLOSING == 8 &&
               TCP_LAST_ACK == 9 && TCP_TIME_WAIT == 10, "procnet.cee's enum tcp_state");

/* The digest of the C (procnet.c, 2026-11-04), which the CeeSafe port gives. */
#define PROCNET_DIGEST 0x4b8286238c3a895bull
#define ROUNDS 3000u
#define SEED 0x9C0C4E7E2026B111ull

static uint64_t g_rng, g_digest;

static uint64_t next(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static unsigned pick(unsigned n) { return (unsigned)(next() % n); }

static void mix(const void *bytes, size_t n)
{
    const uint8_t *b = bytes;
    for (size_t i = 0; i < n; i++) {
        g_digest ^= b[i];
        g_digest *= 0x100000001b3ull;
    }
}

static void mix64(uint64_t v) { mix(&v, sizeof(v)); }

static bool g_benching;

static void note(uint32_t which, uint64_t value)
{
    if (g_benching)
        return;
    mix(&which, sizeof(which));
    mix64(value);
}

/* A value of every magnitude: 0, small, a power's edge, the type's top. */
static uint64_t magnitude(void)
{
    switch (pick(6)) {
    case 0: return 0;
    case 1: return pick(10);
    case 2: return next() % 100000;
    case 3: {
        uint64_t edge = 1ull << pick(64);   /* one draw per statement */
        return edge - pick(2);
    }
    case 4: return ~0ull - pick(3);
    default: return next();
    }
}

/* ---- the allocator: malloc, counted; kmalloc fails when g_fail_in counts down to it ---- */

static int g_fail_in = -1;
static long g_live;

void *kmalloc(size_t size, unsigned flags)
{
    if (g_fail_in >= 0 && g_fail_in-- == 0) {
        note(1, size);
        return NULL;
    }
    void *p = (flags & KMEM_ZERO) ? calloc(1, size) : malloc(size);
    if (p != NULL)
        g_live++;
    return p;
}

void kfree(void *p)
{
    if (p != NULL)
        g_live--;
    free(p);
}


/* ---- the listings ---- */

/* How many entries a listing returns: none, a few, or the most it may write (or more, which procnet clips). */
static unsigned count(unsigned max)
{
    if (g_benching)
        return max;   /* the bench: every listing full */
    unsigned k = pick(8);
    if (k == 0)
        return 0;
    if (k == 1)
        return max;
    if (k == 2)
        return max + 1 + pick(3);
    return 1 + pick(max < 12 ? max : 12);
}

static void name(char *out, size_t n)
{
    static const char *const names[] = { "lo", "eth0", "eth1", "tap0", "tap7", "dhcpt0", "abcdefg" };
    snprintf(out, n, "%s", names[pick(sizeof(names) / sizeof(names[0]))]);
}

unsigned netif_list(struct netif_info *out, unsigned max)
{
    unsigned n = count(max);
    if (n > max)
        n = max;   /* netif_list writes and returns at most max: the registry holds no more */
    for (unsigned i = 0; i < n; i++) {
        struct netif_info *f = &out[i];
        memset(f, 0, sizeof(*f));
        name(f->name, sizeof(f->name));
        f->index = 1 + pick(40);
        f->flags = (unsigned)next() & 0xff;
        f->mtu = pick(2) ? 1500 : 65535;
        for (unsigned k = 0; k < 6; k++)
            f->mac[k] = (uint8_t)next();
        f->addr = (uint32_t)next();
        f->mask = (uint32_t)next();
        if (pick(3) != 0)
            for (unsigned k = 0; k < 16; k++)
                f->ip6_ll.s6_addr[k] = (uint8_t)next();
        f->stats.rx_packets = magnitude();
        f->stats.rx_bytes = magnitude();
        f->stats.rx_dropped = magnitude();
        f->stats.rx_errors = magnitude();
        f->stats.tx_packets = magnitude();
        f->stats.tx_bytes = magnitude();
        f->stats.tx_dropped = magnitude();
        f->stats.tx_errors = magnitude();
    }
    note(2, ((uint64_t)max << 32) | n);
    return n;
}

unsigned ipv4_route_list(struct ipv4_route_info *out, unsigned max)
{
    unsigned n = count(max);
    if (n > max)
        n = max;
    for (unsigned i = 0; i < n; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        name(out[i].ifname, sizeof(out[i].ifname));
        out[i].dst = (uint32_t)next();
        out[i].mask = (uint32_t)next();
        out[i].gateway = pick(2) ? 0 : (uint32_t)next();
        out[i].src = (uint32_t)next();
        out[i].flags = (unsigned)next() & 0xffff;
    }
    note(3, ((uint64_t)max << 32) | n);
    return n;
}

unsigned arp_list(struct arp_info *out, unsigned max)
{
    unsigned n = count(max);
    unsigned k = n < max ? n : max;
    for (unsigned i = 0; i < k; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        out[i].ip = (uint32_t)next();
        for (unsigned j = 0; j < 6; j++)
            out[i].mac[j] = (uint8_t)next();
        out[i].complete = pick(2);
        name(out[i].ifname, sizeof(out[i].ifname));
    }
    note(4, ((uint64_t)max << 32) | n);
    return n;
}

static void addr(struct netaddr *a, uint16_t family)
{
    memset(a, 0, sizeof(*a));
    a->family = pick(16) == 0 ? 0 : family;   /* now and then an unset one */
    a->port = (uint16_t)next();
    if (family == COSMO_AF_INET6)
        for (unsigned k = 0; k < 16; k++)
            a->v6.s6_addr[k] = (uint8_t)next();
    else
        a->v4 = (uint32_t)next();
}

unsigned tcp_list(struct tcp_sock_info *out, unsigned max, uint16_t family)
{
    unsigned n = count(max);
    unsigned k = n < max ? n : max;
    for (unsigned i = 0; i < k; i++) {
        addr(&out[i].local, family);
        addr(&out[i].remote, family);
        out[i].state = (enum tcp_state)pick(12);
        out[i].txq = (uint32_t)magnitude();
        out[i].rxq = (uint32_t)magnitude();
    }
    note(5, ((uint64_t)max << 32) | ((uint64_t)family << 16) | n);
    return n;
}

unsigned udp_list(struct udp_sock_info *out, unsigned max, uint16_t family)
{
    unsigned n = count(max);
    unsigned k = n < max ? n : max;
    for (unsigned i = 0; i < k; i++) {
        addr(&out[i].local, family);
        addr(&out[i].remote, family);
        if (pick(2))
            out[i].remote.port = 0;   /* not connected */
        out[i].rxq = (unsigned)magnitude();
        out[i].drops = magnitude();
    }
    note(6, ((uint64_t)max << 32) | ((uint64_t)family << 16) | n);
    return n;
}

/* ---- the form ---- */

static void check_form(enum procnet_file f, const char *text, int len)
{
    EXPECT(len >= 0 && (size_t)len <= PROCNET_TEXT_MAX && strlen(text) == (size_t)len);
    EXPECT(len == 0 || text[len - 1] == '\n');
    const char *p = text;
    unsigned lines = 0;
    while (*p) {
        const char *e = strchr(p, '\n');
        EXPECT(e != NULL);
        if (e == NULL)
            return;
        size_t w = (size_t)(e - p);
        if (f == PROCNET_TCP || f == PROCNET_TCP6)
            EXPECT(w >= 149);
        else if (f == PROCNET_UDP || f == PROCNET_UDP6 || f == PROCNET_ROUTE)
            EXPECT(w >= 127);
        if (lines == 0 && f == PROCNET_DEV)
            EXPECT(strncmp(p, "Inter-|   Receive", 17) == 0);
        if (lines == 0 && f == PROCNET_ARP)
            EXPECT(strncmp(p, "IP address       HW type", 24) == 0);
        lines++;
        p = e + 1;
    }
    note(7, lines);
}

static void render(enum procnet_file f)
{
    char *out = NULL;
    g_fail_in = pick(12) == 0 ? (int)pick(6) : -1;
    int rc = procnet_render(f, &out);
    g_fail_in = -1;
    note(8, ((uint64_t)f << 32) | (uint32_t)rc);
    if (rc < 0) {
        EXPECT(rc == -ENOMEM || rc == -ENOENT);
        return;
    }
    check_form(f, out, rc);
    mix(out, (size_t)rc);
    kfree(out);
}

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    long baseline = g_live;
    for (unsigned f = 0; f < PROCNET_COUNT; f++)
        mix(procnet_names[f], strlen(procnet_names[f]) + 1);
    for (unsigned r = 0; r < ROUNDS; r++)
        render((enum procnet_file)pick(PROCNET_COUNT + 1));   /* PROCNET_COUNT: no such file */
    EXPECT(g_live == baseline);
    printf("  procnet digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, ROUNDS);
    EXPECT(g_digest == PROCNET_DIGEST);
}

/* PROCNET_BENCH=1: the median of nine runs of 2,000 renders of /proc/net/tcp with its full 400 sockets and of
 * /proc/net/dev with 32 interfaces. */
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

static void procnet_bench(void)
{
    g_benching = true;
    double t[9], d[9];
    for (unsigned r = 0; r < 9; r++) {
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 2000; i++) {
            g_rng = SEED;   /* count() then picks: the same listings every time */
            char *out;
            int rc = procnet_render(PROCNET_TCP, &out);
            if (rc >= 0)
                kfree(out);
        }
        t[r] = ns_since(&t0) / 2000;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 2000; i++) {
            g_rng = SEED ^ 1;
            char *out;
            int rc = procnet_render(PROCNET_DEV, &out);
            if (rc >= 0)
                kfree(out);
        }
        d[r] = ns_since(&t0) / 2000;
    }
    qsort(t, 9, sizeof(double), cmp_double);
    qsort(d, 9, sizeof(double), cmp_double);
    g_benching = false;
    printf("  bench /proc/net/tcp %.1f ns, /proc/net/dev %.1f ns\n", t[4], d[4]);
}

static const struct host_test tests[] = {
    { "procnet digest", test_digest },
};

int main(void)
{
    int rc = harness_run(tests, sizeof(tests) / sizeof(tests[0]));
    if (getenv("PROCNET_BENCH") != NULL)
        procnet_bench();
    return rc;
}
