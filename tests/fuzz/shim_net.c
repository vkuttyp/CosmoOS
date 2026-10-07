/*
 * shim_net.c - What the network stack needs from the kernel, on the host.
 *
 * See shim_net.h. One thread: the stack's worker-thread input, its timer
 * callbacks (interrupt context in the kernel) and the target's own calls
 * all run on it, in the order the target makes them. The spinlocks are the
 * host shim's (tests/host/shim_spinlock.c): taking one twice aborts, and a
 * lock still held when an input ends is the target's lock-balance oracle.
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/net/cksum.h>
#include <kernel/net/ether.h>
#include <kernel/net/ip.h>
#include <kernel/net/tcp.h>
#include <kernel/net/udp.h>
#include <kernel/object.h>
#include <kernel/panic.h>
#include <kernel/random.h>
#include <kernel/sched.h>
#include <kernel/socket.h>
#include <kernel/timer.h>

/* The host headers after the kernel's: Darwin's <stdlib.h> defines htons as
 * a macro, which would break inet.h's inline one. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "shim_net.h"

/* --- the allocator: malloc behind kmalloc, so ASan sees every object ----- */

static size_t g_live;

/* Every live object, for naming a leak: pointer and size. */
#define FZ_LIVE_MAX 65536u
static struct { void *p; size_t size; } g_objs[FZ_LIVE_MAX];
static unsigned g_nobjs;

static void track(void *p, size_t size)
{
    if (g_nobjs < FZ_LIVE_MAX) {
        g_objs[g_nobjs].p = p;
        g_objs[g_nobjs].size = size;
        g_nobjs++;
    }
}

static void untrack(void *p)
{
    for (unsigned i = g_nobjs; i-- > 0;) {
        if (g_objs[i].p == p) {
            g_objs[i] = g_objs[--g_nobjs];
            return;
        }
    }
}

void fz_dump_live(void)
{
    for (unsigned i = 0; i < g_nobjs; i++)
        fprintf(stderr, "  live object %p, %zu bytes\n", g_objs[i].p, g_objs[i].size);
}

void *fz_live_object_of_size(size_t size)
{
    for (unsigned i = 0; i < g_nobjs; i++)
        if (g_objs[i].size == size)
            return g_objs[i].p;
    return NULL;
}

void *kmalloc(size_t size, unsigned flags)
{
    void *p = malloc(size ? size : 1);
    if (p == NULL)
        return NULL;
    if (flags & KMEM_ZERO)
        memset(p, 0, size);
    g_live++;
    track(p, size);
    return p;
}

void *kzalloc(size_t size)
{
    return kmalloc(size, KMEM_ZERO);
}

void kfree(void *p)
{
    if (p == NULL)
        return;
    g_live--;
    untrack(p);
    free(p);
}

struct fz_cache {
    struct kmem_cache k;
    size_t size;
};

struct kmem_cache *kmem_cache_create(const char *name, size_t object_size, size_t align)
{
    (void)name;
    (void)align;
    struct fz_cache *c = calloc(1, sizeof(*c));
    if (c == NULL)
        return NULL;
    c->size = object_size;
    return &c->k;
}

void *kmem_cache_alloc(struct kmem_cache *cache, unsigned flags)
{
    struct fz_cache *c = (struct fz_cache *)cache;
    return kmalloc(c->size, flags);
}

void kmem_cache_free(struct kmem_cache *cache, void *obj)
{
    (void)cache;
    kfree(obj);
}

size_t fz_allocs_live(void)
{
    return g_live;
}

/* --- kobjects: a plain count ------------------------------------------- */

void kobject_init(struct kobject *obj, const struct kobject_type *type)
{
    memset(obj, 0, sizeof(*obj));
    obj->type = type;
    obj->refcount = 1;
}

void kobject_get(struct kobject *obj)
{
    if (obj->refcount == 0)
        panic("kobject_get on a released object");
    obj->refcount++;
}

bool kobject_tryget(struct kobject *obj)
{
    if (obj->refcount == 0)
        return false;
    obj->refcount++;
    return true;
}

void kobject_put(struct kobject *obj)
{
    if (obj->refcount == 0)
        panic("kobject_put underflow");
    if (--obj->refcount == 0 && obj->type && obj->type->release)
        obj->type->release(obj);
}

/* --- the clock and the timers ------------------------------------------ */

static uint64_t g_now = 1000ull * 1000000000ull;   /* not 0: the stack treats 0 stamps as "never" */

uint64_t clock_now_ns(void)
{
    return g_now;
}

uint64_t clock_since_ns(uint64_t stamp)
{
    return g_now > stamp ? g_now - stamp : 0;
}

