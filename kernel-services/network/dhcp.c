/*
 * dhcp.c - The kernel's DHCP client (kernel/net/dhcp.h; design.md, "The DHCP
 * client").
 *
 * One client per autoconfigured interface, in a small static table. Its
 * state machine runs in two places, both on the network workers: the input
 * path (a reply, claimed by ipv4_input through dhcp_client_input) and the
 * client's deferred work (its timer, which only queues the work -- timers
 * never send). Both take the client's lock, decide, and leave what to send
 * in a struct dhcp_tx; the message is built and transmitted after the lock
 * is dropped, since no lock is held across a transmit.
 *
 * Messages are built at the frame level, as the tap's server builds its
 * replies: a DISCOVER or a REQUEST before the ACK comes from 0.0.0.0 to the
 * limited broadcast, which IP routing cannot carry (ipv4_route has no
 * address to pick a source from, and sends 255.255.255.255 to the default
 * interface, not this one). A renewal is unicast from the leased address to
 * the server, its link address resolved by ARP like any datagram's.
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/mbuf.h>
#include <kernel/module.h>
#include <kernel/net/cksum.h>
#include <kernel/net/dhcp.h>
#include <kernel/net/ether.h>
#include <kernel/net/inet.h>
#include <kernel/net/ip.h>
#include <kernel/net/udp.h>
#include <kernel/netif.h>
#include <kernel/random.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/timer.h>
#include <kernel/wait.h>

#define NS_PER_S 1000000000ull

struct dhcp_client {
    struct netif *nif;             /* referenced while the slot is in use */
    bool in_use;
    bool stopping;                 /* dhcp_client_stop: no more sends, no re-arm */
    enum dhcp_state state;
    uint32_t xid;
    unsigned tries;                /* retransmissions of the current message */
    uint64_t next_ns;              /* the current message's retransmission */
    uint64_t start_ns;             /* the exchange's first message (the secs field) */
    uint64_t req_ns;               /* its first REQUEST: what the lease's times count from (RFC 2131 §4.4.1) */
    /* The offer being requested, then the lease. */
    uint32_t offer_addr, server;
    uint32_t addr, mask, router;
    uint32_t dns[DHCP_MAX_DNS];
    uint8_t ndns;
    uint32_t lease_s, t1_s, t2_s;
    uint64_t t1_at, t2_at, end_at; /* absolute, clock_now_ns */
    struct timer timer;
    struct net_work work;
    spinlock_t lock;
};

/*
 * A slot's lock is set up once and never again: a caller that found the
 * slot by its interface (client_of) and is about to lock it must find a
 * lock, whatever happened to the slot meanwhile. Everything else is reset
 * under that lock, and every user checks under it that the slot is still
 * the client of the interface it looked for (lock_client).
 */
static struct dhcp_client g_clients[DHCP_MAX_CLIENTS];
static bool g_slots_ready;
static spinlock_t g_table_lock = SPINLOCK_INIT("dhcp-table");
static struct dhcp_stats g_stats;
#define STAT(f) __atomic_fetch_add(&g_stats.f, 1, __ATOMIC_RELAXED)

/* What one decision sends, built after the lock is dropped. */
struct dhcp_tx {
    uint8_t type;                  /* 0: nothing */
    bool unicast;                  /* to `server` from `ciaddr` (renewing); else 0.0.0.0 -> broadcast */
    uint32_t xid, ciaddr, reqip, server_opt, server;
    uint16_t secs;
};

static uint64_t backoff_ns(unsigned tries)
{
    uint64_t d = DHCP_RETX_FIRST_NS << (tries < 6 ? tries : 6);
    return d > DHCP_RETX_MAX_NS ? DHCP_RETX_MAX_NS : d;
}

static struct dhcp_client *client_of(const struct netif *nif)
{
    if (!__atomic_load_n(&g_slots_ready, __ATOMIC_ACQUIRE))
        return NULL;
    for (unsigned i = 0; i < DHCP_MAX_CLIENTS; i++)
        if (__atomic_load_n(&g_clients[i].in_use, __ATOMIC_ACQUIRE) && g_clients[i].nif == nif)
            return &g_clients[i];
    return NULL;
}

/* The interface's client, locked, or NULL: found, then confirmed under its
 * own lock to still be this interface's and not stopping -- a slot can be
 * stopped and handed to another interface between the two. */
