/*
 * netif.c - Interfaces, the per-CPU receive queues, the network workers.
 *
 * Drivers call netif_rx from any context; the packet is steered by its
 * flow hash to one CPU's receive queue, whose pinned worker ("netrx/N")
 * drains it and runs protocol input in thread context, so one flow is
 * processed in order by one thread and different flows in parallel
 * (docs/kernel-services/network/design.md, "Receive scaling"). Timers
 * hand deferred work to the calling CPU's worker through net_work_queue
 * so no protocol code runs in interrupt context.
 */

#include <kernel/completion.h>
#include <kernel/errno.h>
#include <kernel/fwcfg.h>
#include <kernel/kmalloc.h>
#include <kernel/net/cksum.h>
#include <kernel/net/tcp.h>
#include <kernel/net/udp.h>
#include <kernel/percpu.h>
#include <arch/cpu.h>
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
/* One receive queue, work list and worker per CPU (unit 11). CPU 0's
 * worker starts in net_init; the others in net_start_workers after SMP
 * bring-up. A packet steered to a CPU whose worker is not ready yet goes
 * to CPU 0. */
struct net_cpu {
    struct mbufq rxq;
    struct list_node work;
    spinlock_t work_lock;
    struct waitqueue wq;
    struct thread *worker;
    volatile bool ready;
    struct net_cpu_stats stats;
    unsigned id;
    char name[16];
};
static struct net_cpu g_cpu[CONFIG_MAX_CPUS];
static unsigned g_ncpu = 1;            /* CPUs with a queue (workers may still be starting) */
static bool g_steer = true;
static netif_rx_hook_fn g_rx_hook;
static void *g_rx_hook_arg;

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
    arch_irq_state_t s = spin_lock_irqsave(&g_netif_lock);
    struct netif *n;
    list_for_each_entry(n, &g_netifs, link) {
        if (strcmp(n->name, nif->name) == 0) {
            spin_unlock_irqrestore(&g_netif_lock, s);
            return -EEXIST;   /* the object is untouched: no kobject, no owner count; the caller frees its storage */
        }
    }
    /* Accepted: only now does the object exist (reference 1 to the
     * creator, the owner module's live-object count raised). A failed
     * registration must leave nothing to balance, since the caller's
     * failure path frees the storage directly, not through the release. */
    kobject_init(&nif->obj, &netif_type);
    kobject_track_code(&nif->obj, (uintptr_t)nif->ops->release);
    nif->flags &= ~NETIF_GONE;
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

/* Drop every queued receive packet that arrived on `nif`, on every CPU's
 * queue. Each queue is drained and rebuilt in order; they are short
 * (NET_RXQ_MAX). */
static unsigned rxq_purge(struct netif *nif)
{
    struct mbufq keep;
    mbufq_init(&keep, NET_RXQ_MAX, "net-rxq-keep");
    unsigned dropped = 0;
    for (unsigned i = 0; i < g_ncpu; i++) {
        struct mbufq *q = &g_cpu[i].rxq;
        struct mbuf *m;
        while ((m = mbufq_dequeue(q)) != NULL) {
            if (m->pkt.rcvif == nif) {
                m_freem(m);
                dropped++;
            } else if (!mbufq_enqueue(&keep, m)) {
                m_freem(m);
            }
        }
        while ((m = mbufq_dequeue(&keep)) != NULL) {
            if (!mbufq_enqueue(q, m))
                m_freem(m);
        }
    }
    return dropped;
}

unsigned netif_rxq_count(const struct netif *nif)
{
    (void)nif;
    unsigned n = 0;
    for (unsigned i = 0; i < g_ncpu; i++)
        n += mbufq_len(&g_cpu[i].rxq);
    return n;
}

