/*
 * udp.h - UDP (RFC 768) over IPv4 and IPv6.
 */

#ifndef KERNEL_NET_UDP_H
#define KERNEL_NET_UDP_H

#include <kernel/list.h>
#include <kernel/mbuf.h>
#include <kernel/net/inet.h>
#include <kernel/net/ip.h>

struct udp_hdr {
    uint16_t sport;
    uint16_t dport;
    uint16_t len;
    uint16_t cksum;
} __packed;

#define UDP_RXQ_MAX 64
#define NET_EPHEMERAL_LO 49152u
#define NET_EPHEMERAL_HI 65535u

struct socket;

struct udp_pcb {
    struct netaddr local;          /* family always set; port 0 = unbound */
    struct netaddr remote;         /* connected peer or unspecified */
    struct mbufq rxq;
    struct socket *sock;
    struct list_node link;
    uint64_t rx_dropped;
};

void udp_init(void);
int udp_pcb_init(struct udp_pcb *pcb, uint16_t family);
/* Bind; port 0 picks an ephemeral one. -EADDRINUSE, -EADDRNOTAVAIL. */
int udp_bind(struct udp_pcb *pcb, const struct netaddr *local);
void udp_unbind(struct udp_pcb *pcb);
/* Build and send a datagram. Binds an ephemeral port if needed. */
int udp_sendto(struct udp_pcb *pcb, const void *data, size_t len, const struct netaddr *to);
/* Dequeue one datagram (or NULL). The mbuf's pkt.src is the sender. */
struct mbuf *udp_recv(struct udp_pcb *pcb);
void udp_input(struct netif *nif, struct mbuf *m, const struct ipv4_hdr *ip4, const struct ipv6_hdr *ip6);
struct udp_stats {
    uint64_t rx, rx_bad_len, rx_bad_cksum, rx_no_port, rx_queue_full, tx;
};
void udp_get_stats(struct udp_stats *out);

#endif /* KERNEL_NET_UDP_H */
