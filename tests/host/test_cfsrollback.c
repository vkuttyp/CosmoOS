/*
 * test_cfsrollback.c - cosmofs's rollback (kernel-services/filesystem/cosmofs/cosmofs_rollback.cee) on the host: roadmap
 * 2.0 SA7. A digest recorded from the C over seeded rounds against a snapshot list, trees, inodes, an orphan record,
 * deadlists, blocks and a transaction the test owns; the record's own checks (cfs_rollback_parse and
 * cfs_rollback_crc, cfsparse) are the real ones. Requests (to snapshots, clones and nothing, with safety names given,
 * taken, too long and drawn from the clock, under a pending record and a hold), cancels (back to a hold or not), the
 * checker's drop, BOOT_OK, the protection a record gives a snapshot, and the switch at mount -- records that do not
 * check out, targets and safety snapshots missing, read-only mounts, targets with no root directory, walks and
 * deadlist filters and inode reads that fail, the target's orphans and an orphan record that is full -- with every
 * commit and allocation able to fail. Every return, every block freed, every commit, the record's bytes, the roots and
 * every logged line are in the digest; every buffer taken is given back and nothing is left allocated.
 */

#include "harness.h"

#include <kernel/blk.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/mutex.h>
#include <kernel/vfs.h>

#include "cosmofs_internal.h"   /* before the C library's headers, as test_cfsparse.c: its signal.h is the kernel's */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The values the CeeSafe port writes as its own constants. */
_Static_assert(EIO == 5 && ENOENT == 2 && EINVAL == 22 && EEXIST == 17 && ENOMEM == 12 && EOPNOTSUPP == 95 &&
                   EBUSY == 16 && ENAMETOOLONG == 36 && CFS_SNAP_F_CLONE == 1 && CFS_DVA_NONE == 0xFFFFFFFFFFFFFFFFull &&
                   CFS_KIND_ROLLBACK == 16 && CFS_MHDR_SIZE == 32 && CFS_ROLLBACK_MAGIC == 0x42524643u &&
                   CFS_ROLLBACK_PENDING == 1 && CFS_ROLLBACK_HELD == 2 && CFS_ROLLBACK_F_FROM_HELD == 1 &&
                   CFS_ROOT_INO == 1 && CFS_TYPE_DIR == 2 && CFS_SNAP_NAME_MAX == 31 && MOUNT_RDONLY == 1,
               "cosmofs_rollback's errno.h, vfs.h and cosmofs_format.h values");
_Static_assert(sizeof(struct cfs_rollback) <= 16 * 8 && sizeof(struct cfs_snapshot) <= 16 * 8 &&
                   sizeof(struct cfs_inode) <= 32 * 8,
               "cosmofs_rollback.cee keeps a record and an entry in 16 words, an inode in 32");

/* The digest of the C (cosmofs_rollback.c, 2026-11-06), which the CeeSafe port gives. */
#define ROLLBACK_DIGEST 0x06458013d8846511ull
#define ROUNDS 60000u
#define SEED 0xC05F5FA2026B0017ull

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

void *kzalloc(size_t size) { return kmalloc(size, KMEM_ZERO); }

void kfree(void *p)
{
    if (p != NULL)
        g_live--;
    free(p);
}

/* ---- the lock, the clock, the transaction ---- */

static bool g_locked;
static unsigned g_fail_pct;   /* of the transaction's and the lists' operations that fail */

void mutex_lock(struct mutex *m)
{
    (void)m;
    EXPECT(!g_locked);
    g_locked = true;
    note(3, 0);
}

void mutex_unlock(struct mutex *m)
{
    (void)m;
    EXPECT(g_locked);
    g_locked = false;
    note(4, 0);
}

static uint64_t g_now = 1700000000000000000ull;

uint64_t clock_realtime_ns(void)
{
    g_now += pick(4) == 0 ? 0 : 400000000ull;   /* two requests in one second, now and then */
    return g_now;
}

static int draw_rc(void)
{
    unsigned r = pick(100);
    if (r >= g_fail_pct)
        return 0;
    return pick(2) == 0 ? -EIO : -ENOMEM;
}

