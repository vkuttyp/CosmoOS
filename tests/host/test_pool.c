/*
 * test_pool.c - The storage pool (kernel-services/storage/pool.c, kernel/include/kernel/storage.h) on the host:
 * roadmap 2.0 SA7. A digest recorded from the C over seeded rounds against block devices the test owns: pools opened
 * and closed, members and copies added, slots set, attached, swapped, replaced and detached, devices of every
 * geometry (sector sizes that do and do not divide the pool block, devices too small for the group), copy states
 * moved by the operator and by failures (the fault count, -ENODEV, the last ONLINE copy), the missed-region bitmaps
 * and holes, commits, resilvers, spares, the lookups, and the I/O -- reads, the copy reads and writes a repair does,
 * flagged writes, the root's ONLINE-only write and the flushes -- against devices that fail some of the time.
 * Allocations fail on request. Every return, every request a device sees, every reference taken and dropped, every
 * notification and every logged line are in the digest, with a dump of the pool's state.
 */

#include "harness.h"

#include <kernel/blk.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/object.h>
#include <kernel/storage.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The values the CeeSafe port writes as its own constants. */
_Static_assert(EINVAL == 22 && ENOSPC == 28 && EEXIST == 17 && ENODEV == 19 && EIO == 5 && ENOMEM == 12 &&
                   EAGAIN == 11 && EBUSY == 16 && ENOENT == 2 && BIO_PREFLUSH == 1 && BIO_FUA == 2,
               "pool's errno.h and blk.h values");

/* The digest of the C (pool.c, 2026-11-06), which the CeeSafe port gives. */
#define POOL_DIGEST 0xb210836b3a1c3655ull
#define ROUNDS 200000u
#define SEED 0xC05F5FA2026B0014ull

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

static void on_klog(int level, const char *line)
{
    note(1, (uint64_t)level);
    mix(line, strlen(line));
}

/* ---- the allocator: malloc, counted; kzalloc fails when g_fail_in counts down to it ---- */

static int g_fail_in = -1;
static long g_live;

