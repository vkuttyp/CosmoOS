/*
 * procnet.h - /proc/net: the network stack's state in Linux's text
 * formats (shell completeness S1; docs/kernel-services/network/design.md,
 * "Inspecting the stack"). procfs owns the paths and the snapshot-at-open
 * rule (P3); the stack owns the formats, rendered from the listings
 * (netif_list, ipv4_route_list, arp_list, tcp_list, udp_list), each a copy
 * taken under its table's lock and formatted after it is released.
 */

#ifndef KERNEL_NET_PROCNET_H
#define KERNEL_NET_PROCNET_H

#include <kernel/types.h>

enum procnet_file {
    PROCNET_DEV,        /* per-interface counters */
    PROCNET_ROUTE,      /* IPv4 routes */
    PROCNET_ARP,        /* the ARP table */
    PROCNET_TCP,        /* IPv4 TCP sockets */
    PROCNET_TCP6,       /* IPv6 TCP sockets */
    PROCNET_UDP,        /* IPv4 UDP sockets */
    PROCNET_UDP6,       /* IPv6 UDP sockets */
    PROCNET_IF_INET6,   /* IPv6 interface addresses */
    PROCNET_COUNT,
};

extern const char *const procnet_names[PROCNET_COUNT];

/* The most one file renders: a listing longer than this ends at its last
 * whole line, as /proc/mounts does. Just under sixteen pages (400 sockets
 * at /proc/net/tcp's 150 bytes a line), so the text and procfs's header
 * stay inside one 64 KiB allocation. */
#define PROCNET_TEXT_MAX (64u * 1024u - 64u)

/* Render `f` into a buffer that starts at a page and doubles as the text
 * needs, up to PROCNET_TEXT_MAX, so a short file costs a short buffer.
 * Returns the length and sets *out (NUL-terminated, the caller's to
 * kfree), or -ENOMEM. Thread context. */
int procnet_render(enum procnet_file f, char **out);

#endif /* KERNEL_NET_PROCNET_H */
