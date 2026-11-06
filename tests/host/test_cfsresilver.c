/*
 * test_cfsresilver.c - cosmofs's resilver (kernel-services/filesystem/cosmofs/cosmofs_resilver.cee) on the host:
 * roadmap 2.0 SA7. A digest recorded from the C over seeded pools the test builds and owns (struct spool itself, with
 * the pool's operations, the labels, the holds and the devices modelled here): copies in every state, rebuilt whole
 * and region by region, an ONLINE copy's holes filled, sources that disagree, spares taking over, devices removed, the
 * finish on member 0 and on another member, replaces that detach the copy they replace, and every operator command
 * (status, offline, online, replace, attach, detach, spares) with malformed names, unknown devices and every refusal.
 * The resilver thread runs here, a few windows at a time; the self-test seams run it in the caller. Every call, every
 * argument, every return, every logged line and the pool's state after each step are in the digest; no device
 * reference and no allocation is left over.
 */

#include "harness.h"

#include <kernel/blk.h>
#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/mutex.h>
#include <kernel/random.h>
#include <kernel/storage.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/vfs.h>

#include "cosmofs_internal.h"   /* before the C library's headers, as test_cfsparse.c: its signal.h is the kernel's */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The values the CeeSafe port writes as its own constants, and the stack slot it relies on. */
_Static_assert(ENOENT == 2 && EIO == 5 && ENOMEM == 12 && EAGAIN == 11 && EBUSY == 16 && EEXIST == 17 &&
                   ENODEV == 19 && EINVAL == 22 && ENOSPC == 28 && EROFS == 30 && EOPNOTSUPP == 95,
               "cosmofs_resilver's errno.h values");
_Static_assert(POOL_COPY_ONLINE == 0 && POOL_COPY_MISSING == 1 && POOL_COPY_OFFLINE == 2 && POOL_COPY_FAULTED == 3 &&
                   POOL_COPY_RESILVERING == 4 && POOL_MAX_COPIES == 4 && POOL_BLOCK == 4096 && POOL_DVA_SHIFT == 56 &&
                   CFS_BLOCK == 4096 && CFS_LABEL_RESILVER == 1 && CFS_LABEL_SPARE == 2 && SCHED_PRIO_DEFAULT == 32,
               "cosmofs_resilver's storage.h, thread.h and cosmofs_format.h values");
_Static_assert(COSMOFS_POOL_STATUS == 0 && COSMOFS_POOL_OFFLINE == 1 && COSMOFS_POOL_ONLINE == 2 &&
                   COSMOFS_POOL_REPLACE == 3 && COSMOFS_POOL_ATTACH == 4 && COSMOFS_POOL_DETACH == 5 &&
                   COSMOFS_POOL_SPARE_ADD == 6 && COSMOFS_POOL_SPARE_REMOVE == 7 && COSMOFS_COPY_SPARE == 5 &&
                   COSMOFS_COPY_I_WHOLE == 1 && COSMOFS_COPY_I_DETACH == 2 && sizeof(enum pool_copy_state) == 4 &&
                   sizeof(enum cosmofs_pool_op) == 4 && sizeof(((struct cfs *)0)->rs_stop) == 4 &&
                   sizeof(((struct cfs *)0)->pool_name) == 17,
               "cosmofs_resilver's cosmofs.h values, and the enums and words it reads as u32");
_Static_assert(sizeof(struct cfs_pace) <= 4 * 8, "cosmofs_resilver.cee keeps the pacing state in 4 words");

/* The digest of the C (cosmofs_resilver.c, 2026-11-06), which the CeeSafe port gives. */
#define RESILVER_DIGEST 0x2524b2fde370ae3bull
#define ROUNDS 12000u
#define SEED 0x2E5112E52026B019ull

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

/* ---- the allocator: malloc, counted; an allocation fails when g_fail_in counts down to it ---- */

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

void kfree(void *p)
{
    if (p != NULL)
        g_live--;
    free(p);
}

static unsigned g_fail_pct;   /* of the pool's, the labels' and the commits' operations that fail */

static int draw_rc(void)
{
    unsigned r = pick(100);
    if (r >= g_fail_pct)
        return 0;
    unsigned k = pick(3);
    return k == 0 ? -EIO : k == 1 ? -ENOSPC : -EBUSY;
}

/* ---- the devices: referenced as kobjects ---- */

#define NDEV 12u

