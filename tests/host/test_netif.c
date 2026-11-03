/*
 * test_netif.c - The interface layer (kernel-services/network/netif.c,
 * kernel/include/kernel/netif.h) on the host: roadmap 2.0 SA6. A digest over
 * seeded histories, recorded from the C, of up to eight interfaces:
 * registered (valid and refused: names, MTUs, ops, duplicates), configured
 * from fw_cfg or handed to DHCP, found by name, chosen as the default or the
 * connected route, given addresses, brought up and down, forwarded and
 * masqueraded, asked whether they own an address, listed, sent on (with the
 * checksum finished or not, up, down or gone) and received on (queued on a
 * CPU, purged at unregister), and unregistered. The flow hash takes frames
 * of every family, fragmented or not.
 *
 * mbuf.c and the checksum are real; the kernel services netif.c calls are
 * stubs that fold each call into the digest. Worker threads are made but do
 * not run on the host: the boot tests run them.
 *
 * NETIF_BENCH=1 times the hot paths: a receive taken off CPU 0's queue, a
 * transmit, the routing predicates and the flow hash.
 */

#include "harness.h"

#include <kernel/completion.h>
#include <kernel/errno.h>
#include <kernel/fwcfg.h>
#include <kernel/kmalloc.h>
#include <kernel/mbuf.h>
#include <kernel/mutex.h>
#include <kernel/net/cksum.h>
#include <kernel/net/dhcp.h>
#include <kernel/net/ether.h>
#include <kernel/net/ip.h>
#include <kernel/net/netif_cpu.h>
#include <kernel/netif.h>
#include <kernel/object.h>
#include <kernel/percpu.h>
#include <kernel/quiesce.h>
#include <kernel/sched.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/wait.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The digest of the C (netif.c, 2026-11-02), which the CeeSafe port gives. */
#define NETIF_DIGEST 0x83fa866653f4b673ull
#define ROUNDS 6000u
#define SEED 0xC05F5FA2026B000Dull

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

/* A call to a stub: which, and a value. */
static void note(uint32_t which, uint64_t value)
{
    mix32(0x5700u + which);
    mix(&value, sizeof(value));
}

/* ---- mbuf.c's caches, over malloc ---- */

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

/* ---- the kernel services netif.c calls ---- */

static struct thread g_threads[CONFIG_MAX_CPUS + 1];
static unsigned g_threads_made;
static struct net_cpu *g_cpu0;
static struct percpu g_percpu = { .cpu_id = 0, .current = &g_threads[CONFIG_MAX_CPUS] };
static unsigned g_cpu_count = 4;
static uint64_t g_now = 1000000;

struct percpu *arch_percpu_get(void) { return &g_percpu; }
void preempt_enable(void) { g_percpu.preempt_count--; }
void quiesce_read_lock_debug(void) {}
void quiesce_read_unlock_debug(void) {}
void synchronize_quiesce(void) { note(1, 0); }
unsigned cpu_count(void) { return g_cpu_count; }
uint64_t clock_now_ns(void) { return g_now += 1000; }
uint64_t clock_since_ns(uint64_t then) { return g_now > then ? g_now - then : 0; }

struct thread *thread_create_on(void (*entry)(void *arg), void *arg, const char *name, int priority, cpumask_t mask)
{
    (void)entry;
    if (g_cpu0 == NULL)
        g_cpu0 = arg;   /* net_init's: CPU 0's state, which NETIF_BENCH drains */
    note(2, priority);
    note(3, mask);
    mix(name, strlen(name));
    return &g_threads[g_threads_made++ % CONFIG_MAX_CPUS];   /* made, not run: the boot tests run workers */
}

void waitqueue_init(struct waitqueue *wq, const char *name) { (void)wq; mix(name, strlen(name)); }
unsigned waitqueue_wake_one(struct waitqueue *wq) { (void)wq; note(4, 0); return 0; }
void waitqueue_prepare(struct waitqueue *wq, struct wait_entry *e) { (void)wq; (void)e; }
void waitqueue_finish(struct waitqueue *wq, struct wait_entry *e) { (void)wq; (void)e; }
void sched_block_current(void) { abort(); }

void mutex_init(struct mutex *m, const char *name) { (void)m; mix(name, strlen(name)); }
void mutex_lock(struct mutex *m) { (void)m; note(5, 0); }
void mutex_unlock(struct mutex *m) { (void)m; note(6, 0); }
void completion_init(struct completion *c, const char *name) { (void)c; (void)name; }
void complete(struct completion *c) { (void)c; }
void wait_for_completion(struct completion *c) { (void)c; abort(); }   /* no worker is ready: nothing is posted */

