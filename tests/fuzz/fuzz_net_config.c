/*
 * fuzz_net_config.c - The firewall and NAT rule editors, interleaved with
 * frames.
 *
 * The real stack on the host (shim_net.c), with an uplink `fz0`
 * (10.0.2.15/24, gateway 10.0.2.2) and two masquerading guest taps, `fz1`
 * (10.75.0.1/24, guest A 10.75.0.15) and `fz2` (10.76.0.1/24, guest B
 * 10.76.0.15), each guest attached to the firewall as a tap attaches it,
 * and a host UDP socket on :7. The input is a sequence of records: an
 * operation byte, then the operation's arguments (a fixed number of bytes
 * for each; a short input reads zeros). The operations: add a firewall
 * rule (to the host object, guest A, guest B, or a guest nobody attached),
 * delete one (one the model holds, or one built from the arguments), set a
 * policy, flush the firewall; add, delete and clear port forwards; release
 * a guest as its tap's release does (fw_guest_purge, nat_guest_purge) or
 * attach it again; flush NAT; let time pass; send from the host (the OUTPUT
 * chain); deliver a frame built from a template -- the uplink to the host
 * or a forwarded port, a guest to the world, to the host or to the other
 * guest, a reply to a translation the NAT table holds.
 *
 * The target keeps a model of what it configured: each owner's rule list
 * in order and its policies, which guests are attached, the port-forward
 * set. Oracles, after every record: every spinlock released; each owner's
 * listing is the model's list, in order (a listing round-trips what was
 * added; a delete removes exactly one); each owner's policies are the
 * model's; the attached guests are the model's; the tables within their
 * capacities (rules per owner, port forwards, guests, flows); every return
 * code is the one the model predicts (a duplicate of an installed rule is
 * -EEXIST, a full list -ENOSPC, a missing owner -ENOENT, a port forward to
 * an address on no forwarding tap -EINVAL); the OUTPUT fast path agrees
 * with the host's rules and OUTPUT default; every DNAT translation the NAT
 * table holds belongs to a port forward the model holds (a deleted forward
 * translates nothing more); no NAT entry or firewall flow names a released
 * guest. As a frame is decided: every rule that decides a verdict is
 * installed in the model, for that owner, at that moment -- a removed rule
 * never matches again. After teardown: no mbuf alive, the allocator back
 * at its baseline, every table empty.
 *
 * A stateful flow that a rule accepted outlives the rule, as conntrack's
 * do: a frame it passes is passed by the flow, not by the removed rule, and
 * is not held to it. A translation does not outlive its forward. That is
 * network invariant N28 (state that copies a rule ends with it, state that
 * records admitted traffic does not), and both halves are oracles: the
 * DNAT check above, and a rule, policy or forward change that leaves the
 * flows (and, for a forward, the masquerade entries) exactly as they were.
 */

#include <kernel/net/cksum.h>
#include <kernel/net/ether.h>
#include <kernel/net/fw.h>
#include <kernel/net/inet.h>
#include <kernel/net/ip.h>
#include <kernel/net/nat.h>
#include <kernel/net/tcp.h>
#include <kernel/net/udp.h>
#include <kernel/socket.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fuzz.h"
#include "harness.h"
#include "netpkt.h"
#include "shim_net.h"

