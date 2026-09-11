/*
 * fw.c - The forwarding firewall (docs/audit/next-subsystem-firewall.md).
 *
 * See fw.h for the model. Everything here is under one lock, g_fw_lock:
 * the per-guest rule lists and policies, the guest attachments, and the
 * stateful flow table. Order against the rest of the stack: g_fw_lock ->
 * g_nat_lock (the filter runs before NAT and never holds NAT's lock); the
 * verdict path takes no other lock.
 *
 * Transport headers are read by byte offset with m_copydata, so the packet
 * is never re-pointed and no pullup is needed here.
 */

#include <kernel/errno.h>
#include <kernel/log.h>
#include <kernel/mbuf.h>
#include <kernel/net/fw.h>
#include <kernel/net/inet.h>
#include <kernel/net/ip.h>
#include <kernel/net/nat.h>
#include <kernel/net/tapsvc.h>
#include <kernel/net/tcp.h>
#include <kernel/netif.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/timer.h>

struct fw_guest {
    bool     attached;
    uint32_t ip;                          /* network order */
    uint32_t gateway;                     /* its tap's host address (network order), for the seeds */
    uint8_t  policy[FW_DIR_COUNT];        /* [0] TO_UPLINK, [1] TO_GUEST, [2] TO_HOST */
    unsigned nrules;
    struct fw_rule rules[FW_RULES_PER_GUEST];
};

/* A guest-to-guest flow: `a` sent the accepted first packet to `b`. For ICMP
 * echo a_port is the echo identifier and b_port is 0. */
struct fw_flow {
    bool     in_use;
    bool     est;                         /* TCP: an ACK without SYN has passed */
    uint8_t  proto;
    uint32_t guest_ip;                    /* the initiator, for its share */
    uint32_t a_ip, b_ip;                  /* network order */
    uint16_t a_port, b_port;              /* host order */
    uint64_t expires_ns;
};

static struct fw_guest g_guests[FW_MAX_GUESTS];
static struct fw_flow g_flows[FW_FLOW_MAX];
static spinlock_t g_fw_lock = SPINLOCK_INIT("fw");
static struct fw_stats g_stats;

#define STAT(f) __atomic_fetch_add(&g_stats.f, 1, __ATOMIC_RELAXED)

/* The defaults: the world stays reachable; a neighbour and the host's own
 * services do not -- except what the tap offers, seeded as rules below. */
#define POLICY_TO_UPLINK_DEFAULT FW_ACCEPT
#define POLICY_TO_GUEST_DEFAULT  FW_DROP
#define POLICY_TO_HOST_DEFAULT   FW_DROP

static inline unsigned dir_slot(uint8_t dir)
{
    return dir == FW_DIR_TO_GUEST ? 1u : dir == FW_DIR_TO_HOST ? 2u : 0u;
}

/* --- guests (caller holds g_fw_lock) -------------------------------------- */

static struct fw_guest *guest_find(uint32_t ip)
{
    for (unsigned i = 0; i < FW_MAX_GUESTS; i++)
        if (g_guests[i].attached && g_guests[i].ip == ip)
            return &g_guests[i];
    return NULL;
}

/* "As attached": the defaults, and the two TO_HOST rules that keep the tap's
 * own services reachable through a default-deny INPUT chain -- ordinary
 * rules, listed and deletable, not hard-coded holes. DHCP needs none (it is
 * answered at the frame level, before the stack). */
static void guest_reset(struct fw_guest *g, uint32_t ip, uint32_t gateway)
{
    memset(g, 0, sizeof(*g));
    g->attached = true;
    g->ip = ip;
    g->gateway = gateway;
    g->policy[0] = POLICY_TO_UPLINK_DEFAULT;
    g->policy[1] = POLICY_TO_GUEST_DEFAULT;
    g->policy[2] = POLICY_TO_HOST_DEFAULT;
    g->rules[0] = (struct fw_rule){ .direction = FW_DIR_TO_HOST, .proto = IPPROTO_UDP, .dst_prefix = 32,
                                    .verdict = FW_ACCEPT, .dst_ip = gateway, .dst_port = 53 };
    g->rules[1] = (struct fw_rule){ .direction = FW_DIR_TO_HOST, .proto = IPPROTO_ICMP, .dst_prefix = 32,
                                    .verdict = FW_ACCEPT, .dst_ip = gateway, .dst_port = ICMP_ECHO };
    g->nrules = 2;
}

