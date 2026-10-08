/*
 * virtio_net.c - virtio network device (VirtIO 1.1 section 5.1) as a
 * netif. Module `virtio_net`, depends on `virtio`.
 *
 * No mergeable buffers are negotiated, so every received frame is one
 * posted cluster with a 12-byte virtio_net_hdr in front; transmit
 * prepends the same header and maps the mbuf chain (at most four
 * buffers, longer chains are linearised). Checksum offload (unit 11):
 * with CSUM the header asks the device to finish a transport checksum
 * the stack left in its partial form; with GUEST_CSUM a received frame
 * marked DATA_VALID is trusted and one marked NEEDS_CSUM is finished in
 * software. One queue pair: QEMU's user-mode backend offers no MQ
 * (docs/kernel-services/network/design.md, "virtio-net offloads").
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/mbuf.h>
#include <kernel/module.h>
#include <kernel/panic.h>
#include <kernel/net/cksum.h>
#include <kernel/netif.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>

#include <drivers/virtio.h>

#define VIRTIO_NET_F_CSUM       (1ULL << 0)
#define VIRTIO_NET_F_GUEST_CSUM (1ULL << 1)
#define VIRTIO_NET_F_MAC        (1ULL << 5)
#define VIRTIO_NET_F_STATUS     (1ULL << 16)
#define VNET_HDR_LEN            12u
#define VNET_HDR_F_NEEDS_CSUM   1u
#define VNET_HDR_F_DATA_VALID   2u

struct vnet_hdr {
    uint8_t flags;
    uint8_t gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;    /* from the start of the frame (after this header) */
    uint16_t csum_offset;
    uint16_t num_buffers;   /* VERSION_1: present without MRG_RXBUF too */
} __packed;
_Static_assert(sizeof(struct vnet_hdr) == VNET_HDR_LEN, "virtio_net_hdr is 12 bytes");
#define VNET_RX_BUFS        32u
#define VNET_MAX_SEGS       4u

struct vnet_tx {
    struct mbuf *m;
    struct vnet_tx *next;
};

struct vnet {
    struct virtio_device *vdev;
    struct virtqueue *rx, *tx;
    struct netif nif;
    spinlock_t lock;
    unsigned rx_posted;
    bool stopping;
    /* Queue cookies disappear in virtq_free. Keep buffer ownership here
     * until a completion or the stopped-device cleanup claims it. */
    struct mbuf *rx_buf[VNET_RX_BUFS];
    struct vnet_tx tx_buf[VIRTQ_MAX_SIZE], *tx_free;
#if CONFIG_DEBUG && CONFIG_SELFTEST
    struct vnet_test_state *test;   /* immutable while this device's callbacks exist */
#endif
    bool tx_csum, rx_csum;
    uint64_t rx_drops, tx_drops, rx_csum_valid, rx_csum_finished, tx_csum_offloaded;
};

/* A seam armed only for the synthetic transport in the removal tests.
 * No lock or atomic RMW is paid by an unarmed callback. */
#if CONFIG_DEBUG && CONFIG_SELFTEST
struct vnet_test_record {
    struct mbuf *m;
    dma_addr_t dma;
    unsigned len;
    enum dma_dir dir;
    bool mapped;
};
struct vnet_test_state {
    struct vnet *v;
    spinlock_t irq_lock;
    bool hold, reset, late;
    unsigned records, maps, unmaps, freed, parked[2], callbacks_after_reset, posts_after_reset;
    struct vnet_test_record record[256];
};
static struct vnet_test_state *vnet_test_of(struct vnet *v)
{
    return v->test;
}

static void vnet_note_map(struct vnet *v, struct mbuf *m, unsigned len, enum dma_dir dir)
{
    struct vnet_test_state *t = vnet_test_of(v);
    if (t == NULL)
        return;
    KASSERT(t->records < ARRAY_SIZE(t->record));
    t->record[t->records++] = (struct vnet_test_record){ m, m->pkt.dma, len, dir, true };
    t->maps++;
}

