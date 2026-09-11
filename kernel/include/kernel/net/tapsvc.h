/*
 * tapsvc.h - The tap's autoconfiguration service: a DHCP server and a DNS
 * proxy for the one guest on a tap (docs/audit/next-subsystem-dhcp-dns.md).
 *
 * DHCP is handled at the frame level, scoped to the tap (a tap input filter,
 * replies sent out the tap with ether_output per RFC 2131 §4.1): the guest
 * has no address yet and the tap is NETIF_NODEFAULT, so routing cannot carry
 * the reply. DNS is an ordinary in-kernel UDP relay on the gateway address.
 *
 * One instance per tap: /dev/net/tap starts one for each owner's tap on
 * open and stops it on the last close.
 */
#ifndef KERNEL_NET_TAPSVC_H
#define KERNEL_NET_TAPSVC_H

#include <kernel/types.h>

struct tap;

/* Guest address convention within the tap's subnet (the tap unit's slot). */
#define TAPSVC_GUEST_HOST   15u        /* the guest is <subnet>.15, the host <subnet>.1 */
#define TAPSVC_LEASE_SECS   3600u

/* DHCP message types (RFC 2132 option 53), shared with the self-test. */
#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_DECLINE  4
#define DHCP_ACK      5
#define DHCP_NAK      6
#define DHCP_RELEASE  7

struct tapsvc;
/* Start a service instance for this tap (its DHCP binding and DNS proxy);
 * NULL on no memory or too many instances. The caller keeps the pointer. */
struct tapsvc *tapsvc_start(struct tap *t);
/* Stop and free it: the DHCP filter is dropped and the DNS threads joined
 * before their sockets go. Call before destroying the tap. NULL is a no-op. */
void tapsvc_stop(struct tapsvc *svc);

struct tapsvc_stats {
    uint64_t dhcp_discover, dhcp_offer, dhcp_request, dhcp_ack, dhcp_nak, dhcp_release;
    uint64_t dhcp_ignored;             /* a second client, or a malformed packet */
    uint64_t dns_query, dns_answer, dns_servfail, dns_drop_full, dns_expired;
    uint32_t dns_pending;              /* live pending queries right now */
};
void tapsvc_get_stats(struct tapsvc_stats *out);

/* Reclaim DNS pending entries past their timeout (the net worker calls this
 * from periodic aging; tests drive it with a future timestamp). */
void tapsvc_dns_age(uint64_t now_ns);
/* Test hook: point the DNS proxy at a chosen upstream (ip/port network+host). */
void tapsvc_test_set_upstream(struct tapsvc *svc, uint32_t ip, uint16_t port);

#endif /* KERNEL_NET_TAPSVC_H */