static struct blkdev g_dev[NDEV];
static int g_refs[NDEV];

static int dev_index(const struct blkdev *bd)
{
    EXPECT(bd >= g_dev && bd < g_dev + NDEV);
    return (int)(bd - g_dev);
}

void kobject_get(struct kobject *o)
{
    struct blkdev *bd = (struct blkdev *)((uint8_t *)o - offsetof(struct blkdev, obj));
    g_refs[dev_index(bd)]++;
}

void kobject_put(struct kobject *o)
{
    struct blkdev *bd = (struct blkdev *)((uint8_t *)o - offsetof(struct blkdev, obj));
    EXPECT(g_refs[dev_index(bd)] > 0);
    g_refs[dev_index(bd)]--;
}

static struct blkdev *ref(struct blkdev *bd)
{
    blkdev_get(bd);
    return bd;
}

struct blkdev *blk_find(const char *name)
{
    note(3, name[0] == 'd' ? (uint64_t)name[1] : 0);
    for (unsigned i = 0; i < NDEV; i++)
        if (strcmp(g_dev[i].name, name) == 0)
            return ref(&g_dev[i]);
    return NULL;
}

int blk_flush(struct blkdev *bd)
{
    note(4, (uint64_t)dev_index(bd));
    return draw_rc();
}

bool vfs_bdev_mounted(const struct blkdev *bd) { return dev_index(bd) == 9; }

/* ---- the pool: struct spool itself; what each copy missed and holds is kept here ---- */

#define NMEM 3u
#define REGIONS 8u

static struct spool g_pool;
static bool g_missed[NMEM][POOL_MAX_COPIES][REGIONS];
static uint8_t g_content[NMEM][POOL_MAX_COPIES][128];   /* a block's first byte, by copy */

static unsigned region_of(unsigned v, uint64_t blk) { return (unsigned)(blk / g_pool.m[v].region_blocks); }

static void notify(void *arg)
{
    EXPECT(arg == &g_pool);
    note(5, g_pool.state_seq);
}

bool pool_copy_online(const struct spool *p, unsigned v, unsigned c)
{
    return p->m[v].copy[c].state == POOL_COPY_ONLINE && p->m[v].copy[c].dev != NULL;
}

bool pool_copy_readable(const struct spool *p, uint64_t dva, unsigned c)
{
    unsigned v = POOL_DVA_VDEV(dva);
    return pool_copy_online(p, v, c) && !g_missed[v][c][region_of(v, POOL_DVA_BLK(dva))];
}

bool pool_member_degraded(const struct spool *p, unsigned v)
{
    for (unsigned c = 0; c < p->m[v].ncopies; c++)
        if (!pool_copy_online(p, v, c))
            return true;
    return false;
}

bool pool_degraded(const struct spool *p)
{
    for (unsigned v = 0; v < p->nmembers; v++)
        if (pool_member_degraded(p, v))
            return true;
    return false;
}

static void fill_block(void *buf, uint8_t value)
{
    memset(buf, 0, CFS_BLOCK);
    *(uint8_t *)buf = value;
}

int pool_read_copy(struct spool *p, uint64_t dva, unsigned c, void *buf)
{
    (void)p;
    note(6, dva);
    note(7, c);
    int rc = draw_rc();
    if (rc == 0)
        fill_block(buf, g_content[POOL_DVA_VDEV(dva)][c][POOL_DVA_BLK(dva) % 128]);
    return rc;
}

int pool_read(struct spool *p, uint64_t dva, void *buf)
{
    unsigned v = POOL_DVA_VDEV(dva);
    note(8, dva);
    for (unsigned c = 0; c < p->m[v].ncopies; c++)
        if (pool_copy_readable(p, dva, c))
            return pool_read_copy(p, dva, c, buf);
    return -EIO;
}

int pool_write_copy(struct spool *p, uint64_t dva, unsigned c, const void *buf)
{
    unsigned v = POOL_DVA_VDEV(dva);
    note(9, dva);
    note(10, c);
    note(11, *(const uint8_t *)buf);
    int rc = draw_rc();
    if (rc) {
        /* The pool's answer to a failed write: the region missed, and the error counted. */
        p->m[v].copy[c].write_errors++;
        g_missed[v][c][region_of(v, POOL_DVA_BLK(dva))] = true;
        if (p->m[v].copy[c].write_errors > 3) {
            p->m[v].copy[c].state = POOL_COPY_FAULTED;   /* past fs.fault_errors, as the pool has it */
            p->state_seq++;
        }
        return rc;
    }
    g_content[v][c][POOL_DVA_BLK(dva) % 128] = *(const uint8_t *)buf;
    return 0;
}

