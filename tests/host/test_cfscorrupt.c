/*
 * test_cfscorrupt.c - cosmofs's corruption hook (kernel-services/filesystem/cosmofs/cosmofs_corrupt.c) on the host:
 * roadmap 2.0 SA7. A digest recorded from the C over every kind the hook knows and some it does not, with the real
 * DVA mapping, directory-entry and snapshot-list decoders (cfsparse): filesystems of every version with one member or
 * two, inode tables with files and directories of zero to four runs (some compressed, some past the pool), directory
 * blocks of short names, long names, continuations and garbage, snapshot lists that end, loop or run into a block of
 * noise, member tables and rollback records -- while inode reads and writes, the allocators, directory reads and
 * writes, buffer reads and kmalloc fail now and then. Every argument, return, written inode, directory block and
 * buffer, the bitmap and the counters are in the digest; the lock is let go, every buffer is put and nothing is left
 * allocated.
 */

#include "harness.h"

#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/mutex.h>
#include <kernel/vfs.h>

#include "cosmofs_internal.h"   /* before the C library's headers, as test_cfsparse.c: its signal.h is the kernel's */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The digest of the C (cosmofs_corrupt.c, 2026-11-07), which the CeeSafe port gives. */
#define CORRUPT_DIGEST 0x8e1237dd995b9aa1ull
#define ROUNDS 20000u
#define SEED 0xC0EEC7A72026C0EEull

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

static void fill(uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i += 8) {
        uint64_t v = next();
        memcpy(p + i, &v, n - i < 8 ? n - i : 8);
    }
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

/* ---- the lock ---- */

static bool g_locked;
static int g_bufs;   /* held */

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
    EXPECT(g_bufs == 0);
    g_locked = false;
    note(4, 0);
}

/* ---- the filesystem: one or two members, an inode table, directory blocks, a few disk blocks ---- */

#define NINO 24u
#define NDISK 12u

static struct cfs g_fs;
static struct mount g_mnt;
static struct cfs_memstate g_mem[2];
static uint8_t g_bitmap[2 * CFS_BITS_PER_BITMAP / 8];
static uint8_t g_bitmap_dirty[2];
static struct cfs_inode g_ino[NINO];
static bool g_present[NINO];
static uint8_t g_dir[NINO][CFS_BLOCK];
static uint64_t g_disk_blk[NDISK];
static uint8_t g_disk[NDISK][CFS_BLOCK];
static unsigned g_ndisk;
static uint64_t g_next_new, g_next_run;
static unsigned g_inode_fail, g_alloc_fail, g_dir_fail, g_buf_fail;

static uint64_t dva(unsigned v, uint64_t blk) { return ((uint64_t)v << CFS_DVA_VDEV_SHIFT) | blk; }

static uint8_t *disk_make(uint64_t blkno)
{
    EXPECT(g_ndisk < NDISK);
    g_disk_blk[g_ndisk] = blkno;
    fill(g_disk[g_ndisk], CFS_BLOCK);
    return g_disk[g_ndisk++];
}

int cfs_inode_read(struct cfs *fs, uint64_t ino, struct cfs_inode *out)
{
    EXPECT(fs == &g_fs);
    note(10, ino);
    if (g_inode_fail && pick(g_inode_fail) == 0) {
        note(11, 0);
        return -EIO;
    }
    if (ino >= NINO || !g_present[ino])
        return -ENOENT;
    *out = g_ino[ino];
    return 0;
}

int cfs_inode_write(struct cfs *fs, uint64_t ino, const struct cfs_inode *in)
{
    EXPECT(fs == &g_fs);
    EXPECT(g_locked);
    note(12, ino);
    mix(in, sizeof(*in));
    if (g_inode_fail && pick(g_inode_fail) == 0) {
        note(13, 0);
        return -EIO;
    }
    if (ino < NINO) {
        g_ino[ino] = *in;
        g_present[ino] = true;
    }
    return 0;
}

