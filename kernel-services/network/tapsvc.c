/*
 * tapsvc.c - The tap's autoconfiguration service (tapsvc.h).
 *
 * DHCP server (this file, §2 of the report): a tap input filter claims the
 * guest's UDP-to-port-67 frames, answers DISCOVER/REQUEST for the one guest
 * slot, and sends the reply back out the tap with ether_output — never
 * through IP routing, since the guest has no address yet and the tap is
 * NETIF_NODEFAULT. Reply destination follows the client's broadcast flag at
 * both layers (RFC 2131 §4.1). The DNS proxy is added alongside (§3).
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/net/cksum.h>
#include <kernel/net/ether.h>
#include <kernel/net/inet.h>
#include <kernel/net/ip.h>
#include <kernel/net/nat.h>
#include <kernel/net/tap.h>
#include <kernel/net/tapsvc.h>
#include <kernel/net/udp.h>
#include <kernel/random.h>
#include <kernel/netif.h>
#include <kernel/fwcfg.h>
#include <kernel/socket.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/timer.h>

/* --- DHCP wire format (RFC 2131 / 2132) ----------------------------------- */

#define BOOTP_REQUEST 1
#define BOOTP_REPLY   2
#define DHCP_HTYPE_ETHER 1
#define DHCP_MAGIC    0x63825363u

#define OPT_PAD        0
#define OPT_SUBNET     1
#define OPT_ROUTER     3
#define OPT_DNS        6
#define OPT_LEASE      51
#define OPT_MSGTYPE    53
#define OPT_SERVERID   54
#define OPT_REQIP      50
#define OPT_END        255

#define DHCP_SPORT 67   /* server */
#define DHCP_CPORT 68   /* client */
#define DHCP_FLAG_BROADCAST 0x8000u

/* BOOTP fixed header (236 bytes), then the 4-byte magic cookie, then options. */
#define BOOTP_LEN 236
#define DHCP_FIXED (BOOTP_LEN + 4)
#define BOOTP_OFF_OP     0
#define BOOTP_OFF_HTYPE  1
#define BOOTP_OFF_HLEN   2
#define BOOTP_OFF_XID    4
#define BOOTP_OFF_FLAGS  10
#define BOOTP_OFF_YIADDR 16
#define BOOTP_OFF_SIADDR 20
#define BOOTP_OFF_CHADDR 28

/* --- DNS proxy (§3): per-instance state ------------------------------------ */

#define DNS_PENDING_MAX 128u
#define DNS_TIMEOUT_NS  (5ull * 1000000000ull)
#define DNS_PORT 53
#define DNS_HDR 12                 /* the fixed DNS header: id, flags, 4 counts */

struct dns_pending {
    bool in_use;
    uint16_t guest_id;             /* the id the guest used (host order) */
    uint16_t up_id;                /* the id we lent upstream (host order) */
    struct netaddr guest;          /* where the answer goes back */
    uint64_t expires_ns;
};

/* One service instance per tap: its DHCP binding and its DNS proxy. */
struct tapsvc {
    struct tap *tap;
    struct netif *nif;
    uint32_t gateway;      /* the tap's host address (network order) */
    uint32_t mask;
    uint32_t guest;        /* the one guest's address (network order) */
    bool bound;            /* a client has taken the lease */
    uint8_t client_mac[6];
    spinlock_t lock;       /* the DHCP binding */
    /* DNS proxy */
    struct dns_pending dns_tab[DNS_PENDING_MAX];
    spinlock_t dns_lock;
    struct socket *gsock;          /* bound gateway:53, faces the guest */
    struct socket *usock;          /* faces the upstream resolver */
    struct thread *gth, *uth;
    uint32_t up_ip; uint16_t up_port; bool up_set;
    volatile bool running;
};

/* The live instances, for the periodic age sweep and aggregate stats. */
#define TAPSVC_MAX 8u
static struct tapsvc *g_svcs[TAPSVC_MAX];
static spinlock_t g_svcs_lock = SPINLOCK_INIT("tapsvc-list");
static struct tapsvc_stats g_stats;

#define STAT(f) __atomic_fetch_add(&g_stats.f, 1, __ATOMIC_RELAXED)

/* Find DHCP option `code` in [opts, opts+len); returns its length via *olen
 * and a pointer to its value, or NULL. Stops at OPT_END, skips OPT_PAD. */
