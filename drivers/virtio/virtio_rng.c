/*
 * virtio_rng.c - virtio entropy device (VirtIO 1.1 section 5.4), feeding
 * the kernel pool. Module `virtio_rng`, depends on `virtio`.
 *
 * One queue. A 64-byte device-writable buffer is posted; every
 * completion credits the bytes the device wrote and re-posts until a
 * per-boot budget is reached, so the pool is seeded without keeping the
 * device busy forever.
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/module.h>
#include <kernel/random.h>

#include <drivers/virtio.h>

#define VRNG_BUF   64u
#define VRNG_BUDGET 4096u

struct vrng {
    struct virtio_device *vdev;
    struct virtqueue *vq;
    uint8_t *buf;
    dma_addr_t buf_dma;
    unsigned collected;
    bool posted;
};

static void vrng_post(struct vrng *r)
{
    struct virtq_sg sg = { .addr = r->buf_dma, .len = VRNG_BUF };
    if (virtq_add(r->vq, &sg, 0, 1, r) == 0) {
        r->posted = true;
        virtq_kick(r->vq);
    }
}

static void vrng_done(struct virtqueue *vq)
{
    struct vrng *r = vq->vdev->priv;
    uint32_t len;
    while (virtq_pop(vq, &len) != NULL) {
        r->posted = false;
        if (len > VRNG_BUF)
            len = VRNG_BUF;
        if (len > 0) {
            random_add_entropy(r->buf, len, len * 8);
            r->collected += len;
        }
        if (r->collected < VRNG_BUDGET)
            vrng_post(r);
    }
}

static int vrng_probe(struct virtio_device *vdev)
{
    struct vrng *r = kzalloc(sizeof(*r));
    if (r == NULL)
        return -ENOMEM;
    r->vdev = vdev;
    vdev->priv = r;

    int rc = virtio_device_init(vdev, 0);
    if (rc)
        goto fail;
    r->buf = dma_alloc(&vdev->dev, VRNG_BUF, &r->buf_dma, DMA_ZERO);
    if (r->buf == NULL) {
        rc = -ENOMEM;
        goto fail;
    }
    rc = virtq_alloc(vdev, 0, 0, vrng_done, &r->vq);
    if (rc)
        goto fail;
    virtio_device_ready(vdev);
    vrng_post(r);
    kinfo("virtio-rng: %s: feeding the entropy pool", vdev->dev.name);
    return 0;

fail:
    virtio_device_reset(vdev);
    if (r->buf)
        dma_free(&vdev->dev, VRNG_BUF, r->buf, r->buf_dma);
    kfree(r);
    vdev->priv = NULL;
    return rc;
}

static void vrng_remove(struct virtio_device *vdev)
{
    struct vrng *r = vdev->priv;
    virtio_device_reset(vdev);
    virtq_free(r->vq);
    dma_free(&vdev->dev, VRNG_BUF, r->buf, r->buf_dma);
    kinfo("virtio-rng: %s: removed after %u bytes", vdev->dev.name, r->collected);
    kfree(r);
    vdev->priv = NULL;
}

static const uint32_t vrng_ids[] = { VIRTIO_ID_RNG, 0 };

static struct virtio_driver vrng_driver = {
    .drv = { .name = "virtio_rng" },
    .ids = vrng_ids,
    .probe = vrng_probe,
    .remove = vrng_remove,
};

static int vrng_module_init(void)
{
    return virtio_register_driver(&vrng_driver);
}

static void vrng_module_shutdown(void)
{
    virtio_unregister_driver(&vrng_driver);
}

COSMO_MODULE("virtio_rng", "1.0", vrng_module_init, vrng_module_shutdown, "virtio", MODULE_CAP_DRIVER);