void pool_region_clear(struct spool *p, unsigned v, unsigned c, uint64_t r)
{
    (void)p;
    note(12, r);
    EXPECT(r < REGIONS);
    g_missed[v][c][r] = false;
}

bool pool_region_missed(const struct spool *p, unsigned v, unsigned c, uint64_t r)
{
    return r < REGIONS && (p->m[v].copy[c].whole || g_missed[v][c][r]);
}

bool pool_copy_any_missed(const struct spool *p, unsigned v, unsigned c)
{
    if (p->m[v].copy[c].whole)
        return true;
    for (unsigned r = 0; r < REGIONS; r++)
        if (g_missed[v][c][r])
            return true;
    return false;
}

int pool_copy_set_state(struct spool *p, unsigned v, unsigned c, enum pool_copy_state state, int why)
{
    note(13, state);
    note(14, (uint64_t)(int64_t)why);
    if (state == POOL_COPY_ONLINE && pool_copy_any_missed(p, v, c))
        return -EAGAIN;
    p->m[v].copy[c].state = state;
    p->state_seq++;
    return 0;
}

const char *pool_copy_state_name(enum pool_copy_state s)
{
    static const char *const names[] = { "ONLINE", "MISSING", "OFFLINE", "FAULTED", "RESILVERING" };
    return (unsigned)s < 5 ? names[s] : "?";
}

int pool_copy_resilvered(struct spool *p, unsigned v, unsigned c)
{
    note(15, v * 8 + c);
    int rc = draw_rc();
    if (rc)
        return rc;
    p->m[v].copy[c].state = POOL_COPY_ONLINE;
    p->m[v].copy[c].whole = false;
    p->state_seq++;
    return 0;
}

static void clear_copy(unsigned v, unsigned c)
{
    struct pool_copy *cp = &g_pool.m[v].copy[c];
    if (cp->dev != NULL)
        blkdev_put(cp->dev);
    memset(cp, 0, sizeof(*cp));
    cp->state = POOL_COPY_MISSING;
    for (unsigned r = 0; r < REGIONS; r++)
        g_missed[v][c][r] = false;
}

int pool_copy_replace_dev(struct spool *p, unsigned v, unsigned c, struct blkdev *bd, enum pool_copy_state state)
{
    note(16, (uint64_t)dev_index(bd));
    note(17, state);
    int rc = draw_rc();
    if (rc)
        return rc;
    struct pool_copy *cp = &p->m[v].copy[c];
    if (cp->dev != NULL)
        blkdev_put(cp->dev);
    cp->dev = ref(bd);
    cp->state = state;
    cp->whole = true;
    cp->cursor = 0;
    cp->read_errors = cp->write_errors = cp->csum_errors = 0;
    p->state_seq++;
    return 0;
}

int pool_set_copies(struct spool *p, unsigned v, unsigned n)
{
    note(18, n);
    int rc = draw_rc();
    if (rc)
        return rc;
    for (unsigned c = p->m[v].ncopies; c < n; c++)
        clear_copy(v, c);
    p->m[v].ncopies = n;
    return 0;
}

int pool_attach_copy(struct spool *p, unsigned v, unsigned c, struct blkdev *bd, enum pool_copy_state state)
{
    note(19, (uint64_t)dev_index(bd));
    int rc = draw_rc();
    if (rc)
        return rc;
    p->m[v].copy[c].dev = ref(bd);
    p->m[v].copy[c].state = state;
    return 0;
}

int pool_detach_copy(struct spool *p, unsigned v, unsigned c)
{
    note(20, v * 8 + c);
    if (p->m[v].ncopies <= 1 || (p->m[v].copy[c].dev != NULL && dev_index(p->m[v].copy[c].dev) == 5))
        return -EBUSY;   /* the last copy, or (d5) one the pool will not let go of */
    int rc = draw_rc();
    if (rc)
        return rc;
    clear_copy(v, c);
    for (unsigned k = c; k + 1 < p->m[v].ncopies; k++) {
        p->m[v].copy[k] = p->m[v].copy[k + 1];
        memcpy(g_missed[v][k], g_missed[v][k + 1], sizeof(g_missed[v][k]));
        memcpy(g_content[v][k], g_content[v][k + 1], sizeof(g_content[v][k]));
    }
    p->m[v].ncopies--;
    memset(&p->m[v].copy[p->m[v].ncopies], 0, sizeof(struct pool_copy));
    p->state_seq++;
    return 0;
}

