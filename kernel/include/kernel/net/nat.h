/*
 * nat.h - Masquerade (source) NAT for forwarded IPv4 flows.
 *
 * A datagram forwarded out an interface whose subnet does not hold its
 * source -- a private guest going out an uplink -- has its source address
 * rewritten to the egress interface's address and its transport identifier
 * (TCP/UDP source port, or ICMP echo id) rewritten to a free value the NAT
 * owns, so the far side answers the host's real address. The reply,
 * arriving for that value, is rewritten back to the original guest address
 * and identifier and forwarded to the guest. A bounded conntrack table
 * remembers each flow; entries expire and a full table drops new flows.
 *
 * Inbound port forwarding (DNAT, docs/audit/next-subsystem-dnat.md) is the
 * mirror: a connection to a configured host port is rewritten to a guest
 * address/port and forwarded to the guest, and the guest's reply is
 * rewritten back. Only IPv4 UDP, TCP and ICMP echo are masqueraded; IPv6 is
 * a later unit.
 */
#ifndef KERNEL_NET_NAT_H
#define KERNEL_NET_NAT_H

#include <kernel/types.h>

struct netif;
struct mbuf;
struct ipv4_hdr;

#define NAT_TABLE_SIZE 256u        /* conntrack entries; a full table drops new flows */
#define NAT_PORT_MIN   40000u      /* the NAT identifier range (ports and ICMP ids); kept below
                                    * NET_EPHEMERAL_LO (49152) so a host outbound flow, which
                                    * sources from an ephemeral port, never collides with a lent one */
#define NAT_PORT_MAX   49151u

/* Idle timeouts (RFC 5382 / 5508 order of magnitude, shortened). */
#define NAT_TIMEOUT_UDP_NS   (30ull * 1000000000ull)
#define NAT_TIMEOUT_ICMP_NS  (30ull * 1000000000ull)
#define NAT_TIMEOUT_TCP_NS   (30ull * 1000000000ull)    /* a half-open / new TCP flow */
#define NAT_TIMEOUT_TCPEST_NS (300ull * 1000000000ull)  /* once both sides have been seen */

#define NAT_PF_MAX 16u             /* static port-forward (DNAT) rules */
#define NAT_GUESTS 8u              /* max concurrent guests (mirrors tap.c TAP_MAX_GUESTS) */
#define NAT_QUOTA_PER_GUEST (NAT_TABLE_SIZE / NAT_GUESTS)  /* one guest's share of the table */

/* Configure the port-forward table from a fw_cfg string: a comma-separated
 * list of `proto:hostport:guestaddr:guestport` (proto tcp|udp), a wildcard
 * host-address bind. Replaces the table; ignores malformed rules. */
void nat_portforward_config(const char *cfg);
/* The same parse, but adds to the table without clearing it (a rule already
 * bound, or targeting a tap that is not up, is skipped); for applying the
 * boot-time rules as each guest's tap appears. */
void nat_portforward_apply(const char *cfg);
/* Add one rule (proto IPPROTO_TCP/UDP, ports host order, guest_ip network
 * order). 0 on success; -EINVAL (invalid fields or the target is not on the
 * guest tap's subnet), -EEXIST (already bound), -ENOSPC (table full). */
int nat_pf_add(uint8_t proto, uint16_t host_port, uint32_t guest_ip, uint16_t guest_port);
/* Remove the rule bound to (proto, host_port) and reap the DNAT conntrack
 * entries it created (so an in-flight flow stops at once). false if none. */
bool nat_pf_del(uint8_t proto, uint16_t host_port);
void nat_pf_clear(void);

struct nat_pf_rule {              /* one port-forward rule, for listing */
    uint8_t proto;
    uint16_t host_port;           /* host order */
    uint16_t guest_port;          /* host order */
    uint32_t guest_ip;            /* network order */
};
/* Snapshot the live rules into out[0..max); returns the count written. */
unsigned nat_pf_list(struct nat_pf_rule *out, unsigned max);

/* Remove every port-forward rule whose target is `guest_ip` and every
 * conntrack entry (masquerade or DNAT) whose guest side is that address, and
 * nothing else -- called when a guest departs, before its subnet is reused,
 * so a later guest handed the same address inherits no stale flows or rules.
 * Other guests' state is untouched. */
void nat_guest_purge(uint32_t guest_ip);

/*
 * Outbound: masquerade a datagram being forwarded from `in` out `out`.
 * `m` still carries the whole IP header; iph/ihl describe it. When the
 * source is not on the egress subnet, the transport source identifier and
 * (via *new_src) the source address are rewritten and the transport
 * checksum fixed up, and a conntrack entry is recorded or refreshed.
 * Returns the source address output_on should stamp (the egress address
 * when masqueraded, the original source otherwise) in *new_src. Returns 0
 * on success (including "not masqueraded"), -ENOMEM/-ENOSPC when a needed
 * entry could not be made (the caller drops the packet).
 */
int nat_out(struct netif *in, struct netif *out, struct mbuf *m,
            const struct ipv4_hdr *iph, unsigned ihl, uint32_t *new_src);

/*
 * Inbound: a datagram addressed to one of our own addresses has arrived on
 * `nif`. If it is the reply to a masqueraded flow (or an ICMP error quoting
 * one), rewrite it back to the original guest and forward it there, and
 * return true (m consumed). Return false when it is not NAT traffic, and
 * the caller delivers it to the host normally. `m` carries the whole IP
 * header; iph/ihl/total describe it.
 */
bool nat_in(struct netif *nif, struct mbuf *m,
            const struct ipv4_hdr *iph, unsigned ihl, uint16_t total);

/* Reclaim entries whose idle timeout has passed. The network worker calls
 * this from its periodic ARP/ND aging (arp.c age_work); tests drive it
 * directly with a future timestamp. Lookups also skip expired entries and
 * nat_alloc reclaims them on demand, so a missed sweep is never a
 * correctness bug, only delayed reclamation. */
void nat_age(uint64_t now_ns);
void nat_flush(void);                 /* drop every entry (test isolation) */

struct nat_stats {
    uint64_t out_new, out_reuse, out_drop_full, out_drop_noport;
    uint64_t in_translated, in_no_match, in_icmp_error;
    uint64_t dnat_in, dnat_reply, dnat_drop_full;
    uint64_t expired;
    uint32_t entries;                 /* live entries right now */
};
void nat_get_stats(struct nat_stats *out);

#endif /* KERNEL_NET_NAT_H */
