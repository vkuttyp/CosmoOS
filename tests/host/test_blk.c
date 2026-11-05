/*
 * test_blk.c - The block layer (kernel/block/blk.cee, kernel/include/kernel/blk.h) on the host: roadmap 2.0 SA7. A
 * digest recorded from the C over seeded rounds against drivers the test owns: registrations of every outcome (bad
 * geometry, a prefix that does not fit, names taken, out of letters), bios checked (direction, range, read-only, the
 * single buffer and the segment rules, DMA-able memory) and handed to a driver that accepts, refuses for a moment
 * (-EAGAIN: the pending queue, its resubmission and the redrain when nothing is in flight), fails or completes at
 * once; flush-before and force-unit-access writes as their sequence of flushes; completions local and remote,
 * injected faults; the timeout scan by the clock and by its own count, with skewed stamps, recovery deferring new
 * bios; resets; unregistration with its pause, its drain and the bios it fails; the synchronous helpers; the
 * lookups and the dump; and the test hooks (the parked submitter, the resubmission from a completion, the driver
 * hooks). Every return, every bio the driver sees, every completion with its status, every logged line and the
 * dump are in the digest. The threads, the clock, the allocator, the partitions and the completions are the
 * test's: the timeout thread runs one scan at a time, its sleep returning to the test.
 */

#include "harness.h"

#include <kernel/blk.h>
#include <kernel/completion.h>
#include <kernel/dma.h>
#include <kernel/errno.h>
#include <kernel/faultinject.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/mutex.h>
#include <kernel/object.h>
#include <kernel/page.h>
#include <kernel/part.h>
#include <kernel/percpu.h>
#include <kernel/sched.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/wait.h>

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The values blk.cee writes as its own constants. */
_Static_assert(EINVAL == 22 && ENOSPC == 28 && EEXIST == 17 && ENODEV == 19 && EROFS == 30 && EIO == 5 &&
                   ENOMEM == 12 && EAGAIN == 11 && BIO_READ == 0 && BIO_WRITE == 1 && BIO_FLUSH == 2 &&
                   BIO_PREFLUSH == 1 && BIO_FUA == 2 && BLK_TIMEOUT_NS == 30000000000ull &&
                   BLK_TIMEOUT_SCAN_NS == 500000000ull && BLKDEV_NAME_MAX == 16 && PAGE_SIZE == 4096 &&
                   SCHED_PRIO_DEFAULT == 32 && FI_BLK_SUBMIT == 1 && FI_BLK_COMPLETE == 2 && KMEM_ZERO == 1,
               "blk.cee's errno.h, blk.h, page.h, thread.h, faultinject.h and kmalloc.h values");

/* The digest of the C (blk.c, 2026-11-05), which the CeeSafe port gives. */
#define BLK_DIGEST 0xf136565ff61bcea3ull
#define ROUNDS 20000u
#define SEED 0xC05F5FA2026B0013ull

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

void *kzalloc(size_t size) { return kmalloc(size, KMEM_ZERO); }

void kfree(void *p)
{
    if (p != NULL)
        g_live--;
    free(p);
}

/* ---- the memory the devices reach: the direct map (harness.c's) starts at 64 KiB, so 0x200 is outside it ---- */

#define BAD_BUF ((void *)(uintptr_t)0x200)

bool dma_mappable(struct device *dev, const void *va, size_t len)
{
    note(3, (uintptr_t)dev);
    note(4, len);
    return (uintptr_t)va >= 0x10000 && pick(25) != 0;
}

/* ---- the CPU, the clock, the faults ---- */

static struct percpu g_cpu;
struct percpu *arch_percpu_get(void) { return &g_cpu; }

static uint64_t g_now = 1000000000ull;
uint64_t clock_now_ns(void) { return g_now; }

static unsigned g_fi_pct[2];   /* FI_BLK_SUBMIT, FI_BLK_COMPLETE */

bool faultinject_should_fail(enum fi_kind kind)
{
    EXPECT(kind == FI_BLK_SUBMIT || kind == FI_BLK_COMPLETE);
    bool fail = pick(100) < g_fi_pct[kind - FI_BLK_SUBMIT];
    note(5, (uint64_t)kind * 2 + fail);
    return fail;
}

