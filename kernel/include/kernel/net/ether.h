/*
 * ether.h - Ethernet framing and ARP.
 */

#ifndef KERNEL_NET_ETHER_H
#define KERNEL_NET_ETHER_H

#include <kernel/mbuf.h>
#include <kernel/netif.h>

#define ETH_ALEN    6
#define ETH_HLEN    14
#define ETH_ZLEN    60   /* minimum frame without FCS */
#define ETH_P_IP    0x0800
#define ETH_P_ARP   0x0806
#define ETH_P_IPV6  0x86DD

struct eth_hdr {
    uint8_t dst[ETH_ALEN];
    uint8_t src[ETH_ALEN];
    uint16_t type;          /* network order */
} __packed;

extern const uint8_t eth_broadcast[ETH_ALEN];

/* Worker thread: parse and dispatch. Takes the packet. */
void ether_input(struct netif *nif, struct mbuf *m);
/* Prepend a header and transmit. Takes the packet. */
int ether_output(struct netif *nif, struct mbuf *m, const uint8_t dst[ETH_ALEN], uint16_t type);

/* ARP (RFC 826). */
#define ARP_TABLE_SIZE 64
void arp_init(void);
void arp_input(struct netif *nif, struct mbuf *m);
/* MAC for `ip` on `nif`. 0: mac filled. -EINPROGRESS: `m` was queued
 * and a request sent (ownership taken). Other errno: `m` freed. */
int arp_resolve(struct netif *nif, uint32_t ip, uint8_t mac[ETH_ALEN], struct mbuf *m);
bool arp_lookup(uint32_t ip, uint8_t mac[ETH_ALEN]);
void arp_flush(struct netif *nif);
/* Test hook: run the ageing pass as if `now_ns` had passed. */
void arp_age(uint64_t now_ns);
struct arp_stats {
    uint64_t requests_sent, replies_sent, requests_rcvd, replies_rcvd, entries, pending_dropped, timeouts;
    uint64_t unsolicited;   /* replies that answered no request of ours */
};
void arp_get_stats(struct arp_stats *out);

#endif /* KERNEL_NET_ETHER_H */
