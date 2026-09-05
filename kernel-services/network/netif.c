/*
 * netif.c - Interfaces, the receive queue, the network worker thread.
 *
 * Drivers call netif_rx from any context; the worker thread ("netrx")
 * drains the queue and runs protocol input in thread context. Timers
 * hand deferred work to the same thread through net_work_queue so no
 * protocol code runs in interrupt context.
 */

#include <kernel/completion.h>
#include <kernel/errno.h>
#include <kernel/fwcfg.h>
#include <kernel/log.h>
#include <kernel/netif.h>
#include <kernel/net/ether.h>
#include <kernel/net/ip.h>
#include <kernel/panic.h>
#include <kernel/printf.h>
#include <kernel/quiesce.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/wait.h>

#define NET_RXQ_MAX 512u

/*
 * The interface registry lock is a spinlock, not a mutex: netif_owns_*,
 * netif_default and netif_find are predicates that routing and address
 * checks call from thread context, and they must stay callable from any
 * context that does not sleep. Nothing under this lock sleeps, prints
 * are the diagnostics' business (netif_dump), and a mutex here was the
 * sleeping-lock-under-spinlock hazard the Prompt #3 fix pass removed.
 */
static LIST_HEAD(g_netifs);
static spinlock_t g_netif_lock = SPINLOCK_INIT("netifs");
static unsigned g_next_index = 1;
static struct mbufq g_rxq;
static LIST_HEAD(g_work);
static spinlock_t g_work_lock = SPINLOCK_INIT("network");
static struct waitqueue g_worker_wq = WAITQUEUE_INIT(g_worker_wq);
static struct thread *g_worker;
static volatile bool g_worker_ready;

static void netif_release(struct kobject *obj)
{
    struct netif *nif = container_of(obj, struct netif, obj);
    KASSERT(list_empty(&nif->link));   /* unregistered before the last put */
    nif->ops->release(nif);
}

static const struct kobject_type netif_type = { .name = "netif", .release = netif_release };

void netif_release_static(struct netif *nif)
{
    (void)nif;
}

/* --- interfaces -------------------------------------------------------- */

/* "a.b.c.d" -> network-order address; returns the end of the text. */
static const char *parse_ipv4(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < 4; i++) {
        unsigned n = 0, digits = 0;
        while (*s >= '0' && *s <= '9' && digits < 3) {
            n = n * 10 + (unsigned)(*s - '0');
            s++;
            digits++;
        }
        if (digits == 0 || n > 255)
            return NULL;
        v = (v << 8) | n;
        if (i < 3) {
            if (*s != '.')
                return NULL;
            s++;
        }
    }
    *out = htonl(v);
    return s;
}

/* IPv4 configuration for a real interface: fw_cfg "opt/cosmo/ipv4" as
 * "addr/prefix,gateway", else the QEMU user-mode defaults. DHCP is a later
 * phase. */
static void netif_autoconfig(struct netif *nif)
{
    char cfg[64];
    uint32_t addr = IPV4_ADDR(10, 0, 2, 15), gw = IPV4_ADDR(10, 0, 2, 2), mask = htonl(0xffffff00u);
    if (fwcfg_get_string("ipv4", cfg, sizeof(cfg))) {
        uint32_t a, g;
        const char *p = parse_ipv4(cfg, &a);
        unsigned prefix = 24;
        if (p && *p == '/') {
            prefix = 0;
            p++;
            while (*p >= '0' && *p <= '9')
                prefix = prefix * 10 + (unsigned)(*p++ - '0');
        }
        if (p && *p == ',' && parse_ipv4(p + 1, &g) && prefix <= 32) {
            addr = a;
            gw = g;
            mask = prefix == 0 ? 0 : htonl(0xffffffffu << (32 - prefix));
        } else {
            kwarn("net: %s: ignoring malformed opt/cosmo/ipv4 '%s'", nif->name, cfg);
        }
    }
    netif_set_ipv4(nif, addr, mask, gw);
}

