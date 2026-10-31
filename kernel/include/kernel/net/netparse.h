/*
 * netparse.h - The network receive path's decoders: every check and field
 * read the stack makes on bytes a frame brought (roadmap 2.0 SA5, the
 * network-frame family). The callers keep the mbufs, the tables and the
 * policy; these see only a buffer the caller has made contiguous (with
 * m_pullup or m_copydata) and its length, and say what it holds or why it
 * is dropped. No function here reads past the bytes its comment names.
 *
 * Addresses are copied byte for byte, so they stay in network order: a
 * caller compares them with memcmp or copies them into its own uint32_t
 * or struct in6_addr. Ports, ids and sequence numbers are in host order.
 */

#ifndef KERNEL_NET_NETPARSE_H
#define KERNEL_NET_NETPARSE_H

#include <kernel/types.h>

/* --- Ethernet ---------------------------------------------------------- */

#define NETPARSE_ETH_OTHER     0u   /* unicast to another host: drop (a promiscuous device's) */
#define NETPARSE_ETH_OWN       1u   /* sent by this interface and reflected: drop */
#define NETPARSE_ETH_UNICAST   2u
#define NETPARSE_ETH_BROADCAST 3u
#define NETPARSE_ETH_MULTICAST 4u

/* The 14-byte header `hdr` of a frame arriving on an interface whose MAC is
 * `own` (6 bytes): one of NETPARSE_ETH_*, and the EtherType in *type. */
uint32_t netparse_ether(const uint8_t *hdr, const uint8_t *own, uint16_t *type);

/* --- ARP --------------------------------------------------------------- */

struct netparse_arp {
    uint16_t op;          /* ARP_OP_REQUEST, ARP_OP_REPLY or another */
    uint8_t sha[6];       /* the sender's MAC */
    uint8_t spa[4];       /* the sender's IPv4 address */
    uint8_t tpa[4];       /* the target's IPv4 address */
};

/* The 28-byte packet `pkt`: true for Ethernet hardware and IPv4 protocol
 * addresses of their sizes, with the fields in *out; false for any other. */
bool netparse_arp(const uint8_t *pkt, struct netparse_arp *out);

/* --- IPv4 -------------------------------------------------------------- */

#define NETPARSE_OK        0u
#define NETPARSE_BAD       1u   /* a malformed header, or one that claims more bytes than arrived */
#define NETPARSE_BAD_CKSUM 2u
#define NETPARSE_FRAGMENT  3u   /* MF or an offset: fragments are not reassembled */

/* The fixed 20 bytes of an IPv4 header, in a packet of `pkt_len` bytes:
 * NETPARSE_OK with the header's length in *ihl (20-60) and the datagram's
 * in *total (ihl..pkt_len), or NETPARSE_BAD. */
uint32_t netparse_ipv4_outer(const uint8_t *hdr, uint32_t pkt_len, uint32_t *ihl, uint32_t *total);

/* The whole header, `ihl` bytes as netparse_ipv4_outer gave them:
 * NETPARSE_OK, NETPARSE_BAD_CKSUM or NETPARSE_FRAGMENT. */
uint32_t netparse_ipv4_header(const uint8_t *hdr, uint32_t ihl);

/* An IPv4 header quoted in an ICMP error, its first 20 bytes: true with its
 * length in *ihl when it is version 4 and at least 20 bytes long. Nothing
 * else is checked: a quote is truncated and its checksum is the sender's. */
bool netparse_ipv4_quote(const uint8_t *q, uint32_t *ihl);

/* --- IPv6 -------------------------------------------------------------- */

/* The 40-byte header of a packet of `pkt_len` bytes: NETPARSE_OK with the
 * payload length in *plen (at most pkt_len - 40), or NETPARSE_BAD. */
uint32_t netparse_ipv6(const uint8_t *hdr, uint32_t pkt_len, uint32_t *plen);

struct netparse_nd {
    uint8_t target[16];   /* the solicited or advertised address */
    uint8_t mac[6];       /* the link-layer address option's, when has_mac */
    bool has_mac;
};

/* A 32-byte neighbour solicitation or advertisement (its ICMPv6 header, the
 * target, one option): the target, and the MAC when the option is of type
 * `opt_type` (1, source; 2, target link-layer address) and 8 bytes long. */
void netparse_nd(const uint8_t *msg, uint8_t opt_type, struct netparse_nd *out);

/* --- transports -------------------------------------------------------- */

struct netparse_udp {
    uint16_t len;         /* the datagram's length, header included: 8..pkt_len */
    uint16_t sport;
    uint16_t dport;
    bool has_cksum;       /* a zero checksum field means none (IPv4 only) */
};

/* The 8-byte header of a UDP datagram whose IP payload is `pkt_len` bytes:
 * NETPARSE_OK with its fields in *out, or NETPARSE_BAD for a length below
 * the header's or beyond the payload. */
uint32_t netparse_udp(const uint8_t *hdr, uint32_t pkt_len, struct netparse_udp *out);

struct netparse_l4 {
    uint16_t sport;       /* UDP and TCP: bytes 0-1 */
    uint16_t dport;       /* bytes 2-3 */
    uint32_t seq;         /* TCP: bytes 4-7 */
    uint8_t icmp_type;    /* ICMP: byte 0 */
    uint16_t icmp_id;     /* ICMP echo: bytes 4-5 */
    uint8_t tcp_flags;    /* TCP: byte 13, when `avail` reaches it; else 0 */
};