int cfs_commit(struct cfs *fs)
{
    note(5, fs->switching);
    note(6, fs->sb.rollback_root);
    int rc = draw_rc();
    note(7, (uint64_t)(int64_t)rc);
    if (rc == 0) {
        fs->sb.generation = fs->gen;
        fs->gen++;
    }
    return rc;
}

void cfs_fail(struct cfs *fs, int rc)
{
    note(8, (uint64_t)(int64_t)rc);
    if (!fs->failed)
        fs->failed = rc;
}

void cfs_free_block_exempt(struct cfs *fs, uint64_t blk)
{
    (void)fs;
    note(9, blk);
}

void cfs_free_block_deferred(struct cfs *fs, uint64_t blk)
{
    (void)fs;
    note(10, blk);
}

/* ---- the blocks: rollback records (and blocks of other kinds a damaged pointer can name) ---- */

#define NSTORE 64u

static uint8_t g_store[NSTORE][CFS_BLOCK];
static bool g_bad[NSTORE];
static unsigned g_next_blk;
static long g_held;

static int lin_of(uint64_t dva) { return CFS_DVA_VDEV(dva) == 0 && CFS_DVA_BLK(dva) >= 100 && CFS_DVA_BLK(dva) < 100 + NSTORE ? (int)(CFS_DVA_BLK(dva) - 100) : -1; }

int cfs_buf_get(struct cfs *fs, uint64_t blkno, uint32_t kind, struct cfs_buf **out)
{
    (void)fs;
    note(11, blkno);
    int i = lin_of(blkno);
    if (i < 0 || g_bad[i])
        return -EIO;
    if (kind != 0 && ((struct cfs_mhdr *)g_store[i])->kind != kind)
        return -EIO;
    struct cfs_buf *b = calloc(1, sizeof(*b));
    EXPECT(b != NULL);
    b->blkno = blkno;
    b->data = g_store[i];
    g_held++;
    *out = b;
    return 0;
}

void cfs_buf_put(struct cfs *fs, struct cfs_buf *b)
{
    (void)fs;
    note(12, b->blkno);
    g_held--;
    free(b);
}

int cfs_buf_new(struct cfs *fs, uint32_t kind, struct cfs_buf **out)
{
    (void)fs;
    int rc = draw_rc();
    note(13, (uint64_t)(int64_t)rc);
    if (rc)
        return rc;
    unsigned i = g_next_blk++ % NSTORE;
    memset(g_store[i], 0, CFS_BLOCK);
    g_bad[i] = false;
    ((struct cfs_mhdr *)g_store[i])->kind = kind;
    struct cfs_buf *b = calloc(1, sizeof(*b));
    EXPECT(b != NULL);
    b->blkno = CFS_DVA(0, 100 + i);
    b->data = g_store[i];
    g_held++;
    *out = b;
    return 0;
}

/* ---- the snapshot list ---- */

#define MAXS 20

static struct cfs_snapshot g_snaps[MAXS];
static unsigned g_nsnaps;
static int g_last_found = -1;   /* the entry the last find answered with */
static uint64_t g_max_id;

static void draw_uuid(uint8_t out[16])
{
    for (unsigned i = 0; i < 16; i++)
        out[i] = (uint8_t)pick(4);   /* few values: two snapshots share one now and then */
}

static struct cfs_snapshot *add_entry(const char *name, uint32_t flags, const uint8_t *uuid, uint64_t imap_root)
{
    if (g_nsnaps == MAXS)
        return NULL;
    struct cfs_snapshot *s = &g_snaps[g_nsnaps++];
    memset(s, 0, sizeof(*s));
    s->id = ++g_max_id;
    s->flags = flags;
    if (uuid)
        memcpy(s->uuid, uuid, 16);
    else
        draw_uuid(s->uuid);
    s->imap_root = imap_root;
    s->next_ino = 2 + pick(12);
    s->inode_count = pick(20);
    s->generation = pick(1000);
    snprintf(s->name, sizeof(s->name), "%s", name);
    return s;
}