void fw_guest_attach(uint32_t guest_ip, uint32_t gateway_ip)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_fw_lock);
    struct fw_guest *g = guest_find(guest_ip);
    if (g == NULL)
        for (unsigned i = 0; i < FW_MAX_GUESTS && g == NULL; i++)
            if (!g_guests[i].attached)
                g = &g_guests[i];
    if (g != NULL)
        guest_reset(g, guest_ip, gateway_ip);
    spin_unlock_irqrestore(&g_fw_lock, s);
    if (g == NULL)
        kwarn("fw: no guest slot for a new tap; its traffic takes the built-in defaults");
}

/* Detach the guest and drop everything that names its address: its rules and
 * policy, and every flow with that address on either side -- a peer's flow to
 * it must not outlive it either, since the address is about to be reused. */
void fw_guest_purge(uint32_t guest_ip)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_fw_lock);
    struct fw_guest *g = guest_find(guest_ip);
    if (g != NULL)
        memset(g, 0, sizeof(*g));         /* attached = false */
    for (unsigned i = 0; i < FW_FLOW_MAX; i++)
        if (g_flows[i].in_use && (g_flows[i].a_ip == guest_ip || g_flows[i].b_ip == guest_ip))
            g_flows[i].in_use = false;
    spin_unlock_irqrestore(&g_fw_lock, s);
}

/* --- rules ---------------------------------------------------------------- */

static bool rule_valid(const struct fw_rule *r)
{
    if (r->direction > FW_DIR_TO_HOST || r->verdict > FW_ACCEPT || r->dst_prefix > 32)
        return false;
    if (r->proto != 0 && r->proto != IPPROTO_TCP && r->proto != IPPROTO_UDP && r->proto != IPPROTO_ICMP)
        return false;
    if (r->proto == IPPROTO_ICMP && r->dst_port > 255 && r->dst_port != FW_ICMP_TYPE_ANY)
        return false;                     /* for ICMP the selector is a type, or the wildcard */
    if (r->dst_prefix == 0 && r->dst_ip != 0)
        return false;                     /* "any destination" is written as 0/0 */
    return true;
}

static bool rule_same(const struct fw_rule *a, const struct fw_rule *b)
{
    return a->direction == b->direction && a->proto == b->proto && a->dst_prefix == b->dst_prefix &&
           a->verdict == b->verdict && a->dst_ip == b->dst_ip && a->dst_port == b->dst_port;
}

int fw_rule_add(uint32_t guest_ip, unsigned at_index, const struct fw_rule *r)
{
    if (!rule_valid(r))
        return -EINVAL;
    /* Liveness and insertion under one hold: a release's purge (also under
     * g_fw_lock) either ran first -- the guest is gone and this is refused --
     * or runs after and removes what was inserted. No window. */
    arch_irq_state_t s = spin_lock_irqsave(&g_fw_lock);
    struct fw_guest *g = guest_find(guest_ip);
    if (g == NULL) {
        spin_unlock_irqrestore(&g_fw_lock, s);
        return -ENOENT;
    }
    for (unsigned i = 0; i < g->nrules; i++)
        if (rule_same(&g->rules[i], r)) {
            spin_unlock_irqrestore(&g_fw_lock, s);
            return -EEXIST;
        }
    if (g->nrules >= FW_RULES_PER_GUEST) {
        spin_unlock_irqrestore(&g_fw_lock, s);
        return -ENOSPC;
    }
    if (at_index > g->nrules)
        at_index = g->nrules;             /* clamp: append */
    for (unsigned i = g->nrules; i > at_index; i--)
        g->rules[i] = g->rules[i - 1];
    g->rules[at_index] = *r;
    g->nrules++;
    spin_unlock_irqrestore(&g_fw_lock, s);
    return 0;
}