static const uint8_t k_mac0[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
static const uint8_t k_mac1[6] = { 0x02, 0xf2, 0x00, 0x00, 0x00, 0x01 };
static const uint8_t k_mac2[6] = { 0x02, 0xf2, 0x00, 0x00, 0x00, 0x02 };
static const uint8_t k_peer_mac[6] = { 0x52, 0x55, 0x0a, 0x00, 0x02, 0x02 };
static const uint8_t k_guest_a_mac[6] = { 0x02, 0xa1, 0x00, 0x00, 0x00, 0x0f };
static const uint8_t k_guest_b_mac[6] = { 0x02, 0xb1, 0x00, 0x00, 0x00, 0x0f };

/* IPV4_ADDR goes through htonl, which is no constant expression on glibc;
 * these addresses initialise static tables. */
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define IP4C(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#else
#define IP4C(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
#endif
#define IP0      IP4C(10, 0, 2, 15)
#define PEER     IP4C(10, 0, 2, 2)
#define WORLD    IP4C(8, 8, 8, 8)
#define IP1      IP4C(10, 75, 0, 1)
#define GUEST_A  IP4C(10, 75, 0, 15)
#define IP2      IP4C(10, 76, 0, 1)
#define GUEST_B  IP4C(10, 76, 0, 15)
#define NOBODY   IP4C(10, 77, 0, 15)   /* a guest no tap attached */
#define MASK24   htonl(0xffffff00u)

static struct fz_netif g_f0, g_f1, g_f2;
static size_t g_baseline;
static struct udp_pcb g_udp7;

static void trace(const char *fmt, ...)
{
    static int on = -1;
    if (on < 0)
        on = getenv("FZ_TRACE") != NULL;
    if (!on)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* --- the model ---------------------------------------------------------------- */

enum { OWN_HOST, OWN_A, OWN_B, OWN_NOBODY, OWN_COUNT };
static const uint32_t k_owner_ip[OWN_COUNT] = { FW_HOST_GUEST_IP, GUEST_A, GUEST_B, NOBODY };
static const uint32_t k_owner_gw[OWN_COUNT] = { 0, IP1, IP2, 0 };

struct owner_model {
    bool attached;
    unsigned n;
    struct fw_rule rules[FW_RULES_PER_GUEST];
    uint8_t policy[5];    /* to_uplink, to_guest, to_host, from_uplink, output: fw_policy_get's order */
};
static struct owner_model g_own[OWN_COUNT];
/* What attach installs (the seeds and the defaults), per owner, read once at
 * the first attach: a reset puts exactly this back. */
static struct owner_model g_reset[OWN_COUNT];
static bool g_reset_known;

struct pf_model {
    uint8_t proto;
    uint16_t host_port, guest_port;
    uint32_t guest_ip;
};
static struct pf_model g_pf[NAT_PF_MAX];
static unsigned g_npf;

static bool same_rule(const struct fw_rule *a, const struct fw_rule *b)
{
    return a->direction == b->direction && a->proto == b->proto && a->dst_prefix == b->dst_prefix &&
           a->verdict == b->verdict && a->dst_ip == b->dst_ip && a->dst_port == b->dst_port &&
           a->src_prefix == b->src_prefix && a->src_ip == b->src_ip && a->scope == b->scope;
}

static int owner_of(uint32_t ip)
{
    for (int i = 0; i < OWN_COUNT; i++)
        if (k_owner_ip[i] == ip)
            return i;
    return -1;
}

static void dump_rule(const char *what, const struct fw_rule *r)
{
    fprintf(stderr, "  %s: dir %u proto %u dst %08x/%u port %u verdict %u src %08x/%u scope %u\n", what, r->direction,
            r->proto, ntohl(r->dst_ip), r->dst_prefix, r->dst_port, r->verdict, ntohl(r->src_ip), r->src_prefix,
            r->scope);
}

/* Rule decisions per chain and verdict, since the last record began: the
 * verdict oracle (check_verdicts) holds the firewall's counters to them. */
enum { CH_FORWARD, CH_INPUT, CH_HOST, CH_OUTPUT, CH_COUNT };
static unsigned g_tally[CH_COUNT][2];

static int chain_of(uint8_t direction)
{
    switch (direction) {
    case FW_DIR_TO_HOST: return CH_INPUT;
    case FW_DIR_FROM_UPLINK: return CH_HOST;
    case FW_DIR_OUTPUT: return CH_OUTPUT;
    default: return CH_FORWARD;
    }
}

/* As a frame is decided: the rule that decided it is installed, for that owner, now. */
static void on_rule_matched(uint32_t owner_ip, const struct fw_rule *r)
{
    g_tally[chain_of(r->direction)][r->verdict == FW_ACCEPT]++;
    trace("  rule decided: owner %08x dir %u proto %u verdict %u\n", ntohl(owner_ip), r->direction, r->proto,
          r->verdict);
    int o = owner_of(owner_ip);
    bool ok = o >= 0 && g_own[o].attached;
    bool found = false;
    for (unsigned i = 0; ok && i < g_own[o].n && !found; i++)
        found = same_rule(&g_own[o].rules[i], r);
    if (ok && found)
        return;
    fprintf(stderr, "fuzz_net_config: a rule decided a frame that the model does not hold for owner %08x (%s)\n",
            ntohl(owner_ip), ok ? "not in its list" : "owner not attached");
    dump_rule("the rule", r);
    FUZZ_ASSERT(ok && found);
}

static void read_owner(int o, struct owner_model *m)
{
    memset(m, 0, sizeof(*m));
    static struct fw_rule buf[FW_RULES_PER_GUEST + 1];
    uint8_t p[5];
    m->attached = fw_policy_get(k_owner_ip[o], &p[0], &p[1], &p[2], &p[3], &p[4]) == 0;
    if (!m->attached)
        return;
    memcpy(m->policy, p, sizeof(p));
    m->n = fw_rule_list(k_owner_ip[o], buf, FW_RULES_PER_GUEST + 1);
    FUZZ_ASSERT(m->n <= FW_RULES_PER_GUEST);
    memcpy(m->rules, buf, m->n * sizeof(buf[0]));
}

/* --- the oracles ---------------------------------------------------------------- */

static void check_model(void)
{
    FUZZ_ASSERT(harness_locks_held() == 0);
    FUZZ_ASSERT(fz_sock_wake_refs() == 0);
    for (int o = 0; o < OWN_COUNT; o++) {
        struct owner_model have;
        read_owner(o, &have);
        const struct owner_model *want = &g_own[o];
        bool same = have.attached == want->attached && have.n == want->n &&
                    (!have.attached || memcmp(have.policy, want->policy, sizeof(have.policy)) == 0);
        for (unsigned i = 0; same && i < have.n; i++)
            same = same_rule(&have.rules[i], &want->rules[i]);
        if (!same) {
            fprintf(stderr, "fuzz_net_config: owner %08x: listed %s, %u rule(s); the model %s, %u rule(s)\n",
                    ntohl(k_owner_ip[o]), have.attached ? "attached" : "absent", have.n,
                    want->attached ? "attached" : "absent", want->n);
            for (unsigned i = 0; i < have.n; i++)
                dump_rule("listed", &have.rules[i]);
            for (unsigned i = 0; i < want->n; i++)
                dump_rule("model ", &want->rules[i]);
            FUZZ_ASSERT(same);
        }
    }
    /* The attached guests, and nothing else. */
    uint32_t guests[FW_MAX_GUESTS + 1];
    unsigned ng = fw_guest_list(guests, FW_MAX_GUESTS + 1);
    FUZZ_ASSERT(ng <= FW_MAX_GUESTS);
    unsigned want_g = (unsigned)g_own[OWN_A].attached + (unsigned)g_own[OWN_B].attached;
    FUZZ_ASSERT(ng == want_g);
    for (unsigned i = 0; i < ng; i++) {
        int o = owner_of(guests[i]);
        FUZZ_ASSERT(o == OWN_A || o == OWN_B);
        FUZZ_ASSERT(g_own[o].attached);
    }
    /* The OUTPUT fast path is what the host's rules and default say. */
    bool fast = g_own[OWN_HOST].policy[4] == FW_ACCEPT;
    for (unsigned i = 0; i < g_own[OWN_HOST].n && fast; i++)
        if (g_own[OWN_HOST].rules[i].direction == FW_DIR_OUTPUT)
            fast = false;
    FUZZ_ASSERT(fw_test_out_fast() == fast);
    /* The port forwards, as a set. */
    static struct nat_pf_rule pf[NAT_PF_MAX + 1];
    unsigned npf = nat_pf_list(pf, NAT_PF_MAX + 1);
    FUZZ_ASSERT(npf <= NAT_PF_MAX);
    if (npf != g_npf)
        fprintf(stderr, "fuzz_net_config: %u port forward(s) listed, %u in the model\n", npf, g_npf);
    FUZZ_ASSERT(npf == g_npf);
    for (unsigned i = 0; i < npf; i++) {
        bool found = false;
        for (unsigned j = 0; j < g_npf && !found; j++)
            found = g_pf[j].proto == pf[i].proto && g_pf[j].host_port == pf[i].host_port &&
                    g_pf[j].guest_ip == pf[i].guest_ip && g_pf[j].guest_port == pf[i].guest_port;
        FUZZ_ASSERT(found);
    }
    /* NAT: every DNAT translation belongs to a forward the model holds; no
     * entry names a released guest. */
    static struct nat_flow nf[NAT_TABLE_SIZE + 1];
    unsigned nn = nat_flow_list(nf, NAT_TABLE_SIZE + 1, fz_now());
    FUZZ_ASSERT(nn <= NAT_TABLE_SIZE);
    for (unsigned i = 0; i < nn; i++) {
        int o = owner_of(nf[i].orig_ip);
        if ((o == OWN_A || o == OWN_B) && !g_own[o].attached) {
            fprintf(stderr, "fuzz_net_config: a NAT entry (kind %u proto %u port %u) names released guest %08x\n",
                    nf[i].kind, nf[i].proto, nf[i].nat_port, ntohl(nf[i].orig_ip));
            FUZZ_ASSERT(false);
        }
        if (nf[i].kind != NAT_KIND_DNAT)
            continue;
        bool found = false;
        for (unsigned j = 0; j < g_npf && !found; j++)
            found = g_pf[j].proto == nf[i].proto && g_pf[j].host_port == nf[i].nat_port &&
                    g_pf[j].guest_ip == nf[i].orig_ip && g_pf[j].guest_port == nf[i].orig_port;
        if (!found) {
            fprintf(stderr, "fuzz_net_config: a DNAT entry (proto %u host port %u -> %08x:%u) with no port forward\n",
                    nf[i].proto, nf[i].nat_port, ntohl(nf[i].orig_ip), nf[i].orig_port);
            FUZZ_ASSERT(found);
        }
    }
    /* No flow is a released guest's own. (One that names its address can be
     * the host's, made after the release: with the tap gone the address is
     * the world's, reached through the uplink. At the release itself none
     * may name it: op_guest checks.) */
    static struct fw_flow_info ff[FW_FLOW_MAX + 1];
    unsigned nfl = fw_flow_list(ff, FW_FLOW_MAX + 1, fz_now());
    FUZZ_ASSERT(nfl <= FW_FLOW_MAX);
    for (unsigned i = 0; i < nfl; i++)
        for (int o = OWN_A; o <= OWN_B; o++)
            if (!g_own[o].attached && ff[i].guest_ip == k_owner_ip[o]) {
                fprintf(stderr, "fuzz_net_config: a firewall flow of released guest %08x\n", ntohl(k_owner_ip[o]));
                FUZZ_ASSERT(false);
            }
}

static void check_released(uint32_t guest)
{
    static struct fw_flow_info ff[FW_FLOW_MAX + 1];
    unsigned nfl = fw_flow_list(ff, FW_FLOW_MAX + 1, fz_now());
    for (unsigned i = 0; i < nfl; i++)
        if (ff[i].guest_ip == guest || ff[i].a_ip == guest || ff[i].b_ip == guest) {
            fprintf(stderr, "fuzz_net_config: a firewall flow names guest %08x after its release\n", ntohl(guest));
            FUZZ_ASSERT(false);
        }
}

/*
 * The verdict oracle. Every rule that decides a frame is reported by the
 * hook, and every verdict is counted by the firewall, once, as by-rule or
 * by-default; the two must agree. A rule's DROP is counted as a rule drop
 * in its chain. A rule's ACCEPT is counted as a rule accept, except in the
 * forward chain, where an accepted guest-to-guest flow with no room in the
 * flow table is refused and counted as flow_drop_share or flow_drop_table
 * instead. A stale fast path, a rule skipped, or a verdict inverted shows
 * up as a count with no decision behind it, or a decision with no count.
 */
static void check_verdicts(const struct fw_stats *a, const struct fw_stats *b)
{
    unsigned fwd_acc = (unsigned)(b->accept_rule - a->accept_rule);
    unsigned fwd_refused = (unsigned)((b->flow_drop_share - a->flow_drop_share) + (b->flow_drop_table - a->flow_drop_table));
    bool ok = g_tally[CH_FORWARD][0] == b->drop_rule - a->drop_rule &&
              g_tally[CH_FORWARD][1] >= fwd_acc && g_tally[CH_FORWARD][1] <= fwd_acc + fwd_refused &&
              g_tally[CH_INPUT][0] == b->in_drop_rule - a->in_drop_rule &&
              g_tally[CH_INPUT][1] == b->in_accept_rule - a->in_accept_rule &&
              g_tally[CH_HOST][0] == b->hin_drop_rule - a->hin_drop_rule &&
              g_tally[CH_HOST][1] == b->hin_accept_rule - a->hin_accept_rule &&
              g_tally[CH_OUTPUT][0] == b->out_drop_rule - a->out_drop_rule &&
              g_tally[CH_OUTPUT][1] == b->out_accept_rule - a->out_accept_rule;
    if (ok)
        return;
    fprintf(stderr, "fuzz_net_config: rule decisions (drop/accept) forward %u/%u input %u/%u host %u/%u output %u/%u; "
                    "counted forward %llu/%llu (+%u refused) input %llu/%llu host %llu/%llu output %llu/%llu\n",
            g_tally[CH_FORWARD][0], g_tally[CH_FORWARD][1], g_tally[CH_INPUT][0], g_tally[CH_INPUT][1],
            g_tally[CH_HOST][0], g_tally[CH_HOST][1], g_tally[CH_OUTPUT][0], g_tally[CH_OUTPUT][1],
            (unsigned long long)(b->drop_rule - a->drop_rule), (unsigned long long)fwd_acc, fwd_refused,
            (unsigned long long)(b->in_drop_rule - a->in_drop_rule), (unsigned long long)(b->in_accept_rule - a->in_accept_rule),
            (unsigned long long)(b->hin_drop_rule - a->hin_drop_rule), (unsigned long long)(b->hin_accept_rule - a->hin_accept_rule),
            (unsigned long long)(b->out_drop_rule - a->out_drop_rule), (unsigned long long)(b->out_accept_rule - a->out_accept_rule));
    FUZZ_ASSERT(ok);
}

/* The default a chain applies for `o` in `slot` (fw_policy_get's order): the
 * model's policy for an attached owner, the built-in one otherwise. */
static uint8_t default_of(int o, unsigned slot)
{
    if (o >= 0 && g_own[o].attached)
        return g_own[o].policy[slot];
    static const uint8_t builtin[5] = { FW_ACCEPT, FW_DROP, FW_DROP, FW_ACCEPT, FW_ACCEPT };
    return builtin[slot];
}

/* A chain decided by its default (one count moved, no rule decided in it):
 * the verdict is the owner's default. */
static void check_default(const char *chain, uint64_t acc_before, uint64_t acc_after, uint64_t drop_before,
                          uint64_t drop_after, uint8_t want)
{
    uint64_t acc = acc_after - acc_before, drop = drop_after - drop_before;
    if (acc + drop != 1)
        return;   /* the chain did not run, or ran for more than this frame */
    uint8_t got = acc ? FW_ACCEPT : FW_DROP;
    if (got != want)
        fprintf(stderr, "fuzz_net_config: the %s chain's default gave %s; the model's is %s\n", chain,
                got ? "ACCEPT" : "DROP", want ? "ACCEPT" : "DROP");
    FUZZ_ASSERT(got == want);
}

/* --- the operations -------------------------------------------------------------- */

static void announce(struct fz_netif *f, const uint8_t *mac, uint32_t ip, uint32_t ours);

enum { OP_FW_ADD, OP_FW_DEL, OP_POLICY, OP_FW_FLUSH, OP_PF_ADD, OP_PF_DEL, OP_PF_CLEAR, OP_GUEST, OP_NAT_FLUSH,
       OP_CLOCK, OP_SEND, OP_FRAME, OP_COUNT };
static const unsigned k_op_args[OP_COUNT] = { 12, 12, 3, 0, 4, 2, 0, 1, 0, 1, 3, 6 };

static const uint32_t k_ips[8] = { 0, PEER, IP0, IP1, GUEST_A, GUEST_B, WORLD, IP4C(10, 75, 0, 0) };
static const uint16_t k_ports[8] = { 80, 22, 53, 8080, 8081, 5353, 7, 9 };

static uint16_t port_of(uint8_t b)
{
    return b < 0xc0 ? k_ports[b & 7] : (uint16_t)(1000 + b);
}

static void rule_of(const uint8_t *a, struct fw_rule *r)
{
    static const uint8_t protos[5] = { 0, IPPROTO_TCP, IPPROTO_UDP, IPPROTO_ICMP, 47 };
    memset(r, 0, sizeof(*r));
    r->direction = a[0] % 7;                          /* 6 is no direction */
    r->proto = protos[a[1] % 5];
    r->dst_prefix = a[2] < 0x80 ? (uint8_t)(a[2] % 2 ? 32 : 0) : (uint8_t)(a[2] % 34);   /* 33: invalid */
    r->verdict = a[3] % 3;                            /* 2: invalid */
    r->dst_ip = k_ips[a[4] & 7];
    r->dst_port = a[5] >= 0xf8 ? (a[5] & 1 ? FW_ICMP_TYPE_ANY : ICMP_ECHO) : (a[5] < 0x40 ? 0 : port_of(a[5]));
    r->src_prefix = a[6] < 0xb0 ? 0 : (uint8_t)(a[6] % 34);
    r->src_ip = r->src_prefix ? k_ips[a[7] & 7] : 0;
    r->scope = a[8] < 0xc0 ? 0 : a[8] % 4;            /* 3: invalid */
}

static void model_reset(int o)
{
    g_own[o] = g_reset[o];
}

/* N28's other half: what a configuration change must leave alone, read
 * before and after it at one clock. Field by field: the listings copy
 * structs, padding and all. */
struct kept_state {
    struct fw_flow_info ff[FW_FLOW_MAX];
    unsigned nff;
    struct nat_flow masq[NAT_TABLE_SIZE];
    unsigned nmasq;
};
static struct kept_state g_kept_before, g_kept_after;

static void kept_read(struct kept_state *k)
{
    k->nff = fw_flow_list(k->ff, FW_FLOW_MAX, fz_now());
    static struct nat_flow nf[NAT_TABLE_SIZE];
    unsigned nn = nat_flow_list(nf, NAT_TABLE_SIZE, fz_now());
    k->nmasq = 0;
    for (unsigned i = 0; i < nn; i++)
        if (nf[i].kind != NAT_KIND_DNAT)
            k->masq[k->nmasq++] = nf[i];
}

static bool same_fw_flow(const struct fw_flow_info *x, const struct fw_flow_info *y)
{
    return x->guest_ip == y->guest_ip && x->a_ip == y->a_ip && x->b_ip == y->b_ip && x->a_port == y->a_port &&
           x->b_port == y->b_port && x->proto == y->proto && x->est == y->est && x->expires_ns == y->expires_ns;
}

static bool same_nat_flow(const struct nat_flow *x, const struct nat_flow *y)
{
    return x->kind == y->kind && x->proto == y->proto && x->est == y->est && x->orig_port == y->orig_port &&
           x->nat_port == y->nat_port && x->peer_port == y->peer_port && x->orig_ip == y->orig_ip &&
           x->nat_ip == y->nat_ip && x->peer_ip == y->peer_ip && x->expires_ns == y->expires_ns;
}

/* `masq` too for a forward change; a firewall change may not touch NAT
 * either, but nothing in it could, so the firewall's flows suffice. */
static void kept_check(const char *op, bool masq)
{
    kept_read(&g_kept_after);
    const struct kept_state *b = &g_kept_before, *a = &g_kept_after;
    bool same = a->nff == b->nff;
    for (unsigned i = 0; same && i < a->nff; i++)
        same = same_fw_flow(&a->ff[i], &b->ff[i]);
    if (!same) {
        fprintf(stderr, "fuzz_net_config: %s changed the firewall's flows (%u -> %u): N28\n", op, b->nff, a->nff);
        FUZZ_ASSERT(same);
    }
    if (!masq)
        return;
    same = a->nmasq == b->nmasq;
    for (unsigned i = 0; same && i < a->nmasq; i++)
        same = same_nat_flow(&a->masq[i], &b->masq[i]);
    if (!same) {
        fprintf(stderr, "fuzz_net_config: %s changed the masquerade entries (%u -> %u): N28\n", op, b->nmasq,
                a->nmasq);
        FUZZ_ASSERT(same);
    }
}

static void op_fw_add(const uint8_t *a)
{
    struct fw_rule r;
    rule_of(a, &r);
    int o = a[9] % OWN_COUNT;
    unsigned at = a[10] % (FW_RULES_PER_GUEST + 2);
    kept_read(&g_kept_before);
    int rc = fw_rule_add(k_owner_ip[o], at, &r);
    trace("fw add owner %d at %u: %d\n", o, at, rc);
    kept_check("a rule add", false);
    struct owner_model *m = &g_own[o];
    bool dup = false;
    for (unsigned i = 0; m->attached && i < m->n && !dup; i++)
        dup = same_rule(&m->rules[i], &r);
    if (rc == -EINVAL) {
        FUZZ_ASSERT(!dup);   /* an installed copy was valid in this scope, so this one is */
        return;
    }
    if (!m->attached) {
        FUZZ_ASSERT(rc == -ENOENT);
        return;
    }
    if (dup) {
        FUZZ_ASSERT(rc == -EEXIST);
        return;
    }
    if (m->n == FW_RULES_PER_GUEST) {
        FUZZ_ASSERT(rc == -ENOSPC);
        return;
    }
    FUZZ_ASSERT(rc == 0);
    if (at > m->n)
        at = m->n;
    memmove(&m->rules[at + 1], &m->rules[at], (m->n - at) * sizeof(m->rules[0]));
    m->rules[at] = r;
    m->n++;
}

static void op_fw_del(const uint8_t *a)
{
    int o = a[9] % OWN_COUNT;
    struct owner_model *m = &g_own[o];
    struct fw_rule r;
    if (m->attached && m->n > 0 && a[11] < 0xc0)
        r = m->rules[a[11] % m->n];   /* one installed */
    else
        rule_of(a, &r);
    kept_read(&g_kept_before);
    int rc = fw_rule_del(k_owner_ip[o], &r);
    trace("fw del owner %d: %d\n", o, rc);
    kept_check("a rule delete", false);
    int at = -1;
    for (unsigned i = 0; m->attached && i < m->n && at < 0; i++)
        if (same_rule(&m->rules[i], &r))
            at = (int)i;
    FUZZ_ASSERT(rc == (at >= 0 ? 0 : -ENOENT));
    if (at >= 0) {
        memmove(&m->rules[at], &m->rules[at + 1], (m->n - (unsigned)at - 1) * sizeof(m->rules[0]));
        m->n--;
    }
}

static void op_policy(const uint8_t *a)
{
    int o = a[0] % OWN_COUNT;
    uint8_t dir = a[1] % 7, verdict = a[2] % 3;
    kept_read(&g_kept_before);
    int rc = fw_policy_set(k_owner_ip[o], dir, verdict);
    trace("policy owner %d dir %u verdict %u: %d\n", o, dir, verdict, rc);
    kept_check("a policy change", false);
    bool host = o == OWN_HOST;
    bool valid = verdict <= FW_ACCEPT &&
                 (host ? (dir == FW_DIR_FROM_UPLINK || dir == FW_DIR_OUTPUT)
                       : (dir >= FW_DIR_TO_UPLINK && dir <= FW_DIR_TO_HOST));
    if (!valid) {
        FUZZ_ASSERT(rc == -EINVAL);
        return;
    }
    if (!g_own[o].attached) {
        FUZZ_ASSERT(rc == -ENOENT);
        return;
    }
    FUZZ_ASSERT(rc == 0);
    static const int slot[] = { -1, 0, 1, 2, 3, 4 };   /* by direction: fw_policy_get's order */
    g_own[o].policy[slot[dir]] = verdict;
}

static void op_fw_flush(void)
{
    fw_flush();
    for (int o = 0; o < OWN_COUNT; o++)
        if (g_own[o].attached)
            model_reset(o);
}

static void op_pf_add(const uint8_t *a)
{
    static const uint8_t protos[3] = { IPPROTO_TCP, IPPROTO_UDP, IPPROTO_ICMP };
    static const uint32_t targets[5] = { GUEST_A, GUEST_B, PEER, 0, NOBODY };
    uint8_t proto = protos[a[0] % 3];
    uint16_t hport = a[1] < 0x10 ? 0 : port_of(a[1]);
    uint32_t gip = targets[a[2] % 5];
    uint16_t gport = a[3] < 0x10 ? 0 : port_of(a[3]);
    int rc = nat_pf_add(proto, hport, gip, gport);
    trace("pf add %u %u -> %08x:%u: %d\n", proto, hport, ntohl(gip), gport, rc);
    /* A forward's target must sit on a forwarding tap's subnet, up: a
     * released guest's tap is down. */
    bool valid = proto != IPPROTO_ICMP && hport != 0 && gport != 0 &&
                 ((gip == GUEST_A && g_own[OWN_A].attached) || (gip == GUEST_B && g_own[OWN_B].attached));
    if (!valid) {
        FUZZ_ASSERT(rc == -EINVAL);
        return;
    }
    for (unsigned i = 0; i < g_npf; i++)
        if (g_pf[i].proto == proto && g_pf[i].host_port == hport) {
            FUZZ_ASSERT(rc == -EEXIST);
            return;
        }
    if (g_npf == NAT_PF_MAX) {
        FUZZ_ASSERT(rc == -ENOSPC);
        return;
    }
    FUZZ_ASSERT(rc == 0);
    g_pf[g_npf++] = (struct pf_model){ proto, hport, gport, gip };
}

static void pf_remove(unsigned i)
{
    g_pf[i] = g_pf[--g_npf];
}

static void op_pf_del(const uint8_t *a)
{
    uint8_t proto;
    uint16_t hport;
    if (g_npf > 0 && a[0] < 0xc0) {
        proto = g_pf[a[1] % g_npf].proto;
        hport = g_pf[a[1] % g_npf].host_port;
    } else {
        proto = a[0] & 1 ? IPPROTO_UDP : IPPROTO_TCP;
        hport = port_of(a[1]);
    }
    kept_read(&g_kept_before);
    bool found = nat_pf_del(proto, hport);
    trace("pf del %u %u: %d\n", proto, hport, found);
    kept_check("a forward delete", true);
    for (unsigned i = 0; i < g_npf; i++)
        if (g_pf[i].proto == proto && g_pf[i].host_port == hport) {
            FUZZ_ASSERT(found);
            pf_remove(i);
            return;
        }
    FUZZ_ASSERT(!found);
}

static void op_pf_clear(void)
{
    kept_read(&g_kept_before);
    nat_pf_clear();
    kept_check("a forward clear", true);
    g_npf = 0;
}

/* A guest's tap released, or attached again. The release takes the tap's
 * interface down before the purges, as tap_chr_release does: a frame the
 * interface still delivered after the purges would leave a translation or
 * a flow naming the released address (this target's second finding). */
static void op_guest(const uint8_t *a)
{
    int o = a[0] & 1 ? OWN_B : OWN_A;
    struct fz_netif *f = o == OWN_B ? &g_f2 : &g_f1;
    if (a[0] & 2) {
        f->nif.flags |= NETIF_UP;
        fw_guest_attach(k_owner_ip[o], k_owner_gw[o]);
        model_reset(o);
        g_own[o].attached = true;
        announce(f, o == OWN_B ? k_guest_b_mac : k_guest_a_mac, k_owner_ip[o], k_owner_gw[o]);
        trace("attach %d\n", o);
        return;
    }
    if (!g_own[o].attached)
        return;   /* released already: a tap is released once */
    f->nif.flags &= ~NETIF_UP;
    arp_flush(&f->nif);
    nd_flush(&f->nif);
    nat_guest_purge(k_owner_ip[o]);
    fw_guest_purge(k_owner_ip[o]);
    check_released(k_owner_ip[o]);
    memset(&g_own[o], 0, sizeof(g_own[o]));
    for (unsigned i = 0; i < g_npf;)
        if (g_pf[i].guest_ip == k_owner_ip[o])
            pf_remove(i);
        else
            i++;
    trace("release %d\n", o);
}

static void op_send(const uint8_t *a)
{
    static const uint8_t payload[32] = { 'o' };
    struct netaddr to;
    memset(&to, 0, sizeof(to));
    to.family = COSMO_AF_INET;
    to.v4 = k_ips[(a[0] & 7) ? (a[0] & 7) : 1];
    to.port = port_of(a[1]);
    struct fw_stats s0, s1;
    fw_get_stats(&s0);
    unsigned rd0 = g_tally[CH_OUTPUT][0], ra0 = g_tally[CH_OUTPUT][1];
    int rc = udp_sendto(&g_udp7, payload, (size_t)(a[2] % 32) + 1, &to);
    fw_get_stats(&s1);
    trace("send to %08x:%u: %d\n", ntohl(to.v4), to.port, rc);
    /* The OUTPUT chain's verdict is what the send returns: -EPERM for a drop. */
    uint64_t dropped = (s1.out_drop_rule - s0.out_drop_rule) + (s1.out_drop_default - s0.out_drop_default);
    uint64_t passed = (s1.out_accept_rule - s0.out_accept_rule) + (s1.out_accept_default - s0.out_accept_default);
    if (dropped + passed == 1)
        FUZZ_ASSERT((rc == -EPERM) == (dropped == 1));
    if (g_tally[CH_OUTPUT][0] == rd0 && g_tally[CH_OUTPUT][1] == ra0)
        check_default("OUTPUT", s0.out_accept_default, s1.out_accept_default, s0.out_drop_default, s1.out_drop_default,
                      default_of(OWN_HOST, 4));
}

/* A well-formed frame from a template, so the rules and translations meet
 * traffic they could match. */
static void op_frame(const uint8_t *a)
{
    static const uint8_t payload[24] = "config fuzz frame";
    static const uint8_t protos[3] = { IPPROTO_TCP, IPPROTO_UDP, IPPROTO_ICMP };
    uint8_t proto = protos[a[1] % 3];
    uint16_t sport = port_of(a[2]), dport = port_of(a[3]);
    uint8_t flags = a[4] ? a[4] : TH_SYN;
    struct fz_netif *on;
    const uint8_t *dmac, *smac;
    uint32_t src, dst;
    switch (a[0] % 7) {
    case 0:   /* the uplink to the host */
        on = &g_f0, dmac = k_mac0, smac = k_peer_mac, src = a[5] & 1 ? WORLD : PEER, dst = IP0;
        break;
    case 1:   /* the uplink to a forwarded port */
        on = &g_f0, dmac = k_mac0, smac = k_peer_mac, src = a[5] & 1 ? WORLD : PEER, dst = IP0;
        if (g_npf > 0) {
            proto = g_pf[a[5] % g_npf].proto;
            dport = g_pf[a[5] % g_npf].host_port;
        }
        break;
    case 2:   /* a guest to the world */
        on = a[5] & 1 ? &g_f2 : &g_f1;
        dmac = a[5] & 1 ? k_mac2 : k_mac1, smac = a[5] & 1 ? k_guest_b_mac : k_guest_a_mac;
        src = a[5] & 1 ? GUEST_B : GUEST_A, dst = a[5] & 2 ? PEER : WORLD;
        break;
    case 3:   /* a guest to the host */
        on = a[5] & 1 ? &g_f2 : &g_f1;
        dmac = a[5] & 1 ? k_mac2 : k_mac1, smac = a[5] & 1 ? k_guest_b_mac : k_guest_a_mac;
        src = a[5] & 1 ? GUEST_B : GUEST_A, dst = a[5] & 1 ? IP2 : IP1;
        break;
    case 4:   /* a guest to the other */
        on = a[5] & 1 ? &g_f2 : &g_f1;
        dmac = a[5] & 1 ? k_mac2 : k_mac1, smac = a[5] & 1 ? k_guest_b_mac : k_guest_a_mac;
        src = a[5] & 1 ? GUEST_B : GUEST_A, dst = a[5] & 1 ? GUEST_A : GUEST_B;
        break;
    default: {   /* a reply to a translation the table holds */
        static struct nat_flow nf[NAT_TABLE_SIZE];
        unsigned nn = nat_flow_list(nf, NAT_TABLE_SIZE, fz_now());
        if (nn == 0)
            return;
        const struct nat_flow *e = &nf[a[5] % nn];
        proto = e->proto;
        flags = a[4] ? a[4] : TH_SYN | TH_ACK;
        if (e->kind == NAT_KIND_MASQ) {   /* the world answers the masqueraded port */
            on = &g_f0, dmac = k_mac0, smac = k_peer_mac;
            src = e->peer_ip, dst = e->nat_ip, sport = e->peer_port, dport = e->nat_port;
        } else {                          /* the guest answers the forwarded client */
            bool b = owner_of(e->orig_ip) == OWN_B;
            on = b ? &g_f2 : &g_f1;
            dmac = b ? k_mac2 : k_mac1, smac = b ? k_guest_b_mac : k_guest_a_mac;
            src = e->orig_ip, dst = e->peer_ip, sport = e->orig_port, dport = e->peer_port;
        }
        break;
    }
    }
    uint8_t f[256];
    size_t n;
    if (proto == IPPROTO_TCP)
        n = np_frame_tcp4(f, dmac, smac, src, dst, sport, dport, 1000, flags & TH_ACK ? 1 : 0, flags, 65535, NULL, 0,
                          payload, flags & TH_SYN ? 0 : 8);
    else if (proto == IPPROTO_UDP)
        n = np_frame_udp4(f, dmac, smac, src, dst, sport, dport, payload, 8);
    else
        n = np_frame_icmp4(f, dmac, smac, src, dst, a[4] & 1 ? ICMP_ECHO_REPLY : ICMP_ECHO, 0, sport, 1, payload, 8);
    trace("frame %u: %08x:%u -> %08x:%u proto %u on %s\n", a[0] % 7, ntohl(src), sport, ntohl(dst), dport, proto,
          on->nif.name);
    if (!(on->nif.flags & NETIF_UP))
        return;   /* netif_rx refuses a down interface's frames */
    struct fw_stats s0, s1;
    fw_get_stats(&s0);
    unsigned tally0[CH_COUNT][2];
    memcpy(tally0, g_tally, sizeof(tally0));
    fz_deliver(on, f, (uint32_t)n);
    fw_get_stats(&s1);
    /* Where the template names the chain and the owner, a default decision
     * is that owner's default. (A reply takes whatever path its translation
     * gives it, and is held only to the tally.) */
    int owner = owner_of(src);
    unsigned kind = a[0] % 7;
    if ((kind == 0 || kind == 1) && memcmp(tally0[CH_HOST], g_tally[CH_HOST], sizeof(tally0[CH_HOST])) == 0)
        check_default("host", s0.hin_accept_default, s1.hin_accept_default, s0.hin_drop_default, s1.hin_drop_default,
                      default_of(OWN_HOST, 3));
    if (kind == 3 && memcmp(tally0[CH_INPUT], g_tally[CH_INPUT], sizeof(tally0[CH_INPUT])) == 0)
        check_default("input", s0.in_accept_default, s1.in_accept_default, s0.in_drop_default, s1.in_drop_default,
                      default_of(owner, 2));
    if ((kind == 2 || kind == 4) && memcmp(tally0[CH_FORWARD], g_tally[CH_FORWARD], sizeof(tally0[CH_FORWARD])) == 0)
        check_default("forward", s0.accept_default, s1.accept_default, s0.drop_default, s1.drop_default,
                      default_of(owner, kind == 4 && g_own[owner_of(dst)].attached ? 1 : 0));
    /* (To the other guest is TO_GUEST only while its tap is up: with it
     * released the address routes out the uplink, TO_UPLINK.) */
    struct mbuf *m;
    while ((m = udp_recv(&g_udp7)) != NULL)
        m_freem(m);
}

/* --- setup and teardown ----------------------------------------------------------- */

/* Each neighbour announces itself, so frames toward it leave at once. */
static void announce(struct fz_netif *f, const uint8_t *mac, uint32_t ip, uint32_t ours)
{
    uint8_t fr[64];
    size_t n = np_frame_arp(fr, eth_broadcast, mac, 1, mac, ip, eth_broadcast, ours);
    fz_deliver(f, fr, (uint32_t)n);
}

static void setup(void)
{
    fz_random_seed(0xc0f1);
    fz_netif_register(&g_f0, "fz0", k_mac0, IP0, MASK24, PEER, 0);
    fz_netif_register(&g_f1, "fz1", k_mac1, IP1, MASK24, 0, NETIF_NODEFAULT | NETIF_FORWARD | NETIF_MASQUERADE);
    fz_netif_register(&g_f2, "fz2", k_mac2, IP2, MASK24, 0, NETIF_NODEFAULT | NETIF_FORWARD | NETIF_MASQUERADE);
    fw_guest_attach(GUEST_A, IP1);
    fw_guest_attach(GUEST_B, IP2);
    if (!g_reset_known) {
        for (int o = 0; o < OWN_COUNT; o++)
            read_owner(o, &g_reset[o]);
        FUZZ_ASSERT(g_reset[OWN_HOST].attached && g_reset[OWN_A].attached && g_reset[OWN_B].attached);
        FUZZ_ASSERT(!g_reset[OWN_NOBODY].attached);
        g_reset_known = true;
    }
    for (int o = 0; o < OWN_COUNT; o++)
        g_own[o] = g_reset[o];
    g_npf = 0;
    struct netaddr a;
    memset(&a, 0, sizeof(a));
    a.family = COSMO_AF_INET;
    a.port = 7;
    FUZZ_ASSERT(udp_pcb_init(&g_udp7, COSMO_AF_INET) == 0);
    FUZZ_ASSERT(udp_bind(&g_udp7, &a) == 0);
    announce(&g_f0, k_peer_mac, PEER, IP0);
    announce(&g_f1, k_guest_a_mac, GUEST_A, IP1);
    announce(&g_f2, k_guest_b_mac, GUEST_B, IP2);
    fw_test_rule_matched = on_rule_matched;
    check_model();
}

static void teardown(void)
{
    fw_test_rule_matched = NULL;
    udp_unbind(&g_udp7);
    struct mbuf *m;
    while ((m = udp_recv(&g_udp7)) != NULL)
        m_freem(m);
    for (unsigned i = 0; i < 900 && fz_allocs_live() != g_baseline; i++)
        fz_clock_advance(1000000000ull);
    nat_flush();
    nat_pf_clear();
    fw_guest_purge(GUEST_A);
    fw_guest_purge(GUEST_B);
    fw_flush();
    fz_netif_unregister_all();
    FUZZ_ASSERT(harness_locks_held() == 0);
    struct mbuf_stats ms;
    mbuf_get_stats(&ms);
    FUZZ_ASSERT(ms.mbufs_alive == 0);
    FUZZ_ASSERT(ms.clusters_alive == 0);
    static struct nat_flow nf[1];
    FUZZ_ASSERT(nat_flow_list(nf, 1, fz_now()) == 0);
    static struct fw_flow_info ff[1];
    FUZZ_ASSERT(fw_flow_list(ff, 1, fz_now()) == 0);
    struct arp_stats as;
    arp_get_stats(&as);
    FUZZ_ASSERT(as.entries == 0);
    if (fz_allocs_live() != g_baseline) {
        fprintf(stderr, "fuzz_net_config: %zu object(s) still allocated after teardown (baseline %zu)\n",
                fz_allocs_live(), g_baseline);
        FUZZ_ASSERT(fz_allocs_live() == g_baseline);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    fz_net_init();
    if (g_baseline == 0)
        g_baseline = fz_allocs_live();
    setup();
    size_t off = 0;
    unsigned ops = 0;
    while (off < size && ops < 128) {
        uint8_t op = data[off++] % OP_COUNT;
        uint8_t a[12] = { 0 };
        size_t n = k_op_args[op];
        for (size_t i = 0; i < n && off < size; i++)
            a[i] = data[off++];
        ops++;
        struct fw_stats before;
        fw_get_stats(&before);
        memset(g_tally, 0, sizeof(g_tally));
        switch (op) {
        case OP_FW_ADD: op_fw_add(a); break;
        case OP_FW_DEL: op_fw_del(a); break;
        case OP_POLICY: op_policy(a); break;
        case OP_FW_FLUSH: op_fw_flush(); break;
        case OP_PF_ADD: op_pf_add(a); break;
        case OP_PF_DEL: op_pf_del(a); break;
        case OP_PF_CLEAR: op_pf_clear(); break;
        case OP_GUEST: op_guest(a); break;
        case OP_NAT_FLUSH: nat_flush(); break;
        case OP_CLOCK: fz_clock_advance(((uint64_t)a[0] + 1) * 500ull * 1000000ull); break;
        case OP_SEND: op_send(a); break;
        default: op_frame(a); break;
        }
        struct fw_stats after;
        fw_get_stats(&after);
        check_verdicts(&before, &after);
        check_model();
    }
    teardown();
    return 0;
}

size_t fuzz_max_len(void)
{
    return 2048;
}

/* --- seeds ------------------------------------------------------------------------ */

static size_t put(uint8_t *buf, size_t cap, size_t at, uint8_t op, const uint8_t *arg)
{
    size_t n = k_op_args[op];
    if (at + 1 + n > cap)
        return at;
    buf[at] = op;
    if (n)
        memcpy(buf + at + 1, arg, n);   /* the operations without arguments pass NULL */
    return at + 1 + n;
}

size_t fuzz_seed(unsigned i, uint8_t *buf, size_t cap)
{
    /* rule args: dir proto prefix verdict dstip dstport srcpfx srcip scope owner at sel */
    static const uint8_t drop_a_udp53[12] = { FW_DIR_TO_UPLINK, 2, 0, FW_DROP, 0, 2, 0, 0, 0, OWN_A, 0, 0 };
    static const uint8_t host_drop80[12] = { FW_DIR_FROM_UPLINK, 1, 0, FW_DROP, 0, 0, 0, 0, 0, OWN_HOST, 0, 0 };
    static const uint8_t out_drop[12] = { FW_DIR_OUTPUT, 2, 0, FW_DROP, 0, 0x40, 0, 0, 0, OWN_HOST, 0, 0 };
    static const uint8_t pf_a[4] = { 0, 0x13, 0, 0x10 };    /* tcp 8080 -> A:80 */
    static const uint8_t pf_b_udp[4] = { 1, 0x15, 1, 0x12 }; /* udp 5353 -> B:53 */
    size_t n = 0;
    switch (i) {
    case 0:   /* a guest's drop rule, a frame it decides, deleted, the frame again */
        n = put(buf, cap, n, OP_FW_ADD, drop_a_udp53);
        n = put(buf, cap, n, OP_FRAME, (const uint8_t[]){ 2, 1, 9, 2, 0, 0 });
        n = put(buf, cap, n, OP_FW_DEL, drop_a_udp53);
        return put(buf, cap, n, OP_FRAME, (const uint8_t[]){ 2, 1, 9, 2, 0, 0 });
    case 1:   /* a port forward, a client through it, its reply, then the forward deleted and the client again */
        n = put(buf, cap, n, OP_PF_ADD, pf_a);
        n = put(buf, cap, n, OP_FRAME, (const uint8_t[]){ 1, 0, 0xc5, 0, TH_SYN, 0 });
        n = put(buf, cap, n, OP_FRAME, (const uint8_t[]){ 6, 0, 0, 0, TH_SYN | TH_ACK, 0 });
        n = put(buf, cap, n, OP_PF_DEL, (const uint8_t[]){ 0, 0 });
        return put(buf, cap, n, OP_FRAME, (const uint8_t[]){ 1, 0, 0xc5, 3, TH_ACK, 0 });
    case 2:   /* the forwards cleared while a translation is live */
        n = put(buf, cap, n, OP_PF_ADD, pf_a);
        n = put(buf, cap, n, OP_PF_ADD, pf_b_udp);
        n = put(buf, cap, n, OP_FRAME, (const uint8_t[]){ 1, 0, 0xc5, 0, TH_SYN, 0 });
        n = put(buf, cap, n, OP_FRAME, (const uint8_t[]){ 1, 1, 0xc6, 0, 0, 1 });
        n = put(buf, cap, n, OP_PF_CLEAR, NULL);
        return put(buf, cap, n, OP_FRAME, (const uint8_t[]){ 1, 0, 0xc5, 3, TH_ACK, 0 });
    case 3:   /* a guest released with a forward to it and a masqueraded flow, then attached again */
        n = put(buf, cap, n, OP_PF_ADD, pf_b_udp);
        n = put(buf, cap, n, OP_FRAME, (const uint8_t[]){ 2, 1, 0xc7, 2, 0, 1 });
        n = put(buf, cap, n, OP_FRAME, (const uint8_t[]){ 1, 1, 0xc6, 0, 0, 0 });
        n = put(buf, cap, n, OP_GUEST, (const uint8_t[]){ 1 });
        n = put(buf, cap, n, OP_FRAME, (const uint8_t[]){ 6, 1, 0, 0, 0, 0 });
        return put(buf, cap, n, OP_GUEST, (const uint8_t[]){ 3 });
    case 4:   /* the host's INPUT and OUTPUT chains: a rule each, traffic, a flush */
        n = put(buf, cap, n, OP_FW_ADD, host_drop80);
        n = put(buf, cap, n, OP_FRAME, (const uint8_t[]){ 0, 0, 0xc8, 0, TH_SYN, 0 });
        n = put(buf, cap, n, OP_FW_ADD, out_drop);
        n = put(buf, cap, n, OP_SEND, (const uint8_t[]){ 1, 0x40, 4 });
        n = put(buf, cap, n, OP_FW_FLUSH, NULL);
        return put(buf, cap, n, OP_SEND, (const uint8_t[]){ 1, 0x40, 4 });
    case 5:   /* policies: an OUTPUT default of DROP, back to ACCEPT; a guest's TO_GUEST drop */
        n = put(buf, cap, n, OP_POLICY, (const uint8_t[]){ OWN_HOST, FW_DIR_OUTPUT, FW_DROP });
        n = put(buf, cap, n, OP_SEND, (const uint8_t[]){ 1, 0x40, 4 });
        n = put(buf, cap, n, OP_POLICY, (const uint8_t[]){ OWN_HOST, FW_DIR_OUTPUT, FW_ACCEPT });
        n = put(buf, cap, n, OP_POLICY, (const uint8_t[]){ OWN_A, FW_DIR_TO_GUEST, FW_DROP });
        return put(buf, cap, n, OP_FRAME, (const uint8_t[]){ 4, 1, 0xc9, 7, 0, 0 });
    case 6: { /* a guest's list filled to its capacity, then one more */
        for (unsigned k = 0; k < FW_RULES_PER_GUEST + 1; k++) {
            uint8_t r[12] = { FW_DIR_TO_UPLINK, 2, 1, FW_DROP, 6, (uint8_t)(0xc0 + k), 0, 0, 0, OWN_A, 0xff, 0 };
            n = put(buf, cap, n, OP_FW_ADD, r);
        }
        return n;
    }
    case 7: { /* the port-forward table to its capacity, then one more */
        for (unsigned k = 0; k < NAT_PF_MAX + 1; k++) {
            uint8_t p[4] = { 0, (uint8_t)(0xc0 + k), 0, 0x10 };
            n = put(buf, cap, n, OP_PF_ADD, p);
        }
        return n;
    }
    default:
        return 0;
    }
}
