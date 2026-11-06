/*
 * test_cfscheck.c - cosmofs's structural check (kernel-services/filesystem/cosmofs/cosmofs_check.c) on the host:
 * roadmap 2.0 SA7. A digest recorded from the C over seeded filesystems the test builds and owns, on one member or
 * two: an inode map of one or two levels with slots that do and do not name themselves, files whose runs are in
 * order, out of order, overlapping and compressed, extent chains (one in a loop), checksum trees of one and two
 * levels, directories written with the format's own entry encoder -- long names, repeats, names whose hashes collide,
 * malformed and dangling entries, a directory with two parents, a tree too deep -- snapshots and clones with their
 * member tables and deadlists, rollback records and holds, the free and orphan records, the open transaction's
 * pending blocks, and an allocation bitmap that does and does not agree. The check runs on a mount, plain, repairing,
 * offline and at boot, and on an unmounted device whose superblock slots, mount and pool vary; the page allocator and
 * kmalloc fail at every point. Every call the check makes, every argument, every return, every logged line and every
 * field of its reports are in the digest; nothing is left allocated, no page is left taken and no buffer is left held.
 */

#include "harness.h"

#include <kernel/blk.h>
#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/mutex.h>
#include <kernel/pmm.h>
#include <kernel/storage.h>
#include <kernel/timer.h>
#include <kernel/vfs.h>
#include <uapi/cosmo/fsctl.h>

#include "cosmofs_internal.h"   /* before the C library's headers, as test_cfsparse.c: its signal.h is the kernel's */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The digest of the C (cosmofs_check.c, 2026-11-06), which the CeeSafe port gives. */
#define CHECK_DIGEST 0xe84d1de85355a449ull
#define ROUNDS 6000u
#define SEED 0xC4EC4C0FF5EE2026ull

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

/*
 * Faults in this round's filesystem: none, so that a sound one is checked too; only those a repair may act on, so
 * that the repairs run; or any, now and then.
 */
static unsigned g_level;

static unsigned g_sparse;   /* faults this many times rarer, so that one class can be found alone */

static bool chance(unsigned n) { return g_level >= 2 && pick(n * g_sparse) == 0; }

static bool fixable(unsigned n) { return g_level >= 1 && pick(n * g_sparse) == 0; }

/* A round with one fault and no other: entries naming nothing (1), or runs out of order (2). */
static unsigned g_solo;

static bool solo(unsigned which, unsigned n) { return g_solo == which && pick(n) == 0; }

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
    void *p = malloc(size);
    EXPECT(p != NULL);
    memset(p, (flags & KMEM_ZERO) ? 0 : 0xA5, size);   /* never read before written: a fixed pattern if it were */
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

/* ---- the pool: up to two members of NB blocks, every 60th block padding with no address ---- */

#define NB 512u
#define NMEM 2u

struct block {
    _Alignas(8) uint8_t data[CFS_BLOCK];
    bool used;    /* the builder handed it out */
    bool alloc;   /* the allocation bitmap's bit */
};

static struct block g_blk[NMEM * NB];
static unsigned g_nmem;
static unsigned g_fail_pct;   /* of reads that fail outright */
static unsigned g_bufs;       /* buffers held */
static bool g_locked;

static bool padding(uint64_t b) { return b % 60 == 59; }

/* The block a DVA names in g_blk, or CFS_DVA_NONE. */
static uint64_t real_lin(uint64_t dva)
{
    unsigned v = CFS_DVA_VDEV(dva);
    uint64_t b = CFS_DVA_BLK(dva);
    if (v >= g_nmem || b >= NB || padding(b))
        return CFS_DVA_NONE;
    return v * NB + b;
}

uint64_t cfs_lin_dva(const struct cfs *fs, uint64_t lin)
{
    EXPECT(lin < fs->nblocks);
    unsigned v = (unsigned)(lin / NB);
    uint64_t b = lin % NB;
    if (v >= g_nmem || padding(b))
        return CFS_DVA_NONE;
    return CFS_DVA(v, b);
}

bool cfs_bitmap_test(const struct cfs *fs, uint64_t lin)
{
    EXPECT(lin < fs->nblocks);
    return g_blk[lin].alloc;
}

void cfs_bitmap_claim(struct cfs *fs, uint64_t lin)
{
    (void)fs;
    note(3, lin);
    EXPECT(!g_blk[lin].alloc);
    g_blk[lin].alloc = true;
}

void cfs_free_block_deferred(struct cfs *fs, uint64_t blk)
{
    (void)fs;
    EXPECT(g_locked);
    note(4, blk);
}

static struct cfs_mhdr *hdr_of(uint64_t lin) { return (struct cfs_mhdr *)g_blk[lin].data; }

static uint64_t *ptrs_of(uint64_t lin) { return (uint64_t *)(g_blk[lin].data + CFS_MHDR_SIZE); }

static int draw_fail(void)
{
    unsigned r = pick(100);
    return r < g_fail_pct ? -EIO : 0;
}

int cfs_buf_get(struct cfs *fs, uint64_t blkno, uint32_t kind, struct cfs_buf **out)
{
    (void)fs;
    EXPECT(g_locked);
    note(10, blkno);
    note(11, kind);
    uint64_t lin = real_lin(blkno);
    int rc = draw_fail();
    if (rc == 0 && (lin == CFS_DVA_NONE || (kind != 0 && hdr_of(lin)->kind != kind)))
        rc = -EIO;
    note(12, (uint64_t)(int64_t)rc);
    if (rc)
        return rc;
    struct cfs_buf *b = calloc(1, sizeof(*b));
    EXPECT(b != NULL);
    b->blkno = blkno;
    b->data = g_blk[lin].data;
    b->refs = 1;
    g_bufs++;
    *out = b;
    return 0;
}

void cfs_buf_put(struct cfs *fs, struct cfs_buf *b)
{
    (void)fs;
    note(13, b->blkno);
    EXPECT(g_bufs > 0);
    g_bufs--;
    free(b);
}

struct cfs_mhdr *cfs_buf_hdr(struct cfs_buf *b) { return (struct cfs_mhdr *)b->data; }

/* ---- the snapshot list: SLOTS entries of a block, read by the real cfs_snaplist_get ---- */

#define SLOTS 3u

uint32_t cfs_snaplist_kind(const struct cfs *fs) { return fs->sb.version >= 12 ? CFS_KIND_SNAPLIST2 : CFS_KIND_SNAPLIST; }

unsigned cfs_snaplist_slots(const struct cfs *fs)
{
    (void)fs;
    return SLOTS;
}

uint64_t cfs_snaplist_next(const struct cfs_buf *b) { return *(const uint64_t *)(b->data + CFS_MHDR_SIZE); }

static struct cfs_snap_block2 *list_of(uint64_t lin) { return (struct cfs_snap_block2 *)(g_blk[lin].data + CFS_MHDR_SIZE); }

/* Entry `i` of list block `lin`, in the layout of the pool's version. */
static void put_entry(unsigned version, uint64_t lin, unsigned i, const struct cfs_snapshot *e)
{
    if (version >= 12) {
        list_of(lin)->snap[i] = *e;
        return;
    }
    struct cfs_snapshot_v11 *o = &((struct cfs_snap_block *)(g_blk[lin].data + CFS_MHDR_SIZE))->snap[i];
    memcpy(o, e, offsetof(struct cfs_snapshot_v11, name));
    memcpy(o->name, e->name, sizeof(o->name));
}