static const uint8_t *dhcp_opt(const uint8_t *opts, uint32_t len, uint8_t code, uint8_t *olen)
{
    uint32_t i = 0;
    while (i < len) {
        uint8_t c = opts[i++];
        if (c == OPT_END)
            break;
        if (c == OPT_PAD)
            continue;
        if (i >= len)
            break;
        uint8_t l = opts[i++];
        if (i + l > len)
            break;
        if (c == code) {
            *olen = l;
            return opts + i;
        }
        i += l;
    }
    return NULL;
}

static uint8_t *put_opt(uint8_t *p, uint8_t code, uint8_t len, const void *val)
{
    *p++ = code;
    *p++ = len;
    memcpy(p, val, len);
    return p + len;
}

/* Build and send a DHCP reply of `type` out the tap. `req` points at the
 * request's DHCP payload (BOOTP header onward), used for xid/flags/chaddr. */
static void dhcp_reply(struct tapsvc *s, const uint8_t *req, uint8_t type)
{
    /* The two flag bytes assembled big-endian already give the host value. */
    uint16_t flags = (uint16_t)(req[BOOTP_OFF_FLAGS] << 8 | req[BOOTP_OFF_FLAGS + 1]);
    bool bcast = (flags & DHCP_FLAG_BROADCAST) || type == DHCP_NAK;

    struct mbuf *m = m_getcl();
    if (m == NULL)
        return;
    uint8_t *ip = m->data;
    uint8_t *udp = ip + sizeof(struct ipv4_hdr);
    uint8_t *dh = udp + sizeof(struct udp_hdr);
    memset(dh, 0, DHCP_FIXED);
    dh[BOOTP_OFF_OP] = BOOTP_REPLY;
    dh[BOOTP_OFF_HTYPE] = DHCP_HTYPE_ETHER;
    dh[BOOTP_OFF_HLEN] = 6;
    memcpy(dh + BOOTP_OFF_XID, req + BOOTP_OFF_XID, 4);
    dh[BOOTP_OFF_FLAGS] = req[BOOTP_OFF_FLAGS];
    dh[BOOTP_OFF_FLAGS + 1] = req[BOOTP_OFF_FLAGS + 1];
    if (type != DHCP_NAK)
        memcpy(dh + BOOTP_OFF_YIADDR, &s->guest, 4);
    memcpy(dh + BOOTP_OFF_SIADDR, &s->gateway, 4);
    memcpy(dh + BOOTP_OFF_CHADDR, req + BOOTP_OFF_CHADDR, 16);
    uint32_t cookie = htonl(DHCP_MAGIC);
    memcpy(dh + BOOTP_LEN, &cookie, 4);

    uint8_t *o = dh + DHCP_FIXED;
    o = put_opt(o, OPT_MSGTYPE, 1, &type);
    o = put_opt(o, OPT_SERVERID, 4, &s->gateway);
    if (type == DHCP_OFFER || type == DHCP_ACK) {
        uint32_t lease = htonl(TAPSVC_LEASE_SECS);
        o = put_opt(o, OPT_LEASE, 4, &lease);
        o = put_opt(o, OPT_SUBNET, 4, &s->mask);
        o = put_opt(o, OPT_ROUTER, 4, &s->gateway);
        o = put_opt(o, OPT_DNS, 4, &s->gateway);
    }
    *o++ = OPT_END;
    uint32_t dhlen = (uint32_t)(o - dh);

    uint32_t udplen = (uint32_t)sizeof(struct udp_hdr) + dhlen;
    uint32_t iplen = (uint32_t)sizeof(struct ipv4_hdr) + udplen;
    uint32_t dst_ip = bcast ? INADDR_BROADCAST_N : s->guest;

    struct udp_hdr *uh = (struct udp_hdr *)udp;
    uh->sport = htons(DHCP_SPORT);
    uh->dport = htons(DHCP_CPORT);
    uh->len = htons((uint16_t)udplen);
    uh->cksum = 0;
    uint32_t sum = cksum_pseudo4(s->gateway, dst_ip, IPPROTO_UDP, (uint16_t)udplen);
    uint16_t c = cksum_fold(cksum_partial(udp, udplen, sum));
    uh->cksum = c ? c : 0xffff;

    struct ipv4_hdr *iph = (struct ipv4_hdr *)ip;
    iph->vhl = 0x45; iph->tos = 0; iph->len = htons((uint16_t)iplen);
    iph->id = 0; iph->frag = 0; iph->ttl = IP_DEFAULT_TTL; iph->proto = IPPROTO_UDP;
    iph->cksum = 0; iph->src = s->gateway; iph->dst = dst_ip;
    iph->cksum = in_cksum(iph, sizeof(*iph));

    m->len = m->pkt.len = iplen;
    uint8_t dstmac[6];
    if (bcast)
        memcpy(dstmac, eth_broadcast, 6);
    else
        memcpy(dstmac, req + BOOTP_OFF_CHADDR, 6);   /* the client's hardware address */
    ether_output(s->nif, m, dstmac, ETH_P_IP);
}