int fw_rule_del(uint32_t guest_ip, const struct fw_rule *match)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_fw_lock);
    struct fw_guest *g = guest_find(guest_ip);
    int rc = -ENOENT;
    if (g != NULL) {
        for (unsigned i = 0; i < g->nrules; i++) {
            if (rule_same(&g->rules[i], match)) {
                for (unsigned j = i + 1; j < g->nrules; j++)
                    g->rules[j - 1] = g->rules[j];
                g->nrules--;
                rc = 0;
                break;
            }
        }
    }
    spin_unlock_irqrestore(&g_fw_lock, s);
    return rc;
}

unsigned fw_rule_list(uint32_t guest_ip, struct fw_rule *out, unsigned max)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_fw_lock);
    struct fw_guest *g = guest_find(guest_ip);
    unsigned n = 0;
    if (g != NULL)
        for (; n < g->nrules && n < max; n++)
            out[n] = g->rules[n];
    spin_unlock_irqrestore(&g_fw_lock, s);
    return n;
}

int fw_policy_set(uint32_t guest_ip, uint8_t direction, uint8_t verdict)
{
    if (direction < FW_DIR_TO_UPLINK || direction > FW_DIR_TO_HOST || verdict > FW_ACCEPT)
        return -EINVAL;
    arch_irq_state_t s = spin_lock_irqsave(&g_fw_lock);
    struct fw_guest *g = guest_find(guest_ip);
    if (g != NULL)
        g->policy[dir_slot(direction)] = verdict;
    spin_unlock_irqrestore(&g_fw_lock, s);
    return g != NULL ? 0 : -ENOENT;
}

int fw_policy_get(uint32_t guest_ip, uint8_t *to_uplink, uint8_t *to_guest, uint8_t *to_host)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_fw_lock);
    struct fw_guest *g = guest_find(guest_ip);
    if (g != NULL) {
        *to_uplink = g->policy[0];
        *to_guest = g->policy[1];
        *to_host = g->policy[2];
    }
    spin_unlock_irqrestore(&g_fw_lock, s);
    return g != NULL ? 0 : -ENOENT;
}

unsigned fw_guest_list(uint32_t *out, unsigned max)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_fw_lock);
    unsigned n = 0;
    for (unsigned i = 0; i < FW_MAX_GUESTS && n < max; i++)
        if (g_guests[i].attached)
            out[n++] = g_guests[i].ip;
    spin_unlock_irqrestore(&g_fw_lock, s);
    return n;
}

/* --- the verdict ---------------------------------------------------------- */

/* What the verdict needs from the transport header, read by copy. */
struct l4_view {
    bool     ok;                          /* enough bytes were present */
    bool     ports;                       /* TCP/UDP: sport/dport valid */
    uint16_t sport, dport;                /* host order */
    uint8_t  tcp_flags;
    uint8_t  icmp_type;
    uint16_t icmp_id;                     /* host order (echo only) */
};

static void l4_read(struct mbuf *m, unsigned ihl, uint8_t proto, struct l4_view *v)
{
    uint8_t b[20];
    memset(v, 0, sizeof(*v));
    unsigned need = proto == IPPROTO_TCP ? 14u : 8u;   /* through the TCP flags byte / UDP hdr / ICMP id */
    if (proto != IPPROTO_TCP && proto != IPPROTO_UDP && proto != IPPROTO_ICMP)
        return;
    if (m->pkt.len < ihl + need || !m_copydata(m, ihl, need, b))
        return;
    v->ok = true;
    if (proto == IPPROTO_ICMP) {
        v->icmp_type = b[0];
        v->icmp_id = (uint16_t)(b[4] << 8 | b[5]);
        return;
    }
    v->ports = true;
    v->sport = (uint16_t)(b[0] << 8 | b[1]);
    v->dport = (uint16_t)(b[2] << 8 | b[3]);
    if (proto == IPPROTO_TCP)
        v->tcp_flags = b[13];
}

