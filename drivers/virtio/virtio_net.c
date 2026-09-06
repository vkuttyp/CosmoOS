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

struct vnet {
    struct virtio_device *vdev;
    struct virtqueue *rx, *tx;
    struct netif nif;
    spinlock_t lock;
    unsigned rx_posted;
    bool tx_csum, rx_csum;
    uint64_t rx_drops, tx_drops, rx_csum_valid, rx_csum_finished, tx_csum_offloaded;
};

static void vnet_post_rx(struct vnet *v)
{
    while (v->rx_posted < VNET_RX_BUFS && virtq_free_count(v->rx) > 0) {
        struct mbuf *m = m_getcl();
        if (m == NULL)
            break;
        m->data = m->buf;   /* header + frame fill the whole cluster */
        dma_addr_t dma = dma_map(&v->vdev->dev, m->data, MCLBYTES, DMA_FROM_DEVICE);
        if (dma == 0) {
            m_freem(m);
            break;
        }
        m->pkt.dma = dma;
        struct virtq_sg sg = { .addr = dma, .len = MCLBYTES };
        if (virtq_add(v->rx, &sg, 0, 1, m) != 0) {
            dma_unmap(&v->vdev->dev, dma, MCLBYTES, DMA_FROM_DEVICE);
            m_freem(m);
            break;
        }
        v->rx_posted++;
    }
    virtq_kick(v->rx);
}

static void vnet_rx_done(struct virtqueue *vq)
{
    struct vnet *v = vq->vdev->priv;
    uint32_t len;
    struct mbuf *m;
    while ((m = virtq_pop(vq, &len)) != NULL) {
        v->rx_posted--;
        dma_unmap(&v->vdev->dev, m->pkt.dma, MCLBYTES, DMA_FROM_DEVICE);
        m->pkt.dma = 0;
        if (len < VNET_HDR_LEN + 14 || len > MCLBYTES) {
            v->rx_drops++;
            m_freem(m);
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
                m_freem(m);
                continue;
            }
        } else if (v->rx_csum && (hdr.flags & VNET_HDR_F_DATA_VALID)) {
            m->flags |= M_CSUM_OK;
            v->rx_csum_valid++;
        }
        netif_rx(&v->nif, m);
    }
    vnet_post_rx(v);
}

/* Every buffer of a transmitted chain carries its own mapping in pkt.dma. */
static void tx_unmap(struct vnet *v, struct mbuf *m)
{
    for (struct mbuf *b = m; b; b = b->next) {
        if (b->pkt.dma) {
            dma_unmap(&v->vdev->dev, b->pkt.dma, b->len, DMA_TO_DEVICE);
            b->pkt.dma = 0;
        }
    }
}

static void vnet_tx_done(struct virtqueue *vq)
{
    struct vnet *v = vq->vdev->priv;
    uint32_t len;
    struct mbuf *m;
    while ((m = virtq_pop(vq, &len)) != NULL) {
        tx_unmap(v, m);
        m_freem(m);
    }
}

static int vnet_transmit(struct netif *nif, struct mbuf *m)
{
    struct vnet *v = nif->priv;
    unsigned nbufs = 0;
    for (struct mbuf *b = m; b; b = b->next)
        nbufs++;
    if (nbufs > VNET_MAX_SEGS) {
        struct mbuf *lin = m_copypacket(m);
        m_freem(m);
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
            m_freem(m);
            return -EINVAL;
        }
        b->pkt.dma = dma;
        sg[n].addr = dma;
        sg[n].len = b->len;
        n++;
    }
    int rc = virtq_add(v->tx, sg, n, 0, m);
    if (rc) {
        v->tx_drops++;
        tx_unmap(v, m);
        m_freem(m);
        return -ENOBUFS;
    }
    virtq_kick(v->tx);
    return 0;
}

static void vnet_release(struct netif *nif);

static const struct netif_ops vnet_ops = { .transmit = vnet_transmit, .release = vnet_release };

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
    virtio_device_reset(vdev);
    if (v->rx)
        virtq_free(v->rx);
    if (v->tx)
        virtq_free(v->tx);
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
    netif_unregister(&v->nif);   /* no transmit or receive touches the queues after this */
    virtio_device_reset(vdev);
    /* Everything the device held is dropped; free the posted buffers. */
    struct mbuf *m;
    uint32_t len;
    while ((m = virtq_pop(v->rx, &len)) != NULL)
        m_freem(m);
    while ((m = virtq_pop(v->tx, &len)) != NULL)
        m_freem(m);
    virtq_free(v->rx);
    virtq_free(v->tx);
    v->rx = v->tx = NULL;
    vdev->priv = NULL;
    netif_put(&v->nif);          /* the creator's reference; vnet_release frees v when holders are gone */
}

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