static struct dhcp_client *lock_client(const struct netif *nif, arch_irq_state_t *s)
{
    struct dhcp_client *c = client_of(nif);
    if (c == NULL)
        return NULL;
    *s = spin_lock_irqsave(&c->lock);
    if (c->in_use && c->nif == nif && !c->stopping)
        return c;
    spin_unlock_irqrestore(&c->lock, *s);
    return NULL;
}

static uint16_t secs_since(const struct dhcp_client *c, uint64_t now)
{
    /* clock_now_ns is comparable across CPUs to within the measured skew,
     * so a reading on another worker may be a hair earlier: that is 0. */
    uint64_t s = now > c->start_ns ? (now - c->start_ns) / NS_PER_S : 0;
    return s > 0xffff ? 0xffff : (uint16_t)s;
}

/* --- sending ----------------------------------------------------------------------- */

static uint8_t *put_opt(uint8_t *p, uint8_t code, uint8_t len, const void *val)
{
    *p++ = code;
    *p++ = len;
    memcpy(p, val, len);
    return p + len;
}

static void dhcp_send(struct netif *nif, const struct dhcp_tx *tx)
{
    struct mbuf *m = m_getcl();
    if (m == NULL)
        return;
    uint8_t *ip = m->data;
    uint8_t *udp = ip + sizeof(struct ipv4_hdr);
    uint8_t *dh = udp + sizeof(struct udp_hdr);
    memset(dh, 0, DHCP_FIXED);
    dh[BOOTP_OFF_OP] = BOOTP_REQUEST;
    dh[BOOTP_OFF_HTYPE] = DHCP_HTYPE_ETHER;
    dh[BOOTP_OFF_HLEN] = 6;
    memcpy(dh + BOOTP_OFF_XID, &tx->xid, 4);
    dh[BOOTP_OFF_SECS] = (uint8_t)(tx->secs >> 8);
    dh[BOOTP_OFF_SECS + 1] = (uint8_t)tx->secs;
    memcpy(dh + BOOTP_OFF_CIADDR, &tx->ciaddr, 4);   /* flags 0: the reply may be unicast (dhcp_client_input takes either) */
    memcpy(dh + BOOTP_OFF_CHADDR, nif->mac, 6);
    uint32_t cookie = htonl(DHCP_MAGIC);
    memcpy(dh + BOOTP_LEN, &cookie, 4);

    uint8_t *o = dh + DHCP_FIXED;
    o = put_opt(o, OPT_MSGTYPE, 1, &tx->type);
    uint8_t cid[7] = { DHCP_HTYPE_ETHER };
    memcpy(cid + 1, nif->mac, 6);
    o = put_opt(o, OPT_CLIENTID, sizeof(cid), cid);
    if (tx->reqip)
        o = put_opt(o, OPT_REQIP, 4, &tx->reqip);
    if (tx->server_opt)
        o = put_opt(o, OPT_SERVERID, 4, &tx->server_opt);
    static const uint8_t params[] = { OPT_SUBNET, OPT_ROUTER, OPT_DNS, OPT_LEASE, OPT_T1, OPT_T2 };
    o = put_opt(o, OPT_PARAMS, sizeof(params), params);
    *o++ = OPT_END;
    while ((o - dh) < 300)          /* BOOTP's minimum message, which some relays still expect */
        *o++ = OPT_PAD;
    uint32_t dhlen = (uint32_t)(o - dh);

    uint32_t src = tx->unicast ? tx->ciaddr : 0;
    uint32_t dst = tx->unicast ? tx->server : INADDR_BROADCAST_N;
    uint32_t udplen = (uint32_t)sizeof(struct udp_hdr) + dhlen;
    uint32_t iplen = (uint32_t)sizeof(struct ipv4_hdr) + udplen;
    struct udp_hdr *uh = (struct udp_hdr *)udp;
    uh->sport = htons(DHCP_CPORT);
    uh->dport = htons(DHCP_SPORT);
    uh->len = htons((uint16_t)udplen);
    uh->cksum = 0;
    uint16_t c = cksum_fold(cksum_partial(udp, udplen, cksum_pseudo4(src, dst, IPPROTO_UDP, (uint16_t)udplen)));
    uh->cksum = c ? c : 0xffff;

    struct ipv4_hdr *iph = (struct ipv4_hdr *)ip;
    iph->vhl = 0x45; iph->tos = 0; iph->len = htons((uint16_t)iplen);
    iph->id = 0; iph->frag = 0; iph->ttl = IP_DEFAULT_TTL; iph->proto = IPPROTO_UDP;
    iph->cksum = 0; iph->src = src; iph->dst = dst;
    iph->cksum = in_cksum(iph, sizeof(*iph));
    m->len = m->pkt.len = iplen;
    m->pkt.proto = ETH_P_IP;

    if (!tx->unicast) {
        ether_output(nif, m, eth_broadcast, ETH_P_IP);
        return;
    }
    /* The server is on the link the lease is on (RFC 2131 §4.4.5 unicasts
     * the renewal to it); if not, through the router the lease named. */
    uint32_t hop = ((dst ^ nif->ip4.addr) & nif->ip4.mask) == 0 || nif->ip4.gateway == 0 ? dst : nif->ip4.gateway;
    uint8_t mac[ETH_ALEN];
    int rc = arp_resolve(nif, hop, mac, m);
    if (rc == 0)
        ether_output(nif, m, mac, ETH_P_IP);
    /* -EINPROGRESS: queued on the ARP entry; any other error freed it, and
     * the retransmission tries again. */
}

