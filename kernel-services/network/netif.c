/*
 * netif.c - Interfaces, the receive queue, the network worker thread.
 *
 * Drivers call netif_rx from any context; the worker thread ("netrx")
 * drains the queue and runs protocol input in thread context. Timers
 * hand deferred work to the same thread through net_work_queue so no
 * protocol code runs in interrupt context.
 */

#include <kernel/errno.h>
#include <kernel/fwcfg.h>
#include <kernel/log.h>
#include <kernel/mutex.h>
#include <kernel/netif.h>
#include <kernel/net/ether.h>
#include <kernel/net/ip.h>
#include <kernel/panic.h>
#include <kernel/printf.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/wait.h>

#define NET_RXQ_MAX 512u

static LIST_HEAD(g_netifs);
static struct mutex g_netif_lock;
static unsigned g_next_index = 1;
static struct mbufq g_rxq;
static LIST_HEAD(g_work);
static spinlock_t g_work_lock = SPINLOCK_INIT("network");
static struct waitqueue g_worker_wq = WAITQUEUE_INIT(g_worker_wq);
static struct thread *g_worker;
static volatile bool g_worker_ready;

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
    if (nif->name[0] == '\0' || nif->ops == NULL || nif->ops->transmit == NULL || nif->mtu < 68)
        return -EINVAL;
    mutex_lock(&g_netif_lock);
    struct netif *n;
    list_for_each_entry(n, &g_netifs, link) {
        if (strcmp(n->name, nif->name) == 0) {
            mutex_unlock(&g_netif_lock);
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
    mutex_unlock(&g_netif_lock);
    kinfo("net: %s registered (%02x:%02x:%02x:%02x:%02x:%02x, mtu %u)", nif->name, nif->mac[0], nif->mac[1],
          nif->mac[2], nif->mac[3], nif->mac[4], nif->mac[5], nif->mtu);
    if (!(nif->flags & NETIF_LOOPBACK) && nif->ip4.addr == 0)
        netif_autoconfig(nif);
    return 0;
}

void netif_unregister(struct netif *nif)
{
    mutex_lock(&g_netif_lock);
    list_remove(&nif->link);
    list_init(&nif->link);
    mutex_unlock(&g_netif_lock);
    arp_flush(nif);
    kinfo("net: %s unregistered", nif->name);
}

struct netif *netif_find(const char *name)
{
    mutex_lock(&g_netif_lock);
    struct netif *n, *found = NULL;
    list_for_each_entry(n, &g_netifs, link) {
        if (strcmp(n->name, name) == 0) {
            found = n;
            break;
        }
    }
    mutex_unlock(&g_netif_lock);
    return found;
}

struct netif *netif_default(void)
{
    mutex_lock(&g_netif_lock);
    struct netif *n, *found = NULL;
    list_for_each_entry(n, &g_netifs, link) {
        if (!(n->flags & NETIF_LOOPBACK) && (n->flags & NETIF_UP)) {
            found = n;
            break;
        }
    }
    mutex_unlock(&g_netif_lock);
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
    mutex_lock(&g_netif_lock);
    struct netif *n;
    list_for_each_entry(n, &g_netifs, link) {
        if (n->ip4.addr && n->ip4.addr == addr) {
            owns = true;
            break;
        }
    }
    mutex_unlock(&g_netif_lock);
    return owns;
}

bool netif_owns_ipv6(const struct in6_addr *a)
{
    if (in6_is_loopback(a))
        return true;
    bool owns = false;
    mutex_lock(&g_netif_lock);
    struct netif *n;
    list_for_each_entry(n, &g_netifs, link) {
        if (!(n->flags & NETIF_LOOPBACK) && in6_equal(&n->ip6_ll, a)) {
            owns = true;
            break;
        }
    }
    mutex_unlock(&g_netif_lock);
    return owns;
}

/* --- receive path --------------------------------------------------------- */

void netif_rx(struct netif *nif, struct mbuf *m)
{
    m->pkt.rcvif = nif;
    m->pkt.rx_ns = clock_now_ns();
    __atomic_fetch_add(&nif->stats.rx_packets, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&nif->stats.rx_bytes, m->pkt.len, __ATOMIC_RELAXED);
    if (!mbufq_enqueue(&g_rxq, m)) {
        __atomic_fetch_add(&nif->stats.rx_dropped, 1, __ATOMIC_RELAXED);
        return;
    }
    waitqueue_wake_one(&g_worker_wq);
}

int netif_transmit(struct netif *nif, struct mbuf *m)
{
    if (!(nif->flags & NETIF_UP)) {
        m_freem(m);
        __atomic_fetch_add(&nif->stats.tx_dropped, 1, __ATOMIC_RELAXED);
        return -ENETUNREACH;
    }
    uint32_t len = m->pkt.len;
    int rc = nif->ops->transmit(nif, m);
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
    mutex_init(&g_netif_lock, "netifs");
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
    mutex_lock(&g_netif_lock);
    struct netif *n;
    list_for_each_entry(n, &g_netifs, link) {
        const uint8_t *a = (const uint8_t *)&n->ip4.addr;
        kprintf("%s: %s mtu %u  %u.%u.%u.%u  rx %llu/%llu drop %llu  tx %llu/%llu err %llu\n", n->name,
                (n->flags & NETIF_UP) ? "up" : "down", n->mtu, a[0], a[1], a[2], a[3],
                (unsigned long long)n->stats.rx_packets, (unsigned long long)n->stats.rx_bytes,
                (unsigned long long)n->stats.rx_dropped, (unsigned long long)n->stats.tx_packets,
                (unsigned long long)n->stats.tx_bytes, (unsigned long long)n->stats.tx_errors);
    }
    mutex_unlock(&g_netif_lock);
}

/* Module ABI v1 exports (docs/kernel/module/api.md). */
#include <kernel/module.h>
EXPORT_SYMBOL(netif_register);
EXPORT_SYMBOL(netif_unregister);
EXPORT_SYMBOL(netif_rx);
EXPORT_SYMBOL(netif_set_ipv4);
EXPORT_SYMBOL(netif_set_up);
