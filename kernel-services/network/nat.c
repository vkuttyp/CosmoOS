/*
 * nat.c - Masquerade (source) NAT over a bounded conntrack table.
 *
 * A guest datagram forwarded out an uplink has its private source rewritten
 * to the uplink's address and its transport identifier (TCP/UDP port, ICMP
 * echo id) rewritten to a value the NAT owns; the reply, arriving for that
 * value, is rewritten back and forwarded to the guest. The transport
 * checksum is fixed up incrementally (RFC 1624) as the pseudo-header and
 * identifier change; the IP header checksum is recomputed downstream when
 * ipv4_output / output_on rebuild the header.
 *
 * See docs/audit/next-subsystem-nat.md. Only IPv4 UDP, TCP and ICMP echo
 * are handled; the table is bounded (NAT_TABLE_SIZE) and its entries expire.
 * The transport headers are accessed by byte offset (they are __packed, so
 * a pointer to a member could be unaligned): the identifier and checksum
 * live at fixed offsets -- UDP src 0/cksum 6, TCP src 0/cksum 16, ICMP
 * cksum 2/id 4.
 */

#include <kernel/errno.h>
#include <kernel/log.h>
#include <kernel/net/cksum.h>
#include <kernel/net/inet.h>
#include <kernel/net/ip.h>
#include <kernel/net/nat.h>
#include <kernel/net/tcp.h>
#include <kernel/net/udp.h>
#include <kernel/netif.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/timer.h>

struct nat_entry {
    bool     in_use;
    bool     tcp_est;         /* a non-SYN segment has passed: the longer timeout */
    uint8_t  proto;           /* IPPROTO_UDP / TCP / ICMP */
    uint16_t orig_port;       /* the guest's source port, or ICMP echo id (host order) */
    uint16_t nat_port;        /* the value we lend it on the uplink (host order) */
    uint32_t orig_ip;         /* the guest (network order) */
    uint32_t nat_ip;          /* the uplink address we masquerade as (network order) */
    uint32_t peer_ip;         /* the far side (network order) */
    uint16_t peer_port;       /* the far side's port (host order); 0 for ICMP */
    uint64_t expires_ns;
};

static struct nat_entry g_nat[NAT_TABLE_SIZE];
static spinlock_t g_nat_lock = SPINLOCK_INIT("nat");
static struct nat_stats g_stats;
static uint16_t g_port_next = NAT_PORT_MIN;

#define STAT(f) __atomic_fetch_add(&g_stats.f, 1, __ATOMIC_RELAXED)

/* Read / write a 16-bit field at a byte offset, unaligned-safe, keeping the
 * value in network order (as stored). */
static inline uint16_t get16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }

/* --- incremental checksum (RFC 1624) -------------------------------------- */

/* Update the ones-complement checksum stored (network order) at *sum for a
 * 16-bit word changing wold->wnew, both HOST order (RFC 1624). The stored
 * field is read with ntohs and written with htons; the running sum treats
 * the packet as big-endian 16-bit words, as cksum_partial does. */
static void csum_patch16(uint16_t *sum, uint16_t wold, uint16_t wnew)
{
    uint32_t x = (uint16_t)~ntohs(*sum);
    x += (uint16_t)~wold;
    x += wnew;
    while (x >> 16)
        x = (x & 0xffff) + (x >> 16);
    *sum = htons((uint16_t)~x);
}

/* The same for a 32-bit field (two big-endian words); old/neu HOST order. */
static void csum_patch32(uint16_t *sum, uint32_t old, uint32_t neu)
{
    csum_patch16(sum, (uint16_t)(old >> 16), (uint16_t)(neu >> 16));
    csum_patch16(sum, (uint16_t)(old & 0xffff), (uint16_t)(neu & 0xffff));
}

/* --- table helpers (caller holds g_nat_lock) ------------------------------ */

static uint64_t nat_timeout(const struct nat_entry *e)
{
    if (e->proto == IPPROTO_TCP)
        return e->tcp_est ? NAT_TIMEOUT_TCPEST_NS : NAT_TIMEOUT_TCP_NS;
    return e->proto == IPPROTO_UDP ? NAT_TIMEOUT_UDP_NS : NAT_TIMEOUT_ICMP_NS;
}