/* --- the state machine (under c->lock) ---------------------------------------------- */

static void new_exchange(struct dhcp_client *c, uint64_t now)
{
    c->xid = (uint32_t)random_u64();
    c->tries = 0;
    c->start_ns = now;
    c->req_ns = now;   /* a renewal's or rebind's first message is its REQUEST */
}

static void tx_discover(struct dhcp_client *c, uint64_t now, struct dhcp_tx *tx)
{
    *tx = (struct dhcp_tx){ .type = DHCP_DISCOVER, .xid = c->xid, .secs = secs_since(c, now) };
    c->next_ns = now + backoff_ns(c->tries);
    STAT(discovers);
}

static void tx_request_selecting(struct dhcp_client *c, uint64_t now, struct dhcp_tx *tx)
{
    if (c->tries == 0)
        c->req_ns = now;   /* the lease counts from this REQUEST, not from the DISCOVER */
    *tx = (struct dhcp_tx){ .type = DHCP_REQUEST, .xid = c->xid, .reqip = c->offer_addr, .server_opt = c->server,
                            .secs = secs_since(c, now) };
    c->next_ns = now + backoff_ns(c->tries);
    STAT(requests);
}

/* RENEWING (unicast to the server) or REBINDING (broadcast): ciaddr is the
 * address held, and neither the requested-address nor the server option is
 * sent (RFC 2131 §4.3.2). The retransmission is half the time left to T2
 * (renewing) or to the lease's end (rebinding), 60 s at least, and never
 * past that deadline. */
static void tx_request_bound(struct dhcp_client *c, uint64_t now, struct dhcp_tx *tx)
{
    bool renew = c->state == DHCP_S_RENEWING;
    *tx = (struct dhcp_tx){ .type = DHCP_REQUEST, .unicast = renew, .xid = c->xid, .ciaddr = c->addr,
                            .server = c->server, .secs = secs_since(c, now) };
    uint64_t until = renew ? c->t2_at : c->end_at;
    uint64_t wait = until > now ? (until - now) / 2 : 0;
    if (wait < DHCP_RENEW_MIN_NS)
        wait = DHCP_RENEW_MIN_NS;
    c->next_ns = now + wait < until ? now + wait : until;
    if (renew)
        STAT(renews);
    else
        STAT(rebinds);
}

static void lose_lease(struct dhcp_client *c)
{
    c->addr = c->mask = c->router = 0;
    c->ndns = 0;
    c->lease_s = c->t1_s = c->t2_s = 0;
    netif_set_ipv4(c->nif, 0, 0, 0);
}

/* Restart from INIT: a fresh exchange and its DISCOVER now. */
static void restart(struct dhcp_client *c, uint64_t now, struct dhcp_tx *tx)
{
    c->state = DHCP_S_SELECTING;
    new_exchange(c, now);
    tx_discover(c, now, tx);
}

