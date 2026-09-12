/*
 * fw.c - The firewall (docs/audit/next-subsystem-firewall.md,
 * docs/audit/next-subsystem-input-chain.md, docs/audit/next-subsystem-host-input.md).
 *
 * See fw.h for the model. Everything here is under one lock, g_fw_lock:
 * the per-guest rule lists and policies, the host object, the guest
 * attachments, and the stateful flow table. Order against the rest of the stack: g_fw_lock ->
 * g_nat_lock (the filter runs before NAT and never holds NAT's lock); the
 * verdict path takes no other lock. The host-state unit added a second
 * caller, fw_host_record from ipv4_output -- also holding nothing else: NAT
 * decides under g_nat_lock and transmits after releasing it
 * (nat_forward_to), so the output path never arrives here with NAT's lock
 * held, and the order is unchanged.
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
    uint8_t  policy[FW_DIR_COUNT];        /* [0] TO_UPLINK, [1] TO_GUEST, [2] TO_HOST, [3] FROM_UPLINK (host only) */
    unsigned nrules;
    struct fw_rule rules[FW_RULES_PER_GUEST];
};

/* A tracked flow: `a` sent the accepted first packet to `b`. For ICMP echo
 * a_port is the echo identifier and b_port is 0. Two kinds share the table,
 * told apart by the initiator in `guest_ip` and separated by their shares: a
 * guest-to-guest flow recorded by the FORWARD chain, and a flow the *host*
 * opened (guest_ip == FW_HOST_GUEST_IP) recorded at ipv4_output so the host
 * chain admits its reply. */
struct fw_flow {
    bool     in_use;
    bool     est;                         /* TCP: an ACK without SYN has passed */
    uint8_t  proto;
    uint32_t guest_ip;                    /* the initiator, for its share */
    uint32_t a_ip, b_ip;                  /* network order */
    uint16_t a_port, b_port;              /* host order */
    uint64_t expires_ns;
};

/* The defaults: the world stays reachable; a neighbour and the host's own
 * services do not -- except what the tap offers, seeded as rules below. The
 * host chain's default is ACCEPT: the host runs services meant to be reached
 * from the uplink, and the operator hardens by rule or flips it. */
#define POLICY_TO_UPLINK_DEFAULT   FW_ACCEPT
#define POLICY_TO_GUEST_DEFAULT    FW_DROP
#define POLICY_TO_HOST_DEFAULT     FW_DROP
#define POLICY_FROM_UPLINK_DEFAULT FW_ACCEPT
/* The host's own egress: ACCEPT. A default DROP here would stop the tap
 * services answering guests, the host's replies on its uplink, nat_in's
 * deliveries of masqueraded and DNAT'd traffic, and the machine's own name
 * resolution -- all at once; seeding all of that back is a copy of the
 * implementation, not a policy. The operator hardens by rule. */
#define POLICY_OUTPUT_DEFAULT      FW_ACCEPT

static struct fw_guest g_guests[FW_MAX_GUESTS];
/* The host object: a permanent policy object outside the guest table, so no
 * datagram's source (guest_find on iph->src) can ever name it -- only the
 * control path reaches it, through FW_HOST_GUEST_IP (policy_find below). */
static struct fw_guest g_host = { .attached = true, .ip = FW_HOST_GUEST_IP,
                                  .policy = { [3] = POLICY_FROM_UPLINK_DEFAULT,
                                              [4] = POLICY_OUTPUT_DEFAULT } };
static struct fw_flow g_flows[FW_FLOW_MAX];
/* The host's most recent flow. Two jobs, both hints, both validated before
 * use: a one-entry cache for the case that dominates the host's *send* path
 * (the same tuple again -- one socket to one peer, as the DNS proxy's
 * upstream does), and, by being NULL until the host has any flow at all, the
 * answer to "is there any host state to look for?" on the *receive* path,
 * which is the uplink's and must not walk the table for nothing. A stale
 * non-NULL costs a scan that finds nothing; it cannot cost correctness. */
