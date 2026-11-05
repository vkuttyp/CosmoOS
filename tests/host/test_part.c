/*
 * test_part.c - Partitions as block devices (kernel/block/part.c,
 * kernel/include/kernel/part.h) on the host: roadmap 2.0 SA7. A digest
 * recorded from the C over seeded rounds against disks the test owns:
 * scans of every outcome (a table, none, a refused one, a read error, a
 * partition whose name does not fit, registrations and allocations
 * failing), rescans while a partition is held, bios forwarded with their
 * translation and completed now, later or refused, the removal's wait for
 * forwarded bios (the reap thread, and the inline wait when no thread
 * starts), and the lookups (partuuid, a sibling by type, info, the disk).
 * Every registration, forwarded bio, completion, logged line and return
 * is in the digest. The block layer, the GPT parser, the threads and the
 * allocator are the test's: gpt_parse hands back tables the test draws.
 */

#include "harness.h"

#include <kernel/blk.h>
#include <kernel/errno.h>
#include <kernel/gpt.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/mutex.h>
#include <kernel/object.h>
#include <kernel/part.h>
#include <kernel/thread.h>
#include <kernel/wait.h>

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The digest of the C (part.c, 2026-11-05), which the CeeSafe port gives. */
#define PART_DIGEST 0x003796d5fcf97df9ull
#define ROUNDS 20000u
#define SEED 0xC05F5FA2026B0007ull

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

size_t (strlcpy)(char *dst, const char *src, size_t size)   /* parenthesised: a fortified libc makes strlcpy a macro */
{
    size_t n = strlen(src);
    if (size > 0) {
        size_t k = n < size - 1 ? n : size - 1;
        memcpy(dst, src, k);
        dst[k] = '\0';
    }
    return n;
}

/* ---- the allocator: malloc, counted; kmalloc fails when g_fail_in counts down to it ---- */

static int g_fail_in = -1;
static long g_live;

void *kmalloc(size_t size, unsigned flags)
{
    if (g_fail_in >= 0 && g_fail_in-- == 0) {
        note(2, 0);
        return NULL;
    }
    void *p = (flags & KMEM_ZERO) ? calloc(1, size) : malloc(size);
    if (p != NULL)
        g_live++;
    return p;
}

void kfree(void *p)
{
    if (p != NULL)
        g_live--;
    free(p);
}

/* ---- the one mutex part.c has ---- */

static struct mutex *g_mutex;
static bool g_mutex_held;

void mutex_init(struct mutex *m, const char *name)
{
    g_mutex = m;
    mix(name, strlen(name));
}

void mutex_lock(struct mutex *m)
{
    EXPECT(m == g_mutex && !g_mutex_held);
    g_mutex_held = true;
}

