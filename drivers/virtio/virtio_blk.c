/*
 * virtio_blk.c - virtio block device driver (VirtIO 1.1 section 5.2),
 * registering a struct blkdev with the block layer. Module `virtio_blk`,
 * depends on `virtio`.
 *
 * One request queue. Each bio becomes a three-descriptor chain: the
 * request header (device readable), the data buffer, and the status
 * byte (device writable). Headers and status bytes live in a per-slot
 * DMA pool allocated at probe; the data buffer is the bio's, mapped
 * through dma_map.
 */

#include <kernel/blk.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/module.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>

#include <drivers/virtio.h>

#define VIRTIO_BLK_F_SIZE_MAX (1ULL << 1)
#define VIRTIO_BLK_F_SEG_MAX  (1ULL << 2)
#define VIRTIO_BLK_F_RO       (1ULL << 5)
#define VIRTIO_BLK_F_BLK_SIZE (1ULL << 6)
#define VIRTIO_BLK_F_FLUSH    (1ULL << 9)

#define CFG_CAPACITY 0
#define CFG_SIZE_MAX 8
#define CFG_SEG_MAX  12
#define CFG_BLK_SIZE 20

#define VIRTIO_BLK_T_IN    0u
#define VIRTIO_BLK_T_OUT   1u
#define VIRTIO_BLK_T_FLUSH 4u

#define VIRTIO_BLK_S_OK     0u
#define VIRTIO_BLK_S_IOERR  1u
#define VIRTIO_BLK_S_UNSUPP 2u

#define VBLK_MAX_SECTORS 128u   /* 64 KiB per request */

