/*
 * nd.c - Neighbour discovery (RFC 4861) for link-local addresses: the
 * neighbour table keyed by (interface, address), solicitations and
 * advertisements, and its ageing. ipv6.c dispatches the messages here.
 */

#include <arch/cpu.h>
#include <kernel/errno.h>
#include <kernel/net/cksum.h>
#include <kernel/net/ether.h>
#include <kernel/net/ip.h>
#include <kernel/net/netparse_types.h>
#include <kernel/string.h>
#include <kernel/timer.h>

#define ND_TABLE_SIZE 32
#define ND_RETRY_NS   1000000000ull
#define ND_MAX_TRIES  3
#define ND_REACHABLE_NS (20ull * 60 * 1000000000ull)

struct nd_entry {
    struct in6_addr ip;
    uint8_t mac[ETH_ALEN];
    enum { ND_FREE, ND_INCOMPLETE, ND_REACHABLE } state;
    uint64_t updated_ns;
    struct mbuf *pending;
    unsigned tries;
    struct netif *nif;
};

static struct nd_entry g_nd[ND_TABLE_SIZE];
static spinlock_t g_nd_lock = SPINLOCK_INIT("nd");
static uint64_t g_pending_dropped;   /* ip_stats.nd_pending_dropped, which ipv6_get_stats reads */

struct nd_msg {
    uint8_t type;
    uint8_t code;
    uint16_t cksum;
    uint32_t flags;
    struct in6_addr target;
    uint8_t opt_type;      /* 1 source / 2 target link-layer address */
    uint8_t opt_len;       /* 1 (8 bytes) */
    uint8_t opt_mac[ETH_ALEN];
} __packed;

static void solicited_node_mac(const struct in6_addr *ip, uint8_t mac[ETH_ALEN])
{
    mac[0] = 0x33;
    mac[1] = 0x33;
    mac[2] = 0xff;
    mac[3] = ip->s6_addr[13];
    mac[4] = ip->s6_addr[14];
    mac[5] = ip->s6_addr[15];
}

/* Keyed by (interface, address), as arp.c's find: a link-local address
 * is per link by definition, and the same one on two links is two
 * neighbours (invariant N25). */
#if CONFIG_DEBUG
static void nd_test_park_before_lock(const struct netif *nif);
#else
static inline void nd_test_park_before_lock(const struct netif *nif) { (void)nif; }
#endif

static struct nd_entry *nd_find(const struct netif *nif, const struct in6_addr *ip)
{
    for (unsigned i = 0; i < ND_TABLE_SIZE; i++) {
        if (g_nd[i].state != ND_FREE && g_nd[i].nif == nif && in6_equal(&g_nd[i].ip, ip))
            return &g_nd[i];
    }
    return NULL;
}

/* As arp.c's alloc_entry: without `evict` a full table yields NULL. */
static struct nd_entry *nd_alloc(const struct in6_addr *ip, struct netif *nif, bool evict)
{
    struct nd_entry *v = NULL;
    for (unsigned i = 0; i < ND_TABLE_SIZE; i++) {
        struct nd_entry *e = &g_nd[i];
        if (e->state == ND_FREE) {
            v = e;
            break;
        }
        if (!evict)
            continue;
        if (v == NULL || (v->state == ND_INCOMPLETE && e->state == ND_REACHABLE) ||
            (v->state == e->state && e->updated_ns < v->updated_ns))
            v = e;
    }
    if (v == NULL)
        return NULL;
    if (v->state != ND_FREE)
        m_freem(v->pending);
    memset(v, 0, sizeof(*v));
    v->ip = *ip;
    v->nif = nif;
    v->state = ND_INCOMPLETE;
    v->updated_ns = clock_now_ns();
    return v;
}

/* An ND message from `src` to `dst`: its ICMPv6 checksum, then an IPv6 header, sent
 * directly to the link-layer address `dst_mac` (no routing, no resolution). */
