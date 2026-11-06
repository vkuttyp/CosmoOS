/*
 * test_cfsreach.c - cosmofs's tree walk (kernel-services/filesystem/cosmofs/cosmofs_reach.c, cfs_tree_walk) on the
 * host: roadmap 2.0 SA7. A digest recorded from the C over seeded rounds, each a tree the test builds in a block
 * store it owns: inode maps of both levels, inode slots in place and out of it and past the high-water mark, direct,
 * compressed and chained extents, chains that loop, checksum trees of one level and two (on version-14 roots and
 * older ones), blocks of the wrong kind, unreadable blocks, addresses outside the pool, and the walk's allocation
 * failing. Every block marked, every buffer taken and given back, every return and every logged line are in the
 * digest; every buffer taken is given back.
 */

#include "harness.h"

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>

#include "cosmofs_internal.h"   /* before the C library's headers, as test_cfsparse.c: its signal.h is the kernel's */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The values the CeeSafe port writes as its own constants. */
_Static_assert(EIO == 5 && CFS_BLOCK == 4096 && CFS_MHDR_SIZE == 32 && CFS_PTRS_PER_BLOCK == 508 &&
                   CFS_INODES_PER_BLOCK == 15 && CFS_INODE_SIZE == 256 && CFS_EXTENTS_PER_BLOCK == 253 &&
                   CFS_DIRECT == 10 && CFS_MAX_EXTENTS == 4096 && CFS_KIND_IMAP1 == 1 && CFS_KIND_IMAP0 == 2 &&
                   CFS_KIND_INODES == 3 && CFS_KIND_EXTENTS == 6 && CFS_KIND_CSUMIDX == 7 && CFS_KIND_CSUM == 8 &&
                   CFS_KIND_CSUMIDX2 == 18 && CFS_DVA_NONE == 0xFFFFFFFFFFFFFFFFull,
               "cosmofs_reach's errno.h and cosmofs_format.h values");
/* The layout cosmofs_reach.cee reads its blocks by. */
_Static_assert(__builtin_offsetof(struct cfs_mhdr, kind) == 4 && __builtin_offsetof(struct cfs_inode, ino) == 48 &&
                   __builtin_offsetof(struct cfs_inode, direct) == 56 &&
                   __builtin_offsetof(struct cfs_inode, indirect) == 216 &&
                   __builtin_offsetof(struct cfs_inode, csum_root) == 240 && sizeof(struct cfs_inode) == 256 &&
                   sizeof(struct cfs_extent) == 16 && __builtin_offsetof(struct cfs_extent, count) == 8 &&
                   __builtin_offsetof(struct cfs_extent_block, next) == 0 &&
                   __builtin_offsetof(struct cfs_extent_block, ext) == 8 && CFS_EXT_COMPRESSED == 0x80000000u &&
                   CFS_EXT_PSIZE_SHIFT == 16 && CFS_EXT_PSIZE_MASK == 0xFF && CFS_EXT_COUNT_MASK == 0xFFFF &&
                   CFS_MAX_EXTENTS / CFS_EXTENTS_PER_BLOCK + 2 == 18,
               "cosmofs_reach.cee's offsets into cosmofs_format.h's structures");

/* The digest of the C (cosmofs_reach.c, 2026-11-06), which the CeeSafe port gives. */
#define REACH_DIGEST 0x18b576f00e985902ull
#define ROUNDS 6000u
#define SEED 0xC05F5FA2026B0015ull

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

/* ---- the allocator: malloc, counted; kmalloc fails when g_fail_in counts down to it ---- */

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

/* ---- the block store: member 0 and member 1, a block each linear index ---- */

#define M0_BLOCKS 400u
#define M1_BLOCKS 100u
#define NBLK (M0_BLOCKS + M1_BLOCKS)

static uint8_t *g_blk[NBLK];
static bool g_used[NBLK], g_bad[NBLK];
static unsigned g_bad_pct;   /* of reads that fail besides the bad blocks */
static long g_held;

uint64_t cfs_dva_lin(const struct cfs *fs, uint64_t dva)
{
    (void)fs;
    unsigned v = CFS_DVA_VDEV(dva);
    uint64_t b = CFS_DVA_BLK(dva);
    if (v == 0 && b < M0_BLOCKS)
        return b;
    if (v == 1 && b < M1_BLOCKS)
        return M0_BLOCKS + b;
    return CFS_DVA_NONE;
}

