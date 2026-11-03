/*
 * test_raw.c - Raw ICMP sockets (kernel-services/network/raw.c) on the host:
 * roadmap 2.0 SA6. A digest over seeded histories, recorded from the C, of up
 * to forty raw sockets, more than delivery's batch of sixteen:
 *
 *   - pcbs made (kzalloc failing) for ICMP and for another protocol, and
 *     freed with messages queued;
 *   - messages out: with no address, the wrong family, the unspecified
 *     address, past 65,515 bytes, with no source, past a cluster, with
 *     allocations failing and the output refusing;
 *   - ICMP messages in, one copy for each raw ICMP socket in id order, in
 *     batches: with options in the IPv4 header, split across mbufs, copies
 *     and prepends failing, sockets being released, queues full;
 *   - messages read, and the statistics.
 *
 * mbuf.c is real; the IP output and the sockets are stubs that fold each call
 * into the digest. Every mbuf and pcb is freed, every wake reference given
 * back and no lock held at the end.
 *
 * RAW_BENCH=1 times an echo reply delivered to four sockets, and a message sent.
 */

#include "harness.h"

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/mbuf.h>
#include <kernel/net/inet.h>
#include <kernel/net/ip.h>
#include <kernel/net/raw.h>
#include <kernel/socket.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The digest of the C (raw.c, 2026-11-03), which the CeeSafe port gives. */
#define RAW_DIGEST 0x51770db96e3ebf5eull
#define ROUNDS 20000u
#define SEED 0xC05F5FA2026B0018ull
#define NSOCK 40u

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
    mix32(0x5d00u + which);
    mix64(value);
}

/* mbuf.c's caches and kzalloc over malloc; an allocation fails when g_fail_in counts down to it. */
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

void *kzalloc(size_t size)
{
    if (fail_now())
        return NULL;
    g_live++;
    void *p = malloc(size);
    memset(p, 0x6b, size);   /* kzalloc must zero it: show it if it does not */
    memset(p, 0, size);
    return p;
}

void kfree(void *p)
{
    if (p != NULL)
        g_live--;
    free(p);
}

/* ---- the stubs ---- */

static uint64_t g_policy;

static bool refuses(unsigned bit) { return (g_policy >> bit) & 1; }

#define A(a, b, c, d) IPV4_ADDR(a, b, c, d)

uint32_t ipv4_source_for(uint32_t dst)
{
    note(1, dst);
    return refuses(0) ? 0 : A(10, 0, 2, 15);
}

static int g_out_rc;

int ipv4_output(struct mbuf *m, uint32_t src, uint32_t dst, uint8_t proto, uint8_t ttl)
{
    if (!g_benching) {
        uint8_t bytes[96];
        uint32_t n = m_length(m);
        uint32_t take = n < sizeof(bytes) ? n : (uint32_t)sizeof(bytes);
        m_copydata(m, 0, take, bytes);
        note(2, ((uint64_t)src << 32) | dst);
        note(3, ((uint64_t)n << 16) | ((uint64_t)proto << 8) | ttl);
        mix32(m->pkt.len);
        mix(bytes, take);
    }
    m_freem(m);
    return g_out_rc;
}

static struct socket g_socks[NSOCK];
static struct raw_pcb *g_pcbs[NSOCK];
static bool g_dying[NSOCK];
static long g_wake_refs;

static unsigned sock_index(const struct socket *s) { return (unsigned)(s - g_socks); }

struct socket *sock_wake_ref(struct socket *s)
{
    if (s == NULL) {
        note(4, NSOCK);
        return NULL;
    }
    unsigned i = sock_index(s);
    note(4, ((uint64_t)g_dying[i] << 8) | i);
    if (g_dying[i])
        return NULL;
    g_wake_refs++;
    return s;
}

void sock_wake_unref(struct socket *s)
{
    note(5, sock_index(s));
    g_wake_refs--;
}

void sock_wake(struct socket *s) { note(6, sock_index(s)); }

/* ---- the histories ---- */

