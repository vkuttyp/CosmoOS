/*
 * test_cfsfile.c - a cosmofs regular file's vnode operations (kernel-services/filesystem/cosmofs/cosmofs_file.cee) on
 * the host: roadmap 2.0 SA7. A digest recorded from the C over files of the live tree, a snapshot's and a clone's
 * (one that is gone), mounted or not, failed or not: pages read and written over holes, plain blocks and compressed
 * records; record-sized write-backs compressed or kept plain by the run bound; dirty pages charged against the run
 * bound and the pool's space; truncations to every kind of end, with the dirty pages written first near the bound;
 * setattr, sync and eviction of files linked and unlinked -- while the map, the reads, the writes, the inode, the
 * locks, the commit and kmalloc fail now and then. Every argument, return, page and the vnode after are in the digest;
 * the lock is let go and nothing is left allocated.
 */

#include "harness.h"

#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/mutex.h>
#include <kernel/pagecache.h>
#include <kernel/vfs.h>

#include "cosmofs_internal.h"   /* before the C library's headers, as test_cfsparse.c: its signal.h is the kernel's */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The values the CeeSafe port writes as its own constants. */
_Static_assert(EIO == 5 && ENOMEM == 12 && EINVAL == 22 && EFBIG == 27 && ENOSPC == 28 && EROFS == 30 &&
                   VNODE_REG == 1 && offsetof(struct vnode, type) == 56 && sizeof(((struct vnode *)0)->type) == 4,
               "cosmofs_file's errno.h and vfs.h values");
_Static_assert(CFS_BLOCK == 4096 && CFS_RECORD_BLOCKS == 8 && CFS_COMPRESS_NONE == 0 && CFS_ALLOC_DATA == 1 &&
                   CFS_EXT_COMPRESSED == 0x80000000u && CFS_EXT_COUNT_MASK == 0xFFFF &&
                   offsetof(struct cfs_vnode, inode) == 0,
               "cosmofs_file's cosmofs_format.h and cosmofs_internal.h values");
/* cosmofs_file.cee keeps a run in a stack slot of 2 words and an inode in one of 32. */
_Static_assert(sizeof(struct cfs_extent) <= 2 * 8 && sizeof(struct cfs_inode) <= 32 * 8,
               "cosmofs_file.cee's stack slots");

/* The digest of the C (cosmofs_file.c, 2026-11-07), which the CeeSafe port gives. */
#define FILE_DIGEST 0xde01af8468dcf4ccull
#define ROUNDS 6000u
#define SEED 0xF11EF11E2026F11Eull

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

static bool g_faults;

static bool chance(unsigned n) { return g_faults && pick(n) == 0; }

static uint64_t hash(uint64_t x)
{
    x ^= x >> 31;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 29;
    return x;
}

/* ---- the allocator: malloc, counted; an allocation fails when g_fail_in counts down to it ---- */

static int g_fail_in = -1;
static long g_live;

void *kmalloc(size_t size, unsigned flags)
{
    if (size == 0)
        return NULL;
    if (g_fail_in >= 0 && g_fail_in-- == 0) {
        note(2, size);
        return NULL;
    }
    void *p = malloc(size);
    EXPECT(p != NULL);
    memset(p, (flags & KMEM_ZERO) ? 0 : 0xA5, size);
    g_live++;
    return p;
}

void kfree(void *p)
{
    if (p != NULL)
        g_live--;
    free(p);
}

/* ---- locks ---- */

static int g_locked;

void mutex_lock(struct mutex *m)
{
    (void)m;
    EXPECT(g_locked == 0);
    g_locked++;
    note(3, 0);
}

void mutex_unlock(struct mutex *m)
{
    (void)m;
    EXPECT(g_locked == 1);
    g_locked--;
    note(4, 0);
}

/* ---- the filesystem, the vnode and its map ---- */

static struct cfs g_fs;
static struct mount g_mnt;
static struct vnode g_vn;
static bool g_snapshot;      /* the vnode's tree is a snapshot's: read-only */
static bool g_clone_gone;
static bool g_tree_fails;    /* the tree lock fails (-EROFS, a clone being left) */

#define MAPN 64u

/* What logical block i is: 0 a hole, 1 a plain block, 2 a block of the record starting at i & ~7. */
static uint8_t g_map[MAPN];

int cfs_map_ext(struct cfs *fs, const struct cfs_inode *in, uint64_t lblk, struct cfs_extent *out)
{
    EXPECT(fs == &g_fs && g_locked == 1);
    note(10, lblk);
    note(11, in->ino);
    if (chance(30))
        return -EIO;
    unsigned k = lblk < MAPN ? g_map[lblk] : 0;
    if (k == 0)
        return 0;
    if (k == 1) {
        out->start = 1000 + lblk;
        out->count = 1;
        out->lblk = (uint32_t)lblk;
    } else {
        uint64_t l0 = lblk & ~7ull;
        out->start = 5000 + l0;
        out->count = cfs_ext_pack(CFS_COMPRESS_LZ4, 1 + pick(7), CFS_RECORD_BLOCKS);
        out->lblk = (uint32_t)l0;
    }
    return 1;
}