int pool_add_spare(struct spool *p, struct blkdev *bd, uint64_t tag)
{
    note(21, (uint64_t)dev_index(bd));
    note(22, tag);
    if (p->nspares == POOL_MAX_SPARES)
        return -ENOSPC;
    p->spare[p->nspares] = ref(bd);
    p->spare_tag[p->nspares++] = tag;
    return 0;
}

int pool_remove_spare(struct spool *p, struct blkdev *bd)
{
    note(23, (uint64_t)dev_index(bd));
    for (unsigned i = 0; i < p->nspares; i++)
        if (p->spare[i] == bd) {
            blkdev_put(bd);
            for (unsigned k = i; k + 1 < p->nspares; k++) {
                p->spare[k] = p->spare[k + 1];
                p->spare_tag[k] = p->spare_tag[k + 1];
            }
            p->nspares--;
            return 0;
        }
    return -ENOENT;
}

struct blkdev *pool_take_spare(struct spool *p, unsigned v, uint64_t *tag)
{
    note(24, v);
    unsigned small = pick(6);
    if (p->nspares == 0 || small == 0)
        return NULL;   /* none, or none big enough for the member */
    p->nspares--;
    *tag = p->spare_tag[p->nspares];
    return p->spare[p->nspares];   /* the pool's reference, the caller's now */
}

bool pool_find_dev(const struct spool *p, const struct blkdev *bd, unsigned *v, unsigned *c)
{
    for (unsigned i = 0; i < p->nmembers; i++)
        for (unsigned k = 0; k < p->m[i].ncopies; k++)
            if (p->m[i].copy[k].dev == bd) {
                *v = i;
                *c = k;
                return true;
            }
    return false;
}

/* ---- the labels, the holds, the allocator's map, the commit ---- */

int cfs_label_write_dev(struct cfs *fs, struct blkdev *bd, unsigned vdev, uint64_t flags, uint64_t tag,
                        uint64_t nblocks)
{
    (void)fs;
    note(30, (uint64_t)dev_index(bd));
    note(31, vdev);
    note(32, flags);
    note(33, tag);
    note(34, nblocks);
    return draw_rc();
}

int cfs_label_write(struct spool *pool, const struct cfs_mac *mac, unsigned vdev, unsigned copy, uint64_t generation,
                    const uint8_t *uuid, uint64_t nblocks)
{
    EXPECT(pool == &g_pool && mac != NULL && uuid != NULL);
    note(35, vdev * 8 + copy);
    note(36, generation);
    note(37, nblocks);
    return draw_rc();
}

void cfs_label_clear_dev(struct blkdev *bd) { note(38, (uint64_t)dev_index(bd)); }

void cfs_holds_sync(struct cfs *fs)
{
    (void)fs;
    note(39, 0);
}

int cfs_holds_sync_with(struct cfs *fs, struct blkdev *extra)
{
    (void)fs;
    note(40, (uint64_t)dev_index(extra));
    return draw_rc();
}

void cfs_spares_find(struct cfs *fs)
{
    (void)fs;
    note(41, 0);
}

struct blkdev *cfs_find_lagging(struct cfs *fs, unsigned vdev, uint64_t since, bool *exact)
{
    (void)fs;
    note(42, vdev);
    note(43, since);
    unsigned k = pick(4);
    if (k == 0)
        return NULL;
    unsigned e = pick(2);
    *exact = e == 0;
    return ref(&g_dev[k == 1 ? 10 : 6 + pick(4)]);
}

uint64_t cfs_dva_lin(const struct cfs *fs, uint64_t dva)
{
    (void)fs;
    return POOL_DVA_VDEV(dva) * 1000 + POOL_DVA_BLK(dva);
}

bool cfs_bitmap_test(const struct cfs *fs, uint64_t lin)
{
    (void)fs;
    return (lin * 2654435761u >> 7) % 4 != 0;
}

int cosmofs_commit_mount(struct mount *mnt)
{
    (void)mnt;
    int rc = draw_rc();
    note(44, (uint64_t)(int64_t)rc);
    return rc;
}

