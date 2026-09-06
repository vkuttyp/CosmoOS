/*
 * blktest.c - The block layer's pending queue and bio flags on the RAM
 * device (docs/kernel-services/filesystem/cosmofs/design.md, "The block
 * layer"). Debug builds.
 */

#include <kernel/blk.h>
#include <kernel/dma.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/ramblk.h>
#include <kernel/selftest.h>
#include <kernel/string.h>
#include <kernel/timer.h>
#include <kernel/wait.h>

#define STR_(x) #x
#define STR(x)  STR_(x)
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            *reason = "check failed: " #cond " at line " STR(__LINE__);        \
            return false;                                                      \
        }                                                                      \
    } while (0)

#if CONFIG_DEBUG

static volatile unsigned g_done_count;
static volatile int g_done_status;

static void count_done(struct bio *bio)
{
    if (bio->status)
        g_done_status = bio->status;
    __atomic_fetch_add(&g_done_count, 1u, __ATOMIC_SEQ_CST);
}

static void *volatile g_arg_seen;

static void arg_done(struct bio *bio)
{
    g_arg_seen = bio->arg;
    __atomic_fetch_add(&g_done_count, 1u, __ATOMIC_SEQ_CST);
}

bool selftest_blk_queue(const char **reason)
{
    struct blkdev *bd = ramblk_create(64);
    CHECK(bd != NULL);
    ramblk_set_deferred(bd, 2);   /* two in flight, the rest -EAGAIN at the driver */

    /* Eight writes at once: the layer queues six and every one completes. */
    enum { N = 8 };
    static struct bio bios[N];
    uint8_t *bufs = kmalloc((size_t)N * 4096, 0);   /* DMA-able: kmalloc, not the image's BSS */
    CHECK(bufs != NULL);
    g_done_count = 0;
    g_done_status = 0;
    uint64_t requeued0 = bd->requeued;
    /* Nothing completes while the eight are submitted, so the two slots
     * stay taken and the other six are refused and parked: the count is
     * exact instead of a race with the worker's millisecond. */
    ramblk_set_stall(bd, true);
    for (unsigned i = 0; i < N; i++) {
        memset(bufs + (size_t)i * 4096, (int)(0x10 + i), 4096);
        memset(&bios[i], 0, sizeof(bios[i]));
        bios[i].dev = bd;
        bios[i].dir = BIO_WRITE;
        bios[i].sector = (uint64_t)i * 8;
        bios[i].nsectors = 8;
        bios[i].buf = bufs + (size_t)i * 4096;
        bios[i].done = count_done;
        list_init(&bios[i].link);
        CHECK(blk_submit(&bios[i]) == 0);   /* never -EAGAIN */
    }
    CHECK(bd->requeued - requeued0 == N - 2);
    ramblk_set_stall(bd, false);
    uint64_t deadline = clock_now_ns() + 2000000000ULL;
    while (__atomic_load_n(&g_done_count, __ATOMIC_SEQ_CST) < N && clock_now_ns() < deadline)
        thread_sleep_ms(1);
    CHECK(g_done_count == N && g_done_status == 0);
    CHECK(bd->requeued - requeued0 == N - 2);
    uint8_t *back = kmalloc(4096, 0);
    CHECK(back != NULL);
    for (unsigned i = 0; i < N; i++) {
        CHECK(blk_read(bd, (uint64_t)i * 8, 8, back) == 0);
        CHECK(back[0] == 0x10 + i && back[4095] == 0x10 + i);   /* in order, none lost */
    }
    kfree(back);
    kfree(bufs);
    /* A flagged write in the deferred mode: flush, write, flush in the
     * recorded stream, one completion for the caller. */
    ramblk_record_start(bd, 16);
    uint8_t *page = kmalloc(4096, KMEM_ZERO);
    CHECK(page != NULL);
    memset(page, 0x77, 4096);
    CHECK(blk_write_flags(bd, 128, 8, page, BIO_PREFLUSH | BIO_FUA) == 0);
    struct ramblk_log *log = ramblk_record_stop(bd);
    CHECK(log != NULL && log->n == 3);
    CHECK(log->w[0].nsectors == 0 && log->w[1].nsectors == 8 && log->w[1].sector == 128 && log->w[2].nsectors == 0);
    ramblk_log_free(log);
    kfree(page);
    ramblk_set_deferred(bd, 0);

    /* The caller's `arg` survives a flagged write (the sequence borrows
     * the field and gives it back; Greptile on PR #22). */
    static int marker;
    struct bio flagged;
    memset(&flagged, 0, sizeof(flagged));
    flagged.dev = bd;
    flagged.dir = BIO_WRITE;
    flagged.sector = 144;
    flagged.nsectors = 8;
    flagged.buf = kmalloc(4096, KMEM_ZERO);
    CHECK(flagged.buf != NULL);
    flagged.flags = BIO_PREFLUSH | BIO_FUA;
    flagged.done = arg_done;
    flagged.arg = &marker;
    list_init(&flagged.link);
    g_arg_seen = NULL;
    g_done_count = 0;
    CHECK(blk_submit(&flagged) == 0);
    deadline = clock_now_ns() + 2000000000ULL;
    while (__atomic_load_n(&g_done_count, __ATOMIC_SEQ_CST) < 1 && clock_now_ns() < deadline)
        thread_sleep_ms(1);
    CHECK(g_done_count == 1 && g_arg_seen == &marker && flagged.arg == &marker && flagged.done == arg_done);
    kfree(flagged.buf);

    /* Synchronous mode: flags still mean flush, write, flush; a flagged
     * read or a flagged write past the end is refused before anything. */
    ramblk_record_start(bd, 16);
    page = kmalloc(4096, KMEM_ZERO);
    CHECK(page != NULL);
    CHECK(blk_write_flags(bd, 136, 8, page, BIO_FUA) == 0);
    log = ramblk_record_stop(bd);
    CHECK(log != NULL && log->n == 2 && log->w[0].nsectors == 8 && log->w[1].nsectors == 0);
    ramblk_log_free(log);
    struct bio bad;
    memset(&bad, 0, sizeof(bad));
    bad.dev = bd;
    bad.dir = BIO_READ;
    bad.sector = 0;
    bad.nsectors = 8;
    bad.buf = page;
    bad.flags = BIO_FUA;
    bad.done = count_done;
    CHECK(blk_submit(&bad) == -EINVAL);
    bad.dir = BIO_WRITE;
    bad.sector = bd->capacity;
    CHECK(blk_submit(&bad) == -EINVAL);
    kfree(page);
    ramblk_destroy(bd);
    kinfo("selftest: blk-queue: %llu bios waited in the pending queue; flagged writes are flush, write, flush",
          (unsigned long long)(bd->requeued - requeued0));
    return true;
}

