/*
 * ipv4.c - IPv4 (RFC 791) input, output and route selection, and ICMP
 * echo / destination unreachable (RFC 792).
 */

#include <kernel/errno.h>
#include <kernel/log.h>
#include <kernel/net/cksum.h>
#include <kernel/net/ether.h>
#include <kernel/net/ip.h>
#include <kernel/net/udp.h>
#include <kernel/net/tcp.h>
#include <kernel/random.h>
#include <kernel/string.h>

static struct ip_stats g_stats;
static uint16_t g_ip_id;

#define STAT(f) __atomic_fetch_add(&g_stats.f, 1, __ATOMIC_RELAXED)

/* --- routing ------------------------------------------------------------ */

struct netif *ipv4_route(uint32_t dst)
{
    if ((ntohl(dst) >> 24) == 127 || netif_owns_ipv4(dst))
        return netif_loopback();
    struct netif *nif = netif_default();
    if (nif == NULL || nif->ip4.addr == 0)
        return NULL;
    return nif;
}

uint32_t ipv4_source_for(uint32_t dst)
{
    struct netif *nif = ipv4_route(dst);
    if (nif == NULL)
        return 0;
    if (nif->flags & NETIF_LOOPBACK)
        return netif_owns_ipv4(dst) ? dst : INADDR_LOOPBACK_N;
    return nif->ip4.addr;
}

/* --- output --------------------------------------------------------------- */

int ipv4_output(struct mbuf *m, uint32_t src, uint32_t dst, uint8_t proto, uint8_t ttl)
{
    struct netif *nif = ipv4_route(dst);
    if (nif == NULL) {
        STAT(tx_no_route);
        m_freem(m);
        return -ENETUNREACH;
    }
    if (src == 0)
        src = ipv4_source_for(dst);
    uint32_t total = m->pkt.len + sizeof(struct ipv4_hdr);
    if (total > nif->mtu || total > 65535) {
        m_freem(m);
        return -EMSGSIZE;
    }
    m = m_prepend(m, sizeof(struct ipv4_hdr));
    if (m == NULL)
        return -ENOMEM;
    struct ipv4_hdr *iph = (struct ipv4_hdr *)m->data;
    iph->vhl = 0x45;
    iph->tos = 0;
    iph->len = htons((uint16_t)total);
    iph->id = htons(__atomic_fetch_add(&g_ip_id, 1, __ATOMIC_RELAXED));
    iph->frag = htons(0x4000);   /* don't fragment */
    iph->ttl = ttl;
    iph->proto = proto;
    iph->src = src;
    iph->dst = dst;
    iph->cksum = 0;
    iph->cksum = in_cksum(iph, sizeof(*iph));
    m->pkt.proto = ETH_P_IP;
    STAT(tx);

    if (nif->flags & NETIF_LOOPBACK)
        return netif_transmit(nif, m);

    uint32_t next_hop = dst;
    bool on_link = ((dst ^ nif->ip4.addr) & nif->ip4.mask) == 0 || dst == INADDR_BROADCAST_N;
    if (!on_link) {
        if (nif->ip4.gateway == 0) {
            STAT(tx_no_route);
            m_freem(m);
            return -ENETUNREACH;
        }
        next_hop = nif->ip4.gateway;
    }
    uint8_t mac[ETH_ALEN];
    int rc = arp_resolve(nif, next_hop, mac, m);
    if (rc == -EINPROGRESS)
        return 0;   /* queued on the ARP entry; sent when it resolves */
    if (rc)
        return rc;
    return ether_output(nif, m, mac, ETH_P_IP);
}

/* --- ICMP ---------------------------------------------------------------------- */

void icmp_send_unreach(struct mbuf *orig, const struct ipv4_hdr *iph, uint8_t code)
{
    if (iph->dst == INADDR_BROADCAST_N || (ntohl(iph->dst) >> 24) == 127)
        return;
    /* ICMP header + original IP header + 8 bytes. */
    uint32_t ihl = IPV4_HDR_LEN(iph);
    uint32_t quote = ihl + 8;
    if (quote > orig->pkt.len)
        quote = orig->pkt.len;
    struct mbuf *m = m_getcl();
    if (m == NULL)
        return;
    struct icmp_hdr *ic = (struct icmp_hdr *)m->data;
    memset(ic, 0, sizeof(*ic));
    ic->type = ICMP_DEST_UNREACH;
    ic->code = code;
    if (!m_copydata(orig, 0, quote, m->data + sizeof(*ic))) {
        m_freem(m);
        return;
    }
    m->len = m->pkt.len = sizeof(*ic) + quote;
    ic->cksum = in_cksum(m->data, m->len);
    STAT(icmp_unreach_sent);
    ipv4_output(m, iph->dst, iph->src, IPPROTO_ICMP, IP_DEFAULT_TTL);
}

