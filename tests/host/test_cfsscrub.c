/*
 * test_cfsscrub.c - cosmofs's scrub (kernel-services/filesystem/cosmofs/cosmofs_scrub.c) on the host: roadmap 2.0
 * SA7. A digest recorded from the C over seeded filesystems the test builds and owns: blocks with copies that do and do
 * not verify, superblock slots, allocation maps on one or two members, the live tree, snapshots and clones sharing its
 * blocks, deadlists, extent chains (one in a loop), checksum trees of one and two levels, data extents plain and
 * compressed, and blocks the open transaction holds. The scrub runs paced and unpaced, repairing and not, and while
 * its lock is let go a writer commits: snapshots deleted, inodes changed and removed, the transaction abandoned. Every
 * call the scrub makes, every argument, every return, every logged line and its statistics are in the digest; nothing
 * is left allocated and no buffer is left held.
 */

#include "harness.h"

#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/mutex.h>
#include <kernel/sched.h>
#include <kernel/storage.h>
#include <kernel/timer.h>
#include <kernel/vfs.h>
#include <kernel/wait.h>

#include "cosmofs_internal.h"   /* before the C library's headers, as test_cfsparse.c: its signal.h is the kernel's */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The values the CeeSafe port writes as its own constants. */
_Static_assert(EIO == 5 && EINVAL == 22 && ENOMEM == 12 && CFS_DVA_NONE == 0xFFFFFFFFFFFFFFFFull &&
                   CFS_BLOCK == 4096 && CFS_MHDR_SIZE == 32 && CFS_PTRS_PER_BLOCK == 508 &&
                   CFS_INODES_PER_BLOCK == 15 && CFS_INODE_SIZE == 256 && CFS_EXTENTS_PER_BLOCK == 253 &&
                   CFS_MAX_EXTENTS == 4096 && CFS_SNAP_MAX_CHAIN == 4096 && POOL_MAX_COPIES == 4 &&
                   COSMOFS_SCRUB_RATE_DEFAULT == 4096 && COSMOFS_SCRUB_REPAIR == 1,
               "cosmofs_scrub's errno.h, cosmofs_format.h and storage.h values");

/* The digest of the C (cosmofs_scrub.c, 2026-11-06), which the CeeSafe port gives. */
#define SCRUB_DIGEST 0x036f8de81cde37d2ull
#define ROUNDS 3000u
#define SEED 0x5C2B5C2B2026B017ull

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

/* ---- the allocator: malloc, counted; an allocation fails when g_fail_in counts down to it ---- */

static uint64_t g_now = 1000000000ull;   /* the clock: a millisecond a reading */
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

/* The scrub's set of blocks seen: one chunk, counted with the rest. */
int cfs_bits_alloc(struct cfs_bits *m, uint64_t bits)
{
    m->chunk = kmalloc(sizeof(uint8_t *), 0);
    if (m->chunk == NULL)
        return -ENOMEM;
    m->chunk[0] = kmalloc((size_t)(bits + 7) / 8, KMEM_ZERO);
    if (m->chunk[0] == NULL) {
        kfree(m->chunk);
        m->chunk = NULL;
        return -ENOMEM;
    }
    m->nchunks = 1;
    return 0;
}

void cfs_bits_free(struct cfs_bits *m)
{
    if (m->chunk != NULL) {
        kfree(m->chunk[0]);
        kfree(m->chunk);
    }
    m->chunk = NULL;
    m->nchunks = 0;
}

bool cfs_bits_set(struct cfs_bits *m, uint64_t bit)
{
    uint8_t mask = (uint8_t)(1u << (bit % 8));
    bool was = (m->chunk[0][bit / 8] & mask) != 0;
    m->chunk[0][bit / 8] |= mask;
    return was;
}

/* ---- the pool: two members of NB blocks, every 60th block a padding index with no address ---- */

#define NB 600u
#define NMEM 2u

struct block {
    _Alignas(8) uint8_t data[CFS_BLOCK];
    bool bad[POOL_MAX_COPIES];   /* this copy does not verify */
    bool dirty;                  /* the open transaction's, in memory only */
    bool used;
};

static struct block g_blk[NMEM * NB];
static unsigned g_copies;     /* copies of every block in this filesystem */
static unsigned g_fail_pct;   /* of reads that fail outright */
static unsigned g_bufs;       /* buffers held */
static bool g_locked;

