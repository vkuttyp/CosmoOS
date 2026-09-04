/*
 * virtqueue.c - Split virtqueues (VirtIO 1.1 section 2.6).
 *
 * One contiguous DMA allocation per queue holds the descriptor table,
 * the available ring and the used ring at their required alignments.
 * Free descriptors form a singly linked list through `next`. Everything
 * the device writes (used ring) is validated before use.
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/module.h>
#include <kernel/string.h>

#include <drivers/virtio.h>

static inline void wmb(void)
{
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

static inline void rmb(void)
{
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
}

static inline uint16_t read_le16(const volatile uint16_t *p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

int virtq_alloc(struct virtio_device *vdev, unsigned index, unsigned max, void (*callback)(struct virtqueue *),
                struct virtqueue **out)
{
    if (index >= VIRTIO_MAX_QUEUES || vdev->vq[index] != NULL)
        return -EINVAL;
    unsigned devmax = vdev->tr->queue_max_size(vdev, index);
    if (devmax == 0)
        return -ENOENT;
    unsigned size = devmax;
    if (max && max < size)
        size = max;
    if (size > VIRTQ_MAX_SIZE)
        size = VIRTQ_MAX_SIZE;
    if ((size & (size - 1)) != 0)
        return -EINVAL;   /* the spec requires a power of two */

    struct virtqueue *vq = kzalloc(sizeof(*vq));
    if (vq == NULL)
        return -ENOMEM;
    vq->cookies = kzalloc(size * sizeof(void *));
    if (vq->cookies == NULL) {
        kfree(vq);
        return -ENOMEM;
    }

    size_t desc_bytes = (size_t)size * sizeof(struct virtq_desc);
    size_t avail_bytes = 6 + 2 * (size_t)size;      /* flags, idx, ring, used_event */
    size_t avail_off = desc_bytes;                  /* 16-aligned already */
    size_t used_off = ALIGN_UP(avail_off + avail_bytes, 4096);
    size_t used_bytes = 6 + 8 * (size_t)size;
    vq->ring_bytes = ALIGN_UP(used_off + used_bytes, 4096);
    vq->ring_mem = dma_alloc(&vdev->dev, vq->ring_bytes, &vq->ring_dma, DMA_ZERO);
    if (vq->ring_mem == NULL) {
        kfree(vq->cookies);
        kfree(vq);
        return -ENOMEM;
    }
    vq->vdev = vdev;
    vq->index = index;
    vq->size = size;
    vq->desc = vq->ring_mem;
    vq->avail = (struct virtq_avail *)((uint8_t *)vq->ring_mem + avail_off);
    vq->used = (struct virtq_used *)((uint8_t *)vq->ring_mem + used_off);
    vq->desc_dma = vq->ring_dma;
    vq->avail_dma = vq->ring_dma + avail_off;
    vq->used_dma = vq->ring_dma + used_off;
    vq->callback = callback;
    vq->vector = -1;
    spinlock_init(&vq->lock, "virtq");

    for (unsigned i = 0; i < size; i++)
        vq->desc[i].next = (uint16_t)(i + 1);
    vq->free_head = 0;
    vq->num_free = (uint16_t)size;
    vq->last_used = 0;

    int rc = vdev->tr->setup_queue(vdev, vq);
    if (rc) {
        dma_free(&vdev->dev, vq->ring_bytes, vq->ring_mem, vq->ring_dma);
        kfree(vq->cookies);
        kfree(vq);
        return rc;
    }
    vdev->vq[index] = vq;
    if (index + 1 > vdev->nr_vq)
        vdev->nr_vq = index + 1;
    kdebug("virtio: %s: queue %u, %u entries, %s", vdev->dev.name, index, size,
           callback ? "interrupt driven" : "polled");
    *out = vq;
    return 0;
}

void virtq_free(struct virtqueue *vq)
{
    struct virtio_device *vdev = vq->vdev;
    vdev->tr->teardown_queue(vdev, vq);
    vdev->vq[vq->index] = NULL;
    dma_free(&vdev->dev, vq->ring_bytes, vq->ring_mem, vq->ring_dma);
    kfree(vq->cookies);
    kfree(vq);
}

