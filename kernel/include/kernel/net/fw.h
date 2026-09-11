/*
 * fw.h - The firewall: a stateful packet filter over the guest taps
 * (docs/audit/next-subsystem-firewall.md, docs/audit/next-subsystem-input-chain.md).
 *
 * Two chains on one engine. The FORWARD chain, evaluated in ipv4_forward after
 * the anti-spoof and routing steps and before NAT, decides whether a datagram
 * a guest forwards is accepted or dropped -- toward the world (TO_UPLINK: the
 * egress is not a guest tap) or toward another guest (TO_GUEST). The INPUT
 * chain, evaluated in ipv4_input after nat_in has declined a datagram and
 * before the transport demux, decides whether a datagram a guest sends to
 * the *host itself* (TO_HOST) reaches a host service. Each guest owns one
 * ordered rule list (first match wins) whose rules name a direction, and a
 * default verdict per direction. The defaults: TO_GUEST DROP (a guest cannot
 * reach a neighbour unless allowed), TO_UPLINK ACCEPT (its path to the world
 * is as it was), TO_HOST DROP -- with the two services the tap offers, the DNS
 * proxy on gateway:53 and echo-request to the gateway, seeded at attach as
 * ordinary visible, deletable rules rather than hard-coded holes.
 *
 * The filter is stateful where it has to be. A masqueraded guest-to-uplink
 * reply never re-enters ipv4_forward (nat_in delivers it via ipv4_output), and
 * exists only because a NAT conntrack entry -- created when the outbound flow
 * was accepted here -- matched it; conntrack is that direction's state. A
 * guest-to-guest flow is un-NAT'd, so both halves traverse ipv4_forward and
 * the filter keeps its own bounded flow table for it: an accepted NEW flow
 * is recorded, and its reply (the reverse tuple) is ESTABLISHED and accepted
 * without a reverse rule -- except a reverse-direction bare SYN, which opens
 * a connection and is never a reply. ICMP is stateful for echo only, keyed on
 * the echo identifier. The INPUT chain needs no state: the host's reply
 * leaves by ipv4_output and passes no filter, and a guest's later segments
 * match the same rule by destination port. Inbound DNAT is authorized by its
 * port-forward rule and is not re-gated.
 *
 * A rule's transport selector follows its protocol: for TCP/UDP dst_port is
 * the destination port (0 = any); for ICMP it is the ICMP *type* (0..255,
 * FW_ICMP_TYPE_ANY = any) -- a default-deny INPUT chain must be able to admit
 * echo-request without also admitting Need-Fragmentation or Echo Reply.
 *
 * A rule's identity is its whole match tuple (there is no kernel-assigned id;
 * the control channel's write returns only a byte count). Rules and policy
 * are bound to a guest by address. Attachment is the firewall's own: the tap's
 * open attaches the guest and its release purges it, both under g_fw_lock,
 * as are rule insertion and the liveness check -- so an add cannot interleave
 * with a teardown, and a reused guest address inherits nothing.
 */
#ifndef KERNEL_NET_FW_H
#define KERNEL_NET_FW_H

#include <kernel/types.h>

struct netif;
struct mbuf;
struct ipv4_hdr;

#define FW_MAX_GUESTS         8u                          /* mirrors tap.c TAP_MAX_GUESTS */
#define FW_RULES_PER_GUEST    32u                         /* one guest's ordered rule list (two are seeded) */
#define FW_FLOW_MAX           256u                        /* stateful entries (guest-to-guest) */
#define FW_FLOW_QUOTA_PER_GUEST (FW_FLOW_MAX / FW_MAX_GUESTS) /* one guest's share; a flood starves itself */
#define FW_ICMP_TYPE_ANY      0xffffu                     /* dst_port wildcard for an ICMP rule */

enum fw_verdict { FW_DROP = 0, FW_ACCEPT = 1 };

/* Directions. TO_UPLINK/TO_GUEST are decided by the egress in ipv4_forward
 * (the FORWARD chain); TO_HOST is a datagram a guest tap delivers to the host
 * itself, in ipv4_input (the INPUT chain). ANY is a rule wildcard for the
 * *forwarding* directions only -- it keeps the meaning it had before the
 * INPUT chain existed and never matches TO_HOST, so a wildcard written to
 * permit forwarding cannot silently open a host service; host traffic needs
 * an explicit TO_HOST rule. */
enum fw_dir { FW_DIR_ANY = 0, FW_DIR_TO_UPLINK = 1, FW_DIR_TO_GUEST = 2, FW_DIR_TO_HOST = 3 };
#define FW_DIR_COUNT 3u                                   /* policy slots: uplink, guest, host */

/* One rule: the match tuple plus the verdict. dst_prefix 0 matches any
 * destination (dst_ip must then be 0); proto 0 any protocol. dst_port is the
 * transport selector: for proto TCP/UDP/any, a destination port (0 = any; a
 * non-zero port matches only a TCP/UDP datagram with that port); for proto
 * ICMP, the ICMP type 0..255 or FW_ICMP_TYPE_ANY. */