int netif_register(struct netif *nif)
{
    if (nif->name[0] == '\0' || nif->ops == NULL || nif->ops->transmit == NULL || nif->ops->release == NULL ||
        nif->mtu < 68)
        return -EINVAL;
    kobject_init(&nif->obj, &netif_type);
    kobject_track_code(&nif->obj, (uintptr_t)nif->ops->release);
    nif->flags &= ~NETIF_GONE;
    arch_irq_state_t s = spin_lock_irqsave(&g_netif_lock);
    struct netif *n;
    list_for_each_entry(n, &g_netifs, link) {
        if (strcmp(n->name, nif->name) == 0) {
            spin_unlock_irqrestore(&g_netif_lock, s);
            return -EEXIST;
        }
    }
    nif->index = g_next_index++;
    spinlock_init(&nif->lock, "netif");
    list_init(&nif->link);
    memset(&nif->stats, 0, sizeof(nif->stats));
    if (!(nif->flags & NETIF_LOOPBACK)) {
        /* Link-local IPv6 from the MAC (modified EUI-64). */
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
    list_push_back(&g_netifs, &nif->link);
    kobject_get(&nif->obj);   /* the registry's reference */
    spin_unlock_irqrestore(&g_netif_lock, s);
    kinfo("net: %s registered (%02x:%02x:%02x:%02x:%02x:%02x, mtu %u)", nif->name, nif->mac[0], nif->mac[1],
          nif->mac[2], nif->mac[3], nif->mac[4], nif->mac[5], nif->mtu);
    if (!(nif->flags & NETIF_LOOPBACK) && nif->ip4.addr == 0)
        netif_autoconfig(nif);
    return 0;
}

/* A worker barrier: runs on the network thread after everything queued
 * before it, so any input_one that dequeued a packet earlier is done. */
struct worker_barrier {
    struct net_work work;
    struct completion done;
};

static void barrier_fn(void *arg)
{
    struct worker_barrier *b = arg;
    complete(&b->done);
}

/* Drop every queued receive packet that arrived on `nif`. The queue is
 * drained and rebuilt in order; it is short (NET_RXQ_MAX). */
static unsigned rxq_purge(struct netif *nif)
{
    struct mbufq keep;
    mbufq_init(&keep, NET_RXQ_MAX, "net-rxq-keep");
    struct mbuf *m;
    unsigned dropped = 0;
    while ((m = mbufq_dequeue(&g_rxq)) != NULL) {
        if (m->pkt.rcvif == nif) {
            m_freem(m);
            dropped++;
        } else if (!mbufq_enqueue(&keep, m)) {
            m_freem(m);
        }
    }
    while ((m = mbufq_dequeue(&keep)) != NULL) {
        if (!mbufq_enqueue(&g_rxq, m))
            m_freem(m);
    }
    return dropped;
}

unsigned netif_rxq_count(const struct netif *nif)
{
    (void)nif;
    return mbufq_len(&g_rxq);
}

void netif_unregister(struct netif *nif)
{
    /* 1. Down and gone: netif_transmit and netif_rx refuse from here. */
    arch_irq_state_t s = spin_lock_irqsave(&nif->lock);
    nif->flags = (nif->flags & ~NETIF_UP) | NETIF_GONE;
    spin_unlock_irqrestore(&nif->lock, s);

    /* 2. Out of the registry: no new lookups. */
    s = spin_lock_irqsave(&g_netif_lock);
    list_remove(&nif->link);
    list_init(&nif->link);
    spin_unlock_irqrestore(&g_netif_lock, s);

    /* 3. A transmit or netif_rx that read the flags before step 1 runs in
     * a read-side section; after one grace period none is in flight, so
     * every packet of its is either in the queue or already input. */
    synchronize_quiesce();

    /* 4. Nothing of its left in the receive queue, and the worker has
     * finished any input_one it had started. */
    unsigned dropped = rxq_purge(nif);
    if (g_worker_ready) {
        struct worker_barrier b;
        net_work_init(&b.work, barrier_fn, &b);
        completion_init(&b.done, "netif-barrier");
        net_work_queue(&b.work);
        wait_for_completion(&b.done);
    }

    /* 5. Tables that name the interface. */
    arp_flush(nif);
    nd_flush(nif);
    kinfo("net: %s unregistered (%u queued packets dropped)", nif->name, dropped);

    /* 6. The registry's reference. */
    kobject_put(&nif->obj);
}

struct netif *netif_find(const char *name)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_netif_lock);
    struct netif *n, *found = NULL;
    list_for_each_entry(n, &g_netifs, link) {
        if (strcmp(n->name, name) == 0) {
            found = n;
            kobject_get(&n->obj);
            break;
        }
    }
    spin_unlock_irqrestore(&g_netif_lock, s);
    return found;
}