/* ---- pacing, the lock, the clock, the thread, the random source ---- */

static bool g_locked;
static uint64_t g_now = 1000000000ull;
static int g_thread_windows = -1;   /* the running thread's windows left; 0 stops it */
static struct cfs *g_fs;

void cfs_pace_init(struct cfs_pace *p, unsigned rate)
{
    note(45, rate);
    p->quota = rate == 0 ? 0 : 1 + rate / 4096;
    p->used = 0;
    p->window_ns = 0;
    p->window = 0;
}

bool cfs_pace(struct cfs *fs, struct cfs_pace *p)
{
    EXPECT(g_locked);
    if (p->quota == 0 || p->used < p->quota)
        return false;
    note(46, p->used);
    mutex_unlock(&fs->lock);
    /* While the lock is let go: a copy fails, or loses its device. */
    unsigned what = pick(10);
    unsigned v = pick(g_pool.nmembers);
    unsigned c = pick(g_pool.m[v].ncopies);
    struct pool_copy *cp = &g_pool.m[v].copy[c];
    if (what == 0) {
        cp->state = POOL_COPY_FAULTED;
        g_pool.state_seq++;
    } else if (what == 1 && cp->dev != NULL) {
        blkdev_put(cp->dev);
        cp->dev = NULL;
        cp->state = POOL_COPY_MISSING;
    }
    mutex_lock(&fs->lock);
    p->used = 0;
    return true;
}

static void thread_window(void)
{
    if (g_thread_windows > 0 && --g_thread_windows == 0)
        __atomic_store_n(&g_fs->rs_stop, true, __ATOMIC_RELEASE);
}

void mutex_lock(struct mutex *m)
{
    (void)m;
    EXPECT(!g_locked);
    g_locked = true;
}

void mutex_unlock(struct mutex *m)
{
    (void)m;
    EXPECT(g_locked);
    g_locked = false;
    thread_window();
}

uint64_t clock_now_ns(void)
{
    g_now += 3000000ull;
    return g_now;
}

uint64_t clock_since_ns(uint64_t stamp) { return g_now > stamp ? g_now - stamp : 0; }

void thread_sleep_ns(uint64_t ns)
{
    EXPECT(!g_locked);
    note(47, ns);
}

static void (*g_entry)(void *);
static void *g_entry_arg;
static uint8_t g_thread_token;

struct thread *thread_create(void (*entry)(void *arg), void *arg, const char *name, int priority)
{
    note(48, (uint64_t)priority);
    mix(name, strlen(name));
    unsigned none = pick(8);
    if (none == 0)
        return NULL;
    g_entry = entry;
    g_entry_arg = arg;
    return (struct thread *)&g_thread_token;
}

int thread_join(struct thread *t)
{
    EXPECT(t == (struct thread *)&g_thread_token);
    note(49, 0);
    return 0;
}

uint64_t random_u64(void)
{
    unsigned zero = pick(6);
    return zero == 0 ? 0 : next();
}

/* ---- the worlds ---- */

static struct cfs *g_cfs;
static struct mount g_mnt;

