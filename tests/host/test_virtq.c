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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    /* C11 aligned_alloc (posix_memalign is not visible under -std=c11 on
     * glibc); the ring size is a multiple of 4096 by construction. */
    void *p = aligned_alloc(4096, size);
    if (p == NULL)
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

/* The queue's deferred completion work is the kernel's (kernel/core/
 * irqpoll.c). Every queue here is polled (a NULL callback), so nothing is
 * ever scheduled; a scheduled poll would run to the end, synchronously. */
void irq_poll_init(struct irq_poll *ip, irq_poll_fn poll, const char *name)
{
    ip->poll = poll;
    ip->name = name;
}
void irq_poll_sched(struct irq_poll *ip)
{
    while (ip->poll(ip, IRQ_POLL_BUDGET) >= IRQ_POLL_BUDGET)
        ;
}
void irq_poll_disable(struct irq_poll *ip) { (void)ip; }
void irq_poll_set_class(struct irq_poll *ip, const void *fn) { (void)ip; (void)fn; }

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

static const struct virtio_transport g_tr_legacy = {
    .name = "fake-legacy",
    .legacy = true,
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

/* A device-readable-only chain (a transmit) completed with a length: a
 * modern device that claims to have written into it is counted bad; a
 * legacy one, which commonly reports the bytes it read (Parallels Desktop's
 * network adapter, every transmit), is not, and the length is 0 either way. */
static void test_readonly_chain_length(void)
{
    for (int legacy = 0; legacy <= 1; legacy++) {
        struct virtqueue *vq = make_queue(8);
        if (legacy)
            g_vdev.tr = &g_tr_legacy;
        int cookie = 1;
        struct virtq_sg sg[1] = { sg_of(0x1000, 354) };
        EXPECT(virtq_add(vq, sg, 1, 0, &cookie) == 0);
        device_complete(vq, 0, 354);
        uint32_t len = 99;
        EXPECT(virtq_pop(vq, &len) == &cookie);
        EXPECT(len == 0);
        EXPECT(vq->bad_used == (legacy ? 0u : 1u));
        virtq_free(vq);
    }
    /* A writable chain overstated is bad on a legacy device too. */
    struct virtqueue *vq = make_queue(8);
    g_vdev.tr = &g_tr_legacy;
    int cookie = 2;
    struct virtq_sg sg[1] = { sg_of(0x1000, 100) };
    EXPECT(virtq_add(vq, sg, 0, 1, &cookie) == 0);
    device_complete(vq, 0, 200);
    uint32_t len = 0;
    EXPECT(virtq_pop(vq, &len) == &cookie);
    EXPECT(len == 100 && vq->bad_used == 1);
    virtq_free(vq);
}

/* A legacy queue is the device's size, up to VIRTQ_LEGACY_MAX_SIZE (QEMU
 * 10's legacy virtio-net has 1024); a modern one is capped at
 * VIRTQ_MAX_SIZE. */
static void test_legacy_queue_size(void)
{
    struct virtqueue *vq = NULL;
    memset(&g_vdev, 0, sizeof(g_vdev));
    strcpy(g_vdev.dev.name, "fake0");
    g_vdev.tr = &g_tr_legacy;
    g_max_size = 1024;
    EXPECT(virtq_alloc(&g_vdev, 0, 0, NULL, &vq) == 0 && vq != NULL && vq->size == 1024);
    virtq_free(vq);
    g_vdev.vq[0] = NULL;
    vq = NULL;
    g_max_size = 2048;
    EXPECT(virtq_alloc(&g_vdev, 0, 0, NULL, &vq) == -ENOSPC);
    g_vdev.tr = &g_tr;
    g_max_size = 1024;
    EXPECT(virtq_alloc(&g_vdev, 0, 0, NULL, &vq) == 0 && vq != NULL && vq->size == VIRTQ_MAX_SIZE);
    virtq_free(vq);
}

/* --- the decoder of used elements (virtq_used; roadmap 2.0 SA5) --------- */

/* A recorded digest over seeded queues: records of chains in flight, free
 * heads and cleared cookies, used elements with every kind of id and
 * length, legacy and modern; then each confirmed chain reclaimed. Every
 * result is a line, the lines' FNV-1a digest recorded. VIRTQ_DUMP=1 prints
 * them; VIRTQ_BENCH=1 times the decoder alone. */
#define USED_ROUNDS 20000
#define USED_SEED 0xC05F5FA2026B0004ull
/* The digest of the C decoder (virtq_used.c, 2026-11-01). */
#define USED_DIGEST 0xb37c1a028a201a5aull

static uint64_t g_rng;
static uint64_t g_digest;
static bool g_dump;

static uint64_t next(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static void emit(const char *line, int n)
{
    EXPECT(n > 0 && n < 256);
    for (int i = 0; i < n; i++) {
        g_digest ^= (uint8_t)line[i];
        g_digest *= 0x100000001b3ull;
    }
    g_digest ^= '\n';
    g_digest *= 0x100000001b3ull;
    if (g_dump)
        printf("%s\n", line);
}

#define QMAX 256u
static uint16_t g_next[QMAX], g_chain[QMAX];
static uint32_t g_in[QMAX];
static void *g_cookie[QMAX];
static int g_token[QMAX];

/* A queue's records as the driver keeps them: links below its size, chains
 * of 1-4 descriptors or free, writable bytes near every bound. */
static void gen_queue(struct virtqueue *vq)
{
    memset(vq, 0, sizeof(*vq));
    vq->size = 1u << (next() % 9);   /* 1 .. 256 */
    vq->shadow_next = g_next;
    vq->chain_len = g_chain;
    vq->in_bytes = g_in;
    vq->cookies = g_cookie;
    static const uint32_t edge[] = { 0, 1, 511, 512, 4096, 65536, 0x7fffffff, 0xffffffffu };
    for (unsigned i = 0; i < vq->size; i++) {
        g_next[i] = (uint16_t)(next() % vq->size);
        g_chain[i] = (uint16_t)(next() % 3 ? 1 + next() % 4 : 0);
        g_in[i] = next() % 2 ? edge[next() % 8] : (uint32_t)(next() % 100000);
        g_cookie[i] = next() % 8 ? &g_token[i] : NULL;
    }
    vq->free_head = (uint16_t)(next() % vq->size);
    vq->num_free = (uint16_t)(next() % (vq->size + 1));
}

static uint64_t records_hash(const struct virtqueue *vq)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned i = 0; i < vq->size; i++) {
        uint64_t w = (uint64_t)g_next[i] | (uint64_t)g_chain[i] << 16 | (uint64_t)g_in[i] << 32;
        h = (h ^ w) * 0x100000001b3ull;
        h = (h ^ (g_cookie[i] == NULL ? 0u : (unsigned)(i + 1))) * 0x100000001b3ull;
    }
    return h;
}