static void page_of(uint64_t seed, void *buf)
{
    uint8_t *b = buf;
    for (unsigned i = 0; i < CFS_BLOCK; i += 8) {
        uint64_t v = hash(seed * 4099 + i);
        memcpy(b + i, &v, 8);
    }
}

int cfs_data_read_verified(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, uint64_t dva, void *buf)
{
    EXPECT(fs == &g_fs && g_locked == 1);
    note(12, lblk);
    note(13, dva);
    note(14, in->ino);
    if (chance(20))
        return -EIO;
    page_of(dva, buf);
    return 0;
}

int cfs_record_read(struct cfs *fs, struct cfs_inode *in, const struct cfs_extent *e, uint8_t *out)
{
    EXPECT(fs == &g_fs && g_locked == 1);
    (void)in;
    note(15, e->start);
    if (chance(20))
        return -EIO;
    for (unsigned i = 0; i < cfs_ext_count(e); i++)
        page_of(e->start + i, out + (size_t)i * CFS_BLOCK);
    return 0;
}

void cfs_fail(struct cfs *fs, int rc);

static int write_answer(void)
{
    unsigned w = pick(12);
    if (!g_faults || w > 3)
        return 0;
    if (w == 3) {
        g_fs.fbig_injected = true;   /* the extent-fail seam's -EFBIG */
        return -EFBIG;
    }
    int rc = w == 0 ? -EIO : w == 1 ? -ENOSPC : -ENOMEM;
    if (pick(3) == 0)
        cfs_fail(&g_fs, rc);   /* a dropped record's survivors not all written: the transaction is abandoned */
    return rc;
}

int cfs_record_write(struct cfs *fs, struct cfs_inode *in, uint64_t lblk0, const uint8_t *data, unsigned n,
                     bool compress)
{
    EXPECT(fs == &g_fs && g_locked == 1);
    note(16, lblk0);
    note(17, n);
    note(18, compress);
    note(19, in->ino);
    mix(data, (size_t)n * CFS_BLOCK);
    int rc = write_answer();
    if (rc == 0 && lblk0 + n <= MAPN)
        for (unsigned i = 0; i < n; i++)
            g_map[lblk0 + i] = compress && n == CFS_RECORD_BLOCKS ? 2 : 1;
    if (rc == 0 && pick(2)) {
        fs->map_stores++;
        fs->last_store_in = pick(3) ? in : NULL;
        fs->last_store_n = pick(5000);
    }
    return rc;
}

int cfs_data_write_block(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, const void *buf,
                         enum cfs_alloc_class cls)
{
    EXPECT(fs == &g_fs && g_locked == 1);
    note(20, lblk);
    note(21, (uint64_t)cls);
    note(22, in->ino);
    mix(buf, CFS_BLOCK);
    int rc = write_answer();
    if (rc == 0 && lblk < MAPN)
        g_map[lblk] = 1;
    if (rc == 0 && pick(2)) {
        fs->map_stores++;
        fs->last_store_in = in;
        fs->last_store_n = pick(5000);
    }
    return rc;
}

int cfs_vnode_inode_sync(struct cfs *fs, struct vnode *vn)
{
    EXPECT(fs == &g_fs && vn == &g_vn && g_locked == 1);
    note(23, vn->size);
    return chance(20) ? -EIO : 0;
}

int cfs_vnode_prepare(struct cfs *fs, struct vnode *vn)
{
    EXPECT(fs == &g_fs && vn == &g_vn && g_locked == 1);
    note(24, 0);
    return chance(20) ? -ENOMEM : 0;
}

bool cfs_tree_readonly(struct cfs *fs, struct vnode *vn)
{
    EXPECT(vn == &g_vn);
    (void)fs;
    note(25, g_snapshot);
    return g_snapshot;
}

int cfs_lock_tree_w(struct cfs *fs, struct vnode *vn)
{
    EXPECT(fs == &g_fs && vn == &g_vn && g_locked == 0);
    if (g_snapshot)
        return -EROFS;
    if (g_tree_fails && pick(3) == 0) {
        note(26, 0);
        return -ENOENT;
    }
    mutex_lock(&fs->lock);
    return 0;
}

void cfs_unlock_tree(struct cfs *fs)
{
    EXPECT(fs == &g_fs);
    mutex_unlock(&fs->lock);
}