void cfs_tree_roots(struct cfs *fs, const struct cfs_snapshot *s, uint64_t *imap_root, uint64_t *next_ino)
{
    (void)fs;
    note(14, s->id);
    *imap_root = s->imap_root;
    *next_ino = s->next_ino;
}

/* Every snapshot the builder wrote, for the lookups by uuid. */
#define MAXSNAP 16u
static struct cfs_snapshot g_snaps[MAXSNAP];
static unsigned g_nsnaps;

int cfs_snapshot_lookup_uuid(struct cfs *fs, const uint8_t *uuid, struct cfs_snapshot *out)
{
    (void)fs;
    EXPECT(g_locked);
    note(15, out != NULL);
    mix(uuid, 16);
    if (chance(15))
        return -EIO;
    for (unsigned i = 0; i < g_nsnaps; i++)
        if (memcmp(g_snaps[i].uuid, uuid, 16) == 0) {
            if (out != NULL)
                *out = g_snaps[i];
            return 0;
        }
    return -ENOENT;
}

int cfs_snapshot_fix_identity(struct cfs *fs, unsigned *fixed)
{
    (void)fs;
    EXPECT(g_locked);
    unsigned r = pick(5);
    note(16, r);
    *fixed = r;
    return r == 0 ? -ENOSPC : 0;
}

/* ---- the rollback record ---- */

static int g_rb_rc;
static struct cfs_rollback g_rb;

int cfs_rollback_read(struct cfs *fs, struct cfs_rollback *out)
{
    (void)fs;
    EXPECT(g_locked);
    note(17, (uint64_t)(int64_t)g_rb_rc);
    if (g_rb_rc)
        return g_rb_rc;
    *out = g_rb;
    return 0;
}

int cfs_rollback_drop(struct cfs *fs)
{
    EXPECT(g_locked);
    unsigned r = pick(6);
    note(18, r);
    if (r == 0)
        return -EIO;
    fs->sb.rollback_root = 0;
    return 0;
}

/* ---- the inodes, the directories and the orphan record ---- */

static struct cfs_inode *inode_slot(uint64_t root, uint64_t ino)
{
    uint64_t rl = real_lin(root);
    if (rl == CFS_DVA_NONE || hdr_of(rl)->kind != CFS_KIND_IMAP1)
        return NULL;
    uint64_t bi = ino / CFS_INODES_PER_BLOCK;
    if (bi / CFS_PTRS_PER_BLOCK >= CFS_PTRS_PER_BLOCK)
        return NULL;
    uint64_t l0 = real_lin(ptrs_of(rl)[bi / CFS_PTRS_PER_BLOCK]);
    if (l0 == CFS_DVA_NONE || hdr_of(l0)->kind != CFS_KIND_IMAP0)
        return NULL;
    uint64_t ib = real_lin(ptrs_of(l0)[bi % CFS_PTRS_PER_BLOCK]);
    if (ib == CFS_DVA_NONE || hdr_of(ib)->kind != CFS_KIND_INODES)
        return NULL;
    return (struct cfs_inode *)(g_blk[ib].data + CFS_MHDR_SIZE + (ino % CFS_INODES_PER_BLOCK) * CFS_INODE_SIZE);
}

/*
 * Reads that fail only after the walk: a raw read (the orphan repair's) of every fifth inode, and an ordinary read of
 * an inode past the reads its entries account for (the link-count repair's). The walk's own reads then succeed, and
 * its answer is sound enough for the repairs to run into them.
 */
static bool g_late;
static unsigned g_reads[128];
static unsigned g_names[128];

static int read_inode(struct cfs *fs, uint64_t ino, struct cfs_inode *out, bool raw)
{
    EXPECT(g_locked);
    note(20, ino);
    note(21, raw);
    int rc = draw_fail();
    if (g_late && raw && ino % 5 == 2)
        rc = -EIO;
    if (g_late && !raw && ino > 1 && ino < 128 && ++g_reads[ino] > g_names[ino])
        rc = -EIO;
    if (rc == 0) {
        const struct cfs_inode *slot = ino < fs->sb.next_ino ? inode_slot(fs->sb.imap_root, ino) : NULL;
        if (slot == NULL || slot->ino != ino || (!raw && slot->nlink == 0))
            rc = -ENOENT;
        else
            *out = *slot;
    }
    note(22, (uint64_t)(int64_t)rc);
    return rc;
}

int cfs_inode_read(struct cfs *fs, uint64_t ino, struct cfs_inode *out) { return read_inode(fs, ino, out, false); }

int cfs_inode_read_raw(struct cfs *fs, uint64_t ino, struct cfs_inode *out) { return read_inode(fs, ino, out, true); }

int cfs_inode_write(struct cfs *fs, uint64_t ino, const struct cfs_inode *in)
{
    EXPECT(g_locked);
    note(23, ino);
    note(24, in->ino);
    note(25, in->nlink);
    struct cfs_inode *slot = inode_slot(fs->sb.imap_root, ino);
    if (slot == NULL || pick(25) == 0)
        return -EIO;
    *slot = *in;
    return 0;
}

int cfs_truncate_blocks(struct cfs *fs, struct cfs_inode *in, uint64_t keep_blocks)
{
    (void)fs;
    EXPECT(g_locked);
    note(26, in->ino);
    note(27, keep_blocks);
    if (pick(20) == 0)
        return -ENOSPC;
    memset(in->direct, 0, sizeof(in->direct));
    in->indirect = 0;
    in->size = 0;
    return 0;
}

void cfs_fail(struct cfs *fs, int rc)
{
    note(28, (uint64_t)(int64_t)rc);
    fs->failed = rc;
}

int cfs_dir_read_block_at(struct cfs *fs, const struct cfs_inode *dir, uint64_t lblk, uint8_t *buf)
{
    (void)fs;
    EXPECT(g_locked);
    note(29, dir->ino);
    note(30, lblk);
    int rc = draw_fail();
    if (rc == 0) {
        rc = -EIO;   /* a block the directory does not map */
        for (unsigned i = 0; i < CFS_DIRECT; i++) {
            const struct cfs_extent *e = &dir->direct[i];
            if (cfs_ext_count(e) == 0 || lblk < e->lblk || lblk >= (uint64_t)e->lblk + cfs_ext_count(e))
                continue;
            uint64_t lin = real_lin(e->start + (lblk - e->lblk));
            if (lin != CFS_DVA_NONE) {
                memcpy(buf, g_blk[lin].data, CFS_BLOCK);
                rc = 0;
            }
            break;
        }
    }
    note(31, (uint64_t)(int64_t)rc);
    return rc;
}

static unsigned g_orphan_mod;   /* the orphan record names every inode this divides, or none */

bool cfs_orphan_named(const struct cfs *fs, uint64_t ino)
{
    (void)fs;
    note(32, ino);
    return g_orphan_mod != 0 && ino % g_orphan_mod == 0;
}

/* ---- the lock and the clock ---- */

static uint64_t g_now = 1000000000ull;

uint64_t clock_now_ns(void)
{
    g_now += 1000000ull;
    return g_now;
}

uint64_t clock_since_ns(uint64_t stamp)
{
    g_now += 777;
    return g_now - stamp;
}

void mutex_lock(struct mutex *m)
{
    (void)m;
    EXPECT(!g_locked);
    g_locked = true;
    note(33, 0);
}