bool cfs_snapshot_find(struct cfs *fs, const char *name, struct cfs_snapshot *out)
{
    (void)fs;
    note(20, strlen(name));
    for (unsigned i = 0; i < g_nsnaps; i++)
        if (strcmp(g_snaps[i].name, name) == 0) {
            if (out)
                *out = g_snaps[i];
            g_last_found = (int)i;
            return true;
        }
    return false;
}

int cfs_snapshot_lookup_uuid(struct cfs *fs, const uint8_t *uuid, struct cfs_snapshot *out)
{
    (void)fs;
    if (pick(40) == 0)
        return -EIO;
    for (unsigned i = 0; i < g_nsnaps; i++)
        if (memcmp(g_snaps[i].uuid, uuid, 16) == 0) {
            *out = g_snaps[i];
            return 0;
        }
    return -ENOENT;
}

void cfs_snapshot_new_uuid(struct cfs *fs, uint8_t out[16], const uint8_t (*also)[16], unsigned nalso)
{
    (void)fs;
    note(21, also == NULL ? nalso : 1000 + nalso);
    draw_uuid(out);
}

int cfs_snapshot_create_ex(struct cfs *fs, const char *name, bool identity, bool commit, const uint8_t *uuid,
                           struct cfs_snapshot *out)
{
    note(22, identity);
    note(23, commit);
    mix(name, strlen(name));
    int rc = draw_rc();
    note(24, (uint64_t)(int64_t)rc);
    if (rc)
        return rc;
    if (pick(30) == 0 && fs->sb.version < 12)
        fs->sb.version = 12;   /* the raise the creation can make */
    struct cfs_snapshot *s = add_entry(name, 0, uuid, fs->sb.imap_root);
    if (s == NULL)
        return -ENOSPC;
    if (pick(25) == 0 && g_last_found >= 0 && (unsigned)g_last_found < g_nsnaps - 1)
        snprintf(g_snaps[g_last_found].name, sizeof(g_snaps[0].name), "gone%u", pick(100));   /* the target, read again, missing */
    *out = *s;
    return 0;
}

int cfs_snapshot_deadlists_filter(struct cfs *fs, bool (*live)(void *arg, uint64_t dva), void *arg,
                                  uint64_t *revived)
{
    (void)fs;
    int rc = draw_rc();
    note(25, (uint64_t)(int64_t)rc);
    if (rc)
        return rc;
    uint64_t n = 0;
    unsigned k = pick(12);
    for (unsigned i = 0; i < k; i++) {
        unsigned w = pick(10);
        uint64_t dva = w == 0 ? CFS_DVA(3, pick(10)) : CFS_DVA(0, pick(300));
        bool on = live(arg, dva);
        note(26, on);
        n += on ? 1 : 0;
    }
    *revived = n;
    return 0;
}

/* ---- the trees, the inodes, the orphans: what a root holds follows from the root ---- */

#define NB 300u

static uint64_t mixhash(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    return x;
}

/* cfs_dva_lin is cfsparse's, over the one member test_digest gives the filesystem. */

uint64_t cfs_lin_dva(const struct cfs *fs, uint64_t lin)
{
    (void)fs;
    return lin < NB && lin % 50 != 49 ? CFS_DVA(0, lin) : CFS_DVA_NONE;
}

int cfs_tree_walk(struct cfs *fs, uint64_t imap_root, uint64_t next_ino, void (*mark)(void *arg, uint64_t lin),
                  void *arg)
{
    (void)fs;
    note(30, imap_root);
    note(31, next_ino);
    if (imap_root == 0)
        return 0;
    unsigned n = (unsigned)(mixhash(imap_root) % 30);
    for (unsigned k = 0; k < n; k++)
        mark(arg, mixhash(imap_root * 131 + k) % (NB + 3));   /* past the set's end, now and then */
    return imap_root % 17 == 0 ? -EIO : 0;
}

int cfs_inode_read(struct cfs *fs, uint64_t ino, struct cfs_inode *out)
{
    note(32, fs->sb.imap_root);
    note(33, ino);
    uint64_t h = mixhash(fs->sb.imap_root);
    if (h % 13 == 0)
        return -EIO;
    memset(out, 0, sizeof(*out));
    out->mode = h % 6 == 0 ? (1u << 12) : (CFS_TYPE_DIR << 12);
    return 0;
}

