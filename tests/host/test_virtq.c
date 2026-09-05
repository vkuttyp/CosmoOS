/*
 * test_virtq.c - Host test of the split virtqueue against a hostile device
 * (docs/drivers/virtio/testing.md). ASan/UBSan.
 *
 * The real virtqueue.c runs over a fake transport; the "device" is this
 * file writing into the shared ring exactly as a peer would, including
 * everything a malicious or broken peer might write: descriptor `next`
 * links that loop, point out of range or at a free descriptor; used
 * elements with an id out of range, not in flight, completed twice, or a
 * length beyond the buffers. The driver's records, not the ring, must
 * decide what happens.
 */

#include "harness.h"

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/string.h>

#include <drivers/virtio.h>

#include <stdlib.h>
#include <string.h>

/* --- host services the queue code needs --- */

void *kmalloc(size_t size, unsigned flags)
{
    (void)flags;
    return calloc(1, size ? size : 1);
}

void *kzalloc(size_t size)
{
    return calloc(1, size ? size : 1);
}

void kfree(void *p)
{
    free(p);
}

static unsigned g_dma_live;

void *dma_alloc(struct device *dev, size_t size, dma_addr_t *dma_out, unsigned flags)
{
    (void)dev;
    (void)flags;
    void *p = NULL;
    if (posix_memalign(&p, 4096, size) != 0)
        return NULL;
    memset(p, 0, size);
    *dma_out = (dma_addr_t)(uintptr_t)p;
    g_dma_live++;
    return p;
}

void dma_free(struct device *dev, size_t size, void *va, dma_addr_t dma)
{
    (void)dev;
    (void)size;
    (void)dma;
    g_dma_live--;
    free(va);
}

void dma_sync_for_device(struct device *dev, dma_addr_t dma, size_t len, enum dma_dir dir)
{
    (void)dev; (void)dma; (void)len; (void)dir;
}

/* --- the fake transport --- */

static unsigned g_notifies;
static unsigned g_max_size = 16;

static unsigned tr_queue_max_size(struct virtio_device *vdev, unsigned index)
{
    (void)vdev; (void)index;
    return g_max_size;
}

static int tr_setup_queue(struct virtio_device *vdev, struct virtqueue *vq)
{
    (void)vdev; (void)vq;
    return 0;
}

static void tr_teardown_queue(struct virtio_device *vdev, struct virtqueue *vq)
{
    (void)vdev; (void)vq;
}

static void tr_notify(struct virtio_device *vdev, struct virtqueue *vq)
{
    (void)vdev; (void)vq;
    g_notifies++;
}

static const struct virtio_transport g_tr = {
    .name = "fake",
    .queue_max_size = tr_queue_max_size,
    .setup_queue = tr_setup_queue,
    .teardown_queue = tr_teardown_queue,
    .notify = tr_notify,
};

static struct virtio_device g_vdev;

static struct virtqueue *make_queue(unsigned size)
{
    memset(&g_vdev, 0, sizeof(g_vdev));
    strcpy(g_vdev.dev.name, "fake0");
    g_vdev.tr = &g_tr;
    g_max_size = size;
    struct virtqueue *vq = NULL;
    int rc = virtq_alloc(&g_vdev, 0, 0, NULL, &vq);
    EXPECT(rc == 0 && vq != NULL);
    return vq;
}

/* The device completes the chain at `head` with `len` bytes written. */
static void device_complete(struct virtqueue *vq, uint32_t head, uint32_t len)
{
    uint16_t idx = vq->used->idx;
    vq->used->ring[idx % vq->size].id = head;
    vq->used->ring[idx % vq->size].len = len;
    __atomic_thread_fence(__ATOMIC_RELEASE);   /* the ring is packed: fence, then a plain store */
    vq->used->idx = (uint16_t)(idx + 1);
}

/* Follow the chain the device sees, bounded, and count its descriptors. */
static unsigned device_chain_length(const struct virtqueue *vq, uint16_t head)
{
    unsigned n = 1;
    uint16_t i = head;
    while ((vq->desc[i].flags & VIRTQ_DESC_F_NEXT) && n <= vq->size) {
        i = vq->desc[i].next;
        if (i >= vq->size)
            return 0;   /* the device would fault here */
        n++;
    }
    return n;
}

static struct virtq_sg sg_of(uint64_t addr, uint32_t len)
{
    struct virtq_sg s = { .addr = addr, .len = len };
    return s;
}

/* --- tests --- */

static void test_alloc_free(void)
{
    struct virtqueue *vq = make_queue(16);
    EXPECT(vq->size == 16 && vq->num_free == 16 && vq->free_head == 0);
    for (unsigned i = 0; i < 16; i++)
        EXPECT(vq->shadow_next[i] == i + 1 && vq->chain_len[i] == 0 && vq->cookies[i] == NULL);
    EXPECT(virtq_free_count(vq) == 16);
    virtq_free(vq);
    EXPECT(g_dma_live == 0);

    /* Non power of two sizes are refused; a device maximum of 0 is ENOENT. */
    g_max_size = 24;
    memset(&g_vdev, 0, sizeof(g_vdev));
    g_vdev.tr = &g_tr;
    struct virtqueue *bad = NULL;
    EXPECT(virtq_alloc(&g_vdev, 0, 0, NULL, &bad) == -EINVAL);
    g_max_size = 0;
    EXPECT(virtq_alloc(&g_vdev, 0, 0, NULL, &bad) == -ENOENT);
    EXPECT(virtq_alloc(&g_vdev, VIRTIO_MAX_QUEUES, 0, NULL, &bad) == -EINVAL);
}

