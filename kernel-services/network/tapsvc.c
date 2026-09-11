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
#include <kernel/log.h>
#include <kernel/net/cksum.h>
#include <kernel/net/ether.h>
#include <kernel/net/inet.h>
#include <kernel/net/ip.h>
#include <kernel/net/tap.h>
#include <kernel/net/tapsvc.h>
#include <kernel/net/udp.h>
#include <kernel/netif.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>

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

struct tapsvc {
    struct tap *tap;
    struct netif *nif;
    uint32_t gateway;      /* the tap's host address (network order) */
    uint32_t mask;
    uint32_t guest;        /* the one guest's address (network order) */
    bool bound;            /* a client has taken the lease */
    uint8_t client_mac[6];
};

static struct tapsvc g_svc;
static spinlock_t g_svc_lock = SPINLOCK_INIT("tapsvc");
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

    arch_irq_state_t st = spin_lock_irqsave(&g_svc_lock);
    /* One binding: a second, different client is offered nothing. */
    bool mine = !s->bound || memcmp(s->client_mac, chaddr, 6) == 0;

    switch (mt[0]) {
    case DHCP_DISCOVER:
        STAT(dhcp_discover);
        if (!mine) { STAT(dhcp_ignored); spin_unlock_irqrestore(&g_svc_lock, st); return true; }
        spin_unlock_irqrestore(&g_svc_lock, st);
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
        spin_unlock_irqrestore(&g_svc_lock, st);
        if (ok) { STAT(dhcp_ack); dhcp_reply(s, dh, DHCP_ACK); }
        else    { STAT(dhcp_nak); dhcp_reply(s, dh, DHCP_NAK); }
        return true;
    }
    case DHCP_DECLINE:
    case DHCP_RELEASE:
        STAT(dhcp_release);
        if (mine)
            s->bound = false;
        spin_unlock_irqrestore(&g_svc_lock, st);
        return true;
    default:
        STAT(dhcp_ignored);
        spin_unlock_irqrestore(&g_svc_lock, st);
        return true;
    }
}

/* --- lifecycle ------------------------------------------------------------ */

void tapsvc_start(struct tap *t)
{
    struct netif *nif = tap_netif(t);
    arch_irq_state_t st = spin_lock_irqsave(&g_svc_lock);
    if (g_svc.tap != NULL) {          /* one instance at a time */
        spin_unlock_irqrestore(&g_svc_lock, st);
        return;
    }
    memset(&g_svc, 0, sizeof(g_svc));
    g_svc.tap = t;
    g_svc.nif = nif;
    g_svc.gateway = nif->ip4.addr;
    g_svc.mask = nif->ip4.mask;
    g_svc.guest = (nif->ip4.addr & nif->ip4.mask) | htonl(TAPSVC_GUEST_HOST);
    spin_unlock_irqrestore(&g_svc_lock, st);

    tap_set_input_filter(t, dhcp_filter, &g_svc);
    kinfo("tapsvc: DHCP up on %s (gateway %u.%u.%u.%u, guest .%u)", nif->name,
          (ntohl(g_svc.gateway) >> 24) & 0xff, (ntohl(g_svc.gateway) >> 16) & 0xff,
          (ntohl(g_svc.gateway) >> 8) & 0xff, ntohl(g_svc.gateway) & 0xff, TAPSVC_GUEST_HOST);
}

void tapsvc_stop(void)
{
    arch_irq_state_t st = spin_lock_irqsave(&g_svc_lock);
    struct tap *t = g_svc.tap;
    g_svc.tap = NULL;
    spin_unlock_irqrestore(&g_svc_lock, st);
    if (t != NULL)
        tap_set_input_filter(t, NULL, NULL);
}

void tapsvc_get_stats(struct tapsvc_stats *out)
{
    *out = g_stats;
}