static uint64_t lin_of(uint64_t dva)
{
    unsigned v = CFS_DVA_VDEV(dva);
    uint64_t b = CFS_DVA_BLK(dva);
    if (v >= NMEM || b >= NB || b % 60 == 59)
        return CFS_DVA_NONE;
    return v * NB + b;
}

uint64_t cfs_dva_lin(const struct cfs *fs, uint64_t dva)
{
    (void)fs;
    return lin_of(dva);
}

bool cfs_dva_valid(const struct cfs *fs, uint64_t dva)
{
    return CFS_DVA_VDEV(dva) < fs->nmembers && CFS_DVA_BLK(dva) < NB;
}

static struct cfs_mhdr *hdr_of(uint64_t lin) { return (struct cfs_mhdr *)g_blk[lin].data; }

static uint64_t *ptrs_of(uint64_t lin) { return (uint64_t *)(g_blk[lin].data + CFS_MHDR_SIZE); }

bool cfs_buf_is_dirty(struct cfs *fs, uint64_t blkno)
{
    (void)fs;
    uint64_t lin = lin_of(blkno);
    return lin != CFS_DVA_NONE && g_blk[lin].dirty;
}

bool cfs_mhdr_ok(const struct cfs_mac *mac, const void *block, uint64_t dva, uint32_t kind)
{
    const struct cfs_mhdr *h = block;
    note(10, kind);
    EXPECT(mac != NULL);
    return h->kind == kind && h->blkno == dva;
}

static int draw_fail(void)
{
    unsigned r = pick(100);
    return r < g_fail_pct ? -EIO : 0;
}

int cfs_verify_all(struct cfs *fs, uint64_t dva, void *buf, bool (*verify)(const void *blk, void *arg), void *arg,
                   bool repair, unsigned *bad_out, unsigned *fixed_out)
{
    (void)fs;
    EXPECT(g_locked);
    note(11, dva);
    note(12, repair);
    if (pick(400) == 0) {
        note(13, 0);
        return -ENOMEM;
    }
    uint64_t lin = lin_of(dva);
    unsigned good = 0, bad = 0;
    for (unsigned c = 0; c < g_copies; c++) {
        if (lin == CFS_DVA_NONE)
            memset(buf, 0, CFS_BLOCK);
        else
            memcpy(buf, g_blk[lin].data, CFS_BLOCK);
        if (lin != CFS_DVA_NONE && g_blk[lin].bad[c])
            ((struct cfs_mhdr *)buf)->kind ^= 0x80;
        bool ok = verify(buf, arg);
        note(14, ok);
        if (ok)
            good++;
        else
            bad++;
    }
    if (good == 0) {
        *bad_out = 0;
        *fixed_out = 0;
        return -EIO;
    }
    *bad_out = bad;
    *fixed_out = 0;
    if (repair && bad != 0 && lin != CFS_DVA_NONE) {
        for (unsigned c = 0; c < g_copies; c++)
            g_blk[lin].bad[c] = false;
        *fixed_out = bad;
    }
    return 0;
}

int cfs_buf_get(struct cfs *fs, uint64_t blkno, uint32_t kind, struct cfs_buf **out)
{
    (void)fs;
    EXPECT(g_locked);
    note(15, blkno);
    note(16, kind);
    uint64_t lin = lin_of(blkno);
    int rc = draw_fail();
    if (rc == 0 && (lin == CFS_DVA_NONE || (kind != 0 && hdr_of(lin)->kind != kind)))
        rc = -EIO;
    note(17, (uint64_t)(int64_t)rc);
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
    note(18, b->blkno);
    EXPECT(g_bufs > 0);
    g_bufs--;
    free(b);
}

struct cfs_mhdr *cfs_buf_hdr(struct cfs_buf *b) { return (struct cfs_mhdr *)b->data; }

int cfs_data_scrub_block(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, uint64_t dva, void *buf, bool repair,
                         unsigned *bad, unsigned *fixed)
{
    (void)fs;
    EXPECT(buf != NULL && g_locked);
    note(19, in->ino);
    note(20, in->size);
    note(21, lblk);
    note(22, dva);
    note(23, repair);
    unsigned r = pick(100);
    if (r == 0)
        return -ENOMEM;
    unsigned b = r < 10 ? 1 + pick(2) : 0;
    if (r >= 95)
        g_now += 300000000ull;   /* a slow read: the pacing window is spent before its share */
    *bad = b;
    *fixed = repair ? b : 0;
    return r < 14 ? -EIO : 0;
}