void mutex_unlock(struct mutex *m)
{
    (void)m;
    EXPECT(g_locked);
    EXPECT(g_bufs == 0);
    g_locked = false;
    note(34, 0);
}

/* ---- the unmounted device: its superblock slots, its mount and its pool ---- */

static struct fs_type g_fstype;
static struct mount g_mnt;
static struct blkdev g_dev[4];
static struct spool g_pool;
static struct spool_member g_pmem[NMEM];
static struct cfs_super g_slot[2];
static int g_slot_rc[2], g_slots_rc, g_mount_rc;
static unsigned g_busy, g_release_fail;

int cfs_super_slots(struct blkdev *bd, struct cfs_super *a, int *ra, struct cfs_super *b, int *rb)
{
    note(40, (uint64_t)(bd - g_dev));
    EXPECT(*ra == -EIO && *rb == -EIO);
    if (g_slots_rc)
        return g_slots_rc;
    *a = g_slot[0];
    *b = g_slot[1];
    *ra = g_slot_rc[0];
    *rb = g_slot_rc[1];
    return 0;
}

struct fs_type *vfs_find_fs(const char *name)
{
    EXPECT(strcmp(name, "cosmofs") == 0);
    note(41, 0);
    return &g_fstype;
}

int vfs_mount_detached(struct fs_type *fs, struct blkdev *bdev, unsigned flags, struct mount **out)
{
    EXPECT(fs == &g_fstype);
    note(42, (uint64_t)(bdev - g_dev));
    note(43, flags);
    if (g_mount_rc)
        return g_mount_rc;
    *out = &g_mnt;
    return 0;
}

bool cfs_mount_failure(const struct blkdev *bd, unsigned *stage, int *rc)
{
    note(44, (uint64_t)(bd - g_dev));
    if (pick(3) == 0)
        return false;
    *stage = 1 + pick(6);
    *rc = -EIO;
    return true;
}

bool vfs_bdev_mounted_except(const struct blkdev *bd, const struct mount *self)
{
    EXPECT(self == &g_mnt);
    note(45, (uint64_t)(bd - g_dev));
    return g_busy != 0 && pick(g_busy) == 0;
}

int vfs_mount_detached_release(struct mount *mnt, bool commit)
{
    EXPECT(mnt == &g_mnt);
    EXPECT(!g_locked);
    note(46, commit);
    return g_release_fail != 0 && pick(g_release_fail) == 0 ? -EIO : 0;
}

/* ---- building a filesystem ---- */

static unsigned g_version;
static unsigned g_nused;

static void use(uint64_t lin)
{
    struct block *k = &g_blk[lin];
    memset(k->data, 0, CFS_BLOCK);
    k->used = true;
    g_nused++;
}

/* A fresh metadata block of `kind` on member `v` (0 if none was found); now and then not the kind its parent names. */
static uint64_t alloc_block(unsigned v, uint32_t kind)
{
    for (unsigned tries = 0; tries < 64; tries++) {
        unsigned b = 2 + pick(NB - 2);
        uint64_t dva = CFS_DVA(v, b);
        uint64_t lin = real_lin(dva);
        if (lin == CFS_DVA_NONE || g_blk[lin].used)
            continue;
        use(lin);
        struct cfs_mhdr *h = hdr_of(lin);
        h->kind = chance(60) ? kind + 1 : kind;
        h->blkno = dva;
        return dva;
    }
    return 0;
}

/* `n` fresh data blocks in a row on member 0 (0 if no such run was found). */
static uint64_t alloc_run(unsigned n)
{
    for (unsigned tries = 0; tries < 64; tries++) {
        unsigned b = 2 + pick(NB - 2 - n);
        bool ok = true;
        for (unsigned k = 0; k < n && ok; k++) {
            uint64_t lin = real_lin(CFS_DVA(0, b + k));
            ok = lin != CFS_DVA_NONE && !g_blk[lin].used;
        }
        if (!ok)
            continue;
        for (unsigned k = 0; k < n; k++)
            use(real_lin(CFS_DVA(0, b + k)));
        return CFS_DVA(0, b);
    }
    return 0;
}

/* A pointer somewhere it should not be, now and then: outside the pool, past the maps, or a block already used. */
static uint64_t stray(void)
{
    unsigned r = pick(3);
    return r == 0 ? CFS_DVA(2, pick(NB)) : r == 1 ? CFS_DVA(0, NB + pick(NB)) : CFS_DVA(0, 2 + pick(NB - 2));
}

/* A chain of `n` blocks of `kind` linked through their first word; `loop` closes it. */
static uint64_t chain(unsigned n, uint32_t kind, bool loop)
{
    uint64_t head = 0, prev = 0;
    for (unsigned i = 0; i < n; i++) {
        uint64_t b = alloc_block(0, kind);
        if (b == 0)
            break;
        if (prev == 0)
            head = b;
        else
            *ptrs_of(real_lin(prev)) = b;
        prev = b;
    }
    if (loop && prev != 0)
        *ptrs_of(real_lin(prev)) = head;
    return head;
}

static uint64_t csum_tree(bool top)
{
    unsigned t = pick(3);
    bool two = top && t == 0 && (g_version >= 14 || g_level >= 2);   /* below version 14, a shape the check refuses */
    uint32_t kind = two ? CFS_KIND_CSUMIDX2 : CFS_KIND_CSUMIDX;
    if (top && chance(10))
        kind = CFS_KIND_CSUM;   /* a root that is neither shape */
    uint64_t root = alloc_block(0, kind);
    if (root == 0)
        return 0;
    unsigned n = 1 + pick(3);
    for (unsigned i = 0; i < n; i++) {
        uint64_t *slot = &ptrs_of(real_lin(root))[pick(CFS_PTRS_PER_BLOCK)];
        if (*slot == 0)
            *slot = two ? csum_tree(false) : chance(20) ? stray() : alloc_block(0, CFS_KIND_CSUM);
    }
    return root;
}

static uint64_t g_last_run;

/* One run: in order after `prev` (or not), plain or compressed, in fresh blocks (or not). */
static void make_run(struct cfs_extent *e, const struct cfs_extent *prev)
{
    uint32_t count = 1 + pick(3);
    bool comp = pick(5) == 0;
    uint32_t psize = comp ? 1 + pick(count) : count;
    uint32_t lblk = prev == NULL ? pick(3) : prev->lblk + cfs_ext_count(prev) + pick(3);
    if (prev != NULL && (chance(12) || fixable(15) || solo(2, 3)))
        lblk = prev->lblk - (prev->lblk > 0 ? pick(2) : 0);   /* not after the run before it */
    else if (prev != NULL && cfs_ext_count(prev) > 1 && (chance(12) || fixable(40)))
        lblk = prev->lblk + 1;                                /* inside the run before it */
    if (chance(40)) {
        /* Near the 2^32 bound, where the end of a run only fits in 64 bits. */
        lblk = 0xFFFFFF00u + (prev != NULL ? 0x80u : 0u);
        count = 0x200;
        comp = true;
        psize = 1;
    }
    uint64_t start = chance(25) ? stray() : g_last_run != 0 && fixable(30) ? g_last_run : alloc_run(psize);
    if (start != 0)
        g_last_run = start;   /* the next file's run may share it: a block the live tree claims twice */
    if (start == 0) {
        memset(e, 0, sizeof(*e));   /* no room for it: an unused slot */
        return;
    }
    e->start = start;
    e->lblk = lblk;
    e->count = comp ? CFS_EXT_COMPRESSED | ((psize - 1) << CFS_EXT_PSIZE_SHIFT) | count : count;
}

