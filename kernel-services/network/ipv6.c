/*
 * ipv6.c - IPv6 (RFC 8200) input and output, ICMPv6 echo (RFC 4443),
 * and neighbour discovery (RFC 4861) for link-local addresses.
 */

#include <kernel/errno.h>
#include <kernel/log.h>
#include <kernel/net/cksum.h>
#include <kernel/net/ether.h>
#include <kernel/net/ip.h>
#include <kernel/net/tcp.h>
#include <kernel/net/udp.h>
#include <kernel/string.h>
#include <kernel/timer.h>

static struct ip_stats g_stats;
#define STAT(f) __atomic_fetch_add(&g_stats.f, 1, __ATOMIC_RELAXED)

/* --- routing ------------------------------------------------------------ */

struct netif *ipv6_route(const struct in6_addr *dst)
{
    if (in6_is_loopback(dst) || netif_owns_ipv6(dst))
        return netif_loopback();
    if (in6_is_linklocal(dst) || in6_is_multicast(dst))
        return netif_default();
    return NULL;   /* no global routing in this phase */
}

void ipv6_source_for(const struct in6_addr *dst, struct in6_addr *src)
{
    struct netif *nif = ipv6_route(dst);
    memset(src, 0, sizeof(*src));
    if (nif == NULL)
        return;
    if (nif->flags & NETIF_LOOPBACK) {
        *src = netif_owns_ipv6(dst) ? *dst : nif->ip6_ll;
        return;
    }
    *src = nif->ip6_ll;
}

/* --- neighbour discovery --------------------------------------------------- */

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

static struct nd_entry *nd_find(const struct in6_addr *ip)
{
    for (unsigned i = 0; i < ND_TABLE_SIZE; i++) {
        if (g_nd[i].state != ND_FREE && in6_equal(&g_nd[i].ip, ip))
            return &g_nd[i];
    }
    return NULL;
}

static struct nd_entry *nd_alloc(const struct in6_addr *ip, struct netif *nif)
{
    struct nd_entry *v = NULL;
    for (unsigned i = 0; i < ND_TABLE_SIZE; i++) {
        if (g_nd[i].state == ND_FREE) {
            v = &g_nd[i];
            break;
        }
        if (v == NULL || g_nd[i].updated_ns < v->updated_ns)
            v = &g_nd[i];
    }
    if (v->state != ND_FREE)
        m_freem(v->pending);
    memset(v, 0, sizeof(*v));
    v->ip = *ip;
    v->nif = nif;
    v->state = ND_INCOMPLETE;
    v->updated_ns = clock_now_ns();
    return v;
}

static int icmpv6_send(struct netif *nif, struct mbuf *m, const struct in6_addr *src, const struct in6_addr *dst,
                       const uint8_t *dst_mac)
{
    struct icmp_hdr *ic = (struct icmp_hdr *)m->data;
    ic->cksum = 0;
    uint32_t sum = cksum_pseudo6(src, dst, IPPROTO_ICMPV6, m->pkt.len);
    ic->cksum = cksum_fold(m_cksum_partial(m, 0, m->pkt.len, sum));
    if (dst_mac == NULL)
        return ipv6_output(m, src, dst, IPPROTO_ICMPV6, 255);
    /* Directly to a known link-layer address (ND messages). */
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
    return icmpv6_send(nif, m, &nif->ip6_ll, dst, dst_mac);
}