static bool nat_expired(const struct nat_entry *e, uint64_t now)
{
    return now >= e->expires_ns;
}

/* An outbound flow's entry (the guest's five-tuple), or NULL. */
static struct nat_entry *nat_find_out(uint8_t proto, uint32_t orig_ip, uint16_t orig_port,
                                      uint32_t peer_ip, uint16_t peer_port, uint64_t now)
{
    for (unsigned i = 0; i < NAT_TABLE_SIZE; i++) {
        struct nat_entry *e = &g_nat[i];
        if (!e->in_use || nat_expired(e, now))
            continue;
        if (e->proto == proto && e->orig_ip == orig_ip && e->orig_port == orig_port &&
            e->peer_ip == peer_ip && e->peer_port == peer_port)
            return e;
    }
    return NULL;
}

/* The reply's entry: keyed by what a reply carries (our address and lent
 * identifier, the far side as source). NULL if none. */
static struct nat_entry *nat_find_reply(uint8_t proto, uint32_t nat_ip, uint16_t nat_port,
                                        uint32_t peer_ip, uint16_t peer_port, uint64_t now)
{
    for (unsigned i = 0; i < NAT_TABLE_SIZE; i++) {
        struct nat_entry *e = &g_nat[i];
        if (!e->in_use || nat_expired(e, now))
            continue;
        if (e->proto == proto && e->nat_ip == nat_ip && e->nat_port == nat_port &&
            e->peer_ip == peer_ip && e->peer_port == peer_port)
            return e;
    }
    return NULL;
}

/* True if some live entry already lends this (proto, nat_ip, nat_port). */
static bool nat_port_taken(uint8_t proto, uint32_t nat_ip, uint16_t nat_port, uint64_t now)
{
    for (unsigned i = 0; i < NAT_TABLE_SIZE; i++) {
        struct nat_entry *e = &g_nat[i];
        if (e->in_use && !nat_expired(e, now) &&
            e->proto == proto && e->nat_ip == nat_ip && e->nat_port == nat_port)
            return true;
    }
    return false;
}

/* Allocate a free entry and lend a NAT identifier; NULL when the table is
 * full or no identifier is free. Prefers the guest's own port when it is
 * free (helps protocols that assume it). */
static struct nat_entry *nat_alloc(uint8_t proto, uint32_t nat_ip, uint16_t want, uint64_t now)
{
    struct nat_entry *slot = NULL;
    for (unsigned i = 0; i < NAT_TABLE_SIZE; i++) {
        if (!g_nat[i].in_use || nat_expired(&g_nat[i], now)) {
            slot = &g_nat[i];
            break;
        }
    }
    if (slot == NULL) {
        STAT(out_drop_full);
        return NULL;
    }
    uint16_t port = 0;
    if (want >= NAT_PORT_MIN && want <= NAT_PORT_MAX && !nat_port_taken(proto, nat_ip, want, now))
        port = want;
    for (unsigned tries = 0; port == 0 && tries < (NAT_PORT_MAX - NAT_PORT_MIN + 1); tries++) {
        uint16_t cand = g_port_next++;
        if (g_port_next > NAT_PORT_MAX)
            g_port_next = NAT_PORT_MIN;
        if (cand < NAT_PORT_MIN)
            cand = NAT_PORT_MIN;
        if (!nat_port_taken(proto, nat_ip, cand, now))
            port = cand;
    }
    if (port == 0) {
        STAT(out_drop_noport);
        return NULL;
    }
    if (slot->in_use)
        STAT(expired);            /* we are reclaiming an expired slot */
    memset(slot, 0, sizeof(*slot));
    slot->in_use = true;
    slot->proto = proto;
    slot->nat_ip = nat_ip;
    slot->nat_port = port;
    return slot;
}

/* --- outbound ------------------------------------------------------------- */

