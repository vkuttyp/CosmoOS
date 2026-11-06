/*
 * test_cfsalloc.c - cosmofs's allocation and inode map (kernel-services/filesystem/cosmofs/cosmofs_alloc.c) on the
 * host: roadmap 2.0 SA7. A digest recorded from the C over seeded filesystems the test builds and owns, with the real
 * address and inode-slot checks (cfsparse): pools of one to three members of very different sizes, some spanning
 * several bitmap chunks, partly allocated, with a metadata reserve; runs asked for with and without a hint, metadata and
 * data, until the pool is full; blocks freed deferred and exempt, inside a clone and not, while the lists grow and
 * krealloc fails; orphans reserved, added, named and removed; an inode map of one or two levels with holes, read
 * through its live root and a snapshot's, written (tagged or not, inside a clone or not), prepared, allocated and
 * discarded, while reads, copies and new blocks fail. Every call the allocator makes, every argument, every return,
 * every logged line, the bitmap, the counters, the lists and every inode read back are in the digest; nothing is left
 * allocated and no buffer is left held.
 */

#include "harness.h"

#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>

#include "cosmofs_internal.h"   /* before the C library's headers, as test_cfsparse.c: its signal.h is the kernel's */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The digest of the C (cosmofs_alloc.c, 2026-11-07), which the CeeSafe port gives. */
#define ALLOC_DIGEST 0x583d1da8b67f7b98ull
#define ROUNDS 3000u
#define SEED 0xA110CA7E2026B0B0ull

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

/* ---- the allocator: malloc, counted; krealloc fails when g_fail_in counts down to it ---- */

static int g_fail_in = -1;
static long g_live;

void *krealloc(void *ptr, size_t new_size, unsigned flags)
{
    (void)flags;
    note(2, new_size);
    if (g_fail_in >= 0 && g_fail_in-- == 0)
        return NULL;
    void *p = realloc(ptr, new_size);
    EXPECT(p != NULL);
    if (ptr == NULL)
        g_live++;
    return p;
}

void kfree(void *p)
{
    if (p != NULL)
        g_live--;
    free(p);
}

/* ---- the metadata blocks of the inode map, and the buffer layer over them ---- */

#define NMETA 256u

struct block {
    _Alignas(8) uint8_t data[CFS_BLOCK];
    bool used;
};

static struct block g_meta[NMETA];
static unsigned g_bufs;
static unsigned g_get_fail, g_new_fail;

/* Metadata blocks live at member 0's blocks 1000.. (every pool here has a member 0 that large or the dva is not one of
 * the pool's: then cfs_dva_valid says so, before the buffer layer is asked). */
#define META_BASE 1000u

static uint64_t meta_dva(unsigned i) { return CFS_DVA(0, META_BASE + i); }

static int meta_index(uint64_t dva)
{
    if (CFS_DVA_VDEV(dva) != 0 || CFS_DVA_BLK(dva) < META_BASE || CFS_DVA_BLK(dva) >= META_BASE + NMETA)
        return -1;
    return (int)(CFS_DVA_BLK(dva) - META_BASE);
}

static unsigned new_meta(void)
{
    for (unsigned i = 0; i < NMETA; i++)
        if (!g_meta[i].used) {
            g_meta[i].used = true;
            memset(g_meta[i].data, 0, CFS_BLOCK);
            return i;
        }
    return NMETA;
}

static struct cfs_buf *buf_of(unsigned i)
{
    struct cfs_buf *b = calloc(1, sizeof(*b));
    EXPECT(b != NULL);
    b->blkno = meta_dva(i);
    b->data = g_meta[i].data;
    b->refs = 1;
    g_bufs++;
    return b;
}

int cfs_buf_get(struct cfs *fs, uint64_t blkno, uint32_t kind, struct cfs_buf **out)
{
    (void)fs;
    note(10, blkno);
    note(11, kind);
    int i = meta_index(blkno);
    if (g_get_fail != 0 && pick(g_get_fail) == 0)
        return -EIO;
    if (i < 0 || !g_meta[i].used || (kind != 0 && ((struct cfs_mhdr *)g_meta[i].data)->kind != kind))
        return -EIO;
    *out = buf_of((unsigned)i);
    return 0;
}