static bool rule_matches(const struct fw_rule *r, uint8_t dir, const struct ipv4_hdr *iph,
                         const struct l4_view *v)
{
    if (r->direction != FW_DIR_ANY && r->direction != dir)
        return false;
    if (r->proto != 0 && r->proto != iph->proto)
        return false;
    if (r->dst_prefix != 0) {
        uint32_t mask = htonl(r->dst_prefix == 32 ? 0xffffffffu : ~(0xffffffffu >> r->dst_prefix));
        if (((iph->dst ^ r->dst_ip) & mask) != 0)
            return false;
    }
    /* The transport selector follows the protocol. An ICMP rule names a type
     * (or the wildcard) and matches only a datagram whose ICMP header was
     * read; a TCP/UDP/any rule naming a port never matches a datagram that
     * has none. */
    if (r->proto == IPPROTO_ICMP)
        return v->ok && (r->dst_port == FW_ICMP_TYPE_ANY || v->icmp_type == r->dst_port);
    if (r->dst_port != 0 && (!v->ports || v->dport != r->dst_port))
        return false;
    return true;
}

static uint64_t flow_timeout(const struct fw_flow *f)
{
    if (f->proto == IPPROTO_TCP)
        return f->est ? NAT_TIMEOUT_TCPEST_NS : NAT_TIMEOUT_TCP_NS;
    return f->proto == IPPROTO_UDP ? NAT_TIMEOUT_UDP_NS : NAT_TIMEOUT_ICMP_NS;
}

/* Find the flow this datagram belongs to, if any. `*reverse` says whether it
 * travels b -> a (a reply). ICMP: a type-8 request matches only forward, a
 * type-0 reply only reverse, both on the echo id -- a reverse echo *request*
 * is a new flow, not a reply. Caller holds g_fw_lock. */
static struct fw_flow *flow_find(const struct ipv4_hdr *iph, const struct l4_view *v, uint64_t now,
                                 bool *reverse)
{
    for (unsigned i = 0; i < FW_FLOW_MAX; i++) {
        struct fw_flow *f = &g_flows[i];
        if (!f->in_use || now >= f->expires_ns || f->proto != iph->proto)
            continue;
        if (iph->proto == IPPROTO_ICMP) {
            if (f->a_ip == iph->src && f->b_ip == iph->dst && v->icmp_type == ICMP_ECHO &&
                f->a_port == v->icmp_id) { *reverse = false; return f; }
            if (f->a_ip == iph->dst && f->b_ip == iph->src && v->icmp_type == ICMP_ECHO_REPLY &&
                f->a_port == v->icmp_id) { *reverse = true; return f; }
            continue;
        }
        if (f->a_ip == iph->src && f->b_ip == iph->dst && f->a_port == v->sport && f->b_port == v->dport) {
            *reverse = false; return f;
        }
        if (f->a_ip == iph->dst && f->b_ip == iph->src && f->a_port == v->dport && f->b_port == v->sport) {
            *reverse = true; return f;
        }
    }
    return NULL;
}

/* Only these can be tracked: TCP/UDP with ports, and an ICMP echo request. */
static bool flow_trackable(uint8_t proto, const struct l4_view *v)
{
    if (!v->ok)
        return false;
    if (proto == IPPROTO_ICMP)
        return v->icmp_type == ICMP_ECHO;
    return v->ports;
}