int cfs_inode_alloc(struct cfs *fs, uint64_t *ino)
{
    EXPECT(fs == &g_fs);
    EXPECT(g_locked);
    if (g_alloc_fail && pick(g_alloc_fail) == 0) {
        note(14, 0);
        return -ENOSPC;
    }
    for (uint64_t i = 2; i < NINO; i++) {
        if (!g_present[i]) {
            *ino = i;
            note(15, i);
            return 0;
        }
    }
    *ino = 100 + g_next_new;
    note(15, *ino);
    return 0;
}

int cfs_alloc_run(struct cfs *fs, enum cfs_alloc_class cls, uint64_t hint, uint32_t want, uint64_t *start,
                  uint64_t *got)
{
    EXPECT(fs == &g_fs);
    EXPECT(g_locked);
    note(16, (uint64_t)cls);
    note(17, hint);
    note(18, want);
    if (g_alloc_fail && pick(g_alloc_fail) == 0) {
        note(19, 0);
        return -ENOSPC;
    }
    uint64_t n = want;
    if (g_faults && pick(4) == 0)
        n = pick(want + 1);
    *start = dva(0, 600 + g_next_run);
    *got = n;
    g_next_run += want;
    return 0;
}

int cfs_dir_read_block_at(struct cfs *fs, const struct cfs_inode *dir, uint64_t lblk, uint8_t *block)
{
    EXPECT(fs == &g_fs);
    note(20, dir->ino);
    note(21, lblk);
    if (g_dir_fail && pick(g_dir_fail) == 0) {
        note(22, 0);
        return -EIO;
    }
    if (dir->ino >= NINO)
        return -EIO;
    memcpy(block, g_dir[dir->ino], CFS_BLOCK);
    return 0;
}

int cfs_dir_write_block_at(struct cfs *fs, struct cfs_inode *dir, uint64_t lblk, const uint8_t *block)
{
    EXPECT(fs == &g_fs);
    EXPECT(g_locked);
    note(23, dir->ino);
    note(24, lblk);
    mix(block, CFS_BLOCK);
    if (g_dir_fail && pick(g_dir_fail) == 0) {
        note(25, 0);
        return -EIO;
    }
    if (dir->ino < NINO)
        memcpy(g_dir[dir->ino], block, CFS_BLOCK);
    return 0;
}

static struct cfs_buf *buf_alloc(uint64_t blkno)
{
    struct cfs_buf *b = calloc(1, sizeof(*b));
    EXPECT(b != NULL);
    b->data = malloc(CFS_BLOCK);
    EXPECT(b->data != NULL);
    b->blkno = blkno;
    b->refs = 1;
    g_bufs++;
    return b;
}

int cfs_buf_get(struct cfs *fs, uint64_t blkno, uint32_t kind, struct cfs_buf **out)
{
    EXPECT(fs == &g_fs);
    note(30, blkno);
    note(31, kind);
    if (g_buf_fail && pick(g_buf_fail) == 0) {
        note(32, 0);
        return -EIO;
    }
    struct cfs_buf *b = buf_alloc(blkno);
    unsigned i = 0;
    while (i < g_ndisk && g_disk_blk[i] != blkno)
        i++;
    if (i < g_ndisk) {
        memcpy(b->data, g_disk[i], CFS_BLOCK);
    } else {
        /* A block nothing made: noise from its number alone. */
        uint64_t x = blkno * 0x9E3779B97F4A7C15ull + 1;
        for (unsigned k = 0; k < CFS_BLOCK; k += 8) {
            x ^= x >> 31;
            x *= 0xBF58476D1CE4E5B9ull;
            memcpy(b->data + k, &x, 8);
        }
    }
    *out = b;
    return 0;
}