void mutex_unlock(struct mutex *m)
{
    EXPECT(m == g_mutex && g_mutex_held);
    g_mutex_held = false;
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

uint32_t kobject_refcount(const struct kobject *obj) { return obj->refcount; }

static unsigned g_released_disks, g_released_parts;

static void blkdev_released(struct kobject *obj)
{
    struct blkdev *bd = (struct blkdev *)((char *)obj - __builtin_offsetof(struct blkdev, obj));
    EXPECT(bd->link.next == NULL);   /* unregistered first */
    if (blk_is_partition(bd))
        g_released_parts++;
    note(3, blk_is_partition(bd));
    mix(bd->name, strlen(bd->name));
    bd->ops->release(bd);
}

static const struct kobject_type blkdev_type = { .name = "blkdev", .release = blkdev_released };

/* ---- the registry: registration order, which blk_nth walks ---- */

#define REG_MAX 256
static struct blkdev *g_reg[REG_MAX];
static unsigned g_nreg;
static int g_fail_register_in = -1;

int blk_register_named(struct blkdev *bd, const char *name)
{
    EXPECT(g_mutex_held);   /* part.c registers under its lock, outside the registry's */
    note(4, strlen(name));
    mix(name, strlen(name));
    note(5, bd->sector_size);
    note(6, bd->capacity);
    note(7, bd->max_sectors);
    note(8, bd->max_segments);
    note(9, bd->read_only);
    note(10, bd->timeout_ns);
    note(11, bd->ops != NULL && bd->ops->submit != NULL && bd->ops->release != NULL && bd->ops->timeout == NULL);
    for (unsigned i = 0; i < g_nreg; i++)
        if (strcmp(g_reg[i]->name, name) == 0) {
            note(12, i);
            return -EEXIST;
        }
    if (g_fail_register_in >= 0 && g_fail_register_in-- == 0) {
        note(13, 0);
        return -ENOMEM;
    }
    EXPECT(g_nreg < REG_MAX);
    kobject_init(&bd->obj, &blkdev_type);
    strlcpy(bd->name, name, sizeof(bd->name));
    bd->link.next = bd->link.prev = &bd->link;
    bd->gone = false;
    kobject_get(&bd->obj);   /* the registry's */
    g_reg[g_nreg++] = bd;
    return 0;
}

void blk_unregister(struct blkdev *bd)
{
    note(14, 0);
    mix(bd->name, strlen(bd->name));
    for (unsigned i = 0; i < g_nreg; i++)
        if (g_reg[i] == bd) {
            memmove(&g_reg[i], &g_reg[i + 1], (g_nreg - i - 1) * sizeof(g_reg[0]));
            g_nreg--;
            bd->gone = true;
            bd->link.next = bd->link.prev = NULL;
            kobject_put(&bd->obj);
            return;
        }
    EXPECT(!"blk_unregister of a device not registered");
}

struct blkdev *blk_nth(unsigned i)
{
    if (i >= g_nreg)
        return NULL;
    kobject_get(&g_reg[i]->obj);
    return g_reg[i];
}

/* ---- the disks' driver: bios accepted, refused, completed now or queued ---- */

#define QUEUE_MAX 64
static struct bio *g_queue[QUEUE_MAX];
static unsigned g_queued;
static unsigned g_refuse_pct, g_now_pct;   /* the submit's policy, redrawn now and then */

void bio_complete(struct bio *bio, int status)
{
    note(15, (uint64_t)(int64_t)status);
    bio->status = status;
    bio->done(bio);
}

static void complete_one(unsigned i)
{
    struct bio *b = g_queue[i];
    memmove(&g_queue[i], &g_queue[i + 1], (g_queued - i - 1) * sizeof(g_queue[0]));
    g_queued--;
    note(16, i);
    bio_complete(b, pick(5) == 0 ? -EIO : 0);
}

static void complete_some(unsigned n)
{
    while (n-- > 0 && g_queued > 0)
        complete_one(pick(g_queued));
}

int blk_submit(struct bio *bio)
{
    note(17, bio->dir);
    note(18, bio->sector);
    note(19, bio->nsectors);
    note(20, bio->nr_vecs);
    note(21, bio->flags);
    struct bio *orig = bio->arg;
    note(22, orig != NULL && bio->buf == orig->buf && bio->vecs == orig->vecs && bio->dev != orig->dev &&
                 bio->status == 0 && bio->link.next == &bio->link);
    mix(bio->dev->name, strlen(bio->dev->name));
    if (bio->dev->gone) {
        note(23, 0);
        return -ENODEV;
    }
    if (pick(100) < g_refuse_pct) {
        int rc = pick(2) ? -EIO : -EAGAIN;
        note(24, (uint64_t)(int64_t)rc);
        return rc;
    }
    if (pick(100) < g_now_pct || g_queued == QUEUE_MAX) {
        bio_complete(bio, pick(7) == 0 ? -EIO : 0);
        return 0;
    }
    g_queue[g_queued++] = bio;
    return 0;
}

/* A gone disk completes what it holds with an error, as blk_unregister's contract has its driver do. */
static void fail_queued_of(struct blkdev *bd)
{
    for (unsigned i = 0; i < g_queued;) {
        if (g_queue[i]->dev == bd) {
            struct bio *b = g_queue[i];
            memmove(&g_queue[i], &g_queue[i + 1], (g_queued - i - 1) * sizeof(g_queue[0]));
            g_queued--;
            bio_complete(b, -EIO);
        } else {
            i++;
        }
    }
}

struct disk {
    struct blkdev bd;
    unsigned slot;
};

static int disk_submit(struct blkdev *bd, struct bio *bio)
{
    (void)bd;
    (void)bio;
    EXPECT(!"a disk's own submit is not called by part.c");
    return -EIO;
}

static void disk_release(struct blkdev *bd)
{
    g_released_disks++;
    kfree(bd);
}

static const struct blkdev_ops disk_ops = { .submit = disk_submit, .release = disk_release };

/* ---- the GPT: tables the test draws; the two types are gpt.cee's bytes ---- */

const uint8_t gpt_type_esp[16] = { 0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
                                   0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b };
const uint8_t gpt_type_cosmo_root[16] = { 0x24, 0x92, 0xb0, 0xc9, 0x0d, 0xe0, 0x8e, 0x41,
                                          0x84, 0x6a, 0x9f, 0x1d, 0x9a, 0x61, 0xbf, 0xd5 };
static const uint8_t g_type_other[16] = { 0x0f, 0xc6, 0x3d, 0xaf, 0x84, 0x83, 0x47, 0x72,
                                          0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4 };

void gpt_guid_format(const uint8_t guid[16], char out[GPT_GUID_TEXT])
{
    static const unsigned order[16] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
    static const char hex[] = "0123456789ABCDEF";
    unsigned o = 0;
    for (unsigned i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out[o++] = '-';
        out[o++] = hex[guid[order[i]] >> 4];
        out[o++] = hex[guid[order[i]] & 15];
    }
    out[o] = '\0';
}

/* Partition GUIDs come from a small pool, so lookups find duplicates and misses. */
#define UUID_POOL 24
static uint8_t g_uuids[UUID_POOL][16];

static const uint8_t *draw_type(void)
{
    switch (pick(3)) {
    case 0: return gpt_type_esp;
    case 1: return gpt_type_cosmo_root;
    default: return g_type_other;
    }
}

static int g_read_fail_pct;

int blk_read(struct blkdev *bd, uint64_t sector, uint32_t nsectors, void *buf)
{
    note(25, sector);
    note(26, nsectors);
    mix(bd->name, strlen(bd->name));
    if ((int)pick(100) < g_read_fail_pct)
        return -EIO;
    memset(buf, 0x5a, (size_t)nsectors * bd->sector_size);
    return 0;
}

enum gpt_result gpt_parse(uint32_t sector_size, uint64_t nsectors, gpt_read_fn read, void *ctx, void *scratch,
                          struct gpt_table *out, const char **why)
{
    note(27, sector_size);
    note(28, nsectors);
    EXPECT(scratch != NULL && out != NULL && why != NULL);
    if (read(ctx, 1, 1, scratch) != 0)
        return GPT_IO;
    unsigned r = pick(10);
    if (r == 0)
        return GPT_NONE;
    if (r == 1) {
        static const char *reasons[] = { "header CRC", "entries outside the disk", "overlapping partitions" };
        *why = reasons[pick(3)];
        return GPT_BAD;
    }
    out->nparts = pick(7);
    if (pick(40) == 0)
        out->nparts = GPT_MAX_PARTS;
    uint64_t at = 34;
    for (unsigned i = 0; i < out->nparts; i++) {
        struct gpt_part *p = &out->parts[i];
        p->index = i + 1 + (pick(4) == 0 ? pick(100) : 0);
        uint64_t len = 1 + pick(64);
        p->first_lba = at;
        p->last_lba = at + len - 1;
        at += len + pick(8);
        memcpy(p->type, draw_type(), 16);
        memcpy(p->uuid, g_uuids[pick(UUID_POOL)], 16);
        p->attrs = next();
    }
    return GPT_OK;
}

/* ---- threads: created into a list, run when the test says; thread_exit returns to the runner ---- */

#define THREADS_MAX 64
static struct {
    void (*entry)(void *);
    void *arg;
} g_threads[THREADS_MAX];
static unsigned g_nthreads, g_thread_put;
static int g_thread_fail_pct;
static jmp_buf g_thread_exit;
static bool g_in_thread;
static uint8_t g_thread_token;

struct thread *thread_create(void (*entry)(void *arg), void *arg, const char *name, int priority)
{
    mix(name, strlen(name));
    note(29, (uint64_t)priority);
    if ((int)pick(100) < g_thread_fail_pct || g_nthreads == THREADS_MAX) {
        note(30, 0);
        return NULL;
    }
    g_threads[g_nthreads].entry = entry;
    g_threads[g_nthreads].arg = arg;
    g_nthreads++;
    return (struct thread *)&g_thread_token;
}

void thread_put(struct thread *t)
{
    EXPECT(t == (struct thread *)&g_thread_token);
    g_thread_put++;
}

void thread_exit(int code)
{
    note(31, (uint64_t)code);
    EXPECT(g_in_thread);
    longjmp(g_thread_exit, 1);
}

/* Time passes: the disks complete one request. */
void thread_sleep_ns(uint64_t ns)
{
    note(32, ns);
    EXPECT(g_queued > 0);   /* nothing would ever wake the waiter */
    complete_one(0);
}

static void run_threads(void)
{
    while (g_nthreads > 0) {
        void (*entry)(void *) = g_threads[0].entry;
        void *arg = g_threads[0].arg;
        memmove(&g_threads[0], &g_threads[1], (g_nthreads - 1) * sizeof(g_threads[0]));
        g_nthreads--;
        note(33, g_nthreads);
        g_in_thread = true;
        if (setjmp(g_thread_exit) == 0) {
            entry(arg);
            EXPECT(!"a thread returned without thread_exit");
        }
        g_in_thread = false;
    }
}

/* ---- the test ---- */

#define DISKS_MAX 8
static struct disk *g_disks[DISKS_MAX];   /* registered, the creator's reference held */
static unsigned g_disk_serial;

static const char *const g_disk_names[] = { "vd", "nvme0n", "sd", "mmcblk", "abcdefghijklmn" };

static void add_disk(unsigned slot)
{
    struct disk *d = kmalloc(sizeof(*d), KMEM_ZERO);
    if (d == NULL)
        return;
    d->slot = slot;
    d->bd.ops = &disk_ops;
    d->bd.sector_size = pick(2) ? 512 : 4096;
    d->bd.capacity = 1024 + pick(1 << 20);
    d->bd.max_sectors = 1 + pick(256);
    d->bd.max_segments = pick(33);
    d->bd.read_only = pick(4) == 0;
    d->bd.dev = (struct device *)(uintptr_t)(0x1000 + 0x100 * slot);
    char name[BLKDEV_NAME_MAX];
    const char *base = g_disk_names[pick(5)];
    if (pick(3) == 0)
        snprintf(name, sizeof(name), "%s", base);   /* ends in a letter or a digit as the base does */
    else
        snprintf(name, sizeof(name), "%s%u", base, g_disk_serial++ % 100);
    g_mutex_held = true;   /* blk_register_named asserts part.c's lock; a disk's registration is the driver's */
    int rc = blk_register_named(&d->bd, name);
    g_mutex_held = false;
    note(34, (uint64_t)(int64_t)rc);
    if (rc) {
        kfree(d);
        return;
    }
    g_disks[slot] = d;
}

static void remove_disk(unsigned slot)
{
    struct disk *d = g_disks[slot];
    g_disks[slot] = NULL;
    note(35, slot);
    blk_part_remove(&d->bd);
    blk_unregister(&d->bd);
    fail_queued_of(&d->bd);
    blkdev_put(&d->bd);
}

/* References the test holds: lookups' results, dropped later. */
#define HELD_MAX 16
static struct blkdev *g_held[HELD_MAX];
static unsigned g_nheld;

static void hold(struct blkdev *bd)
{
    if (bd == NULL)
        return;
    if (g_nheld == HELD_MAX) {
        blkdev_put(bd);
        return;
    }
    g_held[g_nheld++] = bd;
}

static void drop_held(unsigned i)
{
    struct blkdev *bd = g_held[i];
    g_held[i] = g_held[--g_nheld];
    blkdev_put(bd);
}

/* A registered partition, by position among them, referenced; NULL if none. */
static struct blkdev *some_partition(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < g_nreg; i++)
        n += blk_is_partition(g_reg[i]);
    if (n == 0)
        return NULL;
    unsigned k = pick(n);
    for (unsigned i = 0; i < g_nreg; i++)
        if (blk_is_partition(g_reg[i]) && k-- == 0) {
            blkdev_get(g_reg[i]);
            return g_reg[i];
        }
    return NULL;
}

