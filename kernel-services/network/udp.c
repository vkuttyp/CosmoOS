/*
 * udp.c - UDP over IPv4 and IPv6.
 */

#include <kernel/errno.h>
#include <kernel/log.h>
#include <kernel/net/cksum.h>
#include <kernel/net/ether.h>
#include <kernel/net/udp.h>
#include <kernel/random.h>
#include <kernel/socket.h>
#include <kernel/string.h>

static LIST_HEAD(g_pcbs);
static spinlock_t g_lock = SPINLOCK_INIT("udp");
static struct udp_stats g_stats;
static uint16_t g_next_ephemeral = NET_EPHEMERAL_LO;

#define STAT(f) __atomic_fetch_add(&g_stats.f, 1, __ATOMIC_RELAXED)

void udp_init(void)
{
    g_next_ephemeral = (uint16_t)(NET_EPHEMERAL_LO + (random_u64() % (NET_EPHEMERAL_HI - NET_EPHEMERAL_LO)));
}

int udp_pcb_init(struct udp_pcb *pcb, uint16_t family)
{
    memset(pcb, 0, sizeof(*pcb));
    pcb->local.family = family;
    pcb->remote.family = family;
    mbufq_init(&pcb->rxq, UDP_RXQ_MAX, "udp-rxq");
    list_init(&pcb->link);
    return 0;
}

/* Lock held. A bound pcb using this local (address, port)? */
static bool port_in_use(uint16_t family, uint16_t port, const struct netaddr *addr)
{
    struct udp_pcb *p;
    list_for_each_entry(p, &g_pcbs, link) {
        if (p->local.family != family || p->local.port != port)
            continue;
        if (netaddr_is_unspecified(&p->local) || netaddr_is_unspecified(addr) || netaddr_addr_equal(&p->local, addr))
            return true;
    }
    return false;
}

static uint16_t pick_ephemeral(uint16_t family, const struct netaddr *addr)
{
    for (unsigned n = 0; n < NET_EPHEMERAL_HI - NET_EPHEMERAL_LO; n++) {
        uint16_t port = g_next_ephemeral;
        g_next_ephemeral = g_next_ephemeral >= NET_EPHEMERAL_HI ? (uint16_t)NET_EPHEMERAL_LO : (uint16_t)(g_next_ephemeral + 1);
        if (!port_in_use(family, port, addr))
            return port;
    }
    return 0;
}

int udp_bind(struct udp_pcb *pcb, const struct netaddr *local)
{
    if (local->family != pcb->local.family)
        return -EAFNOSUPPORT;
    if (!netaddr_is_unspecified(local)) {
        bool ours = local->family == COSMO_AF_INET ? netif_owns_ipv4(local->v4) : netif_owns_ipv6(&local->v6);
        if (!ours)
            return -EADDRNOTAVAIL;
    }
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    if (pcb->local.port != 0) {
        spin_unlock_irqrestore(&g_lock, s);
        return -EINVAL;   /* already bound */
    }
    uint16_t port = local->port;
    if (port == 0) {
        port = pick_ephemeral(local->family, local);
        if (port == 0) {
            spin_unlock_irqrestore(&g_lock, s);
            return -EADDRINUSE;
        }
    } else if (port_in_use(local->family, port, local)) {
        spin_unlock_irqrestore(&g_lock, s);
        return -EADDRINUSE;
    }
    pcb->local = *local;
    pcb->local.port = port;
    list_push_back(&g_pcbs, &pcb->link);
    spin_unlock_irqrestore(&g_lock, s);
    return 0;
}

void udp_unbind(struct udp_pcb *pcb)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    if (pcb->local.port != 0) {
        list_remove(&pcb->link);
        list_init(&pcb->link);
        pcb->local.port = 0;
    }
    spin_unlock_irqrestore(&g_lock, s);
    mbufq_drain(&pcb->rxq);
}

int udp_sendto(struct udp_pcb *pcb, const void *data, size_t len, const struct netaddr *to)
{
    if (to->family != pcb->local.family)
        return -EAFNOSUPPORT;
    if (to->port == 0 || netaddr_is_unspecified(to))
        return -EINVAL;
    if (len > 65535 - sizeof(struct udp_hdr))
        return -EMSGSIZE;
    if (pcb->local.port == 0) {
        struct netaddr any = { .family = pcb->local.family };
        int rc = udp_bind(pcb, &any);
        if (rc)
            return rc;
    }

    struct mbuf *m = m_getcl();
    if (m == NULL)
        return -ENOMEM;
    struct udp_hdr *uh = (struct udp_hdr *)m->data;
    uh->sport = htons(pcb->local.port);
    uh->dport = htons(to->port);
    uh->len = htons((uint16_t)(sizeof(*uh) + len));
    uh->cksum = 0;
    m->len = m->pkt.len = sizeof(*uh);
    int rc = m_append(m, data, (uint32_t)len);
    if (rc) {
        m_freem(m);
        return rc;
    }
    uint32_t sum;
    if (to->family == COSMO_AF_INET) {
        uint32_t src = netaddr_is_unspecified(&pcb->local) ? ipv4_source_for(to->v4) : pcb->local.v4;
        if (src == 0) {
            m_freem(m);
            return -ENETUNREACH;
        }
        sum = cksum_pseudo4(src, to->v4, IPPROTO_UDP, (uint16_t)(sizeof(*uh) + len));
        sum = m_cksum_partial(m, 0, m->pkt.len, sum);
        uh->cksum = cksum_fold(sum);
        if (uh->cksum == 0)
            uh->cksum = 0xffff;
        STAT(tx);
        return ipv4_output(m, src, to->v4, IPPROTO_UDP, IP_DEFAULT_TTL);
    }
    struct in6_addr src;
    if (netaddr_is_unspecified(&pcb->local))
        ipv6_source_for(&to->v6, &src);
    else
        src = pcb->local.v6;
    if (in6_is_unspecified(&src)) {
        m_freem(m);
        return -ENETUNREACH;
    }
    sum = cksum_pseudo6(&src, &to->v6, IPPROTO_UDP, (uint32_t)(sizeof(*uh) + len));
    sum = m_cksum_partial(m, 0, m->pkt.len, sum);
    uh->cksum = cksum_fold(sum);
    if (uh->cksum == 0)
        uh->cksum = 0xffff;
    STAT(tx);
    return ipv6_output(m, &src, &to->v6, IPPROTO_UDP, IP_DEFAULT_TTL);
}