int cfs_buf_new(struct cfs *fs, uint32_t kind, struct cfs_buf **out)
{
    EXPECT(fs == &g_fs);
    EXPECT(g_locked);
    note(33, kind);
    if (g_buf_fail && pick(g_buf_fail) == 0) {
        note(34, 0);
        return -ENOSPC;
    }
    struct cfs_buf *b = buf_alloc(dva(0, 900 + g_next_new++));
    memset(b->data, 0, CFS_BLOCK);
    b->dirty = true;
    *out = b;
    return 0;
}

void cfs_buf_mark_dirty(struct cfs *fs, struct cfs_buf *b)
{
    EXPECT(fs == &g_fs);
    note(35, b->blkno);
    b->dirty = true;
}

void cfs_buf_put(struct cfs *fs, struct cfs_buf *b)
{
    EXPECT(fs == &g_fs);
    EXPECT(g_bufs > 0);
    note(36, b->blkno);
    note(37, b->dirty);
    if (b->dirty)
        mix(b->data, CFS_BLOCK);
    g_bufs--;
    free(b->data);
    free(b);
}

/* The snapshot list's version-dependent accessors, as cosmofs_snap.cee has them. */
uint32_t cfs_snaplist_kind(const struct cfs *fs) { return fs->sb.version >= 12 ? CFS_KIND_SNAPLIST2 : CFS_KIND_SNAPLIST; }

unsigned cfs_snaplist_slots(const struct cfs *fs)
{
    return fs->sb.version >= 12 ? CFS_SNAPS_PER_BLOCK2 : CFS_SNAPS_PER_BLOCK;
}

uint64_t cfs_snaplist_next(const struct cfs_buf *b)
{
    uint64_t v;
    memcpy(&v, b->data + CFS_MHDR_SIZE, 8);
    return v;
}

void cfs_snaplist_set(const struct cfs *fs, struct cfs_buf *b, unsigned i, const struct cfs_snapshot *s)
{
    EXPECT(fs->sb.version >= 12);
    EXPECT(i < CFS_SNAPS_PER_BLOCK2);
    note(38, i);
    memcpy(&((struct cfs_snap_block2 *)(b->data + CFS_MHDR_SIZE))->snap[i], s, sizeof(*s));
}

/* ---- a round's filesystem ---- */

static const unsigned k_versions[] = { 3, 4, 9, 11, 12, 13, 14, 15 };

static uint64_t some_dva(void)
{
    unsigned v = pick(5) == 0 ? 2 : pick(2);
    uint64_t blk = pick(8) == 0 ? g_mem[v < 2 ? v : 0].nblocks + pick(3) : pick(4000);
    return dva(v, blk);
}

static void extents(struct cfs_inode *in)
{
    unsigned runs = pick(5);
    uint32_t lblk = pick(3);
    for (unsigned r = 0; r < runs && r < CFS_DIRECT; r++) {
        in->direct[r].start = some_dva();
        uint32_t count = 1 + pick(4);
        if (pick(10) == 0)
            count |= CFS_EXT_COMPRESSED;
        in->direct[r].count = count;
        in->direct[r].lblk = lblk;
        lblk += 1 + pick(4);
    }
    if (pick(4) == 0)
        memset(&in->direct[pick(3)], 0, sizeof(in->direct[0]));   /* a hole in the array */
}

static void name(char *out, unsigned len)
{
    for (unsigned i = 0; i < len; i++)
        out[i] = (char)('a' + pick(26));
}