static struct disk *some_disk(void)
{
    unsigned start = pick(DISKS_MAX);
    for (unsigned i = 0; i < DISKS_MAX; i++)
        if (g_disks[(start + i) % DISKS_MAX] != NULL)
            return g_disks[(start + i) % DISKS_MAX];
    return NULL;
}

/* The caller's bios: done folds in the status and frees. */
static unsigned g_orig_live;

static void orig_done(struct bio *bio)
{
    note(36, (uint64_t)(int64_t)bio->status);
    g_orig_live--;
    kfree(bio);
}

static void submit_to(struct blkdev *part, bool fail_alloc)
{
    struct bio *b = kmalloc(sizeof(*b), KMEM_ZERO);
    if (b == NULL)
        return;
    g_fail_in = fail_alloc ? 0 : -1;   /* the forwarded bio's allocation */
    static uint8_t buf[512];
    static struct bio_vec vecs[2];
    b->dev = part;
    unsigned dir = pick(3);
    b->dir = dir == 0 ? BIO_READ : dir == 1 ? BIO_WRITE : BIO_FLUSH;
    if (b->dir != BIO_FLUSH) {
        uint64_t cap = part->capacity;
        switch (pick(4)) {
        case 0: b->sector = pick((unsigned)cap); b->nsectors = 1; break;
        case 1: b->sector = cap - 1; b->nsectors = 1 + pick(2); break;   /* the last sector, or one past */
        case 2: b->sector = cap + pick(4); b->nsectors = 1; break;
        default: b->sector = pick((unsigned)cap); b->nsectors = (uint32_t)(cap - b->sector + pick(2)); break;
        }
        if (pick(2)) {
            b->vecs = vecs;
            b->nr_vecs = 1 + pick(2);
        } else {
            b->buf = buf;
        }
    }
    b->flags = pick(4);
    b->done = orig_done;
    g_orig_live++;
    int rc = part->ops->submit(part, b);
    g_fail_in = -1;
    note(37, (uint64_t)(int64_t)rc);
    if (rc) {
        g_orig_live--;
        kfree(b);
    }
}

