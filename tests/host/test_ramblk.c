/*
 * test_ramblk.c - The RAM block device (kernel/block/ramblk.cee,
 * kernel/include/kernel/ramblk.h) on the host: roadmap 2.0 SA7. A digest
 * recorded from the C over seeded rounds: devices created (and refused
 * when memory or the registry fails), bios of one buffer or several
 * segments read and written across its 4 KiB blocks, flushes, the failing
 * modes (every bio, writes alone), the deferred mode with its limit, its
 * worker's batches, a stall, refusals that first complete the oldest bio,
 * completions the test makes and the timeout operation; the write
 * recorder (its log, the dropped entries, replay whole or torn) and
 * snapshots restored. Every completion with its status and the bytes a
 * read returned, every registration and return is in the digest. The
 * block layer, the threads and the allocator are the test's; the worker
 * runs a batch at a time, its sleep returning to the test.
 */

#include "harness.h"

#include <kernel/blk.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/object.h>
#include <kernel/ramblk.h>
#include <kernel/thread.h>
#include <kernel/wait.h>

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The values ramblk.cee writes as its own constants. */
_Static_assert(EAGAIN == 11 && ETIMEDOUT == 110 && BIO_READ == 0 && BIO_WRITE == 1 && BIO_FLUSH == 2 &&
                   RAMBLK_BLOCK == 4096 && KMEM_ZERO == 1 && SCHED_PRIO_DEFAULT == 32,
               "ramblk.cee's errno.h, blk.h, ramblk.h, kmalloc.h and thread.h values");

/* The digest of the C (ramblk.c, 2026-11-05), which the CeeSafe port gives. */
#define RAMBLK_DIGEST 0x3e265dd7cda910d8ull
#define ROUNDS 30000u
#define SEED 0xC05F5FA2026B0011ull

static uint64_t g_rng, g_digest;