static void directory(uint8_t *block, unsigned version)
{
    struct cfs_dirent *d = (struct cfs_dirent *)block;
    memset(block, 0, CFS_BLOCK);
    unsigned style = pick(6);
    if (style == 0)
        return;                                   /* empty */
    if (style == 1) {
        fill(block, CFS_BLOCK);                   /* noise */
        return;
    }
    unsigned s = 0;
    while (s < CFS_DIRENTS_PER_BLOCK) {
        unsigned what = pick(8);
        if (style == 2 && what < 6) {              /* full blocks, mostly */
            what = 6;
        }
        if (what < 3) {
            s++;                                  /* a free slot */
            continue;
        }
        char nm[CFS_NAME_MAX_LONG];
        unsigned len = what == 7 && version >= 15 ? 49 + pick(200) : 1 + pick(CFS_NAME_MAX);
        name(nm, len);
        unsigned need = 1 + cfs_dirent_extra(len);
        if (s + need > CFS_DIRENTS_PER_BLOCK)
            break;
        uint64_t ino = pick(6) == 0 ? 50 + pick(10) : 1 + pick(NINO - 1);
        unsigned type = pick(2) ? CFS_TYPE_DIR : CFS_TYPE_REG;
        cfs_dirent_put(d, s, ino, type, nm, len);
        if (what == 3 && pick(4) == 0)
            d[s].namelen = (uint8_t)(1 + pick(255));      /* a length its slots may not hold */
        if (what == 4 && pick(4) == 0)
            d[s].ino = CFS_DIRENT_CONT;                   /* a continuation with no head */
        s += need;
    }
}

static void snaplist(uint64_t blkno, uint64_t next_blk, unsigned version)
{
    uint8_t *p = disk_make(blkno) + CFS_MHDR_SIZE;
    memcpy(p, &next_blk, 8);
    bool empty = pick(3) == 0;
    if (version >= 12) {
        struct cfs_snap_block2 *l = (struct cfs_snap_block2 *)p;
        for (unsigned i = 0; i < CFS_SNAPS_PER_BLOCK2; i++) {
            unsigned w = empty ? 0 : pick(4);
            l->snap[i].name[0] = w == 0 ? 0 : 's';
            l->snap[i].id = w == 1 ? 0 : 1 + pick(100);
            l->snap[i].alloc_root = pick(3) == 0 ? 0 : 300 + pick(3);
        }
    } else {
        struct cfs_snap_block *l = (struct cfs_snap_block *)p;
        for (unsigned i = 0; i < CFS_SNAPS_PER_BLOCK; i++) {
            unsigned w = empty ? 0 : pick(4);
            l->snap[i].name[0] = w == 0 ? 0 : 's';
            l->snap[i].id = w == 1 ? 0 : 1 + pick(100);
            l->snap[i].alloc_root = pick(3) == 0 ? 0 : 300 + pick(3);
        }
    }
}