static void op_new(void)
{
    unsigned i = (unsigned)(next() % NSOCK);
    if (g_pcbs[i] != NULL)
        return;
    uint8_t proto = next() % 4 == 0 ? IPPROTO_UDP : IPPROTO_ICMP;
    g_socks[i].raw = NULL;
    g_fail_in = next() % 8 == 0 ? 0 : -1;
    struct raw_pcb *p = raw_pcb_new(&g_socks[i], proto);
    g_fail_in = -1;
    note(10, ((uint64_t)(p != NULL) << 32) | ((uint64_t)proto << 8) | i);
    if (p == NULL)
        return;
    EXPECT(g_socks[i].raw == p);
    EXPECT(p->sock == &g_socks[i]);
    mix64(p->id);
    mix32(p->proto);
    g_pcbs[i] = p;
}

static void op_free(void)
{
    unsigned i = (unsigned)(next() % NSOCK);
    raw_pcb_free(g_pcbs[i]);   /* NULL too */
    g_pcbs[i] = NULL;
    note(11, i);
}

static uint8_t g_data[4096];

static void op_send(void)
{
    unsigned i = (unsigned)(next() % NSOCK);
    if (g_pcbs[i] == NULL)
        return;
    struct netaddr to;
    memset(&to, 0, sizeof(to));
    to.family = next() % 8 == 0 ? COSMO_AF_INET6 : COSMO_AF_INET;
    to.v4 = next() % 8 == 0 ? 0 : A(10, 0, 2, (uint8_t)(1 + next() % 9));
    size_t len;
    switch (next() % 8) {
    case 0: len = 65535 - 20 + next() % 2; break;   /* the largest, and one past it */
    case 1: len = sizeof(g_data); break;
    case 2: len = 0; break;
    default: len = 8 + next() % 120; break;
    }
    if (len > sizeof(g_data) && len <= 65535 - 20)
        len = sizeof(g_data);   /* only the size check needs the real 65,515 */
    for (size_t k = 0; k < len && k < 64; k++)
        g_data[k] = (uint8_t)next();
    g_policy = next();
    g_out_rc = refuses(1) ? -ENOBUFS : 0;
    g_fail_in = next() % 8 == 0 ? (int)(next() % 3) : -1;
    int rc = raw_sendto(g_pcbs[i], g_data, len, next() % 16 == 0 ? NULL : &to);
    g_fail_in = -1;
    note(12, ((uint64_t)i << 32) | (uint32_t)rc);
}

static void op_recv(void)
{
    unsigned i = (unsigned)(next() % NSOCK);
    if (g_pcbs[i] == NULL)
        return;
    struct mbuf *m = raw_recv(g_pcbs[i]);
    if (m == NULL) {
        note(13, i);
        return;
    }
    uint8_t bytes[96];
    uint32_t n = m_length(m);
    uint32_t take = n < sizeof(bytes) ? n : (uint32_t)sizeof(bytes);
    m_copydata(m, 0, take, bytes);
    note(14, ((uint64_t)i << 32) | n);
    mix32(m->pkt.len);
    mix(&m->pkt.src, sizeof(m->pkt.src));
    mix(bytes, take);
    m_freem(m);
}

/* An mbuf holding `bytes`, sometimes split across two (the tail at the end of the chain). */
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

