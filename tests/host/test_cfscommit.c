/*
 * test_cfscommit.c - cosmofs's commit (kernel-services/filesystem/cosmofs/cosmofs_commit.cee) on the host: roadmap 2.0
 * SA7. A digest recorded from the C over seeded filesystems the test builds and owns, with the real buffer cache and
 * header seals (cosmofs_buf.cee), the real allocator and inode map (cosmofs_alloc.cee) and the real decoders
 * (cfsparse): pools of one or two members whose allocation indexes, inode maps, free records and orphan records are on
 * a disk the test keeps; formats from version 8 to 15; a mount's replay of both records, sound, damaged, too long or
 * unreadable; transactions that allocate, free (deferred and exempt), write inodes and orphan them, committed once or
 * several times, with snapshots holding blocks, copies changing state during the commit, and the test hooks that stop
 * a commit; while reads, writes, flushes, kmalloc and every hook the commit calls fail now and then. Every write the
 * commit makes, every call, every argument, every return, every logged line, the superblock and the counters are in
 * the digest; nothing is left allocated and no buffer is left held.
 */

#include "harness.h"

#include <kernel/blk.h>
#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/list.h>
#include <kernel/log.h>
#include <kernel/storage.h>

#include "cosmofs_internal.h"   /* before the C library's headers, as test_cfsparse.c: its signal.h is the kernel's */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The values the CeeSafe port writes as its own constants. */
_Static_assert(EIO == 5 && ENOMEM == 12 && ENOSPC == 28 && KMEM_ZERO == 1 && BIO_PREFLUSH == 1 && BIO_FUA == 2 &&
                   POOL_WRITE_ONLINE == 0x80000000u && POOL_MAX_COPIES == 4,
               "cosmofs_commit's errno.h, kmalloc.h, blk.h and storage.h values");
_Static_assert(CFS_DVA_NONE == 0xFFFFFFFFFFFFFFFFull && CFS_DVA_VDEV_SHIFT == 56 &&
                   CFS_DVA_BLK_MASK == 0x00FFFFFFFFFFFFFFull && CFS_BLOCK == 4096 && CFS_MHDR_SIZE == 32 &&
                   CFS_BITS_PER_BITMAP == 32512 && CFS_SUPER_A == 0 && CFS_SUPER_B == 1 && CFS_INODE_SIZE == 256 &&
                   CFS_KIND_ALLOCIDX == 4 && CFS_KIND_BITMAP == 5 && CFS_KIND_FREELOG == 13 &&
                   CFS_KIND_ORPHAN == 14 && CFS_KIND_COPYSTATE == 17 && CFS_ALLOC_META == 0 &&
                   CFS_DEAD_PER_BLOCK == 506 && CFS_ORPHANS_PER_BLOCK == 506,
               "cosmofs_commit's cosmofs_format.h and cosmofs_internal.h values");
_Static_assert(offsetof(struct cfs_mhdr, kind) == 4 && offsetof(struct cfs_dead_block, next) == 0 &&
                   offsetof(struct cfs_dead_block, count) == 8 && offsetof(struct cfs_dead_block, blk) == 16,
               "cosmofs_commit.cee reads and writes a record block at these offsets");
/* cosmofs_commit.cee keeps an inode in a stack slot of 32 words and the reservation in one of 2. */
_Static_assert(sizeof(struct cfs_inode) <= 32 * 8 && sizeof(struct cfs_res) <= 2 * 8 && sizeof(bool) == 1,
               "cosmofs_commit.cee's stack slots");

/* The digest of the C (cosmofs_commit.c, 2026-11-07), which the CeeSafe port gives. */
#define COMMIT_DIGEST 0xddff362db23559a0ull
#define ROUNDS 2000u
#define SEED 0xC0331722026B11ADull

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

/* ---- the allocator: malloc, counted; an allocation fails when g_fail_in counts down to it ---- */

static int g_fail_in = -1;
static long g_live;

