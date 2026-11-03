/*
 * procnet_floor.c - What procnet.cee cannot hold (roadmap 2.0 SA6): the
 * file names procfs lists /proc/net by, data procnet.h exports.
 */

#include <kernel/net/procnet.h>

const char *const procnet_names[PROCNET_COUNT] = {
    [PROCNET_DEV] = "dev",   [PROCNET_ROUTE] = "route", [PROCNET_ARP] = "arp",   [PROCNET_TCP] = "tcp",
    [PROCNET_TCP6] = "tcp6", [PROCNET_UDP] = "udp",     [PROCNET_UDP6] = "udp6", [PROCNET_IF_INET6] = "if_inet6",
};
