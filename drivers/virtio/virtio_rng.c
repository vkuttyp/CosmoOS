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
#include <kernel/spinlock.h>
#include <kernel/string.h>

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
    spinlock_t post_lock;
    bool stopping;
#if CONFIG_SELFTEST
    bool test_synthetic;
    bool test_reset;
    unsigned test_posts_after_reset;
#endif
};

static void vrng_post(struct vrng *r)
{
    arch_irq_state_t s = spin_lock_irqsave(&r->post_lock);
#if CONFIG_SELFTEST
    if (r->test_synthetic) {
        if (r->test_reset)
            r->test_posts_after_reset++;
        r->posted = true;
        spin_unlock_irqrestore(&r->post_lock, s);
        return;
    }
#endif
    struct virtq_sg sg = { .addr = r->buf_dma, .len = VRNG_BUF };
    if (virtq_add(r->vq, &sg, 0, 1, r) == 0) {
        r->posted = true;
        virtq_kick(r->vq);
    }
    spin_unlock_irqrestore(&r->post_lock, s);
}

static void vrng_completed(struct vrng *r, unsigned len)
{
    if (len > VRNG_BUF)
        len = VRNG_BUF;
    if (len > 0) {
        random_add_entropy(r->buf, len, len * 8);
        r->collected += len;
    }
    if (r->collected < VRNG_BUDGET)
        vrng_post(r);
}

static unsigned vrng_done(struct virtqueue *vq, unsigned budget)
{
    struct vrng *r = vq->vdev->priv;
    uint32_t len;
    unsigned n = 0;
    for (; n < budget && virtq_pop(vq, &len) != NULL; n++) {
        arch_irq_state_t s = spin_lock_irqsave(&r->post_lock);
        r->posted = false;
        spin_unlock_irqrestore(&r->post_lock, s);
        vrng_completed(r, len);
    }
    return n;
}

static int vrng_probe(struct virtio_device *vdev)
{
    struct vrng *r = kzalloc(sizeof(*r));
    if (r == NULL)
        return -ENOMEM;
    r->vdev = vdev;
    vdev->priv = r;
    spinlock_init(&r->post_lock, "virtio-rng-post");

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
    {
        arch_irq_state_t s = spin_lock_irqsave(&r->post_lock);
        r->stopping = true;
        spin_unlock_irqrestore(&r->post_lock, s);
    }
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
    arch_irq_state_t s = spin_lock_irqsave(&r->post_lock);
    r->stopping = true;
    spin_unlock_irqrestore(&r->post_lock, s);
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
#if CONFIG_SELFTEST
    struct vrng test;
    memset(&test, 0, sizeof(test));
    spinlock_init(&test.post_lock, "virtio-rng-test");
    test.stopping = true;
    test.test_synthetic = true;
    test.test_reset = true;
    vrng_completed(&test, 0);
    kinfo("VRNG-RESET-REPOST: %s posts_after_reset=%u",
          test.test_posts_after_reset == 0 ? "PASS" : "FAIL", test.test_posts_after_reset);
#endif
    return virtio_register_driver(&vrng_driver);
}

static void vrng_module_shutdown(void)
{
    virtio_unregister_driver(&vrng_driver);
}

COSMO_MODULE("virtio_rng", "1.0", vrng_module_init, vrng_module_shutdown, "virtio", MODULE_CAP_DRIVER);