static bool fail_now(size_t size)
{
    if (g_fail_in >= 0 && g_fail_in-- == 0) {
        note(2, size);
        return true;
    }
    return false;
}

void *kmalloc(size_t size, unsigned flags)
{
    if (size == 0 || fail_now(size))
        return NULL;
    void *p = malloc(size);
    EXPECT(p != NULL);
    memset(p, (flags & KMEM_ZERO) ? 0 : 0xA5, size);
    g_live++;
    return p;
}

void *kzalloc(size_t size) { return kmalloc(size, KMEM_ZERO); }

void *krealloc(void *ptr, size_t new_size, unsigned flags)
{
    (void)flags;
    if (fail_now(new_size))
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

/* ---- the disk: up to two members of NB blocks, each member's linear base a whole bitmap chunk apart ---- */

#define NB 240u
#define NMEM 2u
#define CHUNK CFS_BITS_PER_BITMAP

static uint8_t *g_disk[NMEM][NB];   /* NULL: never written */
static unsigned g_nmem;
static struct spool g_pool;
static unsigned g_read_fail, g_write_fail;

static uint8_t *disk_at(uint64_t dva)
{
    unsigned v = CFS_DVA_VDEV(dva);
    uint64_t b = CFS_DVA_BLK(dva);
    EXPECT(v < g_nmem && b < NB);
    if (g_disk[v][b] == NULL) {
        g_disk[v][b] = calloc(1, CFS_BLOCK);
        EXPECT(g_disk[v][b] != NULL);
    }
    return g_disk[v][b];
}

static void disk_clear(void)
{
    for (unsigned v = 0; v < NMEM; v++)
        for (unsigned b = 0; b < NB; b++) {
            free(g_disk[v][b]);
            g_disk[v][b] = NULL;
        }
}

int pool_read(struct spool *p, uint64_t dva, void *buf)
{
    (void)p;
    note(3, dva);
    memcpy(buf, disk_at(dva), CFS_BLOCK);
    return 0;
}

static int write_block(uint64_t dva, const void *buf, unsigned flags)
{
    note(4, dva);
    note(5, flags);
    mix(buf, 64);
    mix((const uint8_t *)buf + CFS_BLOCK - 512, 512);
    if (g_write_fail != 0 && pick(g_write_fail) == 0)
        return -EIO;
    memcpy(disk_at(dva), buf, CFS_BLOCK);
    return 0;
}

int pool_write(struct spool *p, uint64_t dva, const void *buf)
{
    (void)p;
    return write_block(dva, buf, 0);
}

int pool_write_flags(struct spool *p, uint64_t dva, const void *buf, unsigned flags)
{
    (void)p;
    return write_block(dva, buf, flags);
}

static unsigned g_flush_fail;
static unsigned g_seq_bumps;   /* copy-state changes left to make during flushes */

int pool_flush(struct spool *p)
{
    note(6, p->state_seq);
    if (g_seq_bumps > 0 && (g_seq_bumps > 50 || pick(2) == 0)) {
        g_seq_bumps--;
        p->state_seq++;   /* a copy changed state while the commit's blocks were written */
    }
    if (g_flush_fail != 0 && pick(g_flush_fail) == 0)
        return -EIO;
    return 0;
}

void pool_commit_done(struct spool *p, uint64_t gen)
{
    note(7, gen);
    p->epoch = gen;
}

int cfs_read_repair(struct cfs *fs, uint64_t dva, void *buf, bool (*verify)(const void *blk, void *arg), void *arg,
                    bool *repaired, bool *read_ok)
{
    (void)fs;
    note(8, dva);
    *read_ok = false;
    if (repaired != NULL)
        *repaired = false;
    unsigned v = CFS_DVA_VDEV(dva);
    uint64_t b = CFS_DVA_BLK(dva);
    if (v >= g_nmem || b >= NB || g_disk[v][b] == NULL)
        return -EIO;
    if (g_read_fail != 0 && pick(g_read_fail) == 0)
        return -EIO;
    *read_ok = true;
    memcpy(buf, g_disk[v][b], CFS_BLOCK);
    return verify(buf, arg) ? 0 : -EIO;
}

int cfs_keytab_mac(const uint8_t *uuid, struct cfs_mac *mac)
{
    (void)uuid;
    (void)mac;
    return -ENOKEY;
}

/* A block of the pool, its two draws in a fixed order (an argument list or an operator would not fix it). */
static uint64_t some_block(void)
{
    unsigned v = pick(g_nmem);
    unsigned b = 2 + pick(NB - 2);
    return CFS_DVA(v, b);
}

/* ---- the hooks the commit calls, the test's ---- */

void cfs_note_dirty(struct cfs *fs)
{
    note(10, fs->nr_dirty);
}

void cfs_clone_free(struct cfs *fs, uint64_t dva)
{
    (void)fs;
    note(11, dva);
}

static unsigned g_hook_fail;   /* of the hooks' calls that fail */

static int hook_rc(void) { return g_hook_fail != 0 && pick(g_hook_fail) == 0 ? -EIO : 0; }

int cfs_members_store(struct cfs *fs)
{
    note(12, fs->sb.members);
    int rc = hook_rc();
    if (rc == 0 && fs->sb.members != 0 && pick(3) == 0) {
        /* The member table copied: a block taken, the old one freed. */
        uint64_t got, blk;
        rc = cfs_alloc_run(fs, CFS_ALLOC_META, 0, 1, &blk, &got);
        if (rc == 0) {
            cfs_free_block_deferred(fs, fs->sb.members);
            fs->sb.members = blk;
        }
    }
    return rc;
}

static bool g_states;   /* the members have copy-state records */

int cfs_copystate_prepare(struct cfs *fs)
{
    note(13, g_states);
    int rc = hook_rc();
    if (rc || !g_states)
        return rc;
    for (unsigned v = 0; v < fs->nmembers && rc == 0; v++) {
        struct cfs_buf *b;
        if (fs->mem[v].state_root == 0) {
            rc = cfs_buf_new(fs, CFS_KIND_COPYSTATE, &b);
            if (rc == 0)
                fs->mem[v].state_root = b->blkno;
            if (rc == 0)
                cfs_buf_put(fs, b);
        } else {
            rc = cfs_buf_get(fs, fs->mem[v].state_root, CFS_KIND_COPYSTATE, &b);
            if (rc == 0) {
                rc = cfs_buf_cow(fs, &b, &fs->mem[v].state_root);
                cfs_buf_put(fs, b);   /* held either way: the copy, or the original it could not copy */
            }
        }
    }
    return rc;
}

int cfs_copystate_fill(struct cfs *fs)
{
    note(14, fs->pool->state_seq);
    return hook_rc();
}

bool cfs_copystate_recorded(struct cfs *fs)
{
    (void)fs;
    unsigned r = pick(2);
    note(15, r);
    return r == 0;
}

int cfs_labels_update(struct cfs *fs)
{
    note(16, fs->sb.generation);
    if (g_seq_bumps > 0 && pick(3) == 0) {
        g_seq_bumps--;
        fs->pool->state_seq++;
    }
    return hook_rc();
}

int cfs_clones_publish(struct cfs *fs)
{
    note(17, fs->nclones);
    return hook_rc();
}

void cfs_clone_leave(struct cfs *fs)
{
    note(18, fs->cur_clone != NULL);
    fs->cur_clone = NULL;
}

void cfs_clone_enter(struct cfs *fs, struct cfs_clone *c)
{
    note(19, c->id);
    fs->cur_clone = c;
}

unsigned cfs_snapshot_reserve_bound(struct cfs *fs, unsigned pending_bound)
{
    note(20, pending_bound);
    return fs->snap_count == 0 ? 0 : 1 + pick(3);
}

unsigned cfs_snapshot_exempt_bound(struct cfs *fs)
{
    return fs->snap_count == 0 ? 0 : pick(3);
}

int cfs_snapshot_fill(struct cfs *fs, struct cfs_res *res, bool *held, unsigned n, unsigned *appended)
{
    (void)fs;
    note(21, n);
    note(22, res->n - res->used);
    *appended = 0;
    for (unsigned i = 0; i < n; i++)
        held[i] = pick(4) == 0;
    unsigned take = pick(4) == 0 ? res->n : pick(3);   /* now and then the whole reservation */
    for (unsigned i = 0; i < take; i++)
        note(23, cfs_res_take(res));
    if (chance(12))
        cfs_free_block_deferred(fs, CFS_DVA(0, 2 + pick(NB - 2)));   /* a fill that moves the pending list */
    unsigned app = pick(3);
    *appended = app;
    return hook_rc();
}

int cfs_truncate_blocks(struct cfs *fs, struct cfs_inode *in, uint64_t keep_blocks)
{
    note(24, in->ino);
    note(25, keep_blocks);
    if (in->direct[0].start != 0)
        cfs_free_block_deferred(fs, in->direct[0].start);
    in->direct[0].start = 0;
    return hook_rc();
}

/* ---- building a filesystem on the disk ---- */

static unsigned g_version;
static struct cfs_clone g_clone;

/* A block on the disk, sealed as `kind` in an older generation; its bit set in the bitmap. */
static uint64_t put_block(struct cfs *fs, unsigned v, uint32_t kind)
{
    for (unsigned tries = 0; tries < 64; tries++) {
        uint64_t b = 2 + pick(NB - 2);
        uint64_t lin = fs->mem[v].base + b;
        if (cfs_bit_test(fs->bitmap, lin))
            continue;
        cfs_bit_set(fs->bitmap, lin);
        fs->mem[v].free_blocks--;
        fs->free_blocks--;
        uint64_t dva = CFS_DVA(v, b);
        uint8_t *d = disk_at(dva);
        memset(d, 0, CFS_BLOCK);
        if (kind != 0)
            cfs_mhdr_seal_raw(&fs->mac, d, kind, dva, fs->gen - 2);
        return dva;
    }
    return 0;
}

static void reseal(struct cfs *fs, uint64_t dva)
{
    uint8_t *d = disk_at(dva);
    cfs_mhdr_seal_raw(&fs->mac, d, ((struct cfs_mhdr *)d)->kind, dva, fs->gen - 2);
}

/* A record chain of `kind`: FREELOG blocks naming blocks, ORPHAN ones naming inodes. */
static uint64_t make_record(struct cfs *fs, uint32_t kind, const uint64_t *items, unsigned nitems)
{
    bool loop = chance(30);
    if (loop)
        nitems = 0;   /* a record in a loop names nothing, or every replay of it goes round with them 4096 times */
    unsigned blocks = 1 + pick(2);
    uint64_t head = 0;
    unsigned at = 0;
    for (unsigned i = 0; i < blocks; i++) {
        uint64_t b = put_block(fs, 0, kind);
        if (b == 0)
            break;
        struct cfs_dead_block *d = (struct cfs_dead_block *)(disk_at(b) + CFS_MHDR_SIZE);
        d->next = head;
        unsigned k = 0;
        while (at < nitems && k < (i + 1 == blocks ? nitems : nitems / blocks))
            d->blk[k++] = items[at++];
        d->count = chance(20) ? CFS_DEAD_PER_BLOCK + 3 : k;
        reseal(fs, b);
        head = b;
    }
    if (head != 0 && loop) {
        ((struct cfs_dead_block *)(disk_at(head) + CFS_MHDR_SIZE))->next = head;   /* a record in a loop */
        reseal(fs, head);
    }
    if (head != 0 && chance(25))
        disk_at(head)[100] ^= 1;   /* damaged: it does not verify */
    return head;
}

static void make_inode(struct cfs *fs, uint64_t ino, uint32_t nlink)
{
    struct cfs_inode in;
    memset(&in, 0, sizeof(in));
    in.ino = ino;
    in.nlink = nlink;
    in.mode = 1u << 12;
    in.size = pick(9000);
    in.direct[0].start = pick(2) == 0 ? put_block(fs, 0, 0) : 0;
    in.direct[0].count = 1;
    /* Written through the real inode map, then the open transaction committed away: the test owns no buffer. */
    cfs_inode_write(fs, ino, &in);
}

static void build(struct cfs *fs)
{
    disk_clear();
    unsigned f = pick(4);
    g_faults = f != 0;
    static const unsigned versions[] = { 8, 9, 10, 11, 12, 13, 14, 15, 15 };
    g_version = versions[pick(sizeof(versions) / sizeof(versions[0]))];
    unsigned nm = pick(3);
    g_nmem = nm == 0 ? 2 : 1;
    memset(&g_pool, 0, sizeof(g_pool));
    g_pool.nmembers = g_nmem;
    fs->pool = &g_pool;
    fs->nmembers = g_nmem;
    fs->nr_chunks = g_nmem;
    fs->nblocks = (uint64_t)(g_nmem - 1) * CHUNK + NB;
    memset(fs->bitmap, 0, NMEM * CHUNK / 8);
    memset(fs->bitmap_dirty, 0, NMEM);
    fs->free_blocks = 0;
    for (unsigned v = 0; v < g_nmem; v++) {
        struct cfs_memstate *m = &fs->mem[v];
        memset(m, 0, sizeof(*m));
        m->nblocks = NB;
        m->first_usable = 2;
        m->base = (uint64_t)v * CHUNK;
        m->chunk0 = v;
        m->nchunks = 1;
        m->free_blocks = NB - 2;
        cfs_bit_set(fs->bitmap, m->base);
        cfs_bit_set(fs->bitmap, m->base + 1);
        fs->free_blocks += m->free_blocks;
    }
    memset(&fs->sb, 0, sizeof(fs->sb));
    fs->sb.version = g_version;
    fs->sb.generation = 20 + pick(5);
    fs->gen = fs->sb.generation + 1;
    fs->sb_slot = pick(2);
    fs->mac.on = false;
    fs->failed = 0;
    fs->rdonly = false;
    fs->reserve = pick(3) == 0 ? 16 : 0;
    list_init(&fs->bufs);
    fs->nr_bufs = fs->nr_dirty = 0;
    fs->cur_clone = NULL;
    fs->nclones = 0;
    fs->snap_count = pick(3) == 0 ? 1 + pick(3) : 0;
    fs->copystate_seq = 0;
    fs->test_fail_freelog = chance(20);
    fs->test_fail_orphan = chance(20);
    fs->test_commit_armed = chance(15);
    fs->test_commits_left = pick(3);
    g_states = g_version >= 13 && pick(2) == 0;
    unsigned rf = pick(8), wf = pick(8), ff = pick(8), hf = pick(4);
    g_read_fail = g_faults && rf == 0 ? 12 : 0;
    g_write_fail = g_faults && wf == 0 ? 25 : 0;
    g_flush_fail = g_faults && ff == 0 ? 6 : 0;
    g_hook_fail = g_faults && hf == 0 ? 12 : 0;
    g_seq_bumps = pick(4) == 0 ? 1 + pick(6) : 0;

    /* Each member's allocation index, and a bitmap chunk block it may name already. */
    for (unsigned v = 0; v < g_nmem; v++) {
        uint64_t idx = put_block(fs, v, CFS_KIND_ALLOCIDX);
        fs->mem[v].alloc_root = idx;
        if (pick(2) == 0) {
            uint64_t bm = put_block(fs, v, CFS_KIND_BITMAP);
            ((uint64_t *)(disk_at(idx) + CFS_MHDR_SIZE))[0] = bm;
            reseal(fs, idx);
        }
    }
    fs->sb.members = pick(2) == 0 ? put_block(fs, 0, CFS_KIND_MEMBERS) : 0;
    /* An inode map with a few inodes, some of them orphans: written in an earlier generation and committed. */
    uint64_t root = put_block(fs, 0, CFS_KIND_IMAP1);
    fs->sb.imap_root = root;
    fs->sb.next_ino = 2 + pick(20);
    unsigned saved_gen = (unsigned)fs->gen;
    fs->gen = fs->sb.generation - 1;
    uint64_t orphans[8];
    unsigned norph = 0;
    unsigned ninodes = pick(8);
    for (unsigned i = 0; i < ninodes; i++) {
        uint64_t ino = 1 + pick((unsigned)fs->sb.next_ino - 1);
        unsigned kind = pick(4);
        make_inode(fs, ino, kind == 0 ? 1 : 0);
        if (norph < 8 && kind != 3)
            orphans[norph++] = ino;
    }
    if (norph < 8 && pick(3) == 0)
        orphans[norph++] = fs->sb.next_ino + 5;   /* a number past the map */
    /* What those writes made: the blocks to disk, the cache emptied. */
    struct cfs_buf *b, *tmp;
    list_for_each_entry_safe(b, tmp, &fs->bufs, link) {
        const struct cfs_mhdr *h = (const struct cfs_mhdr *)b->data;
        cfs_mhdr_seal_raw(&fs->mac, b->data, h->kind, b->blkno, h->generation);   /* sealed as the commit would */
        memcpy(disk_at(b->blkno), b->data, CFS_BLOCK);
        list_remove(&b->link);
        kfree(b->data);
        kfree(b);
    }
    fs->nr_bufs = fs->nr_dirty = 0;
    kfree(fs->pending_free);
    kfree(fs->pending_exempt);
    fs->pending_free = fs->pending_exempt = NULL;
    fs->nr_pending = fs->pending_cap = fs->nr_exempt = fs->exempt_cap = 0;
    fs->gen = saved_gen;
    unsigned ic = pick(6);
    fs->sb.inode_count = ic == 0 ? 0 : ninodes + pick(2);   /* or a count already at 0 */
    /* The records. */
    uint64_t freed[12];
    unsigned nfreed = pick(12);
    for (unsigned i = 0; i < nfreed; i++)
        freed[i] = pick(4) == 0 ? CFS_DVA(5, 1) : some_block();
    for (unsigned i = 0; i < nfreed; i++) {
        uint64_t lin = cfs_dva_lin(fs, freed[i]);
        if (lin != CFS_DVA_NONE && !cfs_bit_test(fs->bitmap, lin) && pick(2) == 0) {
            cfs_bit_set(fs->bitmap, lin);
            fs->free_blocks--;
            fs->mem[CFS_DVA_VDEV(freed[i])].free_blocks--;
        }
    }
    fs->sb.free_root = g_version >= 9 && pick(3) != 0 ? make_record(fs, CFS_KIND_FREELOG, freed, nfreed) : 0;
    fs->sb.orphan_root = g_version >= 10 && pick(3) != 0 ? make_record(fs, CFS_KIND_ORPHAN, orphans, norph) : 0;
    if (chance(10))
        fs->free_blocks += 3;   /* a total the bitmap does not bear out */
    if (chance(6)) {
        /* Nearly full: a few free blocks left, so the reservation and the bitmap's fixpoint can run out. */
        unsigned left = pick(6);
        for (unsigned v = 0; v < g_nmem; v++)
            for (uint64_t k = 2; k < NB; k++) {
                uint64_t lin = fs->mem[v].base + k;
                if (!cfs_bit_test(fs->bitmap, lin) && fs->mem[v].free_blocks > left) {
                    cfs_bit_set(fs->bitmap, lin);
                    fs->mem[v].free_blocks--;
                    fs->free_blocks--;
                }
            }
    }
    if (pick(12) == 0)
        g_seq_bumps = 100;   /* a copy changing state at every flush: the commit stops waiting for it */
    if (!g_states && g_version >= 13 && chance(15))
        fs->mem[0].state_root = put_block(fs, 0, CFS_KIND_COPYSTATE);   /* on disk, and no buffer for it */
    note(30, g_version);
}

static void teardown(struct cfs *fs)
{
    struct cfs_buf *b, *tmp;
    list_for_each_entry_safe(b, tmp, &fs->bufs, link) {
        EXPECT(b->refs == 0);
        list_remove(&b->link);
        kfree(b->data);
        kfree(b);
    }
    fs->nr_bufs = fs->nr_dirty = 0;
    kfree(fs->pending_free);
    kfree(fs->pending_exempt);
    kfree(fs->orphans);
    fs->pending_free = fs->pending_exempt = fs->orphans = NULL;
    fs->nr_pending = fs->pending_cap = fs->nr_exempt = fs->exempt_cap = fs->nr_orphans = fs->orphan_cap = 0;
}

/* ---- the transaction's changes ---- */

static void change(struct cfs *fs)
{
    unsigned which = pick(8);
    note(40, which);
    switch (which) {
    case 0: {
        uint64_t start, got;
        unsigned cls = pick(2);
        note(41, (uint64_t)(int64_t)cfs_alloc_run(fs, cls == 0 ? CFS_ALLOC_META : CFS_ALLOC_DATA, 0, 1 + pick(4),
                                                   &start, &got));
        break;
    }
    case 1: {
        uint64_t dva = some_block();
        cfs_free_block_deferred(fs, dva);
        break;
    }
    case 2: {
        uint64_t dva = some_block();
        cfs_free_block_exempt(fs, dva);
        break;
    }
    case 3: {
        struct cfs_inode in;
        memset(&in, 0, sizeof(in));
        uint64_t ino = 1 + pick((unsigned)fs->sb.next_ino);
        in.ino = ino;
        in.nlink = 1;
        in.mode = 1u << 12;
        note(42, (uint64_t)(int64_t)cfs_inode_write(fs, ino, &in));
        break;
    }
    case 4: {
        uint64_t ino = 1 + pick(20);
        if (cfs_orphan_reserve(fs))
            cfs_orphan_add(fs, ino);
        break;
    }
    case 5:
        fs->bitmap_dirty[pick(g_nmem)] = 1;
        break;
    case 6: {
        unsigned n = pick(5) == 0 ? 600 : 1 + pick(10);   /* sometimes more than a record block holds */
        uint64_t one = CFS_DVA(0, 2 + pick(NB - 2));
        unsigned same = pick(2);
        for (unsigned i = 0; i < n; i++) {
            if (same == 0)
                cfs_free_block_deferred(fs, one);   /* one block freed again and again: an entry each time */
            else
                cfs_free_block_deferred(fs, some_block());
        }
        if (pick(3) == 0) {
            cfs_free_block_exempt(fs, one);
            cfs_free_block_exempt(fs, one);   /* and exempt twice: the release finds its bit clear */
        }
        break;
    }
    case 7: {
        unsigned n = pick(4) == 0 ? 600 : 1 + pick(5);   /* more orphans than a record block holds */
        for (unsigned i = 0; i < n; i++)
            if (cfs_orphan_reserve(fs))
                cfs_orphan_add(fs, 1 + i);
        break;
    }
    }
}

static void mix_state(struct cfs *fs)
{
    note(50, fs->sb.generation);
    note(51, fs->sb.free_root);
    note(52, fs->sb.orphan_root);
    note(53, fs->sb.free_blocks);
    note(54, fs->sb.imap_root);
    note(55, fs->sb.inode_count);
    note(56, fs->sb.members);
    note(57, fs->gen);
    note(58, fs->sb_slot);
    note(59, fs->free_blocks);
    note(60, (uint64_t)(int64_t)fs->failed);
    note(61, fs->nr_pending);
    note(62, fs->nr_exempt);
    note(63, fs->nr_dirty);
    note(64, fs->commits);
    note(65, fs->replay_freed);
    note(66, fs->replay_orphans);
    note(67, fs->copystate_seq);
    for (unsigned v = 0; v < g_nmem; v++) {
        note(68, fs->mem[v].alloc_root);
        note(69, fs->mem[v].free_blocks);
        note(70, fs->mem[v].state_root);
    }
    mix(fs->bitmap_dirty, NMEM);
}

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    harness_klog_hook = on_klog;
    harness_klog_min = KLOG_ERROR + 1;
    struct cfs *fs = calloc(1, sizeof(*fs));
    EXPECT(fs != NULL);
    fs->mem = calloc(NMEM, sizeof(*fs->mem));
    fs->bitmap = calloc(1, NMEM * CHUNK / 8);
    fs->bitmap_dirty = calloc(1, NMEM);
    EXPECT(fs->mem != NULL && fs->bitmap != NULL && fs->bitmap_dirty != NULL);
    g_clone.id = 4;
    for (unsigned round = 0; round < ROUNDS; round++) {
        build(fs);
        /* The mount's half: the records replayed, on a writable mount or a read-only one. */
        unsigned ro = pick(10);
        fs->rdonly = ro == 0;
        unsigned rp = pick(4);
        if (rp != 0) {
            unsigned fi = pick(12);
            g_fail_in = fi == 0 ? (int)pick(3) : -1;
            note(80, (uint64_t)(int64_t)cfs_freelog_replay(fs));
            note(81, (uint64_t)(int64_t)cfs_orphan_replay(fs));
            g_fail_in = -1;
        }
        unsigned commits = 1 + pick(3);
        for (unsigned c = 0; c < commits; c++) {
            unsigned nchanges = pick(10);
            for (unsigned k = 0; k < nchanges; k++)
                change(fs);
            unsigned fi = pick(10);
            g_fail_in = fi == 0 ? (int)pick(8) : -1;
            unsigned how = pick(4);
            unsigned inclone = pick(6);
            if (inclone == 0)
                fs->cur_clone = &g_clone;
            int rc;
            if (how == 0) {
                bool published = true;
                unsigned pn = pick(4);
                rc = cfs_commit_ex(fs, pn == 0 ? NULL : &published);
                note(82, published);
            } else {
                rc = cfs_commit(fs);
            }
            g_fail_in = -1;
            fs->cur_clone = NULL;
            note(83, (uint64_t)(int64_t)rc);
            mix_state(fs);
        }
        if (pick(8) == 0) {
            /* The root written on its own, into either slot. */
            unsigned fi = pick(4);
            g_fail_in = fi == 0 ? 0 : -1;
            unsigned slot = pick(2);
            unsigned fua = pick(2);
            note(84, (uint64_t)(int64_t)cfs_super_write(fs, slot, fua == 0 ? 0 : BIO_FUA));
            g_fail_in = -1;
        }
        if (pick(10) == 0) {
            int before = fs->failed;
            cfs_fail(fs, pick(2) == 0 ? 0 : -ENOSPC);
            note(85, (uint64_t)(int64_t)before);
            note(86, (uint64_t)(int64_t)fs->failed);
        }
        teardown(fs);
        EXPECT(g_live == 0);
    }
    disk_clear();
    free(fs->bitmap_dirty);
    free(fs->bitmap);
    free(fs->mem);
    free(fs);
    harness_klog_hook = NULL;
    harness_klog_min = KLOG_WARN;
    printf("  commit digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, ROUNDS);
    EXPECT(g_digest == COMMIT_DIGEST);
}

int main(void)
{
    static const struct host_test tests[] = {
        { "commit_digest", test_digest },
    };
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