int nd_resolve(struct netif *nif, const struct in6_addr *ip, uint8_t mac[ETH_ALEN], struct mbuf *m)
{
    if (in6_is_multicast(ip)) {
        mac[0] = 0x33;
        mac[1] = 0x33;
        memcpy(mac + 2, ip->s6_addr + 12, 4);
        return 0;
    }
    arch_irq_state_t s = spin_lock_irqsave(&g_nd_lock);
    struct nd_entry *e = nd_find(ip);
    if (e && e->state == ND_REACHABLE) {
        memcpy(mac, e->mac, ETH_ALEN);
        spin_unlock_irqrestore(&g_nd_lock, s);
        return 0;
    }
    bool send = e == NULL;
    if (e == NULL)
        e = nd_alloc(ip, nif);
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

void nd_input_ns(struct netif *nif, struct mbuf *m, const struct ipv6_hdr *ip6)
{
    m = m_pullup(m, sizeof(struct nd_msg));
    if (m == NULL)
        return;
    struct nd_msg nd;
    memcpy(&nd, m->data, sizeof(nd));
    m_freem(m);
    if (!in6_equal(&nd.target, &nif->ip6_ll) || in6_is_unspecified(&ip6->src))
        return;
    /* Learn the asker, answer with our address. */
    if (nd.opt_type == 1 && nd.opt_len == 1) {
        arch_irq_state_t s = spin_lock_irqsave(&g_nd_lock);
        struct nd_entry *e = nd_find(&ip6->src);
        if (e == NULL)
            e = nd_alloc(&ip6->src, nif);
        memcpy(e->mac, nd.opt_mac, ETH_ALEN);
        e->state = ND_REACHABLE;
        e->updated_ns = clock_now_ns();
        spin_unlock_irqrestore(&g_nd_lock, s);
        nd_send(nif, ICMPV6_NA, &nif->ip6_ll, &ip6->src, nd.opt_mac);
    }
}

void nd_input_na(struct netif *nif, struct mbuf *m, const struct ipv6_hdr *ip6)
{
    (void)ip6;
    m = m_pullup(m, sizeof(struct nd_msg));
    if (m == NULL)
        return;
    struct nd_msg nd;
    memcpy(&nd, m->data, sizeof(nd));
    m_freem(m);
    if (nd.opt_type != 2 || nd.opt_len != 1)
        return;
    struct mbuf *pending = NULL;
    arch_irq_state_t s = spin_lock_irqsave(&g_nd_lock);
    struct nd_entry *e = nd_find(&nd.target);
    if (e) {
        memcpy(e->mac, nd.opt_mac, ETH_ALEN);
        e->state = ND_REACHABLE;
        e->updated_ns = clock_now_ns();
        pending = e->pending;
        e->pending = NULL;
    }
    spin_unlock_irqrestore(&g_nd_lock, s);
    if (pending)
        ether_output(nif, pending, nd.opt_mac, ETH_P_IPV6);
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
            if (now - e->updated_ns > ND_REACHABLE_NS)
                memset(e, 0, sizeof(*e));
            continue;
        }
        if (now - e->updated_ns < ND_RETRY_NS)
            continue;
        if (e->tries >= ND_MAX_TRIES) {
            m_freem(e->pending);
            memset(e, 0, sizeof(*e));
            continue;
        }
        e->tries++;
        e->updated_ns = now;
        retry[n].nif = e->nif;
        retry[n].ip = e->ip;
        n++;
    }
    spin_unlock_irqrestore(&g_nd_lock, s);
    for (unsigned i = 0; i < n; i++) {
        struct in6_addr sn = { { 0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0xff, retry[i].ip.s6_addr[13],
                                 retry[i].ip.s6_addr[14], retry[i].ip.s6_addr[15] } };
        uint8_t dmac[ETH_ALEN];
        solicited_node_mac(&retry[i].ip, dmac);
        nd_send(retry[i].nif, ICMPV6_NS, &retry[i].ip, &sn, dmac);
    }
}

void nd_init(void)
{
    memset(g_nd, 0, sizeof(g_nd));
}

/* --- output --------------------------------------------------------------- */

int ipv6_output(struct mbuf *m, const struct in6_addr *src, const struct in6_addr *dst, uint8_t proto,
                uint8_t hoplimit)
{
    struct netif *nif = ipv6_route(dst);
    if (nif == NULL) {
        STAT(tx_no_route);
        m_freem(m);
        return -ENETUNREACH;
    }
    struct in6_addr s;
    if (src == NULL || in6_is_unspecified(src)) {
        ipv6_source_for(dst, &s);
        src = &s;
    }
    if (m->pkt.len > 65535 || m->pkt.len + sizeof(struct ipv6_hdr) > nif->mtu) {
        m_freem(m);
        return -EMSGSIZE;
    }
    m = m_prepend(m, sizeof(struct ipv6_hdr));
    if (m == NULL)
        return -ENOMEM;
    struct ipv6_hdr *h = (struct ipv6_hdr *)m->data;
    h->vtcfl = htonl(6u << 28);
    h->plen = htons((uint16_t)(m->pkt.len - sizeof(*h)));
    h->nexthdr = proto;
    h->hoplimit = hoplimit;
    h->src = *src;
    h->dst = *dst;
    m->pkt.proto = ETH_P_IPV6;
    STAT(tx);
    if (nif->flags & NETIF_LOOPBACK)
        return netif_transmit(nif, m);
    uint8_t mac[ETH_ALEN];
    int rc = nd_resolve(nif, dst, mac, m);
    if (rc == -EINPROGRESS)
        return 0;
    if (rc)
        return rc;
    return ether_output(nif, m, mac, ETH_P_IPV6);
}