enum fw_verdict fw_forward_verdict(struct netif *in, struct netif *out, struct mbuf *m,
                                   const struct ipv4_hdr *iph, unsigned ihl)
{
    (void)in;
    uint8_t dir = (out->flags & NETIF_FORWARD) ? FW_DIR_TO_GUEST : FW_DIR_TO_UPLINK;
    struct l4_view v;
    l4_read(m, ihl, iph->proto, &v);
    uint64_t now = clock_now_ns();

    arch_irq_state_t s = spin_lock_irqsave(&g_fw_lock);

    /* Guest-to-guest: is this half of a flow already accepted? Both halves of
     * an un-NAT'd flow pass here, so the reply is admitted by state. (A
     * guest-to-uplink reply never reaches ipv4_forward -- nat_in delivers it
     * on the strength of the NAT conntrack entry -- so it needs none.) */
    if (dir == FW_DIR_TO_GUEST && v.ok) {
        bool rev = false;
        struct fw_flow *f = flow_find(iph, &v, now, &rev);
        /* A TCP segment with SYN set and ACK clear opens a connection; it is
         * never a reply, whatever its ports. A guest injects arbitrary flags,
         * so a reverse-direction bare SYN on an accepted flow's ports is a new
         * flow from the *other* guest and takes that guest's rules and default
         * -- not the ESTABLISHED shortcut. (The initiator's own SYN, forward,
         * is the packet that made the flow; a retransmit of it is harmless.) */
        bool bare_syn = iph->proto == IPPROTO_TCP && (v.tcp_flags & TH_SYN) && !(v.tcp_flags & TH_ACK);
        if (f != NULL && !(rev && bare_syn)) {
            if (iph->proto == IPPROTO_TCP && (v.tcp_flags & TH_ACK) && !(v.tcp_flags & TH_SYN))
                f->est = true;
            f->expires_ns = now + flow_timeout(f);
            spin_unlock_irqrestore(&g_fw_lock, s);
            STAT(accept_established);
            return FW_ACCEPT;
        }
    }

    /* A NEW datagram: the guest's rules first-match, else its default. A
     * guest we know nothing about (never attached) takes the built-in
     * defaults with no rules. */
    struct fw_guest *g = guest_find(iph->src);
    enum fw_verdict verdict;
    bool by_rule = false;
    if (g != NULL) {
        verdict = (enum fw_verdict)g->policy[dir_slot(dir)];
        for (unsigned i = 0; i < g->nrules; i++)
            if (rule_matches(&g->rules[i], dir, iph, &v)) {
                verdict = (enum fw_verdict)g->rules[i].verdict;
                by_rule = true;
                break;
            }
    } else {
        verdict = dir == FW_DIR_TO_GUEST ? POLICY_TO_GUEST_DEFAULT : POLICY_TO_UPLINK_DEFAULT;
    }

    /* Record an accepted guest-to-guest flow so its reply is admitted. No
     * room in the guest's share means no state, and no state would strand
     * the reply -- so the flow is refused outright, as a full NAT table
     * refuses a new masquerade, and the guest's flood starves only itself. */
    if (verdict == FW_ACCEPT && dir == FW_DIR_TO_GUEST && flow_trackable(iph->proto, &v)) {
        unsigned mine = 0;
        struct fw_flow *slot = NULL;
        for (unsigned i = 0; i < FW_FLOW_MAX; i++) {
            struct fw_flow *f = &g_flows[i];
            if (f->in_use && now < f->expires_ns) {
                if (f->guest_ip == iph->src)
                    mine++;
            } else if (slot == NULL) {
                slot = f;
            }
        }
        if (slot == NULL || mine >= FW_FLOW_QUOTA_PER_GUEST) {
            spin_unlock_irqrestore(&g_fw_lock, s);
            STAT(flow_drop_full);
            return FW_DROP;
        }
        memset(slot, 0, sizeof(*slot));
        slot->in_use = true;
        slot->proto = iph->proto;
        slot->guest_ip = iph->src;
        slot->a_ip = iph->src;
        slot->b_ip = iph->dst;
        if (iph->proto == IPPROTO_ICMP) {
            slot->a_port = v.icmp_id;
        } else {
            slot->a_port = v.sport;
            slot->b_port = v.dport;
        }
        slot->expires_ns = now + flow_timeout(slot);
        STAT(flow_new);
    }
    spin_unlock_irqrestore(&g_fw_lock, s);

    if (verdict == FW_ACCEPT) {
        if (by_rule) STAT(accept_rule); else STAT(accept_default);
    } else {
        if (by_rule) STAT(drop_rule); else STAT(drop_default);
    }
    return verdict;
}