void kobject_init(struct kobject *obj, const struct kobject_type *type)
{
    memset(obj, 0, sizeof(*obj));
    obj->type = type;
    obj->refcount = 1;
    mix(type->name, strlen(type->name));
}
void kobject_track_code(struct kobject *obj, uintptr_t code) { (void)obj; (void)code; note(7, 0); }
void kobject_get(struct kobject *obj) { obj->refcount++; }
void kobject_put(struct kobject *obj)
{
    if (--obj->refcount == 0)
        obj->type->release(obj);
}

static const char *g_fwcfg;
bool fwcfg_get_string(const char *key, char *buf, size_t len)
{
    mix(key, strlen(key));
    if (g_fwcfg == NULL)
        return false;
    snprintf(buf, len, "%s", g_fwcfg);
    return true;
}

static struct netif g_nifs[8];
static uint32_t slot_of(const struct netif *nif)
{
    return nif == NULL ? 99u : (uint32_t)(nif - g_nifs);
}

int dhcp_client_start(struct netif *nif) { note(8, slot_of(nif)); return 0; }
void dhcp_client_stop(struct netif *nif) { note(9, slot_of(nif)); }
void dhcp_client_kick(struct netif *nif) { note(10, slot_of(nif)); }
void arp_flush(struct netif *nif) { note(11, slot_of(nif)); }
void nd_flush(struct netif *nif) { note(12, slot_of(nif)); }
void arp_init(void) { note(13, 0); }
void nd_init(void) { note(14, 0); }
void udp_init(void) { note(15, 0); }
void tcp_init(void) { note(16, 0); }
void socket_init(void) { note(17, 0); }
void loopback_init(void) { note(18, 0); }
void sched_dump_register(const char *name, void (*fn)(void)) { (void)fn; mix(name, strlen(name)); }
void ipv4_input(struct netif *nif, struct mbuf *m) { (void)nif; m_freem(m); }
void ipv6_input(struct netif *nif, struct mbuf *m) { (void)nif; m_freem(m); }
void ether_input(struct netif *nif, struct mbuf *m) { (void)nif; m_freem(m); }

size_t (strlcpy)(char *dst, const char *src, size_t size)   /* parenthesised: a fortified libc makes strlcpy a macro */
{
    size_t n = strlen(src);
    if (size > 0) {
        size_t k = n < size - 1 ? n : size - 1;
        memcpy(dst, src, k);
        dst[k] = '\0';
    }
    return n;
}

/* ---- the interfaces' drivers ---- */

static int g_tx_rc;

static int drv_transmit(struct netif *nif, struct mbuf *m)
{
    note(20, slot_of(nif));
    note(21, m_length(m));
    note(22, m->pkt.csum_flags);
    m_freem(m);
    return g_tx_rc;
}

static void drv_release(struct netif *nif) { note(23, slot_of(nif)); }

static unsigned drv_pending(struct netif *nif, unsigned *capacity)
{
    *capacity = 256;
    return slot_of(nif) * 3u;
}

static const struct netif_ops g_ops = { .transmit = drv_transmit, .release = drv_release };
static const struct netif_ops g_ops_pending = { .transmit = drv_transmit, .release = drv_release, .tx_pending = drv_pending };
static const struct netif_ops g_ops_bad = { .transmit = NULL, .release = drv_release };

static bool g_registered[8];

/* ---- the digest ---- */

static const char *const g_names[] = { "eth0", "eth1", "tap0", "tap1", "lo", "wlan0", "" };
static const char *const g_fwcfgs[] = { "10.0.2.15/24,10.0.2.2", "192.168.7.9/16,192.168.0.1", "10.1.1.1,10.1.1.254",
                                        "10.0.0.5/0,10.0.0.1", "10.0.0.5/33,10.0.0.1", "300.1.1.1/24,1.1.1.1",
                                        "1.2.3/24,1.2.3.4", "garbage" };