static int nd_output(struct netif *nif, struct mbuf *m, const struct in6_addr *src, const struct in6_addr *dst,
                     const uint8_t *dst_mac)
{
    struct icmp_hdr *ic = (struct icmp_hdr *)m->data;
    ic->cksum = 0;
    uint32_t sum = cksum_pseudo6(src, dst, IPPROTO_ICMPV6, m->pkt.len);
    ic->cksum = cksum_fold(m_cksum_partial(m, 0, m->pkt.len, sum));
    m = m_prepend(m, sizeof(struct ipv6_hdr));
    if (m == NULL)
        return -ENOMEM;
    struct ipv6_hdr *h = (struct ipv6_hdr *)m->data;
    h->vtcfl = htonl(6u << 28);
    h->plen = htons((uint16_t)(m->pkt.len - sizeof(*h)));
    h->nexthdr = IPPROTO_ICMPV6;
    h->hoplimit = 255;
    h->src = *src;
    h->dst = *dst;
    m->pkt.proto = ETH_P_IPV6;
    return ether_output(nif, m, dst_mac, ETH_P_IPV6);
}

static int nd_send(struct netif *nif, uint8_t type, const struct in6_addr *target, const struct in6_addr *dst,
                   const uint8_t *dst_mac)
{
    struct mbuf *m = m_getcl();
    if (m == NULL)
        return -ENOMEM;
    struct nd_msg *nd = (struct nd_msg *)m->data;
    memset(nd, 0, sizeof(*nd));
    nd->type = type;
    nd->flags = type == ICMPV6_NA ? htonl(0x60000000u) : 0;   /* solicited + override */
    nd->target = *target;
    nd->opt_type = type == ICMPV6_NS ? 1 : 2;
    nd->opt_len = 1;
    memcpy(nd->opt_mac, nif->mac, ETH_ALEN);
    m->len = m->pkt.len = sizeof(*nd);
    return nd_output(nif, m, &nif->ip6_ll, dst, dst_mac);
}

int nd_resolve(struct netif *nif, const struct in6_addr *ip, uint8_t mac[ETH_ALEN], struct mbuf *m)
{
    if (in6_is_multicast(ip)) {
        mac[0] = 0x33;
        mac[1] = 0x33;
        memcpy(mac + 2, ip->s6_addr + 12, 4);
        return 0;
    }
    nd_test_park_before_lock(nif);
    arch_irq_state_t s = spin_lock_irqsave(&g_nd_lock);
    if (!(__atomic_load_n(&nif->flags, __ATOMIC_ACQUIRE) & NETIF_UP)) {   /* under the lock, as arp_resolve (N25) */
        if (m)
            __atomic_fetch_add(&g_pending_dropped, 1, __ATOMIC_RELAXED);
        spin_unlock_irqrestore(&g_nd_lock, s);
        m_freem(m);
        return -ENETUNREACH;
    }
    struct nd_entry *e = nd_find(nif, ip);
    if (e && e->state == ND_REACHABLE) {
        memcpy(mac, e->mac, ETH_ALEN);
        spin_unlock_irqrestore(&g_nd_lock, s);
        return 0;
    }
    bool send = e == NULL;
    if (e == NULL)
        e = nd_alloc(ip, nif, true);
    if (m) {
        m_freem(e->pending);
        e->pending = m;
    }
    if (send)
        e->tries = 1;
    spin_unlock_irqrestore(&g_nd_lock, s);
    if (send) {
        struct in6_addr sn = { { 0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0xff, ip->s6_addr[13], ip->s6_addr[14],
                                 ip->s6_addr[15] } };
        uint8_t dmac[ETH_ALEN];
        solicited_node_mac(ip, dmac);
        nd_send(nif, ICMPV6_NS, ip, &sn, dmac);
    }
    return -EINPROGRESS;
}

bool nd_lookup(const struct netif *nif, const struct in6_addr *ip, uint8_t mac[ETH_ALEN])
{
    arch_irq_state_t s = spin_lock_irqsave(&g_nd_lock);
    struct nd_entry *e = nd_find(nif, ip);
    bool ok = e && e->state == ND_REACHABLE;
    if (ok)
        memcpy(mac, e->mac, ETH_ALEN);
    spin_unlock_irqrestore(&g_nd_lock, s);
    return ok;
}