void *kmalloc(size_t size, unsigned flags)
{
    if (g_fail_in >= 0 && g_fail_in-- == 0) {
        note(2, size);
        return NULL;
    }
    void *p = (flags & KMEM_ZERO) ? calloc(1, size) : malloc(size);
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

/* ---- the devices: a geometry each, a failure rate, a reference count ---- */

#define NDEV 12

struct tdev {
    struct blkdev bd;
    unsigned id;
    unsigned fail_pct;   /* of requests that fail */
};

static struct tdev g_dev[NDEV];

static struct tdev *tdev_of(const struct blkdev *bd)
{
    for (unsigned i = 0; i < NDEV; i++)
        if (bd == &g_dev[i].bd)
            return &g_dev[i];
    EXPECT(!"a device the test does not own");
    return &g_dev[0];
}

static uint64_t dev_id(const struct blkdev *bd) { return bd == NULL ? 0xff : tdev_of(bd)->id; }

void kobject_get(struct kobject *obj)
{
    EXPECT(obj->refcount > 0);
    obj->refcount++;
    note(3, dev_id((struct blkdev *)((char *)obj - __builtin_offsetof(struct blkdev, obj))));
}

void kobject_put(struct kobject *obj)
{
    EXPECT(obj->refcount > 1);   /* the test's own reference is never dropped */
    obj->refcount--;
    note(4, dev_id((struct blkdev *)((char *)obj - __builtin_offsetof(struct blkdev, obj))));
}

/* A new geometry, for a device no pool holds. */
static void reshape(struct tdev *d)
{
    static const uint32_t sizes[] = { 512, 4096, 512, 4096, 1024, 2048, 0, 8192, 3000 };
    unsigned s = pick(sizeof(sizes) / sizeof(sizes[0]));
    d->bd.sector_size = sizes[s];
    unsigned kind = pick(4);
    uint64_t blocks;
    if (kind == 0)
        blocks = 1 + pick(40);
    else if (kind == 1)
        blocks = 500 + pick(4000);
    else if (kind == 2)
        blocks = 7000 + pick(3000);
    else
        blocks = 1000000 + next() % 9000000;
    uint64_t spb = d->bd.sector_size != 0 && d->bd.sector_size <= 4096 ? 4096 / d->bd.sector_size : 1;
    uint64_t extra = pick(8);
    d->bd.capacity = blocks * spb + extra;
    unsigned f = pick(5);
    d->fail_pct = f == 0 ? 30 : f == 1 ? 5 : 0;
    d->bd.gone = false;
    note(5, d->id);
    note(6, d->bd.sector_size);
    note(7, d->bd.capacity);
}

static int draw_rc(struct tdev *d)
{
    unsigned r = pick(100);
    if (r >= d->fail_pct)
        return 0;
    unsigned k = pick(4);
    return k == 0 ? -ENODEV : k == 1 ? -ETIMEDOUT : -EIO;
}

int blk_read(struct blkdev *bd, uint64_t sector, uint32_t nsectors, void *buf)
{
    struct tdev *d = tdev_of(bd);
    note(10, d->id);
    note(11, sector);
    note(12, nsectors);
    int rc = draw_rc(d);
    if (rc == 0) {
        uint64_t v = ((uint64_t)d->id << 56) ^ sector;
        memcpy(buf, &v, sizeof(v));
    }
    return rc;
}

static int write_common(struct blkdev *bd, uint64_t sector, uint32_t nsectors, const void *buf, unsigned flags)
{
    struct tdev *d = tdev_of(bd);
    note(13, d->id);
    note(14, sector);
    note(15, nsectors);
    note(16, flags);
    uint64_t v;
    memcpy(&v, buf, sizeof(v));
    note(17, v);
    return draw_rc(d);
}

int blk_write(struct blkdev *bd, uint64_t sector, uint32_t nsectors, const void *buf)
{
    return write_common(bd, sector, nsectors, buf, 0x100);
}

int blk_write_flags(struct blkdev *bd, uint64_t sector, uint32_t nsectors, const void *buf, unsigned flags)
{
    return write_common(bd, sector, nsectors, buf, flags);
}

int blk_flush(struct blkdev *bd)
{
    struct tdev *d = tdev_of(bd);
    note(18, d->id);
    return draw_rc(d);
}

/* ---- the pools ---- */

#define NPOOL 3

static struct spool *g_pool[NPOOL];
static uint8_t g_buf[POOL_BLOCK];

static void notified(void *arg) { note(20, (uintptr_t)arg); }

static struct blkdev *draw_dev(void)
{
    unsigned i = pick(NDEV + 1);
    return i == NDEV ? NULL : &g_dev[i].bd;
}

static unsigned draw_vdev(const struct spool *p)
{
    unsigned k = pick(20);
    if (k == 0)
        return p->nmembers;
    if (k == 1)
        return 255;
    if (k == 2)
        return 0x7fffffffu;
    return pick(p->nmembers);
}

static unsigned draw_copy(void) { return pick(POOL_MAX_COPIES + 1); }

static enum pool_copy_state draw_state(void) { return (enum pool_copy_state)pick(POOL_COPY_NSTATES + 1); }

static uint64_t draw_dva(const struct spool *p)
{
    unsigned v = draw_vdev(p);
    uint64_t n = v < p->nmembers ? p->m[v].nblocks : 100;
    unsigned k = pick(10);
    uint64_t b;
    if (k == 0)
        b = n;
    else if (k == 1)
        b = POOL_DVA_BLK_MASK;
    else if (k < 5)
        b = pick(n < 64 ? (unsigned)n + 1 : 64);
    else
        b = next() % (n + 1);
    return POOL_DVA(v & 0xff, b);
}

static uint64_t draw_region(void)
{
    unsigned k = pick(10);
    if (k == 0)
        return POOL_DIRTY_BITS;
    if (k == 1)
        return POOL_DIRTY_BITS - 1;
    if (k < 5)
        return pick(16);
    return pick(POOL_DIRTY_BITS);
}

static void mix_bits(const uint8_t *bits)
{
    if (bits == NULL) {
        note(30, 0);
        return;
    }
    note(31, 1);
    mix(bits, POOL_DIRTY_BYTES);
}

static void dump(const struct spool *p)
{
    note(32, p->nmembers);
    note(33, p->block_size);
    note(34, p->nblocks);
    note(35, p->reads);
    note(36, p->writes);
    note(37, p->flushes);
    note(38, p->epoch);
    note(39, p->state_seq);
    for (unsigned v = 0; v < p->nmembers; v++) {
        const struct spool_member *m = &p->m[v];
        note(40, m->ncopies);
        note(41, m->nblocks);
        note(42, m->region_blocks);
        mix_bits(m->recent);
        for (unsigned c = 0; c < m->ncopies; c++) {
            const struct pool_copy *cp = &m->copy[c];
            note(43, dev_id(cp->dev));
            note(44, cp->sectors_per_block);
            note(45, cp->state);
            note(46, cp->whole);
            note(47, cp->since);
            note(48, cp->cursor);
            note(49, cp->read_errors);
            note(50, cp->write_errors);
            note(51, cp->csum_errors);
            note(52, cp->tag);
            note(53, cp->peer);
            mix_bits(cp->dirty);
        }
    }
    note(54, p->nspares);
    for (unsigned i = 0; i < p->nspares; i++) {
        note(55, dev_id(p->spare[i]));
        note(56, p->spare_tag[i]);
    }
}

static void open_one(unsigned i)
{
    struct blkdev *bd = draw_dev();
    if (pick(20) == 0)
        g_fail_in = (int)pick(2);
    struct spool *p = NULL;
    int rc = pool_open(bd, &p);
    g_fail_in = -1;
    note(60, (uint64_t)(int64_t)rc);
    if (rc != 0) {
        EXPECT(p == NULL);
        return;
    }
    if (pick(2) == 0)
        p->name = i == 0 ? "tank" : "data";
    if (pick(3) != 0) {
        p->notify = notified;
        p->notify_arg = (void *)(uintptr_t)(i + 1);
    }
    g_pool[i] = p;
    dump(p);
}

static void close_one(unsigned i)
{
    dump(g_pool[i]);
    pool_close(g_pool[i]);
    g_pool[i] = NULL;
    note(61, i);
}

static void fill_buf(void)
{
    uint64_t v = next();
    memcpy(g_buf, &v, sizeof(v));
}

static void io(struct spool *p)
{
    uint64_t dva = draw_dva(p);
    unsigned what = pick(8);
    int rc;
    if (what == 0) {
        rc = pool_read(p, dva, g_buf);
        uint64_t v;
        memcpy(&v, g_buf, sizeof(v));
        note(70, rc == 0 ? v : 0);
    } else if (what == 1) {
        unsigned c = draw_copy();
        rc = pool_read_copy(p, dva, c, g_buf);
    } else if (what == 2) {
        unsigned c = draw_copy();
        fill_buf();
        rc = pool_write_copy(p, dva, c, g_buf);
    } else if (what == 3) {
        unsigned k = pick(6);
        unsigned flags = k == 0   ? BIO_PREFLUSH
                         : k == 1 ? BIO_FUA
                         : k == 2 ? BIO_PREFLUSH | BIO_FUA
                         : k == 3 ? POOL_WRITE_ONLINE | BIO_PREFLUSH | BIO_FUA
                         : k == 4 ? POOL_WRITE_ONLINE
                                  : 0;
        fill_buf();
        rc = pool_write_flags(p, dva, g_buf, flags);
    } else if (what == 4) {
        rc = pool_flush(p);
    } else {
        fill_buf();
        rc = pool_write(p, dva, g_buf);
    }
    note(71, what);
    note(72, (uint64_t)(int64_t)rc);
}

static void queries(struct spool *p)
{
    uint64_t dva = draw_dva(p);
    unsigned v = draw_vdev(p);
    unsigned c = draw_copy();
    uint64_t r = draw_region();
    note(80, pool_copies(p, dva));
    note(81, pool_copy_readable(p, dva, c));
    note(82, pool_copy_online(p, v, c));
    note(83, pool_member_degraded(p, v));
    note(84, pool_degraded(p));
    note(85, pool_copy_missed(p, dva, c));
    note(86, pool_region_missed(p, v, c, r));
    note(87, pool_copy_any_missed(p, v, c));
    enum pool_copy_state s = draw_state();
    const char *name = pool_copy_state_name(s);
    mix(name, strlen(name));
    struct blkdev *bd = draw_dev();
    unsigned fv = 0xaaaa, fc = 0xbbbb;
    bool found = pool_find_dev(p, bd, &fv, &fc);
    note(88, found);
    note(89, fv);
    note(90, fc);
}

static void spare_op(struct spool *p)
{
    unsigned k = pick(3);
    if (k == 0) {
        struct blkdev *bd = draw_dev();
        uint64_t tag = next();
        note(100, (uint64_t)(int64_t)pool_add_spare(p, bd, tag));
    } else if (k == 1) {
        struct blkdev *bd = draw_dev();
        note(101, (uint64_t)(int64_t)pool_remove_spare(p, bd));
    } else {
        unsigned v = draw_vdev(p);
        uint64_t tag = 0x1234;
        uint64_t *tp = pick(4) == 0 ? NULL : &tag;
        struct blkdev *bd = pool_take_spare(p, v, tp);
        note(102, dev_id(bd));
        note(103, tag);
        if (bd == NULL)
            return;
        /* What cosmofs does with one: put it in a failed copy's place, or give it back. */
        if (pick(2) == 0 && v < p->nmembers) {
            unsigned c = pick(p->m[v].ncopies);
            enum pool_copy_state st = pick(4) == 0 ? POOL_COPY_ONLINE : POOL_COPY_RESILVERING;
            note(104, (uint64_t)(int64_t)pool_copy_replace_dev(p, v, c, bd, st));
        }
        blkdev_put(bd);
    }
}

static void member_op(struct spool *p)
{
    unsigned k = pick(12);
    unsigned v = draw_vdev(p);
    unsigned c = draw_copy();
    int rc = 0;
    if (pick(25) == 0)
        g_fail_in = (int)pick(5);
    if (k == 0) {
        if (pick(300) == 0) {
            /* As many as a DVA can name, and one more. */
            for (unsigned d = 0; d < NDEV && rc == 0; d++)
                if (g_dev[d].bd.sector_size == 512 || g_dev[d].bd.sector_size == 4096)
                    while (rc == 0)
                        rc = pool_add_member(p, &g_dev[d].bd, NULL);
            note(113, p->nmembers);
        } else if (p->nmembers < 40) {
            struct blkdev *bd = draw_dev();
            unsigned out = 0xcccc;
            unsigned *op = pick(4) == 0 ? NULL : &out;
            rc = pool_add_member(p, bd, op);
            note(110, out);
        }
    } else if (k == 1) {
        struct blkdev *bd = draw_dev();
        rc = pool_add_copy(p, v, bd);
    } else if (k == 2) {
        unsigned n = pick(POOL_MAX_COPIES + 2);
        rc = pool_set_copies(p, v, n);
    } else if (k == 3) {
        struct blkdev *bd = draw_dev();
        enum pool_copy_state s = draw_state();
        rc = pool_attach_copy(p, v, c, bd, s);
    } else if (k == 4) {
        unsigned b = draw_copy();
        pool_swap_copies(p, v, c, b);
    } else if (k == 5) {
        uint64_t n = v < p->nmembers ? p->m[v].nblocks : 10;
        unsigned how = pick(3);
        uint64_t nb = how == 0 ? n + 1 : how == 1 ? 0 : 1 + next() % n;
        pool_set_member_blocks(p, v, nb);
    } else if (k == 6 || k == 7) {
        enum pool_copy_state s = draw_state();
        unsigned w = pick(3);
        int why = w == 0 ? 0 : w == 1 ? -EIO : -ENODEV;
        rc = pool_copy_set_state(p, v, c, s, why);
    } else if (k == 8) {
        rc = pool_copy_resilvered(p, v, c);
    } else if (k == 9) {
        struct blkdev *bd = draw_dev();
        enum pool_copy_state s = draw_state();
        rc = pool_copy_replace_dev(p, v, c, bd, s);
    } else if (k == 10) {
        rc = pool_detach_copy(p, v, c);
    } else {
        /* The fields the filesystem keeps in the copy record. */
        if (v < p->nmembers && c < p->m[v].ncopies) {
            struct pool_copy *cp = &p->m[v].copy[c];
            unsigned f = pick(3);
            uint64_t val = pick(4) == 0 ? 0 : next();
            if (f == 0)
                cp->cursor = val;
            else if (f == 1)
                cp->tag = val;
            else
                cp->peer = val;
        }
    }
    g_fail_in = -1;
    note(111, k);
    note(112, (uint64_t)(int64_t)rc);
}

static void bitmap_op(struct spool *p)
{
    unsigned k = pick(4);
    if (k == 0) {
        uint64_t dva = draw_dva(p);
        pool_mark_missed(p, dva);
    } else if (k == 1) {
        unsigned v = draw_vdev(p);
        unsigned c = draw_copy();
        uint64_t r = draw_region();
        pool_region_clear(p, v, c, r);
    } else if (k == 2) {
        uint64_t dva = draw_dva(p);
        unsigned c = draw_copy();
        pool_copy_csum_error(p, dva, c);
    } else {
        uint64_t gen = pick(3) == 0 ? p->epoch : p->epoch + 1 + pick(3);
        pool_commit_done(p, gen);
    }
    note(120, k);
}

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    harness_klog_hook = on_klog;
    harness_klog_min = KLOG_ERROR + 1;   /* the copy state changes' warnings are expected, by the thousand */
    for (unsigned i = 0; i < NDEV; i++) {
        g_dev[i].id = i;
        g_dev[i].bd.obj.refcount = 1;   /* the test's */
        snprintf(g_dev[i].bd.name, sizeof(g_dev[i].bd.name), "vd%c", 'a' + i);
        reshape(&g_dev[i]);
    }
    for (unsigned round = 0; round < ROUNDS; round++) {
        if (round % 5000 == 0) {
            static const uint32_t limits[] = { 16, 0, 1, 3 };
            g_pool_fault_errors = limits[pick(4)];
            note(130, g_pool_fault_errors);
        }
        unsigned i = pick(NPOOL);
        struct spool *p = g_pool[i];
        unsigned what = pick(100);
        if (p == NULL) {
            open_one(i);
        } else if (what < 1) {
            close_one(i);
        } else if (what < 41) {
            io(p);
        } else if (what < 56) {
            queries(p);
        } else if (what < 80) {
            member_op(p);
        } else if (what < 90) {
            bitmap_op(p);
        } else if (what < 94) {
            spare_op(p);
        } else if (what < 97) {
            dump(p);
        } else {
            /* A device comes and goes: gone, back, or (held by nobody) reshaped. */
            struct tdev *d = &g_dev[pick(NDEV)];
            if (d->bd.obj.refcount == 1 && pick(2) == 0)
                reshape(d);
            else
                d->bd.gone = !d->bd.gone;
            note(131, d->id);
        }
    }
    for (unsigned i = 0; i < NPOOL; i++)
        if (g_pool[i] != NULL)
            close_one(i);
    for (unsigned i = 0; i < NDEV; i++)
        EXPECT(g_dev[i].bd.obj.refcount == 1);
    EXPECT(g_live == 0);
    harness_klog_hook = NULL;
    harness_klog_min = KLOG_WARN;
    printf("  pool digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, ROUNDS);
    EXPECT(g_digest == POOL_DIGEST);
}

int main(void)
{
    static const struct host_test tests[] = {
        { "pool_digest", test_digest },
    };
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