/* Page-aligned, DMA-able test memory (kmalloc classes do not promise page alignment). */
static uint8_t *pages_alloc(unsigned n, dma_addr_t *dma)
{
    return dma_alloc(NULL, (size_t)n * PAGE_SIZE, dma, DMA_ZERO);
}

bool selftest_blk_segments(const char **reason)
{
    struct blkdev *bd = ramblk_create(64);
    CHECK(bd != NULL);
    CHECK(bd->max_segments == 8);
    dma_addr_t d1, d2;
    uint8_t *a = pages_alloc(2, &d1), *b = pages_alloc(2, &d2);
    CHECK(a != NULL && b != NULL);
    for (unsigned i = 0; i < 2 * PAGE_SIZE; i++) {
        a[i] = (uint8_t)(i * 3 + 1);
        b[i] = (uint8_t)(i * 5 + 2);
    }
    /* Three segments: half a page (from mid-page), a whole page, half a page: 4 KiB + 4 KiB = 16 sectors. */
    struct bio_vec vecs[3] = {
        { a + PAGE_SIZE / 2, (uint32_t)(PAGE_SIZE / 2) },
        { b, (uint32_t)PAGE_SIZE },
        { a + PAGE_SIZE, (uint32_t)(PAGE_SIZE / 2) },
    };
    struct bio bio;
    memset(&bio, 0, sizeof(bio));
    bio.dev = bd;
    bio.dir = BIO_WRITE;
    bio.sector = 16;
    bio.nsectors = 16;
    bio.vecs = vecs;
    bio.nr_vecs = 3;
    bio.done = count_done;
    g_done_count = 0;
    g_done_status = 0;
    CHECK(blk_submit(&bio) == 0);
    CHECK(g_done_count == 1 && g_done_status == 0);
    /* Read back flat and compare segment by segment. */
    dma_addr_t d3;
    uint8_t *flat = pages_alloc(2, &d3);
    CHECK(flat != NULL);
    CHECK(blk_read(bd, 16, 16, flat) == 0);
    CHECK(memcmp(flat, a + PAGE_SIZE / 2, PAGE_SIZE / 2) == 0);
    CHECK(memcmp(flat + PAGE_SIZE / 2, b, PAGE_SIZE) == 0);
    CHECK(memcmp(flat + PAGE_SIZE / 2 + PAGE_SIZE, a + PAGE_SIZE, PAGE_SIZE / 2) == 0);
    /* And back through segments in another shape (two whole pages). */
    memset(a, 0, 2 * PAGE_SIZE);
    struct bio_vec rd[2] = { { a, (uint32_t)PAGE_SIZE }, { a + PAGE_SIZE, (uint32_t)PAGE_SIZE } };
    bio.dir = BIO_READ;
    bio.vecs = rd;
    bio.nr_vecs = 2;
    g_done_count = 0;
    CHECK(blk_submit(&bio) == 0 && g_done_count == 1 && g_done_status == 0);
    CHECK(memcmp(a, flat, 2 * PAGE_SIZE) == 0);
    /* Refusals: a middle segment not ending on a page, a later one not
     * starting on one, a wrong total, too many segments, a stack buffer. */
    struct bio_vec bad1[2] = { { a, (uint32_t)(PAGE_SIZE / 2) }, { a + PAGE_SIZE, (uint32_t)(PAGE_SIZE + PAGE_SIZE / 2) } };
    bio.vecs = bad1;
    bio.nr_vecs = 2;
    CHECK(blk_submit(&bio) == -EINVAL);
    struct bio_vec bad2[2] = { { a, (uint32_t)PAGE_SIZE }, { a + PAGE_SIZE + 8, (uint32_t)(PAGE_SIZE - 8) } };
    bio.vecs = bad2;
    CHECK(blk_submit(&bio) == -EINVAL);
    struct bio_vec bad3[2] = { { a, (uint32_t)PAGE_SIZE }, { a + PAGE_SIZE, (uint32_t)(PAGE_SIZE / 2) } };
    bio.vecs = bad3;
    CHECK(blk_submit(&bio) == -EINVAL);   /* 12 KiB promised, 6 KiB given */
    struct bio_vec many[9];
    for (unsigned i = 0; i < 9; i++)
        many[i] = (struct bio_vec){ a, 0 };
    bio.vecs = many;
    bio.nr_vecs = 9;
    CHECK(blk_submit(&bio) == -EINVAL);
    uint8_t stackbuf[512];
    struct bio_vec bad4[1] = { { stackbuf, 512 } };
    bio.vecs = bad4;
    bio.nr_vecs = 1;
    bio.nsectors = 1;
    CHECK(blk_submit(&bio) == -EINVAL);
    dma_free(NULL, 2 * PAGE_SIZE, a, d1);
    dma_free(NULL, 2 * PAGE_SIZE, b, d2);
    dma_free(NULL, 2 * PAGE_SIZE, flat, d3);
    ramblk_destroy(bd);
    kinfo("selftest: blk-segments: three-segment write read back flat and in two pages; five refusals");
    return true;
}