/* The timer's or a test's turn: whatever deadline has passed. */
static void step(struct dhcp_client *c, uint64_t now, struct dhcp_tx *tx)
{
    switch (c->state) {
    case DHCP_S_INIT:
        restart(c, now, tx);
        break;
    case DHCP_S_SELECTING:
        if (now >= c->next_ns) {
            c->tries++;
            tx_discover(c, now, tx);
        }
        break;
    case DHCP_S_REQUESTING:
        if (now >= c->next_ns) {
            if (++c->tries >= DHCP_REQUEST_TRIES)
                restart(c, now, tx);
            else
                tx_request_selecting(c, now, tx);
        }
        break;
    case DHCP_S_BOUND:
    case DHCP_S_RENEWING:
    case DHCP_S_REBINDING:
        if (now >= c->end_at) {
            kinfo("dhcp: %s: the lease on %u.%u.%u.%u ended; the address is removed", c->nif->name,
                  ((uint8_t *)&c->addr)[0], ((uint8_t *)&c->addr)[1], ((uint8_t *)&c->addr)[2],
                  ((uint8_t *)&c->addr)[3]);
            STAT(expiries);
            lose_lease(c);
            restart(c, now, tx);
        } else if (now >= c->t2_at && c->state != DHCP_S_REBINDING) {
            c->state = DHCP_S_REBINDING;
            new_exchange(c, now);
            tx_request_bound(c, now, tx);
        } else if (now >= c->t1_at && c->state == DHCP_S_BOUND) {
            c->state = DHCP_S_RENEWING;
            new_exchange(c, now);
            tx_request_bound(c, now, tx);
        } else if (c->state != DHCP_S_BOUND && now >= c->next_ns) {
            c->tries++;
            tx_request_bound(c, now, tx);
        }
        break;
    }
}

/* When the client next needs to run. */
static uint64_t next_deadline(const struct dhcp_client *c)
{
    switch (c->state) {
    case DHCP_S_SELECTING:
    case DHCP_S_REQUESTING:
        return c->next_ns;
    case DHCP_S_BOUND:
        return c->t1_at;
    case DHCP_S_RENEWING: {
        uint64_t d = c->next_ns < c->t2_at ? c->next_ns : c->t2_at;
        return d;
    }
    case DHCP_S_REBINDING:
        return c->next_ns < c->end_at ? c->next_ns : c->end_at;
    default:
        return 0;
    }
}

static void arm(struct dhcp_client *c, uint64_t now)
{
    uint64_t at = next_deadline(c);
    /* The timer may be pending (an input re-arms before it fires): cancel
     * first. A callback already past its start only queues the work, which
     * finds nothing due and re-arms -- harmless. */
    timer_cancel(&c->timer);
    timer_start(&c->timer, at > now ? at - now : 0);
}

static void dhcp_work(void *arg)
{
    struct dhcp_client *c = arg;
    struct dhcp_tx tx = { 0 };
    arch_irq_state_t s = spin_lock_irqsave(&c->lock);
    if (c->stopping || !c->in_use) {
        spin_unlock_irqrestore(&c->lock, s);
        return;
    }
    uint64_t now = clock_now_ns();
    step(c, now, &tx);
    arm(c, now);
    struct netif *nif = c->nif;
    spin_unlock_irqrestore(&c->lock, s);
    if (tx.type)
        dhcp_send(nif, &tx);
}

static void dhcp_timer(struct timer *t, void *arg)
{
    (void)t;
    net_work_queue(&((struct dhcp_client *)arg)->work);
}

/* --- receiving --------------------------------------------------------------------- */

static uint32_t be32_at(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return ntohl(v);
}

/* The natural mask of an address's class, for an ACK without option 1. */
static uint32_t class_mask(uint32_t addr)
{
    uint32_t a = ntohl(addr);
    return htonl(a < 0x80000000u ? 0xff000000u : a < 0xc0000000u ? 0xffff0000u : 0xffffff00u);
}