int nat_out(struct netif *in, struct netif *out, struct mbuf *m,
            const struct ipv4_hdr *iph, unsigned ihl, uint32_t *new_src)
{
    *new_src = iph->src;

    /* Masquerade only flows forwarded from a NETIF_MASQUERADE interface that
     * leave an interface whose subnet does not already hold the source. */
    bool on_egress = out->ip4.addr && out->ip4.mask &&
                     ((iph->src ^ out->ip4.addr) & out->ip4.mask) == 0;
    if (!(in->flags & NETIF_MASQUERADE) || on_egress || out->ip4.addr == 0)
        return 0;

    uint8_t proto = iph->proto;
    uint8_t *l4 = m->data + ihl;
    uint16_t orig_port, peer_port;
    unsigned foff, coff;      /* identifier and checksum offsets within l4 */
    bool icmp = false, est = false;

    if (proto == IPPROTO_UDP) {
        orig_port = ntohs(get16(l4 + 0));
        peer_port = ntohs(get16(l4 + 2));
        foff = 0; coff = 6;
    } else if (proto == IPPROTO_TCP) {
        orig_port = ntohs(get16(l4 + 0));
        peer_port = ntohs(get16(l4 + 2));
        foff = 0; coff = 16;
        est = (l4[13] & TH_ACK) && !(l4[13] & TH_SYN);
    } else if (proto == IPPROTO_ICMP) {
        if (l4[0] != ICMP_ECHO)
            return -ENOTSUP;             /* only echo is masqueraded */
        orig_port = ntohs(get16(l4 + 4));
        peer_port = 0;
        foff = 4; coff = 2;
        icmp = true;
    } else {
        return -ENOTSUP;
    }

    uint64_t now = clock_now_ns();
    arch_irq_state_t s = spin_lock_irqsave(&g_nat_lock);
    struct nat_entry *e = nat_find_out(proto, iph->src, orig_port, iph->dst, peer_port, now);
    bool fresh = false;
    if (e == NULL) {
        e = nat_alloc(proto, out->ip4.addr, orig_port, now);
        if (e == NULL) {
            spin_unlock_irqrestore(&g_nat_lock, s);
            return -ENOSPC;
        }
        e->orig_ip = iph->src;
        e->orig_port = orig_port;
        e->peer_ip = iph->dst;
        e->peer_port = peer_port;
        fresh = true;
    }
    if (est)
        e->tcp_est = true;
    uint16_t nat_port = e->nat_port;
    e->expires_ns = now + nat_timeout(e);
    spin_unlock_irqrestore(&g_nat_lock, s);
    if (fresh)
        STAT(out_new);
    else
        STAT(out_reuse);

    /* Rewrite the identifier and, for the pseudo-header protocols, the
     * source address into the transport checksum, then the fields. ICMP has
     * no pseudo-header, so only its id enters its checksum; a UDP datagram
     * with a zero (absent) checksum is left unchecked. */
    uint16_t ck = get16(l4 + coff);
    bool udp_nock = (proto == IPPROTO_UDP && ck == 0);
    if (!udp_nock) {
        if (!icmp)
            csum_patch32(&ck, ntohl(iph->src), ntohl(out->ip4.addr));
        csum_patch16(&ck, orig_port, nat_port);
        put16(l4 + coff, ck);
    }
    put16(l4 + foff, htons(nat_port));
    *new_src = out->ip4.addr;
    return 0;
}

/* --- inbound -------------------------------------------------------------- */

/* Strip the L3 header and forward the translated datagram to `dst`, from
 * `src`, TTL decremented. Consumes m. */
static void nat_forward_to(struct mbuf *m, unsigned ihl, uint16_t total,
                           uint32_t src, uint32_t dst, uint8_t proto, uint8_t ttl)
{
    if (ttl <= 1) {                 /* would expire in transit: drop the reply */
        m_freem(m);
        return;
    }
    if (total < m->pkt.len)
        m_adj(m, -(int)(m->pkt.len - total));
    m_adj(m, (int)ihl);
    ipv4_output(m, src, dst, proto, (uint8_t)(ttl - 1));
}

/* Translate an ICMP error (type 3/11) whose quoted datagram is one we
 * masqueraded, back to the guest. m holds the whole outer IP header;
 * ihl/total describe it. Returns true (m consumed) when handled. */
