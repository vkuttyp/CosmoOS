/*
 * ipv6.c - IPv6 (RFC 8200) input and output, ICMPv6 echo (RFC 4443),
 * and the dispatch of neighbour discovery's messages (nd.c).
 */

#include <arch/cpu.h>
#include <kernel/errno.h>
#include <kernel/log.h>
#include <kernel/net/cksum.h>
#include <kernel/net/ether.h>
#include <kernel/net/ip.h>
#include <kernel/net/netparse_types.h>
#include <kernel/net/tcp.h>
#include <kernel/net/udp.h>
#include <kernel/string.h>
#include <kernel/timer.h>

static struct ip_stats g_stats;
#define STAT(f) __atomic_fetch_add(&g_stats.f, 1, __ATOMIC_RELAXED)

/* --- routing ------------------------------------------------------------ */

/* Referenced interface (netif_put when done) or NULL. */
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
    if (nif->flags & NETIF_LOOPBACK)
        *src = netif_owns_ipv6(dst) ? *dst : nif->ip6_ll;
    else
        *src = nif->ip6_ll;
    netif_put(nif);
}

/* An ICMPv6 message from `src` to `dst`: its checksum, then IPv6 output (echo replies). Neighbour
 * discovery sends its messages to a known link-layer address itself (nd.c). */
static int icmpv6_send(struct mbuf *m, const struct in6_addr *src, const struct in6_addr *dst)
{
    struct icmp_hdr *ic = (struct icmp_hdr *)m->data;
    ic->cksum = 0;
    uint32_t sum = cksum_pseudo6(src, dst, IPPROTO_ICMPV6, m->pkt.len);
    ic->cksum = cksum_fold(m_cksum_partial(m, 0, m->pkt.len, sum));
    return ipv6_output(m, src, dst, IPPROTO_ICMPV6, 255);
}

/* --- output --------------------------------------------------------------- */

static int output_on(struct netif *nif, struct mbuf *m, const struct in6_addr *src, const struct in6_addr *dst,
                     uint8_t proto, uint8_t hoplimit);

int ipv6_output(struct mbuf *m, const struct in6_addr *src, const struct in6_addr *dst, uint8_t proto,
                uint8_t hoplimit)
{
    struct netif *nif = ipv6_route(dst);
    if (nif == NULL) {
        STAT(tx_no_route);
        m_freem(m);
        return -ENETUNREACH;
    }
    int rc = output_on(nif, m, src, dst, proto, hoplimit);
    netif_put(nif);
    return rc;
}

static int output_on(struct netif *nif, struct mbuf *m, const struct in6_addr *src, const struct in6_addr *dst,
                     uint8_t proto, uint8_t hoplimit)
{
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
    if (m->pkt.csum_flags & NET_CSUM_TX)
        m->pkt.csum_start += sizeof(*h);
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
        if (!icmp_ratelimit_allow()) {
            m_freem(m);
            return;
        }
        ic->type = ICMPV6_ECHO_REPLY;
        m->flags &= ~(M_BCAST | M_MCAST);
        STAT(icmp_echo_replied);
        icmpv6_send(m, in6_is_multicast(&ip6->dst) ? &nif->ip6_ll : &ip6->dst, &ip6->src);
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
    uint32_t plen;
    if (!netparse_ipv6(m->data, m->pkt.len, &plen)) {
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
    if (plen + sizeof(*h) < m->pkt.len)
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
    out->nd_pending_dropped = nd_pending_count();
}