static void vnet_note_unmap(struct vnet *v, dma_addr_t dma)
{
    struct vnet_test_state *t = vnet_test_of(v);
    if (t == NULL)
        return;
    for (unsigned i = 0; i < t->records; i++) {
        if (t->record[i].mapped && t->record[i].dma == dma) {
            t->record[i].mapped = false;
            t->unmaps++;
            return;
        }
    }
    panic("vnet test: mapping unmapped twice");
}

static void vnet_note_free(struct vnet *v, struct mbuf *m)
{
    struct vnet_test_state *t = vnet_test_of(v);
    if (t == NULL)
        return;
    for (struct mbuf *b = m; b; b = b->next) {
        for (unsigned i = 0; i < t->records; i++) {
            if (t->record[i].m == b) {
                t->record[i].m = NULL;
                t->freed++;
                break;
            }
        }
    }
}

static bool vnet_test_park(struct vnet *v, unsigned queue)
{
    struct vnet_test_state *t = vnet_test_of(v);
    if (t == NULL)
        return false;
    if (t->reset)
        t->callbacks_after_reset++;
    if (!t->hold)
        return false;
    t->parked[queue]++;
    return true;
}
#else
static inline void vnet_note_map(struct vnet *v, struct mbuf *m, unsigned len, enum dma_dir dir)
{ (void)v; (void)m; (void)len; (void)dir; }
static inline void vnet_note_unmap(struct vnet *v, dma_addr_t dma) { (void)v; (void)dma; }
static inline void vnet_note_free(struct vnet *v, struct mbuf *m) { (void)v; (void)m; }
static inline bool vnet_test_park(struct vnet *v, unsigned queue) { (void)v; (void)queue; return false; }
#endif

static void vnet_free_mbuf(struct vnet *v, struct mbuf *m)
{
    vnet_note_free(v, m);
    m_freem(m);
}

static void vnet_unmap(struct vnet *v, dma_addr_t dma, unsigned len, enum dma_dir dir)
{
    vnet_note_unmap(v, dma);
    dma_unmap(&v->vdev->dev, dma, len, dir);
}

static unsigned vnet_buffer_slot(struct mbuf **buffers, unsigned count, struct mbuf *m)
{
    for (unsigned i = 0; i < count; i++)
        if (buffers[i] == m)
            return i;
    return count;
}

/* Caller holds v->lock. A cookie has exactly one owner until claimed. */
static void vnet_buffer_take(struct mbuf **buffers, unsigned count, struct mbuf *m)
{
    unsigned slot = vnet_buffer_slot(buffers, count, m);
    KASSERT(slot < count);
    buffers[slot] = NULL;
}

/* Initialize before publication. Queue cookies point directly at these
 * private records; no descriptor API or mbuf layout change is needed. */
static void vnet_tx_init(struct vnet *v)
{
    for (unsigned i = 0; i < v->tx->size; i++) {
        v->tx_buf[i].next = v->tx_free;
        v->tx_free = &v->tx_buf[i];
    }
}

/* Caller holds v->lock. Clear ownership before making the record reusable. */
static void vnet_tx_put(struct vnet *v, struct vnet_tx *tx)
{
    KASSERT(tx->m != NULL);
    tx->m = NULL;
    tx->next = v->tx_free;
    v->tx_free = tx;
}

static void vnet_post_rx(struct vnet *v)
{
    arch_irq_state_t s = spin_lock_irqsave(&v->lock);
    while (!v->stopping && v->rx_posted < VNET_RX_BUFS && virtq_free_count(v->rx) > 0) {
        struct mbuf *m = m_getcl();
        if (m == NULL)
            break;
        m->data = m->buf;   /* header + frame fill the whole cluster */
        dma_addr_t dma = dma_map(&v->vdev->dev, m->data, MCLBYTES, DMA_FROM_DEVICE);
        if (dma == 0) {
            vnet_free_mbuf(v, m);
            break;
        }
        m->pkt.dma = dma;
        vnet_note_map(v, m, MCLBYTES, DMA_FROM_DEVICE);
        unsigned slot = vnet_buffer_slot(v->rx_buf, ARRAY_SIZE(v->rx_buf), NULL);
        KASSERT(slot < ARRAY_SIZE(v->rx_buf));
        v->rx_buf[slot] = m;
        struct virtq_sg sg = { .addr = dma, .len = MCLBYTES };
        if (virtq_add(v->rx, &sg, 0, 1, m) != 0) {
            v->rx_buf[slot] = NULL;
            vnet_unmap(v, dma, MCLBYTES, DMA_FROM_DEVICE);
            vnet_free_mbuf(v, m);
            break;
        }
        v->rx_posted++;
    }
    if (!v->stopping)
        virtq_kick(v->rx);
    spin_unlock_irqrestore(&v->lock, s);
}