/* ---- the superblock slots ---- */

#define SUPER_MARK 0x5355504552u

struct super_copy {
    int state;   /* 0 good, 1 bad, 2 unreadable (not ONLINE), 3 a read that fails */
    uint64_t generation;
};

static struct super_copy g_super[2][POOL_MAX_COPIES];

static int super_slot(uint64_t dva)
{
    EXPECT(CFS_DVA_VDEV(dva) == 0 && CFS_DVA_BLK(dva) < 2);
    return (int)CFS_DVA_BLK(dva);
}

unsigned pool_copies(const struct spool *p, uint64_t dva)
{
    (void)p;
    note(24, dva);
    return g_copies;
}

bool pool_copy_readable(const struct spool *p, uint64_t dva, unsigned c)
{
    (void)p;
    note(25, c);
    return g_super[super_slot(dva)][c].state != 2;
}

int pool_read_copy(struct spool *p, uint64_t dva, unsigned copy, void *buf)
{
    (void)p;
    const struct super_copy *s = &g_super[super_slot(dva)][copy];
    note(26, copy);
    memset(buf, 0xA5, CFS_BLOCK);
    if (s->state == 3)
        return -EIO;
    uint64_t mark = s->state == 0 ? SUPER_MARK : 0;
    memcpy(buf, &mark, sizeof(mark));
    ((struct cfs_super *)buf)->generation = s->generation;
    return 0;
}

int pool_write_copy(struct spool *p, uint64_t dva, unsigned copy, const void *buf)
{
    (void)p;
    note(27, dva);
    note(28, copy);
    note(29, ((const struct cfs_super *)buf)->generation);
    if (pick(5) == 0)
        return -EIO;
    g_super[super_slot(dva)][copy].state = 0;
    return 0;
}

bool cfs_super_ok(const void *block)
{
    uint64_t mark;
    memcpy(&mark, block, sizeof(mark));
    return mark == SUPER_MARK;
}

/* ---- the snapshot list: SLOTS entries a block after the chain pointer ---- */

#define SLOTS 3u

uint32_t cfs_snaplist_kind(const struct cfs *fs) { return fs->sb.version >= 12 ? CFS_KIND_SNAPLIST2 : CFS_KIND_SNAPLIST; }

unsigned cfs_snaplist_slots(const struct cfs *fs)
{
    (void)fs;
    return SLOTS;
}

uint64_t cfs_snaplist_next(const struct cfs_buf *b) { return *(const uint64_t *)(b->data + CFS_MHDR_SIZE); }

static struct cfs_snapshot *entry_of(uint8_t *data, unsigned i)
{
    return (struct cfs_snapshot *)(data + CFS_MHDR_SIZE + 8 + (size_t)i * sizeof(struct cfs_snapshot));
}

bool cfs_snaplist_get(const struct cfs *fs, const struct cfs_buf *b, unsigned i, struct cfs_snapshot *out)
{
    (void)fs;
    EXPECT(i < SLOTS);
    const struct cfs_snapshot *e = entry_of(b->data, i);
    if (e->id % 23 == 22)
        return false;   /* an entry that does not parse */
    *out = *e;
    return true;
}

void cfs_tree_roots(struct cfs *fs, const struct cfs_snapshot *s, uint64_t *imap_root, uint64_t *next_ino)
{
    (void)fs;
    note(30, s->id);
    *imap_root = s->imap_root;
    *next_ino = s->next_ino;
}

/* ---- the inodes and their extents ---- */

static struct cfs_inode *inode_slot(uint64_t root, uint64_t ino)
{
    uint64_t rl = lin_of(root);
    if (rl == CFS_DVA_NONE || hdr_of(rl)->kind != CFS_KIND_IMAP1)
        return NULL;
    uint64_t bi = ino / CFS_INODES_PER_BLOCK;
    if (bi / CFS_PTRS_PER_BLOCK >= CFS_PTRS_PER_BLOCK)
        return NULL;
    uint64_t l0 = lin_of(ptrs_of(rl)[bi / CFS_PTRS_PER_BLOCK]);
    if (l0 == CFS_DVA_NONE || hdr_of(l0)->kind != CFS_KIND_IMAP0)
        return NULL;
    uint64_t ib = lin_of(ptrs_of(l0)[bi % CFS_PTRS_PER_BLOCK]);
    if (ib == CFS_DVA_NONE || hdr_of(ib)->kind != CFS_KIND_INODES)
        return NULL;
    return (struct cfs_inode *)(g_blk[ib].data + CFS_MHDR_SIZE + (ino % CFS_INODES_PER_BLOCK) * CFS_INODE_SIZE);
}