void cfs_buf_put(struct cfs *fs, struct cfs_buf *b)
{
    (void)fs;
    note(12, b->blkno);
    EXPECT(g_bufs > 0);
    g_bufs--;
    free(b);
}

static void stamp(struct cfs *fs, unsigned i, uint32_t kind)
{
    struct cfs_mhdr *h = (struct cfs_mhdr *)g_meta[i].data;
    h->kind = kind;
    h->blkno = meta_dva(i);
    h->generation = fs->gen;
}

int cfs_buf_new(struct cfs *fs, uint32_t kind, struct cfs_buf **out)
{
    note(13, kind);
    if (g_new_fail != 0 && pick(g_new_fail) == 0)
        return -ENOSPC;
    unsigned i = new_meta();
    if (i == NMETA)
        return -ENOSPC;
    stamp(fs, i, kind);
    *out = buf_of(i);
    return 0;
}

int cfs_buf_cow(struct cfs *fs, struct cfs_buf **bp, uint64_t *parent_slot)
{
    struct cfs_buf *b = *bp;
    note(14, b->blkno);
    struct cfs_mhdr *h = (struct cfs_mhdr *)b->data;
    if (h->generation == fs->gen)
        return 0;
    if (g_new_fail != 0 && pick(g_new_fail) == 0)
        return -ENOSPC;
    unsigned i = new_meta();
    if (i == NMETA)
        return -ENOSPC;
    memcpy(g_meta[i].data, b->data, CFS_BLOCK);
    stamp(fs, i, h->kind);
    cfs_buf_put(fs, b);
    *parent_slot = meta_dva(i);
    *bp = buf_of(i);
    return 0;
}

/* ---- the rest of the filesystem the allocator calls ---- */

void cfs_note_dirty(struct cfs *fs)
{
    note(20, fs->free_blocks);
}

void cfs_fail(struct cfs *fs, int rc)
{
    note(21, (uint64_t)(int64_t)rc);
    fs->failed = rc;
}

void cfs_clone_free(struct cfs *fs, uint64_t dva)
{
    (void)fs;
    note(22, dva);
}

/* ---- building a filesystem ---- */

#define MAXMEM 3u
#define CHUNK CFS_BITS_PER_BITMAP

static struct cfs_clone g_clone;

static uint64_t g_total;   /* bits the bitmap has room for */