void nd_input_ns(struct netif *nif, struct mbuf *m, const struct ipv6_hdr *ip6)
{
    m = m_pullup(m, sizeof(struct nd_msg));
    if (m == NULL)
        return;
    struct netparse_nd nd;
    netparse_nd(m->data, 1, &nd);   /* a source link-layer address option */
    m_freem(m);
    struct in6_addr target;
    memcpy(target.s6_addr, nd.target, sizeof(target.s6_addr));
    if (!in6_equal(&target, &nif->ip6_ll) || in6_is_unspecified(&ip6->src))
        return;
    /* Learn the asker (never at the cost of an entry in use), answer with our address. A
     * packet parked on the asker's incomplete entry goes now, as ARP's does when a request
     * teaches it the asker: before 2026-11-03 it stayed parked on the reachable entry, never
     * sent, and leaked when the entry expired. */
    if (nd.has_mac) {
        struct mbuf *pending = NULL;
        nd_test_park_before_lock(nif);
        arch_irq_state_t s = spin_lock_irqsave(&g_nd_lock);
        if (!(__atomic_load_n(&nif->flags, __ATOMIC_ACQUIRE) & NETIF_UP)) {
            spin_unlock_irqrestore(&g_nd_lock, s);   /* queued input on a down interface learns nothing (N25): read under the lock */
            return;
        }
        struct nd_entry *e = nd_find(nif, &ip6->src);
        if (e == NULL)
            e = nd_alloc(&ip6->src, nif, false);
        if (e) {
            memcpy(e->mac, nd.mac, ETH_ALEN);
            e->state = ND_REACHABLE;
            e->updated_ns = clock_now_ns();
            pending = e->pending;
            e->pending = NULL;
        }
        spin_unlock_irqrestore(&g_nd_lock, s);
        nd_send(nif, ICMPV6_NA, &nif->ip6_ll, &ip6->src, nd.mac);
        if (pending)
            ether_output(nif, pending, nd.mac, ETH_P_IPV6);
    }
}

void nd_input_na(struct netif *nif, struct mbuf *m, const struct ipv6_hdr *ip6)
{
    (void)ip6;
    m = m_pullup(m, sizeof(struct nd_msg));
    if (m == NULL)
        return;
    struct netparse_nd nd;
    netparse_nd(m->data, 2, &nd);   /* a target link-layer address option */
    m_freem(m);
    if (!nd.has_mac)
        return;
    struct in6_addr target;
    memcpy(target.s6_addr, nd.target, sizeof(target.s6_addr));
    struct mbuf *pending = NULL;
    nd_test_park_before_lock(nif);
    arch_irq_state_t s = spin_lock_irqsave(&g_nd_lock);
    if (!(__atomic_load_n(&nif->flags, __ATOMIC_ACQUIRE) & NETIF_UP)) {
        spin_unlock_irqrestore(&g_nd_lock, s);   /* as nd_input_ns */
        return;
    }
    struct nd_entry *e = nd_find(nif, &target);   /* this interface's entry: an advertisement here completes no other's */
    if (e && e->state == ND_INCOMPLETE) {   /* only the answer to our own solicitation */
        memcpy(e->mac, nd.mac, ETH_ALEN);
        e->state = ND_REACHABLE;
        e->updated_ns = clock_now_ns();
        pending = e->pending;
        e->pending = NULL;
    }
    spin_unlock_irqrestore(&g_nd_lock, s);
    if (pending)
        ether_output(nif, pending, nd.mac, ETH_P_IPV6);
}

#if CONFIG_DEBUG
/* ND's twin of arp_test_park_before_lock (N25 follow-up). */
static const struct netif *g_test_hold_lock_entry;   /* the interface armed for; NULL: disarmed */
static unsigned g_test_lock_entry_parked, g_test_lock_entry_release;

void nd_test_hold_lock_entry(const struct netif *nif)
{
    __atomic_store_n(&g_test_lock_entry_parked, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_test_lock_entry_release, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_test_hold_lock_entry, nif, __ATOMIC_RELEASE);
}
bool nd_test_lock_entry_parked(void) { return __atomic_load_n(&g_test_lock_entry_parked, __ATOMIC_ACQUIRE) != 0; }
void nd_test_release_lock_entry(void) { __atomic_store_n(&g_test_lock_entry_release, 1u, __ATOMIC_RELEASE); }