struct fw_rule {
    uint8_t  direction;   /* enum fw_dir */
    uint8_t  proto;       /* IPPROTO_TCP/UDP/ICMP, or 0 */
    uint8_t  dst_prefix;  /* 0..32 */
    uint8_t  verdict;     /* enum fw_verdict */
    uint32_t dst_ip;      /* network order */
    uint16_t dst_port;    /* host order: port, or ICMP type / FW_ICMP_TYPE_ANY */
};

/* Attach a guest (the tap's open): its address and its tap's gateway (the
 * host address the guest talks to). Resets it to the defaults and seeds the
 * two TO_HOST rules that keep the tap's own services reachable (udp
 * gateway/32 :53, icmp gateway/32 type 8). A re-attach of a live address
 * resets it likewise. Detach a guest and remove every rule, its policy and
 * every flow that names its address (the tap's release, before the subnet is
 * reused). Both hold g_fw_lock, so they are strictly ordered against adds. */
void fw_guest_attach(uint32_t guest_ip, uint32_t gateway_ip);
void fw_guest_purge(uint32_t guest_ip);

/* Insert `r` into the guest's list at `at_index` (>= count appends). 0 on
 * success; -ENOENT (guest not attached), -EINVAL (bad field), -EEXIST (that
 * tuple is already installed), -ENOSPC (the guest's list is full). */
int fw_rule_add(uint32_t guest_ip, unsigned at_index, const struct fw_rule *r);
/* Remove the guest's rule whose tuple equals `match`. 0, or -ENOENT. */
int fw_rule_del(uint32_t guest_ip, const struct fw_rule *match);
/* Snapshot the guest's rules in evaluation order; the count written. */
unsigned fw_rule_list(uint32_t guest_ip, struct fw_rule *out, unsigned max);
/* Set / read a guest's default verdicts. direction must be TO_UPLINK,
 * TO_GUEST or TO_HOST. -ENOENT if the guest is not attached, -EINVAL on a bad
 * field. */
int fw_policy_set(uint32_t guest_ip, uint8_t direction, uint8_t verdict);
int fw_policy_get(uint32_t guest_ip, uint8_t *to_uplink, uint8_t *to_guest, uint8_t *to_host);
/* The attached guests' addresses; the count written. */
unsigned fw_guest_list(uint32_t *out, unsigned max);

/*
 * FORWARD: the verdict for a datagram being forwarded from `in` out `out`.
 * `m` still carries the whole IP header (iph/ihl describe it) and has passed
 * ipv4_forward's anti-spoof, so iph->src is the ingress guest. Reads the
 * transport header by copy (no pullup: m is never re-pointed). On FW_DROP the
 * caller frees m.
 */
enum fw_verdict fw_forward_verdict(struct netif *in, struct netif *out, struct mbuf *m,
                                   const struct ipv4_hdr *iph, unsigned ihl);

/*
 * INPUT: the verdict for a datagram a guest tap (`nif`, NETIF_MASQUERADE)
 * is about to deliver to the host -- unicast to one of our addresses after
 * nat_in has declined it, or a broadcast. `m` still carries the whole IP
 * header. Applies the anti-spoof first (the source must be the tap's guest,
 * <subnet>.15, or the datagram is dropped as spoofed -- the check
 * ipv4_forward makes, now made on this path too), then the guest's TO_HOST
 * rules first-match, else its TO_HOST default. Stateless. On FW_DROP the
 * caller frees m. A guest the firewall never saw attached takes the built-in
 * TO_HOST default (DROP) with no seeded rules -- fail closed.
 */
enum fw_verdict fw_input_verdict(struct netif *nif, struct mbuf *m,
                                 const struct ipv4_hdr *iph, unsigned ihl);

/* Reclaim expired flows (the network worker's periodic tick, beside nat_age). */
void fw_age(uint64_t now_ns);
/* Drop every flow and reset every attached guest to "as attached" -- the
 * default policies and the two seeded rules, nothing else (attachments
 * kept) -- test isolation. */
void fw_flush(void);

struct fw_stats {
    uint64_t accept_rule, drop_rule;             /* FORWARD verdicts from a matching rule */
    uint64_t accept_default, drop_default;       /* FORWARD verdicts from the default policy */
    uint64_t accept_established;                 /* a flow-table hit */
    uint64_t flow_new, flow_drop_full;           /* flows recorded / refused for the guest's share */
    uint64_t expired;
    uint64_t in_accept_rule, in_drop_rule;       /* INPUT verdicts from a matching rule */
    uint64_t in_accept_default, in_drop_default; /* INPUT verdicts from the default policy */
    uint64_t in_spoofed;                         /* INPUT: source was not the tap's guest */
    uint32_t flows, rules;                       /* live right now */
};
void fw_get_stats(struct fw_stats *out);

#endif /* KERNEL_NET_FW_H */