/* --- ICMPv6 ----------------------------------------------------------------- */

void icmpv6_input(struct netif *nif, struct mbuf *m, const struct ipv6_hdr *ip6)
{
    uint32_t len = m->pkt.len;
    if (len < sizeof(struct icmp_hdr)) {
        m_freem(m);
        return;
    }
    uint32_t sum = cksum_pseudo6(&ip6->src, &ip6->dst, IPPROTO_ICMPV6, len);
    if (cksum_fold(m_cksum_partial(m, 0, len, sum)) != 0) {
        STAT(rx_bad_cksum);
        m_freem(m);
        return;
    }
    m = m_pullup(m, sizeof(struct icmp_hdr));
    if (m == NULL)
        return;
    struct icmp_hdr *ic = (struct icmp_hdr *)m->data;
    switch (ic->type) {
    case ICMPV6_ECHO:
        STAT(icmp_echo_rcvd);
        ic->type = ICMPV6_ECHO_REPLY;
        m->flags &= ~(M_BCAST | M_MCAST);
        STAT(icmp_echo_replied);
        icmpv6_send(nif, m, in6_is_multicast(&ip6->dst) ? &nif->ip6_ll : &ip6->dst, &ip6->src, NULL);
        return;
    case ICMPV6_NS:
        if (ip6->hoplimit == 255)
            nd_input_ns(nif, m, ip6);
        else
            m_freem(m);
        return;
    case ICMPV6_NA:
        if (ip6->hoplimit == 255)
            nd_input_na(nif, m, ip6);
        else
            m_freem(m);
        return;
    default:
        m_freem(m);
        return;
    }
}

/* --- input ----------------------------------------------------------------- */

void ipv6_input(struct netif *nif, struct mbuf *m)
{
    STAT(rx);
    m = m_pullup(m, sizeof(struct ipv6_hdr));
    if (m == NULL) {
        STAT(rx_bad_header);
        return;
    }
    const struct ipv6_hdr *h = (const struct ipv6_hdr *)m->data;
    uint16_t plen = ntohs(h->plen);
    if ((ntohl(h->vtcfl) >> 28) != 6 || (uint32_t)plen + sizeof(*h) > m->pkt.len) {
        STAT(rx_bad_header);
        m_freem(m);
        return;
    }
    if (!(nif->flags & NETIF_LOOPBACK) && (in6_is_loopback(&h->src) || netif_owns_ipv6(&h->src))) {
        STAT(rx_bad_header);
        m_freem(m);
        return;
    }
    bool for_us = netif_owns_ipv6(&h->dst);
    bool mcast = in6_is_multicast(&h->dst);
    if (mcast) {
        /* All-nodes or our solicited-node group. */
        static const uint8_t all_nodes[16] = { 0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
        bool sn = h->dst.s6_addr[11] == 1 && h->dst.s6_addr[12] == 0xff && h->dst.s6_addr[13] == nif->ip6_ll.s6_addr[13] &&
                  h->dst.s6_addr[14] == nif->ip6_ll.s6_addr[14] && h->dst.s6_addr[15] == nif->ip6_ll.s6_addr[15];
        for_us = memcmp(h->dst.s6_addr, all_nodes, 16) == 0 || sn;
        if (for_us)
            m->flags |= M_MCAST;
    }
    if (!for_us) {
        STAT(rx_not_for_us);
        m_freem(m);
        return;
    }
    if ((uint32_t)plen + sizeof(*h) < m->pkt.len)
        m_adj(m, -(int)(m->pkt.len - plen - sizeof(*h)));
    struct ipv6_hdr hdr;
    memcpy(&hdr, h, sizeof(hdr));
    m_adj(m, (int)sizeof(hdr));
    switch (hdr.nexthdr) {
    case IPPROTO_ICMPV6: icmpv6_input(nif, m, &hdr); break;
    case IPPROTO_UDP:    udp_input(nif, m, NULL, &hdr); break;
    case IPPROTO_TCP:    tcp_input(nif, m, NULL, &hdr); break;
    default:
        STAT(rx_unknown_proto);   /* extension headers are not parsed in this phase */
        m_freem(m);
        break;
    }
}

void ipv6_get_stats(struct ip_stats *out)
{
    *out = g_stats;
}