static struct fw_flow *g_host_last;
/* "The OUTPUT chain has nothing to say": no rule of the host's names OUTPUT
 * and its OUTPUT default is ACCEPT, so every host-originated datagram is
 * accepted whatever it is. Maintained under g_fw_lock on every change to the
 * host object and read without it, so the send path -- which is every send
 * this machine makes, TCP's segments included -- pays one relaxed load
 * instead of a transport-header copy, a key, a lock and a walk. */
static uint32_t g_out_fast = 1;
static spinlock_t g_fw_lock = SPINLOCK_INIT("fw");
static struct fw_stats g_stats;

#define STAT(f) __atomic_fetch_add(&g_stats.f, 1, __ATOMIC_RELAXED)

static inline unsigned dir_slot(uint8_t dir)
{
    return dir == FW_DIR_TO_GUEST ? 1u : dir == FW_DIR_TO_HOST ? 2u : dir == FW_DIR_FROM_UPLINK ? 3u
           : dir == FW_DIR_OUTPUT ? 4u : 0u;
}

/* --- guests (caller holds g_fw_lock) -------------------------------------- */

/* A datagram's guest, by its source address: never the host object. */
static struct fw_guest *guest_find(uint32_t ip)
{
    for (unsigned i = 0; i < FW_MAX_GUESTS; i++)
        if (g_guests[i].attached && g_guests[i].ip == ip)
            return &g_guests[i];
    return NULL;
}

/* Recompute the OUTPUT fast path. Caller holds g_fw_lock, and must call this
 * after anything that changes the host's rules or its OUTPUT default. */
static void out_fast_update(void)
{
    uint32_t fast = g_host.policy[4] == FW_ACCEPT;
    for (unsigned i = 0; i < g_host.nrules && fast; i++)
        if (g_host.rules[i].direction == FW_DIR_OUTPUT)
            fast = 0;
    __atomic_store_n(&g_out_fast, fast, __ATOMIC_RELAXED);
}

/* A control operation's policy object: the host for FW_HOST_GUEST_IP, else
 * the attached guest of that address. */
static struct fw_guest *policy_find(uint32_t ip)
{
    return ip == FW_HOST_GUEST_IP ? &g_host : guest_find(ip);
}

/* The host object "as shipped": the default and no rules. There is no small
 * known set of services the uplink is offered, so nothing is seeded. */