static void op_input(void)
{
    uint8_t ip4b[60];
    unsigned ihl = next() % 4 == 0 ? 20 + 4 * (unsigned)(next() % 11) : 20;
    for (unsigned k = 0; k < ihl; k++)
        ip4b[k] = (uint8_t)next();
    struct ipv4_hdr *iph = (struct ipv4_hdr *)ip4b;
    iph->vhl = (uint8_t)(0x40 | ihl / 4);
    iph->proto = IPPROTO_ICMP;
    static uint8_t msg[1600];
    uint32_t len = next() % 8 == 0 ? 1200 + (uint32_t)(next() % 400) : 8 + (uint32_t)(next() % 120);
    for (uint32_t k = 0; k < len; k++)
        msg[k] = (uint8_t)next();
    struct mbuf *m = packet(msg, len);
    for (unsigned i = 0; i < NSOCK; i++)
        g_dying[i] = next() % 8 == 0;
    g_fail_in = next() % 4 == 0 ? (int)(next() % 24) : -1;
    note(15, ((uint64_t)ihl << 32) | len);
    raw_icmp_input(m, iph);
    g_fail_in = -1;
    for (unsigned i = 0; i < NSOCK; i++)
        g_dying[i] = false;
    /* Not consumed or changed (N31). */
    EXPECT(m_length(m) == len && m->pkt.len == len);
    uint8_t check[1600];
    m_copydata(m, 0, len, check);
    EXPECT(memcmp(check, msg, len) == 0);
    m_freem(m);
}

static void op_stats(void)
{
    struct raw_stats s;
    memset(&s, 0x33, sizeof(s));
    raw_get_stats(&s);
    mix(&s, sizeof(s));
}

static void setup(void)
{
    static bool done;
    if (!done) {
        mbuf_init();
        done = true;
    }
}

static void teardown(void)
{
    for (unsigned i = 0; i < NSOCK; i++) {
        raw_pcb_free(g_pcbs[i]);
        g_pcbs[i] = NULL;
    }
}

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    setup();
    for (unsigned r = 0; r < ROUNDS; r++) {
        g_policy = 0;
        g_out_rc = 0;
        uint64_t k = next() % 32;
        if (k < 6)
            op_new();
        else if (k < 8)
            op_free();
        else if (k < 13)
            op_send();
        else if (k < 22)
            op_recv();
        else if (k < 31)
            op_input();
        else
            op_stats();
        EXPECT(harness_locks_held() == 0);
    }
    op_stats();
    teardown();
    printf("  raw digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, ROUNDS);
    EXPECT(g_digest == RAW_DIGEST);
    EXPECT(g_live == 0);
    EXPECT(g_wake_refs == 0);
}

/* RAW_BENCH=1: the median of nine runs, 100,000 messages each. */
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

static void raw_bench(void)
{
    g_rng = SEED;
    setup();
    g_benching = true;
    g_policy = 0;
    g_out_rc = 0;
    for (unsigned i = 0; i < 4; i++)
        g_pcbs[i] = raw_pcb_new(&g_socks[i], IPPROTO_ICMP);
    uint8_t ip4b[20] = { 0x45 };
    struct ipv4_hdr *iph = (struct ipv4_hdr *)ip4b;
    iph->proto = IPPROTO_ICMP;
    iph->src = A(10, 0, 2, 7);
    uint8_t reply[64] = { 0 };
    double in[9], out[9];
    for (unsigned t = 0; t < 9; t++) {
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 100000; i++) {
            struct mbuf *m = m_getcl();
            m_append(m, reply, sizeof(reply));
            raw_icmp_input(m, iph);
            m_freem(m);
            for (unsigned k = 0; k < 4; k++)
                m_freem(raw_recv(g_pcbs[k]));
        }
        in[t] = ns_since(&t0) / 100000;
        struct netaddr to = { .family = COSMO_AF_INET, .v4 = A(10, 0, 2, 7) };
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 100000; i++)
            (void)raw_sendto(g_pcbs[0], reply, sizeof(reply), &to);
        out[t] = ns_since(&t0) / 100000;
    }
    qsort(in, 9, sizeof(double), cmp_double);
    qsort(out, 9, sizeof(double), cmp_double);
    teardown();
    g_benching = false;
    printf("  bench raw_icmp_input (64 bytes, four sockets) %.1f ns, raw_sendto (64 bytes) %.1f ns\n", in[4], out[4]);
}

static const struct host_test tests[] = {
    { "raw digest", test_digest },
};

int main(void)
{
    int rc = harness_run(tests, sizeof(tests) / sizeof(tests[0]));
    if (getenv("RAW_BENCH") != NULL)
        raw_bench();
    return rc;
}
