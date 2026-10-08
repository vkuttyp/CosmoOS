/*
 * netpkt.h - Frame builders and checkers for the network fuzz targets.
 *
 * Plain functions over a byte buffer: an Ethernet header, an IPv4 header
 * with its checksum, UDP and TCP with the pseudo-header checksum, ARP,
 * ICMP, IPv6 and ICMPv6 neighbour discovery. The targets use them for
 * their programmatic seeds, for the handshake that establishes a
 * connection before an input runs, and -- the checkers -- as the oracle
 * that every frame the stack transmits carries correct checksums.
 *
 * Include after the kernel headers (see shim_net.c on Darwin's htons).
 */
#ifndef COSMO_FUZZ_NETPKT_H
#define COSMO_FUZZ_NETPKT_H

#include <kernel/net/cksum.h>
#include <kernel/net/ether.h>
#include <kernel/net/inet.h>
#include <kernel/net/ip.h>
#include <kernel/net/tcp.h>
#include <kernel/net/udp.h>

#include <string.h>

#define NP_ETH   14u
#define NP_IPV4  20u
#define NP_IPV6  40u
#define NP_UDP   8u
#define NP_TCP   20u

static inline void np_put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void np_put32(uint8_t *p, uint32_t v) { np_put16(p, (uint16_t)(v >> 16)); np_put16(p + 2, (uint16_t)v); }
static inline uint16_t np_get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t np_get32(const uint8_t *p) { return ((uint32_t)np_get16(p) << 16) | np_get16(p + 2); }

/* Ethernet header at `f`; returns the payload offset. */
static inline size_t np_eth(uint8_t *f, const uint8_t dst[6], const uint8_t src[6], uint16_t type)
{
    memcpy(f, dst, 6);
    memcpy(f + 6, src, 6);
    np_put16(f + 12, type);
    return NP_ETH;
}

/* IPv4 header at `p` for `payload` bytes of `proto`; checksum filled. */
static inline size_t np_ipv4(uint8_t *p, uint32_t src, uint32_t dst, uint8_t proto, uint16_t payload, uint8_t ttl)
{
    memset(p, 0, NP_IPV4);
    p[0] = 0x45;
    np_put16(p + 2, (uint16_t)(NP_IPV4 + payload));
    np_put16(p + 4, 0x1234);
    np_put16(p + 6, 0x4000);   /* DF */
    p[8] = ttl;
    p[9] = proto;
    memcpy(p + 12, &src, 4);   /* network order in, network order out */
    memcpy(p + 16, &dst, 4);
    uint16_t c = in_cksum(p, NP_IPV4);
    memcpy(p + 10, &c, 2);
    return NP_IPV4;
}

/* The transport checksum over [l4, l4 + len) with the IPv4 pseudo-header,
 * written into the header at `at` (6 for UDP, 16 for TCP). */
static inline void np_l4_cksum4(uint8_t *l4, uint32_t len, uint32_t src, uint32_t dst, uint8_t proto, unsigned at)
{
    l4[at] = l4[at + 1] = 0;
    uint32_t sum = cksum_pseudo4(src, dst, proto, (uint16_t)len);
    uint16_t c = cksum_fold(cksum_partial(l4, len, sum));   /* the field's value: inverted, network order */
    if (c == 0 && proto == IPPROTO_UDP)
        c = 0xffff;
    memcpy(l4 + at, &c, 2);
}

static inline void np_l4_cksum6(uint8_t *l4, uint32_t len, const struct in6_addr *src, const struct in6_addr *dst,
                                uint8_t proto, unsigned at)
{
    l4[at] = l4[at + 1] = 0;
    uint32_t sum = cksum_pseudo6(src, dst, proto, len);
    uint16_t c = cksum_fold(cksum_partial(l4, len, sum));
    if (c == 0 && proto == IPPROTO_UDP)
        c = 0xffff;
    memcpy(l4 + at, &c, 2);
}

/* UDP header + payload at `p`; returns the UDP length. Checksum left 0
 * (the caller runs np_l4_cksum4 after the IP addresses are known). */