static void test_normal_chain(void)
{
    struct virtqueue *vq = make_queue(8);
    int cookie = 1;
    struct virtq_sg sg[3] = { sg_of(0x1000, 16), sg_of(0x2000, 100), sg_of(0x3000, 200) };
    EXPECT(virtq_add(vq, sg, 1, 2, &cookie) == 0);
    EXPECT(vq->num_free == 5 && vq->free_head == 3);
    EXPECT(vq->avail->idx == 1 && vq->avail->ring[0] == 0);
    /* What the device sees: three linked descriptors, the last unlinked. */
    EXPECT(vq->desc[0].addr == 0x1000 && vq->desc[0].len == 16 && vq->desc[0].flags == VIRTQ_DESC_F_NEXT &&
           vq->desc[0].next == 1);
    EXPECT(vq->desc[1].flags == (VIRTQ_DESC_F_NEXT | VIRTQ_DESC_F_WRITE) && vq->desc[1].next == 2);
    EXPECT(vq->desc[2].flags == VIRTQ_DESC_F_WRITE && vq->desc[2].next == 0);
    EXPECT(device_chain_length(vq, 0) == 3);
    /* Driver's records. */
    EXPECT(vq->chain_len[0] == 3 && vq->in_bytes[0] == 300 && vq->cookies[0] == &cookie);

    virtq_kick(vq);
    EXPECT(g_notifies == 1);
    vq->used->flags = VIRTQ_USED_F_NO_NOTIFY;
    virtq_kick(vq);
    EXPECT(g_notifies == 1);
    vq->used->flags = 0;

    uint32_t len = ~0u;
    EXPECT(virtq_pop(vq, &len) == NULL);   /* nothing completed yet */
    device_complete(vq, 0, 120);
    EXPECT(virtq_pop(vq, &len) == &cookie && len == 120);
    EXPECT(virtq_pop(vq, &len) == NULL);
    EXPECT(vq->num_free == 8 && vq->chain_len[0] == 0 && vq->cookies[0] == NULL && vq->bad_used == 0);
    virtq_free(vq);
}

static void test_empty_and_oversized(void)
{
    struct virtqueue *vq = make_queue(4);
    int c = 0;
    struct virtq_sg sg[5] = { sg_of(1, 1), sg_of(2, 1), sg_of(3, 1), sg_of(4, 1), sg_of(5, 1) };
    EXPECT(virtq_add(vq, sg, 0, 0, &c) == -EINVAL);          /* empty chain */
    EXPECT(virtq_add(vq, sg, 3, 2, &c) == -EINVAL);          /* longer than the table */
    EXPECT(virtq_add(vq, sg, 1, 0, NULL) == -EINVAL);        /* no cookie */
    EXPECT(virtq_add(vq, sg, 2, 2, &c) == 0);                /* exactly the table: the boundary descriptor is used */
    EXPECT(vq->num_free == 0 && vq->chain_len[0] == 4);
    EXPECT(vq->desc[2].next == 3 && vq->desc[3].flags == VIRTQ_DESC_F_WRITE);
    EXPECT(virtq_add(vq, sg, 1, 0, &c) == -ENOSPC);          /* full */
    device_complete(vq, 0, 2);
    uint32_t len;
    EXPECT(virtq_pop(vq, &len) == &c && len == 2 && vq->num_free == 4);
    virtq_free(vq);
}

/* The device rewrites descriptor links after the chain was posted: a
 * self-loop, a two-element loop, an out-of-range next, a link into a free
 * descriptor. None of it may change what the driver reclaims or builds
 * next. */
