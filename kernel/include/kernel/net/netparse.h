/*
 * netparse.h - The network receive path's decoders (roadmap 2.0 SA5):
 * every check and field read the stack makes on bytes a frame brought,
 * written in CeeSafe (kernel-services/network/netparse.cee, whose doc
 * comments are the API). Their result structs and prototypes are
 * ceesafec's (netparse_types.h, generated); this header adds the result
 * codes. Addresses come back in network order, as copied bytes; ports,
 * ids and sequence numbers in host order.
 */

#ifndef KERNEL_NET_NETPARSE_H
#define KERNEL_NET_NETPARSE_H

#include <kernel/net/netparse_types.h>

/* netparse_ether */
#define NETPARSE_ETH_OTHER     0u   /* unicast to another host: drop */
#define NETPARSE_ETH_OWN       1u   /* sent by this interface and reflected: drop */
#define NETPARSE_ETH_UNICAST   2u
#define NETPARSE_ETH_BROADCAST 3u
#define NETPARSE_ETH_MULTICAST 4u

/* netparse_ipv4_outer, netparse_ipv4_header, netparse_ipv6, netparse_udp, netparse_tcp, netparse_dhcp */
#define NETPARSE_OK        0u
#define NETPARSE_BAD       1u   /* malformed, or claims more bytes than arrived */
#define NETPARSE_BAD_CKSUM 2u
#define NETPARSE_FRAGMENT  3u   /* MF or an offset: fragments are not reassembled */

#endif /* KERNEL_NET_NETPARSE_H */