/* The input filter: claim the guest's DHCP-to-server frames, answer them. */
static bool dhcp_filter(struct tap *t, const void *frame, uint32_t len, void *arg)
{
    (void)t;
    struct tapsvc *s = arg;
    const uint8_t *f = frame;
    if (len < ETH_HLEN + sizeof(struct ipv4_hdr) + sizeof(struct udp_hdr))
        return false;
    if (!(f[12] == 0x08 && f[13] == 0x00))       /* IPv4 */
        return false;
    const struct ipv4_hdr *iph = (const struct ipv4_hdr *)(f + ETH_HLEN);
    unsigned ihl = IPV4_HDR_LEN(iph);
    if ((iph->vhl >> 4) != 4 || ihl < 20 || iph->proto != IPPROTO_UDP)
        return false;
    if (ETH_HLEN + ihl + sizeof(struct udp_hdr) > len)
        return false;
    const struct udp_hdr *uh = (const struct udp_hdr *)(f + ETH_HLEN + ihl);
    if (ntohs(uh->dport) != DHCP_SPORT)
        return false;                            /* not DHCP-to-server: leave it for the stack */

    /* From here the frame is ours (claimed), whatever its contents. */
    const uint8_t *dh = f + ETH_HLEN + ihl + sizeof(struct udp_hdr);
    uint32_t dhlen = len - (uint32_t)(ETH_HLEN + ihl + sizeof(struct udp_hdr));
    if (dhlen < DHCP_FIXED) {
        STAT(dhcp_ignored);
        return true;
    }
    uint32_t cookie;
    memcpy(&cookie, dh + BOOTP_LEN, 4);
    if (dh[BOOTP_OFF_OP] != BOOTP_REQUEST || dh[BOOTP_OFF_HTYPE] != DHCP_HTYPE_ETHER ||
        dh[BOOTP_OFF_HLEN] != 6 || ntohl(cookie) != DHCP_MAGIC) {
        STAT(dhcp_ignored);
        return true;
    }
    const uint8_t *opts = dh + DHCP_FIXED;
    uint32_t optlen = dhlen - DHCP_FIXED;
    uint8_t l;
    const uint8_t *mt = dhcp_opt(opts, optlen, OPT_MSGTYPE, &l);
    if (mt == NULL || l != 1) {
        STAT(dhcp_ignored);
        return true;
    }
    const uint8_t *chaddr = dh + BOOTP_OFF_CHADDR;

    arch_irq_state_t st = spin_lock_irqsave(&s->lock);
    /* One binding: a second, different client is offered nothing. */
    bool mine = !s->bound || memcmp(s->client_mac, chaddr, 6) == 0;

    switch (mt[0]) {
    case DHCP_DISCOVER:
        STAT(dhcp_discover);
        if (!mine) { STAT(dhcp_ignored); spin_unlock_irqrestore(&s->lock, st); return true; }
        spin_unlock_irqrestore(&s->lock, st);
        STAT(dhcp_offer);
        dhcp_reply(s, dh, DHCP_OFFER);
        return true;
    case DHCP_REQUEST: {
        STAT(dhcp_request);
        /* The address the client is asking for: option 50, else ciaddr. */
        const uint8_t *req_ip = dhcp_opt(opts, optlen, OPT_REQIP, &l);
        uint32_t want = 0;
        if (req_ip && l == 4)
            memcpy(&want, req_ip, 4);
        else
            memcpy(&want, dh + 12, 4);   /* ciaddr */
        bool ok = mine && want == s->guest;
        if (ok) {
            s->bound = true;
            memcpy(s->client_mac, chaddr, 6);
        }
        spin_unlock_irqrestore(&s->lock, st);
        if (ok) { STAT(dhcp_ack); dhcp_reply(s, dh, DHCP_ACK); }
        else    { STAT(dhcp_nak); dhcp_reply(s, dh, DHCP_NAK); }
        return true;
    }
    case DHCP_DECLINE:
    case DHCP_RELEASE:
        STAT(dhcp_release);
        if (mine)
            s->bound = false;
        spin_unlock_irqrestore(&s->lock, st);
        return true;
    default:
        STAT(dhcp_ignored);
        spin_unlock_irqrestore(&s->lock, st);
        return true;
    }
}