static inline size_t np_udp(uint8_t *p, uint16_t sport, uint16_t dport, const void *payload, size_t n)
{
    np_put16(p, sport);
    np_put16(p + 2, dport);
    np_put16(p + 4, (uint16_t)(NP_UDP + n));
    p[6] = p[7] = 0;
    if (n)
        memcpy(p + NP_UDP, payload, n);
    return NP_UDP + n;
}

/* TCP header (+ options, + payload) at `p`; returns the segment length. */
static inline size_t np_tcp(uint8_t *p, uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack, uint8_t flags,
                            uint16_t win, const void *opts, size_t optlen, const void *payload, size_t n)
{
    size_t hlen = NP_TCP + ((optlen + 3) & ~(size_t)3);
    memset(p, 0, hlen);
    np_put16(p, sport);
    np_put16(p + 2, dport);
    np_put32(p + 4, seq);
    np_put32(p + 8, ack);
    p[12] = (uint8_t)((hlen / 4) << 4);
    p[13] = flags;
    np_put16(p + 14, win);
    if (optlen)
        memcpy(p + NP_TCP, opts, optlen);
    if (n)
        memcpy(p + hlen, payload, n);
    return hlen + n;
}

/* A whole IPv4/UDP frame; returns its length. */
static inline size_t np_frame_udp4(uint8_t *f, const uint8_t dmac[6], const uint8_t smac[6], uint32_t src, uint32_t dst,
                                   uint16_t sport, uint16_t dport, const void *payload, size_t n)
{
    size_t o = np_eth(f, dmac, smac, ETH_P_IP);
    size_t ul = np_udp(f + o + NP_IPV4, sport, dport, payload, n);
    np_ipv4(f + o, src, dst, IPPROTO_UDP, (uint16_t)ul, 64);
    np_l4_cksum4(f + o + NP_IPV4, (uint32_t)ul, src, dst, IPPROTO_UDP, 6);
    return o + NP_IPV4 + ul;
}

/* A whole IPv4/TCP frame; returns its length. */
static inline size_t np_frame_tcp4(uint8_t *f, const uint8_t dmac[6], const uint8_t smac[6], uint32_t src, uint32_t dst,
                                   uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack, uint8_t flags,
                                   uint16_t win, const void *opts, size_t optlen, const void *payload, size_t n)
{
    size_t o = np_eth(f, dmac, smac, ETH_P_IP);
    size_t tl = np_tcp(f + o + NP_IPV4, sport, dport, seq, ack, flags, win, opts, optlen, payload, n);
    np_ipv4(f + o, src, dst, IPPROTO_TCP, (uint16_t)tl, 64);
    np_l4_cksum4(f + o + NP_IPV4, (uint32_t)tl, src, dst, IPPROTO_TCP, 16);
    return o + NP_IPV4 + tl;
}

/* A whole IPv4/ICMP frame (type, code, id, seq, payload). */
static inline size_t np_frame_icmp4(uint8_t *f, const uint8_t dmac[6], const uint8_t smac[6], uint32_t src, uint32_t dst,
                                    uint8_t type, uint8_t code, uint16_t id, uint16_t seq, const void *payload, size_t n)
{
    size_t o = np_eth(f, dmac, smac, ETH_P_IP);
    uint8_t *ic = f + o + NP_IPV4;
    ic[0] = type;
    ic[1] = code;
    ic[2] = ic[3] = 0;
    np_put16(ic + 4, id);
    np_put16(ic + 6, seq);
    if (n)
        memcpy(ic + 8, payload, n);
    uint16_t c = in_cksum(ic, 8 + n);
    memcpy(ic + 2, &c, 2);
    np_ipv4(f + o, src, dst, IPPROTO_ICMP, (uint16_t)(8 + n), 64);
    return o + NP_IPV4 + 8 + n;
}