/* --- the INPUT chain ------------------------------------------------------ */

enum fw_verdict fw_input_verdict(struct netif *nif, struct mbuf *m,
                                 const struct ipv4_hdr *iph, unsigned ihl)
{
    /* Anti-spoof, the forwarding rule made on this path too: a masquerading
     * tap is a point-to-point link to one owner at <subnet>.15, so any other
     * source is a forgery -- of a neighbour, of the uplink, of the host
     * itself -- and never reaches a host service, whatever the rules say. */
    if (nif->ip4.addr && nif->ip4.mask &&
        iph->src != ((nif->ip4.addr & nif->ip4.mask) | htonl(TAPSVC_GUEST_HOST))) {
        STAT(in_spoofed);
        return FW_DROP;
    }
    struct l4_view v;
    l4_read(m, ihl, iph->proto, &v);

    /* No state on this chain: the host's reply leaves by ipv4_output and
     * passes no filter, and a guest's later segments match the same rule by
     * destination port. A guest never attached (no tap open made it) fails
     * closed: the built-in default and no seeded rules. */
    arch_irq_state_t s = spin_lock_irqsave(&g_fw_lock);
    struct fw_guest *g = guest_find(iph->src);
    enum fw_verdict verdict = POLICY_TO_HOST_DEFAULT;
    bool by_rule = false;
    if (g != NULL) {
        verdict = (enum fw_verdict)g->policy[dir_slot(FW_DIR_TO_HOST)];
        for (unsigned i = 0; i < g->nrules; i++)
            if (rule_matches(&g->rules[i], FW_DIR_TO_HOST, iph, &v)) {
                verdict = (enum fw_verdict)g->rules[i].verdict;
                by_rule = true;
                break;
            }
    }
    spin_unlock_irqrestore(&g_fw_lock, s);

    if (verdict == FW_ACCEPT) {
        if (by_rule) STAT(in_accept_rule); else STAT(in_accept_default);
    } else {
        if (by_rule) STAT(in_drop_rule); else STAT(in_drop_default);
    }
    return verdict;
}

/* --- maintenance ---------------------------------------------------------- */

void fw_age(uint64_t now_ns)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_fw_lock);
    for (unsigned i = 0; i < FW_FLOW_MAX; i++)
        if (g_flows[i].in_use && now_ns >= g_flows[i].expires_ns) {
            g_flows[i].in_use = false;
            STAT(expired);
        }
    spin_unlock_irqrestore(&g_fw_lock, s);
}

void fw_flush(void)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_fw_lock);
    memset(g_flows, 0, sizeof(g_flows));
    for (unsigned i = 0; i < FW_MAX_GUESTS; i++)
        if (g_guests[i].attached)
            guest_reset(&g_guests[i], g_guests[i].ip, g_guests[i].gateway);
    spin_unlock_irqrestore(&g_fw_lock, s);
}

void fw_get_stats(struct fw_stats *out)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_fw_lock);
    *out = g_stats;
    uint64_t now = clock_now_ns();
    out->flows = 0;
    out->rules = 0;
    for (unsigned i = 0; i < FW_FLOW_MAX; i++)
        if (g_flows[i].in_use && now < g_flows[i].expires_ns)
            out->flows++;
    for (unsigned i = 0; i < FW_MAX_GUESTS; i++)
        if (g_guests[i].attached)
            out->rules += g_guests[i].nrules;
    spin_unlock_irqrestore(&g_fw_lock, s);
}