static void lookups(void)
{
    const uint8_t *uuid = g_uuids[pick(UUID_POOL)];
    struct blkdev *bd = blk_find_partuuid(uuid);
    note(38, bd != NULL);
    if (bd != NULL) {
        mix(bd->name, strlen(bd->name));
        if (pick(2))
            hold(bd);
        else
            blkdev_put(bd);
    }
    unsigned matches = 99;
    const uint8_t *type = draw_type();   /* each draw in its own statement: argument order is unspecified */
    bool count = pick(4) != 0;
    bd = blk_find_sibling_typed(uuid, type, count ? &matches : NULL);
    note(39, matches);
    note(40, bd != NULL);
    if (bd != NULL) {
        mix(bd->name, strlen(bd->name));
        hold(bd);
    }
}

static void inspect(struct blkdev *bd)
{
    struct blk_part_info info;
    memset(&info, 0xee, sizeof(info));
    bool ok = blk_part_info(bd, &info);
    note(41, ok);
    if (ok) {
        mix(info.disk, strlen(info.disk));
        note(42, info.index);
        note(43, info.start);
        mix(info.uuid, 16);
        mix(info.type, 16);
    }
    const struct blkdev *disk = blk_part_disk(bd);
    note(44, disk != NULL);
    if (disk != NULL)
        mix(disk->name, strlen(disk->name));
    note(45, blk_is_partition(bd));
    note(46, blk_part_any(bd));
}