/* An ARP request or reply (op 1 / 2). */
static inline size_t np_frame_arp(uint8_t *f, const uint8_t dmac[6], const uint8_t smac[6], uint16_t op,
                                  const uint8_t sha[6], uint32_t spa, const uint8_t tha[6], uint32_t tpa)
{
    size_t o = np_eth(f, dmac, smac, ETH_P_ARP);
    uint8_t *a = f + o;
    np_put16(a, 1);
    np_put16(a + 2, ETH_P_IP);
    a[4] = 6;
    a[5] = 4;
    np_put16(a + 6, op);
    memcpy(a + 8, sha, 6);
    memcpy(a + 14, &spa, 4);
    memcpy(a + 18, tha, 6);
    memcpy(a + 24, &tpa, 4);
    return o + 28;
}

/* IPv6 header at `p`. */
static inline size_t np_ipv6(uint8_t *p, const struct in6_addr *src, const struct in6_addr *dst, uint8_t nexthdr,
                             uint16_t payload, uint8_t hoplimit)
{
    memset(p, 0, NP_IPV6);
    p[0] = 0x60;
    np_put16(p + 4, payload);
    p[6] = nexthdr;
    p[7] = hoplimit;
    memcpy(p + 8, src->s6_addr, 16);
    memcpy(p + 24, dst->s6_addr, 16);
    return NP_IPV6;
}

/* An ICMPv6 neighbour solicitation / advertisement for `target` with the
 * link-layer option (type 1 source / 2 target) carrying `mac`. */
static inline size_t np_frame_nd(uint8_t *f, const uint8_t dmac[6], const uint8_t smac[6], const struct in6_addr *src,
                                 const struct in6_addr *dst, uint8_t type, const struct in6_addr *target,
                                 const uint8_t mac[6])
{
    size_t o = np_eth(f, dmac, smac, ETH_P_IPV6);
    uint8_t *ic = f + o + NP_IPV6;
    memset(ic, 0, 32);
    ic[0] = type;
    if (type == ICMPV6_NA)
        np_put32(ic + 4, 0x60000000u);
    memcpy(ic + 8, target->s6_addr, 16);
    ic[24] = type == ICMPV6_NS ? 1 : 2;
    ic[25] = 1;
    memcpy(ic + 26, mac, 6);
    np_ipv6(f + o, src, dst, IPPROTO_ICMPV6, 32, 255);
    np_l4_cksum6(ic, 32, src, dst, IPPROTO_ICMPV6, 2);
    return o + NP_IPV6 + 32;
}

/* A whole IPv6/UDP frame; returns its length. */
static inline size_t np_frame_udp6(uint8_t *f, const uint8_t dmac[6], const uint8_t smac[6], const struct in6_addr *src,
                                   const struct in6_addr *dst, uint16_t sport, uint16_t dport, const void *payload,
                                   size_t n)
{
    size_t o = np_eth(f, dmac, smac, ETH_P_IPV6);
    size_t ul = np_udp(f + o + NP_IPV6, sport, dport, payload, n);
    np_ipv6(f + o, src, dst, IPPROTO_UDP, (uint16_t)ul, 64);
    np_l4_cksum6(f + o + NP_IPV6, (uint32_t)ul, src, dst, IPPROTO_UDP, 6);
    return o + NP_IPV6 + ul;
}

/* A whole IPv6/TCP frame; returns its length. */
static inline size_t np_frame_tcp6(uint8_t *f, const uint8_t dmac[6], const uint8_t smac[6], const struct in6_addr *src,
                                   const struct in6_addr *dst, uint16_t sport, uint16_t dport, uint32_t seq,
                                   uint32_t ack, uint8_t flags, uint16_t win, const void *opts, size_t optlen,
                                   const void *payload, size_t n)
{
    size_t o = np_eth(f, dmac, smac, ETH_P_IPV6);
    size_t tl = np_tcp(f + o + NP_IPV6, sport, dport, seq, ack, flags, win, opts, optlen, payload, n);
    np_ipv6(f + o, src, dst, IPPROTO_TCP, (uint16_t)tl, 64);
    np_l4_cksum6(f + o + NP_IPV6, (uint32_t)tl, src, dst, IPPROTO_TCP, 16);
    return o + NP_IPV6 + tl;
}