static bool net_work_queue_on(struct net_work *w, struct net_cpu *c);

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

    /* 4. Nothing of its left in any receive queue, and every worker has
     * finished any input_one it had started: a barrier through each. */
    unsigned dropped = rxq_purge(nif);
    struct worker_barrier *bs = kmalloc(g_ncpu * sizeof(*bs), 0);
    if (bs) {
        unsigned posted = 0;
        for (unsigned i = 0; i < g_ncpu; i++) {
            if (!g_cpu[i].ready)
                continue;
            net_work_init(&bs[i].work, barrier_fn, &bs[i]);
            completion_init(&bs[i].done, "netif-barrier");
            if (net_work_queue_on(&bs[i].work, &g_cpu[i]))
                posted |= 1u << (i % 32);
        }
        for (unsigned i = 0; i < g_ncpu; i++)
            if (g_cpu[i].ready && (posted & (1u << (i % 32))))
                wait_for_completion(&bs[i].done);
        kfree(bs);
    } else {
        /* No memory for the barriers: a grace period plus a pause covers
         * the one input_one per worker that may still run. */
        synchronize_quiesce();
        thread_sleep_ms(10);
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

static uint32_t mix32(uint32_t h, uint32_t v)
{
    h ^= v;
    h *= 0x9E3779B1u;
    h ^= h >> 15;
    return h;
}

uint32_t net_flow_hash(const struct mbuf *m, bool ether)
{
    uint8_t b[96];
    uint32_t n = m->pkt.len < sizeof(b) ? m->pkt.len : (uint32_t)sizeof(b);
    if (n == 0 || !m_copydata(m, 0, n, b))
        return 0;
    uint32_t off = 0, type;
    if (ether) {
        if (n < ETH_HLEN)
            return 0;
        type = ((uint32_t)b[12] << 8) | b[13];
        off = ETH_HLEN;
    } else {
        type = m->pkt.proto;
    }
    uint32_t h = 0x811C9DC5u, proto = 0, ports_at = 0;
    if (type == ETH_P_IP) {
        if (n < off + 20)
            return 0;
        uint32_t ihl = (uint32_t)(b[off] & 0xf) * 4;
        uint32_t src, dst;
        memcpy(&src, b + off + 12, 4);
        memcpy(&dst, b + off + 16, 4);
        proto = b[off + 9];
        h = mix32(mix32(h, src), dst);
        if (!(((b[off + 6] & 0x3f) != 0) || b[off + 7] != 0))   /* not a fragment: ports are here */
            ports_at = off + ihl;
    } else if (type == ETH_P_IPV6) {
        if (n < off + 40)
            return 0;
        for (unsigned k = 0; k < 8; k++) {
            uint32_t w;
            memcpy(&w, b + off + 8 + 4 * k, 4);
            h = mix32(h, w);
        }
        proto = b[off + 6];
        ports_at = off + 40;   /* extension headers: ports left out */
    } else {
        return mix32(h, type);
    }
    h = mix32(h, proto);
    if ((proto == IPPROTO_TCP || proto == IPPROTO_UDP) && ports_at && ports_at + 4 <= n) {
        uint32_t ports;
        memcpy(&ports, b + ports_at, 4);
        h = mix32(h, ports);
    }
    return h ? h : 1;
}

/* The queue a packet goes to: the flow's CPU when steering is on and that
 * CPU's worker is running, else CPU 0's. */
static struct net_cpu *steer(struct netif *nif, struct mbuf *m, int cpu)
{
    if (cpu < 0) {
        if (!g_steer || g_ncpu <= 1)
            return &g_cpu[0];
        m->pkt.flow_hash = net_flow_hash(m, !(nif->flags & NETIF_LOOPBACK));
        cpu = (int)(m->pkt.flow_hash % g_ncpu);
    }
    struct net_cpu *c = &g_cpu[(unsigned)cpu < g_ncpu ? (unsigned)cpu : 0];
    return c->ready ? c : &g_cpu[0];
}

static void rx_common(struct netif *nif, struct mbuf *m, int cpu)
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
    struct net_cpu *c = steer(nif, m, cpu);
    if (c->id == arch_cpu_id())
        __atomic_fetch_add(&c->stats.rx_steered_here, 1, __ATOMIC_RELAXED);
    if (!mbufq_enqueue(&c->rxq, m)) {
        __atomic_fetch_add(&nif->stats.rx_dropped, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&c->stats.rx_dropped, 1, __ATOMIC_RELAXED);
        quiesce_read_unlock();
        return;
    }
    __atomic_fetch_add(&c->stats.rx_queued, 1, __ATOMIC_RELAXED);
    quiesce_read_unlock();
    waitqueue_wake_one(&c->wq);
}

void netif_rx(struct netif *nif, struct mbuf *m)
{
    rx_common(nif, m, -1);
}

void netif_rx_on(struct netif *nif, struct mbuf *m, unsigned cpu)
{
    rx_common(nif, m, (int)cpu);
}

void netif_set_steering(bool on)
{
    g_steer = on;
}

bool netif_steering(void)
{
    return g_steer;
}

bool netif_cpu_stats(unsigned cpu, struct net_cpu_stats *out)
{
    if (cpu >= g_ncpu || !g_cpu[cpu].ready)
        return false;
    *out = g_cpu[cpu].stats;
    return true;
}