static void build(struct cfs *fs)
{
    unsigned f = pick(4);
    g_faults = f != 0;
    unsigned nm = 1 + pick(MAXMEM);
    fs->nmembers = nm;
    uint64_t base = 0;
    fs->free_blocks = 0;
    for (unsigned v = 0; v < nm; v++) {
        struct cfs_memstate *m = &fs->mem[v];
        memset(m, 0, sizeof(*m));
        unsigned size = pick(4);
        m->nblocks = v == 0 ? META_BASE + NMETA + 50 + pick(400)
                            : size == 0 ? 2 * CHUNK + pick(500) : 3 + pick(300);
        m->first_usable = v == 0 ? 2 : 1;
        m->base = base;
        m->chunk0 = (unsigned)(base / CHUNK);
        m->nchunks = (unsigned)((m->nblocks + CHUNK - 1) / CHUNK);
        base += (uint64_t)m->nchunks * CHUNK;
        m->alloc_hint = pick(3) == 0 ? m->nblocks + 3 : pick((unsigned)m->nblocks);
    }
    g_total = base;
    fs->nblocks = base;
    memset(fs->bitmap, 0, (size_t)((g_total + 7) / 8));
    memset(fs->bitmap_dirty, 0, 16);
    /* Allocated: the blocks below first_usable, the metadata area, and a random share of the rest. */
    unsigned dens = pick(4);
    unsigned full = dens == 0 ? 2 : dens == 1 ? 10 : dens == 2 ? 50 : 98;
    for (unsigned v = 0; v < nm; v++) {
        struct cfs_memstate *m = &fs->mem[v];
        /* In runs of random length, taken or free by the density. */
        uint64_t b = 0;
        while (b < m->nblocks) {
            uint64_t len = 1 + pick(pick(8) == 0 ? 3000 : 40);
            bool run_taken = pick(100) < full;
            for (uint64_t e = b + len; b < e && b < m->nblocks; b++) {
                bool taken = run_taken || b < m->first_usable || (v == 0 && b >= META_BASE && b < META_BASE + NMETA);
                if (taken)
                    cfs_bit_set(fs->bitmap, m->base + b);
                else
                    m->free_blocks++;
            }
        }
        fs->free_blocks += m->free_blocks;
    }
    if (chance(10))
        fs->free_blocks += 1 + pick(5);   /* a total the bitmap does not bear out: a full scan finds nothing */
    unsigned r = pick(3);
    fs->reserve = r == 0 ? 0 : r == 1 ? 8 : fs->free_blocks + 1;
    fs->alloc_hint = 0;
    fs->gen = 50 + pick(10);
    fs->failed = 0;
    fs->cur_clone = NULL;
    g_clone.id = 3;
    unsigned gf = pick(5);
    g_get_fail = g_faults && gf == 0 ? 8 : 0;
    unsigned nf = pick(5);
    g_new_fail = g_faults && nf == 0 ? 5 : 0;

    /* The inode map. */
    for (unsigned i = 0; i < NMETA; i++)
        g_meta[i].used = false;
    unsigned root = new_meta();
    stamp(fs, root, CFS_KIND_IMAP1);
    ((struct cfs_mhdr *)g_meta[root].data)->generation = fs->gen - 1;
    fs->sb.imap_root = meta_dva(root);
    unsigned wide = pick(6);
    unsigned l1s = wide == 0 ? 2 : 1;
    fs->sb.next_ino = 0;
    fs->sb.inode_count = pick(20);
    for (unsigned l1 = 0; l1 < l1s; l1++) {
        if (pick(5) == 0)
            continue;
        unsigned l0 = new_meta();
        stamp(fs, l0, chance(20) ? CFS_KIND_INODES : CFS_KIND_IMAP0);
        ((struct cfs_mhdr *)g_meta[l0].data)->generation = fs->gen - 1;
        ((uint64_t *)(g_meta[root].data + CFS_MHDR_SIZE))[l1] = meta_dva(l0);
        unsigned nib = 1 + pick(4);
        for (unsigned j = 0; j < nib; j++) {
            if (pick(4) == 0)
                continue;
            unsigned ib = new_meta();
            stamp(fs, ib, chance(20) ? CFS_KIND_IMAP0 : CFS_KIND_INODES);
            ((struct cfs_mhdr *)g_meta[ib].data)->generation = pick(2) == 0 ? fs->gen : fs->gen - 1;
            ((uint64_t *)(g_meta[l0].data + CFS_MHDR_SIZE))[j] = meta_dva(ib);
            for (unsigned k = 0; k < CFS_INODES_PER_BLOCK; k++) {
                uint64_t ino = ((uint64_t)l1 * CFS_PTRS_PER_BLOCK + j) * CFS_INODES_PER_BLOCK + k;
                struct cfs_inode *in =
                    (struct cfs_inode *)(g_meta[ib].data + CFS_MHDR_SIZE + (size_t)k * CFS_INODE_SIZE);
                if (pick(3) == 0)
                    continue;
                in->ino = chance(15) ? ino + 1 : ino;
                in->nlink = pick(4) == 0 ? 0 : 1 + pick(3);
                in->mode = (1 + pick(3)) << 12;
                in->size = pick(100000);
                in->generation = pick(9);
                if (ino + 1 > fs->sb.next_ino)
                    fs->sb.next_ino = ino + 1;
            }
        }
    }
    if (fs->sb.next_ino == 0 || pick(8) == 0)
        fs->sb.next_ino += pick(40);
    if (pick(25) == 0)
        fs->sb.next_ino = CFS_MAX_INODES - pick(2);   /* the map's own bound */
    note(30, nm);
}

/* ---- the operations ---- */

static uint64_t some_dva(struct cfs *fs)
{
    unsigned k = pick(12);
    if (k == 0)
        return 0;
    if (k == 1)
        return CFS_DVA(7, 2);
    unsigned v = pick(fs->nmembers);
    if (k == 2)
        return CFS_DVA(v, fs->mem[v].nblocks + pick(3));
    return CFS_DVA(v, pick((unsigned)fs->mem[v].nblocks));
}

