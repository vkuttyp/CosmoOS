/*
 * netparse.c - The network receive path's decoders (kernel/net/netparse.h),
 * the C that SA5 replaces with netparse.cee once the recorded digest
 * (tests/host/test_netparse.c) agrees. The checks are the receive path's
 * own, moved here unchanged: ether_input, arp_input, ipv4_input,
 * icmp_quoted_flow and nat_in_icmp_error, ipv6_input, nd_input_ns and
 * nd_input_na, udp_input, nat_in and the firewall's l4_read.
 */

#include <kernel/net/netparse.h>
#include <kernel/string.h>

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

uint32_t netparse_ether(const uint8_t *hdr, const uint8_t *own, uint16_t *type)
{
    static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    const uint8_t *dst = hdr, *src = hdr + 6;
    bool is_bcast = memcmp(dst, bcast, 6) == 0;
    bool is_mcast = (dst[0] & 1) != 0;
    *type = be16(hdr + 12);
    if (!is_bcast && !is_mcast && memcmp(dst, own, 6) != 0)
        return NETPARSE_ETH_OTHER;
    if (memcmp(src, own, 6) == 0)
        return NETPARSE_ETH_OWN;
    if (is_bcast)
        return NETPARSE_ETH_BROADCAST;
    if (is_mcast)
        return NETPARSE_ETH_MULTICAST;
    return NETPARSE_ETH_UNICAST;
}

bool netparse_arp(const uint8_t *pkt, struct netparse_arp *out)
{
    /* htype 1 (Ethernet), ptype 0x0800 (IPv4), hlen 6, plen 4. */
    if (be16(pkt) != 1 || be16(pkt + 2) != 0x0800 || pkt[4] != 6 || pkt[5] != 4)
        return false;
    out->op = be16(pkt + 6);
    memcpy(out->sha, pkt + 8, 6);
    memcpy(out->spa, pkt + 14, 4);
    memcpy(out->tpa, pkt + 24, 4);
    return true;
}

uint32_t netparse_ipv4_outer(const uint8_t *hdr, uint32_t pkt_len, uint32_t *ihl, uint32_t *total)
{
    uint32_t hl = (uint32_t)(hdr[0] & 0xf) * 4u;
    uint32_t len = be16(hdr + 2);
    if ((hdr[0] >> 4) != 4 || hl < 20 || len < hl || len > pkt_len)
        return NETPARSE_BAD;
    *ihl = hl;
    *total = len;
    return NETPARSE_OK;
}

uint32_t netparse_ipv4_header(const uint8_t *hdr, uint32_t ihl)
{
    /* RFC 1071: the ones' complement sum of a correct header is 0xffff. */
    uint32_t sum = 0;
    for (uint32_t i = 0; i + 1 < ihl; i += 2)
        sum += be16(hdr + i);
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    if (sum != 0xffff)
        return NETPARSE_BAD_CKSUM;
    if (be16(hdr + 6) & 0x3fff)   /* MF or an offset */
        return NETPARSE_FRAGMENT;
    return NETPARSE_OK;
}

bool netparse_ipv4_quote(const uint8_t *q, uint32_t *ihl)
{
    uint32_t hl = (uint32_t)(q[0] & 0xf) * 4u;
    if ((q[0] >> 4) != 4 || hl < 20)
        return false;
    *ihl = hl;
    return true;
}

uint32_t netparse_ipv6(const uint8_t *hdr, uint32_t pkt_len, uint32_t *plen)
{
    uint32_t len = be16(hdr + 4);
    if ((hdr[0] >> 4) != 6 || len + 40u > pkt_len)
        return NETPARSE_BAD;
    *plen = len;
    return NETPARSE_OK;
}

void netparse_nd(const uint8_t *msg, uint8_t opt_type, struct netparse_nd *out)
{
    memcpy(out->target, msg + 8, 16);
    out->has_mac = msg[24] == opt_type && msg[25] == 1;
    if (out->has_mac)
        memcpy(out->mac, msg + 26, 6);
    else
        memset(out->mac, 0, 6);
}

uint32_t netparse_udp(const uint8_t *hdr, uint32_t pkt_len, struct netparse_udp *out)
{
    uint32_t len = be16(hdr + 4);
    if (len < 8 || len > pkt_len)
        return NETPARSE_BAD;
    out->len = (uint16_t)len;
    out->sport = be16(hdr);
    out->dport = be16(hdr + 2);
    out->has_cksum = hdr[6] != 0 || hdr[7] != 0;
    return NETPARSE_OK;
}

void netparse_l4(const uint8_t *b, uint32_t avail, struct netparse_l4 *out)
{
    out->sport = be16(b);
    out->dport = be16(b + 2);
    out->seq = be32(b + 4);
    out->icmp_type = b[0];
    out->icmp_id = be16(b + 4);
    out->tcp_flags = avail >= 14 ? b[13] : 0;
}