static void file_blocks(struct cfs_inode *in)
{
    unsigned n = pick(5);
    const struct cfs_extent *prev = NULL;
    for (unsigned i = 0; i < n && i < CFS_DIRECT; i++) {
        if (pick(8) == 0)
            continue;   /* an unused slot between runs */
        make_run(&in->direct[i], prev);
        prev = &in->direct[i];
    }
    if (pick(5) == 0) {
        unsigned blocks = 1 + pick(2);
        bool loop = chance(6);
        in->indirect = chain(blocks, CFS_KIND_EXTENTS, loop);
        for (uint64_t b = in->indirect; b != 0;) {
            uint64_t lin = real_lin(b);
            struct cfs_extent_block *eb = (struct cfs_extent_block *)(g_blk[lin].data + CFS_MHDR_SIZE);
            unsigned m = 1 + pick(3);
            for (unsigned i = 0; i < m; i++) {
                make_run(&eb->ext[1 + 2 * i], prev);
                prev = &eb->ext[1 + 2 * i];
            }
            uint64_t nb = eb->next;
            b = nb == in->indirect ? 0 : nb;
        }
    }
    if (pick(3) == 0)
        in->csum_root = csum_tree(true);
}

/* The slot for `ino` in the tree at `root`, making the map's blocks it needs. */
static struct cfs_inode *ensure_slot(uint64_t root, uint64_t ino)
{
    uint64_t rl = real_lin(root);
    uint64_t bi = ino / CFS_INODES_PER_BLOCK;
    uint64_t *p0 = &ptrs_of(rl)[bi / CFS_PTRS_PER_BLOCK];
    if (*p0 == 0)
        *p0 = alloc_block(0, CFS_KIND_IMAP0);
    if (*p0 == 0 || real_lin(*p0) == CFS_DVA_NONE)
        return NULL;
    uint64_t *pi = &ptrs_of(real_lin(*p0))[bi % CFS_PTRS_PER_BLOCK];
    if (*pi == 0)
        *pi = alloc_block(0, CFS_KIND_INODES);
    if (*pi == 0)
        return NULL;
    return (struct cfs_inode *)(g_blk[real_lin(*pi)].data + CFS_MHDR_SIZE + (ino % CFS_INODES_PER_BLOCK) * CFS_INODE_SIZE);
}

/* ---- the live tree: inodes 1..g_k, directories with entries, and what the entries say ---- */

#define MAXINO 100u
#define DIRBLKS 6u

struct mino {
    bool made;
    unsigned type;                 /* CFS_TYPE_* */
    uint64_t blk[DIRBLKS];         /* a directory's blocks */
    unsigned nblk;
    uint32_t links;                /* what the entries this builder wrote add up to */
};

static struct mino g_mi[MAXINO];
static unsigned g_k;
static bool g_deep, g_wide;   /* a chain of directories too deep to walk; inodes past the map's first index block */
static unsigned g_live_inodes;
static char g_collide[2][16];      /* two names whose hashes collide */

static bool add_entry(unsigned dir, uint64_t ino, unsigned type, const char *name, unsigned len)
{
    struct mino *d = &g_mi[dir];
    if (d->nblk == 0)
        return false;
    unsigned first = pick(d->nblk);
    for (unsigned i = 0; i < d->nblk; i++) {
        uint64_t lin = real_lin(d->blk[(first + i) % d->nblk]);
        if (lin == CFS_DVA_NONE)
            continue;
        struct cfs_dirent *blk = (struct cfs_dirent *)g_blk[lin].data;
        int s = cfs_dirent_free_run(blk, 1 + cfs_dirent_extra(len));
        if (s < 0)
            continue;
        cfs_dirent_put(blk, (unsigned)s, ino, type, name, len);
        if (ino < 128)
            g_names[ino]++;
        return true;
    }
    return false;
}

static unsigned make_name(char *out, uint64_t ino)
{
    unsigned long_name = pick(6);
    if (long_name == 0 && (g_version >= 15 || g_level >= 2)) {
        /* A long name: valid from version 15, too long below it. */
        unsigned len = 50 + pick(150);
        for (unsigned i = 0; i < len; i++)
            out[i] = (char)('a' + (ino + i) % 26);
        out[0] = (char)('A' + ino % 26);
        out[1] = (char)('A' + ino / 26 % 26);
        return len;
    }
    return (unsigned)snprintf(out, 32, "n%llu", (unsigned long long)ino);
}

/* Name `ino` in `dir`; a directory named this way counts its "." and its parent's "..". */
static void link_into(unsigned dir, unsigned ino)
{
    char name[256];
    unsigned len = make_name(name, ino);
    unsigned type = g_mi[ino].type;
    if (!add_entry(dir, ino, type, name, len))
        return;
    g_mi[ino].links++;
    if (type == CFS_TYPE_DIR) {
        g_mi[ino].links++;
        g_mi[dir].links++;
    }
}

static void make_dir_blocks(struct mino *m, unsigned n)
{
    m->nblk = 0;
    for (unsigned i = 0; i < n && i < DIRBLKS; i++) {
        uint64_t b = alloc_run(1);
        if (b != 0)
            m->blk[m->nblk++] = b;
    }
}

/* What the entries and the faults leave a directory's slots holding. */
static void dir_faults(unsigned dir)
{
    struct mino *d = &g_mi[dir];
    if (chance(8) || fixable(40)) {
        /* A repeat of a name already there, naming some inode. */
        char name[256];
        unsigned ino = 1 + pick(g_k);
        unsigned len = make_name(name, ino);
        unsigned other = 1 + pick(g_k);
        if (g_level >= 2 || (g_mi[ino].type != CFS_TYPE_DIR && g_mi[other].type != CFS_TYPE_DIR)) {
            add_entry(dir, ino, g_mi[ino].type, name, len);   /* a directory named twice has two parents */
            add_entry(dir, other, g_mi[other].type, name, len);
        }
    }
    if (chance(8)) {
        /* Two different names that hash alike: a re-scan, never a finding. */
        unsigned a = 1 + pick(g_k), b = 1 + pick(g_k);
        add_entry(dir, a, g_mi[a].type, g_collide[0], (unsigned)strlen(g_collide[0]));
        add_entry(dir, b, g_mi[b].type, g_collide[1], (unsigned)strlen(g_collide[1]));
    }
    if (chance(10) || fixable(15) || solo(1, 3))
        add_entry(dir, g_k + 3 + pick(4), CFS_TYPE_REG, "gone", 4);   /* an entry naming nothing */
    if (chance(10)) {
        unsigned ino = 1 + pick(g_k);
        add_entry(dir, ino, g_mi[ino].type == CFS_TYPE_DIR ? CFS_TYPE_REG : CFS_TYPE_DIR, "wrong", 5);
    }
    if (chance(10) && d->nblk > 0) {
        /* A malformed slot: an empty name, or a continuation with no head before it. */
        uint64_t lin = real_lin(d->blk[pick(d->nblk)]);
        struct cfs_dirent *blk = (struct cfs_dirent *)g_blk[lin].data;
        int s = cfs_dirent_free_run(blk, 1);
        if (s >= 0) {
            unsigned how = pick(3);
            if (how == 0) {
                blk[s].ino = 1 + pick(g_k);
                blk[s].namelen = 0;
            } else if (how == 1) {
                blk[s].ino = CFS_DIRENT_CONT;
            } else {
                blk[s].ino = 1 + pick(g_k);
                blk[s].namelen = 60;
                blk[s].extra = 1;   /* a continuation that is not marked */
                memcpy(blk[s].name, "x", 1);
            }
        }
    }
}