static bool nat_in_icmp_error(struct mbuf *m, unsigned ihl, uint16_t total)
{
    uint32_t icmp_off = ihl;
    uint8_t hdr[sizeof(struct icmp_hdr) + sizeof(struct ipv4_hdr) + 8];
    if (!m_copydata(m, icmp_off, sizeof(hdr), hdr))
        return false;
    const struct ipv4_hdr *qiph = (const struct ipv4_hdr *)(hdr + sizeof(struct icmp_hdr));
    unsigned qihl = IPV4_HDR_LEN(qiph);
    if ((qiph->vhl >> 4) != 4 || qihl < 20)
        return false;
    /* The quote is the packet we sent (src = our nat_ip). Its L4 source
     * identifier is the value we lent. */
    uint8_t ql4[8];
    if (!m_copydata(m, icmp_off + sizeof(struct icmp_hdr) + qihl, 8, ql4))
        return false;
    uint8_t qproto = qiph->proto;
    uint16_t nat_id, peer_port;
    if (qproto == IPPROTO_ICMP) {
        nat_id = (uint16_t)(ql4[4] << 8 | ql4[5]);      /* echo id */
        peer_port = 0;
    } else {
        nat_id = (uint16_t)(ql4[0] << 8 | ql4[1]);      /* our lent source port */
        peer_port = (uint16_t)(ql4[2] << 8 | ql4[3]);   /* the far side's port */
    }

    uint64_t now = clock_now_ns();
    arch_irq_state_t s = spin_lock_irqsave(&g_nat_lock);
    struct nat_entry *e = nat_find_reply(qproto, qiph->src, nat_id, qiph->dst, peer_port, now);
    struct nat_entry snap;
    if (e)
        snap = *e;
    spin_unlock_irqrestore(&g_nat_lock, s);
    if (e == NULL)
        return false;

    /* Rewrite the quoted datagram so the guest recognises its own packet:
     * inner src (our nat_ip) -> the guest, inner L4 source id -> the guest's;
     * recompute the inner IP checksum and the outer ICMP checksum. */
    m = m_pullup(m, icmp_off + sizeof(struct icmp_hdr) + qihl + 8);
    if (m == NULL)
        return true;   /* we own it now; drop on failure */
    uint8_t *base = m->data;
    struct icmp_hdr *oic = (struct icmp_hdr *)(base + icmp_off);
    struct ipv4_hdr *inq = (struct ipv4_hdr *)(base + icmp_off + sizeof(struct icmp_hdr));
    uint8_t *inl4 = base + icmp_off + sizeof(struct icmp_hdr) + qihl;

    inq->src = snap.orig_ip;
    inq->cksum = 0;
    inq->cksum = in_cksum(inq, qihl);
    if (qproto == IPPROTO_ICMP) {
        inl4[4] = (uint8_t)(snap.orig_port >> 8);
        inl4[5] = (uint8_t)snap.orig_port;
    } else {
        inl4[0] = (uint8_t)(snap.orig_port >> 8);
        inl4[1] = (uint8_t)snap.orig_port;
    }
    uint16_t icmp_len = (uint16_t)(total - ihl);
    oic->cksum = 0;
    oic->cksum = in_cksum(oic, icmp_len);

    struct ipv4_hdr *outer = (struct ipv4_hdr *)base;
    STAT(in_icmp_error);
    STAT(in_translated);
    nat_forward_to(m, ihl, total, outer->src, snap.orig_ip, IPPROTO_ICMP, outer->ttl);
    return true;
}

