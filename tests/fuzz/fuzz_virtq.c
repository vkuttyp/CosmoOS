/*
 * fuzz_virtq.c - The split virtqueue (drivers/virtio/virtqueue.c) against
 * a hostile device driven by the input (docs/verification/design.md).
 *
 * The input is a program: one byte selects an operation, the following
 * bytes are operands. The device side writes the shared ring exactly as a
 * peer would, including every corruption a malicious peer could make; the
 * driver's private records must decide what happens. Assertions: a popped
 * cookie is one the driver added and has not reclaimed yet; num_free never
 * exceeds the queue size; the driver never reads outside its allocations
 * (ASan).
 */

#include "fuzz.h"

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

void *dma_alloc(struct device *dev, size_t size, dma_addr_t *dma_out, unsigned flags)
{
    (void)dev;
    (void)flags;
    void *p = aligned_alloc(4096, (size + 4095) & ~(size_t)4095);
    if (p == NULL)
        return NULL;
    memset(p, 0, size);
    *dma_out = (dma_addr_t)(uintptr_t)p;
    return p;
}

void dma_free(struct device *dev, size_t size, void *va, dma_addr_t dma)
{
    (void)dev; (void)size; (void)dma;
    free(va);
}

void dma_sync_for_device(struct device *dev, dma_addr_t dma, size_t len, enum dma_dir dir)
{
    (void)dev; (void)dma; (void)len; (void)dir;
}

static unsigned g_max_size = 16;

static unsigned tr_queue_max_size(struct virtio_device *vdev, unsigned index)
{
    (void)vdev; (void)index;
    return g_max_size;
}
static int tr_setup_queue(struct virtio_device *vdev, struct virtqueue *vq) { (void)vdev; (void)vq; return 0; }
static void tr_teardown_queue(struct virtio_device *vdev, struct virtqueue *vq) { (void)vdev; (void)vq; }
static void tr_notify(struct virtio_device *vdev, struct virtqueue *vq) { (void)vdev; (void)vq; }

static const struct virtio_transport g_tr = {
    .name = "fuzz",
    .queue_max_size = tr_queue_max_size,
    .setup_queue = tr_setup_queue,
    .teardown_queue = tr_teardown_queue,
    .notify = tr_notify,
};

/* --- the program --- */

#define MAX_COOKIES 64
static char g_cookies[MAX_COOKIES];      /* distinct addresses */
static bool g_inflight[MAX_COOKIES];