void icmp_input(struct netif *nif, struct mbuf *m, const struct ipv4_hdr *iph)
{
    (void)nif;
    uint32_t len = m->pkt.len;
    if (len < sizeof(struct icmp_hdr)) {
        m_freem(m);
        return;
    }
    if (cksum_fold(m_cksum_partial(m, 0, len, 0)) != 0) {
        STAT(rx_bad_cksum);
        m_freem(m);
        return;
    }
    m = m_pullup(m, sizeof(struct icmp_hdr));
    if (m == NULL)
        return;
    struct icmp_hdr *ic = (struct icmp_hdr *)m->data;
    if (ic->type == ICMP_ECHO && ic->code == 0 && !(m->flags & M_BCAST)) {
        STAT(icmp_echo_rcvd);
        /* Turn the request into the reply in place. */
        ic->type = ICMP_ECHO_REPLY;
        ic->cksum = 0;
        ic->cksum = cksum_fold(m_cksum_partial(m, 0, len, 0));
        m->flags &= ~(M_BCAST | M_MCAST);
        STAT(icmp_echo_replied);
        ipv4_output(m, iph->dst, iph->src, IPPROTO_ICMP, IP_DEFAULT_TTL);
        return;
    }
    if (ic->type == ICMP_ECHO_REPLY)
        icmp_echo_reply_hook(iph->src, ntohs(ic->id), ntohs(ic->seq));
    m_freem(m);
}

static icmp_echo_reply_fn g_echo_hook;

void icmp_set_echo_reply_hook(icmp_echo_reply_fn fn)
{
    g_echo_hook = fn;
}

void icmp_echo_reply_hook(uint32_t src, uint16_t id, uint16_t seq)
{
    if (g_echo_hook)
        g_echo_hook(src, id, seq);
}

int icmp_send_echo(uint32_t dst, uint16_t id, uint16_t seq, const void *payload, size_t len)
{
    if (len > 1400)
        return -EMSGSIZE;
    struct mbuf *m = m_getcl();
    if (m == NULL)
        return -ENOMEM;
    struct icmp_hdr *ic = (struct icmp_hdr *)m->data;
    ic->type = ICMP_ECHO;
    ic->code = 0;
    ic->cksum = 0;
    ic->id = htons(id);
    ic->seq = htons(seq);
    if (len)
        memcpy(m->data + sizeof(*ic), payload, len);
    m->len = m->pkt.len = (uint32_t)(sizeof(*ic) + len);
    ic->cksum = in_cksum(m->data, m->len);
    return ipv4_output(m, 0, dst, IPPROTO_ICMP, IP_DEFAULT_TTL);
}

/* --- input --------------------------------------------------------------------- */

void ipv4_input(struct netif *nif, struct mbuf *m)
{
    STAT(rx);
    m = m_pullup(m, sizeof(struct ipv4_hdr));
    if (m == NULL) {
        STAT(rx_bad_header);
        return;
    }
    const struct ipv4_hdr *iph = (const struct ipv4_hdr *)m->data;
    unsigned ihl = IPV4_HDR_LEN(iph);
    uint16_t total = ntohs(iph->len);
    if ((iph->vhl >> 4) != 4 || ihl < 20 || total < ihl || total > m->pkt.len) {
        STAT(rx_bad_header);
        m_freem(m);
        return;
    }
    m = m_pullup(m, ihl);
    if (m == NULL) {
        STAT(rx_bad_header);
        return;
    }
    iph = (const struct ipv4_hdr *)m->data;
    if (in_cksum(iph, ihl) != 0) {
        STAT(rx_bad_cksum);
        m_freem(m);
        return;
    }
    if (ntohs(iph->frag) & 0x3fff) {   /* MF or offset: fragments unsupported */
        STAT(rx_fragments);
        m_freem(m);
        return;
    }
    /* Martians: loopback or our own addresses arriving from a real link. */
    if (!(nif->flags & NETIF_LOOPBACK) &&
        ((ntohl(iph->src) >> 24) == 127 || netif_owns_ipv4(iph->src))) {
        STAT(rx_bad_header);
        m_freem(m);
        return;
    }
    bool bcast = iph->dst == INADDR_BROADCAST_N ||
                 (nif->ip4.mask && nif->ip4.addr && (iph->dst | nif->ip4.mask) == INADDR_BROADCAST_N &&
                  ((iph->dst ^ nif->ip4.addr) & nif->ip4.mask) == 0);
    if (!bcast && !netif_owns_ipv4(iph->dst)) {
        STAT(rx_not_for_us);
        m_freem(m);
        return;
    }
    if (bcast)
        m->flags |= M_BCAST;

    /* Trim link padding, drop the header, deliver. The header stays
     * readable behind m->data for the transport layer. */
    if (total < m->pkt.len)
        m_adj(m, -(int)(m->pkt.len - total));
    struct ipv4_hdr hdr;
    memcpy(&hdr, iph, sizeof(hdr));
    m_adj(m, (int)ihl);

    switch (hdr.proto) {
    case IPPROTO_ICMP: icmp_input(nif, m, &hdr); break;
    case IPPROTO_UDP:  udp_input(nif, m, &hdr, NULL); break;
    case IPPROTO_TCP:  tcp_input(nif, m, &hdr, NULL); break;
    default:
        STAT(rx_unknown_proto);
        if (!bcast) {
            /* Re-quote the header for the ICMP error. */
            struct mbuf *q = m_prepend(m, (uint32_t)ihl);
            if (q) {
                memcpy(q->data, &hdr, sizeof(hdr));
                icmp_send_unreach(q, &hdr, ICMP_UNREACH_PROTO);
                m_freem(q);
            }
        } else {
            m_freem(m);
        }
        break;
    }
}

void ipv4_get_stats(struct ip_stats *out)
{
    *out = g_stats;
}