static void reg(unsigned k)
{
    struct netif *nif = &g_nifs[k];
    memset(nif, 0, sizeof(*nif));
    const char *name = g_names[next() % 7];
    memcpy(nif->name, name, strlen(name) + 1);
    if (next() % 16 == 0)
        memset(nif->name, 'x', sizeof(nif->name));   /* unterminated: refused */
    unsigned mtu_pick = (unsigned)(next() % 4);
    nif->mtu = mtu_pick == 0 ? 40u : mtu_pick == 1 ? 68u : mtu_pick == 2 ? 1500u : 65535u;
    nif->flags = (unsigned)next() & (NETIF_LOOPBACK | NETIF_NODEFAULT | NETIF_AUTOCONF | NETIF_UP);
    nif->caps = (unsigned)next() & (NETIF_CAP_TXCSUM | NETIF_CAP_RXCSUM);
    for (unsigned i = 0; i < 6; i++)
        nif->mac[i] = (uint8_t)next();
    unsigned ops_pick = (unsigned)(next() % 8);
    nif->ops = ops_pick == 0 ? NULL : ops_pick == 1 ? &g_ops_bad : ops_pick < 5 ? &g_ops : &g_ops_pending;
    if (next() % 3 == 0)
        nif->ip4.addr = (uint32_t)next();
    unsigned cfg = (unsigned)(next() % 12);
    g_fwcfg = cfg < 8 ? g_fwcfgs[cfg] : NULL;
    int rc = netif_register(nif);
    mix32((uint32_t)rc);
    if (rc == 0) {
        g_registered[k] = true;
        mix32(nif->index);
        mix(&nif->ip6_ll, sizeof(nif->ip6_ll));
        mix32(nif->ip4.addr);
        mix32(nif->ip4.mask);
        mix32(nif->ip4.gateway);
        mix32(nif->flags);
    }
}

static struct mbuf *frame(uint32_t n)
{
    static uint8_t bytes[200];
    for (uint32_t i = 0; i < n; i++)
        bytes[i] = (uint8_t)next();
    unsigned kind = (unsigned)(next() % 5);
    if (n >= 14) {
        bytes[12] = kind == 0 ? 0x08 : kind == 1 ? 0x86 : kind == 2 ? 0x08 : (uint8_t)next();
        bytes[13] = kind == 0 ? 0x00 : kind == 1 ? 0xdd : kind == 2 ? 0x06 : (uint8_t)next();
    }
    if (kind == 0 && n >= 34) {
        bytes[14] = (uint8_t)(0x45 + (next() % 3));
        if (next() % 2) {
            bytes[20] = 0;
            bytes[21] = 0;   /* not a fragment */
        }
        bytes[23] = next() % 2 ? 6 : 17;
    }
    if (kind == 1 && n >= 54)
        bytes[20] = next() % 2 ? 6 : 17;
    struct mbuf *m = m_getcl();
    m_append(m, bytes, n);
    m->pkt.proto = next() % 2 ? ETH_P_IP : ETH_P_IPV6;
    m->pkt.csum_flags = (uint16_t)(next() % 4);
    m->pkt.csum_start = (uint16_t)(next() % 40);
    m->pkt.csum_offset = (uint16_t)(next() % 20);
    return m;
}

static void mix_info(const struct netif_info *o)
{
    mix(o->name, sizeof(o->name));
    mix32(o->index);
    mix32(o->flags);
    mix32(o->mtu);
    mix(o->mac, sizeof(o->mac));
    mix32(o->addr);
    mix32(o->mask);
    mix32(o->gateway);
    mix(&o->ip6_ll, sizeof(o->ip6_ll));
    mix(&o->stats, sizeof(o->stats));
}