/* True if some live entry already lent this upstream id. Caller holds dns_lock. */
static bool dns_id_taken(struct tapsvc *svc, uint16_t id, uint64_t now)
{
    for (unsigned i = 0; i < DNS_PENDING_MAX; i++)
        if (svc->dns_tab[i].in_use && now < svc->dns_tab[i].expires_ns && svc->dns_tab[i].up_id == id)
            return true;
    return false;
}

/* Record a guest query; lend a unique random upstream id via *uid. false when full. */
static bool dns_alloc(struct tapsvc *svc, uint16_t guest_id, const struct netaddr *from, uint16_t *uid)
{
    uint64_t now = clock_now_ns();
    arch_irq_state_t s = spin_lock_irqsave(&svc->dns_lock);
    struct dns_pending *slot = NULL;
    for (unsigned i = 0; i < DNS_PENDING_MAX; i++)
        if (!svc->dns_tab[i].in_use || now >= svc->dns_tab[i].expires_ns) { slot = &svc->dns_tab[i]; break; }
    if (slot == NULL) {
        spin_unlock_irqrestore(&svc->dns_lock, s);
        return false;
    }
    /* A random id (not a predictable counter) so an off-path attacker cannot
     * guess the outstanding value and race a forged answer. */
    uint16_t id = 0;
    for (unsigned tries = 0; tries < 4096u; tries++) {
        uint16_t cand = (uint16_t)random_u64();
        if (cand != 0 && !dns_id_taken(svc, cand, now)) { id = cand; break; }
    }
    if (id == 0) {
        spin_unlock_irqrestore(&svc->dns_lock, s);
        return false;
    }
    slot->in_use = true;
    slot->guest_id = guest_id;
    slot->up_id = id;
    slot->guest = *from;
    slot->expires_ns = now + DNS_TIMEOUT_NS;
    *uid = id;
    spin_unlock_irqrestore(&svc->dns_lock, s);
    return true;
}

/* Find and consume the entry for upstream id `uid`; false if none live. */
static bool dns_take(struct tapsvc *svc, uint16_t uid, struct netaddr *guest, uint16_t *guest_id)
{
    uint64_t now = clock_now_ns();
    arch_irq_state_t s = spin_lock_irqsave(&svc->dns_lock);
    for (unsigned i = 0; i < DNS_PENDING_MAX; i++) {
        struct dns_pending *e = &svc->dns_tab[i];
        if (e->in_use && now < e->expires_ns && e->up_id == uid) {
            *guest = e->guest;
            *guest_id = e->guest_id;
            e->in_use = false;
            spin_unlock_irqrestore(&svc->dns_lock, s);
            return true;
        }
    }
    spin_unlock_irqrestore(&svc->dns_lock, s);
    return false;
}

static void dns_age_one(struct tapsvc *svc, uint64_t now_ns)
{
    arch_irq_state_t s = spin_lock_irqsave(&svc->dns_lock);
    for (unsigned i = 0; i < DNS_PENDING_MAX; i++)
        if (svc->dns_tab[i].in_use && now_ns >= svc->dns_tab[i].expires_ns) {
            svc->dns_tab[i].in_use = false;
            STAT(dns_expired);
        }
    spin_unlock_irqrestore(&svc->dns_lock, s);
}

/* Reclaim expired pending queries across every live instance. */
void tapsvc_dns_age(uint64_t now_ns)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_svcs_lock);
    for (unsigned i = 0; i < TAPSVC_MAX; i++)
        if (g_svcs[i])
            dns_age_one(g_svcs[i], now_ns);
    spin_unlock_irqrestore(&g_svcs_lock, s);
}

/* No upstream configured: answer the guest itself with SERVFAIL. */
static void dns_servfail(struct tapsvc *svc, uint8_t *buf, uint32_t n, const struct netaddr *to)
{
    buf[2] |= 0x80;                        /* QR = response */
    buf[3] = (uint8_t)((buf[3] & 0xf0) | 2);   /* RCODE = 2 (server failure) */
    STAT(dns_servfail);
    ksock_sendto(svc->gsock, buf, n, to);
}