static void run_used(void)
{
    static struct virtqueue vq;
    char line[128];
    gen_queue(&vq);
    for (unsigned k = 0; k < 4; k++) {
        uint32_t id;
        switch (next() % 4) {
        case 0: id = (uint32_t)next(); break;
        case 1: id = vq.size + (uint32_t)(next() % 4); break;
        default: id = (uint32_t)(next() % vq.size); break;
        }
        uint32_t in = id < vq.size ? g_in[id] : 0;
        uint32_t len;
        switch (next() % 5) {
        case 0: len = in; break;
        case 1: len = in + 1; break;
        case 2: len = in ? in - 1 : 0; break;
        case 3: len = 0; break;
        default: len = (uint32_t)next(); break;
        }
        bool legacy = next() % 3 == 0;
        uint32_t written = 0xa5a5a5a5u;
        int v = virtq_used_check(&vq, id, len, legacy, &written);
        int n = snprintf(line, sizeof(line), "u%d %08x", v, v == VIRTQ_USED_BAD_ID ? 0 : written);
        emit(line, n);
        if (v != VIRTQ_USED_BAD_ID) {
            virtq_reclaim(&vq, (uint16_t)id);
            n = snprintf(line, sizeof(line), "r%u %u %016llx", vq.free_head, vq.num_free,
                         (unsigned long long)records_hash(&vq));
            emit(line, n);
        }
    }
}

static void test_used_digest(void)
{
    g_dump = getenv("VIRTQ_DUMP") != NULL;
    g_digest = 0xcbf29ce484222325ull;
    g_rng = USED_SEED;
    for (unsigned r = 0; r < USED_ROUNDS; r++)
        run_used();
    printf("  virtq used digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, USED_ROUNDS);
    EXPECT(g_digest == USED_DIGEST);
}

static volatile uint64_t g_sink;

/* VIRTQ_BENCH=1: the check and the reclaim alone, on queues made beforehand (best of 15). */
static int bench_used(void)
{
    enum { N = 4096 };
    static struct virtqueue vq;
    struct elem { uint32_t id, len; bool legacy; } *es = malloc(sizeof(*es) * N);
    if (es == NULL)
        return 1;
    g_rng = USED_SEED;
    gen_queue(&vq);
    while (vq.size < 64)
        gen_queue(&vq);
    for (unsigned i = 0; i < N; i++) {
        es[i].id = (uint32_t)(next() % (vq.size + 2));
        es[i].len = (uint32_t)(next() % 70000);
        es[i].legacy = next() % 3 == 0;
    }
    uint16_t chain0[QMAX], next0[QMAX];
    uint32_t in0[QMAX];
    void *cookie0[QMAX];
    memcpy(chain0, g_chain, sizeof(chain0));
    memcpy(next0, g_next, sizeof(next0));
    memcpy(in0, g_in, sizeof(in0));
    memcpy(cookie0, g_cookie, sizeof(cookie0));
    double best = 1e30;
    for (int run = 0; run < 15; run++) {
        struct timespec t0, t1;
        uint64_t acc = 0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < N; i++) {
            uint32_t w = 0;
            int v = virtq_used_check(&vq, es[i].id, es[i].len, es[i].legacy, &w);
            acc += (uint64_t)v + w;
            if (v != VIRTQ_USED_BAD_ID) {
                virtq_reclaim(&vq, (uint16_t)es[i].id);
                g_chain[es[i].id] = chain0[es[i].id];   /* in flight again, for the next element */
                g_in[es[i].id] = in0[es[i].id];
                g_cookie[es[i].id] = cookie0[es[i].id];
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        g_sink += acc;
        memcpy(g_next, next0, sizeof(next0));
        double ns = (double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec);
        if (ns < best)
            best = ns;
    }
    printf("virtq used bench: %.2f ns per element (best of 15 runs of %u)\n", best / N, N);
    free(es);
    return 0;
}

static const struct host_test tests[] = {
    { "virtq-alloc-free", test_alloc_free },
    { "virtq-normal-chain", test_normal_chain },
    { "virtq-empty-oversized", test_empty_and_oversized },
    { "virtq-hostile-links", test_hostile_descriptor_links },
    { "virtq-hostile-used", test_hostile_used_ring },
    { "virtq-churn", test_churn },
    { "virtq-readonly-chain-length", test_readonly_chain_length },
    { "virtq-legacy-queue-size", test_legacy_queue_size },
    { "virtq-used-digest", test_used_digest },
};

int main(void)
{
    if (getenv("VIRTQ_BENCH") != NULL)
        return bench_used();
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