static void build(void)
{
    struct cfs *fs = g_cfs;
    for (unsigned i = 0; i < NDEV; i++) {
        unsigned gone = pick(25);
        g_dev[i].gone = gone == 0;
        unsigned ss = pick(10);
        g_dev[i].sector_size = ss == 0 ? 0 : ss == 1 ? 8192 : ss == 2 ? 1000 : 512;
        unsigned cap = pick(4);
        g_dev[i].capacity = (uint64_t)(cap == 0 ? 16 : 1024) * 8;
        unsigned rdo = pick(12);
        g_dev[i].read_only = rdo == 0;
    }
    memset(&g_pool, 0, sizeof(g_pool));
    memset(g_missed, 0, sizeof(g_missed));
    g_pool.m = calloc(NMEM, sizeof(*g_pool.m));
    unsigned nn = pick(4);
    g_pool.notify = nn == 0 ? NULL : notify;
    g_pool.notify_arg = &g_pool;
    unsigned nm = pick(3);
    g_pool.nmembers = 1 + nm;
    unsigned next_dev = 1;
    for (unsigned v = 0; v < g_pool.nmembers; v++) {
        struct spool_member *m = &g_pool.m[v];
        unsigned nc = pick(4);
        m->ncopies = 1 + nc;
        unsigned rb = pick(3);
        m->region_blocks = 4u << rb;
        unsigned nb = pick(4);
        unsigned ragged = pick(3);
        m->nblocks = m->region_blocks * (2 + nb) - ragged;   /* the last region short, now and then */
        fs->mem[v].nblocks = m->nblocks;
        fs->mem[v].first_usable = v == 0 ? 2 : 1;
        fs->mem[v].copies = m->ncopies;
        for (unsigned c = 0; c < m->ncopies; c++) {
            struct pool_copy *cp = &m->copy[c];
            unsigned st = pick(10);
            cp->state = st < 5 ? POOL_COPY_ONLINE : (enum pool_copy_state)(st - 4 > 4 ? 4 : st - 4);
            unsigned nodev = pick(3);
            if (!(nodev == 0 && cp->state != POOL_COPY_ONLINE) && next_dev < 6)
                cp->dev = ref(&g_dev[next_dev++]);
            unsigned wh = pick(3);
            cp->whole = cp->state != POOL_COPY_ONLINE && wh == 0;
            unsigned cur = pick(3);
            cp->cursor = cur == 0 ? pick((unsigned)m->nblocks) : 0;
            unsigned tg = pick(3);
            cp->tag = tg == 0 ? 0 : 1 + pick(5);
            unsigned since = pick(50);
            cp->since = since;
            unsigned errs = pick(3);
            cp->write_errors = errs;
            for (unsigned r = 0; r * m->region_blocks < m->nblocks; r++) {   /* the member's regions only */
                unsigned miss = pick(4);
                g_missed[v][c][r] = cp->state != POOL_COPY_ONLINE ? miss != 0 : miss == 0 && pick(3) == 0;
            }
            for (unsigned b = 0; b < 128; b++)
                g_content[v][c][b] = (uint8_t)(b * 7 + v);
            unsigned rot = pick(4);
            if (rot == 0)
                g_content[v][c][pick(128)] ^= 0x55;   /* a source that disagrees */
        }
        /* A replace in progress, now and then: an old copy whose peer is another's tag. */
        unsigned peer = pick(4);
        if (peer == 0 && m->ncopies > 1) {
            unsigned a = pick(m->ncopies);
            unsigned b = pick(m->ncopies);
            if (a != b && m->copy[b].tag != 0)
                m->copy[a].peer = m->copy[b].tag;
        }
    }
    unsigned ns = pick(3);
    for (unsigned i = 0; i < ns && next_dev < 9; i++) {
        g_pool.spare[g_pool.nspares] = ref(&g_dev[next_dev++]);
        g_pool.spare[g_pool.nspares]->sector_size = 512;   /* a spare's was checked when it was added */
        g_pool.spare_tag[g_pool.nspares++] = 100 + i;
    }
    fs->pool = &g_pool;
    fs->nmembers = g_pool.nmembers;
    unsigned ver = pick(6);
    fs->sb.version = ver == 0 ? 3 : ver == 1 ? 10 : 11 + pick(4);
    fs->sb.generation = 1 + pick(1000);
    snprintf(fs->sb.label, sizeof(fs->sb.label), "pool%u", pick(9));
    snprintf(fs->pool_name, sizeof(fs->pool_name), "%s", fs->sb.label);
    unsigned ro = pick(12);
    fs->rdonly = ro == 0;
    unsigned mt = pick(12);
    fs->maint = mt == 0;
    unsigned md = pick(8);
    fs->mount_done = md != 0;
    unsigned fl = pick(20);
    fs->failed = fl == 0 ? -EIO : 0;
    fs->rs_thread = NULL;
    fs->rs_stop = false;
    fs->rs_buf = NULL;
    unsigned rec = pick(4);
    fs->rs_since_record = rec == 0 ? 2040 : 0;
    fs->rs_started_ns = 0;
    fs->rs_copied = 0;
    unsigned dis = pick(4);
    fs->rs_disagree = dis == 0 ? 7 : 0;
    fs->rs_region_errors = 0;
}

static void teardown(void)
{
    cfs_resilver_stop(g_cfs);
    kfree(g_cfs->rs_buf);   /* kept for the next wake when no thread could be made: the mount's to free */
    g_cfs->rs_buf = NULL;
    for (unsigned v = 0; v < g_pool.nmembers; v++)
        for (unsigned c = 0; c < POOL_MAX_COPIES; c++)
            if (g_pool.m[v].copy[c].dev != NULL)
                blkdev_put(g_pool.m[v].copy[c].dev);
    for (unsigned i = 0; i < g_pool.nspares; i++)
        blkdev_put(g_pool.spare[i]);
    free(g_pool.m);
    for (unsigned i = 0; i < NDEV; i++)
        EXPECT(g_refs[i] == (i == 0 ? 1 : 0));   /* the mount's own device */
}