static void step(void)
{
    unsigned k = (unsigned)(next() % 8);
    struct netif *nif = &g_nifs[k];
    unsigned op = (unsigned)(next() % 16);
    mix32(op);
    switch (op) {
    case 0:
    case 1:
        if (!g_registered[k])
            reg(k);
        break;
    case 2:
        if (g_registered[k] && next() % 3 == 0) {
            netif_unregister(nif);
            g_registered[k] = false;
        }
        break;
    case 3: {
        struct netif *f = netif_find(g_names[next() % 7]);
        mix32(slot_of(f));
        if (f != NULL)
            netif_put(f);
        f = netif_loopback();
        mix32(slot_of(f));
        if (f != NULL)
            netif_put(f);
        break;
    }
    case 4: {
        struct netif *f = netif_default();
        mix32(slot_of(f));
        if (f != NULL)
            netif_put(f);
        f = netif_default_ipv4();
        mix32(slot_of(f));
        if (f != NULL)
            netif_put(f);
        uint32_t dst = next() % 2 && g_registered[k] ? nif->ip4.addr ^ (uint32_t)(next() % 256) << 24 : (uint32_t)next();
        f = netif_connected(dst);
        mix32(slot_of(f));
        if (f != NULL)
            netif_put(f);
        break;
    }
    case 5:
        if (g_registered[k]) {
            uint32_t prefix = (uint32_t)(next() % 33);
            uint32_t mask = prefix == 0 ? 0 : htonl(0xffffffffu << (32 - prefix));
            uint32_t addr = next() % 5 == 0 ? 0 : (uint32_t)next();
            netif_set_ipv4(nif, addr, mask, (uint32_t)next());
            mix32(nif->ip4.addr);
        }
        break;
    case 6:
        if (g_registered[k]) {
            unsigned which = (unsigned)(next() % 3);
            bool on = next() % 2;
            if (which == 0)
                netif_set_up(nif, on);
            else if (which == 1)
                netif_set_forward(nif, on);
            else
                netif_set_masquerade(nif, on);
            mix32(nif->flags);
        }
        break;
    case 7: {
        uint32_t a = next() % 2 && g_registered[k] ? nif->ip4.addr : (uint32_t)next();
        if (next() % 8 == 0)
            a = htonl(0x7f000000u | (uint32_t)(next() % 0xffffff));
        bool owns = netif_owns_ipv4(a);
        mix(&owns, sizeof(owns));
        struct in6_addr six;
        if (next() % 2 && g_registered[k])
            six = nif->ip6_ll;
        else
            for (unsigned i = 0; i < 16; i++)
                six.s6_addr[i] = (uint8_t)(next() % 4 == 0 ? next() : 0);
        owns = netif_owns_ipv6(&six);
        mix(&owns, sizeof(owns));
        break;
    }
    case 8: {
        struct mbuf *m = frame((uint32_t)(next() % 200));
        uint32_t h = net_flow_hash(m, next() % 2);
        mix32(h);
        m_freem(m);
        break;
    }
    case 9:
    case 10:
        if (g_registered[k]) {
            struct mbuf *m = frame(14 + (uint32_t)(next() % 120));
            if (op == 9)
                netif_rx(nif, m);
            else
                netif_rx_on(nif, m, (unsigned)(next() % 6));
            mix32(netif_rxq_count(nif));
            mix(&nif->stats, sizeof(nif->stats));
        }
        break;
    case 11:
        if (g_registered[k]) {
            struct mbuf *m = frame(20 + (uint32_t)(next() % 150));
            g_tx_rc = next() % 6 == 0 ? -ENOBUFS : 0;
            int rc = netif_transmit(nif, m);
            mix32((uint32_t)rc);
            mix(&nif->stats, sizeof(nif->stats));
            unsigned pending = 0, capacity = 0;
            bool live = netif_tx_pending(nif, &pending, &capacity);
            mix(&live, sizeof(live));
            mix32(pending);
            mix32(capacity);
        }
        break;
    case 12: {
        struct netif_info list[8];
        memset(list, 0, sizeof(list));
        unsigned max = (unsigned)(next() % 9);
        unsigned n = netif_list(list, max);
        mix32(n);
        for (unsigned i = 0; i < (n < max ? n : max); i++)
            mix_info(&list[i]);
        struct netif_info one;
        memset(&one, 0, sizeof(one));
        bool found = netif_info_by_name(g_names[next() % 7], &one);
        mix(&found, sizeof(found));
        if (found)
            mix_info(&one);
        found = netif_info_by_index((unsigned)(next() % 12), &one);
        mix(&found, sizeof(found));
        if (found)
            mix_info(&one);
        break;
    }
    case 13: {
        char buf[48];
        memset(buf, 0x55, sizeof(buf));
        size_t n = (size_t)(next() % sizeof(buf));
        unsigned count = netif_names(n ? buf : NULL, n);
        mix32(count);
        mix(buf, sizeof(buf));
        break;
    }
    case 14: {
        netif_set_steering(next() % 2);
        bool on = netif_steering();
        mix(&on, sizeof(on));
        struct net_cpu_stats st;
        memset(&st, 0, sizeof(st));
        bool ok = netif_cpu_stats((unsigned)(next() % 5), &st);
        mix(&ok, sizeof(ok));
        mix(&st, sizeof(st));
        g_percpu.cpu_id = (unsigned)(next() % 4);
        break;
    }
    default: {
        static struct net_work works[4];
        static bool made;
        if (!made) {
            for (unsigned i = 0; i < 4; i++)
                net_work_init(&works[i], NULL, NULL);   /* once: an item on a list is never initialised again */
            made = true;
        }
        unsigned w = (unsigned)(next() % 4);
        bool q = net_work_queue(&works[w]);
        mix(&q, sizeof(q));
        break;
    }
    }
}

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    net_init();
    net_start_workers();
    for (unsigned r = 0; r < ROUNDS; r++)
        step();
    for (unsigned k = 0; k < 8; k++)
        if (g_registered[k]) {
            netif_unregister(&g_nifs[k]);
            g_registered[k] = false;
        }
    struct mbuf_stats s;
    mbuf_get_stats(&s);
    printf("  netif digest 0x%016llx over %u rounds: %llu mbufs made, %u threads\n", (unsigned long long)g_digest,
           ROUNDS, (unsigned long long)s.allocs, g_threads_made);
    EXPECT(g_digest == NETIF_DIGEST);
    EXPECT(s.mbufs_alive == 0 && s.clusters_alive == 0);
    EXPECT(g_live == 0);
}