/* A directory whose entries are mostly one name repeated: more confirmations than the pass will make. */
static void repeated_dir(unsigned dir)
{
    struct mino *d = &g_mi[dir];
    make_dir_blocks(d, DIRBLKS);
    for (unsigned i = 0; i < 300; i++)
        add_entry(dir, 1 + pick(g_k), CFS_TYPE_REG, "same", 4);
}

static void put_inode(uint64_t root, uint64_t ino, const struct cfs_inode *in)
{
    struct cfs_inode *slot = ensure_slot(root, ino);
    if (slot != NULL)
        *slot = *in;
}

static void make_live(struct cfs *fs)
{
    memset(g_mi, 0, sizeof(g_mi));
    memset(g_names, 0, sizeof(g_names));
    fs->sb.imap_root = alloc_block(0, CFS_KIND_IMAP1);
    bool deep = g_deep;
    g_k = deep ? 72 : 2 + pick(30);
    fs->sb.next_ino = g_k + 1;
    if (fs->sb.imap_root == 0 || real_lin(fs->sb.imap_root) == CFS_DVA_NONE ||
        hdr_of(real_lin(fs->sb.imap_root))->kind != CFS_KIND_IMAP1) {
        g_live_inodes = 0;
        return;
    }
    /* The types and the directories' blocks. */
    for (unsigned i = 1; i <= g_k; i++) {
        struct mino *m = &g_mi[i];
        m->made = true;
        m->type = deep || i == 1 || pick(3) == 0 ? CFS_TYPE_DIR : pick(4) == 0 ? CFS_TYPE_LNK : CFS_TYPE_REG;
        if (i == 1 && chance(25))
            m->type = CFS_TYPE_REG;   /* a root that is not a directory */
        if (m->type == CFS_TYPE_DIR)
            make_dir_blocks(m, 1 + pick(2));
    }
    if (!deep && chance(6))
        repeated_dir(1 + pick(g_k));
    /* The names: each inode in a directory before it, or a deep chain. */
    for (unsigned i = 2; i <= g_k; i++) {
        if (deep) {
            link_into(i - 1, i);
            continue;
        }
        unsigned parent = 1 + pick(i - 1);
        while (parent > 1 && g_mi[parent].type != CFS_TYPE_DIR)
            parent--;
        if (g_mi[parent].type != CFS_TYPE_DIR)
            continue;
        if (fixable(12))
            continue;   /* no name: an orphan */
        link_into(parent, i);
        if (fixable(12) && (g_mi[i].type != CFS_TYPE_DIR || g_level >= 2)) {
            /* A second name: a hard link, or a directory with two parents. */
            unsigned other = 1 + pick(g_k);
            if (g_mi[other].type == CFS_TYPE_DIR)
                link_into(other, i);
        }
    }
    if (chance(15) && g_mi[g_k].type == CFS_TYPE_DIR)
        link_into(g_k, 1 + pick(g_k));   /* a directory naming one above it */
    for (unsigned i = 1; i <= g_k; i++)
        if (g_mi[i].type == CFS_TYPE_DIR)
            dir_faults(i);
    /* The inodes themselves. */
    g_live_inodes = 0;
    for (unsigned i = 1; i <= g_k; i++) {
        struct mino *m = &g_mi[i];
        struct cfs_inode in;
        memset(&in, 0, sizeof(in));
        in.ino = i;
        in.mode = (m->type << 12) | 0644u;
        in.nlink = m->links + (i == 1 ? 2 : 0);
        in.generation = pick(1000);
        if (fixable(10))
            in.nlink += 1 + pick(2);   /* a link count the entries do not support */
        if (chance(30))
            in.nlink = 0;              /* a slot whose last link went */
        if (m->type == CFS_TYPE_DIR) {
            for (unsigned b = 0; b < m->nblk; b++) {
                in.direct[b].start = m->blk[b];
                in.direct[b].count = 1;
                in.direct[b].lblk = b;
            }
            in.size = (uint64_t)(m->nblk + (chance(10) ? 1 : 0)) * CFS_BLOCK;   /* one past what it maps */
            if (pick(4) == 0)
                in.csum_root = csum_tree(true);
        } else {
            in.size = (uint64_t)pick(8) * CFS_BLOCK;
            file_blocks(&in);
        }
        uint64_t at = i;
        if (chance(40))
            in.ino = i + 1;            /* a slot that does not name itself */
        put_inode(fs->sb.imap_root, at, &in);
        g_live_inodes++;
    }
    if (g_wide) {
        /* A wide map: inodes past the first index block of the second level, named by nothing. */
        fs->sb.next_ino = (uint64_t)CFS_PTRS_PER_BLOCK * CFS_INODES_PER_BLOCK + 20;
        for (unsigned k = 0; k < 3; k++) {
            uint64_t ino = (uint64_t)CFS_PTRS_PER_BLOCK * CFS_INODES_PER_BLOCK + 2 + k * 5;
            struct cfs_inode in;
            memset(&in, 0, sizeof(in));
            in.ino = ino;
            in.mode = CFS_TYPE_REG << 12;
            in.nlink = 1;
            file_blocks(&in);
            put_inode(fs->sb.imap_root, ino, &in);
            g_live_inodes++;
        }
    }
    if (chance(15)) {
        /* A slot past the high-water mark. */
        struct cfs_inode in;
        memset(&in, 0, sizeof(in));
        in.ino = fs->sb.next_ino + 1;
        in.nlink = 1;
        put_inode(fs->sb.imap_root, in.ino, &in);
    }
}

/* A snapshot's own tree: a few inodes, some sharing the live tree's blocks. */
static uint64_t make_snap_tree(uint64_t *next_ino)
{
    uint64_t root = alloc_block(0, CFS_KIND_IMAP1);
    unsigned n = 1 + pick(4);
    *next_ino = n + 1;
    if (root == 0 || hdr_of(real_lin(root))->kind != CFS_KIND_IMAP1)
        return root;
    for (unsigned i = 1; i <= n; i++) {
        struct cfs_inode in;
        memset(&in, 0, sizeof(in));
        in.ino = chance(15) ? i + 7 : i;
        in.nlink = 1;
        in.mode = CFS_TYPE_REG << 12;
        file_blocks(&in);
        put_inode(root, i, &in);
    }
    return root;
}

static void make_members(struct cfs *fs)
{
    for (unsigned v = 0; v < fs->nmembers; v++) {
        struct cfs_memstate *m = &fs->mem[v];
        memset(m, 0, sizeof(*m));
        m->first_usable = v == 0 ? 2 : 1;
        m->nblocks = NB;
        m->base = (uint64_t)v * NB;
        for (uint64_t b = 0; b < m->first_usable; b++)
            use(real_lin(CFS_DVA(v, b)));
        m->alloc_root = alloc_block(v, CFS_KIND_ALLOCIDX);
        if (m->alloc_root != 0) {
            unsigned n = pick(4);
            for (unsigned c = 0; c < n; c++) {
                uint64_t *slot = &ptrs_of(real_lin(m->alloc_root))[pick(CFS_PTRS_PER_BLOCK)];
                if (*slot == 0)
                    *slot = chance(15) ? stray() : alloc_block(v, CFS_KIND_BITMAP);
            }
        }
        if (g_version >= 13 && pick(3) == 0)
            m->state_root = chance(8) ? stray() : alloc_block(v, CFS_KIND_COPYSTATE);
    }
    fs->sb.members = g_version >= 4 && pick(4) != 0 ? alloc_block(0, CFS_KIND_MEMBERS) : 0;
}

