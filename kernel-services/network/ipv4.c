/*
 * ipv4.c - IPv4 (RFC 791) input, output and route selection, ICMP echo /
 * destination unreachable (RFC 792) behind a rate limit, and path MTU
 * discovery (RFC 1191) from "fragmentation needed" messages.
 */

#include <kernel/errno.h>
#include <kernel/log.h>
#include <kernel/net/cksum.h>
#include <kernel/net/ether.h>
#include <kernel/net/ip.h>
#include <kernel/net/udp.h>
#include <kernel/net/tcp.h>
#include <kernel/random.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/timer.h>

static struct ip_stats g_stats;
static uint16_t g_ip_id;

#define STAT(f) __atomic_fetch_add(&g_stats.f, 1, __ATOMIC_RELAXED)

/* --- ICMP rate limit and the path MTU cache -------------------------------------- */

static spinlock_t g_icmp_lock = SPINLOCK_INIT("icmp-rate");
static uint64_t g_icmp_window_ns;
static unsigned g_icmp_count;

bool icmp_ratelimit_allow(void)
{
    uint64_t now = clock_now_ns();
    arch_irq_state_t s = spin_lock_irqsave(&g_icmp_lock);
    if (now - g_icmp_window_ns >= 1000000000ull) {
        g_icmp_window_ns = now;
        g_icmp_count = 0;
    }
    bool ok = g_icmp_count < ICMP_RATE_PER_SEC;
    if (ok)
        g_icmp_count++;
    spin_unlock_irqrestore(&g_icmp_lock, s);
    if (!ok)
        STAT(icmp_ratelimited);
    return ok;
}

struct pmtu_entry {
    uint32_t dst;
    uint32_t mtu;
    uint64_t expires_ns;   /* 0 = free */
};

static spinlock_t g_pmtu_lock = SPINLOCK_INIT("ipv4-pmtu");
static struct pmtu_entry g_pmtu[IPV4_PMTU_ENTRIES];

static uint32_t pmtu_lookup(uint32_t dst)
{
    uint64_t now = clock_now_ns();
    uint32_t mtu = 0;
    arch_irq_state_t s = spin_lock_irqsave(&g_pmtu_lock);
    for (unsigned i = 0; i < IPV4_PMTU_ENTRIES; i++) {
        if (g_pmtu[i].expires_ns && g_pmtu[i].dst == dst) {
            if (now < g_pmtu[i].expires_ns)
                mtu = g_pmtu[i].mtu;
            else
                g_pmtu[i].expires_ns = 0;
            break;
        }
    }
    spin_unlock_irqrestore(&g_pmtu_lock, s);
    return mtu;
}

void ipv4_pmtu_update(uint32_t dst, uint32_t mtu)
{
    if (mtu < IPV4_PMTU_MIN)
        mtu = IPV4_PMTU_MIN;
    uint64_t now = clock_now_ns();
    arch_irq_state_t s = spin_lock_irqsave(&g_pmtu_lock);
    struct pmtu_entry *slot = NULL, *oldest = &g_pmtu[0];
    for (unsigned i = 0; i < IPV4_PMTU_ENTRIES; i++) {
        struct pmtu_entry *e = &g_pmtu[i];
        if (e->expires_ns && e->dst == dst) {
            slot = e;
            break;
        }
        if (e->expires_ns == 0 || now >= e->expires_ns) {
            if (slot == NULL)
                slot = e;
        } else if (e->expires_ns < oldest->expires_ns) {
            oldest = e;
        }
    }
    if (slot == NULL)
        slot = oldest;
    if (slot->expires_ns == 0 || slot->dst != dst || now >= slot->expires_ns || mtu < slot->mtu)
        slot->mtu = mtu;
    slot->dst = dst;
    slot->expires_ns = now + IPV4_PMTU_TTL_NS;
    spin_unlock_irqrestore(&g_pmtu_lock, s);
    STAT(pmtu_updates);
}

void ipv4_pmtu_flush(void)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_pmtu_lock);
    memset(g_pmtu, 0, sizeof(g_pmtu));
    spin_unlock_irqrestore(&g_pmtu_lock, s);
}

uint32_t ipv4_path_mtu(uint32_t dst)
{
    uint32_t mtu = pmtu_lookup(dst);
    if (mtu)
        return mtu;
    struct netif *nif = ipv4_route(dst);
    if (nif == NULL)
        return IPV4_PMTU_MIN;
    mtu = nif->mtu;
    netif_put(nif);
    return mtu;
}

/* RFC 1191 plateau table for routers that report no MTU. */
static uint32_t pmtu_plateau_below(uint32_t len)
{
    static const uint32_t plateaus[] = { 65535, 32000, 17914, 8166, 4352, 2002, 1492, 1006, 508, 296, 68 };
    for (unsigned i = 0; i < ARRAY_SIZE(plateaus); i++)
        if (plateaus[i] < len)
            return plateaus[i];
    return 68;
}

/* --- routing ------------------------------------------------------------ */