static void filesystem(void)
{
    memset(&g_fs, 0, sizeof(g_fs));
    memset(&g_mnt, 0, sizeof(g_mnt));
    memset(g_mem, 0, sizeof(g_mem));
    memset(g_ino, 0, sizeof(g_ino));
    memset(g_present, 0, sizeof(g_present));
    g_ndisk = 0;
    g_next_new = 0;
    g_next_run = 0;
    unsigned version = k_versions[pick(sizeof(k_versions) / sizeof(k_versions[0]))];
    g_fs.sb.version = version;
    g_fs.nmembers = version >= 4 ? 1 + pick(2) : 1;
    for (unsigned v = 0; v < g_fs.nmembers; v++) {
        g_mem[v].nblocks = 1000 + pick(3000);
        g_mem[v].first_usable = v == 0 ? 2 : 1;
        g_mem[v].base = (uint64_t)v * CFS_BITS_PER_BITMAP;
    }
    g_fs.mem = g_mem;
    fill(g_bitmap, sizeof(g_bitmap));
    memset(g_bitmap_dirty, 0, sizeof(g_bitmap_dirty));
    g_fs.bitmap = g_bitmap;
    g_fs.bitmap_dirty = g_bitmap_dirty;
    g_fs.free_blocks = pick(5000);
    g_fs.sb.free_blocks = g_fs.free_blocks;
    g_fs.sb.inode_count = pick(4);
    g_fs.sb.next_ino = NINO + pick(10);
    g_mnt.fs_priv = &g_fs;

    for (unsigned i = 1; i < NINO; i++) {
        if (i != 1 && pick(4) == 0)
            continue;
        if (i == 1 && pick(30) == 0)
            continue;                             /* no root */
        struct cfs_inode *in = &g_ino[i];
        g_present[i] = true;
        bool dir = i == 1 || pick(3) == 0;
        in->mode = CFS_MODE(dir ? CFS_TYPE_DIR : CFS_TYPE_REG, pick(2) ? 0755 : 0644);
        in->nlink = 1 + pick(3);
        in->ino = i;
        in->parent = CFS_ROOT_INO;
        in->csum_algo = 1 + pick(2);
        in->size = (uint64_t)pick(6) * CFS_BLOCK;
        if (pick(6) == 0)
            in->size = 64 * CFS_BLOCK;
        in->indirect = pick(5) == 0 ? some_dva() : 0;
        extents(in);
        if (dir)
            directory(g_dir[i], version);
        else
            memset(g_dir[i], 0, CFS_BLOCK);
    }

    /* The snapshot list: none, one to three blocks that end, loop to the head or run into noise. */
    unsigned shape = pick(5);
    if (shape > 0 && version >= 4) {
        g_fs.sb.snap_root = 200;
        unsigned n = 1 + pick(3);
        for (unsigned k = 0; k < n; k++) {
            uint64_t nx = k + 1 < n ? 201 + k : shape == 1 ? 200 : shape == 2 ? 250 : 0;
            snaplist(200 + k, nx, version);
        }
    }
    for (unsigned k = 0; k < 3; k++) {
        uint8_t *p = disk_make(300 + k) + CFS_MHDR_SIZE;
        uint64_t count = pick(CFS_MEMBERS_PER_BLOCK + 2);
        memcpy(p, &count, 8);
    }
    if (version >= 12 && pick(3) != 0) {
        g_fs.sb.rollback_root = 400;
        disk_make(400);
    }
}

static void op(void)
{
    unsigned kind = pick(24);
    unsigned which = pick(4);
    uint64_t ino = which == 0 ? 0 : which == 1 ? 1 : 1 + pick(NINO + 3);
    note(50, kind);
    note(51, ino);
    unsigned nomount = pick(40);
    g_mnt.fs_priv = nomount == 0 ? NULL : &g_fs;
    unsigned kf = pick(8);
    g_fail_in = g_faults && kf <= 1 ? (int)pick(3) : -1;
    uint64_t what = 0xEEEE;
    unsigned want = pick(5);
    int rc = cosmofs_test_corrupt(&g_mnt, (enum cosmofs_corruption)kind, ino, want == 0 ? NULL : &what);
    g_fail_in = -1;
    note(52, (uint64_t)(int64_t)rc);
    note(53, what);
    note(54, g_fs.free_blocks);
    note(55, g_fs.sb.free_blocks);
    note(56, g_fs.sb.inode_count);
    mix(g_bitmap, sizeof(g_bitmap));
    mix(g_bitmap_dirty, sizeof(g_bitmap_dirty));
    for (unsigned i = 0; i < NINO; i++) {
        note(57, g_present[i]);
        if (g_present[i])
            mix(&g_ino[i], sizeof(g_ino[i]));
    }
    EXPECT(!g_locked);
    EXPECT(g_bufs == 0);
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
        filesystem();
        unsigned a = pick(5), b = pick(5), c = pick(5), d = pick(5);
        g_inode_fail = g_faults && a == 0 ? 4 : 0;
        g_alloc_fail = g_faults && b == 0 ? 4 : 0;
        g_dir_fail = g_faults && c == 0 ? 4 : 0;
        g_buf_fail = g_faults && d == 0 ? 3 : 0;
        op();
    }
    harness_klog_hook = NULL;
    harness_klog_min = KLOG_WARN;
    printf("  corrupt digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, ROUNDS);
    EXPECT(g_digest == CORRUPT_DIGEST);
}

int main(void)
{
    static const struct host_test tests[] = {
        { "corrupt_digest", test_digest },
    };
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