static uint64_t lin_dva(uint64_t lin) { return lin < M0_BLOCKS ? CFS_DVA(0, lin) : CFS_DVA(1, lin - M0_BLOCKS); }

struct cfs_mhdr *cfs_buf_hdr(struct cfs_buf *b) { return (struct cfs_mhdr *)b->data; }

int cfs_buf_get(struct cfs *fs, uint64_t blkno, uint32_t kind, struct cfs_buf **out)
{
    uint64_t lin = cfs_dva_lin(fs, blkno);
    note(10, blkno);
    note(11, kind);
    if (lin == CFS_DVA_NONE)
        return -EIO;
    unsigned r = pick(100);
    if (g_bad[lin] || r < g_bad_pct)
        return -EIO;
    if (kind != 0 && ((struct cfs_mhdr *)g_blk[lin])->kind != kind)
        return -EIO;
    struct cfs_buf *b = calloc(1, sizeof(*b));
    EXPECT(b != NULL);
    b->blkno = blkno;
    b->data = g_blk[lin];
    b->refs = 1;
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

/* ---- the trees ---- */

/* A block of `kind`, zeroed, at a free linear index (or one in use, now and then: a tree that shares). */
static uint64_t new_block(uint32_t kind)
{
    unsigned lin = pick(NBLK);
    if (pick(10) != 0)
        for (unsigned k = 0; k < NBLK && g_used[lin]; k++)
            lin = (lin + 1) % NBLK;
    g_used[lin] = true;
    memset(g_blk[lin], 0, CFS_BLOCK);
    struct cfs_mhdr *h = (struct cfs_mhdr *)g_blk[lin];
    unsigned wrong = pick(40);
    h->kind = wrong == 0 ? CFS_KIND_BITMAP : kind;
    if (pick(60) == 0)
        g_bad[lin] = true;
    return lin_dva(lin);
}

static uint64_t *slots_of(uint64_t dva) { return (uint64_t *)(g_blk[cfs_dva_lin(NULL, dva)] + CFS_MHDR_SIZE); }

/* A pointer to a block of data: anywhere, sometimes outside the pool. */
static uint64_t data_dva(void)
{
    unsigned k = pick(30);
    if (k == 0)
        return CFS_DVA(2, pick(10));
    if (k == 1)
        return CFS_DVA(0, M0_BLOCKS + pick(5));
    unsigned lin = pick(NBLK);
    return lin_dva(lin);
}

static void fill_extent(struct cfs_extent *e)
{
    unsigned k = pick(6);
    e->start = data_dva();
    e->lblk = pick(1000);
    if (k == 0) {
        e->count = 0;
    } else if (k == 1) {
        unsigned psize = 1 + pick(3), count = psize + pick(8);
        e->count = cfs_ext_pack(1 + pick(2), psize, count);
    } else {
        e->count = 1 + pick(6);
    }
}

static uint64_t csum_tree(unsigned depth)
{
    unsigned two = depth == 0 && pick(3) == 0;
    uint64_t idx = new_block(two ? CFS_KIND_CSUMIDX2 : CFS_KIND_CSUMIDX);
    unsigned n = pick(5);
    for (unsigned s = 0; s < n; s++) {
        unsigned at = pick(4) == 0 ? CFS_PTRS_PER_BLOCK - 1 - pick(3) : pick(8);
        uint64_t leaf = two ? csum_tree(1) : new_block(CFS_KIND_CSUM);
        if (pick(30) == 0)
            leaf = data_dva();
        slots_of(idx)[at] = leaf;
    }
    return idx;
}

static void fill_inode(struct cfs_inode *in, uint64_t ino)
{
    in->ino = ino;
    unsigned nd = pick(CFS_DIRECT + 1);
    for (unsigned i = 0; i < nd; i++)
        fill_extent(&in->direct[i]);
    unsigned chain = pick(5);
    uint64_t first = 0, prev = 0;
    for (unsigned c = 0; c < chain; c++) {
        uint64_t b = new_block(CFS_KIND_EXTENTS);
        struct cfs_extent_block *eb = (struct cfs_extent_block *)slots_of(b);
        unsigned ne = pick(4) == 0 ? CFS_EXTENTS_PER_BLOCK - pick(3) : pick(6);
        for (unsigned i = 0; i < ne; i++)
            fill_extent(&eb->ext[i]);
        if (prev == 0)
            first = b;
        else
            ((struct cfs_extent_block *)slots_of(prev))->next = b;
        prev = b;
    }
    if (prev != 0 && pick(12) == 0)
        ((struct cfs_extent_block *)slots_of(prev))->next = first;   /* a loop: the guard ends it */
    in->indirect = first;
    unsigned k = pick(4);
    in->csum_root = k == 0 ? csum_tree(0) : k == 1 ? new_block(CFS_KIND_CSUMIDX) : 0;
}

static uint64_t build(uint64_t *next_ino)
{
    uint64_t root = new_block(CFS_KIND_IMAP1);
    uint64_t high = 0;
    unsigned n1 = 1 + pick(3);
    for (unsigned a = 0; a < n1; a++) {
        unsigned i = pick(4) == 0 ? pick(CFS_PTRS_PER_BLOCK) : pick(2);
        uint64_t l0 = new_block(CFS_KIND_IMAP0);
        slots_of(root)[i] = l0;
        unsigned n0 = 1 + pick(3);
        for (unsigned z = 0; z < n0; z++) {
            unsigned j = pick(4) == 0 ? pick(CFS_PTRS_PER_BLOCK) : pick(3);
            uint64_t ib = new_block(CFS_KIND_INODES);
            slots_of(l0)[j] = ib;
            unsigned nin = pick(CFS_INODES_PER_BLOCK + 1);
            for (unsigned q = 0; q < nin; q++) {
                unsigned k = pick(CFS_INODES_PER_BLOCK);
                struct cfs_inode *in = (struct cfs_inode *)((uint8_t *)slots_of(ib) + (size_t)k * CFS_INODE_SIZE);
                uint64_t expect = ((uint64_t)i * CFS_PTRS_PER_BLOCK + j) * CFS_INODES_PER_BLOCK + k;
                unsigned w = pick(25);
                fill_inode(in, w == 0 ? expect + 1 : expect);
                if (expect + 1 > high)
                    high = expect + 1;
            }
        }
    }
    unsigned k = pick(10);
    *next_ino = k == 0 ? high / 2 : high + pick(3);
    return root;
}

static void marked(void *arg, uint64_t lin)
{
    note(20, (uintptr_t)arg);
    note(21, lin);
}

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    harness_klog_hook = on_klog;
    harness_klog_min = KLOG_ERROR + 1;   /* the walk's warnings are expected, by the thousand */
    for (unsigned i = 0; i < NBLK; i++)
        g_blk[i] = aligned_alloc(4096, CFS_BLOCK);
    struct cfs *fs = calloc(1, sizeof(*fs));
    EXPECT(fs != NULL);
    for (unsigned round = 0; round < ROUNDS; round++) {
        memset(g_used, 0, sizeof(g_used));
        memset(g_bad, 0, sizeof(g_bad));
        unsigned v = pick(3);
        fs->sb.version = v == 0 ? 13 : 14 + pick(2);
        unsigned b = pick(10);
        g_bad_pct = b == 0 ? 3 : 0;
        uint64_t next_ino = 0;
        uint64_t root = pick(30) == 0 ? 0 : build(&next_ino);
        if (pick(20) == 0)
            g_fail_in = (int)pick(3);
        int rc = cfs_tree_walk(fs, root, next_ino, marked, (void *)(uintptr_t)(round % 7 + 1));
        g_fail_in = -1;
        note(30, (uint64_t)(int64_t)rc);
        EXPECT(g_held == 0);
    }
    free(fs);
    for (unsigned i = 0; i < NBLK; i++)
        free(g_blk[i]);
    EXPECT(g_live == 0);
    harness_klog_hook = NULL;
    harness_klog_min = KLOG_WARN;
    printf("  reach digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, ROUNDS);
    EXPECT(g_digest == REACH_DIGEST);
}

int main(void)
{
    static const struct host_test tests[] = {
        { "reach_digest", test_digest },
    };
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