/* Bounded twice: by the budget, and by the buffers posted, which are
 * reposted only after the loop (VNET_RX_BUFS). */
static unsigned vnet_rx_done(struct virtqueue *vq, unsigned budget)
{
    struct vnet *v = vq->vdev->priv;
    if (vnet_test_park(v, 0))
        return 0;
    uint32_t len;
    struct mbuf *m;
    unsigned n = 0;
    for (; n < budget && (m = virtq_pop(vq, &len)) != NULL; n++) {
        arch_irq_state_t s = spin_lock_irqsave(&v->lock);
        vnet_buffer_take(v->rx_buf, ARRAY_SIZE(v->rx_buf), m);
        KASSERT(v->rx_posted > 0);
        v->rx_posted--;
        spin_unlock_irqrestore(&v->lock, s);
        vnet_unmap(v, m->pkt.dma, MCLBYTES, DMA_FROM_DEVICE);
        m->pkt.dma = 0;
        if (len < VNET_HDR_LEN + 14 || len > MCLBYTES) {
            v->rx_drops++;
            vnet_free_mbuf(v, m);
            continue;
        }
        struct vnet_hdr hdr;
        memcpy(&hdr, m->data, sizeof(hdr));
        m->len = m->pkt.len = len;
        m_adj(m, (int)VNET_HDR_LEN);
        if (v->rx_csum && (hdr.flags & VNET_HDR_F_NEEDS_CSUM)) {
            /* A partially checksummed frame (another guest's offload): finish it. */
            m->pkt.csum_start = hdr.csum_start;
            m->pkt.csum_offset = hdr.csum_offset;
            m->pkt.csum_flags = NET_CSUM_TCP;
            if (m_csum_complete(m)) {
                m->flags |= M_CSUM_OK;
                v->rx_csum_finished++;
            } else {
                v->rx_drops++;
                vnet_free_mbuf(v, m);
                continue;
            }
        } else if (v->rx_csum && (hdr.flags & VNET_HDR_F_DATA_VALID)) {
            m->flags |= M_CSUM_OK;
            v->rx_csum_valid++;
        }
        vnet_note_free(v, m);   /* the stack takes ownership, including GONE drops */
        netif_rx(&v->nif, m);
    }
    vnet_post_rx(v);
    return n;
}

/* Every buffer of a transmitted chain carries its own mapping in pkt.dma. */
static void tx_unmap(struct vnet *v, struct mbuf *m)
{
    for (struct mbuf *b = m; b; b = b->next) {
        if (b->pkt.dma) {
            vnet_unmap(v, b->pkt.dma, b->len, DMA_TO_DEVICE);
            b->pkt.dma = 0;
        }
    }
}

/* Bounded by the budget: transmits from other CPUs refill this ring
 * while it is drained. */
static unsigned vnet_tx_done(struct virtqueue *vq, unsigned budget)
{
    struct vnet *v = vq->vdev->priv;
    if (vnet_test_park(v, 1))
        return 0;
    uint32_t len;
    struct vnet_tx *tx;
    unsigned n = 0;
    for (; n < budget && (tx = virtq_pop(vq, &len)) != NULL; n++) {
        arch_irq_state_t s = spin_lock_irqsave(&v->lock);
        struct mbuf *m = tx->m;
        vnet_tx_put(v, tx);
        spin_unlock_irqrestore(&v->lock, s);
        tx_unmap(v, m);
        vnet_free_mbuf(v, m);
    }
    return n;
}