void netif_set_rx_hook(netif_rx_hook_fn fn, void *arg)
{
    g_rx_hook_arg = arg;
    __atomic_store_n(&g_rx_hook, fn, __ATOMIC_RELEASE);
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
    /* A transport checksum the interface cannot finish is finished here
     * (unit 11: the transports leave the partial form and NET_CSUM_*). */
    if ((m->pkt.csum_flags & NET_CSUM_TX) && !(nif->caps & NETIF_CAP_TXCSUM) && !m_csum_complete(m)) {
        quiesce_read_unlock();
        m_freem(m);
        __atomic_fetch_add(&nif->stats.tx_errors, 1, __ATOMIC_RELAXED);
        return -EINVAL;
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

static bool net_work_queue_on(struct net_work *w, struct net_cpu *c)
{
    arch_irq_state_t s = spin_lock_irqsave(&c->work_lock);
    bool fresh = !w->queued;
    if (fresh) {
        w->queued = true;
        list_push_back(&c->work, &w->link);
    }
    spin_unlock_irqrestore(&c->work_lock, s);
    waitqueue_wake_one(&c->wq);
    return fresh;
}

bool net_work_queue(struct net_work *w)
{
    /* The calling CPU's worker (a timer fires on the CPU that armed it);
     * an item already on some list stays there. */
    unsigned cpu = arch_cpu_id();
    struct net_cpu *c = &g_cpu[cpu < g_ncpu ? cpu : 0];
    if (!c->ready)
        c = &g_cpu[0];
    return net_work_queue_on(w, c);
}

static bool work_pending(struct net_cpu *c)
{
    arch_irq_state_t s = spin_lock_irqsave(&c->work_lock);
    bool p = !list_empty(&c->work);
    spin_unlock_irqrestore(&c->work_lock, s);
    return p;
}

static void run_work(struct net_cpu *c)
{
    for (;;) {
        arch_irq_state_t s = spin_lock_irqsave(&c->work_lock);
        if (list_empty(&c->work)) {
            spin_unlock_irqrestore(&c->work_lock, s);
            return;
        }
        struct net_work *w = list_entry(c->work.next, struct net_work, link);
        list_remove(&w->link);
        list_init(&w->link);
        w->queued = false;
        spin_unlock_irqrestore(&c->work_lock, s);
        c->stats.work_runs++;
        w->fn(w->arg);
    }
}

static void input_one(struct mbuf *m)
{
    struct netif *nif = m->pkt.rcvif;
    netif_rx_hook_fn hook = __atomic_load_n(&g_rx_hook, __ATOMIC_ACQUIRE);
    if (hook && !hook(nif, m, g_rx_hook_arg))
        return;
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
    struct net_cpu *c = arg;
    c->ready = true;
    for (;;) {
        wait_event(&c->wq, mbufq_len(&c->rxq) > 0 || work_pending(c));
        struct mbuf *m;
        unsigned budget = 64;
        while (budget-- && (m = mbufq_dequeue(&c->rxq)) != NULL)
            input_one(m);
        run_work(c);
    }
}

static void cpu_init(struct net_cpu *c, unsigned id)
{
    c->id = id;
    mbufq_init(&c->rxq, NET_RXQ_MAX, "net-rxq");
    list_init(&c->work);
    spinlock_init(&c->work_lock, "network");
    waitqueue_init(&c->wq, "netrx");
    ksnprintf(c->name, sizeof(c->name), "netrx/%u", id);
}

static void start_worker(struct net_cpu *c)
{
    c->worker = thread_create_on(worker_main, c, c->name, 40, CPUMASK_OF(c->id));
    if (c->worker == NULL)
        panic("net: cannot create the worker thread for CPU %u", c->id);
}

void net_start_workers(void)
{
    unsigned n = cpu_count();
    if (n > CONFIG_MAX_CPUS)
        n = CONFIG_MAX_CPUS;
    for (unsigned i = 1; i < n; i++) {
        cpu_init(&g_cpu[i], i);
        start_worker(&g_cpu[i]);
    }
    __atomic_store_n(&g_ncpu, n, __ATOMIC_RELEASE);
    kinfo("net: %u receive queues", n);
}

extern void udp_init(void);
extern void tcp_init(void);
extern void socket_init(void);

void net_init(void)
{
    cpu_init(&g_cpu[0], 0);
    mbuf_init();
    arp_init();
    nd_init();
    udp_init();
    tcp_init();
    socket_init();
    start_worker(&g_cpu[0]);
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
    for (unsigned i = 0; i < g_ncpu; i++) {
        const struct net_cpu_stats *st = &g_cpu[i].stats;
        kprintf("netrx/%u: %s queued %llu drop %llu local %llu work %llu\n", i, g_cpu[i].ready ? "up" : "starting",
                (unsigned long long)st->rx_queued, (unsigned long long)st->rx_dropped,
                (unsigned long long)st->rx_steered_here, (unsigned long long)st->work_runs);
    }
}

/* Module ABI v1 exports (docs/kernel/module/api.md). */
#include <kernel/module.h>
EXPORT_SYMBOL(netif_register);
EXPORT_SYMBOL(netif_unregister);
EXPORT_SYMBOL(netif_release_static);
EXPORT_SYMBOL(netif_rx);
EXPORT_SYMBOL(netif_rx_on);
EXPORT_SYMBOL(net_flow_hash);
EXPORT_SYMBOL(netif_set_ipv4);
EXPORT_SYMBOL(netif_set_up);