uint64_t fz_now(void)
{
    return g_now;
}

#define FZ_TIMERS_MAX 4096u
static struct timer *g_timers[FZ_TIMERS_MAX];
static unsigned g_ntimers;

static void timer_unlink(struct timer *t)
{
    for (unsigned i = 0; i < g_ntimers; i++) {
        if (g_timers[i] == t) {
            g_timers[i] = g_timers[--g_ntimers];
            t->state = TIMER_IDLE;
            return;
        }
    }
    panic("fuzz timer: pending timer not on the list");
}

void timer_setup(struct timer *t, timer_fn fn, void *arg)
{
    memset(t, 0, sizeof(*t));
    t->fn = fn;
    t->arg = arg;
    t->state = TIMER_IDLE;
}

void timer_start(struct timer *t, uint64_t delay_ns)
{
    if (t->state == TIMER_PENDING)
        panic("fuzz timer: timer_start on a pending timer");   /* as kernel/timer/timer.c: cancel first */
    if (g_ntimers == FZ_TIMERS_MAX)
        panic("fuzz timer: too many pending timers");
    t->expires_ns = g_now + delay_ns;
    t->state = TIMER_PENDING;
    g_timers[g_ntimers++] = t;
}

bool timer_cancel(struct timer *t)
{
    if (t->state != TIMER_PENDING)
        return false;
    timer_unlink(t);
    return true;
}

bool timer_cancel_sync(struct timer *t)
{
    return timer_cancel(t);   /* one thread: no callback runs elsewhere */
}

unsigned fz_timers_pending(void)
{
    return g_ntimers;
}

void fz_dump_timers(void)
{
    for (unsigned i = 0; i < g_ntimers; i++)
        fprintf(stderr, "  pending timer %p (arg %p), expires in %llu ms\n", (void *)g_timers[i], g_timers[i]->arg,
                (unsigned long long)((g_timers[i]->expires_ns - g_now) / 1000000ull));
}

static struct timer *next_due(void)
{
    struct timer *best = NULL;
    for (unsigned i = 0; i < g_ntimers; i++)
        if (best == NULL || g_timers[i]->expires_ns < best->expires_ns)
            best = g_timers[i];
    return best;
}

/* --- the worker's work queue -------------------------------------------- */

#define FZ_WORK_MAX 4096u
static struct net_work *g_work[FZ_WORK_MAX];
static unsigned g_nwork;

void net_work_init(struct net_work *w, net_work_fn fn, void *arg)
{
    memset(w, 0, sizeof(*w));
    w->fn = fn;
    w->arg = arg;
}

bool net_work_queue(struct net_work *w)
{
    if (__atomic_exchange_n(&w->queued, true, __ATOMIC_ACQ_REL))
        return false;
    if (g_nwork == FZ_WORK_MAX)
        panic("fuzz work: queue full");
    g_work[g_nwork++] = w;
    return true;
}

void fz_run_work(void)
{
    unsigned guard = 0;
    while (g_nwork > 0) {
        struct net_work *w = g_work[0];
        memmove(&g_work[0], &g_work[1], (--g_nwork) * sizeof(g_work[0]));
        __atomic_store_n(&w->queued, false, __ATOMIC_RELEASE);
        w->fn(w->arg);
        if (++guard > 1000000u)
            panic("fuzz work: an item requeues itself without end");
    }
}

static void fire_due(void)
{
    unsigned guard = 0;
    for (;;) {
        struct timer *t = next_due();
        if (t == NULL || t->expires_ns > g_now)
            break;
        timer_unlink(t);
        t->fn(t, t->arg);
        fz_run_work();
        if (++guard > 1000000u)
            panic("fuzz timer: a timer re-arms at once without end");
    }
}

void fz_clock_advance(uint64_t ns)
{
    g_now += ns;
    fire_due();
}

unsigned fz_fire_until(uint64_t max_ns, unsigned max_fires)
{
    /* One timer per count: the earliest pending, the clock moved to it, its
     * callback and the work it queued run -- another timer due at the same
     * instant waits for the next count (fz_clock_advance drains). */
    uint64_t limit = g_now + max_ns;
    unsigned fired = 0;
    while (fired < max_fires) {
        struct timer *t = next_due();
        if (t == NULL || t->expires_ns > limit)
            break;
        if (t->expires_ns > g_now)
            g_now = t->expires_ns;
        timer_unlink(t);
        t->fn(t, t->arg);
        fz_run_work();
        fired++;
    }
    return fired;
}

/* --- random, scheduling, sockets ---------------------------------------- */

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;