static void scan(struct blkdev *bd)
{
    unsigned n = 77;
    bool want = pick(3) != 0;
    int rc = blk_part_scan(bd, want ? &n : NULL);
    note(47, (uint64_t)(int64_t)rc);
    note(48, n);
}

static void step(void)
{
    unsigned what = pick(100);
    if (what < 6) {
        unsigned slot = pick(DISKS_MAX);
        if (g_disks[slot] == NULL)
            add_disk(slot);
        else if (pick(3) == 0)
            remove_disk(slot);
    } else if (what < 18) {
        struct disk *d = some_disk();
        if (d != NULL) {
            g_fail_in = pick(8) == 0 ? (int)pick(4) : -1;
            g_fail_register_in = pick(10) == 0 ? (int)pick(4) : -1;
            scan(&d->bd);
            g_fail_in = g_fail_register_in = -1;
        }
    } else if (what < 20) {
        struct blkdev *p = some_partition();
        if (p != NULL) {
            scan(p);   /* a partition is not scanned */
            blk_part_remove(p);   /* nor removed through */
            blkdev_put(p);
        }
    } else if (what < 21) {
        blk_part_scan_all();
    } else if (what < 55) {
        struct blkdev *p = some_partition();
        if (p != NULL) {
            submit_to(p, pick(30) == 0);
            blkdev_put(p);
        }
    } else if (what < 70) {
        complete_some(1 + pick(4));
    } else if (what < 78) {
        lookups();
    } else if (what < 86) {
        if (g_nheld > 0)
            drop_held(pick(g_nheld));
    } else if (what < 92) {
        struct blkdev *p = pick(2) ? some_partition() : NULL;
        if (p != NULL) {
            inspect(p);
            blkdev_put(p);
        } else {
            struct disk *d = some_disk();
            if (d != NULL)
                inspect(&d->bd);
        }
    } else if (what < 96) {
        run_threads();
    } else if (what < 98) {
        g_refuse_pct = pick(30);
        g_now_pct = pick(100);
        g_read_fail_pct = (int)pick(15);
        g_thread_fail_pct = (int)pick(40);
    } else {
        struct disk *d = some_disk();
        if (d != NULL) {
            note(49, 0);
            blk_part_remove(&d->bd);
        }
    }
    EXPECT(!g_mutex_held);
}

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    harness_klog_hook = on_klog;
    for (unsigned i = 0; i < UUID_POOL; i++)
        for (unsigned j = 0; j < 16; j++)
            g_uuids[i][j] = (uint8_t)next();
    g_refuse_pct = 10;
    g_now_pct = 50;
    g_read_fail_pct = 5;
    g_thread_fail_pct = 20;
    blk_part_init();
    for (unsigned i = 0; i < 4; i++)
        add_disk(i);
    blk_part_scan_all();
    for (unsigned r = 0; r < ROUNDS; r++)
        step();

    /* Teardown: drop what the test holds, remove every disk, let the disks and the reapers finish. */
    while (g_nheld > 0)
        drop_held(0);
    for (unsigned i = 0; i < DISKS_MAX; i++)
        if (g_disks[i] != NULL)
            remove_disk(i);
    complete_some(QUEUE_MAX);
    run_threads();
    note(50, g_released_disks);
    note(51, g_released_parts);
    note(52, g_thread_put);
    harness_klog_hook = NULL;
    printf("  part digest 0x%016llx over %u rounds: %u disks and %u partitions released, %u reap threads\n",
           (unsigned long long)g_digest, ROUNDS, g_released_disks, g_released_parts, g_thread_put);
    EXPECT(g_nreg == 0);
    EXPECT(g_queued == 0);
    EXPECT(g_orig_live == 0);
    EXPECT(g_live == 0);
    EXPECT(g_digest == PART_DIGEST);
}

static const struct host_test tests[] = {
    { "part digest", test_digest },
};

int main(void)
{
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