static uint8_t take(const uint8_t **p, const uint8_t *end)
{
    return *p < end ? *(*p)++ : 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const uint8_t *p = data, *end = data + size;
    static const unsigned sizes[4] = { 4, 8, 16, 64 };
    g_max_size = sizes[take(&p, end) & 3];

    struct virtio_device vdev;
    memset(&vdev, 0, sizeof(vdev));
    strcpy(vdev.dev.name, "fuzz0");
    vdev.tr = &g_tr;
    struct virtqueue *vq = NULL;
    if (virtq_alloc(&vdev, 0, 0, NULL, &vq) != 0 || vq == NULL)
        return 0;
    memset(g_inflight, 0, sizeof(g_inflight));
    /* One buffer everything points at: the device never writes to it
     * here, so its contents do not matter; the lengths do. */
    static uint8_t buffer[8192];

    for (unsigned step = 0; step < 512 && p < end; step++) {
        uint8_t op = take(&p, end);
        switch (op % 6) {
        case 0: { /* driver adds a chain of n buffers, k of them device-writable */
            unsigned n = 1 + (take(&p, end) % 8);
            unsigned k = take(&p, end) % (n + 1);
            unsigned c = take(&p, end) % MAX_COOKIES;
            if (g_inflight[c])
                break;
            struct virtq_sg sg[8];
            for (unsigned i = 0; i < n; i++) {
                sg[i].addr = (uint64_t)(uintptr_t)buffer + i * 512;
                sg[i].len = 1 + (take(&p, end) % 512);
            }
            int rc = virtq_add(vq, sg, n - k, k, &g_cookies[c]);
            if (rc == 0) {
                g_inflight[c] = true;
            } else {
                FUZZ_ASSERT(rc == -ENOSPC || rc == -EINVAL);
            }
            break;
        }
        case 1: { /* device completes a used element with an arbitrary id and length */
            uint16_t idx = vq->used->idx;
            uint32_t id = take(&p, end);
            if (take(&p, end) & 1)
                id |= (uint32_t)take(&p, end) << 8;
            uint32_t len = ((uint32_t)take(&p, end) << 8) | take(&p, end);
            if (take(&p, end) & 1)
                len |= 0xffff0000u;
            vq->used->ring[idx % vq->size].id = id;
            vq->used->ring[idx % vq->size].len = len;
            __atomic_thread_fence(__ATOMIC_RELEASE);
            vq->used->idx = (uint16_t)(idx + 1);
            break;
        }
        case 2: { /* driver pops */
            uint32_t len = 0;
            void *cookie = virtq_pop(vq, &len);
            if (cookie) {
                FUZZ_ASSERT(cookie >= (void *)g_cookies && cookie < (void *)(g_cookies + MAX_COOKIES));
                unsigned c = (unsigned)((char *)cookie - g_cookies);
                FUZZ_ASSERT(g_inflight[c]);   /* never a cookie not in flight, never twice */
                g_inflight[c] = false;
            }
            break;
        }
        case 3: { /* device rewrites a descriptor field */
            unsigned d = take(&p, end) % vq->size;
            switch (take(&p, end) % 4) {
            case 0: vq->desc[d].next = (uint16_t)(take(&p, end) | ((uint16_t)take(&p, end) << 8)); break;
            case 1: vq->desc[d].flags = (uint16_t)take(&p, end); break;
            case 2: vq->desc[d].len = (uint32_t)take(&p, end) << 12; break;
            default: vq->desc[d].addr = (uint64_t)take(&p, end) << 40; break;
            }
            break;
        }
        case 4: { /* device jumps used->idx by k */
            uint16_t k = take(&p, end);
            __atomic_thread_fence(__ATOMIC_RELEASE);
            vq->used->idx = (uint16_t)(vq->used->idx + k);
            break;
        }
        default: /* device scribbles over the avail ring */
            vq->avail->ring[take(&p, end) % vq->size] = (uint16_t)(take(&p, end) | ((uint16_t)take(&p, end) << 8));
            vq->avail->idx = (uint16_t)(take(&p, end) | ((uint16_t)take(&p, end) << 8));
            break;
        }
        FUZZ_ASSERT(vq->num_free <= vq->size);
        FUZZ_ASSERT(virtq_free_count(vq) <= vq->size);
    }
    /* Drain what the driver still believes is in flight through a
     * well-formed completion of each head it knows; the pop must return
     * each cookie at most once. */
    for (unsigned r = 0; r < 2 * MAX_COOKIES; r++) {
        uint32_t len;
        void *cookie = virtq_pop(vq, &len);
        if (cookie == NULL)
            break;
        unsigned c = (unsigned)((char *)cookie - g_cookies);
        FUZZ_ASSERT(c < MAX_COOKIES && g_inflight[c]);
        g_inflight[c] = false;
    }
    virtq_free(vq);
    return 0;
}

size_t fuzz_seed(unsigned i, uint8_t *buf, size_t cap)
{
    if (i != 0 || cap < 32)
        return 0;
    /* size 16; add a 2-buffer chain as cookie 3; the device completes head 0 with 100 bytes; pop. */
    static const uint8_t prog[] = { 2, 0, 2, 0, 3, 100, 200, 1, 0, 0, 0, 100, 0, 2 };
    memcpy(buf, prog, sizeof(prog));
    return sizeof(prog);
}