static void dump(void)
{
    struct cfs *fs = g_cfs;
    note(60, g_pool.state_seq);
    note(61, g_pool.nspares);
    for (unsigned i = 0; i < g_pool.nspares; i++)
        note(62, (uint64_t)dev_index(g_pool.spare[i]) * 1000 + g_pool.spare_tag[i]);
    for (unsigned v = 0; v < g_pool.nmembers; v++) {
        note(63, g_pool.m[v].ncopies);
        note(64, fs->mem[v].copies);
        for (unsigned c = 0; c < g_pool.m[v].ncopies; c++) {
            const struct pool_copy *cp = &g_pool.m[v].copy[c];
            note(65, cp->dev ? (uint64_t)dev_index(cp->dev) : 99);
            note(66, cp->state);
            note(67, cp->whole);
            note(68, cp->cursor);
            note(69, cp->tag);
            note(70, cp->peer);
            note(71, cp->since);
            note(72, cp->read_errors + cp->write_errors * 100u + cp->csum_errors * 10000u);
            mix(g_missed[v][c], sizeof(g_missed[v][c]));
            mix(g_content[v][c], sizeof(g_content[v][c]));
        }
    }
    note(73, fs->rs_thread != NULL);
    note(74, fs->rs_stop);
    note(75, fs->rs_since_record);
    note(76, fs->rs_copied);
    note(77, fs->rs_disagree);
    note(78, fs->rs_region_errors);
    note(79, fs->rs_started_ns != 0);
}

static const char *some_name(char *buf, size_t n)
{
    unsigned k = pick(12);
    if (k == 0)
        return NULL;
    if (k == 1)
        return "";
    if (k < 5) {
        unsigned v = pick(4);
        unsigned c = pick(5);
        snprintf(buf, n, "%u:%u", v, c);
    } else if (k == 5) {
        static const char *const bad[] = { ":1", "1:", "a:0", "0:b", "12:0", "0:1x", "#:0", "0:/" };
        snprintf(buf, n, "%s", bad[pick(8)]);
    } else if (k == 6) {
        snprintf(buf, n, "nope");
    } else {
        snprintf(buf, n, "d%u", pick(NDEV - 1));   /* d0 .. d10: d11's name is "x11" */
    }
    return buf;
}

static void poolctl(void)
{
    char a[16], b[16];
    struct cosmofs_copy_info out[10];
    memset(out, 0xEE, sizeof(out));
    struct cosmofs_pool_req req;
    memset(&req, 0, sizeof(req));
    unsigned op = pick(10);
    req.op = (enum cosmofs_pool_op)(op == 9 ? 99 : op);
    req.name = some_name(a, sizeof(a));
    req.name2 = some_name(b, sizeof(b));
    if (req.op == COSMOFS_POOL_REPLACE || req.op == COSMOFS_POOL_ATTACH || req.op == COSMOFS_POOL_SPARE_ADD) {
        unsigned fresh = pick(3);
        if (fresh != 0) {
            snprintf(b, sizeof(b), "d%u", 6 + pick(5));
            req.name2 = b;
            if (req.op == COSMOFS_POOL_SPARE_ADD)
                req.name = b;
        }
    }
    if (req.op == COSMOFS_POOL_ONLINE || req.op == COSMOFS_POOL_REPLACE) {
        unsigned back = pick(2);
        if (back == 0) {
            snprintf(a, sizeof(a), "d%u", 6 + pick(5));   /* a device that may be a lagging copy */
            req.name = a;
        }
    }
    unsigned nofs = pick(40);
    g_mnt.fs_priv = nofs == 0 ? NULL : g_cfs;
    unsigned nobd = pick(6);
    g_mnt.bdev = nobd == 0 ? NULL : &g_dev[0];
    unsigned mx = pick(11);
    req.max = mx;
    req.out = out;
    unsigned fi = pick(20);
    g_fail_in = fi == 0 ? 0 : -1;
    int rc = cosmofs_poolctl(&g_mnt, &req);
    g_fail_in = -1;
    g_mnt.fs_priv = g_cfs;
    g_mnt.bdev = &g_dev[0];
    EXPECT(!g_locked);
    note(80, (uint64_t)(int64_t)rc);
    if (req.op == COSMOFS_POOL_STATUS) {
        note(81, req.count);
        note(82, req.total);
        note(83, req.generation);
        note(84, req.members * 1000u + req.format);
        note(85, req.rate);
        note(86, req.degraded * 2u + req.resilvering);
        mix(req.label, sizeof(req.label));
        mix(out, sizeof(out));
    }
}