static uint64_t next(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static unsigned pick(unsigned n) { return (unsigned)(next() % n); }

static void mix(const void *bytes, size_t n)
{
    const uint8_t *b = bytes;
    for (size_t i = 0; i < n; i++) {
        g_digest ^= b[i];
        g_digest *= 0x100000001b3ull;
    }
}

static void note(uint32_t which, uint64_t value)
{
    mix(&which, sizeof(which));
    mix(&value, sizeof(value));
}

/* ---- the allocator: malloc, counted; an allocation fails when g_fail_in counts down to it ---- */

static int g_fail_in = -1;
static long g_live;

void *kmalloc(size_t size, unsigned flags)
{
    if (g_fail_in >= 0 && g_fail_in-- == 0) {
        note(1, 0);
        return NULL;
    }
    void *p = (flags & KMEM_ZERO) ? calloc(1, size ? size : 1) : malloc(size ? size : 1);
    if (p != NULL)
        g_live++;
    return p;
}

void *kzalloc(size_t size) { return kmalloc(size, KMEM_ZERO); }

void kfree(void *p)
{
    if (p != NULL)
        g_live--;
    free(p);
}

/* ---- kobjects: a plain count, released through the blkdev type ---- */

void kobject_init(struct kobject *obj, const struct kobject_type *type)
{
    memset(obj, 0, sizeof(*obj));
    obj->type = type;
    obj->refcount = 1;
}

void kobject_get(struct kobject *obj)
{
    EXPECT(obj->refcount > 0);
    obj->refcount++;
}

void kobject_put(struct kobject *obj)
{
    EXPECT(obj->refcount > 0);
    if (--obj->refcount == 0)
        obj->type->release(obj);
}

static unsigned g_released;

static void blkdev_released(struct kobject *obj)
{
    struct blkdev *bd = (struct blkdev *)((char *)obj - __builtin_offsetof(struct blkdev, obj));
    g_released++;
    note(2, g_released);
    bd->ops->release(bd);
}

static const struct kobject_type blkdev_type = { .name = "blkdev", .release = blkdev_released };

/* ---- the block layer: registration, segments, completion ---- */

static unsigned g_letter;
static int g_fail_register_in = -1;

int blk_register(struct blkdev *bd, const char *prefix)
{
    mix(prefix, strlen(prefix));
    note(3, bd->sector_size);
    note(4, bd->capacity);
    note(5, bd->max_sectors);
    note(6, bd->max_segments);
    note(7, bd->ops != NULL && bd->ops->submit != NULL && bd->ops->release != NULL && bd->ops->timeout != NULL &&
                bd->ops->debug_dma == NULL && bd->ops->debug_presence == NULL);
    note(8, bd->priv != NULL);
    if (g_fail_register_in >= 0 && g_fail_register_in-- == 0) {
        note(9, 0);
        return -ENOSPC;
    }
    kobject_init(&bd->obj, &blkdev_type);
    snprintf(bd->name, sizeof(bd->name), "%s%c", prefix, 'a' + (int)(g_letter++ % 26));
    kobject_get(&bd->obj);   /* the registry's */
    return 0;
}

void blk_unregister(struct blkdev *bd)
{
    mix(bd->name, strlen(bd->name));
    bd->gone = true;
    kobject_put(&bd->obj);
}

void bio_segment(const struct bio *bio, unsigned i, struct bio_vec *out)
{
    if (bio->nr_vecs == 0) {
        EXPECT(i == 0);
        out->buf = bio->buf;
        out->len = bio->nsectors * bio->dev->sector_size;
        return;
    }
    EXPECT(i < bio->nr_vecs);
    *out = bio->vecs[i];
}

/* The test's bios: a pool, each with room for its data and its segments. */
#define BIOS 24
#define BIO_SECTORS_MAX 24
struct tbio {
    struct bio bio;
    struct bio_vec vecs[4];
    uint8_t data[BIO_SECTORS_MAX * 512];
    bool busy;   /* handed to the driver, not completed */
};
static struct tbio g_bios[BIOS];
static unsigned g_completed;

static void tbio_done(struct bio *bio)
{
    struct tbio *t = (struct tbio *)bio;
    EXPECT(t->busy);
    t->busy = false;
    g_completed++;
    note(10, (uint64_t)(t - g_bios));
    note(11, (uint64_t)(int64_t)bio->status);
    if (bio->status == 0 && bio->dir == BIO_READ)
        mix(t->data, (size_t)bio->nsectors * 512);
}

void bio_complete(struct bio *bio, int status)
{
    bio->status = status;
    bio->done(bio);
}

/* ---- threads: the worker runs a batch when the test ticks it; its sleep returns to the test ---- */

#define THREADS 8
static struct worker {
    void (*entry)(void *);
    void *arg;
    bool live;
} g_workers[THREADS];
static jmp_buf g_sleep_jmp;
static bool g_in_worker, g_joining;
static int g_thread_fail_pct;

struct thread *thread_create(void (*entry)(void *arg), void *arg, const char *name, int priority)
{
    mix(name, strlen(name));
    note(12, (uint64_t)priority);
    if ((int)pick(100) < g_thread_fail_pct) {
        note(13, 0);
        return NULL;
    }
    for (unsigned i = 0; i < THREADS; i++)
        if (!g_workers[i].live) {
            g_workers[i] = (struct worker){ entry, arg, true };
            return (struct thread *)&g_workers[i];
        }
    EXPECT(!"no worker slot");
    return NULL;
}

/* One batch: the worker runs until it sleeps. */
static void tick(struct worker *w)
{
    g_in_worker = true;
    if (setjmp(g_sleep_jmp) == 0) {
        w->entry(w->arg);
        EXPECT(!"the worker returned while not stopping");
    }
    g_in_worker = false;
}

void thread_sleep_ns(uint64_t ns)
{
    note(14, ns);
    EXPECT(g_in_worker && !g_joining);
    longjmp(g_sleep_jmp, 1);
}

int thread_join(struct thread *t)
{
    struct worker *w = (struct worker *)t;
    EXPECT(w->live);
    note(15, 0);
    g_joining = true;
    g_in_worker = true;
    w->entry(w->arg);   /* stopping: it drains and returns */
    g_in_worker = false;
    g_joining = false;
    w->live = false;
    return 0;
}

/* ---- the test ---- */

#define DEVS 3
static struct dev {
    struct blkdev *bd;
    uint64_t nblocks;
    uint8_t *snap;               /* a snapshot of it, or NULL */
    struct ramblk_log *log;      /* a stopped recording, or NULL */
    bool recording, deferred;
} g_devs[DEVS];

static void fill(void *p, size_t n)
{
    uint8_t *b = p;
    for (size_t i = 0; i < n; i++)
        b[i] = (uint8_t)next();
}

static void create(struct dev *d)
{
    uint64_t nblocks = 1 + pick(6);
    unsigned f = pick(12);
    g_fail_in = f == 0 ? (int)pick((unsigned)nblocks + 3) : -1;
    g_fail_register_in = f == 1 ? 0 : -1;
    struct blkdev *bd = ramblk_create(nblocks);
    g_fail_in = g_fail_register_in = -1;
    note(16, bd != NULL);
    if (bd == NULL)
        return;
    mix(bd->name, strlen(bd->name));
    d->bd = bd;
    d->nblocks = nblocks;
}

static void destroy(struct dev *d)
{
    note(17, 0);
    if (d->recording)
        ramblk_log_free(ramblk_record_stop(d->bd));
    struct blkdev *bd = d->bd;
    ramblk_destroy(bd);
    for (unsigned i = 0; i < BIOS; i++)
        EXPECT(!g_bios[i].busy || g_bios[i].bio.dev != bd);   /* everything it held completed */
    ramblk_log_free(d->log);
    kfree(d->snap);
    memset(d, 0, sizeof(*d));
}

static struct tbio *free_bio(void)
{
    unsigned start = pick(BIOS);
    for (unsigned i = 0; i < BIOS; i++)
        if (!g_bios[(start + i) % BIOS].busy)
            return &g_bios[(start + i) % BIOS];
    return NULL;
}

static void submit(struct dev *d)
{
    struct tbio *t = free_bio();
    if (t == NULL)
        return;
    struct bio *b = &t->bio;
    memset(b, 0, sizeof(*b));
    b->dev = d->bd;
    unsigned dir = pick(7);
    b->dir = dir < 3 ? BIO_READ : dir < 6 ? BIO_WRITE : BIO_FLUSH;
    b->done = tbio_done;
    list_init(&b->link);
    if (b->dir != BIO_FLUSH) {
        uint64_t cap = d->bd->capacity;
        uint32_t n = 1 + pick(BIO_SECTORS_MAX);
        if (n > cap)
            n = (uint32_t)cap;
        b->nsectors = n;
        b->sector = pick((unsigned)(cap - n + 1));
        if (b->dir == BIO_WRITE)
            fill(t->data, (size_t)n * 512);
        else
            memset(t->data, 0xa5, (size_t)n * 512);
        unsigned segs = pick(3) == 0 ? 0 : 1 + pick(n < 4 ? n : 4);
        if (segs == 0) {
            b->buf = t->data;
        } else {
            /* Segments of whole sectors, the last taking what is left. */
            uint32_t left = n, at = 0;
            for (unsigned i = 0; i < segs; i++) {
                uint32_t take = i == segs - 1 ? left : 1 + pick(left - (segs - 1 - i));
                t->vecs[i].buf = t->data + (size_t)at * 512;
                t->vecs[i].len = take * 512;
                at += take;
                left -= take;
            }
            b->vecs = t->vecs;
            b->nr_vecs = segs;
        }
    }
    note(18, b->dir);
    note(19, b->sector);
    note(20, b->nsectors);
    note(21, b->nr_vecs);
    t->busy = true;
    g_fail_in = pick(20) == 0 ? 0 : -1;   /* the recorder's copy of a write */
    int rc = d->bd->ops->submit(d->bd, b);
    g_fail_in = -1;
    note(22, (uint64_t)(int64_t)rc);
    if (rc != 0) {
        EXPECT(rc == -EAGAIN);
        t->busy = false;
    }
}

/* The device's worker: its argument is the device's own state, the blkdev's priv. */
static struct worker *worker_of(struct dev *d)
{
    for (unsigned i = 0; i < THREADS; i++)
        if (g_workers[i].live && g_workers[i].arg == d->bd->priv)
            return &g_workers[i];
    return NULL;
}

static void step(void)
{
    struct dev *d = &g_devs[pick(DEVS)];
    if (d->bd == NULL) {
        create(d);
        return;
    }
    unsigned what = pick(100);
    if (what < 40) {
        submit(d);
    } else if (what < 46) {
        struct worker *w = worker_of(d);
        if (w != NULL)
            tick(w);
    } else if (what < 50) {
        note(23, ramblk_complete_one(d->bd));
    } else if (what < 53) {
        struct tbio *t = &g_bios[pick(BIOS)];
        note(24, t->busy);
        d->bd->ops->timeout(d->bd, &t->bio);
    } else if (what < 58) {
        unsigned limit = pick(3) == 0 ? 0 : 1 + pick(4);
        note(25, limit);
        ramblk_set_deferred(d->bd, limit);
    } else if (what < 61) {
        ramblk_set_stall(d->bd, pick(2));
    } else if (what < 63) {
        ramblk_set_fail(d->bd, pick(3) == 0 ? -EIO : 0);
    } else if (what < 66) {
        ramblk_set_fail_writes(d->bd, pick(3) == 0 ? -ENOSPC : 0);
    } else if (what < 69) {
        ramblk_set_refuse_completes(d->bd, pick(2));
    } else if (what < 75) {
        if (!d->recording) {
            unsigned cap = pick(12);
            g_fail_in = pick(10) == 0 ? (int)pick(2) : -1;
            ramblk_record_start(d->bd, cap);
            g_fail_in = -1;
            d->recording = true;   /* a start whose allocation failed records nothing: its stop hands back NULL */
            note(26, cap);
        } else {
            note(27, ramblk_record_count(d->bd));
            if (pick(2)) {
                struct ramblk_log *log = ramblk_record_stop(d->bd);
                d->recording = false;
                note(28, log != NULL);
                if (log != NULL) {
                    note(29, log->n);
                    note(30, log->cap);
                    note(31, log->dropped);
                    for (unsigned i = 0; i < log->n; i++) {
                        note(32, log->w[i].sector);
                        note(33, log->w[i].nsectors);
                        if (log->w[i].data != NULL)
                            mix(log->w[i].data, (size_t)log->w[i].nsectors * 512);
                    }
                }
                ramblk_log_free(d->log);
                d->log = log;
            }
        }
    } else if (what < 80) {
        if (d->log != NULL) {
            unsigned count = pick(d->log->n + 3);
            bool torn = pick(2);
            note(34, count);
            note(35, torn);
            ramblk_replay(d->bd, d->log, count, torn);
        }
    } else if (what < 86) {
        g_fail_in = pick(8) == 0 ? 0 : -1;
        uint8_t *img = ramblk_snapshot(d->bd);
        g_fail_in = -1;
        note(36, img != NULL);
        if (img != NULL) {
            mix(img, (size_t)d->nblocks * RAMBLK_BLOCK);
            kfree(d->snap);
            d->snap = img;
        }
    } else if (what < 90) {
        if (d->snap != NULL)
            ramblk_restore(d->bd, d->snap);
    } else if (what < 93) {
        g_thread_fail_pct = (int)pick(40);
    } else if (what < 95) {
        ramblk_log_free(NULL);   /* a log of none */
    } else if (what < 97) {
        destroy(d);
    } else {
        note(37, ramblk_record_count(d->bd));
    }
}

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    g_thread_fail_pct = 10;
    for (unsigned r = 0; r < ROUNDS; r++)
        step();
    for (unsigned i = 0; i < DEVS; i++)
        if (g_devs[i].bd != NULL)
            destroy(&g_devs[i]);
    for (unsigned i = 0; i < BIOS; i++)
        EXPECT(!g_bios[i].busy);
    for (unsigned i = 0; i < THREADS; i++)
        EXPECT(!g_workers[i].live);
    note(38, g_completed);
    printf("  ramblk digest 0x%016llx over %u rounds: %u completions, %u devices released\n",
           (unsigned long long)g_digest, ROUNDS, g_completed, g_released);
    EXPECT(g_live == 0);
    EXPECT(g_digest == RAMBLK_DIGEST);
}

static const struct host_test tests[] = {
    { "ramblk digest", test_digest },
};

int main(void)
{
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
