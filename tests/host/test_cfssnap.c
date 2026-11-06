/*
 * test_cfssnap.c - cosmofs's snapshots (kernel-services/filesystem/cosmofs/cosmofs_snap.c) on the host: roadmap 2.0
 * SA7. A digest recorded from the C over seeded filesystems the test builds and owns, with the real list decoders
 * (cfsparse): snapshot lists in the version-11 and version-12 layouts, one block or several, in a loop or unreadable;
 * entries with and without identity, clones among them; member tables (or, below version 4, one index) whose bitmaps
 * say which blocks each snapshot occupies; deadlists of one block or more, full, cyclic or unreadable. Each round runs
 * a random sequence of the file's operations -- create, delete, find, list, the uuid lookups and draws, the verdicts
 * and the commit's fill, the raise to 12, the identity repair, the walks, the deadlist filter, the clone entry --
 * while commits, copies, block allocations, reads and kmalloc fail now and then; directed rounds after them make the
 * commit's fill, a deletion and the identity repair fail at each of their steps. Every call the snapshots make, every
 * argument, every return, every logged line, every output and every block they mark dirty are in the digest; nothing
 * is left allocated and no buffer is left held.
 */

#include "harness.h"

#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/random.h>
#include <kernel/timer.h>

#include "cosmofs_internal.h"   /* before the C library's headers, as test_cfsparse.c: its signal.h is the kernel's */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The digest of the C (cosmofs_snap.c, 2026-11-07), which the CeeSafe port gives. */
#define SNAP_DIGEST 0xcdbb968635a6b0f6ull
#define ROUNDS 12000u
#define SEED 0x5A9C0FFEE2026B11ull

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

/* Faults: none, or now and then. */
static bool g_faults;

static bool chance(unsigned n) { return g_faults && pick(n) == 0; }

/* ---- the allocator: malloc, counted; an allocation fails when g_fail_in counts down to it ---- */

static int g_fail_in = -1;
static long g_live;