bool selftest_blk_timeout(const char **reason)
{
    struct blkdev *bd = ramblk_create(16);
    CHECK(bd != NULL);
    CHECK(bd->timeout_ns == BLK_TIMEOUT_NS);
    bd->timeout_ns = 300ull * 1000000ull;   /* 300 ms for the test */
    ramblk_set_deferred(bd, 4);
    ramblk_set_stall(bd, true);              /* a silent device */
    uint8_t *page = kmalloc(4096, KMEM_ZERO);
    CHECK(page != NULL);
    uint64_t t0 = clock_now_ns();
    uint64_t timeouts0 = bd->timeouts;
    int rc = blk_read(bd, 0, 8, page);        /* would block for ever without the timeout */
    uint64_t took_ms = (clock_now_ns() - t0) / 1000000;
    CHECK(rc == -ETIMEDOUT);
    CHECK(bd->timeouts == timeouts0 + 1);
    CHECK(took_ms >= 300 && took_ms < 3000);
    /* The device recovers for the test's sake: a fresh request completes. */
    ramblk_set_stall(bd, false);
    CHECK(blk_read(bd, 0, 8, page) == 0);
    ramblk_set_deferred(bd, 0);
    kfree(page);
    ramblk_destroy(bd);
    kinfo("selftest: blk-timeout: a stalled request returned -ETIMEDOUT after %llu ms", (unsigned long long)took_ms);
    return true;
}

#else
bool selftest_blk_queue(const char **reason)
{
    (void)reason;
    return true;
}
bool selftest_blk_segments(const char **reason)
{
    (void)reason;
    return true;
}
bool selftest_blk_timeout(const char **reason)
{
    (void)reason;
    return true;
}
#endif
