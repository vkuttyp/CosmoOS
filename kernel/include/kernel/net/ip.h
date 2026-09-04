/*
 * ip.h - IPv4, IPv6, ICMP, ICMPv6, neighbour discovery.
 */

#ifndef KERNEL_NET_IP_H
#define KERNEL_NET_IP_H

#include <kernel/mbuf.h>
#include <kernel/netif.h>

#define IPPROTO_ICMP   1
#define IPPROTO_TCP    6
#define IPPROTO_UDP    17
#define IPPROTO_ICMPV6 58

#define IP_DEFAULT_TTL 64

struct ipv4_hdr {
    uint8_t  vhl;        /* version 4, header length in words */
    uint8_t  tos;
    uint16_t len;
    uint16_t id;
    uint16_t frag;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t cksum;
    uint32_t src;
    uint32_t dst;
} __packed;
#define IPV4_HDR_LEN(h) ((unsigned)((h)->vhl & 0xf) * 4u)

struct ipv6_hdr {
    uint32_t vtcfl;      /* version 6, traffic class, flow label */
    uint16_t plen;       /* payload length */
    uint8_t  nexthdr;
    uint8_t  hoplimit;
    struct in6_addr src;
    struct in6_addr dst;
} __packed;

struct icmp_hdr {
    uint8_t type;
    uint8_t code;
    uint16_t cksum;
    uint16_t id;
    uint16_t seq;
} __packed;
#define ICMP_ECHO_REPLY   0
#define ICMP_DEST_UNREACH 3
#define ICMP_ECHO         8
#define ICMP_UNREACH_PORT 3
#define ICMP_UNREACH_PROTO 2
#define ICMPV6_DEST_UNREACH 1
#define ICMPV6_ECHO       128
#define ICMPV6_ECHO_REPLY 129
#define ICMPV6_NS         135
#define ICMPV6_NA         136

/* Worker thread input; both take the packet. `m->data` at the IP header. */
void ipv4_input(struct netif *nif, struct mbuf *m);
void ipv6_input(struct netif *nif, struct mbuf *m);

/* Output: m->data at the transport header. Chooses the interface,
 * fills the IP header, resolves the next hop. Takes the packet. */
int ipv4_output(struct mbuf *m, uint32_t src, uint32_t dst, uint8_t proto, uint8_t ttl);
int ipv6_output(struct mbuf *m, const struct in6_addr *src, const struct in6_addr *dst, uint8_t proto,
                uint8_t hoplimit);
/* Source address selection for a destination (0 / unspecified if none). */
uint32_t ipv4_source_for(uint32_t dst);
void ipv6_source_for(const struct in6_addr *dst, struct in6_addr *src);
/* The interface a destination is sent through, or NULL. */
struct netif *ipv4_route(uint32_t dst);
struct netif *ipv6_route(const struct in6_addr *dst);

void icmp_input(struct netif *nif, struct mbuf *m, const struct ipv4_hdr *iph);
void icmp_send_unreach(struct mbuf *orig, const struct ipv4_hdr *iph, uint8_t code);   /* borrows orig */
void icmpv6_input(struct netif *nif, struct mbuf *m, const struct ipv6_hdr *ip6);
/* Echo replies are reported to an optional observer (tests). */
typedef void (*icmp_echo_reply_fn)(uint32_t src, uint16_t id, uint16_t seq);
void icmp_set_echo_reply_hook(icmp_echo_reply_fn fn);
void icmp_echo_reply_hook(uint32_t src, uint16_t id, uint16_t seq);
/* Send an echo request (tests, diagnostics). */
int icmp_send_echo(uint32_t dst, uint16_t id, uint16_t seq, const void *payload, size_t len);

/* Neighbour discovery: like ARP for IPv6. */
void nd_init(void);
int nd_resolve(struct netif *nif, const struct in6_addr *ip, uint8_t mac[6], struct mbuf *m);
void nd_input_ns(struct netif *nif, struct mbuf *m, const struct ipv6_hdr *ip6);
void nd_input_na(struct netif *nif, struct mbuf *m, const struct ipv6_hdr *ip6);
void nd_age(uint64_t now_ns);

struct ip_stats {
    uint64_t rx, rx_bad_header, rx_bad_cksum, rx_not_for_us, rx_fragments, rx_unknown_proto, tx, tx_no_route;
    uint64_t icmp_echo_rcvd, icmp_echo_replied, icmp_unreach_sent;
};
void ipv4_get_stats(struct ip_stats *out);
void ipv6_get_stats(struct ip_stats *out);

#endif /* KERNEL_NET_IP_H */