int cfs_inode_read_at(struct cfs *fs, uint64_t imap_root, uint64_t next_ino, uint64_t ino, struct cfs_inode *in)
{
    (void)fs;
    EXPECT(g_locked);
    note(31, imap_root);
    note(32, next_ino);
    note(33, ino);
    int rc = draw_fail();
    if (rc)
        return rc;
    struct cfs_inode *slot = ino < next_ino ? inode_slot(imap_root, ino) : NULL;
    if (slot == NULL || slot->nlink == 0)
        return -ENOENT;
    *in = *slot;
    return 0;
}

static uint64_t mixhash(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    return x;
}

int cfs_map_ext(struct cfs *fs, const struct cfs_inode *in, uint64_t lblk, struct cfs_extent *out)
{
    (void)fs;
    note(34, in->ino);
    note(35, lblk);
    uint64_t h = mixhash(in->ino * 7919 + lblk + in->generation);
    unsigned k = (unsigned)(h % 10);
    if (k == 0)
        return 0;   /* a hole */
    if (k == 1)
        return -EIO;
    memset(out, 0, sizeof(*out));
    out->start = CFS_DVA((h >> 20) % 7 == 0 ? 1 : 0, (h >> 8) % (NB + 4));
    if (k == 2) {
        uint32_t count = 1 + (uint32_t)((h >> 32) % 4);
        uint32_t psize = 1 + (uint32_t)((h >> 40) % count);
        out->count = CFS_EXT_COMPRESSED | ((psize - 1) << CFS_EXT_PSIZE_SHIFT) | count;
        out->lblk = (uint32_t)lblk;
    } else {
        uint32_t back = (uint32_t)((h >> 36) % 3);
        if (back > lblk)
            back = (uint32_t)lblk;
        out->lblk = (uint32_t)(lblk - back);
        out->count = back + 1 + (uint32_t)((h >> 44) % 3);
    }
    return 1;
}

/* ---- the lock, the clock and the writer that runs while the lock is let go ---- */

static unsigned g_writer_pct;   /* of the pauses in which a commit lands */
static struct cfs *g_fs;

uint64_t clock_now_ns(void)
{
    g_now += 1000000ull;
    return g_now;
}

uint64_t clock_since_ns(uint64_t stamp) { return g_now > stamp ? g_now - stamp : 0; }

void thread_sleep_ns(uint64_t ns)
{
    EXPECT(!g_locked);
    note(40, ns);
    g_now += ns;
}

void sched_yield(void)
{
    EXPECT(!g_locked);
    note(41, 0);
}

static void writer(struct cfs *fs);

void mutex_lock(struct mutex *m)
{
    (void)m;
    EXPECT(!g_locked);
    EXPECT(g_bufs == 0);
    g_locked = true;
    note(42, 0);
}

void mutex_unlock(struct mutex *m)
{
    (void)m;
    EXPECT(g_locked);
    EXPECT(g_bufs == 0);   /* the walk holds no buffer when it lets the lock go */
    g_locked = false;
    note(43, 0);
    if (g_fs != NULL)
        writer(g_fs);
}

/* ---- building a filesystem ---- */

static unsigned g_nused;