void fz_random_seed(uint64_t seed)
{
    g_rng = seed ? seed : 0x9E3779B97F4A7C15ull;
}

uint64_t random_u64(void)
{
    uint64_t x = g_rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    g_rng = x;
    return x * 0x2545F4914F6CDD1Dull;
}

void random_get_bytes(void *buf, size_t len)
{
    uint8_t *p = buf;
    while (len > 0) {
        uint64_t r = random_u64();
        size_t n = len < 8 ? len : 8;
        memcpy(p, &r, n);
        p += n;
        len -= n;
    }
}

void sched_yield(void)
{
}

static unsigned g_sock_wakes;

void sock_wake(struct socket *s)
{
    (void)s;
    g_sock_wakes++;
}

void sock_set_error(struct socket *s, int err)
{
    (void)s;
    (void)err;
    g_sock_wakes++;
}

unsigned fz_sock_wakes(void)
{
    return g_sock_wakes;
}

/* arp.c's age sweep calls into the tap services; the DHCP/DNS target links
 * the real one, every other target this empty one. */
__attribute__((weak)) void tapsvc_dns_age(uint64_t now_ns)
{
    (void)now_ns;
}

/* --- the interface registry --------------------------------------------- */

#define FZ_NETIFS_MAX 4u
static struct fz_netif *g_ifs[FZ_NETIFS_MAX];
static unsigned g_nifs;
static unsigned g_next_index = 1;

static void fz_release(struct kobject *obj)
{
    (void)obj;   /* static storage: nothing to free */
}

static const struct kobject_type fz_netif_type = { .name = "fz-netif", .release = fz_release };

static int fz_transmit(struct netif *nif, struct mbuf *m)
{
    struct fz_netif *f = (struct fz_netif *)nif;
    uint32_t len = m->pkt.len;
    f->transmits++;
    if (len > nif->mtu + ETH_HLEN)
        f->oversize++;
    if (len < 60)
        f->runts++;
    f->last_len = len < FZ_CAPTURE_MAX ? len : FZ_CAPTURE_MAX;
    if (!m_copydata(m, 0, f->last_len, f->last))
        panic("fuzz netif: a transmitted chain is shorter than its pkt.len");
    m_freem(m);
    if (f->on_frame)
        f->on_frame(f, f->last, f->last_len);
    return 0;
}

static void fz_netif_release(struct netif *nif)
{
    (void)nif;   /* static storage */
}

static const struct netif_ops fz_ops = { .transmit = fz_transmit, .release = fz_netif_release };

static int lo_transmit(struct netif *nif, struct mbuf *m)
{
    (void)nif;
    m_freem(m);   /* host-to-self traffic: nothing on the far side here */
    return 0;
}

static const struct netif_ops lo_ops = { .transmit = lo_transmit, .release = fz_netif_release };
static struct netif g_lo;

static void assign(struct netif *nif)
{
    nif->index = g_next_index++;
    spinlock_init(&nif->lock, "netif");
    memset(&nif->stats, 0, sizeof(nif->stats));
    if (!(nif->flags & NETIF_LOOPBACK)) {
        memset(&nif->ip6_ll, 0, sizeof(nif->ip6_ll));
        nif->ip6_ll.s6_addr[0] = 0xfe;
        nif->ip6_ll.s6_addr[1] = 0x80;
        nif->ip6_ll.s6_addr[8] = nif->mac[0] ^ 0x02;
        nif->ip6_ll.s6_addr[9] = nif->mac[1];
        nif->ip6_ll.s6_addr[10] = nif->mac[2];
        nif->ip6_ll.s6_addr[11] = 0xff;
        nif->ip6_ll.s6_addr[12] = 0xfe;
        nif->ip6_ll.s6_addr[13] = nif->mac[3];
        nif->ip6_ll.s6_addr[14] = nif->mac[4];
        nif->ip6_ll.s6_addr[15] = nif->mac[5];
    }
}

void fz_net_init(void)
{
    static bool done;
    if (done)
        return;
    done = true;
    harness_klog_min = KLOG_WARN;   /* the stack's info lines would drown the run */
    memset(&g_lo, 0, sizeof(g_lo));
    strcpy(g_lo.name, "lo");
    g_lo.mtu = 65535;
    g_lo.flags = NETIF_LOOPBACK | NETIF_NOARP | NETIF_UP;
    g_lo.ops = &lo_ops;
    g_lo.ip4.addr = INADDR_LOOPBACK_N;
    g_lo.ip4.mask = htonl(0xff000000u);
    kobject_init(&g_lo.obj, &fz_netif_type);
    assign(&g_lo);
    mbuf_init();
    arp_init();
    nd_init();
    udp_init();
    tcp_init();
    fz_run_work();
}