/* A snapshot's allocation state: from version 4 a member table naming each member's index. */
static uint64_t make_snap_alloc(struct cfs *fs)
{
    if (pick(5) == 0)
        return 0;
    if (g_version < 4)
        return pick(2) == 0 ? fs->mem[0].alloc_root : alloc_block(0, CFS_KIND_ALLOCIDX);
    uint64_t t = alloc_block(0, CFS_KIND_MEMBERS);
    if (t == 0)
        return 0;
    struct cfs_member_block *mb = (struct cfs_member_block *)(g_blk[real_lin(t)].data + CFS_MHDR_SIZE);
    mb->count = chance(10) ? CFS_MEMBERS_PER_BLOCK + 3 : fs->nmembers;
    for (unsigned v = 0; v < fs->nmembers; v++) {
        mb->m[v].alloc_root = pick(2) == 0 ? fs->mem[v].alloc_root : alloc_block(v, CFS_KIND_ALLOCIDX);
        unsigned st = pick(3);
        if (st == 0 && (g_version >= 13 || g_level >= 2))
            mb->m[v].state_root = alloc_block(v, CFS_KIND_COPYSTATE);   /* below version 13, one nothing claims */
    }
    return t;
}

static uint64_t make_deadlist(void)
{
    unsigned n = pick(3);
    bool loop = chance(15);
    uint64_t head = chain(n, CFS_KIND_DEADLIST, loop);
    for (uint64_t b = head; b != 0;) {
        struct cfs_dead_block *d = (struct cfs_dead_block *)(g_blk[real_lin(b)].data + CFS_MHDR_SIZE);
        d->count = chance(20) ? CFS_DEAD_PER_BLOCK + 9 : pick(4);
        for (unsigned k = 0; k < d->count && k < 4; k++) {
            d->blk[k] = fixable(3) ? CFS_DVA(0, 2 + pick(NB - 2)) : alloc_run(1);
            if (d->blk[k] == 0)
                d->count = k;   /* no room for more */
        }
        b = d->next == head ? 0 : d->next;
    }
    return head;
}

static void make_uuid(uint8_t *uuid)
{
    uint64_t a = next(), b = next();
    memcpy(uuid, &a, 8);
    memcpy(uuid + 8, &b, 8);
}

static void make_snapshots(struct cfs *fs)
{
    g_nsnaps = 0;
    fs->sb.snap_root = 0;
    unsigned blocks = pick(7);
    if (chance(30)) {
        /* A list that never ends, of one block with nothing in it: the guard ends the walk. */
        uint64_t b = alloc_block(0, cfs_snaplist_kind(fs));
        if (b != 0) {
            fs->sb.snap_root = b;
            list_of(real_lin(b))->next = b;
        }
        return;
    }
    uint64_t prev = 0, id = 0;
    for (unsigned k = 0; k < blocks; k++) {
        uint64_t b = alloc_block(0, cfs_snaplist_kind(fs));
        if (b == 0)
            break;
        if (prev == 0)
            fs->sb.snap_root = b;
        else
            list_of(real_lin(prev))->next = b;
        prev = b;
        for (unsigned i = 0; i < SLOTS; i++) {
            if (pick(4) == 0)
                continue;
            struct cfs_snapshot snap, *e = &snap;
            memset(e, 0, sizeof(*e));
            id += 1 + pick(3);
            e->id = id;
            if (fixable(10) && id > 2)
                e->id = id - 1 - pick(2);   /* an id another has, maybe */
            make_uuid(e->uuid);
            if (fixable(12))
                memset(e->uuid, 0, 16);
            else if (fixable(12) && g_nsnaps > 0)
                memcpy(e->uuid, g_snaps[pick(g_nsnaps)].uuid, 16);
            snprintf(e->name, sizeof(e->name), "snap-%llu", (unsigned long long)e->id);
            if (chance(20))
                e->id = 0;            /* a named entry with no id */
            if (pick(3) == 0) {
                e->imap_root = fs->sb.imap_root;
                e->next_ino = fs->sb.next_ino;
            } else {
                e->imap_root = fixable(10) ? 0 : make_snap_tree(&e->next_ino);
            }
            e->alloc_root = make_snap_alloc(fs);
            e->deadlist = make_deadlist();
            put_entry(g_version, real_lin(b), i, e);
            if (g_nsnaps < MAXSNAP && e->id != 0)
                g_snaps[g_nsnaps++] = *e;
        }
    }
    if (fs->sb.snap_root != 0)
        list_of(real_lin(fs->sb.snap_root))->max_id = fixable(8) ? id / 2 : id;
}

static void make_rollback(struct cfs *fs)
{
    fs->sb.rollback_root = 0;
    g_rb_rc = 0;
    if (g_version < 12 || pick(2) != 0)
        return;
    if (g_nsnaps == 0 && g_level == 0)
        return;   /* a sound record names snapshots that are there */
    fs->sb.rollback_root = alloc_block(0, CFS_KIND_ROLLBACK);
    unsigned r = fixable(6) ? 1 + pick(2) : 0;
    g_rb_rc = r == 1 ? -EIO : r == 2 ? -ENOENT : 0;
    memset(&g_rb, 0, sizeof(g_rb));
    g_rb.state = pick(2) == 0 ? CFS_ROLLBACK_PENDING : CFS_ROLLBACK_HELD;
    if (g_nsnaps > 0 && !fixable(4))
        memcpy(g_rb.target_uuid, g_snaps[pick(g_nsnaps)].uuid, 16);
    else
        make_uuid(g_rb.target_uuid);
    if (g_nsnaps > 0 && !fixable(4))
        memcpy(g_rb.safety_uuid, g_snaps[pick(g_nsnaps)].uuid, 16);
    else
        make_uuid(g_rb.safety_uuid);
}

static void make_bitmap(struct cfs *fs)
{
    uint64_t free_count = 0;
    bool noisy = fixable(2);
    for (uint64_t lin = 0; lin < fs->nblocks; lin++) {
        struct block *k = &g_blk[lin];
        k->alloc = k->used;
        if (cfs_lin_dva(fs, lin) == CFS_DVA_NONE)
            continue;
        if (k->used && noisy && pick(80) == 0)
            k->alloc = false;   /* in use, and the bitmap calls it free */
        else if (!k->used && noisy && pick(90) == 0)
            k->alloc = true;    /* allocated, and nothing names it */
        if (!k->alloc)
            free_count++;
    }
    fs->free_blocks = free_count + (fixable(6) ? 1 + pick(3) : 0);
    fs->sb.free_blocks = fs->free_blocks;
    fs->sb.inode_count = g_live_inodes;
    if (fixable(4))
        fs->sb.inode_count = pick(2) == 0 ? 0 : g_live_inodes + 1;
}