bool nat_in(struct netif *nif, struct mbuf *m,
            const struct ipv4_hdr *iph, unsigned ihl, uint16_t total)
{
    (void)nif;
    uint8_t proto = iph->proto;
    if (proto != IPPROTO_UDP && proto != IPPROTO_TCP && proto != IPPROTO_ICMP)
        return false;

    uint8_t l4[8];
    if (!m_copydata(m, ihl, sizeof(l4), l4))
        return false;

    /* Decide without disturbing m (read-only via the copy), so a non-NAT
     * packet falls through to normal delivery untouched. */
    if (proto == IPPROTO_ICMP && (l4[0] == ICMP_DEST_UNREACH || l4[0] == ICMP_TIME_EXCEEDED))
        return nat_in_icmp_error(m, ihl, total);

    uint16_t nat_port, peer_port;
    if (proto == IPPROTO_ICMP) {
        if (l4[0] != ICMP_ECHO_REPLY)
            return false;
        nat_port = (uint16_t)(l4[4] << 8 | l4[5]);    /* echo id */
        peer_port = 0;
    } else {
        peer_port = (uint16_t)(l4[0] << 8 | l4[1]);   /* reply's source port */
        nat_port = (uint16_t)(l4[2] << 8 | l4[3]);    /* reply's dest port = the lent id */
    }

    uint64_t now = clock_now_ns();
    arch_irq_state_t s = spin_lock_irqsave(&g_nat_lock);
    struct nat_entry *e = nat_find_reply(proto, iph->dst, nat_port, iph->src, peer_port, now);
    struct nat_entry snap;
    if (e) {
        if (proto == IPPROTO_TCP)
            e->tcp_est = true;
        e->expires_ns = now + nat_timeout(e);
        snap = *e;
    }
    spin_unlock_irqrestore(&g_nat_lock, s);
    if (e == NULL) {
        STAT(in_no_match);
        return false;
    }

    /* Commit: pull up the transport header and rewrite the destination back
     * to the guest, fixing the transport checksum incrementally. */
    unsigned l4min = proto == IPPROTO_TCP ? 20u : 8u;
    m = m_pullup(m, ihl + l4min);
    if (m == NULL)
        return true;              /* we own it; dropped */
    struct ipv4_hdr *miph = (struct ipv4_hdr *)m->data;
    uint8_t *mp = m->data + ihl;
    uint32_t old_dst_h = ntohl(miph->dst);
    uint32_t new_dst_h = ntohl(snap.orig_ip);
    uint16_t new_port = htons(snap.orig_port);

    if (proto == IPPROTO_UDP) {
        uint16_t ck = get16(mp + 6);
        if (ck != 0) {
            csum_patch32(&ck, old_dst_h, new_dst_h);
            csum_patch16(&ck, ntohs(get16(mp + 2)), snap.orig_port);
            put16(mp + 6, ck);
        }
        put16(mp + 2, new_port);                /* dport */
    } else if (proto == IPPROTO_TCP) {
        uint16_t ck = get16(mp + 16);
        csum_patch32(&ck, old_dst_h, new_dst_h);
        csum_patch16(&ck, ntohs(get16(mp + 2)), snap.orig_port);
        put16(mp + 16, ck);
        put16(mp + 2, new_port);
    } else {                                    /* ICMP echo reply: id at +4, cksum at +2 */
        uint16_t ck = get16(mp + 2);
        csum_patch16(&ck, ntohs(get16(mp + 4)), snap.orig_port);
        put16(mp + 2, ck);
        put16(mp + 4, new_port);
    }

    uint32_t peer = miph->src;
    uint8_t ttl = miph->ttl;
    STAT(in_translated);
    nat_forward_to(m, ihl, total, peer, snap.orig_ip, proto, ttl);
    return true;
}

/* --- maintenance ---------------------------------------------------------- */

void nat_age(uint64_t now_ns)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_nat_lock);
    for (unsigned i = 0; i < NAT_TABLE_SIZE; i++) {
        if (g_nat[i].in_use && nat_expired(&g_nat[i], now_ns)) {
            g_nat[i].in_use = false;
            STAT(expired);
        }
    }
    spin_unlock_irqrestore(&g_nat_lock, s);
}

void nat_flush(void)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_nat_lock);
    memset(g_nat, 0, sizeof(g_nat));
    spin_unlock_irqrestore(&g_nat_lock, s);
}

void nat_get_stats(struct nat_stats *out)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_nat_lock);
    uint32_t live = 0;
    uint64_t now = clock_now_ns();
    for (unsigned i = 0; i < NAT_TABLE_SIZE; i++)
        if (g_nat[i].in_use && !nat_expired(&g_nat[i], now))
            live++;
    *out = g_stats;
    out->entries = live;
    spin_unlock_irqrestore(&g_nat_lock, s);
}
