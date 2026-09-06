/*
 * mbuf.h - Packet buffers (constitution section 34).
 *
 * A pointer carries one reference. m_free drops one reference on one
 * buffer and returns the next; m_freem frees a chain. Functions that
 * return an mbuf (m_prepend, m_pullup) may return a different pointer
 * and consume their argument on failure: write m = m_prepend(m, n).
 * Handing a packet to netif_transmit or netif_rx transfers ownership.
 * See docs/kernel-services/network/design.md.
 */

#ifndef KERNEL_MBUF_H
#define KERNEL_MBUF_H

#include <kernel/spinlock.h>
#include <kernel/types.h>

#include <kernel/net/inet.h>

#define MCLBYTES     2048u
#define MHLEN        128u
#define NET_HEADROOM 128u   /* fits vnet(12) + eth(14) + IPv6(40) + TCP with options(60) (unit 11) */

#define M_PKTHDR   (1u << 0)   /* first buffer of a packet: pkt valid */
#define M_EXT      (1u << 1)   /* storage is a shared cluster */
#define M_BCAST    (1u << 2)   /* received as link-layer broadcast */
#define M_MCAST    (1u << 3)
#define M_CSUM_OK  (1u << 4)   /* transport checksum verified by lower layer */

/* pkt.csum_flags on transmit (unit 11): the transport left the pseudo-header
 * sum (folded, not inverted) in its checksum field and asks the interface to
 * finish the sum over [csum_start, end) into csum_start + csum_offset; an
 * interface without NETIF_CAP_TXCSUM has netif_transmit finish it in software. */
#define NET_CSUM_TCP (1u << 0)
#define NET_CSUM_UDP (1u << 1)
#define NET_CSUM_TX  (NET_CSUM_TCP | NET_CSUM_UDP)

struct netif;

struct mbuf {
    struct mbuf *next;
    struct mbuf *nextpkt;
    uint8_t *data;
    uint32_t len;
    uint32_t flags;
    uint32_t refcount;         /* atomic; the pointer holder's references */
    uint8_t *buf;
    uint32_t size;
    struct {
        uint32_t len;
        struct netif *rcvif;
        uint64_t rx_ns;
        uint16_t proto;
        uint16_t csum_flags;   /* NET_CSUM_* (transmit) */
        uint16_t csum_start;   /* transmit offload: the transport header's offset from data (layers add theirs) */
        uint16_t csum_offset;  /* the checksum field's offset within the transport header */
        uint32_t flow_hash;    /* set by netif_rx: the receive-steering hash (unit 11) */
        struct netaddr src;    /* UDP: sender, set by udp_input */
        uint64_t dma;          /* a driver's dma_map of this buffer while the device holds it (unmapped on return) */
    } pkt;
    struct mbuf_cluster *cl;   /* M_EXT */
    uint8_t inl[MHLEN];
};

struct mbuf_cluster {
    uint32_t refcount;
    uint8_t data[MCLBYTES];
};

void mbuf_init(void);

/* A small mbuf with inline storage; data at the start. NULL if no memory. */
struct mbuf *m_get(void);
/* A packet-header mbuf backed by a cluster, data NET_HEADROOM bytes in. */
struct mbuf *m_getcl(void);
/* Drop one reference on one buffer; returns m->next. */
struct mbuf *m_free(struct mbuf *m);
/* Free a chain. NULL is fine. */
void m_freem(struct mbuf *m);
/* Share a cluster: a new mbuf referencing the same storage and range. */
struct mbuf *m_ref(struct mbuf *m);

/* Bytes available before data / after data + len in this buffer. */
static inline uint32_t m_leadingspace(const struct mbuf *m) { return (uint32_t)(m->data - m->buf); }
static inline uint32_t m_trailingspace(const struct mbuf *m) { return m->size - m_leadingspace(m) - m->len; }

/* Make room for n bytes before data (in place when headroom allows,
 * else a new leading buffer). Adjusts pkt.len. */
struct mbuf *m_prepend(struct mbuf *m, uint32_t n);
/* Guarantee the first n bytes are contiguous in the first buffer. */
struct mbuf *m_pullup(struct mbuf *m, uint32_t n);
/* Trim n bytes from the front (n > 0) or the back (n < 0) of the chain. */
void m_adj(struct mbuf *m, int n);
/* Copy `len` bytes at `off` out of the chain; false if short. */
bool m_copydata(const struct mbuf *m, uint32_t off, uint32_t len, void *dst);
/* Append bytes to the end of the chain (growing the last buffer or
 * adding clusters). 0 or -ENOMEM. */
int m_append(struct mbuf *m, const void *src, uint32_t len);
/* Total bytes in the chain, recomputed. */
uint32_t m_length(const struct mbuf *m);
/* Copy a whole chain into a fresh cluster-backed packet: linear when it
 * fits one cluster behind the headroom, else a chain of clusters. */
struct mbuf *m_copypacket(const struct mbuf *m);

struct mbufq {
    struct mbuf *head, *tail;
    unsigned len, maxlen;
    spinlock_t lock;
};
void mbufq_init(struct mbufq *q, unsigned maxlen, const char *name);
/* Takes the packet; returns false (and frees it) when full. Any context. */
bool mbufq_enqueue(struct mbufq *q, struct mbuf *m);
struct mbuf *mbufq_dequeue(struct mbufq *q);
void mbufq_drain(struct mbufq *q);
unsigned mbufq_len(struct mbufq *q);

struct mbuf_stats {
    uint64_t mbufs_alive, clusters_alive, allocs, frees, alloc_failures;
};
void mbuf_get_stats(struct mbuf_stats *out);

#endif /* KERNEL_MBUF_H */