struct vblk_req_hdr {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __packed;

/* One DMA'd slot: header then status byte, padded to 32 bytes. */
struct vblk_slot {
    struct vblk_req_hdr hdr;
    uint8_t status;
    uint8_t pad[15];
} __packed;

struct vblk {
    struct virtio_device *vdev;
    struct virtqueue *vq;
    struct blkdev bd;
    struct vblk_slot *slots;
    dma_addr_t slots_dma;
    size_t slots_bytes;
    struct bio **inflight;      /* per slot */
    unsigned nr_slots;
    unsigned next_slot;
    spinlock_t lock;
    bool flush;
};

static int vblk_submit(struct blkdev *bd, struct bio *bio)
{
    struct vblk *vb = bd->priv;

    arch_irq_state_t s = spin_lock_irqsave(&vb->lock);
    unsigned slot = vb->nr_slots;
    for (unsigned n = 0; n < vb->nr_slots; n++) {
        unsigned i = (vb->next_slot + n) % vb->nr_slots;
        if (vb->inflight[i] == NULL) {
            slot = i;
            break;
        }
    }
    if (slot == vb->nr_slots) {
        spin_unlock_irqrestore(&vb->lock, s);
        return -EAGAIN;
    }
    vb->inflight[slot] = bio;
    vb->next_slot = (slot + 1) % vb->nr_slots;
    spin_unlock_irqrestore(&vb->lock, s);

    struct vblk_slot *sl = &vb->slots[slot];
    dma_addr_t sl_dma = vb->slots_dma + slot * sizeof(*sl);
    sl->hdr.reserved = 0;
    sl->status = 0xff;

    struct virtq_sg sg[3];
    unsigned out = 1, in = 1;
    sg[0].addr = sl_dma;
    sg[0].len = sizeof(sl->hdr);
    if (bio->dir == BIO_FLUSH) {
        sl->hdr.type = VIRTIO_BLK_T_FLUSH;
        sl->hdr.sector = 0;
        sg[1].addr = sl_dma + offsetof(struct vblk_slot, status);
        sg[1].len = 1;
    } else {
        size_t bytes = (size_t)bio->nsectors * bd->sector_size;
        dma_addr_t data = dma_map(bd->dev, bio->buf, bytes,
                                  bio->dir == BIO_WRITE ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
        if (data == 0) {
            vb->inflight[slot] = NULL;
            return -EINVAL;
        }
        sl->hdr.type = bio->dir == BIO_WRITE ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
        sl->hdr.sector = bio->sector * (bd->sector_size / 512);
        sg[1].addr = data;
        sg[1].len = (uint32_t)bytes;
        sg[2].addr = sl_dma + offsetof(struct vblk_slot, status);
        sg[2].len = 1;
        if (bio->dir == BIO_WRITE)
            out = 2;
        else
            in = 2;
    }
    bio->drvpriv = (void *)(uintptr_t)(slot + 1);
    int rc = virtq_add(vb->vq, sg, out, in, bio);
    if (rc) {
        vb->inflight[slot] = NULL;
        return rc;
    }
    virtq_kick(vb->vq);
    return 0;
}

static void vblk_done(struct virtqueue *vq)
{
    struct vblk *vb = vq->vdev->priv;
    uint32_t len;
    struct bio *bio;
    while ((bio = virtq_pop(vq, &len)) != NULL) {
        unsigned slot = (unsigned)(uintptr_t)bio->drvpriv - 1;
        int status;
        if (slot >= vb->nr_slots || vb->inflight[slot] != bio) {
            kerror("virtio-blk: completion for an unknown request");
            continue;
        }
        switch (vb->slots[slot].status) {
        case VIRTIO_BLK_S_OK:     status = 0; break;
        case VIRTIO_BLK_S_UNSUPP: status = -ENOTSUP; break;
        default:                  status = -EIO; break;
        }
        vb->inflight[slot] = NULL;
        bio_complete(bio, status);
    }
}

static const struct blkdev_ops vblk_ops = {
    .submit = vblk_submit,
};

static int vblk_probe(struct virtio_device *vdev)
{
    struct vblk *vb = kzalloc(sizeof(*vb));
    if (vb == NULL)
        return -ENOMEM;
    vb->vdev = vdev;
    vdev->priv = vb;
    spinlock_init(&vb->lock, "virtio-blk");

    int rc = virtio_device_init(vdev, VIRTIO_BLK_F_SEG_MAX | VIRTIO_BLK_F_RO | VIRTIO_BLK_F_BLK_SIZE |
                                          VIRTIO_BLK_F_FLUSH | VIRTIO_BLK_F_SIZE_MAX);
    if (rc)
        goto fail;

    uint64_t capacity = virtio_read_config64(vdev, CFG_CAPACITY);   /* 512-byte sectors */
    uint32_t blk_size = virtio_has_feature(vdev, VIRTIO_BLK_F_BLK_SIZE) ? virtio_read_config32(vdev, CFG_BLK_SIZE)
                                                                       : 512;
    if (capacity == 0 || blk_size < 512 || (blk_size & (blk_size - 1)) != 0 || blk_size > 4096) {
        kerror("virtio-blk: %s: unusable geometry (capacity %llu, block size %u)", vdev->dev.name,
               (unsigned long long)capacity, blk_size);
        rc = -EIO;
        goto fail;
    }
    vb->flush = virtio_has_feature(vdev, VIRTIO_BLK_F_FLUSH);

    rc = virtq_alloc(vdev, 0, 0, vblk_done, &vb->vq);
    if (rc)
        goto fail;

    vb->nr_slots = vb->vq->size / 4;
    if (vb->nr_slots < 4)
        vb->nr_slots = 4;
    vb->slots_bytes = vb->nr_slots * sizeof(struct vblk_slot);
    vb->slots = dma_alloc(&vdev->dev, vb->slots_bytes, &vb->slots_dma, DMA_ZERO);
    vb->inflight = kzalloc(vb->nr_slots * sizeof(*vb->inflight));
    if (vb->slots == NULL || vb->inflight == NULL) {
        rc = -ENOMEM;
        goto fail_vq;
    }

    virtio_device_ready(vdev);

    vb->bd.dev = &vdev->dev;
    vb->bd.ops = &vblk_ops;
    vb->bd.sector_size = blk_size;
    vb->bd.capacity = capacity / (blk_size / 512);
    vb->bd.max_sectors = VBLK_MAX_SECTORS * 512 / blk_size;
    vb->bd.read_only = virtio_has_feature(vdev, VIRTIO_BLK_F_RO);
    vb->bd.priv = vb;
    rc = blk_register(&vb->bd, "vd");
    if (rc)
        goto fail_vq;
    kinfo("virtio-blk: %s is %s%s", vdev->dev.name, vb->bd.name, vb->flush ? " (flush)" : "");
    return 0;

fail_vq:
    virtio_device_reset(vdev);
    if (vb->vq)
        virtq_free(vb->vq);
fail:
    if (vb->slots)
        dma_free(&vdev->dev, vb->slots_bytes, vb->slots, vb->slots_dma);
    kfree(vb->inflight);
    kfree(vb);
    vdev->priv = NULL;
    return rc;
}

static void vblk_remove(struct virtio_device *vdev)
{
    struct vblk *vb = vdev->priv;
    blk_unregister(&vb->bd);
    virtio_device_reset(vdev);   /* the device drops every in-flight request */
    for (unsigned i = 0; i < vb->nr_slots; i++) {
        if (vb->inflight[i]) {
            struct bio *bio = vb->inflight[i];
            vb->inflight[i] = NULL;
            bio_complete(bio, -EIO);
        }
    }
    virtq_free(vb->vq);
    dma_free(&vdev->dev, vb->slots_bytes, vb->slots, vb->slots_dma);
    kfree(vb->inflight);
    kfree(vb);
    vdev->priv = NULL;
}

static const uint32_t vblk_ids[] = { VIRTIO_ID_BLOCK, 0 };

static struct virtio_driver vblk_driver = {
    .drv = { .name = "virtio_blk" },
    .ids = vblk_ids,
    .probe = vblk_probe,
    .remove = vblk_remove,
};

static int vblk_module_init(void)
{
    return virtio_register_driver(&vblk_driver);
}

static void vblk_module_shutdown(void)
{
    virtio_unregister_driver(&vblk_driver);
}

COSMO_MODULE("virtio_blk", "1.0", vblk_module_init, vblk_module_shutdown, "virtio", MODULE_CAP_DRIVER);
