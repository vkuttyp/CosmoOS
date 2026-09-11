/*
 * fw.h - The forwarding firewall: a stateful packet filter over the guest
 * taps (docs/audit/next-subsystem-firewall.md).
 *
 * A single FORWARD chain, evaluated in ipv4_forward after the anti-spoof and
 * routing steps and before NAT, decides whether a datagram a guest forwards
 * is accepted or dropped. Each guest owns an ordered rule list (first match
 * wins) and a default verdict per direction -- TO_UPLINK (the egress is not a
 * guest tap: the world) and TO_GUEST (the egress is another guest's tap).
 * The defaults close the gap the multi-guest unit deferred: inter-guest
 * traffic is dropped unless a rule allows it; guest-to-uplink stays open.
 *
 * The filter is stateful where it has to be. A masqueraded guest-to-uplink
 * reply never re-enters ipv4_forward (nat_in delivers it via ipv4_output), and
 * exists only because a NAT conntrack entry -- created when the outbound flow
 * was accepted here -- matched it; conntrack is that direction's state. A
 * guest-to-guest flow is un-NAT'd, so both halves traverse ipv4_forward and
 * the filter keeps its own bounded flow table for it: an accepted NEW flow
 * is recorded, and its reply (the reverse tuple) is ESTABLISHED and accepted
 * without a reverse rule. ICMP is stateful for echo only, keyed on the echo
 * identifier (type 8 request -> type 0 reply, same id); a reverse echo
 * *request* is not a reply. Inbound DNAT is authorized by its port-forward
 * rule and is not re-gated here.
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
#define FW_RULES_PER_GUEST    32u                         /* one guest's ordered rule list */
#define FW_FLOW_MAX           256u                        /* stateful entries (guest-to-guest) */
#define FW_FLOW_QUOTA_PER_GUEST (FW_FLOW_MAX / FW_MAX_GUESTS) /* one guest's share; a flood starves itself */

enum fw_verdict { FW_DROP = 0, FW_ACCEPT = 1 };

/* Directions. ANY is a rule wildcard; a datagram is always one of the other two. */
enum fw_dir { FW_DIR_ANY = 0, FW_DIR_TO_UPLINK = 1, FW_DIR_TO_GUEST = 2 };

/* One rule: the match tuple plus the verdict. dst_prefix 0 matches any
 * destination; proto 0 any protocol; dst_port 0 any port. For proto ICMP
 * dst_port must be 0 (ports do not apply); for any other proto a non-zero
 * dst_port matches only a TCP/UDP datagram with that destination port. */
struct fw_rule {
    uint8_t  direction;   /* enum fw_dir */
    uint8_t  proto;       /* IPPROTO_TCP/UDP/ICMP, or 0 */
    uint8_t  dst_prefix;  /* 0..32 */
    uint8_t  verdict;     /* enum fw_verdict */
    uint32_t dst_ip;      /* network order */
    uint16_t dst_port;    /* host order */
};

/* Attach a guest (the tap's open); a re-attach of a live address resets it to
 * the defaults. Detach a guest and remove every rule, its policy and every
 * flow that names its address (the tap's release, before the subnet is
 * reused). Both hold g_fw_lock, so they are strictly ordered against adds. */
void fw_guest_attach(uint32_t guest_ip);
void fw_guest_purge(uint32_t guest_ip);

/* Insert `r` into the guest's list at `at_index` (>= count appends). 0 on
 * success; -ENOENT (guest not attached), -EINVAL (bad field), -EEXIST (that
 * tuple is already installed), -ENOSPC (the guest's list is full). */
int fw_rule_add(uint32_t guest_ip, unsigned at_index, const struct fw_rule *r);
/* Remove the guest's rule whose tuple equals `match`. 0, or -ENOENT. */
int fw_rule_del(uint32_t guest_ip, const struct fw_rule *match);
/* Snapshot the guest's rules in evaluation order; the count written. */
unsigned fw_rule_list(uint32_t guest_ip, struct fw_rule *out, unsigned max);
/* Set / read a guest's default verdict. direction must be TO_UPLINK or
 * TO_GUEST. -ENOENT if the guest is not attached, -EINVAL on a bad field. */
int fw_policy_set(uint32_t guest_ip, uint8_t direction, uint8_t verdict);
int fw_policy_get(uint32_t guest_ip, uint8_t *to_uplink, uint8_t *to_guest);
/* The attached guests' addresses; the count written. */
unsigned fw_guest_list(uint32_t *out, unsigned max);

/*
 * The verdict for a datagram being forwarded from `in` out `out`. `m` still
 * carries the whole IP header (iph/ihl describe it) and has passed
 * ipv4_forward's anti-spoof, so iph->src is the ingress guest. Reads the
 * transport header by copy (no pullup: m is never re-pointed). On FW_DROP the
 * caller frees m.
 */
enum fw_verdict fw_forward_verdict(struct netif *in, struct netif *out, struct mbuf *m,
                                   const struct ipv4_hdr *iph, unsigned ihl);

/* Reclaim expired flows (the network worker's periodic tick, beside nat_age). */
void fw_age(uint64_t now_ns);
/* Drop every flow and reset every attached guest to no rules and the default
 * policy (attachments kept) -- test isolation. */
void fw_flush(void);

struct fw_stats {
    uint64_t accept_rule, drop_rule;             /* verdicts from a matching rule */
    uint64_t accept_default, drop_default;       /* verdicts from the default policy */
    uint64_t accept_established;                 /* a flow-table hit */
    uint64_t flow_new, flow_drop_full;           /* flows recorded / refused for the guest's share */
    uint64_t expired;
    uint32_t flows, rules;                       /* live right now */
};
void fw_get_stats(struct fw_stats *out);

#endif /* KERNEL_NET_FW_H */