/* An ACK: take the lease (under c->lock). False when it is unusable. */
static bool bind_lease(struct dhcp_client *c, const uint8_t *dh, const uint8_t *opts, uint32_t optlen, uint64_t now)
{
    uint32_t yi;
    memcpy(&yi, dh + BOOTP_OFF_YIADDR, 4);
    uint8_t l;
    const uint8_t *v = dhcp_opt(opts, optlen, OPT_LEASE, &l);
    if (yi == 0 || yi == INADDR_BROADCAST_N || v == NULL || l != 4)
        return false;   /* an ACK to a REQUEST must carry the lease time (RFC 2131 table 3) */
    uint32_t lease = be32_at(v);
    if (lease == 0)
        return false;
    uint32_t mask = class_mask(yi), router = 0;
    if ((v = dhcp_opt(opts, optlen, OPT_SUBNET, &l)) != NULL && l == 4)
        memcpy(&mask, v, 4);
    if ((v = dhcp_opt(opts, optlen, OPT_ROUTER, &l)) != NULL && l >= 4)
        memcpy(&router, v, 4);   /* the first router */
    uint8_t ndns = 0;
    if ((v = dhcp_opt(opts, optlen, OPT_DNS, &l)) != NULL)
        for (unsigned i = 0; i + 4 <= l && ndns < DHCP_MAX_DNS; i += 4)
            memcpy(&c->dns[ndns++], v + i, 4);
    c->ndns = ndns;
    /* T1 and T2: the server's when they are ordered (T1 < T2 < lease),
     * else the RFC's 0.5 and 0.875 of the lease -- in nanoseconds, so a
     * lease of a second or two still has a T1 before its T2. 0xffffffff
     * is infinite. */
    uint64_t lease_ns = (uint64_t)lease * NS_PER_S;
    uint64_t t1_ns = lease_ns / 2, t2_ns = lease_ns / 8 * 7;
    uint32_t st1 = 0, st2 = 0;
    if ((v = dhcp_opt(opts, optlen, OPT_T1, &l)) != NULL && l == 4)
        st1 = be32_at(v);
    if ((v = dhcp_opt(opts, optlen, OPT_T2, &l)) != NULL && l == 4)
        st2 = be32_at(v);
    if (st1 && st2 && st1 < st2 && st2 < lease) {
        t1_ns = (uint64_t)st1 * NS_PER_S;
        t2_ns = (uint64_t)st2 * NS_PER_S;
    }
    uint32_t t1 = (uint32_t)(t1_ns / NS_PER_S), t2 = (uint32_t)(t2_ns / NS_PER_S);
    bool changed = c->addr != yi || c->mask != mask || c->router != router;
    c->addr = yi;
    c->mask = mask;
    c->router = router;
    c->lease_s = lease;
    c->t1_s = t1;
    c->t2_s = t2;
    /* Times count from the time the REQUEST was sent (RFC 2131 §4.4.1),
     * not the DISCOVER: an offer that was slow to come takes nothing off
     * the lease. An infinite lease never reaches T1. */
    uint64_t base = c->req_ns;
    if (lease == 0xffffffffu) {
        c->t1_at = c->t2_at = c->end_at = UINT64_MAX;
    } else {
        c->t1_at = base + t1_ns;
        c->t2_at = base + t2_ns;
        c->end_at = base + lease_ns;
    }
    c->state = DHCP_S_BOUND;
    c->tries = 0;
    (void)now;
    if (changed)
        netif_set_ipv4(c->nif, yi, mask, router);
    return true;
}

/*
 * An ACK or a NAK may answer only a REQUEST, and only from a server the
 * client asked: the server it selected (REQUESTING) or the one that granted
 * the lease (RENEWING, which unicast to it); a REBINDING client asked every
 * server. The server identifier is required (RFC 2131 §4.3.1). Under c->lock.
 */
static bool from_server(const struct dhcp_client *c, uint32_t server)
{
    if (server == 0)
        return false;
    switch (c->state) {
    case DHCP_S_REQUESTING:
    case DHCP_S_RENEWING:
        return server == c->server;
    case DHCP_S_REBINDING:
        return true;
    default:
        return false;
    }
}