static void test_hostile_descriptor_links(void)
{
    struct virtqueue *vq = make_queue(8);
    int a = 1, b = 2, d = 3;
    struct virtq_sg sga[2] = { sg_of(0x10, 8), sg_of(0x20, 8) };
    struct virtq_sg sgb[3] = { sg_of(0x30, 8), sg_of(0x40, 8), sg_of(0x50, 8) };
    EXPECT(virtq_add(vq, sga, 1, 1, &a) == 0);    /* descriptors 0,1 */
    EXPECT(virtq_add(vq, sgb, 1, 2, &b) == 0);    /* descriptors 2,3,4 */
    EXPECT(vq->free_head == 5 && vq->num_free == 3);

    vq->desc[0].next = 0;          /* self-loop */
    vq->desc[2].next = 3;
    vq->desc[3].next = 2;          /* two-element loop */
    vq->desc[3].flags |= VIRTQ_DESC_F_NEXT;
    vq->desc[4].next = 0xFFFF;     /* out of range */
    vq->desc[4].flags |= VIRTQ_DESC_F_NEXT;
    vq->desc[1].next = 6;          /* into a free descriptor */
    vq->desc[1].flags |= VIRTQ_DESC_F_NEXT;

    uint32_t len;
    device_complete(vq, 2, 16);
    EXPECT(virtq_pop(vq, &len) == &b && len == 16);
    EXPECT(vq->num_free == 6 && vq->chain_len[2] == 0);
    device_complete(vq, 0, 8);
    EXPECT(virtq_pop(vq, &len) == &a && len == 8);
    EXPECT(vq->num_free == 8 && vq->bad_used == 0);

    /* Every descriptor is back on the free list exactly once: a chain of
     * the full table succeeds and touches each index once. */
    struct virtq_sg full[8];
    for (unsigned i = 0; i < 8; i++)
        full[i] = sg_of(0x100 * (i + 1), 4);
    EXPECT(virtq_add(vq, full, 8, 0, &d) == 0);
    EXPECT(vq->num_free == 0);
    bool seen[8] = { false };
    unsigned n = 0;
    uint16_t i = vq->free_head;   /* the head that was posted is avail->ring[2] */
    i = vq->avail->ring[2];
    for (unsigned k = 0; k < 8; k++) {
        EXPECT(i < 8 && !seen[i]);
        if (i >= 8)
            break;
        seen[i] = true;
        n++;
        i = vq->shadow_next[i];
    }
    EXPECT(n == 8);
    for (unsigned k = 0; k < 8; k++)
        EXPECT(vq->desc[k].len == 4);
    device_complete(vq, vq->avail->ring[2], 0);
    EXPECT(virtq_pop(vq, &len) == &d && len == 0 && vq->num_free == 8);
    virtq_free(vq);
}

/* Used elements the driver's records do not confirm are skipped and
 * counted; a length beyond the writable bytes is clamped. */
static void test_hostile_used_ring(void)
{
    struct virtqueue *vq = make_queue(8);
    int a = 1, b = 2;
    struct virtq_sg sga[2] = { sg_of(0x10, 8), sg_of(0x20, 64) };
    struct virtq_sg sgb[1] = { sg_of(0x30, 8) };
    EXPECT(virtq_add(vq, sga, 1, 1, &a) == 0);    /* 0,1 */
    EXPECT(virtq_add(vq, sgb, 1, 0, &b) == 0);    /* 2 */

    uint32_t len;
    device_complete(vq, 9, 1);                     /* index out of range */
    device_complete(vq, 7, 1);                     /* in range, never posted */
    device_complete(vq, 1, 1);                     /* in a chain but not its head */
    device_complete(vq, 0, 1000);                  /* valid head, impossible length */
    EXPECT(virtq_pop(vq, &len) == &a && len == 64);
    EXPECT(vq->bad_used == 4);
    device_complete(vq, 0, 8);                     /* completed twice */
    device_complete(vq, 2, 0);
    EXPECT(virtq_pop(vq, &len) == &b && len == 0);
    EXPECT(vq->bad_used == 5);
    EXPECT(virtq_pop(vq, &len) == NULL);
    EXPECT(vq->num_free == 8);
    virtq_free(vq);
}

/* Many cycles with out-of-order completions keep the free list exact. */
static void test_churn(void)
{
    struct virtqueue *vq = make_queue(32);
    int cookies[32];
    uint32_t rng = 12345;
    for (unsigned round = 0; round < 500; round++) {
        unsigned posted = 0;
        uint16_t heads[16];
        for (unsigned k = 0; k < 16; k++) {
            rng = rng * 1103515245u + 12345u;
            unsigned n = 1 + (rng >> 16) % 3;
            struct virtq_sg sg[3] = { sg_of(1, 1), sg_of(2, 2), sg_of(3, 3) };
            int rc = virtq_add(vq, sg, 1, n - 1, &cookies[k]);
            if (rc == -ENOSPC)
                break;
            EXPECT(rc == 0);
            heads[posted++] = vq->avail->ring[(vq->avail->idx - 1) % vq->size];
        }
        /* Complete in reverse order (nothing written: some chains have no writable bytes), one duplicate thrown in. */
        for (unsigned k = posted; k-- > 0;)
            device_complete(vq, heads[k], 0);
        if (posted)
            device_complete(vq, heads[0], 0);
        unsigned got = 0;
        uint32_t len;
        while (virtq_pop(vq, &len) != NULL)
            got++;
        EXPECT(got == posted);
        EXPECT(vq->num_free == 32);
    }
    EXPECT(vq->bad_used == 500);   /* exactly the duplicates */
    virtq_free(vq);
}

static const struct host_test tests[] = {
    { "virtq-alloc-free", test_alloc_free },
    { "virtq-normal-chain", test_normal_chain },
    { "virtq-empty-oversized", test_empty_and_oversized },
    { "virtq-hostile-links", test_hostile_descriptor_links },
    { "virtq-hostile-used", test_hostile_used_ring },
    { "virtq-churn", test_churn },
};

int main(void)
{
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