/* Referenced interface (netif_put when done) or NULL. */
struct netif *ipv4_route(uint32_t dst)
{
    if ((ntohl(dst) >> 24) == 127 || netif_owns_ipv4(dst))
        return netif_loopback();
    struct netif *nif = netif_default();
    if (nif == NULL)
        return NULL;
    if (nif->ip4.addr == 0) {
        netif_put(nif);
        return NULL;
    }
    return nif;
}

uint32_t ipv4_source_for(uint32_t dst)
{
    struct netif *nif = ipv4_route(dst);
    if (nif == NULL)
        return 0;
    uint32_t src;
    if (nif->flags & NETIF_LOOPBACK)
        src = netif_owns_ipv4(dst) ? dst : INADDR_LOOPBACK_N;
    else
        src = nif->ip4.addr;
    netif_put(nif);
    return src;
}

/* --- output --------------------------------------------------------------- */

static int output_on(struct netif *nif, struct mbuf *m, uint32_t src, uint32_t dst, uint8_t proto, uint8_t ttl);

int ipv4_output(struct mbuf *m, uint32_t src, uint32_t dst, uint8_t proto, uint8_t ttl)
{
    struct netif *nif = ipv4_route(dst);
    if (nif == NULL) {
        STAT(tx_no_route);
        m_freem(m);
        return -ENETUNREACH;
    }
    int rc = output_on(nif, m, src, dst, proto, ttl);
    netif_put(nif);
    return rc;
}

static int output_on(struct netif *nif, struct mbuf *m, uint32_t src, uint32_t dst, uint8_t proto, uint8_t ttl)
{
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
    if (!icmp_ratelimit_allow())
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

/*
 * Fragmentation needed (RFC 1191): the quoted datagram names the
 * destination and, for TCP, the connection. Record the MTU and let TCP
 * shrink and retransmit. Only a message quoting our own address as the
 * source is considered; the TCP layer checks the quoted sequence number.
 */
static void icmp_needfrag(struct mbuf *m, const struct icmp_hdr *ic)
{
    STAT(icmp_needfrag_rcvd);
    uint8_t quote[sizeof(struct ipv4_hdr) + 8];
    if (!m_copydata(m, sizeof(*ic), sizeof(quote), quote))
        return;
    const struct ipv4_hdr *q = (const struct ipv4_hdr *)quote;
    unsigned ihl = IPV4_HDR_LEN(q);
    if ((q->vhl >> 4) != 4 || ihl < 20 || !netif_owns_ipv4(q->src))
        return;
    uint32_t mtu = ntohs(ic->seq);   /* the header's last 16 bits */
    if (mtu == 0)
        mtu = pmtu_plateau_below(ntohs(q->len));
    if (mtu < IPV4_PMTU_MIN)
        mtu = IPV4_PMTU_MIN;
    ipv4_pmtu_update(q->dst, mtu);
    if (q->proto != IPPROTO_TCP)
        return;
    uint8_t th[8];
    if (!m_copydata(m, sizeof(*ic) + ihl, sizeof(th), th))
        return;
    struct netaddr local, remote;
    memset(&local, 0, sizeof(local));
    memset(&remote, 0, sizeof(remote));
    local.family = remote.family = COSMO_AF_INET;
    local.v4 = q->src;
    remote.v4 = q->dst;
    local.port = (uint16_t)(th[0] << 8 | th[1]);
    remote.port = (uint16_t)(th[2] << 8 | th[3]);
    uint32_t seq = (uint32_t)th[4] << 24 | (uint32_t)th[5] << 16 | (uint32_t)th[6] << 8 | th[7];
    tcp_pmtu_notify(&local, &remote, seq, (uint16_t)(mtu > 65535 ? 65535 : mtu));
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
    if (ic->type == ICMP_DEST_UNREACH && ic->code == ICMP_UNREACH_NEEDFRAG) {
        icmp_needfrag(m, ic);
        m_freem(m);
        return;
    }
    if (ic->type == ICMP_ECHO && ic->code == 0 && !(m->flags & M_BCAST)) {
        STAT(icmp_echo_rcvd);
        if (!icmp_ratelimit_allow()) {
            m_freem(m);
            return;
        }
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

    /* Trim link padding, drop the header, deliver. The whole header,
     * options included, is copied out (60 bytes at most) so that an ICMP
     * error can quote exactly what arrived and never a byte beyond it. */
    if (total < m->pkt.len)
        m_adj(m, -(int)(m->pkt.len - total));
    uint8_t hdrbuf[60];
    memcpy(hdrbuf, iph, ihl);
    const struct ipv4_hdr *hdr = (const struct ipv4_hdr *)hdrbuf;
    m_adj(m, (int)ihl);

    switch (hdr->proto) {
    case IPPROTO_ICMP: icmp_input(nif, m, hdr); break;
    case IPPROTO_UDP:  udp_input(nif, m, hdr, NULL); break;
    case IPPROTO_TCP:  tcp_input(nif, m, hdr, NULL); break;
    default:
        STAT(rx_unknown_proto);
        if (!bcast) {
            /* Re-quote the header for the ICMP error. */
            struct mbuf *q = m_prepend(m, (uint32_t)ihl);
            if (q) {
                memcpy(q->data, hdrbuf, ihl);
                icmp_send_unreach(q, hdr, ICMP_UNREACH_PROTO);
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