bool dhcp_client_input(struct netif *nif, struct mbuf *m, unsigned ihl)
{
    uint8_t ports[4];
    if (!m_copydata(m, ihl, 4, ports) || ports[0] != 0 || ports[1] != DHCP_SPORT || ports[2] != 0 ||
        ports[3] != DHCP_CPORT)
        return false;
    if (client_of(nif) == NULL)
        return false;
    /* Only a datagram for this host: the limited broadcast, the address it
     * holds, or anything while it holds none (an offer may be unicast to
     * the address offered). Anything else takes the ordinary path --
     * forwarding, its firewall, or the not-for-us drop. */
    uint32_t dst;
    if (!m_copydata(m, 16, 4, &dst))
        return false;
    uint32_t mine = __atomic_load_n(&nif->ip4.addr, __ATOMIC_ACQUIRE);
    if (dst != INADDR_BROADCAST_N && mine != 0 && dst != mine)
        return false;

    /* A server-to-client datagram for this interface's client: claimed,
     * whatever becomes of it. Copied out whole (an Ethernet frame at most). */
    uint32_t total = m->pkt.len;
    uint8_t *buf = total > ihl + sizeof(struct udp_hdr) + DHCP_FIXED && total <= 1600 ? kmalloc(total, 0) : NULL;
    if (buf == NULL || !m_copydata(m, 0, total, buf)) {
        STAT(ignored);
        kfree(buf);
        m_freem(m);
        return true;
    }
    m_freem(m);
    const struct ipv4_hdr *iph = (const struct ipv4_hdr *)buf;
    const uint8_t *udp = buf + ihl;
    const struct udp_hdr *uh = (const struct udp_hdr *)udp;
    uint32_t udplen = ntohs(uh->len);
    if (udplen < sizeof(*uh) + DHCP_FIXED || udplen > total - ihl ||
        (uh->cksum != 0 &&
         cksum_fold(cksum_partial(udp, udplen, cksum_pseudo4(iph->src, iph->dst, IPPROTO_UDP, (uint16_t)udplen))) !=
             0)) {   /* cksum_fold complements: a valid datagram sums to 0 */
        STAT(ignored);
        kfree(buf);
        return true;
    }
    const uint8_t *dh = udp + sizeof(*uh);
    uint32_t dhlen = udplen - (uint32_t)sizeof(*uh);
    const uint8_t *opts = dh + DHCP_FIXED;
    uint32_t optlen = dhlen - DHCP_FIXED;
    uint8_t l = 0;
    const uint8_t *mt = dhcp_opt(opts, optlen, OPT_MSGTYPE, &l);
    uint32_t xid;
    memcpy(&xid, dh + BOOTP_OFF_XID, 4);

    struct dhcp_tx tx = { 0 };
    arch_irq_state_t s;
    struct dhcp_client *c = lock_client(nif, &s);
    if (c == NULL) {
        kfree(buf);
        return true;   /* the client stopped while the datagram was copied */
    }
    uint64_t now = clock_now_ns();
    bool ours = dh[BOOTP_OFF_OP] == BOOTP_REPLY && dh[BOOTP_OFF_HTYPE] == DHCP_HTYPE_ETHER &&
                dh[BOOTP_OFF_HLEN] == 6 && be32_at(dh + BOOTP_LEN) == DHCP_MAGIC && xid == c->xid &&
                memcmp(dh + BOOTP_OFF_CHADDR, nif->mac, 6) == 0 && mt != NULL && l == 1;
    bool used = false;
    if (ours) {
        uint8_t sl;
        const uint8_t *sid = dhcp_opt(opts, optlen, OPT_SERVERID, &sl);
        uint32_t server = 0;
        if (sid != NULL && sl == 4)
            memcpy(&server, sid, 4);
        switch (*mt) {
        case DHCP_OFFER:
            if (c->state == DHCP_S_SELECTING && server != 0) {
                uint32_t yi;
                memcpy(&yi, dh + BOOTP_OFF_YIADDR, 4);
                if (yi != 0 && yi != INADDR_BROADCAST_N) {
                    STAT(offers);
                    c->offer_addr = yi;
                    c->server = server;
                    c->state = DHCP_S_REQUESTING;
                    c->tries = 0;
                    tx_request_selecting(c, now, &tx);
                    used = true;
                }
            }
            break;
        case DHCP_ACK:
            if (from_server(c, server) && bind_lease(c, dh, opts, optlen, now)) {
                if (server != 0)
                    c->server = server;   /* rebinding may be answered by another server */
                STAT(acks);
                used = true;
            }
            break;
        case DHCP_NAK:
            if (from_server(c, server)) {
                STAT(naks);
                kinfo("dhcp: %s: NAK; starting over", nif->name);
                if (c->addr != 0)
                    lose_lease(c);
                restart(c, now, &tx);
                used = true;
            }
            break;
        default:
            break;
        }
    }
    if (used)
        arm(c, now);
    else
        STAT(ignored);
    bool bound_now = used && c->state == DHCP_S_BOUND;
    uint32_t a = c->addr, lease = c->lease_s;
    uint8_t ndns = c->ndns;
    uint32_t dns0 = c->dns[0];
    spin_unlock_irqrestore(&c->lock, s);
    kfree(buf);
    if (tx.type)
        dhcp_send(nif, &tx);
    if (bound_now) {
        const uint8_t *p = (const uint8_t *)&a, *d = (const uint8_t *)&dns0;
        kinfo("dhcp: %s: bound to %u.%u.%u.%u for %u s, %u DNS server(s)%s%u.%u.%u.%u", nif->name, p[0], p[1], p[2],
              p[3], lease, ndns, ndns ? ", first " : "", ndns ? d[0] : 0, ndns ? d[1] : 0, ndns ? d[2] : 0,
              ndns ? d[3] : 0);
    }
    return true;
}