static void run_thread(void)
{
    if (g_cfs->rs_thread == NULL || g_entry == NULL)
        return;
    unsigned w = pick(6);
    g_thread_windows = 1 + (int)w;
    g_entry(g_entry_arg);
    g_thread_windows = -1;
    __atomic_store_n(&g_cfs->rs_stop, false, __ATOMIC_RELEASE);   /* still running, as far as the mount knows */
}

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    harness_klog_hook = on_klog;
    harness_klog_min = KLOG_ERROR + 1;   /* the refusals and failures are expected, by the thousand */
    for (unsigned i = 0; i < NDEV; i++)
        snprintf(g_dev[i].name, sizeof(g_dev[i].name), i == 11 ? "x%u" : "d%u", i);
    g_cfs = calloc(1, sizeof(*g_cfs));
    EXPECT(g_cfs != NULL);
    g_cfs->mem = calloc(NMEM, sizeof(*g_cfs->mem));
    g_fs = g_cfs;
    g_mnt.fs_priv = g_cfs;
    g_mnt.bdev = ref(&g_dev[0]);
    g_cfs->mnt = &g_mnt;
    for (unsigned round = 0; round < ROUNDS; round++) {
        build();
        unsigned f = pick(3);
        g_fail_pct = f == 0 ? 0 : f == 1 ? 5 : 25;
        g_cosmofs_resilver_rate = pick(3) == 0 ? 0 : 8192;
        unsigned ops = 1 + pick(8);
        for (unsigned k = 0; k < ops; k++) {
            unsigned what = pick(20);
            if (what < 7) {
                poolctl();
            } else if (what < 9) {
                cfs_resilver_mount(g_cfs);
            } else if (what < 13) {
                unsigned blocks = pick(4);
                unsigned priv = pick(30);
                g_mnt.fs_priv = priv == 0 ? NULL : g_cfs;
                unsigned fi = pick(15);
                g_fail_in = fi == 0 ? 0 : fi == 1 ? 1 + (int)pick(3) : -1;
                /* To the end only where the end comes: a failing write would leave its region to do again, round
                 * after round, up to the seam's guard. */
                unsigned saved = g_fail_pct;
                if (blocks == 0)
                    g_fail_pct = 0;
                int rc = cosmofs_test_resilver_run(&g_mnt, blocks == 0 ? 0 : pick(40));
                g_fail_pct = saved;
                g_fail_in = -1;
                g_mnt.fs_priv = g_cfs;
                note(90, (uint64_t)(int64_t)rc);
            } else if (what < 16) {
                unsigned hold = pick(4);
                cosmofs_test_resilver_hold(hold == 0);
                run_thread();
            } else if (what < 17) {
                cfs_resilver_stop(g_cfs);
                unsigned keep = pick(2);
                if (keep == 0)
                    g_cfs->rs_stop = false;   /* else stopped for good: no wake makes another */
            } else if (what < 18) {
                unsigned i = pick(NDEV);
                g_dev[i].gone = !g_dev[i].gone;
            } else {
                unsigned fi = pick(2);
                g_fail_in = fi == 0 ? 0 : -1;
                cfs_resilver_mount(g_cfs);   /* the buffer cannot be had, now and then */
                g_fail_in = -1;
            }
            dump();
            EXPECT(!g_locked);
        }
        teardown();
        EXPECT(g_live == 0);
    }
    cosmofs_test_resilver_hold(false);
    blkdev_put(g_mnt.bdev);
    free(g_cfs->mem);
    free(g_cfs);
    EXPECT(g_live == 0);
    harness_klog_hook = NULL;
    harness_klog_min = KLOG_WARN;
    printf("  resilver digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, ROUNDS);
    EXPECT(g_digest == RESILVER_DIGEST);
}

int main(void)
{
    static const struct host_test tests[] = {
        { "resilver_digest", test_digest },
    };
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
