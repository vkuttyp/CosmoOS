/*
 * raw.h - Raw ICMP sockets over IPv4: what `ping` opens
 * (docs/kernel-services/network/design.md, "Raw ICMP sockets").
 */

#ifndef KERNEL_NET_RAW_H
#define KERNEL_NET_RAW_H

#include <kernel/list.h>
#include <kernel/mbuf.h>
#include <kernel/net/inet.h>
#include <kernel/net/ip.h>

#define RAW_RXQ_MAX 64

struct socket;

struct raw_pcb {
    uint8_t proto;              /* IPPROTO_ICMP */
    struct mbufq rxq;           /* copies, each with its IPv4 header; pkt.src the sender */
    struct socket *sock;        /* cleared under the list lock at release */
    struct list_node link;
    uint64_t rx_dropped;
};

/* A pcb for `proto` on the list, or NULL without memory. */
struct raw_pcb *raw_pcb_new(struct socket *sock, uint8_t proto);
/* Off the list, its queue freed; the pcb with it. */
void raw_pcb_free(struct raw_pcb *pcb);
/* Send `len` bytes (the ICMP message, as built) to `to`. */
int raw_sendto(struct raw_pcb *pcb, const void *data, size_t len, const struct netaddr *to);
/* Dequeue one message (or NULL). */
struct mbuf *raw_recv(struct raw_pcb *pcb);
/* From icmp_input, after its checksum and quiet checks: a copy of the
 * message (`m` holds the ICMP part, `iph` its header) for each raw ICMP
 * socket. `m` is not consumed or changed (N31). */
void raw_icmp_input(const struct mbuf *m, const struct ipv4_hdr *iph);

struct raw_stats {
    uint64_t rx_delivered, rx_queue_full, rx_nomem, tx;
};
void raw_get_stats(struct raw_stats *out);

#endif /* KERNEL_NET_RAW_H */