static int vnet_transmit(struct netif *nif, struct mbuf *m)
{
    struct vnet *v = nif->priv;
    unsigned nbufs = 0;
    for (struct mbuf *b = m; b; b = b->next)
        nbufs++;
    if (nbufs > VNET_MAX_SEGS) {
        struct mbuf *lin = m_copypacket(m);
        vnet_free_mbuf(v, m);
        if (lin == NULL)
            return -ENOMEM;
        m = lin;
    }
    m = m_prepend(m, VNET_HDR_LEN);
    if (m == NULL)
        return -ENOMEM;
    struct vnet_hdr hdr;
    memset(&hdr, 0, sizeof(hdr));   /* no GSO */
    if (v->tx_csum && (m->pkt.csum_flags & NET_CSUM_TX)) {
        /* csum_start counts from the frame's first byte, after this header. */
        hdr.flags = VNET_HDR_F_NEEDS_CSUM;
        hdr.csum_start = m->pkt.csum_start;
        hdr.csum_offset = m->pkt.csum_offset;
        v->tx_csum_offloaded++;
    }
    memcpy(m->data, &hdr, sizeof(hdr));

    struct virtq_sg sg[VNET_MAX_SEGS + 1];
    unsigned n = 0;
    for (struct mbuf *b = m; b; b = b->next) {
        b->pkt.dma = 0;
        if (b->len == 0)
            continue;
        dma_addr_t dma = n < ARRAY_SIZE(sg) ? dma_map(&v->vdev->dev, b->data, b->len, DMA_TO_DEVICE) : 0;
        if (dma == 0) {
            tx_unmap(v, m);
            vnet_free_mbuf(v, m);
            return -EINVAL;
        }
        b->pkt.dma = dma;
        vnet_note_map(v, b, b->len, DMA_TO_DEVICE);
        sg[n].addr = dma;
        sg[n].len = b->len;
        n++;
    }
    arch_irq_state_t s = spin_lock_irqsave(&v->lock);
    int rc = -ENOBUFS;
    struct vnet_tx *tx = v->tx_free;
    if (!v->stopping && tx != NULL) {
        v->tx_free = tx->next;
        KASSERT(tx->m == NULL);
        tx->m = m;
        rc = virtq_add(v->tx, sg, n, 0, tx);
        if (rc)
            vnet_tx_put(v, tx);
        else
            virtq_kick(v->tx);
    }
    spin_unlock_irqrestore(&v->lock, s);
    if (rc) {
        v->tx_drops++;
        tx_unmap(v, m);
        vnet_free_mbuf(v, m);
        return -ENOBUFS;
    }
    return 0;
}

/* No submitters may remain: remove unregisters the interface first;
 * probe failure has never published it. Poll disable waits for callbacks
 * and prevents RX refill at the reset acknowledgement. virtq_free then
 * synchronizes the transport vectors before private buffer reclamation. */
static void vnet_cleanup(struct vnet *v)
{
    arch_irq_state_t s = spin_lock_irqsave(&v->lock);
    v->stopping = true;
    spin_unlock_irqrestore(&v->lock, s);
    if (v->rx)
        irq_poll_disable(&v->rx->poll);
    if (v->tx)
        irq_poll_disable(&v->tx->poll);
    virtio_device_reset(v->vdev);
    if (v->rx)
        virtq_free(v->rx);
    if (v->tx)
        virtq_free(v->tx);
    v->rx = v->tx = NULL;
    /* irq_poll_disable joins running callbacks and prevents new ones;
     * virtq_free has also synchronized the vectors. Submitters were
     * excluded before entry, so these ownership records are now private. */
    for (unsigned i = 0; i < ARRAY_SIZE(v->rx_buf); i++) {
        struct mbuf *m = v->rx_buf[i];
        if (m == NULL)
            continue;
        v->rx_buf[i] = NULL;
        vnet_unmap(v, m->pkt.dma, MCLBYTES, DMA_FROM_DEVICE);
        m->pkt.dma = 0;
        KASSERT(v->rx_posted > 0);
        v->rx_posted--;
        vnet_free_mbuf(v, m);
    }
    KASSERT(v->rx_posted == 0);
    for (unsigned i = 0; i < ARRAY_SIZE(v->tx_buf); i++) {
        struct mbuf *m = v->tx_buf[i].m;
        if (m == NULL)
            continue;
        v->tx_buf[i].m = NULL;
        tx_unmap(v, m);
        vnet_free_mbuf(v, m);
    }
}