static uint64_t some_ino(struct cfs *fs)
{
    unsigned k = pick(10);
    if (k == 0)
        return 0;
    if (k == 1)
        return fs->sb.next_ino + pick(3);
    if (k == 2)
    {
        unsigned tag = pick(2) == 0 ? 3 : 5;
        uint64_t n = 1 + pick(40);
        return CFS_SNAP_INO(tag, n);   /* a tree's tagged number */
    }
    if (k == 3)
        return CFS_MAX_INODES + pick(2);
    return pick(fs->sb.next_ino < 2000 ? (unsigned)fs->sb.next_ino + 1 : 2000);
}

static void mix_inode(const struct cfs_inode *in)
{
    note(40, in->ino);
    note(41, in->nlink);
    note(42, in->mode);
    note(43, in->size);
    note(44, in->generation);
}

static void op(struct cfs *fs)
{
    unsigned which = pick(16);
    note(50, which);
    unsigned fi = pick(10);
    g_fail_in = fi == 0 ? (int)pick(2) : -1;
    unsigned cl = pick(5);
    fs->cur_clone = cl == 0 ? &g_clone : NULL;
    int rc = 0;
    switch (which) {
    case 0:
    case 1:
    case 2: {
        uint64_t start = 99, got = 99;
        unsigned cls = pick(2);
        unsigned want = pick(5) == 0 ? 0 : 1 + pick(pick(4) == 0 ? 400 : 8);
        rc = cfs_alloc_run(fs, cls == 0 ? CFS_ALLOC_META : CFS_ALLOC_DATA, some_dva(fs), want, &start, &got);
        note(51, start);
        note(52, got);
        break;
    }
    case 3: {
        uint64_t out = 99;
        rc = cfs_alloc_block(fs, &out);
        note(53, out);
        uint64_t start = 99, got = 99;
        uint64_t hint = some_dva(fs);
        unsigned want = 1 + pick(6);
        rc = cfs_alloc_data(fs, hint, want, &start, &got);
        note(54, start);
        note(55, got);
        break;
    }
    case 4:
        for (unsigned k = 0; k < 6; k++)
            note(56, cfs_bitmap_test(fs, pick(4) == 0 ? g_total + pick(9) : pick((unsigned)g_total)));
        break;
    case 5:
    case 6: {
        unsigned n = 1 + pick(pick(4) == 0 ? 90 : 6);
        for (unsigned k = 0; k < n; k++) {
            uint64_t d = some_dva(fs);
            unsigned ex = pick(2);
            if (ex == 0)
                cfs_free_block_exempt(fs, d);
            else
                cfs_free_block_deferred(fs, d);
        }
        break;
    }
    case 7:
    case 8: {
        unsigned n = 1 + pick(12);
        for (unsigned k = 0; k < n; k++) {
            uint64_t ino = pick(30);
            unsigned how = pick(4);
            if (how == 0)
                note(57, cfs_orphan_reserve(fs));
            else if (how == 1)
                cfs_orphan_add(fs, ino);
            else if (how == 2)
                cfs_orphan_remove(fs, ino);
            else
                note(58, cfs_orphan_named(fs, ino));
        }
        break;
    }
    case 9:
    case 10: {
        struct cfs_inode in;
        memset(&in, 0xEE, sizeof(in));
        uint64_t ino = some_ino(fs);
        unsigned how = pick(3);
        if (how == 0)
            rc = cfs_inode_read(fs, ino, &in);
        else if (how == 1)
            rc = cfs_inode_read_raw(fs, ino, &in);
        else
            rc = cfs_inode_read_at(fs, pick(5) == 0 ? CFS_DVA(0, 3) : fs->sb.imap_root, fs->sb.next_ino, ino, &in);
        if (rc == 0)
            mix_inode(&in);
        break;
    }
    case 11:
    case 12: {
        struct cfs_inode in;
        memset(&in, 0, sizeof(in));
        uint64_t ino = some_ino(fs);
        in.ino = CFS_INO_OF(ino);
        in.nlink = pick(3);
        in.mode = 1u << 12;
        in.size = pick(5000);
        rc = cfs_inode_write(fs, ino, &in);
        if (rc == 0) {
            struct cfs_inode back;
            memset(&back, 0xEE, sizeof(back));
            note(59, (uint64_t)(int64_t)cfs_inode_read_raw(fs, CFS_INO_OF(ino), &back));
            mix_inode(&back);
        }
        break;
    }
    case 13:
        rc = cfs_inode_prepare(fs, some_ino(fs));
        break;
    case 14: {
        uint64_t ino = 99;
        unsigned first = pick(4);
        if (first == 0)
            cfs_inode_discard(fs, fs->sb.next_ino - 1);   /* a discard with nothing allocated: the count may be 0 */
        rc = cfs_inode_alloc(fs, &ino);
        note(60, ino);
        unsigned d = pick(3);
        if (d == 0)
            cfs_inode_discard(fs, ino);
        else if (d == 1)
            cfs_inode_discard(fs, pick(5));
        break;
    }
    case 15: {
        /* Until the pool is full. */
        unsigned n = 0;
        for (;;) {
            uint64_t start, got;
            unsigned cls = pick(2);
            if (cfs_alloc_run(fs, cls == 0 ? CFS_ALLOC_META : CFS_ALLOC_DATA, 0, 64, &start, &got) != 0)
                break;
            n++;
        }
        note(61, n);
        break;
    }
    }
    g_fail_in = -1;
    fs->cur_clone = NULL;
    note(70, (uint64_t)(int64_t)rc);
    note(71, fs->free_blocks);
    note(72, fs->alloc_hint);
    note(73, fs->nr_pending);
    note(74, fs->nr_exempt);
    note(75, fs->nr_orphans);
    note(76, fs->sb.next_ino);
    note(77, fs->sb.inode_count);
    note(78, fs->sb.imap_root);
    note(79, (uint64_t)(int64_t)fs->failed);
    EXPECT(g_bufs == 0);
}

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    harness_klog_hook = on_klog;
    harness_klog_min = KLOG_ERROR + 1;
    struct cfs *fs = calloc(1, sizeof(*fs));
    EXPECT(fs != NULL);
    fs->mem = calloc(MAXMEM, sizeof(*fs->mem));
    fs->bitmap = calloc(1, (size_t)(MAXMEM * 4 * CHUNK + 7) / 8);
    fs->bitmap_dirty = calloc(1, 16);
    EXPECT(fs->mem != NULL && fs->bitmap != NULL && fs->bitmap_dirty != NULL);
    for (unsigned round = 0; round < ROUNDS; round++) {
        build(fs);
        unsigned ops = 2 + pick(14);
        for (unsigned k = 0; k < ops; k++)
            op(fs);
        mix(fs->bitmap, (size_t)((g_total + 7) / 8));
        mix(fs->bitmap_dirty, 16);
        for (unsigned v = 0; v < fs->nmembers; v++) {
            note(80, fs->mem[v].free_blocks);
            note(81, fs->mem[v].alloc_hint);
        }
        for (unsigned i = 0; i < fs->nr_pending; i++)
            note(82, fs->pending_free[i]);
        for (unsigned i = 0; i < fs->nr_exempt; i++)
            note(83, fs->pending_exempt[i]);
        for (unsigned i = 0; i < fs->nr_orphans; i++)
            note(84, fs->orphans[i]);
        for (unsigned i = 0; i < NMETA; i++)
            if (g_meta[i].used)
                mix(g_meta[i].data, 64 + 4 * CFS_INODE_SIZE);
        kfree(fs->pending_free);
        kfree(fs->pending_exempt);
        kfree(fs->orphans);
        fs->pending_free = fs->pending_exempt = fs->orphans = NULL;
        fs->nr_pending = fs->pending_cap = fs->nr_exempt = fs->exempt_cap = fs->nr_orphans = fs->orphan_cap = 0;
        EXPECT(g_live == 0);
    }
    free(fs->bitmap_dirty);
    free(fs->bitmap);
    free(fs->mem);
    free(fs);
    harness_klog_hook = NULL;
    harness_klog_min = KLOG_WARN;
    printf("  alloc digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, ROUNDS);
    EXPECT(g_digest == ALLOC_DIGEST);
}

int main(void)
{
    static const struct host_test tests[] = {
        { "alloc_digest", test_digest },
    };
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