int cfs_inode_read_raw(struct cfs *fs, uint64_t ino, struct cfs_inode *out)
{
    uint64_t h = mixhash(fs->sb.imap_root * 977 + ino);
    note(34, ino);
    if (h % 40 == 0)
        return -EIO;
    if (h % 3 == 0)
        return -ENOENT;
    memset(out, 0, sizeof(*out));
    out->mode = h % 7 == 0 ? 0 : (1u << 12);
    out->nlink = h % 4 == 0 ? 0 : 1;
    out->ino = ino;
    return 0;
}

bool cfs_orphan_reserve(struct cfs *fs)
{
    (void)fs;
    bool ok = pick(15) != 0;
    note(35, ok);
    return ok;
}

void cfs_orphan_add(struct cfs *fs, uint64_t ino)
{
    note(36, ino);
    fs->nr_orphans++;
}

/* ---- the rounds ---- */

static struct blkdev g_bdev;
static struct mount g_mnt;

/* A record of `state` written straight into a block, as a commit left it; damaged now and then. */
static void plant_record(struct cfs *fs, unsigned state)
{
    unsigned i = g_next_blk++ % NSTORE;
    memset(g_store[i], 0, CFS_BLOCK);
    g_bad[i] = pick(20) == 0;
    unsigned k = pick(20);
    ((struct cfs_mhdr *)g_store[i])->kind = k == 0 ? CFS_KIND_INODES : CFS_KIND_ROLLBACK;
    struct cfs_rollback r;
    memset(&r, 0, sizeof(r));
    r.state = state;
    r.flags = pick(3) == 0 ? CFS_ROLLBACK_F_FROM_HELD : 0;
    if (g_nsnaps > 0 && pick(5) != 0)
        memcpy(r.target_uuid, g_snaps[pick(g_nsnaps)].uuid, 16);
    else
        draw_uuid(r.target_uuid);
    if (g_nsnaps > 0 && pick(5) != 0)
        memcpy(r.safety_uuid, g_snaps[pick(g_nsnaps)].uuid, 16);
    else
        draw_uuid(r.safety_uuid);
    r.target_id = pick(30);
    r.safety_id = pick(30);
    r.generation = pick(100);
    r.requested_ns = g_now;
    cfs_rollback_seal(&r);
    if (pick(15) == 0)
        r.reserved[0] = 1;   /* after the seal: it does not check out */
    memcpy(g_store[i] + CFS_MHDR_SIZE, &r, sizeof(r));
    fs->sb.rollback_root = CFS_DVA(0, 100 + i);
}

static void dump(struct cfs *fs)
{
    note(40, fs->sb.version);
    note(41, fs->sb.rollback_root);
    note(42, fs->sb.imap_root);
    note(43, fs->sb.next_ino);
    note(44, fs->sb.inode_count);
    note(45, (uint64_t)(int64_t)fs->failed);
    note(46, fs->nr_orphans);
    struct cfs_rollback r;
    int rc = cfs_rollback_read(fs, &r);
    note(47, (uint64_t)(int64_t)rc);
    if (rc == 0) {
        r.requested_ns = 0;   /* the clock's, which the test draws anyway */
        mix(&r, sizeof(r));
    }
}

static void request(struct cfs *fs)
{
    char target[48], safety[48];
    unsigned k = pick(4);
    if (k != 0 && g_nsnaps > 0)
        snprintf(target, sizeof(target), "%s", g_snaps[pick(g_nsnaps)].name);
    else
        snprintf(target, sizeof(target), "t%u", pick(30));
    unsigned s = pick(6);
    const char *sp = safety;
    if (s == 0)
        snprintf(safety, sizeof(safety), "a-safety-name-that-is-far-too-long-%u", pick(9));
    else if (s == 1 && g_nsnaps > 0)
        snprintf(safety, sizeof(safety), "%s", g_snaps[pick(g_nsnaps)].name);
    else if (s == 2)
        snprintf(safety, sizeof(safety), "keep-%u", pick(30));
    else
        sp = NULL;
    if (pick(20) == 0)
        g_fail_in = 0;
    struct cfs_snapshot out;
    memset(&out, 0, sizeof(out));
    struct cfs_snapshot *op = pick(4) == 0 ? NULL : &out;
    int rc = cfs_rollback_request(fs, target, sp, op);
    g_fail_in = -1;
    note(50, (uint64_t)(int64_t)rc);
    if (rc == 0 && op != NULL)
        note(51, out.id);
}