static void host_reset(void)
{
    memset(&g_host, 0, sizeof(g_host));
    g_host.attached = true;
    g_host.ip = FW_HOST_GUEST_IP;
    g_host.policy[3] = POLICY_FROM_UPLINK_DEFAULT;
    g_host.policy[4] = POLICY_OUTPUT_DEFAULT;
    out_fast_update();
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

/* A well-formed rule for its owner. The owner owns the direction: FROM_UPLINK
 * (what the world may ask of the host) and OUTPUT (what the host may send)
 * are the host's and nothing else is; and only the host's rules may name a
 * source (a guest's source is the guest, so its tuple stays canonical at
 * 0/0). The world does not send "as a guest", and a guest's wildcard cannot
 * grow into the more sensitive scope. An egress scope narrows an OUTPUT rule
 * and means nothing anywhere else, so it must be ANY there. */
static bool rule_valid(const struct fw_rule *r, bool host_scoped)
{
    if (r->direction > FW_DIR_OUTPUT || r->verdict > FW_ACCEPT || r->dst_prefix > 32 || r->src_prefix > 32)
        return false;
    if (r->scope > FW_SCOPE_GUEST)
        return false;
    bool host_dir = r->direction == FW_DIR_FROM_UPLINK || r->direction == FW_DIR_OUTPUT;
    if (host_dir != host_scoped)
        return false;
    if (r->scope != FW_SCOPE_ANY && r->direction != FW_DIR_OUTPUT)
        return false;
    if (r->proto != 0 && r->proto != IPPROTO_TCP && r->proto != IPPROTO_UDP && r->proto != IPPROTO_ICMP)
        return false;
    if (r->proto == IPPROTO_ICMP && r->dst_port > 255 && r->dst_port != FW_ICMP_TYPE_ANY)
        return false;                     /* for ICMP the selector is a type, or the wildcard */
    if (r->dst_prefix == 0 && r->dst_ip != 0)
        return false;                     /* "any destination" is written as 0/0 */
    if (r->src_prefix == 0 && r->src_ip != 0)
        return false;                     /* "any source" likewise */
    if (!host_scoped && r->src_prefix != 0)
        return false;                     /* a guest's source is the guest */
    return true;
}

static bool rule_same(const struct fw_rule *a, const struct fw_rule *b)
{
    return a->direction == b->direction && a->proto == b->proto && a->dst_prefix == b->dst_prefix &&
           a->verdict == b->verdict && a->dst_ip == b->dst_ip && a->dst_port == b->dst_port &&
           a->src_prefix == b->src_prefix && a->src_ip == b->src_ip && a->scope == b->scope;
}

int fw_rule_add(uint32_t guest_ip, unsigned at_index, const struct fw_rule *r)
{
    if (!rule_valid(r, guest_ip == FW_HOST_GUEST_IP))
        return -EINVAL;
    /* Liveness and insertion under one hold: a release's purge (also under
     * g_fw_lock) either ran first -- the guest is gone and this is refused --
     * or runs after and removes what was inserted. No window. */
    arch_irq_state_t s = spin_lock_irqsave(&g_fw_lock);
    struct fw_guest *g = policy_find(guest_ip);
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
    out_fast_update();
    spin_unlock_irqrestore(&g_fw_lock, s);
    return 0;
}

int fw_rule_del(uint32_t guest_ip, const struct fw_rule *match)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_fw_lock);
    struct fw_guest *g = policy_find(guest_ip);
    int rc = -ENOENT;
    if (g != NULL) {
        for (unsigned i = 0; i < g->nrules; i++) {
            if (rule_same(&g->rules[i], match)) {
                for (unsigned j = i + 1; j < g->nrules; j++)
                    g->rules[j - 1] = g->rules[j];
                g->nrules--;
                out_fast_update();
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
    struct fw_guest *g = policy_find(guest_ip);
    unsigned n = 0;
    if (g != NULL)
        for (; n < g->nrules && n < max; n++)
            out[n] = g->rules[n];
    spin_unlock_irqrestore(&g_fw_lock, s);
    return n;
}

int fw_policy_set(uint32_t guest_ip, uint8_t direction, uint8_t verdict)
{
    if (verdict > FW_ACCEPT)
        return -EINVAL;
    /* The scope owns the direction, as for a rule: the host's is FROM_UPLINK
     * alone, a guest's are the three that describe its own datagrams. */
    bool host_scoped = guest_ip == FW_HOST_GUEST_IP;
    if (host_scoped ? (direction != FW_DIR_FROM_UPLINK && direction != FW_DIR_OUTPUT)
                    : (direction < FW_DIR_TO_UPLINK || direction > FW_DIR_TO_HOST))
        return -EINVAL;
    arch_irq_state_t s = spin_lock_irqsave(&g_fw_lock);
    struct fw_guest *g = policy_find(guest_ip);
    if (g != NULL) {
        g->policy[dir_slot(direction)] = verdict;
        out_fast_update();
    }
    spin_unlock_irqrestore(&g_fw_lock, s);
    return g != NULL ? 0 : -ENOENT;
}

int fw_policy_get(uint32_t guest_ip, uint8_t *to_uplink, uint8_t *to_guest, uint8_t *to_host,
                  uint8_t *from_uplink, uint8_t *output)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_fw_lock);
    struct fw_guest *g = policy_find(guest_ip);
    if (g != NULL) {
        *to_uplink = g->policy[0];
        *to_guest = g->policy[1];
        *to_host = g->policy[2];
        *from_uplink = g->policy[3];
        *output = g->policy[4];
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

static inline bool prefix_matches(uint32_t addr, uint32_t net, uint8_t prefix)
{
    if (prefix == 0)
        return true;
    uint32_t mask = htonl(prefix == 32 ? 0xffffffffu : ~(0xffffffffu >> prefix));
    return ((addr ^ net) & mask) == 0;
}

static bool rule_matches(const struct fw_rule *r, uint8_t dir, const struct ipv4_hdr *iph,
                         const struct l4_view *v, uint8_t scope)
{
    /* An OUTPUT rule may name the egress it applies to; every other rule
     * carries FW_SCOPE_ANY (rule_valid) and this test passes trivially. */
    if (r->scope != FW_SCOPE_ANY && r->scope != scope)
        return false;
    /* ANY keeps its original meaning -- either *forwarding* direction. It
     * never reaches the host: a wildcard written to permit forwarding must
     * not, by a host chain's arrival, silently open a host service; host
     * traffic needs an explicit TO_HOST or FROM_UPLINK rule. */
    if (r->direction == FW_DIR_ANY ? (dir != FW_DIR_TO_UPLINK && dir != FW_DIR_TO_GUEST) : r->direction != dir)
        return false;
    if (r->proto != 0 && r->proto != iph->proto)
        return false;
    if (!prefix_matches(iph->dst, r->dst_ip, r->dst_prefix) || !prefix_matches(iph->src, r->src_ip, r->src_prefix))
        return false;
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

/* A free slot for a new flow, if this initiator is still within its share.
 * NULL when the table is full or the share is spent -- the caller decides what
 * that means (the FORWARD chain refuses the flow; the host records nothing and
 * sends anyway). Caller holds g_fw_lock. */
static struct fw_flow *flow_slot(uint32_t initiator, unsigned quota, uint64_t now)
{
    unsigned mine = 0;
    struct fw_flow *slot = NULL;
    for (unsigned i = 0; i < FW_FLOW_MAX; i++) {
        struct fw_flow *f = &g_flows[i];
        if (f->in_use && now < f->expires_ns) {
            if (f->guest_ip == initiator)
                mine++;
        } else if (slot == NULL) {
            slot = f;
        }
    }
    return mine >= quota ? NULL : slot;
}

/* Fill a slot with a flow `a_ip` opened to `b_ip`. Caller holds g_fw_lock. */
static void flow_fill(struct fw_flow *slot, uint32_t initiator, uint8_t proto, uint32_t a_ip, uint32_t b_ip,
                      const struct l4_view *v, uint64_t now)
{
    memset(slot, 0, sizeof(*slot));
    slot->in_use = true;
    slot->proto = proto;
    slot->guest_ip = initiator;
    slot->a_ip = a_ip;
    slot->b_ip = b_ip;
    if (proto == IPPROTO_ICMP) {
        slot->a_port = v->icmp_id;
    } else {
        slot->a_port = v->sport;
        slot->b_port = v->dport;
    }
    slot->expires_ns = now + flow_timeout(slot);
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
            if (rule_matches(&g->rules[i], dir, iph, &v, FW_SCOPE_ANY)) {
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
        struct fw_flow *slot = flow_slot(iph->src, FW_FLOW_QUOTA_PER_GUEST, now);
        if (slot == NULL) {
            spin_unlock_irqrestore(&g_fw_lock, s);
            STAT(flow_drop_full);
            return FW_DROP;
        }
        flow_fill(slot, iph->src, iph->proto, iph->src, iph->dst, &v, now);
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
            if (rule_matches(&g->rules[i], FW_DIR_TO_HOST, iph, &v, FW_SCOPE_ANY)) {
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

/* --- the host chain ------------------------------------------------------- */

/* The host is sending: the flow whose reply the chain must admit, if any. */
bool fw_host_flow_of(struct netif *out, struct mbuf *m, uint32_t src, uint32_t dst, uint8_t proto,
                     struct fw_host_flow *f)
{
    /* Only the world's links. A guest tap's egress carries the guest's own
     * traffic (whose state is NAT's or the FORWARD chain's), and nothing
     * delivered over loopback ever reaches a chain. */
    if (out->flags & (NETIF_MASQUERADE | NETIF_LOOPBACK))
        return false;
    if (proto != IPPROTO_UDP && proto != IPPROTO_ICMP)
        return false;                        /* see fw.h: TCP is deliberately not recorded */
    struct l4_view v;
    l4_read(m, 0, proto, &v);                /* the IP header is prepended after this: transport at 0 */
    if (!flow_trackable(proto, &v))
        return false;
    memset(f, 0, sizeof(*f));
    f->proto = proto;
    f->src = src;
    f->dst = dst;
    if (proto == IPPROTO_ICMP) {
        f->a_port = v.icmp_id;
    } else {
        f->a_port = v.sport;
        f->b_port = v.dport;
    }
    return true;
}

/* The stack accepted that datagram for transmission: record its flow. */
void fw_host_record(const struct fw_host_flow *hf)
{
    uint8_t proto = hf->proto;
    uint32_t src = hf->src, dst = hf->dst;
    /* flow_find and flow_fill read the tuple through the shapes the receive
     * path hands them; on this path the header does not exist and the
     * transport was parsed before the send, so both are rebuilt here. */
    struct ipv4_hdr key;
    memset(&key, 0, sizeof(key));
    key.proto = proto;
    key.src = src;
    key.dst = dst;
    struct l4_view v;
    memset(&v, 0, sizeof(v));
    v.ok = true;
    if (proto == IPPROTO_ICMP) {
        v.icmp_type = ICMP_ECHO;
        v.icmp_id = hf->a_port;
    } else {
        v.ports = true;
        v.sport = hf->a_port;
        v.dport = hf->b_port;
    }

    uint64_t now = clock_now_ns();
    arch_irq_state_t s = spin_lock_irqsave(&g_fw_lock);
    /* The same tuple again: refresh without walking the table. This is the
     * host's ordinary case -- one socket sending to one peer -- and the send
     * path is the uplink's, so it must not pay for the whole table. */
    struct fw_flow *last = g_host_last;
    if (last != NULL && last->in_use && now < last->expires_ns && last->guest_ip == FW_HOST_GUEST_IP &&
        last->proto == proto && last->a_ip == src && last->b_ip == dst &&
        (proto == IPPROTO_ICMP ? last->a_port == v.icmp_id
                               : last->a_port == v.sport && last->b_port == v.dport)) {
        last->expires_ns = now + flow_timeout(last);
        spin_unlock_irqrestore(&g_fw_lock, s);
        return;
    }
    bool rev = false;
    struct fw_flow *f = flow_find(&key, &v, now, &rev);
    if (f != NULL && !rev) {
        f->expires_ns = now + flow_timeout(f);   /* a send on a live flow refreshes it */
        g_host_last = f;
        spin_unlock_irqrestore(&g_fw_lock, s);
        return;
    }
    struct fw_flow *slot = flow_slot(FW_HOST_GUEST_IP, FW_FLOW_QUOTA_HOST, now);
    if (slot == NULL) {
        spin_unlock_irqrestore(&g_fw_lock, s);
        STAT(hin_flow_drop_full);
        return;                              /* the datagram still goes out; its reply takes the rules */
    }
    flow_fill(slot, FW_HOST_GUEST_IP, proto, src, dst, &v, now);
    g_host_last = slot;
    spin_unlock_irqrestore(&g_fw_lock, s);
    STAT(hin_flow_new);
}

enum fw_verdict fw_host_verdict(struct netif *nif, struct mbuf *m,
                                const struct ipv4_hdr *iph, unsigned ihl)
{
    (void)nif;   /* every real link shares the one host chain; per-interface chains are a later unit */
    struct l4_view v;
    l4_read(m, ihl, iph->proto, &v);
    uint64_t now = clock_now_ns();

    arch_irq_state_t s = spin_lock_irqsave(&g_fw_lock);

    /* State before rules, as the FORWARD chain does: a datagram that is the
     * reverse of a flow the *host* opened is admitted before any rule is
     * read -- a reply was never what a rule was written about. Exactly one
     * tuple is open (the peer address and port the host sent to, back to the
     * port it sent from), for the life of the flow; the firewall cannot know
     * whether the peer meant it as a reply, so a second datagram on that
     * tuple inside the window is admitted too and the socket's own
     * validation is the second line. A *forward* match on a real link could
     * only be a datagram carrying one of our own addresses as its source,
     * which ipv4_input drops as a martian before this chain; it is not
     * admitted here either. A guest's flow admits nothing here: this chain
     * is the host's. */
    if (v.ok && g_host_last != NULL) {   /* no host flow has ever been recorded: nothing to find */
        bool rev = false;
        struct fw_flow *f = flow_find(iph, &v, now, &rev);
        if (f != NULL && rev && f->guest_ip == FW_HOST_GUEST_IP) {
            f->expires_ns = now + flow_timeout(f);
            spin_unlock_irqrestore(&g_fw_lock, s);
            STAT(hin_accept_established);
            return FW_ACCEPT;
        }
    }

    /* Else the host's rules first-match, else its default. No connection
     * state here: on DROP the caller delivers a TCP/UDP datagram, and an
     * ICMP one, quiet (M_FW_QUIET) and the transport -- which owns
     * acceptability -- admits only what an existing connection, a connected
     * socket, or (for ICMP) TCP's own path-MTU confirmation accepts,
     * answering nothing else. The firewall does not re-derive TCP's tests. */
    enum fw_verdict verdict = (enum fw_verdict)g_host.policy[dir_slot(FW_DIR_FROM_UPLINK)];
    bool by_rule = false;
    for (unsigned i = 0; i < g_host.nrules; i++)
        if (rule_matches(&g_host.rules[i], FW_DIR_FROM_UPLINK, iph, &v, FW_SCOPE_ANY)) {
            verdict = (enum fw_verdict)g_host.rules[i].verdict;
            by_rule = true;
            break;
        }
    spin_unlock_irqrestore(&g_fw_lock, s);

    if (verdict == FW_ACCEPT) {
        if (by_rule) STAT(hin_accept_rule); else STAT(hin_accept_default);
    } else {
        if (by_rule) STAT(hin_drop_rule); else STAT(hin_drop_default);
    }
    return verdict;
}

/* --- the OUTPUT chain ----------------------------------------------------- */

enum fw_verdict fw_output_verdict(struct netif *out, struct mbuf *m, uint32_t src, uint32_t dst,
                                  uint8_t proto)
{
    /* The egress is the scope: a guest's tap, or the world. (Loopback never
     * reaches this function -- ipv4_output does not offer it.) */
    /* Nothing to decide: no OUTPUT rule and an ACCEPT default -- every
     * configuration but a deliberately filtered one, and the state the
     * machine boots in. The send path must not pay for the chain until an
     * operator asks for it; the verdict is still counted, so the statistic
     * keeps meaning "what this chain let out by default". */
    if (__atomic_load_n(&g_out_fast, __ATOMIC_RELAXED)) {
        STAT(out_accept_default);
        return FW_ACCEPT;
    }
    uint8_t scope = (out->flags & NETIF_MASQUERADE) ? FW_SCOPE_GUEST : FW_SCOPE_WORLD;
    /* rule_matches reads the addresses and protocol through a header, which
     * on this path is not built yet, so the key is assembled here as
     * fw_host_record does; the transport sits at offset 0. */
    struct ipv4_hdr key;
    memset(&key, 0, sizeof(key));
    key.proto = proto;
    key.src = src;
    key.dst = dst;
    struct l4_view v;
    l4_read(m, 0, proto, &v);

    arch_irq_state_t s = spin_lock_irqsave(&g_fw_lock);
    enum fw_verdict verdict = (enum fw_verdict)g_host.policy[dir_slot(FW_DIR_OUTPUT)];
    bool by_rule = false;
    for (unsigned i = 0; i < g_host.nrules; i++)
        if (rule_matches(&g_host.rules[i], FW_DIR_OUTPUT, &key, &v, scope)) {
            verdict = (enum fw_verdict)g_host.rules[i].verdict;
            by_rule = true;
            break;
        }
    spin_unlock_irqrestore(&g_fw_lock, s);

    if (verdict == FW_ACCEPT) {
        if (by_rule) STAT(out_accept_rule); else STAT(out_accept_default);
    } else {
        if (by_rule) STAT(out_drop_rule); else STAT(out_drop_default);
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
    g_host_last = NULL;
    for (unsigned i = 0; i < FW_MAX_GUESTS; i++)
        if (g_guests[i].attached)
            guest_reset(&g_guests[i], g_guests[i].ip, g_guests[i].gateway);
    host_reset();
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
    out->rules += g_host.nrules;
    spin_unlock_irqrestore(&g_fw_lock, s);
}