/* ---- the one mutex, the partitions, the kobjects, the serial ---- */

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

void blk_part_init(void) { note(6, 0); }

void blk_part_remove(struct blkdev *disk)
{
    EXPECT(!g_mutex_held);
    note(7, 0);
    mix(disk->name, strlen(disk->name));
}

void kobject_init(struct kobject *obj, const struct kobject_type *type)
{
    memset(obj, 0, sizeof(*obj));
    obj->type = type;
    obj->refcount = 1;
    mix(type->name, strlen(type->name));
}

void kobject_track_code(struct kobject *obj, uintptr_t code)
{
    (void)obj;
    note(8, code != 0);
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

void blk_serial_ascii(uint8_t const *in, uint32_t n, uint32_t swap, uint8_t *out, uint32_t cap)
{
    (void)in, (void)n, (void)swap, (void)out, (void)cap;
}

/* ---- the timeout thread: one scan per tick, its sleep returning to the test ---- */

static void (*g_thread_entry)(void *);
static void *g_thread_arg;
static char g_thread_storage[64];
static jmp_buf g_tick_jmp;
static bool g_scan_armed;

struct thread *thread_create(void (*entry)(void *arg), void *arg, const char *name, int priority)
{
    EXPECT(g_thread_entry == NULL);
    note(9, (uint64_t)priority);
    mix(name, strlen(name));
    g_thread_entry = entry;
    g_thread_arg = arg;
    return (struct thread *)g_thread_storage;
}

void thread_sleep_ns(uint64_t ns)
{
    EXPECT(!g_mutex_held);
    note(10, ns);
    if (g_scan_armed) {
        g_scan_armed = false;
        return;
    }
    longjmp(g_tick_jmp, 1);
}

static void scan(void)
{
    if (g_thread_entry == NULL)
        return;
    g_scan_armed = true;
    if (setjmp(g_tick_jmp) == 0)
        g_thread_entry(g_thread_arg);
}

/* sched_yield: time passes, and another CPU's submitter leaves the device the test named. */
static struct blkdev *g_yield_bd;

void sched_yield(void)
{
    note(11, 0);
    g_now += 1000000;
    if (g_yield_bd != NULL && g_yield_bd->submitting > 0)
        g_yield_bd->submitting--;
}

/* ---- the driver: bios held in one queue for every device, accepted, refused or completed at once ---- */

#define NDEV 8
#define QMAX 64

struct tdev {
    struct blkdev bd;
    unsigned slot;
    unsigned limit;      /* in flight before -EAGAIN; 0: no limit */
    unsigned held;       /* in flight now */
    unsigned now_pct;    /* completed inside submit */
    unsigned refuse_pct; /* refused (mostly -EAGAIN, now and then -EIO) */
    unsigned timeout_mode;
    bool registered;
};

static struct tdev *g_dev[NDEV];
static struct bio *g_q[QMAX];
static unsigned g_queued;
static unsigned g_live_devs;

static unsigned slot_of(const struct blkdev *bd) { return ((const struct tdev *)bd)->slot; }

static int draw_status(void) { return pick(6) == 0 ? -EIO : 0; }

/* Take the i'th held bio off the queue, before anything completes it. */
static struct bio *take(unsigned i)
{
    struct bio *b = g_q[i];
    memmove(&g_q[i], &g_q[i + 1], (g_queued - i - 1) * sizeof(g_q[0]));
    g_queued--;
    ((struct tdev *)b->dev)->held--;
    return b;
}

static void complete_at(unsigned i, int status)
{
    g_cpu.cpu_id = pick(3) == 0 ? 1 : 0;
    note(12, i);
    struct bio *b = take(i);
    bio_complete(b, status);
}

/* What a driver's teardown does: complete everything it holds for `bd`, with `status`, or (DRAWN) as the device
 * left each: a recovery finds some requests already done. */
#define DRAWN 1
static void fail_held(struct blkdev *bd, int status)
{
    for (unsigned i = 0; i < g_queued;) {
        if (g_q[i]->dev == bd)
            complete_at(i, status == DRAWN ? draw_status() : status);
        else
            i++;
    }
}

static int tdev_submit(struct blkdev *bd, struct bio *bio)
{
    struct tdev *d = (struct tdev *)bd;
    note(13, d->slot);
    note(14, bio->dir);
    note(15, bio->sector);
    note(16, bio->nsectors);
    note(17, bio->flags);
    note(18, (uint64_t)(int64_t)bio->status);
    note(19, bio->nr_vecs);
    if (bio->dir != BIO_FLUSH)   /* a driver walks the segments */
        for (unsigned i = 0; i < bio_segments(bio); i++) {
            struct bio_vec v;
            bio_segment(bio, i, &v);
            note(53, (uintptr_t)v.buf & 4095);
            note(54, v.len);
        }
    if (pick(100) < d->refuse_pct) {
        int rc = pick(5) == 0 ? -EIO : -EAGAIN;
        note(20, (uint64_t)(int64_t)rc);
        return rc;
    }
    if ((d->limit != 0 && d->held >= d->limit) || g_queued == QMAX) {
        note(21, d->held);
        return -EAGAIN;
    }
    if (pick(100) < d->now_pct) {
        g_cpu.cpu_id = 0;
        bio_complete(bio, draw_status());
        return 0;
    }
    g_q[g_queued++] = bio;
    d->held++;
    return 0;
}

static void submit_one(struct tdev *d);

static void tdev_timeout(struct blkdev *bd, struct bio *bio)
{
    struct tdev *d = (struct tdev *)bd;
    note(22, d->slot);
    note(23, d->timeout_mode);
    EXPECT(g_mutex_held && bd->recovering);
    if (pick(4) == 0)   /* another CPU's submitter, arriving during the recovery */
        submit_one(d);
    if (d->timeout_mode == 1) {   /* the reset kind: everything it holds completes, most of it failed */
        fail_held(bd, pick(2) ? DRAWN : -EIO);
        return;
    }
    for (unsigned i = 0; i < g_queued; i++)
        if (g_q[i] == bio) {
            note(24, i);
            if (d->timeout_mode == 0)
                complete_at(i, -ETIMEDOUT);
            return;   /* mode 2: found, and left to the device */
        }
    note(25, 0);   /* already completed */
}

static unsigned g_released;

static void tdev_release(struct blkdev *bd)
{
    struct tdev *d = (struct tdev *)bd;
    note(26, d->slot);
    mix(bd->name, strlen(bd->name));
    EXPECT(!d->registered && d->held == 0);
    g_dev[d->slot] = NULL;
    g_live_devs--;
    g_released++;
    free(d);
}

static const struct blkdev_ops ops_timeout = { .submit = tdev_submit, .release = tdev_release, .timeout = tdev_timeout };
static const struct blkdev_ops ops_plain = { .submit = tdev_submit, .release = tdev_release };
static const struct blkdev_ops ops_no_release = { .submit = tdev_submit };
static const struct blkdev_ops ops_no_submit = { .release = tdev_release };

/* ---- the bios: a pool, each with its own buffer of five pages ---- */

#define NBIO 48
#define BUF_BYTES (5 * 4096)

struct tbio {
    struct bio bio;
    struct bio_vec vecs[6];
    uint8_t *buf;
    unsigned id;
    bool busy;
    bool resubmit;   /* its done puts it back in the pending queue once */
};

static struct tbio g_bios[NBIO];

static void tbio_done(struct bio *bio)
{
    struct tbio *t = (struct tbio *)bio;
    note(27, t->id);
    note(28, (uint64_t)(int64_t)bio->status);
    note(29, bio->arg == t);
    EXPECT(t->busy);
    struct tdev *d = (struct tdev *)bio->dev;
    if (t->resubmit && d->registered && pick(2)) {
        t->resubmit = false;
        note(30, t->id);
        blk_test_resubmit_from_done(bio);
        return;
    }
    t->busy = false;
}

static struct tbio *free_bio(void)
{
    unsigned start = pick(NBIO);
    for (unsigned k = 0; k < NBIO; k++) {
        struct tbio *t = &g_bios[(start + k) % NBIO];
        if (!t->busy)
            return t;
    }
    return NULL;
}

/* The segments of `total` bytes from `start`, cut at page boundaries, then perhaps broken one way. */
static unsigned build_vecs(struct tbio *t, uint8_t *start, size_t total)
{
    unsigned n = 0;
    uint8_t *p = start;
    size_t left = total;
    while (left > 0 && n < 6) {
        size_t room = 4096 - ((uintptr_t)p & 4095);
        size_t len = room < left ? room : left;
        t->vecs[n].buf = p;
        t->vecs[n].len = (uint32_t)len;
        n++;
        p += len;
        left -= len;
    }
    unsigned which = pick(n);
    switch (pick(9)) {
    case 0: t->vecs[which].len = 0; break;
    case 1: t->vecs[which].buf = (uint8_t *)t->vecs[which].buf + 512; break;
    case 2: t->vecs[which].len += 512; break;
    case 3: t->vecs[which].buf = NULL; break;
    case 4: t->vecs[which].buf = BAD_BUF; break;
    default: break;
    }
    return n;
}

static struct tdev *live_dev(void)
{
    if (g_live_devs == 0)
        return NULL;
    unsigned start = pick(NDEV);
    for (unsigned k = 0; k < NDEV; k++) {
        struct tdev *d = g_dev[(start + k) % NDEV];
        if (d != NULL && d->registered)
            return d;
    }
    return NULL;
}

static void submit_one(struct tdev *d)
{
    struct tbio *t = free_bio();
    if (t == NULL)
        return;
    struct blkdev *bd = &d->bd;
    memset(&t->bio, 0, sizeof(t->bio));
    t->bio.dev = bd;
    unsigned dir = pick(12);
    t->bio.dir = dir < 5 ? BIO_READ : dir < 10 ? BIO_WRITE : dir == 10 ? BIO_FLUSH : (enum bio_dir)3;
    unsigned f = pick(10);
    t->bio.flags = f == 0 ? BIO_PREFLUSH : f == 1 ? BIO_FUA : f == 2 ? (BIO_PREFLUSH | BIO_FUA) : 0;
    if (t->bio.dir == BIO_FLUSH) {
        if (pick(8) == 0)
            t->bio.sector = 1;
        if (pick(8) == 0)
            t->bio.nsectors = 1;
    } else {
        unsigned ns = pick(12) == 0 ? 0 : 1 + pick(bd->max_sectors + (pick(8) == 0));
        t->bio.nsectors = ns;
        unsigned where = pick(10);
        t->bio.sector = where == 0 ? bd->capacity : where == 1 ? bd->capacity - pick(4) : pick((unsigned)bd->capacity);
        size_t total = (size_t)ns * bd->sector_size;
        size_t off = (size_t)pick(8) * 512;
        if (off + total > BUF_BYTES)
            off = 0;
        if (total > BUF_BYTES)
            total = BUF_BYTES;
        unsigned shape = pick(10);
        if (shape < 5) {
            t->bio.buf = t->buf + off;
        } else if (shape == 5) {
            t->bio.buf = pick(2) ? NULL : BAD_BUF;
        } else if (total > 0) {
            t->bio.nr_vecs = build_vecs(t, t->buf + off, total);
            t->bio.vecs = pick(15) == 0 ? NULL : t->vecs;
        } else {
            t->bio.buf = t->buf;
        }
    }
    unsigned bad = pick(40);
    t->bio.done = bad == 0 ? NULL : tbio_done;
    if (bad == 1)
        t->bio.dev = NULL;
    t->bio.arg = t;
    t->resubmit = pick(10) == 0;
    list_init(&t->bio.link);
    t->busy = true;
    int rc = blk_submit(&t->bio);
    note(31, (uint64_t)(int64_t)rc);
    if (rc != 0)
        t->busy = false;
}

/* ---- the completions the synchronous helpers wait on: the driver completes held bios until it is done ---- */

void completion_init(struct completion *c, const char *name)
{
    mix(name, strlen(name));
    c->done = 0;
}

void complete(struct completion *c)
{
    note(32, 0);
    c->done = 1;
}

void wait_for_completion(struct completion *c)
{
    while (!c->done) {
        if (g_queued == 0) {
            EXPECT(!"a synchronous wait with nothing in flight");
            return;
        }
        unsigned i = pick(g_queued);   /* each draw in a statement of its own: C leaves the order of arguments open */
        complete_at(i, draw_status());
    }
}

static uint8_t *g_sync_buf;

static void sync_one(struct tdev *d)
{
    struct blkdev *bd = &d->bd;
    unsigned what = pick(5);
    uint32_t n = pick(10) == 0 ? 0 : 1 + pick(3 * bd->max_sectors);
    if ((size_t)n * bd->sector_size > 16 * 4096)
        n = (uint32_t)(16 * 4096 / bd->sector_size);
    uint64_t sector = pick(10) == 0 ? bd->capacity : pick((unsigned)bd->capacity);
    void *buf = pick(15) == 0 ? NULL : g_sync_buf;
    int rc;
    switch (what) {
    case 0: rc = blk_read(bd, sector, n, buf); break;
    case 1: rc = blk_write(bd, sector, n, buf); break;
    case 2: rc = blk_write_flags(bd, sector, n, buf, pick(4)); break;
    default: rc = blk_flush(bd); break;
    }
    note(33, what);
    note(34, (uint64_t)(int64_t)rc);
}

/* ---- registration and removal ---- */

static const char *const g_names[] = { "nv0", "nv1", "nv2", "", "0123456789abcdef" };

static struct tdev *new_dev(unsigned slot)
{
    struct tdev *d = calloc(1, sizeof(*d));
    d->slot = slot;
    struct blkdev *bd = &d->bd;
    unsigned o = pick(20);
    bd->ops = o == 0 ? NULL : o == 1 ? &ops_no_release : o == 2 ? &ops_no_submit : pick(3) ? &ops_timeout : &ops_plain;
    unsigned ss = pick(16);
    bd->sector_size = ss == 0 ? 256 : ss == 1 ? 1000 : ss < 6 ? 4096 : 512;
    bd->capacity = pick(25) == 0 ? 0 : 64 + pick(4000);
    unsigned ms = bd->sector_size == 4096 ? 4 : 32;
    bd->max_sectors = pick(25) == 0 ? 0 : 1 + pick(ms);
    bd->read_only = pick(6) == 0;
    bd->dev = pick(3) == 0 ? (struct device *)(uintptr_t)0x5000 : NULL;
    bd->max_segments = pick(5);
    bd->nr_queues = pick(2) * 2;
    unsigned t = pick(4);
    bd->timeout_ns = t == 0 ? 0 : t == 1 ? UINT64_MAX : t == 2 ? 300000000ull : 2000000000ull;
    d->limit = pick(3) == 0 ? 0 : 1 + pick(4);
    d->now_pct = pick(3) * 25;
    d->refuse_pct = pick(3) == 0 ? 20 : 0;
    d->timeout_mode = pick(3);
    return d;
}

static void register_one(void)
{
    unsigned slot = NDEV;
    for (unsigned i = 0; i < NDEV; i++)
        if (g_dev[i] == NULL) {
            slot = i;
            break;
        }
    if (slot == NDEV)
        return;
    struct tdev *d = new_dev(slot);
    int rc;
    unsigned how = pick(10);
    if (how < 6) {
        static const char *const prefixes[] = { "vd", "sd", "abcdefghijklmno", "abcdefghijklmn" };
        rc = blk_register(&d->bd, prefixes[pick(4)]);
    } else {
        rc = blk_register_named(&d->bd, how == 9 ? NULL : g_names[pick(5)]);
    }
    note(35, (uint64_t)(int64_t)rc);
    if (rc != 0) {
        free(d);
        return;
    }
    mix(d->bd.name, strlen(d->bd.name));
    note(36, d->bd.max_segments);
    note(37, d->bd.timeout_ns);
    note(38, d->bd.nr_queues);
    d->registered = true;
    g_dev[slot] = d;
    g_live_devs++;
}

/* The parked submitter: the hold armed and already released (one CPU: nothing else would let it out). */
static void park_one(struct tdev *d)
{
    blk_test_hold_in_driver(true);
    blk_test_release_in_driver();
    submit_one(d);
    note(39, blk_test_submitter_parked());
}

static void unregister_one(struct tdev *d)
{
    struct blkdev *bd = &d->bd;
    if (pick(4) == 0)
        park_one(d);
    blk_test_unregister_pause(pick(4) == 0 ? 1 + pick(3) : 0);
    if (pick(3) == 0) {
        bd->submitting = 1 + pick(3);   /* submitters inside the driver, on other CPUs */
        g_yield_bd = bd;
    }
    blk_unregister(bd);
    g_yield_bd = NULL;
    blk_test_unregister_pause(0);
    d->registered = false;
    note(40, blk_test_unregister_spins());
    note(41, blk_test_drain_ordered());
    blk_test_hold_in_driver(false);
    if (pick(2)) {   /* a submitter that comes late */
        struct tbio *t = free_bio();
        if (t != NULL) {
            memset(&t->bio, 0, sizeof(t->bio));
            t->bio.dev = bd;
            t->bio.done = tbio_done;
            note(42, (uint64_t)(int64_t)blk_submit(&t->bio));
        }
    }
    fail_held(bd, -EIO);
    blkdev_put(bd);   /* the creator's */
}

static int g_reinit_rc;

static int reinit(struct blkdev *bd)
{
    note(43, slot_of(bd));
    EXPECT(g_mutex_held && bd->recovering && bd->submitting == 0);
    fail_held(bd, pick(2) ? DRAWN : -EIO);
    return g_reinit_rc;
}

static void reset_one(struct tdev *d)
{
    struct blkdev *bd = &d->bd;
    g_reinit_rc = pick(4) == 0 ? -EIO : 0;
    if (pick(3) == 0) {
        bd->submitting = 1 + pick(2);
        g_yield_bd = bd;
    }
    note(44, (uint64_t)(int64_t)blk_reset(bd, reinit));
    g_yield_bd = NULL;
}

/* ---- the lookups and the dump ---- */

static void dump(void)
{
    fflush(stdout);
    FILE *f = tmpfile();
    EXPECT(f != NULL);
    int saved = dup(1);
    dup2(fileno(f), 1);
    blk_dump();
    fflush(stdout);
    dup2(saved, 1);
    close(saved);
    rewind(f);
    char text[4096];
    size_t n = fread(text, 1, sizeof(text), f);
    mix(text, n);
    note(45, n);
    fclose(f);
}

static void lookups(void)
{
    struct tdev *d = live_dev();
    const char *name = d != NULL && pick(4) ? d->bd.name : g_names[pick(5)];
    struct blkdev *b = blk_find(name);
    note(46, b != NULL ? slot_of(b) + 1 : 0);
    if (b != NULL)
        blkdev_put(b);
    unsigned count = blk_count();
    note(47, count);
    b = blk_nth(pick(count + 2));
    note(48, b != NULL ? slot_of(b) + 1 : 0);
    if (b != NULL)
        blkdev_put(b);
}

static unsigned g_hook_releases;
static unsigned hook_releases(void) { return ++g_hook_releases; }
static const struct blk_test_driver_hooks g_hooks = { .driver = "tdev", .releases = hook_releases };

static void hooks(void)
{
    blk_test_driver_hooks_set(pick(2) ? &g_hooks : NULL);
    const struct blk_test_driver_hooks *h = blk_test_driver_hooks(pick(2) ? "tdev" : "other");
    note(49, h != NULL ? h->releases() : 0);
    note(50, blk_test_tick());
}

/* ---- the run ---- */

static void out_of_letters(void)
{
    struct tdev *all[27];
    for (unsigned i = 0; i < 27; i++) {
        all[i] = calloc(1, sizeof(*all[i]));
        all[i]->slot = i;
        all[i]->bd.ops = &ops_plain;
        all[i]->bd.sector_size = 512;
        all[i]->bd.capacity = 8;
        all[i]->bd.max_sectors = 8;
        int rc = blk_register(&all[i]->bd, "z");
        note(51, (uint64_t)(int64_t)rc);
        if (rc != 0) {
            free(all[i]);
            all[i] = NULL;
        }
    }
    EXPECT(all[26] == NULL);
    note(52, blk_count());
    for (unsigned i = 0; i < 26; i++) {
        g_dev[0] = all[i];   /* tdev_release clears the slot it names */
        all[i]->slot = 0;
        g_live_devs++;
        blk_unregister(&all[i]->bd);
        blkdev_put(&all[i]->bd);
    }
}

/* Ten requests a stalled device holds: one scan reports the eight it has room for, the next the other two. */
static void stalled(void)
{
    struct tdev *d = calloc(1, sizeof(*d));
    d->bd.ops = &ops_timeout;
    d->bd.sector_size = 512;
    d->bd.capacity = 1024;
    d->bd.max_sectors = 8;
    d->bd.timeout_ns = 300000000ull;
    d->timeout_mode = 2;
    note(55, (uint64_t)(int64_t)blk_register_named(&d->bd, "stall0"));
    d->registered = true;
    g_dev[0] = d;
    g_live_devs++;
    for (unsigned i = 0; i < 10; i++) {
        struct tbio *t = &g_bios[i];
        memset(&t->bio, 0, sizeof(t->bio));
        t->bio.dev = &d->bd;
        t->bio.dir = BIO_READ;
        t->bio.sector = i;
        t->bio.nsectors = 1;
        t->bio.buf = t->buf;
        t->bio.done = tbio_done;
        t->bio.arg = t;
        t->busy = true;
        note(56, (uint64_t)(int64_t)blk_submit(&t->bio));
    }
    for (unsigned i = 0; i < 4; i++) {
        g_now += 500000000ull;
        scan();
    }
    note(57, d->bd.timeouts);
    unregister_one(d);
}

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    harness_klog_hook = on_klog;
    harness_klog_min = KLOG_ERROR + 1;   /* the timeouts' warnings are expected, by the thousand */
    pmm_hhdm_base = 0x10000;
    pmm_hhdm_limit = 1ull << 62;
    for (unsigned i = 0; i < NBIO; i++) {
        g_bios[i].id = i;
        g_bios[i].buf = aligned_alloc(4096, BUF_BYTES);
    }
    g_sync_buf = aligned_alloc(4096, 16 * 4096);

    blk_init();
    out_of_letters();
    stalled();
    for (unsigned round = 0; round < ROUNDS; round++) {
        if (round % 2000 == 0) {
            g_fi_pct[0] = round / 2000 % 3 == 1 ? 5 : 0;
            g_fi_pct[1] = round / 2000 % 3 == 2 ? 5 : 0;
            blk_test_set_issue_skew_ns(pick(3) == 0 ? 3600000000000ull : 0);
        }
        struct tdev *d = live_dev();
        unsigned what = pick(100);
        if (d == NULL || what < 4) {
            register_one();
        } else if (what < 6) {
            unregister_one(d);
        } else if (what < 46) {
            submit_one(d);
        } else if (what < 66) {
            unsigned k = 1 + pick(4);
            while (k-- > 0 && g_queued > 0) {
                unsigned i = pick(g_queued);
                complete_at(i, draw_status());
            }
        } else if (what < 76) {
            unsigned step = pick(4);
            g_now += step == 0 ? 0 : step == 1 ? 100000000ull : step == 2 ? 500000000ull : 31000000000ull;
            scan();
        } else if (what < 80) {
            reset_one(d);
        } else if (what < 88) {
            sync_one(d);
        } else if (what < 94) {
            lookups();
        } else if (what < 96) {
            g_fail_in = pick(2);   /* the next sequence's allocation fails, or the one after */
            submit_one(d);
            g_fail_in = -1;
        } else if (what < 98) {
            hooks();
        } else {
            dump();
        }
    }
    /* Everything is completed, removed and released. */
    for (unsigned i = 0; i < NDEV; i++)
        if (g_dev[i] != NULL && g_dev[i]->registered)
            unregister_one(g_dev[i]);
    EXPECT(g_queued == 0 && g_live_devs == 0);
    for (unsigned i = 0; i < NBIO; i++) {
        EXPECT(!g_bios[i].busy);
        free(g_bios[i].buf);
    }
    free(g_sync_buf);
    EXPECT(g_live == 0);
    harness_klog_hook = NULL;
    harness_klog_min = KLOG_WARN;
    printf("  blk digest 0x%016llx over %u rounds, %u devices released\n", (unsigned long long)g_digest, ROUNDS,
           g_released);
    EXPECT(g_digest == BLK_DIGEST);
}

int main(void)
{
    static const struct host_test tests[] = {
        { "blk_digest", test_digest },
    };
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