/* An ICMPv6 echo request with `n` payload bytes. */
static inline size_t np_frame_echo6(uint8_t *f, const uint8_t dmac[6], const uint8_t smac[6], const struct in6_addr *src,
                                    const struct in6_addr *dst, const void *payload, size_t n)
{
    size_t o = np_eth(f, dmac, smac, ETH_P_IPV6);
    uint8_t *ic = f + o + NP_IPV6;
    memset(ic, 0, 8);
    ic[0] = ICMPV6_ECHO;
    np_put16(ic + 4, 0x4242);
    np_put16(ic + 6, 1);
    if (n)
        memcpy(ic + 8, payload, n);
    np_ipv6(f + o, src, dst, IPPROTO_ICMPV6, (uint16_t)(8 + n), 64);
    np_l4_cksum6(ic, (uint32_t)(8 + n), src, dst, IPPROTO_ICMPV6, 2);
    return o + NP_IPV6 + 8 + n;
}

/*
 * Recompute the checksums of a frame in place: the IPv4 header's, and the
 * transport's for UDP, TCP and ICMP over IPv4 (whole, unfragmented
 * datagrams only) and for UDP, TCP and ICMPv6 over IPv6. A frame that is
 * not one of these is left alone. Lets a mutated input reach past the
 * stack's checksum gates.
 */
static inline void np_fix_checksums(uint8_t *f, size_t len)
{
    if (len < NP_ETH)
        return;
    uint16_t type = np_get16(f + 12);
    if (type == ETH_P_IP) {
        if (len < NP_ETH + NP_IPV4)
            return;
        uint8_t *ip = f + NP_ETH;
        unsigned ihl = (unsigned)(ip[0] & 0xf) * 4u;
        if ((ip[0] >> 4) != 4 || ihl < NP_IPV4 || NP_ETH + ihl > len)
            return;
        ip[10] = ip[11] = 0;
        uint16_t c = in_cksum(ip, ihl);
        memcpy(ip + 10, &c, 2);
        uint16_t total = np_get16(ip + 2);
        if (total < ihl || NP_ETH + total > len || (np_get16(ip + 6) & 0x3fff))
            return;
        uint8_t *l4 = ip + ihl;
        uint32_t l4len = total - ihl;
        uint32_t src, dst;
        memcpy(&src, ip + 12, 4);
        memcpy(&dst, ip + 16, 4);
        if (ip[9] == IPPROTO_UDP && l4len >= NP_UDP) {
            uint16_t ul = np_get16(l4 + 4);
            if (ul >= NP_UDP && ul <= l4len)
                np_l4_cksum4(l4, ul, src, dst, IPPROTO_UDP, 6);
        } else if (ip[9] == IPPROTO_TCP && l4len >= NP_TCP) {
            np_l4_cksum4(l4, l4len, src, dst, IPPROTO_TCP, 16);
        } else if (ip[9] == IPPROTO_ICMP && l4len >= 8) {
            l4[2] = l4[3] = 0;
            uint16_t ic = in_cksum(l4, l4len);
            memcpy(l4 + 2, &ic, 2);
        }
    } else if (type == ETH_P_IPV6) {
        if (len < NP_ETH + NP_IPV6)
            return;
        uint8_t *ip = f + NP_ETH;
        if ((ip[0] >> 4) != 6)
            return;
        uint16_t plen = np_get16(ip + 4);
        if (NP_ETH + NP_IPV6 + plen > len)
            return;
        struct in6_addr src, dst;
        memcpy(src.s6_addr, ip + 8, 16);
        memcpy(dst.s6_addr, ip + 24, 16);
        uint8_t *l4 = ip + NP_IPV6;
        if (ip[6] == IPPROTO_UDP && plen >= NP_UDP)
            np_l4_cksum6(l4, plen, &src, &dst, IPPROTO_UDP, 6);
        else if (ip[6] == IPPROTO_TCP && plen >= NP_TCP)
            np_l4_cksum6(l4, plen, &src, &dst, IPPROTO_TCP, 16);
        else if (ip[6] == IPPROTO_ICMPV6 && plen >= 8)
            np_l4_cksum6(l4, plen, &src, &dst, IPPROTO_ICMPV6, 2);
    }
}

