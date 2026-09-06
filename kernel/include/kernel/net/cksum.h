/*
 * cksum.h - The Internet checksum (RFC 1071) over mbuf chains and buffers.
 */

#ifndef KERNEL_NET_CKSUM_H
#define KERNEL_NET_CKSUM_H

#include <kernel/mbuf.h>
#include <kernel/types.h>

/* Running 32-bit sum over bytes; fold with cksum_fold. */
uint32_t cksum_partial(const void *data, size_t len, uint32_t sum);
uint16_t cksum_fold(uint32_t sum);
/* Checksum of a buffer (returns the one's complement, ready to store). */
uint16_t in_cksum(const void *data, size_t len);
/* Over `len` bytes of the chain starting at `off`, with an initial sum. */
uint32_t m_cksum_partial(const struct mbuf *m, uint32_t off, uint32_t len, uint32_t sum);
/* The partial form a transport leaves for an offloading interface: the
 * folded, not inverted, pseudo-header sum. */
static inline uint16_t cksum_partial_field(uint32_t pseudo) { return (uint16_t)~cksum_fold(pseudo); }
/* Finish a packet's requested transport checksum in software (unit 11):
 * the ones' complement sum over [pkt.csum_start, end), including the
 * field holding the partial pseudo-header sum, written at csum_start +
 * csum_offset; clears pkt.csum_flags. The first buffer must hold the
 * field. Returns false (packet untouched) when the offsets are out of range. */
bool m_csum_complete(struct mbuf *m);
/* Pseudo-header sums for the transport checksums. */
uint32_t cksum_pseudo4(uint32_t src, uint32_t dst, uint8_t proto, uint16_t len);
uint32_t cksum_pseudo6(const struct in6_addr *src, const struct in6_addr *dst, uint8_t proto, uint32_t len);

#endif /* KERNEL_NET_CKSUM_H */