/* The identifiers NAT, the firewall and ICMP errors key a flow on, from the
 * first `avail` bytes (at least 8) of a transport header: every field, read
 * as the protocol it would be; the caller picks the ones its protocol has. */
void netparse_l4(const uint8_t *b, uint32_t avail, struct netparse_l4 *out);

/* --- TCP (the segment family) ---------------------------------------- */

struct netparse_tcp {
    uint32_t hlen;        /* the header's length, options included: 20..60, at most the segment's */
    uint16_t sport;
    uint16_t dport;
    uint32_t seq;
    uint32_t ack;
    uint8_t flags;        /* TH_* */
    uint16_t win;
};

/* The fixed 20 bytes of a TCP header, in a segment of `seg_len` bytes
 * (header and data): NETPARSE_OK with its fields in *out, or NETPARSE_BAD
 * for a data offset below 20 bytes or beyond the segment. */
uint32_t netparse_tcp(const uint8_t *hdr, uint32_t seg_len, struct netparse_tcp *out);

/* The maximum segment size a SYN's options announce, `optlen` bytes (0-40)
 * after the fixed header, raised to 536 (RFC 1122 §4.2.2.6); `dflt` when
 * there is none, or the options end, are padded out or malformed first.
 * End-of-options and no-op are one byte; every other option is a kind and
 * a length (2 or more) that must fit in what is left. */
uint16_t netparse_tcp_mss(const uint8_t *opts, uint32_t optlen, uint16_t dflt);

/* --- DHCP and DNS ----------------------------------------------------- */

/* The DHCP option `code` in `len` bytes of options: its value's offset
 * from `opts`, with its length in *olen, or -1. Stops at the end option
 * (255), skips pads (0), and never reads past `len`: an option whose
 * length runs past the end ends the walk. (dhcp.h's dhcp_opt.) */
int32_t netparse_dhcp_opt(const uint8_t *opts, uint32_t len, uint8_t code, uint8_t *olen);

struct netparse_dhcp {
    uint8_t msg_type;     /* the message type (option 53, one byte) */
    uint8_t xid[4];       /* as on the wire */
    uint16_t flags;       /* BOOTP flags (DHCP_FLAG_BROADCAST) */
    uint8_t ciaddr[4];
    uint8_t yiaddr[4];
    uint8_t chaddr[6];    /* the client's MAC */
    uint8_t server[4];    /* option 54, when has_server */
    bool has_server;
    uint8_t reqip[4];     /* option 50, when has_reqip */
    bool has_reqip;
};

/* A DHCP message of `dhlen` bytes (BOOTP header, cookie, options): OK with
 * its fields in *out when it is at least DHCP_FIXED bytes, its op is `op`
 * (BOOTP_REQUEST or BOOTP_REPLY), its hardware is Ethernet with 6-byte
 * addresses, its cookie is DHCP's, and it has a one-byte message type; or
 * NETPARSE_BAD. The options are the `dhlen - DHCP_FIXED` bytes after
 * DHCP_FIXED. */
uint32_t netparse_dhcp(const uint8_t *dh, uint32_t dhlen, uint8_t op, struct netparse_dhcp *out);

struct netparse_dhcp_lease {
    uint8_t addr[4];      /* yiaddr */
    uint32_t lease;       /* seconds (option 51); 0xffffffff is infinite */
    uint8_t mask[4];      /* option 1, when has_mask */
    bool has_mask;
    uint8_t router[4];    /* the first of option 3, when has_router */
    bool has_router;
    uint8_t ndns;         /* option 6's addresses, at most DHCP_MAX_DNS */
    uint8_t dns[12];
    uint8_t hostname[64]; /* option 12, NUL-terminated, when it is a usable host name (else empty): 1-63 letters, digits, - and . */
    uint32_t t1;          /* option 58, 0 when absent or not 4 bytes */
    uint32_t t2;          /* option 59, likewise */
};

/* An ACK's lease (the message `dh` of `dhlen` bytes, as netparse_dhcp
 * accepted it): true with its parameters in *out; false when it is
 * unusable -- no address (0 or the broadcast), or no lease time, or a
 * lease of 0 (RFC 2131 table 3). */
bool netparse_dhcp_lease(const uint8_t *dh, uint32_t dhlen, struct netparse_dhcp_lease *out);

/* A frame a guest's tap sent, `len` bytes: true when it is an IPv4 UDP
 * datagram to the DHCP server port with its headers whole, with the DHCP
 * message's offset in the frame in *dh_off (its length is the rest of the
 * frame); false when it is anything else, for the stack. */
bool netparse_dhcp_frame(const uint8_t *frame, uint32_t len, uint32_t *dh_off);

/* A DNS message's id, its first two bytes (`len` is the message's length,
 * at least the 12-byte header). */
uint16_t netparse_dns_id(const uint8_t *msg, uint32_t len);

/* A dotted quad (`len` characters, no NUL needed): true with the address in
 * network order in *out (4 bytes); false for anything else. */
bool netparse_ipv4_text(const char *s, uint32_t len, uint8_t *out);

#endif /* KERNEL_NET_NETPARSE_H */