/*
 * The output oracle: does a frame the stack transmitted carry correct
 * checksums? 0 when it does (or is not a checksummed kind); otherwise a
 * small code naming the first wrong one: 1 the IPv4 header, 2 the IPv4
 * transport, 3 the IPv6 transport (a zero UDP checksum included), 4 a
 * malformed length.
 */
static inline int np_check_checksums(const uint8_t *f, size_t len)
{
    if (len < NP_ETH)
        return 0;
    uint16_t type = np_get16(f + 12);
    if (type == ETH_P_IP) {
        if (len < NP_ETH + NP_IPV4)
            return 4;
        const uint8_t *ip = f + NP_ETH;
        unsigned ihl = (unsigned)(ip[0] & 0xf) * 4u;
        if ((ip[0] >> 4) != 4 || ihl < NP_IPV4 || NP_ETH + ihl > len)
            return 4;
        if (in_cksum(ip, ihl) != 0)
            return 1;
        uint16_t total = np_get16(ip + 2);
        if (total < ihl || NP_ETH + total > len)
            return 4;
        if (np_get16(ip + 6) & 0x3fff)
            return 0;   /* a fragment: the stack sends none, but it is not a checksum fault */
        const uint8_t *l4 = ip + ihl;
        uint32_t l4len = total - ihl;
        uint32_t src, dst;
        memcpy(&src, ip + 12, 4);
        memcpy(&dst, ip + 16, 4);
        if (ip[9] == IPPROTO_UDP) {
            if (l4len < NP_UDP)
                return 4;
            uint16_t ul = np_get16(l4 + 4);
            if (ul < NP_UDP || ul > l4len)
                return 4;
            uint16_t have;
            memcpy(&have, l4 + 6, 2);
            if (have == 0)
                return 0;   /* UDP over IPv4 may leave it out */
            if (cksum_fold(cksum_partial(l4, ul, cksum_pseudo4(src, dst, IPPROTO_UDP, ul))) != 0)
                return 2;
        } else if (ip[9] == IPPROTO_TCP) {
            if (l4len < NP_TCP)
                return 4;
            if (cksum_fold(cksum_partial(l4, l4len, cksum_pseudo4(src, dst, IPPROTO_TCP, (uint16_t)l4len))) != 0)
                return 2;
        } else if (ip[9] == IPPROTO_ICMP) {
            if (l4len < 8)
                return 4;
            if (in_cksum(l4, l4len) != 0)
                return 2;
        }
        return 0;
    }
    if (type == ETH_P_IPV6) {
        if (len < NP_ETH + NP_IPV6)
            return 4;
        const uint8_t *ip = f + NP_ETH;
        if ((ip[0] >> 4) != 6)
            return 4;
        uint16_t plen = np_get16(ip + 4);
        if (NP_ETH + NP_IPV6 + plen > len)
            return 4;
        struct in6_addr src, dst;
        memcpy(src.s6_addr, ip + 8, 16);
        memcpy(dst.s6_addr, ip + 24, 16);
        const uint8_t *l4 = ip + NP_IPV6;
        uint8_t nh = ip[6];
        if (nh == IPPROTO_UDP || nh == IPPROTO_TCP || nh == IPPROTO_ICMPV6) {
            uint32_t min = nh == IPPROTO_UDP ? NP_UDP : nh == IPPROTO_TCP ? NP_TCP : 8u;
            if (plen < min)
                return 4;
            if (nh == IPPROTO_UDP && l4[6] == 0 && l4[7] == 0)
                return 3;   /* UDP over IPv6 may not leave it out (RFC 8200 8.1) */
            if (cksum_fold(cksum_partial(l4, plen, cksum_pseudo6(&src, &dst, nh, plen))) != 0)
                return 3;
        }
        return 0;
    }
    return 0;
}

#endif /* COSMO_FUZZ_NETPKT_H */