int cfs_extents_count(struct cfs *fs, const struct cfs_inode *in, unsigned *out)
{
    EXPECT(fs == &g_fs && g_locked == 1);
    (void)in;
    note(27, 0);
    if (chance(20))
        return -EIO;
    unsigned w = pick(4);
    *out = w == 0 ? CFS_MAX_EXTENTS - pick(40) : w == 1 ? CFS_MAX_EXTENTS * 3 / 4 + pick(20) : pick(1000);
    return 0;
}

int cfs_truncate_blocks(struct cfs *fs, struct cfs_inode *in, uint64_t keep)
{
    EXPECT(fs == &g_fs && g_locked == 1);
    note(28, keep);
    note(29, in->ino);
    if (chance(15))
        return -EIO;
    for (uint64_t i = keep; i < MAPN; i++)
        g_map[i] = 0;
    return 0;
}

int cfs_commit(struct cfs *fs)
{
    EXPECT(fs == &g_fs && g_locked == 1);
    note(30, 0);
    return chance(10) ? -EIO : 0;
}

struct cfs_clone *cfs_clone_by_id(struct cfs *fs, uint64_t id)
{
    EXPECT(fs == &g_fs && g_locked == 1);
    note(31, id);
    static struct cfs_clone c;
    return g_clone_gone ? NULL : &c;
}

int cfs_inode_write(struct cfs *fs, uint64_t ino, const struct cfs_inode *in)
{
    EXPECT(fs == &g_fs && g_locked == 1);
    note(32, ino);
    mix(in, sizeof(*in));
    return chance(10) ? -EIO : 0;
}

void cfs_orphan_remove(struct cfs *fs, uint64_t ino)
{
    EXPECT(fs == &g_fs && g_locked == 1);
    note(33, ino);
}

void cfs_fail(struct cfs *fs, int rc)
{
    EXPECT(fs == &g_fs);
    note(34, (uint64_t)(int64_t)rc);
    if (fs->failed == 0)
        fs->failed = rc;
}

void pagecache_truncate(struct vnode *vn, uint64_t size)
{
    EXPECT(vn == &g_vn && g_locked == 0);
    note(35, size);
}

int pagecache_sync(struct vnode *vn)
{
    EXPECT(vn == &g_vn && g_locked == 0);
    note(36, vn->pc.nr_dirty);
    if (chance(5))
        return -EIO;
    vn->pc.nr_dirty = 0;
    return 0;
}

uint64_t vfs_now_ns(void) { return 777; }

/* ---- a round ---- */

static struct cfs_vnode *cv(void) { return g_vn.fs_priv; }

static void vnode(void)
{
    memset(&g_fs, 0, sizeof(g_fs));
    memset(&g_mnt, 0, sizeof(g_mnt));
    memset(&g_vn, 0, sizeof(g_vn));
    g_mnt.fs_priv = pick(30) == 0 ? NULL : &g_fs;
    g_fs.failed = pick(25) == 0 ? -EIO : 0;
    g_fs.free_blocks = pick(3) == 0 ? pick(40) : 1000 + pick(100000);
    g_fs.reserve = pick(30);
    g_fs.map_stores = pick(3);
    g_fs.sb.inode_count = pick(3);
    g_mnt.cache_dirty = pick(3) == 0 ? pick(10000) : pick(10);
    g_vn.mnt = &g_mnt;
    g_vn.type = pick(6) == 0 ? VNODE_DIR : VNODE_REG;
    g_vn.ino = 7;
    g_vn.nlink = pick(3) == 0 ? 0 : 1;
    g_vn.size = (uint64_t)pick(MAPN) * CFS_BLOCK + (pick(2) ? pick(CFS_BLOCK) : 0);
    g_vn.pc.nr_dirty = pick(4) == 0 ? pick(3000) : pick(4);
    struct cfs_vnode *v = kmalloc(sizeof(*v), KMEM_ZERO);
    g_vn.fs_priv = v;
    if (pick(40) == 0) {
        cfs_evict(&g_vn);   /* the reference came and went before any of it was this filesystem's */
        g_vn.fs_priv = NULL;
        cfs_evict(&g_vn);
        v = kmalloc(sizeof(*v), KMEM_ZERO);
        g_vn.fs_priv = v;
    }
    v->inode.ino = 7;
    v->inode.compress_algo = pick(3) ? CFS_COMPRESS_LZ4 : CFS_COMPRESS_NONE;
    unsigned t = pick(8);
    v->snap_tag = t == 0 ? 0xFFFFu : t == 1 ? 3 : 0;
    v->runs_valid = pick(2);
    v->runs_at = pick(3);
    v->runs = pick(4200);
    g_snapshot = pick(10) == 0;
    g_clone_gone = pick(2);
    g_tree_fails = g_faults && pick(8) == 0;
    for (unsigned i = 0; i < MAPN; i++) {
        unsigned w = pick(4);
        g_map[i] = w == 0 ? 0 : w == 1 ? 2 : 1;
    }
    for (unsigned r = 0; r < MAPN; r += CFS_RECORD_BLOCKS) {   /* a record is all of its eight blocks */
        bool rec = g_map[r] == 2;
        for (unsigned i = r; i < r + CFS_RECORD_BLOCKS; i++)
            if (rec || g_map[i] == 2)
                g_map[i] = rec ? 2 : 1;
    }
}