/* The free or orphan record's chain: one block or two, in a loop now and then. */
static uint64_t record(uint32_t kind)
{
    unsigned n = 1 + pick(2);
    bool loop = chance(10);
    return chain(n, kind, loop);
}

static uint64_t g_pending[4], g_exempt[4];

static void build(struct cfs *fs)
{
    for (unsigned i = 0; i < NMEM * NB; i++)
        if (g_blk[i].used) {
            memset(g_blk[i].data, 0, CFS_BLOCK);
            g_blk[i].used = false;
        }
    g_nused = 0;
    unsigned f = pick(5);
    g_level = f == 0 ? 0 : f <= 2 ? 1 : 2;
    unsigned so = pick(8);
    g_solo = g_level == 1 && so < 2 ? 1 + so : 0;
    if (g_solo != 0)
        g_level = 0;
    unsigned sp = pick(3);
    g_sparse = sp == 0 ? 1 : sp == 1 ? 6 : 30;
    unsigned shape = pick(14);
    g_deep = g_level >= 2 && shape == 0;
    if (g_deep)
        g_level = 0;   /* a sound tree, too deep: nothing else in the way of the walk */
    g_wide = shape == 1;
    unsigned late = pick(3);
    g_late = g_level == 1 && late == 0;
    memset(g_reads, 0, sizeof(g_reads));
    static const unsigned versions[] = { 3, 8, 9, 10, 11, 12, 13, 14, 15, 15, 15 };
    g_version = versions[pick(sizeof(versions) / sizeof(versions[0]))];
    unsigned nm = pick(3);
    g_nmem = nm == 0 ? 2 : 1;
    fs->nmembers = g_nmem;
    fs->nblocks = (uint64_t)g_nmem * NB;
    fs->sb.version = g_version;
    fs->failed = 0;
    fs->rdonly = false;
    unsigned fp = pick(5);
    g_fail_pct = g_level < 2 ? 0 : fp == 0 ? 2 : fp == 1 ? 8 : fp == 2 ? 30 : 0;
    g_last_run = 0;
    unsigned om = pick(4);
    g_orphan_mod = om == 0 ? 3 : 0;
    make_members(fs);
    make_live(fs);
    make_snapshots(fs);
    fs->snap_count = chance(5) ? 0 : g_nsnaps;
    make_rollback(fs);
    fs->sb.free_root = g_version >= 9 && pick(3) == 0 ? record(CFS_KIND_FREELOG) : 0;
    fs->sb.orphan_root = g_version >= 10 && pick(3) == 0 ? record(CFS_KIND_ORPHAN) : 0;
    fs->sb.key_root = pick(5) == 0 ? alloc_block(0, CFS_KIND_KEYS) : 0;
    unsigned np = pick(4), ne = pick(3);
    for (unsigned i = 0; i < np; i++) {
        g_pending[i] = chance(4) ? stray() : fixable(3) ? CFS_DVA(0, 2 + pick(NB - 2)) : alloc_run(1);
        if (g_pending[i] == 0)
            np = i;
    }
    for (unsigned i = 0; i < ne; i++) {
        g_exempt[i] = alloc_run(1);
        if (g_exempt[i] == 0)
            ne = i;
    }
    fs->pending_free = g_pending;
    fs->nr_pending = np;
    fs->pending_exempt = g_exempt;
    fs->nr_exempt = ne;
    make_bitmap(fs);
    if (chance(40))
        fs->nblocks = 0;                     /* a pool of no blocks: the maps are one chunk all the same */
    else if (fs->nmembers == 2 && chance(40))
        fs->mem[1].base = 40000;             /* a member placed past every map the check sizes */
    note(50, g_nused);
    note(51, g_version);
}

/* ---- the unmounted device's setup ---- */

static void build_dev(struct cfs *fs)
{
    unsigned r = pick(20);
    g_slots_rc = r == 0 ? -EIO : 0;
    for (unsigned s = 0; s < 2; s++) {
        memset(&g_slot[s], 0, sizeof(g_slot[s]));
        memset(g_slot[s].uuid, 0x5A, sizeof(g_slot[s].uuid));
        g_slot[s].total_blocks = fs->nblocks;
        g_slot[s].block_size = CFS_BLOCK;
        memcpy(g_slot[s].label, "pool", 4);
        g_slot[s].generation = 100 + s + pick(10);
        unsigned q = pick(12);
        g_slot_rc[s] = q == 0 ? -EIO : q == 1 ? -ENOKEY : q == 2 ? -EINVAL : 0;
    }
    unsigned d = pick(10);
    if (d == 0)
        g_slot[1].uuid[3] ^= 1;
    else if (d == 1)
        g_slot[1].total_blocks++;
    else if (d == 2)
        g_slot[1].block_size = 512;
    else if (d == 3)
        g_slot[1].label[0] = 'q';
    unsigned m = pick(14);
    g_mount_rc = m == 0 ? -EBUSY : m == 1 ? -ENOMEM : m == 2 ? -EIO : m == 3 ? -EINVAL : 0;
    memset(g_pmem, 0, sizeof(g_pmem));
    g_pool.m = g_pmem;
    g_pool.nmembers = fs->nmembers;
    for (unsigned v = 0; v < fs->nmembers; v++) {
        g_pmem[v].ncopies = 1 + pick(3);
        for (unsigned c = 0; c < g_pmem[v].ncopies; c++) {
            unsigned w = pick(5);
            g_pmem[v].copy[c].dev = w == 0 ? NULL : &g_dev[(v * 2 + c) % 4];
        }
    }
    unsigned b = pick(4);
    g_busy = b == 0 ? 4 : 0;
    unsigned rf = pick(4);
    g_release_fail = rf == 0 ? 2 : 0;
    fs->pool = &g_pool;
    fs->replay_freed = pick(5);
    fs->replay_orphans = pick(3);
    fs->fell_back = pick(4) == 0;
    fs->rollback_switched = pick(4) == 0;
    fs->rollback_error = pick(3) == 0 ? -EIO : 0;
}

/* ---- the reports ---- */

static void mix_class(const struct cosmofs_check_class *c)
{
    note(80, c->count);
    note(81, c->repaired);
    note(82, c->named);
    for (unsigned i = 0; i < c->named && i < CFS_CHECK_NAMES; i++)
        note(83, c->name[i]);
}

static void mix_report(const struct cosmofs_check_report *r)
{
    const struct cosmofs_check_class *c[] = {
        &r->alloc_not_seen, &r->seen_not_alloc, &r->dup, &r->nlink_wrong, &r->orphan, &r->dangling_entry,
        &r->dir_bad, &r->extent_order, &r->extent_overlap, &r->dir_dup_name, &r->counter_wrong, &r->chain_cycle,
        &r->unreadable, &r->snap_identity, &r->rollback_record, &r->hold,
    };
    for (unsigned i = 0; i < sizeof(c) / sizeof(c[0]); i++)
        mix_class(c[i]);
    note(84, r->blocks_seen);
    note(85, r->inodes_seen);
    note(86, r->dirs_seen);
    note(87, r->snapshots_seen);
    note(88, r->csum_two_level);
    note(89, r->counted_free);
    note(90, r->counted_inodes);
    note(91, r->bytes_allocated);
    note(92, r->partial);
    note(93, r->clean);
    note(94, r->repair_refused);
    note(95, r->boot_refused);
    note(96, r->elapsed_ns);
}