/* --- lifetime -------------------------------------------------------------------- */

int dhcp_client_start(struct netif *nif)
{
    struct dhcp_client *c = NULL;
    arch_irq_state_t s = spin_lock_irqsave(&g_table_lock);
    if (!g_slots_ready) {
        for (unsigned i = 0; i < DHCP_MAX_CLIENTS; i++)
            spinlock_init(&g_clients[i].lock, "dhcp-client");
        __atomic_store_n(&g_slots_ready, true, __ATOMIC_RELEASE);
    }
    for (unsigned i = 0; i < DHCP_MAX_CLIENTS; i++) {
        if (!g_clients[i].in_use && g_clients[i].nif == NULL) {
            c = &g_clients[i];
            break;
        }
    }
    if (c == NULL) {
        spin_unlock_irqrestore(&g_table_lock, s);
        kwarn("dhcp: %s: %u clients already run; no DHCP on it", nif->name, DHCP_MAX_CLIENTS);
        return -ENOSPC;
    }
    netif_get(nif);
    /* Every field but the lock, under the lock (see g_clients). */
    arch_irq_state_t cs = spin_lock_irqsave(&c->lock);
    c->stopping = false;
    c->xid = 0;
    c->tries = 0;
    c->next_ns = c->start_ns = c->req_ns = 0;
    c->offer_addr = c->server = c->addr = c->mask = c->router = 0;
    memset(c->dns, 0, sizeof(c->dns));
    c->ndns = 0;
    c->lease_s = c->t1_s = c->t2_s = 0;
    c->t1_at = c->t2_at = c->end_at = 0;
    timer_setup(&c->timer, dhcp_timer, c);
    net_work_init(&c->work, dhcp_work, c);
    c->nif = nif;
    c->state = DHCP_S_INIT;
    __atomic_store_n(&c->in_use, true, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&c->lock, cs);
    spin_unlock_irqrestore(&g_table_lock, s);
    kinfo("dhcp: %s: no configured address; discovering", nif->name);
    net_work_queue(&c->work);
    return 0;
}

void dhcp_client_stop(struct netif *nif)
{
    struct dhcp_client *c = client_of(nif);
    if (c == NULL)
        return;
    arch_irq_state_t s = spin_lock_irqsave(&c->lock);
    c->stopping = true;
    spin_unlock_irqrestore(&c->lock, s);
    /* The timer may queue the work once more and the work may be queued
     * already; it sees `stopping` and neither sends nor re-arms. After the
     * barrier nothing of the client runs anywhere. */
    timer_cancel_sync(&c->timer);
    net_workers_barrier();
    timer_cancel_sync(&c->timer);
    s = spin_lock_irqsave(&g_table_lock);
    arch_irq_state_t cs = spin_lock_irqsave(&c->lock);
    __atomic_store_n(&c->in_use, false, __ATOMIC_RELEASE);
    c->nif = NULL;
    spin_unlock_irqrestore(&c->lock, cs);
    spin_unlock_irqrestore(&g_table_lock, s);
    netif_put(nif);
}

void dhcp_client_kick(struct netif *nif)
{
    arch_irq_state_t s;
    struct dhcp_client *c = lock_client(nif, &s);
    if (c == NULL)
        return;
    bool waiting = c->state == DHCP_S_SELECTING || c->state == DHCP_S_REQUESTING;
    if (waiting)
        c->next_ns = clock_now_ns();
    spin_unlock_irqrestore(&c->lock, s);
    if (waiting)
        net_work_queue(&c->work);
}