void *kmalloc(size_t size, unsigned flags)
{
    if (size == 0)
        return NULL;   /* as kernel/memory/kmalloc.c */
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

/* ---- the clock and the random source ---- */

static uint64_t g_clock = 1700000000000000000ull;

uint64_t clock_realtime_ns(void)
{
    g_clock += 1000003;
    return g_clock;
}

/* The uuid draws: cosmofs_snap.c's counter, mirrored, so that a draw can be made to come out as a uuid already taken. */
static uint64_t g_draws;
static uint8_t g_given[8][16];   /* the last uuids drawn, as the caller finished them */
static unsigned g_ngiven;
static uint8_t g_target[16];     /* a uuid on the list, when one is wanted */
static bool g_have_target;

void random_get_bytes(void *buf, size_t len)
{
    uint8_t *out = buf;
    note(3, len);
    for (size_t i = 0; i < len; i++)
        out[i] = (uint8_t)next();
    if (len != 16)
        return;
    g_draws++;
    unsigned how = pick(5);
    const uint8_t *want = NULL;
    if (how == 0 && g_ngiven > 0)
        want = g_given[pick(g_ngiven < 8 ? g_ngiven : 8)];
    else if (how == 1 && g_have_target)
        want = g_target;
    if (want != NULL) {
        /* What the caller mixes in next: the clock it reads and its counter, which is this draw's number. */
        uint64_t m = (g_clock + 1000003) ^ (g_draws * 0x9E3779B97F4A7C15ull);
        memcpy(out, want, 16);
        for (unsigned k = 0; k < 8; k++)
            out[k] ^= (uint8_t)(m >> (8 * k));
    }
    /* What it will be once finished, so a later draw can repeat it. */
    uint64_t m = (g_clock + 1000003) ^ (g_draws * 0x9E3779B97F4A7C15ull);
    uint8_t fin[16];
    memcpy(fin, out, 16);
    for (unsigned k = 0; k < 8; k++)
        fin[k] ^= (uint8_t)(m >> (8 * k));
    fin[6] = (uint8_t)((fin[6] & 0x0Fu) | 0x40u);
    fin[8] = (uint8_t)((fin[8] & 0x3Fu) | 0x80u);
    memcpy(g_given[g_ngiven % 8], fin, 16);
    g_ngiven++;
}

/* ---- the pool: up to two members of NB blocks ---- */

#define NB 400u
#define NMEM 2u

struct block {
    _Alignas(8) uint8_t data[CFS_BLOCK];
    bool used;    /* the builder or the model's allocator handed it out */
    bool alloc;   /* the live allocation bitmap's bit */
    bool dirty;   /* marked dirty this round */
};

static struct block g_blk[NMEM * NB];
static unsigned g_nmem;
static unsigned g_fail_pct;
static unsigned g_bufs;

static uint64_t real_lin(uint64_t dva)
{
    unsigned v = CFS_DVA_VDEV(dva);
    uint64_t b = CFS_DVA_BLK(dva);
    if (v >= g_nmem || b >= NB)
        return CFS_DVA_NONE;
    return v * NB + b;
}

static struct cfs_mhdr *hdr_of(uint64_t lin) { return (struct cfs_mhdr *)g_blk[lin].data; }

static uint8_t *payload(uint64_t lin) { return g_blk[lin].data + CFS_MHDR_SIZE; }

bool cfs_bitmap_test(const struct cfs *fs, uint64_t lin)
{
    (void)fs;
    note(4, lin);
    return lin < NMEM * NB && g_blk[lin].alloc;
}

static unsigned g_get_fail_at;   /* this many reads from now, one fails; 0: none */
static bool g_list_after_free;   /* a list block's read fails once a block has been freed */
static bool g_freed;
static unsigned g_list_fail_at;   /* this many list-block reads from now, one fails; 0: none */
static int g_force_op = -1;   /* the directed rounds' operation */

static int draw_fail(void)
{
    unsigned r = pick(100);
    if (g_get_fail_at != 0 && --g_get_fail_at == 0)
        return -EIO;
    return r < g_fail_pct ? -EIO : 0;
}

/* The buffers handed out, from a pool: a walk of a list in a loop takes thousands. */
#define NBUFS 64u
static struct cfs_buf g_buf[NBUFS];
static bool g_buf_used[NBUFS];

static struct cfs_buf *buf_of(uint64_t dva)
{
    struct cfs_buf *b = NULL;
    for (unsigned i = 0; i < NBUFS && b == NULL; i++)
        if (!g_buf_used[i]) {
            g_buf_used[i] = true;
            b = &g_buf[i];
        }
    EXPECT(b != NULL);
    memset(b, 0, sizeof(*b));
    b->blkno = dva;
    b->data = g_blk[real_lin(dva)].data;
    b->refs = 1;
    g_bufs++;
    return b;
}

int cfs_buf_get(struct cfs *fs, uint64_t blkno, uint32_t kind, struct cfs_buf **out)
{
    (void)fs;
    note(10, blkno);
    note(11, kind);
    uint64_t lin = real_lin(blkno);
    int rc = draw_fail();
    bool list = kind == CFS_KIND_SNAPLIST || kind == CFS_KIND_SNAPLIST2;
    if (g_list_after_free && g_freed && list)
        rc = -EIO;
    if (list && g_list_fail_at != 0 && --g_list_fail_at == 0)
        rc = -EIO;
    if (rc == 0 && (lin == CFS_DVA_NONE || !g_blk[lin].used || (kind != 0 && hdr_of(lin)->kind != kind)))
        rc = -EIO;
    note(12, (uint64_t)(int64_t)rc);
    if (rc)
        return rc;
    *out = buf_of(blkno);
    return 0;
}

void cfs_buf_put(struct cfs *fs, struct cfs_buf *b)
{
    (void)fs;
    note(13, b->blkno);
    EXPECT(g_bufs > 0);
    g_bufs--;
    EXPECT(b >= g_buf && b < g_buf + NBUFS && g_buf_used[b - g_buf]);
    g_buf_used[b - g_buf] = false;
}

struct cfs_mhdr *cfs_buf_hdr(struct cfs_buf *b) { return (struct cfs_mhdr *)b->data; }

void cfs_buf_mark_dirty(struct cfs *fs, struct cfs_buf *b)
{
    (void)fs;
    note(14, b->blkno);
    g_blk[real_lin(b->blkno)].dirty = true;
}

/* A fresh block from the model's allocator, on member `v` where it can: 0 when none is free. */
static uint64_t take_block(unsigned v)
{
    unsigned start = pick(NB);
    for (unsigned tries = 0; tries < NB; tries++) {
        unsigned b = 2 + (start + tries) % (NB - 2);
        uint64_t lin = v * NB + b;
        if (!g_blk[lin].used) {
            g_blk[lin].used = true;
            g_blk[lin].alloc = true;
            memset(g_blk[lin].data, 0, CFS_BLOCK);
            return CFS_DVA(v, b);
        }
    }
    return 0;
}

static void stamp(struct cfs *fs, uint64_t dva, uint32_t kind)
{
    uint64_t lin = real_lin(dva);
    struct cfs_mhdr *h = hdr_of(lin);
    h->kind = kind;
    h->blkno = dva;
    h->generation = fs->gen;
    g_blk[lin].dirty = true;
}

static unsigned g_alloc_fail;   /* of the model's allocations that fail */

int cfs_buf_new(struct cfs *fs, uint32_t kind, struct cfs_buf **out)
{
    note(15, kind);
    if (g_alloc_fail != 0 && pick(g_alloc_fail) == 0)
        return pick(2) == 0 ? -ENOSPC : -ENOMEM;
    uint64_t dva = take_block(0);
    if (dva == 0)
        return -ENOSPC;
    stamp(fs, dva, kind);
    note(16, dva);
    *out = buf_of(dva);
    return 0;
}

int cfs_buf_new_at(struct cfs *fs, uint32_t kind, uint64_t blk, struct cfs_buf **out)
{
    note(17, kind);
    note(18, blk);
    if (g_alloc_fail != 0 && pick(g_alloc_fail) == 0)
        return -ENOMEM;
    uint64_t lin = real_lin(blk);
    EXPECT(lin != CFS_DVA_NONE);
    memset(g_blk[lin].data, 0, CFS_BLOCK);
    stamp(fs, blk, kind);
    *out = buf_of(blk);
    return 0;
}

uint64_t cfs_res_take(struct cfs_res *r) { return (r != NULL && r->used < r->n) ? r->blk[r->used++] : 0; }

void cfs_res_untake(struct cfs_res *r)
{
    if (r != NULL && r->used > 0)
        r->used--;
}

void cfs_free_block_deferred(struct cfs *fs, uint64_t blk)
{
    (void)fs;
    note(19, blk);
    g_freed = true;
}

void cfs_free_block_exempt(struct cfs *fs, uint64_t blk)
{
    (void)fs;
    note(20, blk);
}

int cfs_buf_cow_exempt(struct cfs *fs, struct cfs_buf **bp, uint64_t *parent_slot, struct cfs_res *res)
{
    struct cfs_buf *b = *bp;
    note(21, b->blkno);
    note(22, res != NULL);
    struct cfs_mhdr *h = cfs_buf_hdr(b);
    if (h->generation == fs->gen) {
        cfs_buf_mark_dirty(fs, b);
        return 0;
    }
    uint64_t nblk;
    if (res != NULL) {
        nblk = cfs_res_take(res);
        if (nblk == 0)
            return -ENOSPC;
    } else {
        if (g_alloc_fail != 0 && pick(g_alloc_fail) == 0)
            return -ENOSPC;
        nblk = take_block(CFS_DVA_VDEV(b->blkno) < g_nmem ? CFS_DVA_VDEV(b->blkno) : 0);
        if (nblk == 0)
            return -ENOSPC;
    }
    uint64_t lin = real_lin(nblk);
    memcpy(g_blk[lin].data, b->data, CFS_BLOCK);
    stamp(fs, nblk, h->kind);
    cfs_free_block_exempt(fs, b->blkno);
    cfs_buf_put(fs, b);
    *parent_slot = nblk;
    *bp = buf_of(nblk);
    return 0;
}

/* ---- the transaction and the rest of the filesystem the snapshots ask ---- */

static unsigned g_commit_fail;

int cfs_commit(struct cfs *fs)
{
    note(23, fs->gen);
    EXPECT(g_bufs == 0);
    if (g_commit_fail != 0 && pick(g_commit_fail) == 0)
        return -EIO;
    fs->sb.generation = fs->gen;
    fs->gen++;
    return 0;
}

void cfs_fail(struct cfs *fs, int rc)
{
    note(24, (uint64_t)(int64_t)rc);
    fs->failed = rc;
}

static unsigned g_protect, g_clones;

bool cfs_rollback_protects(struct cfs *fs, const struct cfs_snapshot *s)
{
    (void)fs;
    note(25, s->id);
    return g_protect != 0 && s->id % g_protect == 0;
}

bool cfs_snapshot_has_clones(struct cfs *fs, uint64_t id)
{
    (void)fs;
    note(26, id);
    return g_clones != 0 && id % g_clones == 1;
}

int cfs_clone_ready(struct cfs *fs, uint64_t id)
{
    (void)fs;
    note(27, id);
    return chance(4) ? -ENOMEM : 0;
}

int cfs_clone_delete(struct cfs *fs, const struct cfs_snapshot *doomed, uint64_t *freed)
{
    (void)fs;
    note(28, doomed->id);
    *freed = pick(9);
    return chance(3) ? -EIO : 0;
}

/* ---- building a filesystem ---- */

static unsigned g_version;
static bool g_cyclic;   /* a loop or a full deadlist somewhere: the quadratic test hook is not asked */
static bool g_looped;   /* a deadlist in a loop: one a filesystem */

/* A block of `kind` on member `v`, stamped with an old generation: the open transaction did not write it. */
static uint64_t meta_block(struct cfs *fs, unsigned v, uint32_t kind)
{
    uint64_t dva = take_block(v);
    if (dva == 0)
        return 0;
    struct cfs_mhdr *h = hdr_of(real_lin(dva));
    h->kind = chance(50) ? kind + 1 : kind;
    h->blkno = dva;
    unsigned g = pick(8);
    h->generation = g == 0 ? fs->gen : fs->gen - 1 - pick(3);   /* now and then one this transaction already wrote */
    return dva;
}

/* A bitmap saying which of the pool's blocks a snapshot's tree occupied. */
static uint64_t make_index(struct cfs *fs, unsigned v)
{
    uint64_t idx = meta_block(fs, v, CFS_KIND_ALLOCIDX);
    if (idx == 0)
        return 0;
    if (pick(6) != 0) {
        uint64_t bm = meta_block(fs, v, CFS_KIND_BITMAP);
        ((uint64_t *)payload(real_lin(idx)))[0] = bm;
        if (bm != 0) {
            uint8_t *bits = payload(real_lin(bm));
            for (unsigned i = 0; i < NB / 8; i++)
                bits[i] = (uint8_t)next();
        }
    }
    if (pick(4) == 0)
        ((uint64_t *)payload(real_lin(idx)))[1 + pick(4)] = chance(3) ? CFS_DVA(7, 3) : CFS_DVA(v, 2 + pick(NB - 2));
    return idx;
}

static uint64_t make_alloc_root(struct cfs *fs)
{
    if (g_version < 4)
        return make_index(fs, 0);
    uint64_t t = meta_block(fs, 0, CFS_KIND_MEMBERS);
    if (t == 0)
        return 0;
    struct cfs_member_block *mb = (struct cfs_member_block *)payload(real_lin(t));
    unsigned c = pick(10);
    mb->count = c == 0 ? CFS_MEMBERS_PER_BLOCK + 2 : c == 1 ? g_nmem - 1 : g_nmem;
    for (unsigned v = 0; v < g_nmem; v++)
        mb->m[v].alloc_root = pick(8) == 0 ? 0 : chance(10) ? t : make_index(fs, v);   /* or the table itself */
    return t;
}

static uint64_t make_deadlist(struct cfs *fs)
{
    unsigned n = pick(3);
    /* Decided first, and once a filesystem: a deadlist in a loop is kept short, or every walk of it is slow. */
    bool loop = !g_looped && chance(10);
    uint64_t head = 0;
    for (unsigned i = 0; i < n; i++) {
        uint64_t b = meta_block(fs, 0, CFS_KIND_DEADLIST);
        if (b == 0)
            break;
        struct cfs_dead_block *d = (struct cfs_dead_block *)payload(real_lin(b));
        d->next = head;
        unsigned full = pick(8);
        if (full == 0 && !loop)
            g_cyclic = true;   /* not a loop, but as dear to the quadratic hook */
        d->count = loop ? 1 + pick(3) : full == 0 ? CFS_DEAD_PER_BLOCK : full == 1 ? CFS_DEAD_PER_BLOCK - 1 : full == 2 ? 0 : chance(10) ? CFS_DEAD_PER_BLOCK + 5 : 1 + pick(5);
        for (uint64_t k = 0; k < d->count && k < CFS_DEAD_PER_BLOCK; k++)
            d->blk[k] = chance(20) ? CFS_DVA(6, k) : CFS_DVA(pick(g_nmem), 2 + pick(NB - 2));   /* or no member's */
        if (pick(4) == 0 && d->count > 1)
            d->blk[1] = d->blk[0];   /* one block named twice */
        head = b;
    }
    if (head != 0 && loop) {
        ((struct cfs_dead_block *)payload(real_lin(head)))->next = head;   /* a deadlist in a loop */
        g_cyclic = true;
        g_looped = true;
    }
    return head;
}

static uint8_t g_uuids[64][16];
static unsigned g_nuuids;

static void put_entry(struct cfs *fs, uint64_t list, unsigned i, const struct cfs_snapshot *s)
{
    struct cfs_buf b = { .blkno = list, .data = g_blk[real_lin(list)].data };
    cfs_snaplist_set(fs, &b, i, s);
}

static void make_snapshots(struct cfs *fs)
{
    fs->sb.snap_root = 0;
    fs->snap_count = 0;
    g_nuuids = 0;
    g_have_target = false;
    g_cyclic = false;
    g_looped = false;
    unsigned blocks = pick(4);
    uint32_t kind = g_version >= 12 ? CFS_KIND_SNAPLIST2 : CFS_KIND_SNAPLIST;
    if (chance(40)) {
        /* One empty block that names itself: every walk meets its guard, cheaply. */
        uint64_t b = meta_block(fs, 0, kind);
        if (b != 0) {
            fs->sb.snap_root = b;
            *(uint64_t *)payload(real_lin(b)) = b;
        }
        return;
    }
    unsigned slots = g_version >= 12 ? CFS_SNAPS_PER_BLOCK2 : CFS_SNAPS_PER_BLOCK;
    uint64_t id = 0, prev = 0;
    for (unsigned k = 0; k < blocks; k++) {
        uint64_t b = meta_block(fs, 0, kind);
        if (b == 0)
            break;
        if (prev == 0)
            fs->sb.snap_root = b;
        else
            *(uint64_t *)payload(real_lin(prev)) = b;
        prev = b;
        unsigned n = pick(5);
        unsigned full = pick(5);
        if (full == 0)
            n = slots;
        for (unsigned i = 0; i < n && i < slots; i++) {
            unsigned gap = pick(5);
            if (gap == 0 && full != 0)
                continue;   /* a free slot, but in a block that is full */
            struct cfs_snapshot s;
            memset(&s, 0, sizeof(s));
            id += 1 + pick(2);
            s.id = chance(15) ? id - 1 : id;
            if (pick(40) == 0)
                s.id = CFS_SNAP_ID_MAX + pick(2);
            s.generation = 10 + pick(20);
            s.imap_root = CFS_DVA(0, 2 + pick(NB - 2));
            s.next_ino = 5 + pick(9);
            s.inode_count = pick(9);
            s.alloc_root = pick(6) == 0 ? 0 : make_alloc_root(fs);
            s.deadlist = chance(30) ? CFS_DVA(6, 1) : make_deadlist(fs);   /* or a chain outside the pool */
            s.created_ns = pick(1000);
            unsigned clone = pick(3);
            if (g_version >= 12 && clone == 0) {
                s.flags = CFS_SNAP_F_CLONE;
                s.origin_id = 1 + pick(4);
            }
            for (unsigned q = 0; q < 16; q++)
                s.uuid[q] = (uint8_t)next();
            s.uuid[6] = (uint8_t)((s.uuid[6] & 0x0Fu) | 0x40u);
            s.uuid[8] = (uint8_t)((s.uuid[8] & 0x3Fu) | 0x80u);
            if (chance(10))
                memset(s.uuid, 0, 16);
            else if (chance(10) && g_nuuids > 0)
                memcpy(s.uuid, g_uuids[pick(g_nuuids)], 16);
            if (g_nuuids < 64)
                memcpy(g_uuids[g_nuuids++], s.uuid, 16);
            snprintf(s.name, sizeof(s.name), "s%u", pick(12));
            put_entry(fs, b, i, &s);
            fs->snap_count++;
        }
    }
    if (fs->sb.snap_root != 0 && g_version >= 12)
        ((struct cfs_snap_block2 *)payload(real_lin(fs->sb.snap_root)))->max_id = chance(6) ? id / 2 : id + pick(3);
    if (prev != 0 && fs->snap_count < 3 && chance(10)) {
        *(uint64_t *)payload(real_lin(prev)) = fs->sb.snap_root;   /* a list in a loop: a short one, or every walk is slow */
        g_cyclic = true;
    }
    if (g_nuuids > 0) {
        memcpy(g_target, g_uuids[pick(g_nuuids)], 16);
        g_have_target = true;
    }
    if (chance(10))
        fs->snap_count = 0;   /* a count the list does not agree with */
}

static void build(struct cfs *fs)
{
    for (unsigned i = 0; i < NMEM * NB; i++) {
        if (g_blk[i].used)
            memset(g_blk[i].data, 0, CFS_BLOCK);
        g_blk[i].used = false;
        g_blk[i].alloc = false;
        g_blk[i].dirty = false;
    }
    unsigned f = pick(4);
    g_faults = f != 0;
    static const unsigned versions[] = { 3, 4, 10, 11, 11, 12, 12, 13 };
    g_version = versions[pick(sizeof(versions) / sizeof(versions[0]))];
    unsigned nm = pick(3);
    g_nmem = nm == 0 ? 2 : 1;
    fs->nmembers = g_nmem;
    for (unsigned v = 0; v < g_nmem; v++) {
        fs->mem[v].nblocks = NB;
        fs->mem[v].first_usable = 2;
        fs->mem[v].base = (uint64_t)v * NB;
        fs->mem[v].alloc_root = 0;
        for (unsigned b = 0; b < 2; b++) {
            g_blk[v * NB + b].used = true;
            g_blk[v * NB + b].alloc = true;
        }
    }
    fs->nblocks = (uint64_t)g_nmem * NB;
    fs->sb.version = g_version;
    fs->sb.generation = 30;
    fs->gen = 31;
    fs->sb.imap_root = CFS_DVA(0, 2 + pick(NB - 2));
    fs->sb.next_ino = 20;
    fs->sb.inode_count = 9;
    fs->failed = chance(4) ? -EIO : 0;   /* already abandoned: an abandon says so once */
    fs->snap_max_id = pick(3) == 0 ? pick(30) : 0;
    fs->snap_walks = 0;
    fs->snap_verdicts = 0;
    fs->test_fail_snapfill = chance(12);
    fs->test_no_raise = pick(8) == 0;
    unsigned fp = pick(6);
    g_fail_pct = !g_faults ? 0 : fp == 0 ? 3 : fp <= 2 ? 15 : 0;
    unsigned af = pick(3);
    g_alloc_fail = !g_faults ? 0 : af == 0 ? 6 : af == 1 ? 2 : 0;
    unsigned cf = pick(4);
    g_commit_fail = g_faults && cf == 0 ? 4 : 0;
    unsigned pr = pick(3);
    g_protect = pr == 0 ? 3 : 0;
    unsigned cl = pick(3);
    g_clones = cl == 0 ? 4 : 0;
    fs->mem[0].alloc_root = make_index(fs, 0);
    fs->sb.members = g_version >= 4 ? make_alloc_root(fs) : 0;
    make_snapshots(fs);
    /* The live bitmap: what is used, and a few blocks more or less. */
    for (unsigned i = 0; i < NMEM * NB; i++)
        if (chance(30))
            g_blk[i].alloc = !g_blk[i].alloc;
    for (unsigned i = 0; i < NMEM * NB; i++)
        g_blk[i].dirty = false;
    note(50, g_version);
}

/* ---- what the operations are shown and asked ---- */

static void mix_snap(const struct cfs_snapshot *s)
{
    note(60, s->id);
    note(61, s->generation);
    note(62, s->imap_root);
    note(63, s->alloc_root);
    note(64, s->next_ino);
    note(65, s->inode_count);
    note(66, s->deadlist);
    note(67, s->created_ns);
    note(68, s->origin_id);
    note(69, s->flags);
    mix(s->uuid, 16);
    mix(s->name, sizeof(s->name));
}

static void on_mark(void *arg, uint64_t lin)
{
    note(70, lin);
    (*(unsigned *)arg)++;
}

static bool on_live(void *arg, uint64_t dva)
{
    unsigned m = *(unsigned *)arg;
    note(71, dva);
    return m != 0 && dva % m == 0;
}

/* walk_write's callback: renames, empties or leaves the entry it stops at. */
static bool on_write(struct cfs_snapshot *s, void *arg)
{
    unsigned *left = arg;
    note(72, s->id);
    if ((*left)-- != 0)
        return true;
    unsigned how = pick(3);
    if (how == 0)
        memset(s, 0, sizeof(*s));
    else if (how == 1)
        s->name[0] = (char)('a' + pick(26));
    return false;
}

static void name_for(char *out, size_t n)
{
    unsigned k = pick(14);
    if (k == 0)
        out[0] = '\0';
    else if (k == 1)
        snprintf(out, n, "a/b");
    else if (k == 2)
        snprintf(out, n, "%s", "a-name-that-is-longer-than-thirty-one-bytes");
    else
        snprintf(out, n, "s%u", pick(14));
}

/* A dva for the verdicts: a block of the pool, a padding block, a block past it. */
static uint64_t some_dva(void)
{
    unsigned k = pick(10);
    if (k == 0)
        return CFS_DVA(5, 3);
    if (k == 1)
        return CFS_DVA(0, NB + 3);
    return CFS_DVA(pick(g_nmem), pick(NB));
}

static void op(struct cfs *fs)
{
    /* The operations that do the most, more often; the walks of a list in a loop, when there is one. */
    static const unsigned more[] = { 3, 4, 12, 12, 12, 0, 17, 14 };
    unsigned w = pick(31);
    unsigned which = w < 23 ? w : more[w - 23];
    unsigned cyc = pick(4);
    if (g_cyclic && cyc < 2)
        which = cyc == 0 ? 10 : 16;
    if (g_force_op >= 0)
        which = (unsigned)g_force_op;
    note(80, which);
    unsigned fi = pick(12);
    g_fail_in = fi == 0 ? (int)pick(4) : -1;
    unsigned nth = pick(5);
    g_get_fail_at = g_faults && nth == 0 ? 1 + pick(16) : 0;
    unsigned dn = pick(2);
    g_freed = false;
    g_list_after_free = g_force_op == 3 && dn == 1;
    g_list_fail_at = g_force_op == 14 && dn == 1 ? 1 + pick(10) : 0;
    if ((g_force_op == 3 || g_force_op == 14) && dn == 0)
        g_get_fail_at = 1 + pick(g_force_op == 3 ? 200 : 40);   /* a directed deletion: one of its reads fails, at every step in turn */
    char name[64];
    struct cfs_snapshot s;
    memset(&s, 0, sizeof(s));
    int rc = 0;
    switch (which) {
    case 0:
    case 1: {
        name_for(name, sizeof(name));
        unsigned id = pick(3), o = pick(3);
        rc = cfs_snapshot_create(fs, pick(20) == 0 ? NULL : name, id != 0, o == 0 ? NULL : &s);
        break;
    }
    case 2: {
        name_for(name, sizeof(name));
        unsigned id = pick(3), c = pick(2), u = pick(3);
        uint8_t uuid[16];
        for (unsigned q = 0; q < 16; q++)
            uuid[q] = (uint8_t)next();
        rc = cfs_snapshot_create_ex(fs, name, id != 0, c == 0, u == 0 ? uuid : NULL, &s);
        break;
    }
    case 3:
    case 4:
        name_for(name, sizeof(name));
        rc = cfs_snapshot_delete(fs, pick(25) == 0 ? NULL : name);
        break;
    case 5: {
        name_for(name, sizeof(name));
        unsigned o = pick(2);
        rc = cfs_snapshot_find(fs, name, o == 0 ? NULL : &s);
        break;
    }
    case 6: {
        uint8_t uuid[16];
        unsigned k = pick(3);
        if (k == 0 && g_nuuids > 0)
            memcpy(uuid, g_uuids[pick(g_nuuids)], 16);
        else if (k == 1)
            memset(uuid, 0, 16);
        else
            for (unsigned q = 0; q < 16; q++)
                uuid[q] = (uint8_t)next();
        unsigned o = pick(2);
        rc = cfs_snapshot_lookup_uuid(fs, uuid, o == 0 ? NULL : &s);
        note(81, cfs_snapshot_find_uuid(fs, uuid, NULL));
        break;
    }
    case 7: {
        unsigned max = pick(5), count = 99;
        struct cfs_snapshot *all = calloc(max + 1, sizeof(*all));
        EXPECT(all != NULL);
        unsigned o = pick(4);
        rc = cfs_snapshot_list(fs, o == 0 ? NULL : all, max, &count);
        note(82, count);
        for (unsigned i = 0; i < max && i < count; i++)
            mix_snap(&all[i]);
        free(all);
        note(83, cfs_has_snapshots(fs));
        break;
    }
    case 8: {
        unsigned max = 0, count = 0;
        struct cfs_snapshot all[4];
        memset(all, 0, sizeof(all));
        rc = cfs_snapshot_list(fs, all, 4, &count);
        max = count < 4 ? count : 4;
        for (unsigned i = 0; i < max; i++)
            note(84, cfs_snapshot_references(fs, &all[i], some_dva()));
        s.alloc_root = 0;
        note(85, cfs_snapshot_references(fs, &s, some_dva()));
        break;
    }
    case 9:
        note(86, cfs_snapshot_holds(fs, some_dva()));
        break;
    case 10: {
        unsigned k = pick(2);
        note(87, cfs_snapshot_deadlist_len(fs, k == 0 ? 0 : some_dva()));
        uint64_t ex = 0, first = 0;
        unsigned o = pick(3);
        if (!g_cyclic)
            note(88, cfs_snapshot_deadlist_dups(fs, o == 0 ? NULL : &ex, o == 1 ? NULL : &first));
        note(89, ex);
        note(90, first);
        break;
    }
    case 11:
        note(91, cfs_snapshot_reserve_bound(fs, pick(3000)));
        note(92, cfs_snapshot_exempt_bound(fs));
        break;
    case 12: {
        /* The commit's verdicts and appends, from a reservation. */
        uint64_t pend[9], resv[8];
        bool held[9];
        unsigned n = pick(9);
        for (unsigned i = 0; i < n; i++)
            pend[i] = some_dva();
        struct cfs_res res = { .blk = resv, .n = 0, .used = 0 };
        unsigned want = pick(8);
        for (unsigned i = 0; i < want; i++) {
            uint64_t b = take_block(0);
            if (b != 0)
                resv[res.n++] = b;
        }
        uint64_t *saved = fs->pending_free;
        fs->pending_free = pend;
        unsigned appended = 99;
        unsigned r = pick(4);
        rc = cfs_snapshot_fill(fs, r == 0 ? NULL : &res, held, n, &appended);
        fs->pending_free = saved;
        note(93, appended);
        note(94, res.used);
        for (unsigned i = 0; i < n; i++)
            note(95, held[i]);
        break;
    }
    case 13:
        rc = cfs_snap_raise(fs);
        break;
    case 14: {
        unsigned fixed = 99;
        rc = cfs_snapshot_fix_identity(fs, &fixed);
        note(96, fixed);
        break;
    }
    case 15: {
        unsigned marks = 0;
        unsigned k = pick(4);
        uint64_t root = k == 0 ? 0 : k == 1 ? fs->sb.members : k == 2 ? fs->mem[0].alloc_root : some_dva();
        rc = cfs_alloc_walk(fs, root, on_mark, &marks);
        note(97, marks);
        break;
    }
    case 16: {
        struct cfs_snapshot all[3];
        unsigned count = 0;
        rc = cfs_snapshot_list(fs, all, 3, &count);
        for (unsigned i = 0; i < count && i < 3; i++) {
            unsigned marks = 0;
            note(98, (uint64_t)(int64_t)cfs_snapshot_walk_held(fs, &all[i], on_mark, &marks));
            note(99, marks);
        }
        break;
    }
    case 17: {
        unsigned m = pick(4);
        uint64_t dropped = 99;
        rc = cfs_snapshot_deadlists_filter(fs, on_live, &m, &dropped);
        note(100, dropped);
        break;
    }
    case 18:
        rc = cfs_snapshot_list_cow(fs);
        break;
    case 19: {
        unsigned left = pick(6);
        rc = cfs_snapshot_walk_write(fs, on_write, &left);
        break;
    }
    case 20: {
        struct cfs_snapshot all[2];
        unsigned count = 0;
        memset(all, 0, sizeof(all));
        rc = cfs_snapshot_list(fs, all, 2, &count);
        name_for(name, sizeof(name));
        unsigned nn = pick(15);
        rc = cfs_snapshot_add_clone(fs, &all[0], nn == 0 ? NULL : name, &s);
        break;
    }
    case 21: {
        uint64_t root = fs->sb.snap_root;
        if (root != 0 && real_lin(root) != CFS_DVA_NONE) {
            struct cfs_buf b = { .blkno = root, .data = g_blk[real_lin(root)].data };
            unsigned i = pick(45), k = pick(2);
            s.id = 77;
            snprintf(s.name, sizeof(s.name), "set");
            cfs_snaplist_set(fs, &b, i, k == 0 ? NULL : &s);
            note(101, cfs_snaplist_kind(fs));
            note(102, cfs_snaplist_slots(fs));
            note(103, cfs_snaplist_next(&b));
            g_blk[real_lin(root)].dirty = true;
        }
        break;
    }
    case 22: {
        uint8_t out[16], also[3][16];
        unsigned na = pick(4);
        for (unsigned i = 0; i < na; i++)
            memcpy(also[i], g_given[pick(8)], 16);
        cfs_snapshot_new_uuid(fs, out, (const uint8_t (*)[16])also, na);
        mix(out, 16);
        break;
    }
    }
    g_fail_in = -1;
    g_get_fail_at = 0;
    g_list_after_free = false;
    g_list_fail_at = 0;
    note(104, (uint64_t)(int64_t)rc);
    mix_snap(&s);
    note(105, fs->sb.snap_root);
    note(106, fs->sb.version);
    note(107, fs->snap_count);
    note(108, fs->snap_max_id);
    note(109, (uint64_t)(int64_t)fs->failed);
    note(110, fs->snap_walks);
    note(111, fs->snap_verdicts);
    EXPECT(g_bufs == 0);
    EXPECT(g_live == 0);
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
    EXPECT(fs->mem != NULL);
    for (unsigned round = 0; round < ROUNDS; round++) {
        build(fs);
        unsigned ops = 1 + pick(8);
        for (unsigned k = 0; k < ops; k++)
            op(fs);
        for (unsigned i = 0; i < NMEM * NB; i++)
            if (g_blk[i].dirty) {
                note(112, i);
                mix(g_blk[i].data, CFS_BLOCK);
            }
    }
    /* Directed rounds: the commit's fill with reads failing often, so that an append fails after another has landed;
     * deletions and the identity repair with one read failing, so that each fails at every step in turn. */
    for (unsigned round = 0; round < 600; round++) {
        do
            build(fs);
        while (round % 3 == 2 && g_version < 12);   /* the identity repair has nothing to do below 12 */
        g_faults = true;
        unsigned pct = 10 + pick(30);
        static const int directed[] = { 12, 3, 14 };
        g_fail_pct = round % 3 == 0 ? pct : 0;   /* a deletion reads its list whole, or never reaches the clone */
        g_force_op = directed[round % 3];
        for (unsigned k = 0; k < 4; k++)
            op(fs);
        g_force_op = -1;
    }
    free(fs->mem);
    free(fs);
    EXPECT(g_live == 0);
    harness_klog_hook = NULL;
    harness_klog_min = KLOG_WARN;
    printf("  snap digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, ROUNDS);
    EXPECT(g_digest == SNAP_DIGEST);
}

int main(void)
{
    static const struct host_test tests[] = {
        { "snap_digest", test_digest },
    };
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
