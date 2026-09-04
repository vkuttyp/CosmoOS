/*
 * virtio_console.c - virtio console (VirtIO 1.1 section 5.3) as a kernel
 * console sink. Module `virtio_console`, depends on `virtio`.
 *
 * Port 0 only (MULTIPORT is not negotiated): queue 0 receives, queue 1
 * transmits. Transmit is polled so the sink can run in any context the
 * console runs in, including a panic with interrupts off: text is copied
 * into a DMA bounce buffer, queued, and the used ring is polled with a
 * bounded spin. Nothing reads the receive queue yet; one buffer is
 * posted so the device has somewhere to put input.
 */

#include <kernel/console.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/module.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/timer.h>

#include <drivers/virtio.h>

#define VCON_TX_BUF   2048u
#define VCON_RX_BUF   256u
#define VCON_SPIN_NS  (200ull * 1000 * 1000)

struct vcon {
    struct virtio_device *vdev;
    struct virtqueue *rx, *tx;
    uint8_t *txbuf;
    dma_addr_t txbuf_dma;
    uint8_t *rxbuf;
    dma_addr_t rxbuf_dma;
    struct console_sink sink;
    spinlock_t lock;
    uint64_t bytes, drops;
    bool dead;
};

static struct vcon *g_vcon;   /* the console sink is a singleton */

/* Send one chunk and wait for the device to consume it. Lock held. */
static bool vcon_send(struct vcon *c, size_t len)
{
    struct virtq_sg sg = { .addr = c->txbuf_dma, .len = (uint32_t)len };
    static int cookie;
    if (virtq_add(c->tx, &sg, 1, 0, &cookie) != 0)
        return false;
    virtq_kick(c->tx);
    uint64_t deadline = clock_now_ns() + VCON_SPIN_NS;
    for (;;) {
        uint32_t used;
        if (virtq_pop(c->tx, &used) != NULL)
            return true;
        if (clock_now_ns() > deadline)
            return false;
    }
}

static void vcon_write(struct console_sink *sink, const char *s, size_t len)
{
    struct vcon *c = container_of(sink, struct vcon, sink);
    if (c->dead)
        return;
    arch_irq_state_t st = spin_lock_irqsave(&c->lock);
    while (len > 0 && !c->dead) {
        size_t n = len < VCON_TX_BUF ? len : VCON_TX_BUF;
        memcpy(c->txbuf, s, n);
        if (!vcon_send(c, n)) {
            /* The device stopped consuming; do not wedge the console. */
            c->drops++;
            c->dead = true;
            break;
        }
        c->bytes += n;
        s += n;
        len -= n;
    }
    spin_unlock_irqrestore(&c->lock, st);
}

static int vcon_probe(struct virtio_device *vdev)
{
    if (g_vcon != NULL)
        return -EBUSY;   /* one console sink */
    struct vcon *c = kzalloc(sizeof(*c));
    if (c == NULL)
        return -ENOMEM;
    c->vdev = vdev;
    vdev->priv = c;
    spinlock_init(&c->lock, "virtio-console");

    int rc = virtio_device_init(vdev, 0);
    if (rc)
        goto fail;
    c->txbuf = dma_alloc(&vdev->dev, VCON_TX_BUF, &c->txbuf_dma, DMA_ZERO);
    c->rxbuf = dma_alloc(&vdev->dev, VCON_RX_BUF, &c->rxbuf_dma, DMA_ZERO);
    if (c->txbuf == NULL || c->rxbuf == NULL) {
        rc = -ENOMEM;
        goto fail;
    }
    rc = virtq_alloc(vdev, 0, 0, NULL, &c->rx);
    if (rc)
        goto fail;
    rc = virtq_alloc(vdev, 1, 0, NULL, &c->tx);
    if (rc)
        goto fail;
    virtio_device_ready(vdev);

    struct virtq_sg rsg = { .addr = c->rxbuf_dma, .len = VCON_RX_BUF };
    static int rx_cookie;
    if (virtq_add(c->rx, &rsg, 0, 1, &rx_cookie) == 0)
        virtq_kick(c->rx);

    c->sink.name = "virtio-console";
    c->sink.write = vcon_write;
    g_vcon = c;
    console_register(&c->sink);
    kinfo("virtio-console: %s: registered as a console sink", vdev->dev.name);
    return 0;

fail:
    virtio_device_reset(vdev);
    if (c->rx)
        virtq_free(c->rx);
    if (c->tx)
        virtq_free(c->tx);
    if (c->txbuf)
        dma_free(&vdev->dev, VCON_TX_BUF, c->txbuf, c->txbuf_dma);
    if (c->rxbuf)
        dma_free(&vdev->dev, VCON_RX_BUF, c->rxbuf, c->rxbuf_dma);
    kfree(c);
    vdev->priv = NULL;
    return rc;
}

static void vcon_remove(struct virtio_device *vdev)
{
    struct vcon *c = vdev->priv;
    console_unregister(&c->sink);
    g_vcon = NULL;
    virtio_device_reset(vdev);
    virtq_free(c->rx);
    virtq_free(c->tx);
    dma_free(&vdev->dev, VCON_TX_BUF, c->txbuf, c->txbuf_dma);
    dma_free(&vdev->dev, VCON_RX_BUF, c->rxbuf, c->rxbuf_dma);
    kinfo("virtio-console: %s: removed after %llu bytes", vdev->dev.name, (unsigned long long)c->bytes);
    kfree(c);
    vdev->priv = NULL;
}

static const uint32_t vcon_ids[] = { VIRTIO_ID_CONSOLE, 0 };

static struct virtio_driver vcon_driver = {
    .drv = { .name = "virtio_console" },
    .ids = vcon_ids,
    .probe = vcon_probe,
    .remove = vcon_remove,
};

static int vcon_module_init(void)
{
    return virtio_register_driver(&vcon_driver);
}

static void vcon_module_shutdown(void)
{
    virtio_unregister_driver(&vcon_driver);
}

COSMO_MODULE("virtio_console", "1.0", vcon_module_init, vcon_module_shutdown, "virtio", MODULE_CAP_DRIVER);
