/*
 * blktest.c - The block layer's pending queue and bio flags on the RAM
 * device (docs/kernel-services/filesystem/cosmofs/design.md, "The block
 * layer"). Debug builds.
 */

#include <kernel/blk.h>
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
    uint64_t deadline = clock_now_ns() + 2000000000ULL;
    while (__atomic_load_n(&g_done_count, __ATOMIC_SEQ_CST) < N && clock_now_ns() < deadline)
        thread_sleep_ms(1);
    CHECK(g_done_count == N && g_done_status == 0);
    CHECK(bd->requeued - requeued0 >= N - 2);
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

#else
bool selftest_blk_queue(const char **reason)
{
    (void)reason;
    return true;
}
#endif