/* A fresh block of `kind` on member `v` (0 if none is left), or now and then one already in use. */
static uint64_t alloc_block(unsigned v, uint32_t kind)
{
    for (unsigned tries = 0; tries < 8; tries++) {
        unsigned b = pick(NB);
        uint64_t dva = CFS_DVA(v, b);
        uint64_t lin = lin_of(dva);
        if (lin == CFS_DVA_NONE || b < 2 || g_blk[lin].used)
            continue;
        struct block *k = &g_blk[lin];
        memset(k->data, 0, CFS_BLOCK);
        k->used = true;
        g_nused++;
        struct cfs_mhdr *h = hdr_of(lin);
        unsigned odd = pick(40);
        h->kind = odd == 0 ? kind + 1 : kind;   /* one in forty is not the kind its parent names */
        h->blkno = odd == 1 ? dva + 1 : dva;
        for (unsigned c = 0; c < POOL_MAX_COPIES; c++) {
            unsigned r = pick(12);
            k->bad[c] = r == 0;
        }
        unsigned d = pick(50);
        k->dirty = d == 0;
        return dva;
    }
    return 0;
}

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
            *ptrs_of(lin_of(prev)) = b;
        prev = b;
    }
    if (loop && prev != 0)
        *ptrs_of(lin_of(prev)) = head;
    return head;
}

static uint64_t csum_tree(unsigned depth)
{
    bool two = depth == 0 && pick(3) == 0;
    uint64_t root = alloc_block(0, two ? CFS_KIND_CSUMIDX2 : CFS_KIND_CSUMIDX);
    if (root == 0)
        return 0;
    unsigned n = 1 + pick(3);
    for (unsigned i = 0; i < n; i++) {
        unsigned at = pick(CFS_PTRS_PER_BLOCK);
        uint64_t child = two ? csum_tree(1) : alloc_block(0, CFS_KIND_CSUM);
        ptrs_of(lin_of(root))[at] = child;
    }
    return root;
}

static void make_inode(struct cfs_inode *in, uint64_t ino)
{
    memset(in, 0, sizeof(*in));
    unsigned live = pick(4);
    in->nlink = live == 0 ? 0 : 1;
    in->ino = pick(30) == 0 ? 0 : ino;
    in->size = (uint64_t)pick(12) * CFS_BLOCK + pick(CFS_BLOCK);
    in->generation = pick(1000);
    unsigned ext = pick(4);
    if (ext != 0) {
        unsigned loop = pick(15);
        in->indirect = chain(ext, CFS_KIND_EXTENTS, loop == 0);
    }
    unsigned cs = pick(3);
    if (cs != 0)
        in->csum_root = csum_tree(0);
}

/* A tree of `next_ino` inodes; the first of its inode-map blocks left out, now and then. */
static uint64_t make_tree(uint64_t *next_ino)
{
    uint64_t root = alloc_block(0, CFS_KIND_IMAP1);
    if (root == 0)
        return 0;
    unsigned wide = pick(12);
    unsigned l0s = wide == 0 ? 2 : 1;
    unsigned nib = 1 + pick(4);
    *next_ino = wide == 0 ? CFS_PTRS_PER_BLOCK * CFS_INODES_PER_BLOCK + nib * CFS_INODES_PER_BLOCK - pick(5)
                          : nib * CFS_INODES_PER_BLOCK - pick(5);
    for (unsigned l = 0; l < l0s; l++) {
        if (wide == 0 && l == 0)
            continue;   /* l0 0 missing: the walk skips to the next */
        uint64_t l0 = alloc_block(0, CFS_KIND_IMAP0);
        ptrs_of(lin_of(root))[l] = l0;
        if (l0 == 0)
            continue;
        for (unsigned i = 0; i < nib; i++) {
            unsigned gap = pick(8);
            if (gap == 0)
                continue;
            uint64_t ib = alloc_block(0, CFS_KIND_INODES);
            ptrs_of(lin_of(l0))[i] = ib;
            if (ib == 0)
                continue;
            for (unsigned j = 0; j < CFS_INODES_PER_BLOCK; j++) {
                struct cfs_inode in;
                make_inode(&in, ((uint64_t)l * CFS_PTRS_PER_BLOCK + i) * CFS_INODES_PER_BLOCK + j);
                memcpy(g_blk[lin_of(ib)].data + CFS_MHDR_SIZE + j * CFS_INODE_SIZE, &in, sizeof(in));
            }
        }
    }
    return root;
}

static uint64_t g_snap_ids[64];
static unsigned g_nsnap_ids;