int virtq_add(struct virtqueue *vq, const struct virtq_sg *sg, unsigned out, unsigned in, void *cookie)
{
    unsigned total = out + in;
    if (total == 0 || total > vq->size || cookie == NULL)
        return -EINVAL;

    arch_irq_state_t s = spin_lock_irqsave(&vq->lock);
    if (vq->num_free < total) {
        spin_unlock_irqrestore(&vq->lock, s);
        return -ENOSPC;
    }
    uint16_t head = vq->free_head;
    uint16_t i = head;
    for (unsigned n = 0; n < total; n++) {
        struct virtq_desc *d = &vq->desc[i];
        d->addr = sg[n].addr;
        d->len = sg[n].len;
        d->flags = (uint16_t)((n >= out ? VIRTQ_DESC_F_WRITE : 0) | (n + 1 < total ? VIRTQ_DESC_F_NEXT : 0));
        uint16_t next = d->next;
        if (n + 1 == total)
            vq->free_head = next;
        i = next;
    }
    vq->num_free = (uint16_t)(vq->num_free - total);
    vq->cookies[head] = cookie;

    uint16_t idx = vq->avail->idx;
    vq->avail->ring[idx % vq->size] = head;
    wmb();
    vq->avail->idx = (uint16_t)(idx + 1);
    spin_unlock_irqrestore(&vq->lock, s);
    return 0;
}

void virtq_kick(struct virtqueue *vq)
{
    dma_sync_for_device(&vq->vdev->dev, vq->ring_dma, vq->ring_bytes, DMA_BIDIRECTIONAL);
    if ((read_le16(&vq->used->flags) & VIRTQ_USED_F_NO_NOTIFY) == 0) {
        vq->kicks++;
        vq->vdev->tr->notify(vq->vdev, vq);
    }
}

void *virtq_pop(struct virtqueue *vq, uint32_t *len)
{
    arch_irq_state_t s = spin_lock_irqsave(&vq->lock);
    if (vq->last_used == read_le16(&vq->used->idx)) {
        spin_unlock_irqrestore(&vq->lock, s);
        return NULL;
    }
    rmb();
    struct virtq_used_elem e = vq->used->ring[vq->last_used % vq->size];
    vq->last_used++;
    if (e.id >= vq->size || vq->cookies[e.id] == NULL) {
        spin_unlock_irqrestore(&vq->lock, s);
        kerror("virtio: %s: queue %u: device returned bad descriptor id %u", vq->vdev->dev.name, vq->index, e.id);
        return NULL;
    }
    void *cookie = vq->cookies[e.id];
    vq->cookies[e.id] = NULL;

    /* Return the chain to the free list. */
    uint16_t i = (uint16_t)e.id;
    unsigned count = 1;
    while (vq->desc[i].flags & VIRTQ_DESC_F_NEXT) {
        i = vq->desc[i].next;
        if (i >= vq->size || count > vq->size) {
            spin_unlock_irqrestore(&vq->lock, s);
            kerror("virtio: %s: queue %u: corrupt descriptor chain", vq->vdev->dev.name, vq->index);
            return NULL;
        }
        count++;
    }
    vq->desc[i].next = vq->free_head;
    vq->free_head = (uint16_t)e.id;
    vq->num_free = (uint16_t)(vq->num_free + count);
    spin_unlock_irqrestore(&vq->lock, s);
    if (len)
        *len = e.len;
    return cookie;
}

unsigned virtq_free_count(struct virtqueue *vq)
{
    arch_irq_state_t s = spin_lock_irqsave(&vq->lock);
    unsigned n = vq->num_free;
    spin_unlock_irqrestore(&vq->lock, s);
    return n;
}

void virtq_interrupt(struct virtqueue *vq)
{
    vq->interrupts++;
    if (vq->callback)
        vq->callback(vq);
}

EXPORT_SYMBOL(virtq_alloc);
EXPORT_SYMBOL(virtq_free);
EXPORT_SYMBOL(virtq_add);
EXPORT_SYMBOL(virtq_kick);
EXPORT_SYMBOL(virtq_pop);
EXPORT_SYMBOL(virtq_free_count);