/* NETIF_BENCH=1: best of 15 runs of each path. */
static double ns_since(const struct timespec *t0)
{
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (double)(t1.tv_sec - t0->tv_sec) * 1e9 + (double)(t1.tv_nsec - t0->tv_nsec);
}

static void netif_bench(void)
{
    g_rng = SEED;
    net_init();
    net_start_workers();
    struct netif *nif = &g_nifs[0];
    memset(nif, 0, sizeof(*nif));
    memcpy(nif->name, "eth0", 5);
    nif->mtu = 1500;
    nif->flags = NETIF_UP;
    nif->ops = &g_ops;
    nif->ip4.addr = 0x0f02000a;   /* 10.0.2.15 */
    EXPECT(netif_register(nif) == 0);
    netif_set_ipv4(nif, 0x0f02000a, 0x00ffffff, 0x0202000a);
    static uint8_t tcp[60];
    tcp[12] = 0x08;
    tcp[14] = 0x45;
    tcp[23] = 6;
    for (unsigned i = 26; i < 38; i++)
        tcp[i] = (uint8_t)(i * 7);
    struct mbuf *probe = m_getcl();
    m_append(probe, tcp, 60);
    double best[6] = { 1e18, 1e18, 1e18, 1e18, 1e18, 1e18 };
    uint64_t sink = 0;
    for (unsigned t = 0; t < 15; t++) {
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 20000; i++) {   /* a received frame: steered, queued, taken by the worker */
            struct mbuf *m = m_getcl();
            m_append(m, tcp, 60);
            netif_rx(nif, m);
            m = mbufq_dequeue(&g_cpu0->rxq);
            sink += m->pkt.flow_hash;
            m_freem(m);
        }
        double a = ns_since(&t0) / 20000;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 20000; i++) {   /* a sent frame */
            struct mbuf *m = m_getcl();
            m_append(m, tcp, 60);
            sink += (uint64_t)netif_transmit(nif, m);
        }
        double b = ns_since(&t0) / 20000;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 20000; i++) {   /* a lookup by name */
            struct netif *f = netif_find("eth0");
            sink += f->index;
            kobject_put(&f->obj);
        }
        double c = ns_since(&t0) / 20000;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 20000; i++)   /* an address check */
            sink += netif_owns_ipv4(0x0f02000a + (i & 1));
        double e = ns_since(&t0) / 20000;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 20000; i++) {   /* the connected route */
            struct netif *r = netif_connected(0x2002000a);
            sink += r != NULL;
            if (r != NULL)
                kobject_put(&r->obj);
        }
        double f = ns_since(&t0) / 20000;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 20000; i++)   /* a flow's hash */
            sink += net_flow_hash(probe, true);
        double d = ns_since(&t0) / 20000;
        if (a < best[0]) best[0] = a;
        if (b < best[1]) best[1] = b;
        if (c < best[2]) best[2] = c;
        if (d < best[3]) best[3] = d;
        if (e < best[4]) best[4] = e;
        if (f < best[5]) best[5] = f;
    }
    m_freem(probe);
    printf("  bench receive %.1f ns, transmit %.1f ns, find %.1f ns, flow hash %.1f ns, owns %.1f ns, connected %.1f ns "
           "(sink %llu)\n", best[0], best[1], best[2], best[3], best[4], best[5], (unsigned long long)sink);
}

static const struct host_test tests[] = {
    { "netif digest", test_digest },
};

int main(void)
{
    if (getenv("NETIF_BENCH") != NULL) {
        netif_bench();
        return 0;
    }
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