static void after(int rc)
{
    note(50, (uint64_t)(int64_t)rc);
    note(51, g_vn.size);
    note(52, g_vn.mtime_ns);
    note(53, (uint64_t)(int64_t)g_fs.failed);
    note(54, g_fs.sb.inode_count);
    note(55, g_fs.fbig_injected);
    if (g_vn.fs_priv != NULL) {
        note(56, cv()->runs);
        note(57, cv()->runs_at);
        note(58, cv()->runs_valid);
    }
    mix(g_map, sizeof(g_map));
    EXPECT(g_locked == 0);
}

static void op(void)
{
    unsigned w = pick(10);
    note(40, w);
    unsigned kf = pick(8);
    g_fail_in = g_faults && kf == 0 ? (int)pick(3) : -1;
    static uint8_t pages[CFS_RECORD_BLOCKS][CFS_BLOCK];
    int rc = 0;
    uint64_t index = pick(10) == 0 ? 0x100000000ull + pick(3) : pick(MAPN);
    switch (w) {
    case 0: {
        uint8_t buf[CFS_BLOCK];
        memset(buf, 0x11, sizeof(buf));
        if (index >= MAPN)
            index = pick(MAPN);
        rc = cfs_readpage(&g_vn, index, buf);
        if (rc == 0)
            mix(buf, sizeof(buf));
        break;
    }
    case 1:
        if (index >= MAPN)
            index = pick(MAPN);
        page_of(next(), pages[0]);
        rc = cfs_writepage(&g_vn, index, pages[0]);
        break;
    case 2: {
        unsigned n = pick(3) ? CFS_RECORD_BLOCKS + pick(4) : 1 + pick(CFS_RECORD_BLOCKS);
        if (n > CFS_RECORD_BLOCKS)
            n = CFS_RECORD_BLOCKS;
        uint64_t at = pick(3) ? pick(MAPN / CFS_RECORD_BLOCKS) * CFS_RECORD_BLOCKS : pick(MAPN);
        void *ptrs[CFS_RECORD_BLOCKS];
        for (unsigned i = 0; i < CFS_RECORD_BLOCKS; i++) {
            page_of(next(), pages[i]);
            ptrs[i] = pages[i];
        }
        unsigned done = 99;
        rc = cfs_writepages(&g_vn, at, ptrs, n, &done);
        note(41, done);
        break;
    }
    case 3: {
        unsigned dirty = pick(4) == 0 ? pick(3000) : pick(10);
        rc = cfs_dirty_check(&g_vn, index, dirty);
        break;
    }
    case 4: {
        uint64_t size = pick(4) == 0 ? (uint64_t)pick(MAPN) * CFS_BLOCK : pick(MAPN * CFS_BLOCK);
        rc = cfs_truncate(&g_vn, size);
        break;
    }
    case 5:
        rc = cfs_vnode_setattr(&g_vn);
        break;
    case 6:
        rc = cfs_vnode_sync(&g_vn);
        break;
    case 7: {
        uint64_t bad = 99;
        rc = cosmofs_test_verify_file(&g_vn, &bad);
        note(42, bad);
        break;
    }
    case 8: {
        unsigned n = 99;
        rc = cosmofs_test_runs(&g_vn, &n);
        note(43, n);
        break;
    }
    default:
        cfs_evict(&g_vn);
        EXPECT(g_vn.fs_priv == NULL);
        break;
    }
    g_fail_in = -1;
    after(rc);
}

static void round_(void)
{
    vnode();
    unsigned ops = 1 + pick(6);
    for (unsigned k = 0; k < ops && g_vn.fs_priv != NULL; k++) {
        if (g_fs.fbig_injected && pick(2))
            g_fs.fbig_injected = false;
        op();
    }
    if (g_vn.fs_priv != NULL) {
        g_mnt.fs_priv = &g_fs;
        cfs_evict(&g_vn);
    }
    EXPECT(g_live == 0);
}

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    harness_klog_hook = on_klog;
    harness_klog_min = KLOG_ERROR + 1;
    for (unsigned round = 0; round < ROUNDS; round++) {
        unsigned f = pick(3);
        g_faults = f != 0;
        round_();
    }
    harness_klog_hook = NULL;
    harness_klog_min = KLOG_WARN;
    printf("  file digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, ROUNDS);
    EXPECT(g_digest == FILE_DIGEST);
}

int main(void)
{
    static const struct host_test tests[] = {
        { "file_digest", test_digest },
    };
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