void fz_netif_register(struct fz_netif *f, const char *name, const uint8_t mac[6], uint32_t addr, uint32_t mask,
                       uint32_t gateway, unsigned flags)
{
    if (g_nifs == FZ_NETIFS_MAX)
        panic("fuzz netif: too many interfaces");
    memset(f, 0, sizeof(*f));
    struct netif *nif = &f->nif;
    strncpy(nif->name, name, sizeof(nif->name) - 1);
    memcpy(nif->mac, mac, 6);
    nif->mtu = 1500;
    nif->flags = flags | NETIF_UP;
    nif->ops = &fz_ops;
    nif->ip4.addr = addr;
    nif->ip4.mask = mask;
    nif->ip4.gateway = gateway;
    kobject_init(&nif->obj, &fz_netif_type);
    assign(nif);
    g_ifs[g_nifs++] = f;
}

void fz_netif_unregister_all(void)
{
    for (unsigned i = 0; i < g_nifs; i++) {
        arp_flush(&g_ifs[i]->nif);
        nd_flush(&g_ifs[i]->nif);
        g_ifs[i]->nif.flags |= NETIF_GONE;
    }
    g_nifs = 0;
}

void fz_deliver(struct fz_netif *f, const void *frame, uint32_t len)
{
    struct mbuf *m = m_getcl();
    if (m == NULL)
        panic("fuzz netif: no mbuf");
    if (m_append(m, frame, len) != 0) {
        m_freem(m);
        return;
    }
    m->pkt.rcvif = &f->nif;
    m->pkt.rx_ns = g_now;
    f->nif.stats.rx_packets++;
    f->nif.stats.rx_bytes += len;
    ether_input(&f->nif, m);
    fz_run_work();
}

struct netif *netif_loopback(void)
{
    kobject_get(&g_lo.obj);
    return &g_lo;
}

struct netif *netif_default(void)
{
    for (unsigned i = 0; i < g_nifs; i++) {
        struct netif *n = &g_ifs[i]->nif;
        if (!(n->flags & (NETIF_LOOPBACK | NETIF_NODEFAULT)) && (n->flags & NETIF_UP)) {
            kobject_get(&n->obj);
            return n;
        }
    }
    return NULL;
}

struct netif *netif_connected(uint32_t dst)
{
    struct netif *best = NULL;
    uint32_t best_mask = 0;
    for (unsigned i = 0; i < g_nifs; i++) {
        struct netif *n = &g_ifs[i]->nif;
        if ((n->flags & NETIF_LOOPBACK) || !(n->flags & NETIF_UP))
            continue;
        if (n->ip4.addr == 0 || n->ip4.mask == 0)
            continue;
        if (((dst ^ n->ip4.addr) & n->ip4.mask) != 0)
            continue;
        uint32_t m = ntohl(n->ip4.mask);
        if (best == NULL || m > best_mask) {
            best = n;
            best_mask = m;
        }
    }
    if (best != NULL)
        kobject_get(&best->obj);
    return best;
}

bool netif_owns_ipv4(uint32_t addr)
{
    if (addr == INADDR_LOOPBACK_N || (ntohl(addr) >> 24) == 127)
        return true;
    for (unsigned i = 0; i < g_nifs; i++)
        if (g_ifs[i]->nif.ip4.addr && g_ifs[i]->nif.ip4.addr == addr)
            return true;
    return false;
}

bool netif_owns_ipv6(const struct in6_addr *a)
{
    if (in6_is_loopback(a))
        return true;
    for (unsigned i = 0; i < g_nifs; i++)
        if (in6_equal(&g_ifs[i]->nif.ip6_ll, a))
            return true;
    return false;
}

int netif_transmit(struct netif *nif, struct mbuf *m)
{
    unsigned flags = nif->flags;
    if (flags & NETIF_GONE) {
        m_freem(m);
        return -ENODEV;
    }
    if (!(flags & NETIF_UP)) {
        m_freem(m);
        nif->stats.tx_dropped++;
        return -ENETUNREACH;
    }
    if ((m->pkt.csum_flags & NET_CSUM_TX) && !(nif->caps & NETIF_CAP_TXCSUM) && !m_csum_complete(m)) {
        m_freem(m);
        nif->stats.tx_errors++;
        return -EINVAL;
    }
    uint32_t len = m->pkt.len;
    int rc = nif->ops->transmit(nif, m);
    if (rc) {
        nif->stats.tx_errors++;
    } else {
        nif->stats.tx_packets++;
        nif->stats.tx_bytes += len;
    }
    return rc;
}