static void at_mount(struct cfs *fs)
{
    unsigned m = pick(5);
    fs->mnt = m == 0 ? NULL : &g_mnt;
    g_mnt.bdev = m == 1 ? NULL : &g_bdev;
    g_mnt.flags = m == 2 ? MOUNT_RDONLY : 0;
    if (pick(25) == 0)
        g_fail_in = 0;
    bool switched = true;
    int rc = cfs_rollback_at_mount(fs, &switched);
    g_fail_in = -1;
    note(52, (uint64_t)(int64_t)rc);
    note(53, switched);
    EXPECT(!g_locked && !fs->switching);
}

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    harness_klog_hook = on_klog;
    harness_klog_min = KLOG_ERROR + 1;   /* refusals and failed commits are expected, by the hundred */
    snprintf(g_bdev.name, sizeof(g_bdev.name), "vdb");
    struct cfs *fs = calloc(1, sizeof(*fs));
    EXPECT(fs != NULL);
    fs->nblocks = NB;
    static struct cfs_memstate mem0 = { .nblocks = NB, .first_usable = 2, .base = 0 };
    fs->mem = &mem0;
    fs->nmembers = 1;
    for (unsigned round = 0; round < ROUNDS; round++) {
        if (round % 3000 == 0) {
            /* A new filesystem: its list, its version, its failure rate. */
            g_nsnaps = 0;
            fs->failed = 0;
            fs->nr_orphans = 0;
            fs->sb.rollback_root = 0;
            unsigned v = round / 3000 % 5;
            fs->sb.version = v == 0 ? 10 : v == 1 ? 11 : 12 + pick(3);
            unsigned f = pick(3);
            g_fail_pct = f == 0 ? 0 : f == 1 ? 4 : 20;
            fs->sb.imap_root = 1 + pick(400);
            fs->sb.next_ino = 2 + pick(12);
            fs->sb.inode_count = pick(20);
        }
        unsigned what = pick(100);
        if (what < 10) {
            char name[32];
            snprintf(name, sizeof(name), "t%u", pick(30));
            uint32_t flags = pick(5) == 0 ? CFS_SNAP_F_CLONE : 0;   /* each draw a statement: argument order is the compiler's */
            uint64_t root = 1 + pick(400);
            add_entry(name, flags, NULL, root);
        } else if (what < 30) {
            request(fs);
        } else if (what < 38) {
            note(54, (uint64_t)(int64_t)cfs_rollback_cancel(fs));
        } else if (what < 42) {
            note(55, (uint64_t)(int64_t)cfs_rollback_drop(fs));
        } else if (what < 50) {
            bool released = true;
            int rc = cfs_rollback_boot_ok(fs, &released);
            note(56, (uint64_t)(int64_t)rc);
            note(57, released);
        } else if (what < 58) {
            if (g_nsnaps > 0)
                note(58, cfs_rollback_protects(fs, &g_snaps[pick(g_nsnaps)]));
        } else if (what < 76) {
            at_mount(fs);
        } else if (what < 86) {
            plant_record(fs, pick(5) == 0 ? 3 : 1 + pick(2));
        } else if (what < 88) {
            fs->sb.rollback_root = pick(2) == 0 ? 0 : CFS_DVA(0, 5);   /* none, or a pointer to nothing */
        } else if (what < 90) {
            fs->failed = 0;
        } else {
            dump(fs);
        }
        EXPECT(g_held == 0 && !g_locked);
    }
    dump(fs);
    free(fs);
    EXPECT(g_live == 0);
    harness_klog_hook = NULL;
    harness_klog_min = KLOG_WARN;
    printf("  rollback digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, ROUNDS);
    EXPECT(g_digest == ROLLBACK_DIGEST);
}

int main(void)
{
    static const struct host_test tests[] = {
        { "rollback_digest", test_digest },
    };
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