struct netif *netif_default(void)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_netif_lock);
    struct netif *n, *found = NULL;
    list_for_each_entry(n, &g_netifs, link) {
        if (!(n->flags & NETIF_LOOPBACK) && (n->flags & NETIF_UP)) {
            found = n;
            kobject_get(&n->obj);
            break;
        }
    }
    spin_unlock_irqrestore(&g_netif_lock, s);
    return found;
}

struct netif *netif_loopback(void)
{
    return netif_find("lo");
}

void netif_set_ipv4(struct netif *nif, uint32_t addr, uint32_t mask, uint32_t gateway)
{
    arch_irq_state_t s = spin_lock_irqsave(&nif->lock);
    nif->ip4.addr = addr;
    nif->ip4.mask = mask;
    nif->ip4.gateway = gateway;
    spin_unlock_irqrestore(&nif->lock, s);
    const uint8_t *a = (const uint8_t *)&addr, *g = (const uint8_t *)&gateway;
    kinfo("net: %s: %u.%u.%u.%u gateway %u.%u.%u.%u", nif->name, a[0], a[1], a[2], a[3], g[0], g[1], g[2], g[3]);
}

void netif_set_up(struct netif *nif, bool up)
{
    arch_irq_state_t s = spin_lock_irqsave(&nif->lock);
    if (up)
        nif->flags |= NETIF_UP;
    else
        nif->flags &= ~NETIF_UP;
    spin_unlock_irqrestore(&nif->lock, s);
}

bool netif_owns_ipv4(uint32_t addr)
{
    if (addr == INADDR_LOOPBACK_N || (ntohl(addr) >> 24) == 127)
        return true;
    bool owns = false;
    arch_irq_state_t s = spin_lock_irqsave(&g_netif_lock);
    struct netif *n;
    list_for_each_entry(n, &g_netifs, link) {
        if (n->ip4.addr && n->ip4.addr == addr) {
            owns = true;
            break;
        }
    }
    spin_unlock_irqrestore(&g_netif_lock, s);
    return owns;
}

bool netif_owns_ipv6(const struct in6_addr *a)
{
    if (in6_is_loopback(a))
        return true;
    bool owns = false;
    arch_irq_state_t s = spin_lock_irqsave(&g_netif_lock);
    struct netif *n;
    list_for_each_entry(n, &g_netifs, link) {
        if (!(n->flags & NETIF_LOOPBACK) && in6_equal(&n->ip6_ll, a)) {
            owns = true;
            break;
        }
    }
    spin_unlock_irqrestore(&g_netif_lock, s);
    return owns;
}

/* --- receive path --------------------------------------------------------- */

void netif_rx(struct netif *nif, struct mbuf *m)
{
    /* Read-side section: a driver calls this from its interrupt (already
     * one) or from a thread (loopback); netif_unregister's grace period
     * waits for it either way. */
    quiesce_read_lock();
    if (__atomic_load_n(&nif->flags, __ATOMIC_ACQUIRE) & NETIF_GONE) {
        quiesce_read_unlock();
        m_freem(m);
        return;
    }
    m->pkt.rcvif = nif;
    m->pkt.rx_ns = clock_now_ns();
    __atomic_fetch_add(&nif->stats.rx_packets, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&nif->stats.rx_bytes, m->pkt.len, __ATOMIC_RELAXED);
    if (!mbufq_enqueue(&g_rxq, m)) {
        __atomic_fetch_add(&nif->stats.rx_dropped, 1, __ATOMIC_RELAXED);
        quiesce_read_unlock();
        return;
    }
    quiesce_read_unlock();
    waitqueue_wake_one(&g_worker_wq);
}