static void make_snapshots(struct cfs *fs)
{
    unsigned blocks = pick(4);
    uint64_t prev = 0, id = 0;
    for (unsigned k = 0; k < blocks; k++) {
        uint64_t b = alloc_block(0, cfs_snaplist_kind(fs));
        if (b == 0)
            break;
        if (prev == 0)
            fs->sb.snap_root = b;
        else
            *ptrs_of(lin_of(prev)) = b;
        prev = b;
        for (unsigned i = 0; i < SLOTS; i++) {
            struct cfs_snapshot *e = entry_of(g_blk[lin_of(b)].data, i);
            unsigned empty = pick(5);
            if (empty == 0)
                continue;
            unsigned step = 1 + pick(5);
            id += step;
            /* Not in order: the scrub finds the lowest id above its cursor. */
            unsigned swap = pick(4);
            e->id = swap == 0 && id > 3 ? id - 3 : id;
            unsigned share = pick(3);
            if (share == 0) {
                e->imap_root = fs->sb.imap_root;
                e->next_ino = fs->sb.next_ino;
            } else {
                e->imap_root = make_tree(&e->next_ino);
            }
            unsigned dead = pick(4);
            unsigned loop = pick(10);
            e->deadlist = chain(dead, CFS_KIND_DEADLIST, loop == 0);
            if (g_nsnap_ids < 64)
                g_snap_ids[g_nsnap_ids++] = e->id;
        }
    }
    unsigned loop = pick(40);
    if (loop == 0 && prev != 0)
        *ptrs_of(lin_of(prev)) = fs->sb.snap_root;   /* a list that never ends: the guard ends the walk */
}

static void make_alloc(struct cfs *fs)
{
    for (unsigned v = 0; v < fs->nmembers; v++) {
        struct cfs_memstate *m = &fs->mem[v];
        m->nchunks = pick(5);
        m->alloc_root = alloc_block(v, CFS_KIND_ALLOCIDX);
        if (m->alloc_root != 0)
            for (unsigned c = 0; c < m->nchunks; c++) {
                unsigned hole = pick(12);
                ptrs_of(lin_of(m->alloc_root))[c] = hole == 0   ? 0
                                                    : hole == 1 ? CFS_DVA(3, c)   /* no member's */
                                                                : alloc_block(v, CFS_KIND_BITMAP);
            }
        unsigned st = pick(3);
        m->state_root = st == 0 ? 0 : alloc_block(v, CFS_KIND_COPYSTATE);
    }
}

static void build(struct cfs *fs)
{
    for (unsigned i = 0; i < NMEM * NB; i++)
        g_blk[i].used = false;
    g_nused = 0;
    g_nsnap_ids = 0;
    unsigned nm = pick(3);
    fs->nmembers = nm == 0 ? 2 : 1;
    fs->nblocks = NMEM * NB;
    fs->sb.version = 11 + pick(5);
    fs->sb_slot = pick(2);
    fs->failed = 0;
    fs->rdonly = pick(8) == 0;
    fs->commits = pick(100);
    fs->repairs = 0;
    fs->sb.snap_root = 0;
    unsigned cp = pick(3);
    g_copies = 1 + cp;
    unsigned f = pick(4);
    g_fail_pct = f == 0 ? 2 : 0;
    for (unsigned s = 0; s < 2; s++)
        for (unsigned c = 0; c < POOL_MAX_COPIES; c++) {
            unsigned st = pick(10);
            g_super[s][c].state = st < 6 ? 0 : st < 8 ? 1 : st == 8 ? 2 : 3;
            g_super[s][c].generation = pick(50);
        }
    make_alloc(fs);
    unsigned rb = pick(3);
    fs->sb.rollback_root = rb == 0 ? alloc_block(0, CFS_KIND_ROLLBACK) : 0;
    fs->sb.imap_root = make_tree(&fs->sb.next_ino);
    unsigned rootless = pick(30);
    if (rootless == 0)
        fs->sb.imap_root = 0;   /* inodes counted and no map: the walk stops */
    make_snapshots(fs);
    note(50, g_nused);
}