unsigned dhcp_lease_list(struct dhcp_lease_info *out, unsigned max)
{
    unsigned n = 0;
    uint64_t now = clock_now_ns();
    for (unsigned i = 0; i < DHCP_MAX_CLIENTS && n < max; i++) {
        struct dhcp_client *c = &g_clients[i];
        arch_irq_state_t s = spin_lock_irqsave(&g_table_lock);
        bool live = c->in_use && !c->stopping;
        if (!live) {
            spin_unlock_irqrestore(&g_table_lock, s);
            continue;
        }
        arch_irq_state_t s2 = spin_lock_irqsave(&c->lock);
        struct dhcp_lease_info *o = &out[n++];
        memset(o, 0, sizeof(*o));
        strlcpy(o->ifname, c->nif->name, sizeof(o->ifname));
        o->state = (uint8_t)c->state;
        bool held = c->state == DHCP_S_BOUND || c->state == DHCP_S_RENEWING || c->state == DHCP_S_REBINDING;
        if (held) {
            o->addr = c->addr;
            o->mask = c->mask;
            o->router = c->router;
            o->server = c->server;
            o->ndns = c->ndns;
            memcpy(o->dns, c->dns, sizeof(o->dns));
            o->lease_s = c->lease_s;
            o->t1_s = c->t1_s;
            o->t2_s = c->t2_s;
#define MS_TO(at) ((at) == UINT64_MAX ? UINT32_MAX : (at) <= now ? 0u \
                   : ((at) - now) / 1000000u > UINT32_MAX ? UINT32_MAX : (uint32_t)(((at) - now) / 1000000u))
            o->remaining_ms = MS_TO(c->end_at);
            o->t1_ms = MS_TO(c->t1_at);
            o->t2_ms = MS_TO(c->t2_at);
#undef MS_TO
        }
        spin_unlock_irqrestore(&c->lock, s2);
        spin_unlock_irqrestore(&g_table_lock, s);
    }
    return n;
}

void dhcp_boot_wait(void)
{
    uint64_t deadline = clock_deadline_ns(DHCP_BOOT_WAIT_NS);
    for (;;) {
        struct dhcp_lease_info l[DHCP_MAX_CLIENTS];
        unsigned clients = dhcp_lease_list(l, DHCP_MAX_CLIENTS), bound = 0;
        for (unsigned i = 0; i < clients; i++)
            if (l[i].state == DHCP_S_BOUND || l[i].state == DHCP_S_RENEWING || l[i].state == DHCP_S_REBINDING)
                bound++;
        if (bound == clients) {
            if (clients)
                kinfo("dhcp: %u interface(s) bound", bound);
            return;
        }
        if (clock_deadline_passed(deadline)) {
            kwarn("dhcp: %u of %u interface(s) unbound after %llu s; they keep trying", clients - bound, clients,
                  (unsigned long long)(DHCP_BOOT_WAIT_NS / NS_PER_S));
            return;
        }
        thread_sleep_ms(10);
    }
}

void dhcp_get_stats(struct dhcp_stats *out)
{
    out->discovers = __atomic_load_n(&g_stats.discovers, __ATOMIC_RELAXED);
    out->requests = __atomic_load_n(&g_stats.requests, __ATOMIC_RELAXED);
    out->offers = __atomic_load_n(&g_stats.offers, __ATOMIC_RELAXED);
    out->acks = __atomic_load_n(&g_stats.acks, __ATOMIC_RELAXED);
    out->naks = __atomic_load_n(&g_stats.naks, __ATOMIC_RELAXED);
    out->renews = __atomic_load_n(&g_stats.renews, __ATOMIC_RELAXED);
    out->rebinds = __atomic_load_n(&g_stats.rebinds, __ATOMIC_RELAXED);
    out->expiries = __atomic_load_n(&g_stats.expiries, __ATOMIC_RELAXED);
    out->ignored = __atomic_load_n(&g_stats.ignored, __ATOMIC_RELAXED);
}

#if CONFIG_SELFTEST
int dhcp_test_fire(struct netif *nif, enum dhcp_test_event ev)
{
    arch_irq_state_t s;
    struct dhcp_client *c = lock_client(nif, &s);
    if (c == NULL)
        return -ENOENT;
    uint64_t now = clock_now_ns();
    /* Each deadline brings the earlier ones with it: the lease's end is
     * past T2, and T2 past T1. */
    if (ev == DHCP_TEST_EXPIRE)
        c->end_at = now;
    if ((ev == DHCP_TEST_EXPIRE || ev == DHCP_TEST_T2) && c->t2_at > now)
        c->t2_at = now;
    if (ev != DHCP_TEST_RETX && c->t1_at > now)
        c->t1_at = now;
    if (ev == DHCP_TEST_RETX)
        c->next_ns = now;
    spin_unlock_irqrestore(&c->lock, s);
    net_work_queue(&c->work);
    return 0;
}

int dhcp_test_state(struct netif *nif)
{
    arch_irq_state_t s;
    struct dhcp_client *c = lock_client(nif, &s);
    if (c == NULL)
        return 0;
    int st = (int)c->state;
    spin_unlock_irqrestore(&c->lock, s);
    return st;
}
#endif