int netif_transmit(struct netif *nif, struct mbuf *m)
{
    /* Read-side section around the driver's transmit: netif_unregister
     * sets GONE, then waits one grace period before the driver frees its
     * queues, so a transmit that saw UP finishes on live hardware. */
    quiesce_read_lock();
    unsigned flags = __atomic_load_n(&nif->flags, __ATOMIC_ACQUIRE);
    if (flags & NETIF_GONE) {
        quiesce_read_unlock();
        m_freem(m);
        return -ENODEV;
    }
    if (!(flags & NETIF_UP)) {
        quiesce_read_unlock();
        m_freem(m);
        __atomic_fetch_add(&nif->stats.tx_dropped, 1, __ATOMIC_RELAXED);
        return -ENETUNREACH;
    }
    uint32_t len = m->pkt.len;
    int rc = nif->ops->transmit(nif, m);
    quiesce_read_unlock();
    if (rc) {
        __atomic_fetch_add(&nif->stats.tx_errors, 1, __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_add(&nif->stats.tx_packets, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&nif->stats.tx_bytes, len, __ATOMIC_RELAXED);
    }
    return rc;
}

void net_work_init(struct net_work *w, net_work_fn fn, void *arg)
{
    list_init(&w->link);
    w->fn = fn;
    w->arg = arg;
    w->queued = false;
}

void net_work_queue(struct net_work *w)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_work_lock);
    if (!w->queued) {
        w->queued = true;
        list_push_back(&g_work, &w->link);
    }
    spin_unlock_irqrestore(&g_work_lock, s);
    waitqueue_wake_one(&g_worker_wq);
}

static bool work_pending(void)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_work_lock);
    bool p = !list_empty(&g_work);
    spin_unlock_irqrestore(&g_work_lock, s);
    return p;
}

static void run_work(void)
{
    for (;;) {
        arch_irq_state_t s = spin_lock_irqsave(&g_work_lock);
        if (list_empty(&g_work)) {
            spin_unlock_irqrestore(&g_work_lock, s);
            return;
        }
        struct net_work *w = list_entry(g_work.next, struct net_work, link);
        list_remove(&w->link);
        list_init(&w->link);
        w->queued = false;
        spin_unlock_irqrestore(&g_work_lock, s);
        w->fn(w->arg);
    }
}

static void input_one(struct mbuf *m)
{
    struct netif *nif = m->pkt.rcvif;
    if (nif->flags & NETIF_LOOPBACK) {
        /* No link layer: pkt.proto carries the EtherType. */
        if (m->pkt.proto == ETH_P_IP)
            ipv4_input(nif, m);
        else if (m->pkt.proto == ETH_P_IPV6)
            ipv6_input(nif, m);
        else
            m_freem(m);
        return;
    }
    ether_input(nif, m);
}

static void worker_main(void *arg)
{
    (void)arg;
    g_worker_ready = true;
    for (;;) {
        wait_event(&g_worker_wq, mbufq_len(&g_rxq) > 0 || work_pending());
        struct mbuf *m;
        unsigned budget = 64;
        while (budget-- && (m = mbufq_dequeue(&g_rxq)) != NULL)
            input_one(m);
        run_work();
    }
}

extern void udp_init(void);
extern void tcp_init(void);
extern void socket_init(void);

void net_init(void)
{
    mbufq_init(&g_rxq, NET_RXQ_MAX, "net-rxq");
    mbuf_init();
    arp_init();
    nd_init();
    udp_init();
    tcp_init();
    socket_init();
    g_worker = thread_create(worker_main, NULL, "netrx", 40);
    if (g_worker == NULL)
        panic("net: cannot create the worker thread");
    loopback_init();
    kinfo("net: stack ready");
}

void netif_dump(void)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_netif_lock);
    struct netif *n;
    list_for_each_entry(n, &g_netifs, link) {
        const uint8_t *a = (const uint8_t *)&n->ip4.addr;
        kprintf("%s: %s mtu %u  %u.%u.%u.%u  rx %llu/%llu drop %llu  tx %llu/%llu err %llu\n", n->name,
                (n->flags & NETIF_UP) ? "up" : "down", n->mtu, a[0], a[1], a[2], a[3],
                (unsigned long long)n->stats.rx_packets, (unsigned long long)n->stats.rx_bytes,
                (unsigned long long)n->stats.rx_dropped, (unsigned long long)n->stats.tx_packets,
                (unsigned long long)n->stats.tx_bytes, (unsigned long long)n->stats.tx_errors);
    }
    spin_unlock_irqrestore(&g_netif_lock, s);
}

/* Module ABI v1 exports (docs/kernel/module/api.md). */
#include <kernel/module.h>
EXPORT_SYMBOL(netif_register);
EXPORT_SYMBOL(netif_unregister);
EXPORT_SYMBOL(netif_release_static);
EXPORT_SYMBOL(netif_rx);
EXPORT_SYMBOL(netif_set_ipv4);
EXPORT_SYMBOL(netif_set_up);