/* Guest side: read a query, lend an upstream id, forward it. */
static void dns_guest_main(void *arg)
{
    struct tapsvc *svc = arg;
    uint8_t buf[512];
    while (svc->running) {
        struct netaddr from;
        int64_t n = ksock_recvfrom(svc->gsock, buf, sizeof(buf), &from);
        if (n < DNS_HDR) { if (n <= 0) break; continue; }
        STAT(dns_query);
        if (!svc->up_set) { dns_servfail(svc, buf, (uint32_t)n, &from); continue; }
        uint16_t gid = (uint16_t)(buf[0] << 8 | buf[1]);
        uint16_t uid;
        if (!dns_alloc(svc, gid, &from, &uid)) { STAT(dns_drop_full); continue; }
        buf[0] = (uint8_t)(uid >> 8); buf[1] = (uint8_t)uid;
        struct netaddr up;
        memset(&up, 0, sizeof(up));
        up.family = COSMO_AF_INET; up.port = svc->up_port; up.v4 = svc->up_ip;
        ksock_sendto(svc->usock, buf, (size_t)n, &up);
    }
    thread_exit(0);
}

/* Upstream side: read an answer, restore the guest id, relay it back. */
static void dns_up_main(void *arg)
{
    struct tapsvc *svc = arg;
    uint8_t buf[512];
    while (svc->running) {
        struct netaddr from;
        int64_t n = ksock_recvfrom(svc->usock, buf, sizeof(buf), &from);
        if (n < DNS_HDR) { if (n <= 0) break; continue; }
        /* Only the configured upstream may answer: an unconnected socket
         * accepts packets from anyone, and matching on the id alone would let
         * a reachable attacker race a forged response into the guest. */
        if (from.v4 != svc->up_ip || from.port != svc->up_port)
            continue;
        uint16_t uid = (uint16_t)(buf[0] << 8 | buf[1]);
        struct netaddr guest; uint16_t gid;
        if (!dns_take(svc, uid, &guest, &gid))
            continue;                         /* no live entry: drop */
        buf[0] = (uint8_t)(gid >> 8); buf[1] = (uint8_t)gid;
        STAT(dns_answer);
        ksock_sendto(svc->gsock, buf, (size_t)n, &guest);
    }
    thread_exit(0);
}

/* Parse a dotted-quad into a network-order address; false on malformed. */
static bool dns_parse_ip(const char *s, uint32_t *out)
{
    uint32_t o[4] = {0}; int part = 0, digits = 0; uint32_t v = 0;
    for (;; s++) {
        if (*s >= '0' && *s <= '9') { v = v * 10 + (uint32_t)(*s - '0'); digits++; if (v > 255) return false; }
        else if (*s == '.' || *s == 0) {
            if (!digits || part > 3) return false;
            o[part++] = v; v = 0; digits = 0;
            if (*s == 0) break;
        } else return false;
    }
    if (part != 4) return false;
    *out = IPV4_ADDR(o[0], o[1], o[2], o[3]);
    return true;
}

static void dns_stop(struct tapsvc *svc)
{
    svc->running = false;
    if (svc->gsock) ksock_shutdown(svc->gsock, COSMO_SHUT_RD);
    if (svc->usock) ksock_shutdown(svc->usock, COSMO_SHUT_RD);
    if (svc->gth) { thread_join(svc->gth); svc->gth = NULL; }
    if (svc->uth) { thread_join(svc->uth); svc->uth = NULL; }
    if (svc->gsock) { ksock_put(svc->gsock); svc->gsock = NULL; }
    if (svc->usock) { ksock_put(svc->usock); svc->usock = NULL; }
}

static void dns_start(struct tapsvc *svc)
{
    char cfg[32];
    if (fwcfg_get_string("resolver", cfg, sizeof(cfg)) && dns_parse_ip(cfg, &svc->up_ip)) {
        svc->up_port = DNS_PORT;
        svc->up_set = true;
    }
    /* Bound to this tap's own gateway address: a proxy on 0.0.0.0:53 would
     * also answer DNS on the host's real interface, exposing a resolver. */
    struct netaddr me;
    memset(&me, 0, sizeof(me));
    me.family = COSMO_AF_INET; me.port = DNS_PORT; me.v4 = svc->gateway;
    if (ksock_create(COSMO_AF_INET, COSMO_SOCK_DGRAM, 0, &svc->gsock) != 0)
        return;
    if (ksock_bind(svc->gsock, &me) != 0) { ksock_put(svc->gsock); svc->gsock = NULL; return; }
    if (ksock_create(COSMO_AF_INET, COSMO_SOCK_DGRAM, 0, &svc->usock) != 0) {
        ksock_put(svc->gsock); svc->gsock = NULL; return;
    }
    struct netaddr any;
    memset(&any, 0, sizeof(any));
    any.family = COSMO_AF_INET;
    ksock_bind(svc->usock, &any);          /* an ephemeral local port */

    svc->running = true;
    svc->gth = thread_create(dns_guest_main, svc, "dns-guest", SCHED_PRIO_DEFAULT);
    svc->uth = thread_create(dns_up_main, svc, "dns-up", SCHED_PRIO_DEFAULT);
    if (svc->gth == NULL || svc->uth == NULL) {
        kwarn("tapsvc: DNS proxy threads not started on %s; rolling back", svc->nif->name);
        dns_stop(svc);            /* tear the half-started service fully down */
        return;
    }
    kinfo("tapsvc: DNS proxy on %s:53 (upstream %sconfigured)", svc->nif->name, svc->up_set ? "" : "un");
}