static void vnet_release(struct netif *nif);

/* Descriptors the device holds: the ring's size less the free list. A
 * frame is one or two of them (the header is prepended into the first
 * buffer; a chain of more mbufs is linearised above VNET_MAX_SEGS). */
static unsigned vnet_tx_pending(struct netif *nif, unsigned *capacity)
{
    struct vnet *v = nif->priv;
    *capacity = v->tx->size;
    return v->tx->size - virtq_free_count(v->tx);
}

static const struct netif_ops vnet_ops = { .transmit = vnet_transmit, .release = vnet_release,
                                           .tx_pending = vnet_tx_pending };

static int vnet_probe(struct virtio_device *vdev)
{
    struct vnet *v = kzalloc(sizeof(*v));
    if (v == NULL)
        return -ENOMEM;
    v->vdev = vdev;
    vdev->priv = v;
    spinlock_init(&v->lock, "virtio-net");

    int rc = virtio_device_init(vdev, VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS | VIRTIO_NET_F_CSUM |
                                          VIRTIO_NET_F_GUEST_CSUM);
    if (rc)
        goto fail;
    v->tx_csum = virtio_has_feature(vdev, VIRTIO_NET_F_CSUM);
    v->rx_csum = virtio_has_feature(vdev, VIRTIO_NET_F_GUEST_CSUM);
    if (!virtio_has_feature(vdev, VIRTIO_NET_F_MAC)) {
        kerror("virtio-net: %s: device offers no MAC address", vdev->dev.name);
        rc = -ENODEV;
        goto fail;
    }
    virtio_read_config(vdev, 0, v->nif.mac, 6);
    rc = virtq_alloc(vdev, 0, 0, vnet_rx_done, &v->rx);
    if (rc)
        goto fail;
    rc = virtq_alloc(vdev, 1, 0, vnet_tx_done, &v->tx);
    if (rc)
        goto fail;
    vnet_tx_init(v);
    virtio_device_ready(vdev);
    vnet_post_rx(v);

    strlcpy(v->nif.name, "eth0", sizeof(v->nif.name));
    v->nif.mtu = 1500;
    v->nif.ops = &vnet_ops;
    v->nif.priv = v;
    v->nif.flags = 0;
    v->nif.caps = (v->tx_csum ? NETIF_CAP_TXCSUM : 0) | (v->rx_csum ? NETIF_CAP_RXCSUM : 0);
    rc = netif_register(&v->nif);
    if (rc)
        goto fail;
    netif_set_up(&v->nif, true);
    kinfo("virtio-net: %s is %s (checksum offload: tx %s, rx %s)", vdev->dev.name, v->nif.name,
          v->tx_csum ? "on" : "off", v->rx_csum ? "on" : "off");
    return 0;

fail:
    vnet_cleanup(v);
    kfree(v);
    vdev->priv = NULL;
    return rc;
}

/* Last reference: a route lookup or a queued packet may hold the
 * interface past remove; the memory goes here (docs/kernel/quiesce/). */
static void vnet_release(struct netif *nif)
{
    kfree(nif->priv);
}

static void vnet_remove(struct virtio_device *vdev)
{
    struct vnet *v = vdev->priv;
    netif_unregister(&v->nif);   /* reject and drain stack submitters; callbacks still exist */
    vnet_cleanup(v);
    vdev->priv = NULL;
    netif_put(&v->nif);          /* the creator's reference; vnet_release frees v when holders are gone */
}

#if CONFIG_DEBUG && CONFIG_SELFTEST
#include "virtio_net_test.inc"
#endif

static const uint32_t vnet_ids[] = { VIRTIO_ID_NET, 0 };

static struct virtio_driver vnet_driver = {
    .drv = { .name = "virtio_net" },
    .ids = vnet_ids,
    .probe = vnet_probe,
    .remove = vnet_remove,
};

static int vnet_module_init(void)
{
    return virtio_register_driver(&vnet_driver);
}

static void vnet_module_shutdown(void)
{
    virtio_unregister_driver(&vnet_driver);
}

COSMO_MODULE("virtio_net", "1.0", vnet_module_init, vnet_module_shutdown, "virtio", MODULE_CAP_DRIVER);