/* While the lock is let go: a commit, now and then, that changes what the walk copied. */
static void writer(struct cfs *fs)
{
    unsigned r = pick(100);
    if (r >= g_writer_pct)
        return;
    fs->commits++;
    unsigned what = pick(10);
    note(44, what);
    if (what == 0) {
        fs->failed = -EIO;   /* the transaction abandoned */
    } else if (what <= 3 && g_nsnap_ids > 0) {
        /* A snapshot deleted: its entry emptied. */
        uint64_t doomed = g_snap_ids[pick(g_nsnap_ids)];
        for (uint64_t b = fs->sb.snap_root; b != 0;) {
            uint64_t lin = lin_of(b);
            for (unsigned i = 0; i < SLOTS; i++)
                if (entry_of(g_blk[lin].data, i)->id == doomed)
                    entry_of(g_blk[lin].data, i)->id = 0;
            uint64_t nb = *ptrs_of(lin);
            b = nb == fs->sb.snap_root ? 0 : nb;
        }
    } else if (what <= 7) {
        /* An inode of the live tree changed or removed. */
        uint64_t ino = pick((unsigned)fs->sb.next_ino + 1);
        struct cfs_inode *in = inode_slot(fs->sb.imap_root, ino);
        if (in != NULL) {
            unsigned how = pick(3);
            if (how == 0)
                in->nlink = 0;
            else
                in->size = (uint64_t)pick(14) * CFS_BLOCK;
        }
    } else if (what == 8) {
        fs->sb.next_ino = fs->sb.next_ino / 2;   /* the tree shrank */
    }
}

/* ---- the rounds ---- */

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    harness_klog_hook = on_klog;
    harness_klog_min = KLOG_ERROR + 1;   /* the findings are expected, by the thousand */
    struct cfs *fs = calloc(1, sizeof(*fs));
    EXPECT(fs != NULL);
    fs->mem = calloc(NMEM, sizeof(*fs->mem));
    EXPECT(fs->mem != NULL);
    struct mount mnt;
    memset(&mnt, 0, sizeof(mnt));
    static const unsigned rates[] = { 0, 1, 5, 12, 40, 4096 };
    for (unsigned round = 0; round < ROUNDS; round++) {
        build(fs);
        unsigned rate = rates[pick(6)];
        g_cosmofs_scrub_rate = rate;
        unsigned w = pick(3);
        g_writer_pct = w == 0 ? 0 : w == 1 ? 10 : 50;
        unsigned fl = pick(3);
        unsigned flags = fl == 0 ? 0 : COSMOFS_SCRUB_REPAIR;
        unsigned failed = pick(40);
        fs->failed = failed == 0 ? -EIO : 0;
        unsigned fi = pick(20);
        g_fail_in = fi < 4 ? (int)fi : fi < 6 ? (int)(4 + pick(4)) : -1;
        unsigned nm = pick(60);
        mnt.fs_priv = nm == 0 ? NULL : fs;
        unsigned so = pick(6);
        struct cosmofs_scrub_stats st;
        memset(&st, 0xEE, sizeof(st));
        g_fs = fs;
        int rc = cosmofs_scrub(&mnt, so == 0 ? NULL : &st, flags);
        g_fs = NULL;
        g_fail_in = -1;
        EXPECT(!g_locked);
        EXPECT(g_bufs == 0);
        EXPECT(g_live == 0);
        note(60, (uint64_t)(int64_t)rc);
        mix(&st, sizeof(st));
        note(61, fs->repairs);
        for (unsigned s = 0; s < 2; s++)
            for (unsigned c = 0; c < POOL_MAX_COPIES; c++)
                note(62, (uint64_t)g_super[s][c].state);
        for (unsigned i = 0; i < NMEM * NB; i++)
            if (g_blk[i].used)
                mix(g_blk[i].bad, sizeof(g_blk[i].bad));
    }
    /* cfs_pace_init and cfs_pace on their own, at the edges of the window. */
    static const unsigned edge[] = { 0, 1, 3, 9, 10, 11, 99, 4096, 0xFFFFFFFFu };
    for (unsigned i = 0; i < sizeof(edge) / sizeof(edge[0]); i++) {
        struct cfs_pace p;
        cfs_pace_init(&p, edge[i]);
        note(70, p.quota);
        note(71, p.window_ns);
        note(72, p.used);
        g_locked = true;
        for (unsigned k = 0; k < 4; k++) {
            p.used += k;
            note(73, cfs_pace(fs, &p));
            note(74, p.used);
        }
        g_locked = false;
    }
    free(fs->mem);
    free(fs);
    EXPECT(g_live == 0);
    harness_klog_hook = NULL;
    harness_klog_min = KLOG_WARN;
    printf("  scrub digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, ROUNDS);
    EXPECT(g_digest == SCRUB_DIGEST);
}

int main(void)
{
    static const struct host_test tests[] = {
        { "scrub_digest", test_digest },
    };
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