static void mix_dev(const struct cosmofs_dev_report *d)
{
    note(100, d->flags);
    note(101, (uint64_t)(int64_t)d->mount_error);
    note(102, d->mount_stage);
    note(103, (uint64_t)(int64_t)d->rollback_error);
    note(104, d->format);
    note(105, d->replayed_orphans);
    note(106, d->generation);
    note(107, d->slot_generation[0]);
    note(108, d->slot_generation[1]);
    note(109, d->replayed_frees);
    mix(d->rollback_target, sizeof(d->rollback_target));
}

/* ---- the rounds ---- */

/* Pages taken from the arena so that only `left` remain for the check's maps. */
#define HOLD_MAX 1024u
static struct page *g_hold[HOLD_MAX];
static unsigned g_nhold;

static void starve(uint64_t left)
{
    while (host_arena_free_pages() > left && g_nhold < HOLD_MAX) {
        struct page *pg = pmm_alloc_page(0);
        EXPECT(pg != NULL);
        g_hold[g_nhold++] = pg;
    }
}

static void unstarve(void)
{
    while (g_nhold > 0)
        pmm_free_page(g_hold[--g_nhold]);
}

/* Two short names whose hashes collide, found with the check's own hash (CFS_CHECK_NAMEBITS buckets). */
#define NAMEBITS 32768u

static void find_collision(void)
{
    static unsigned seen[NAMEBITS];
    memset(seen, 0, sizeof(seen));
    for (unsigned i = 1; i < 200000; i++) {
        char name[16];
        unsigned len = (unsigned)snprintf(name, sizeof(name), "c%u", i);
        unsigned h = cosmofs_test_name_hash(name, len);
        EXPECT(h < NAMEBITS);
        if (seen[h] != 0) {
            snprintf(g_collide[0], sizeof(g_collide[0]), "c%u", seen[h]);
            memcpy(g_collide[1], name, len + 1);
            note(110, seen[h]);
            note(111, i);
            return;
        }
        seen[h] = i;
    }
    EXPECT(false);
}

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    harness_klog_hook = on_klog;
    harness_klog_min = KLOG_ERROR + 1;   /* the findings are expected, by the thousand */
    host_arena_init(4u << 20);
    uint64_t pages = host_arena_free_pages();
    find_collision();
    struct cfs *fs = calloc(1, sizeof(*fs));
    EXPECT(fs != NULL);
    fs->mem = calloc(NMEM, sizeof(*fs->mem));
    EXPECT(fs->mem != NULL);
    for (unsigned i = 0; i < 4; i++)
        snprintf(g_dev[i].name, sizeof(g_dev[i].name), "vd%c", 'a' + i);
    /* The bit maps on their own: no bits, a bit past the map, and each allocation failing. */
    static const uint64_t sizes[] = { 0, 1, 32768, 32769, 100000 };
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        for (int fail = -1; fail < 2; fail++)
            for (uint64_t left = 0; left < 5; left++) {
                struct cfs_bits m;
                g_fail_in = fail;
                if (fail < 0)
                    starve(left);
                int rc = cfs_bits_alloc(&m, sizes[i]);
                unstarve();
                g_fail_in = -1;
                note(70, (uint64_t)(int64_t)rc);
                if (rc != 0) {
                    EXPECT(m.chunk == NULL);
                    continue;
                }
                note(71, m.nchunks);
                static const uint64_t bits[] = { 0, 7, 32767, 32768, 99999, 100000, 1u << 20 };
                for (unsigned b = 0; b < sizeof(bits) / sizeof(bits[0]); b++) {
                    note(72, cfs_bits_test(&m, bits[b]));
                    note(73, cfs_bits_set(&m, bits[b]));
                    note(74, cfs_bits_set(&m, bits[b]));
                    note(75, cfs_bits_test(&m, bits[b]));
                }
                cfs_bits_free(&m);
                EXPECT(m.chunk == NULL && m.nchunks == 0);
            }
    EXPECT(g_live == 0 && host_arena_free_pages() == pages);
    struct mount mnt;
    memset(&mnt, 0, sizeof(mnt));
    for (unsigned round = 0; round < ROUNDS; round++) {
        build(fs);
        unsigned fl = pick(8);
        unsigned flags = fl < 2 ? 0 : fl < 4 ? COSMOFS_CHECK_REPAIR : fl < 6 ? COSMOFS_CHECK_REPAIR | COSMOFS_CHECK_BOOT
                                                                            : fl == 6 ? COSMOFS_CHECK_REPAIR | COSMOFS_CHECK_OFFLINE
                                                                                      : COSMOFS_CHECK_BOOT;
        unsigned fi = pick(24);
        int fail_in = fi < 13 ? (int)fi : fi < 16 ? (int)(13 + pick(6)) : -1;
        unsigned st = pick(16);
        bool starved = st == 0;
        uint64_t left = pick(14);
        unsigned dev = pick(4);
        int rc;
        if (dev == 0) {
            build_dev(fs);
            g_mnt.fs_priv = fs;
            struct cosmofs_check_report rep;
            struct cosmofs_dev_report dr;
            memset(&rep, 0xEE, sizeof(rep));
            memset(&dr, 0xEE, sizeof(dr));
            g_fail_in = fail_in;
            if (starved)
                starve(left);
            rc = cosmofs_check_dev(&g_dev[pick(4)], flags, &rep, &dr);
            unstarve();
            mix_report(&rep);
            mix_dev(&dr);
        } else {
            unsigned nm = pick(60);
            mnt.fs_priv = nm == 0 ? NULL : fs;
            unsigned ro = pick(30);
            fs->rdonly = ro == 0;
            unsigned failed = pick(40);
            fs->failed = failed == 0 ? -EIO : 0;
            unsigned so = pick(8);
            struct cosmofs_check_report rep;
            memset(&rep, 0, sizeof(rep));   /* not a pattern: it holds bools */
            g_fail_in = fail_in;
            if (starved)
                starve(left);
            rc = cosmofs_check(&mnt, so == 0 ? NULL : &rep, flags);
            unstarve();
            if (so != 0)
                mix_report(&rep);   /* untouched when the check returned before writing it */
        }
        g_fail_in = -1;
        EXPECT(!g_locked);
        EXPECT(g_bufs == 0);
        EXPECT(g_live == 0);
        EXPECT(host_arena_free_pages() == pages);
        note(60, (uint64_t)(int64_t)rc);
        note(61, fs->free_blocks);
        note(62, fs->sb.free_blocks);
        note(63, fs->sb.inode_count);
        note(64, (uint64_t)(int64_t)fs->failed);
        note(65, fs->sb.rollback_root);
        for (unsigned i = 0; i < NMEM * NB; i++)
            note(66, g_blk[i].alloc);
        for (unsigned i = 1; i <= g_k; i++) {
            const struct cfs_inode *in = inode_slot(fs->sb.imap_root, i);
            if (in != NULL) {
                note(67, in->ino);
                note(68, in->nlink);
            }
        }
    }
    free(fs->mem);
    free(fs);
    EXPECT(g_live == 0);
    host_arena_destroy();
    harness_klog_hook = NULL;
    harness_klog_min = KLOG_WARN;
    printf("  check digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, ROUNDS);
    EXPECT(g_digest == CHECK_DIGEST);
}

int main(void)
{
    static const struct host_test tests[] = {
        { "check_digest", test_digest },
    };
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
