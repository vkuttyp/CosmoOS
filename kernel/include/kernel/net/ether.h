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

/* ARP (RFC 826). Entries are keyed by (interface, address): the same
 * address on two links is two neighbours (invariant N25). */
#define ARP_TABLE_SIZE 64
void arp_init(void);
void arp_input(struct netif *nif, struct mbuf *m);
/* MAC for `ip` on `nif`. 0: mac filled. -EINPROGRESS: `m` was queued
 * and a request sent (ownership taken). Other errno: `m` freed --
 * -ENETUNREACH when `nif` is down (decided under the table lock, so a
 * resolve never leaves an entry on a down interface; N25). */
int arp_resolve(struct netif *nif, uint32_t ip, uint8_t mac[ETH_ALEN], struct mbuf *m);
/* `nif`'s entry for `ip`, reachable: mac filled. Reads the table without sending. */
bool arp_lookup(const struct netif *nif, uint32_t ip, uint8_t mac[ETH_ALEN]);
/* Drop every entry naming the interface: netif_unregister (step 5) and
 * netif_set_up(false) -- a neighbour learned over a link that is down is
 * not known to be there when it comes back, and a packet parked on one
 * waits for a link that is not there (N25). */
void arp_flush(struct netif *nif);
/* Remove `nif`'s entry for `ip`, if present (a pending packet is dropped
 * and counted, as the flush and the timeout do). */
void arp_delete(const struct netif *nif, uint32_t ip);
/* Test hook: run the ageing pass as if `now_ns` had passed. */
void arp_age(uint64_t now_ns);
struct arp_stats {
    uint64_t requests_sent, replies_sent, requests_rcvd, replies_rcvd, entries, pending_dropped, timeouts;
    uint64_t unsolicited;   /* replies that answered no request of ours */
};
void arp_get_stats(struct arp_stats *out);

#if CONFIG_DEBUG
/* Park the next ARP retry batch between its unlock and its send -- the
 * one-unlock window a netif reference closes (invariant N22). The test
 * stops the retry there rather than racing netif_unregister against
 * age_work, the way tcp_test_hold_callback parks a timer callback. */
void arp_test_hold_retry(bool on);
bool arp_test_retry_parked(void);
void arp_test_release_retry(void);
/* Park the next arp_input or arp_resolve between its decision to proceed
 * and its taking of the table lock, where the old NETIF_UP check (before
 * the lock) could be overtaken by netif_set_up(false)'s flag clear and
 * flush (N25 follow-up). One caller per arming. */
void arp_test_hold_lock_entry(bool on);
bool arp_test_lock_entry_parked(void);
void arp_test_release_lock_entry(void);
#endif

#endif /* KERNEL_NET_ETHER_H */