void tapsvc_test_set_upstream(struct tapsvc *svc, uint32_t ip, uint16_t port)
{
    svc->up_ip = ip;
    svc->up_port = port;
    svc->up_set = ip != 0;   /* ip 0 clears it: the proxy then answers SERVFAIL */
}

/* --- lifecycle ------------------------------------------------------------ */

struct tapsvc *tapsvc_start(struct tap *t)
{
    struct netif *nif = tap_netif(t);
    struct tapsvc *svc = kmalloc(sizeof(*svc), KMEM_ZERO);
    if (svc == NULL)
        return NULL;
    spinlock_init(&svc->lock, "tapsvc");
    spinlock_init(&svc->dns_lock, "tapsvc-dns");
    svc->tap = t;
    svc->nif = nif;
    svc->gateway = nif->ip4.addr;
    svc->mask = nif->ip4.mask;
    svc->guest = (nif->ip4.addr & nif->ip4.mask) | htonl(TAPSVC_GUEST_HOST);

    arch_irq_state_t s = spin_lock_irqsave(&g_svcs_lock);
    int slot = -1;
    for (unsigned i = 0; i < TAPSVC_MAX; i++)
        if (g_svcs[i] == NULL) { slot = (int)i; break; }
    if (slot >= 0)
        g_svcs[slot] = svc;
    spin_unlock_irqrestore(&g_svcs_lock, s);
    if (slot < 0) {                   /* more instances than taps can exist */
        kfree(svc);
        return NULL;
    }

    tap_set_input_filter(t, dhcp_filter, svc);
    dns_start(svc);
    kinfo("tapsvc: DHCP up on %s (gateway %u.%u.%u.%u, guest .%u)", nif->name,
          (ntohl(svc->gateway) >> 24) & 0xff, (ntohl(svc->gateway) >> 16) & 0xff,
          (ntohl(svc->gateway) >> 8) & 0xff, ntohl(svc->gateway) & 0xff, TAPSVC_GUEST_HOST);
    return svc;
}

/* Stop the service (the DHCP filter first so no reply is built for a tap
 * about to go, then the DNS threads joined before their sockets drop) and
 * free it. The caller destroys the tap afterwards. */
void tapsvc_stop(struct tapsvc *svc)
{
    if (svc == NULL)
        return;
    arch_irq_state_t s = spin_lock_irqsave(&g_svcs_lock);
    for (unsigned i = 0; i < TAPSVC_MAX; i++)
        if (g_svcs[i] == svc) { g_svcs[i] = NULL; break; }
    spin_unlock_irqrestore(&g_svcs_lock, s);
    tap_set_input_filter(svc->tap, NULL, NULL);
    dns_stop(svc);
    kfree(svc);
}

void tapsvc_get_stats(struct tapsvc_stats *out)
{
    *out = g_stats;
    uint32_t live = 0;
    uint64_t now = clock_now_ns();
    arch_irq_state_t s = spin_lock_irqsave(&g_svcs_lock);
    for (unsigned k = 0; k < TAPSVC_MAX; k++) {
        struct tapsvc *svc = g_svcs[k];
        if (svc == NULL) continue;
        arch_irq_state_t d = spin_lock_irqsave(&svc->dns_lock);
        for (unsigned i = 0; i < DNS_PENDING_MAX; i++)
            if (svc->dns_tab[i].in_use && now < svc->dns_tab[i].expires_ns)
                live++;
        spin_unlock_irqrestore(&svc->dns_lock, d);
    }
    spin_unlock_irqrestore(&g_svcs_lock, s);
    out->dns_pending = live;
}