struct mbuf *udp_recv(struct udp_pcb *pcb)
{
    return mbufq_dequeue(&pcb->rxq);
}

/* Lock held: the best pcb for a destination (exact address beats wildcard). */
static struct udp_pcb *lookup(uint16_t family, const struct netaddr *dst, const struct netaddr *src)
{
    struct udp_pcb *best = NULL;
    struct udp_pcb *p;
    list_for_each_entry(p, &g_pcbs, link) {
        if (p->local.family != family || p->local.port != dst->port)
            continue;
        if (!netaddr_is_unspecified(&p->local) && !netaddr_addr_equal(&p->local, dst))
            continue;
        if (p->remote.port != 0 && !netaddr_equal(&p->remote, src))
            continue;
        if (best == NULL || netaddr_is_unspecified(&best->local))
            best = p;
    }
    return best;
}

void udp_input(struct netif *nif, struct mbuf *m, const struct ipv4_hdr *ip4, const struct ipv6_hdr *ip6)
{
    (void)nif;
    STAT(rx);
    uint32_t len = m->pkt.len;
    if (len < sizeof(struct udp_hdr)) {
        STAT(rx_bad_len);
        m_freem(m);
        return;
    }
    m = m_pullup(m, sizeof(struct udp_hdr));
    if (m == NULL)
        return;
    const struct udp_hdr *uh = (const struct udp_hdr *)m->data;
    uint16_t ulen = ntohs(uh->len);
    if (ulen < sizeof(*uh) || ulen > len) {
        STAT(rx_bad_len);
        m_freem(m);
        return;
    }
    if (ulen < len)
        m_adj(m, -(int)(len - ulen));

    struct netaddr src, dst;
    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));
    uint32_t sum = 0;
    if (ip4) {
        src.family = dst.family = COSMO_AF_INET;
        src.v4 = ip4->src;
        dst.v4 = ip4->dst;
        if (uh->cksum != 0)
            sum = cksum_pseudo4(ip4->src, ip4->dst, IPPROTO_UDP, ulen);
    } else {
        src.family = dst.family = COSMO_AF_INET6;
        src.v6 = ip6->src;
        dst.v6 = ip6->dst;
        if (uh->cksum == 0) {
            STAT(rx_bad_cksum);   /* mandatory on IPv6 */
            m_freem(m);
            return;
        }
        sum = cksum_pseudo6(&ip6->src, &ip6->dst, IPPROTO_UDP, ulen);
    }
    if (uh->cksum != 0 && cksum_fold(m_cksum_partial(m, 0, ulen, sum)) != 0) {
        STAT(rx_bad_cksum);
        m_freem(m);
        return;
    }
    src.port = ntohs(uh->sport);
    dst.port = ntohs(uh->dport);
    m_adj(m, (int)sizeof(*uh));
    m->pkt.src = src;

    arch_irq_state_t s = spin_lock_irqsave(&g_lock);
    struct udp_pcb *pcb = lookup(dst.family, &dst, &src);
    struct socket *sock = NULL;
    bool queued = false;
    if (pcb) {
        queued = mbufq_enqueue(&pcb->rxq, m);
        if (!queued) {
            pcb->rx_dropped++;
            STAT(rx_queue_full);
        }
        /* The wake happens after the unlock; hold the socket across it.
         * Its release clears pcb->sock under g_lock but starts at count
         * zero, so only a tryget is safe here (design.md, "UDP"). */
        if (queued && pcb->sock && kobject_tryget(&pcb->sock->obj))
            sock = pcb->sock;
    }
    spin_unlock_irqrestore(&g_lock, s);
    if (pcb == NULL) {
        STAT(rx_no_port);
        if (ip4 && !(m->flags & M_BCAST)) {
            struct mbuf *q = m_prepend(m, (uint32_t)sizeof(*uh) + IPV4_HDR_LEN(ip4));
            if (q) {
                memcpy(q->data, ip4, IPV4_HDR_LEN(ip4));
                icmp_send_unreach(q, ip4, ICMP_UNREACH_PORT);
                m_freem(q);
            }
        } else {
            m_freem(m);
        }
        return;
    }
    if (sock) {
        sock_wake(sock);
        ksock_put(sock);
    }
}

void udp_get_stats(struct udp_stats *out)
{
    *out = g_stats;
}