static void nd_test_park_before_lock(const struct netif *nif)
{
    const struct netif *armed = __atomic_load_n(&g_test_hold_lock_entry, __ATOMIC_RELAXED);
    if (armed == NULL || armed != nif)
        return;   /* not armed, the common case, or armed for another interface: no read-modify-write */
    if (!__atomic_compare_exchange_n(&g_test_hold_lock_entry, &armed, NULL, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return;   /* another caller on this interface took the arming */
    __atomic_store_n(&g_test_lock_entry_parked, 1u, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&g_test_lock_entry_release, __ATOMIC_ACQUIRE))
        arch_cpu_relax();
}

/* ND's twin of arp_test_park_retry: the same one-unlock window, parked
 * the same way, because the defect is in both and a fix proved in one
 * is half a proof (invariant N22). */
static unsigned g_test_hold_retry, g_test_retry_parked, g_test_retry_release;

void nd_test_hold_retry(bool on)
{
    __atomic_store_n(&g_test_retry_parked, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_test_retry_release, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_test_hold_retry, on ? 1u : 0u, __ATOMIC_RELEASE);
}
bool nd_test_retry_parked(void) { return __atomic_load_n(&g_test_retry_parked, __ATOMIC_ACQUIRE) != 0; }
void nd_test_release_retry(void) { __atomic_store_n(&g_test_retry_release, 1u, __ATOMIC_RELEASE); }

static void nd_test_park_retry(unsigned nr)
{
    if (!nr || !__atomic_load_n(&g_test_hold_retry, __ATOMIC_ACQUIRE))
        return;
    __atomic_store_n(&g_test_hold_retry, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_test_retry_parked, 1u, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&g_test_retry_release, __ATOMIC_ACQUIRE))
        arch_cpu_relax();
}
#else
static inline void nd_test_park_retry(unsigned nr) { (void)nr; }
#endif

void nd_flush(struct netif *nif)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_nd_lock);
    for (unsigned i = 0; i < ND_TABLE_SIZE; i++) {
        if (g_nd[i].state != ND_FREE && g_nd[i].nif == nif) {
            /* Counted, like ARP's flush now counts its own. ND keeps a
             * different struct, so this is a different field
             * (docs/audit/next-subsystem-arp-netif-ref.md). */
            if (g_nd[i].pending)
                __atomic_fetch_add(&g_pending_dropped, 1, __ATOMIC_RELAXED);
            m_freem(g_nd[i].pending);
            memset(&g_nd[i], 0, sizeof(g_nd[i]));
        }
    }
    spin_unlock_irqrestore(&g_nd_lock, s);
}

void nd_age(uint64_t now)
{
    struct {
        struct netif *nif;
        struct in6_addr ip;
    } retry[ND_TABLE_SIZE];
    unsigned n = 0;
    arch_irq_state_t s = spin_lock_irqsave(&g_nd_lock);
    for (unsigned i = 0; i < ND_TABLE_SIZE; i++) {
        struct nd_entry *e = &g_nd[i];
        if (e->state == ND_FREE)
            continue;
        if (e->state == ND_REACHABLE) {
            if (clock_delta_ns(now, e->updated_ns) > ND_REACHABLE_NS)
                memset(e, 0, sizeof(*e));
            continue;
        }
        if (clock_delta_ns(now, e->updated_ns) < ND_RETRY_NS)
            continue;
        if (e->tries >= ND_MAX_TRIES) {
            if (e->pending)
                __atomic_fetch_add(&g_pending_dropped, 1, __ATOMIC_RELAXED);   /* the give-up path, for symmetry */
            m_freem(e->pending);
            memset(e, 0, sizeof(*e));
            continue;
        }
        e->tries++;
        e->updated_ns = now;
        /* A reference: this pointer outlives the lock, and `nd_send`
         * below dereferences it. Sound here for the same reason as
         * ARP's -- `nd_flush` takes `g_nd_lock`, so an entry present
         * under this lock means `netif_unregister` step 5 has not run
         * and step 6's put is still to come (invariant N22). */
        netif_get(e->nif);
        retry[n].nif = e->nif;
        retry[n].ip = e->ip;
        n++;
    }
    spin_unlock_irqrestore(&g_nd_lock, s);

    nd_test_park_retry(n);

    for (unsigned i = 0; i < n; i++) {
        struct in6_addr sn = { { 0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0xff, retry[i].ip.s6_addr[13],
                                 retry[i].ip.s6_addr[14], retry[i].ip.s6_addr[15] } };
        uint8_t dmac[ETH_ALEN];
        solicited_node_mac(&retry[i].ip, dmac);
        nd_send(retry[i].nif, ICMPV6_NS, &retry[i].ip, &sn, dmac);
        netif_put(retry[i].nif);
    }
}

void nd_init(void)
{
    memset(g_nd, 0, sizeof(g_nd));
}

uint64_t nd_pending_count(void)
{
    return __atomic_load_n(&g_pending_dropped, __ATOMIC_RELAXED);
}
