/*
 * cosmofs_core.c - Buffers, allocation, the inode map, transactions,
 * commit, format, mount and unmount.
 *
 * Rules (docs/kernel-services/vfs/design.md): a metadata block from a
 * committed generation is never modified in place; the in-memory bitmap
 * is authoritative during a transaction and its chunks are written at
 * commit through a reserve-then-write fixpoint; blocks freed in a
 * transaction become allocatable only after that transaction's root is
 * on disk.
 */

#include <kernel/blk.h>
#include <kernel/cosmofs.h>
#include <kernel/crc32c.h>
#include <kernel/errno.h>
#include <kernel/vfs.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/fwcfg.h>
#include <kernel/random.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/wait.h>

#include "cosmofs_internal.h"

/* --- checksums and headers -------------------------------------------- */

/* CRC32C over a block with the 4-byte crc field at crc_off taken as zero. */
static uint32_t block_crc(const uint8_t *block, size_t crc_off)
{
    static const uint8_t zero4[4] = { 0 };
    uint32_t c = crc32c(block, crc_off);
    c = crc32c_update(c, zero4, 4);
    return crc32c_update(c, block + crc_off + 4, CFS_BLOCK - crc_off - 4);
}

static void mhdr_seal(struct cfs *fs, uint8_t *block, uint32_t kind, uint64_t blkno)
{
    struct cfs_mhdr *h = (struct cfs_mhdr *)block;
    h->magic = CFS_MHDR_MAGIC;
    h->kind = kind;
    h->generation = fs->gen;
    h->blkno = blkno;
    h->pad = 0;
    h->crc = 0;
    h->crc = block_crc(block, offsetof(struct cfs_mhdr, crc));
}

static int mhdr_check(const uint8_t *block, uint64_t blkno, uint32_t kind)
{
    const struct cfs_mhdr *h = (const struct cfs_mhdr *)block;
    if (h->magic != CFS_MHDR_MAGIC || h->blkno != blkno || (kind && h->kind != kind))
        return -EIO;
    if (block_crc(block, offsetof(struct cfs_mhdr, crc)) != h->crc)
        return -EIO;
    return 0;
}

struct mhdr_want {
    uint64_t dva;
    uint32_t kind;
};

void cfs_mhdr_seal_raw(void *block, uint32_t kind, uint64_t dva, uint64_t generation)
{
    struct cfs fake;
    memset(&fake, 0, sizeof(fake));
    fake.gen = generation;
    mhdr_seal(&fake, block, kind, dva);
}

bool cfs_mhdr_ok(const void *block, uint64_t dva, uint32_t kind)
{
    return mhdr_check(block, dva, kind) == 0;
}

bool cfs_super_ok(const void *block)
{
    const struct cfs_super *sb = block;
    if (memcmp(sb->magic, CFS_MAGIC, 8) != 0 || sb->version < CFS_VERSION_MIN || sb->version > CFS_VERSION)
        return false;
    return block_crc(block, offsetof(struct cfs_super, crc)) == sb->crc;
}

/* The verifier cfs_read_repair calls for a metadata block. */
static bool mhdr_ok(const void *block, void *arg)
{
    const struct mhdr_want *w = arg;
    return mhdr_check(block, w->dva, w->kind) == 0;
}

struct cfs_mhdr *cfs_buf_hdr(struct cfs_buf *b)
{
    return (struct cfs_mhdr *)b->data;
}

int cfs_data_read(struct cfs *fs, uint64_t blk, void *buf)
{
    if (!cfs_dva_valid(fs, blk))
        return -EIO;
    return pool_read(fs->pool, blk, buf);
}

int cfs_data_write(struct cfs *fs, uint64_t blk, const void *buf)
{
    if (!cfs_dva_valid(fs, blk))
        return -EIO;
    return pool_write(fs->pool, blk, buf);
}

/* --- buffer cache -------------------------------------------------------- */

static struct cfs_buf *buf_find(struct cfs *fs, uint64_t blkno)
{
    struct cfs_buf *b;
    list_for_each_entry(b, &fs->bufs, link) {
        if (b->blkno == blkno)
            return b;
    }
    return NULL;
}

static void buf_evict_clean(struct cfs *fs)
{
    if (fs->nr_bufs < CFS_BUF_CACHE)
        return;
    struct cfs_buf *b;
    list_for_each_entry_reverse(b, &fs->bufs, link) {
        if (!b->dirty && b->refs == 0) {
            list_remove(&b->link);
            fs->nr_bufs--;
            kfree(b->data);
            kfree(b);
            return;
        }
    }
}

static struct cfs_buf *buf_alloc(struct cfs *fs, uint64_t blkno)
{
    buf_evict_clean(fs);
    struct cfs_buf *b = kzalloc(sizeof(*b));
    if (b == NULL)
        return NULL;
    b->data = kmalloc(CFS_BLOCK, KMEM_ZERO);
    if (b->data == NULL) {
        kfree(b);
        return NULL;
    }
    list_init(&b->link);
    b->blkno = blkno;
    b->refs = 1;
    list_push_front(&fs->bufs, &b->link);
    fs->nr_bufs++;
    return b;
}

int cfs_buf_get(struct cfs *fs, uint64_t blkno, uint32_t kind, struct cfs_buf **out)
{
    if (!cfs_dva_valid(fs, blkno))
        return -EIO;
    struct cfs_buf *b = buf_find(fs, blkno);
    if (b) {
        b->refs++;
        list_remove(&b->link);
        list_push_front(&fs->bufs, &b->link);
        if (kind && cfs_buf_hdr(b)->kind != kind)
            return -EIO;
        *out = b;
        return 0;
    }
    b = buf_alloc(fs, blkno);
    if (b == NULL)
        return -ENOMEM;
    /* A metadata block checks itself, so a mirrored member can answer
     * from another copy and put the bad one right (design.md, "A mirror
     * is only as good as its verifier"). */
    struct mhdr_want want = { .dva = blkno, .kind = kind };
    int rc = cfs_read_repair(fs, blkno, b->data, mhdr_ok, &want, NULL);
    if (rc) {
        kerror("cosmofs: block %llu: %s", (unsigned long long)blkno,
               rc == -EIO ? "bad metadata header or checksum" : "read error");
        list_remove(&b->link);
        fs->nr_bufs--;
        kfree(b->data);
        kfree(b);
        return rc;
    }
    *out = b;
    return 0;
}

void cfs_buf_put(struct cfs *fs, struct cfs_buf *b)
{
    (void)fs;
    KASSERT(b->refs > 0);
    b->refs--;
}

static void note_dirty(struct cfs *fs);

static void buf_mark_dirty(struct cfs *fs, struct cfs_buf *b)
{
    if (!b->dirty) {
        b->dirty = true;
        fs->nr_dirty++;
    }
    note_dirty(fs);
}

/*
 * The inverse, for a rollback. A buffer whose block is being given back
 * must not stay dirty: the commit that eventually succeeds writes every
 * dirty buffer, and by then that block may belong to something else --
 * two owners writing one block, which is a corruption rather than a
 * leak.
 */
static void buf_mark_clean(struct cfs *fs, struct cfs_buf *b)
{
    if (b->dirty) {
        b->dirty = false;
        KASSERT(fs->nr_dirty > 0);
        fs->nr_dirty--;
    }
}

void cfs_buf_mark_dirty(struct cfs *fs, struct cfs_buf *b)
{
    buf_mark_dirty(fs, b);
}

int cfs_buf_new(struct cfs *fs, uint32_t kind, struct cfs_buf **out)
{
    uint64_t blk;
    int rc = cfs_alloc_block(fs, &blk);
    if (rc)
        return rc;
    struct cfs_buf *b = buf_alloc(fs, blk);
    if (b == NULL) {
        cfs_free_block_deferred(fs, blk);
        return -ENOMEM;
    }
    memset(b->data, 0, CFS_BLOCK);
    mhdr_seal(fs, b->data, kind, blk);
    buf_mark_dirty(fs, b);
    *out = b;
    return 0;
}

/*
 * `exempt` says how the superseded block is given back: deferred, the
 * way every tree block is, or exempt -- freed for the new root with no
 * snapshot filter. Exempt is for a block no snapshot's tree can reach,
 * which is every block hanging off a superblock field a snapshot does
 * not copy: the free record, and from this unit the snapshot list and
 * its deadlists. Sending one of those through the filter would have the
 * snapshot hold it and append it to the deadlist it is a copy of
 * (docs/audit/next-subsystem-snap-deadlist.md, "The exemption, and the
 * rule it rests on").
 */
uint64_t cfs_res_take(struct cfs_res *r)
{
    return (r != NULL && r->used < r->n) ? r->blk[r->used++] : 0;
}

void cfs_res_untake(struct cfs_res *r)
{
    if (r != NULL && r->used > 0)
        r->used--;
}

static int buf_cow(struct cfs *fs, struct cfs_buf **bp, uint64_t *parent_slot, bool exempt, struct cfs_res *res)
{
    struct cfs_buf *b = *bp;
    struct cfs_mhdr *h = cfs_buf_hdr(b);
    if (h->generation == fs->gen) {
        buf_mark_dirty(fs, b);
        return 0;
    }
    uint64_t nblk;
    if (res != NULL) {
        /* Inside the commit's window, where allocating is the defect
         * this unit removes: the block comes from what was set aside
         * before the fixpoint. Nothing left means the bound was wrong,
         * which fails the commit rather than quietly allocating. */
        nblk = cfs_res_take(res);
        if (nblk == 0) {
            kerror("cosmofs: the commit's reservation is short; refusing to allocate after the bitmap");
            return -ENOSPC;
        }
    } else {
        /* The copy stays on the member the block was on where that
         * member has room. Otherwise a metadata tree drifts onto
         * whichever member is emptiest, and a member's own allocation
         * index could end up on a different device than the blocks it
         * describes. */
        unsigned v = CFS_DVA_VDEV(b->blkno);
        uint64_t hint = v < fs->nmembers ? CFS_DVA(v, fs->mem[v].first_usable) : 0;
        uint64_t got;
        int rc = cfs_alloc_run(fs, CFS_ALLOC_META, hint, 1, &nblk, &got);
        if (rc)
            return rc;
    }
    struct cfs_buf *nb = buf_alloc(fs, nblk);
    if (nb == NULL) {
        if (res != NULL)
            cfs_res_untake(res);
        else
            cfs_free_block_deferred(fs, nblk);
        return -ENOMEM;
    }
    memcpy(nb->data, b->data, CFS_BLOCK);
    mhdr_seal(fs, nb->data, h->kind, nblk);
    buf_mark_dirty(fs, nb);
    if (exempt)
        cfs_free_block_exempt(fs, b->blkno);
    else
        cfs_free_block_deferred(fs, b->blkno);
    cfs_buf_put(fs, b);
    *parent_slot = nblk;
    *bp = nb;
    return 0;
}

int cfs_buf_cow(struct cfs *fs, struct cfs_buf **bp, uint64_t *parent_slot)
{
    return buf_cow(fs, bp, parent_slot, false, NULL);
}

int cfs_buf_cow_exempt(struct cfs *fs, struct cfs_buf **bp, uint64_t *parent_slot, struct cfs_res *res)
{
    return buf_cow(fs, bp, parent_slot, true, res);
}

int cfs_buf_new_at(struct cfs *fs, uint32_t kind, uint64_t blk, struct cfs_buf **out)
{
    /* Through buf_alloc, not cfs_buf_get: a block just handed out may
     * have a stale buffer from its previous life further down the list.
     * The same reason cfs_buf_cow and freelog_fill do it this way. */
    struct cfs_buf *b = buf_alloc(fs, blk);
    if (b == NULL)
        return -ENOMEM;
    memset(b->data, 0, CFS_BLOCK);
    mhdr_seal(fs, b->data, kind, blk);
    buf_mark_dirty(fs, b);
    *out = b;
    return 0;
}

/* --- allocation ------------------------------------------------------------ */

bool cfs_bitmap_test(const struct cfs *fs, uint64_t lin)
{
    return lin < fs->nblocks && (fs->bitmap[lin / 8] & (1u << (lin % 8))) != 0;
}

static inline bool bit_test(const uint8_t *map, uint64_t i) { return (map[i >> 3] >> (i & 7)) & 1; }
static inline void bit_set(uint8_t *map, uint64_t i) { map[i >> 3] |= (uint8_t)(1u << (i & 7)); }
static inline void bit_clear(uint8_t *map, uint64_t i) { map[i >> 3] &= (uint8_t)~(1u << (i & 7)); }

static void cfs_writeback_thread(void *arg);

/* The open transaction just became (or stayed) non-empty. The writeback
 * thread starts here, on the first change, so a mount that only reads
 * (the replay harness mounts hundreds of prefix images) never has one to
 * join at unmount. */
static void note_dirty(struct cfs *fs)
{
    if (fs->first_dirty_ns == 0)
        fs->first_dirty_ns = clock_now_ns();
    if (fs->wb_thread == NULL && fs->wb_enabled && !fs->wb_stop && fs->mnt) {
        fs->wb_thread = thread_create(cfs_writeback_thread, fs, "cfs-wb", SCHED_PRIO_DEFAULT);
        if (fs->wb_thread == NULL) {
            fs->wb_enabled = false;
            kwarn("cosmofs: no writeback thread; commits happen on sync, fsync and unmount only");
        }
    }
}

/* The first free run in blocks [lo, hi) of member `v`, claimed. `hi` is
 * never past the member's last block, so the padding at the end of its
 * last bitmap chunk is unreachable. */
static bool scan_member(struct cfs *fs, unsigned v, uint64_t lo, uint64_t hi, uint32_t want, uint64_t *start,
                        uint64_t *got)
{
    struct cfs_memstate *m = &fs->mem[v];
    for (uint64_t b = lo; b < hi; b++) {
        if (bit_test(fs->bitmap, m->base + b))
            continue;
        uint64_t len = 1;
        while (len < want && b + len < hi && !bit_test(fs->bitmap, m->base + b + len))
            len++;
        for (uint64_t k = 0; k < len; k++) {
            bit_set(fs->bitmap, m->base + b + k);
            fs->bitmap_dirty[m->chunk0 + (b + k) / CFS_BITS_PER_BITMAP] = 1;
        }
        fs->free_blocks -= len;
        m->free_blocks -= len;
        m->alloc_hint = b + len;
        fs->alloc_hint = m->base + b + len;
        note_dirty(fs);
        *start = CFS_DVA(v, b);
        *got = len;
        return true;
    }
    return false;
}

/* Where a run with no hint should come from: the member with the most
 * room, so a pool fills evenly rather than filling member 0 first. */
static unsigned pick_member(const struct cfs *fs)
{
    unsigned best = 0;
    for (unsigned v = 1; v < fs->nmembers; v++)
        if (fs->mem[v].free_blocks > fs->mem[best].free_blocks)
            best = v;
    return best;
}

/* First fit from `hint` (or the running hint), taking up to `want`
 * consecutive free blocks. Data allocations stop at the reserve so
 * deletion and commit always have metadata blocks (design.md,
 * "Allocation: contiguity and the metadata reserve"). A run never
 * crosses a member: an extent's `count` counts blocks on one device
 * (design.md, "One bitmap, many members"). */
int cfs_alloc_run(struct cfs *fs, enum cfs_alloc_class cls, uint64_t hint, uint32_t want, uint64_t *start,
                  uint64_t *got)
{
    uint64_t usable = cls == CFS_ALLOC_DATA ? (fs->free_blocks > fs->reserve ? fs->free_blocks - fs->reserve : 0)
                                            : fs->free_blocks;
    if (usable == 0)
        return -ENOSPC;
    if (want == 0)
        want = 1;
    if (want > usable)
        want = (uint32_t)usable;
    bool hinted = cfs_dva_valid(fs, hint);
    unsigned first = hinted ? CFS_DVA_VDEV(hint) : pick_member(fs);
    for (unsigned n = 0; n < fs->nmembers; n++) {
        unsigned v = (first + n) % fs->nmembers;
        struct cfs_memstate *m = &fs->mem[v];
        uint64_t from = (n == 0 && hinted) ? CFS_DVA_BLK(hint) : m->alloc_hint;
        if (from < m->first_usable || from >= m->nblocks)
            from = m->first_usable;
        if (scan_member(fs, v, from, m->nblocks, want, start, got))
            return 0;
        if (scan_member(fs, v, m->first_usable, from, want, start, got))
            return 0;
    }
    return -ENOSPC;
}

int cfs_alloc_block(struct cfs *fs, uint64_t *out)
{
    uint64_t got;
    return cfs_alloc_run(fs, CFS_ALLOC_META, 0, 1, out, &got);
}

int cfs_alloc_data(struct cfs *fs, uint64_t hint, uint32_t want, uint64_t *start, uint64_t *got)
{
    return cfs_alloc_run(fs, CFS_ALLOC_DATA, hint, want, start, got);
}

/*
 * Free a block this transaction owns outright: a superseded free
 * record. It waits for the root like any deferred free -- the current
 * root still names it, so handing it to the allocator now would let a
 * crash leave a root naming an overwritten record -- but it skips the
 * snapshot filter phase 7 applies, and the reason is in `struct cfs`
 * beside `pending_exempt`.
 */
void cfs_free_block_exempt(struct cfs *fs, uint64_t blk)
{
    if (!cfs_dva_valid(fs, blk))
        return;
    if (fs->nr_exempt == fs->exempt_cap) {
        unsigned cap = fs->exempt_cap ? fs->exempt_cap * 2 : 16;
        uint64_t *n = krealloc(fs->pending_exempt, cap * sizeof(*n), 0);
        if (n == NULL) {
            kerror("cosmofs: leaking block %llu (no memory for the free list)", (unsigned long long)blk);
            return;
        }
        fs->pending_exempt = n;
        fs->exempt_cap = cap;
    }
    fs->pending_exempt[fs->nr_exempt++] = blk;
    note_dirty(fs);
}

bool cfs_orphan_named(const struct cfs *fs, uint64_t ino)
{
    for (unsigned i = 0; i < fs->nr_orphans; i++)
        if (fs->orphans[i] == ino)
            return true;
    return false;
}

/*
 * Room for one more, taken *before* the caller's point of no return.
 *
 * The first version grew the array inside cfs_orphan_add and, when that
 * failed, logged and returned while the unlink went on to commit a
 * zero-link inode with nothing recording it -- which is this unit's own
 * defect, reintroduced on an allocation failure. An unlink that cannot
 * be recorded must not happen, so the memory is taken while the name is
 * still there and -ENOMEM is still a clean answer.
 */
bool cfs_orphan_reserve(struct cfs *fs)
{
    if (fs->nr_orphans < fs->orphan_cap)
        return true;
    unsigned cap = fs->orphan_cap ? fs->orphan_cap * 2 : 8;
    uint64_t *n = krealloc(fs->orphans, cap * sizeof(*n), 0);
    if (n == NULL)
        return false;
    fs->orphans = n;
    fs->orphan_cap = cap;
    return true;
}

/* Cannot fail: the caller reserved the room. */
void cfs_orphan_add(struct cfs *fs, uint64_t ino)
{
    if (ino == 0 || cfs_orphan_named(fs, ino))
        return;
    if (fs->nr_orphans == fs->orphan_cap) {
        kerror("cosmofs: orphan record has no room for inode %llu; it was not reserved",
               (unsigned long long)ino);
        cfs_fail(fs, -ENOSPC);   /* rather than commit a deletion nothing records */
        return;
    }
    fs->orphans[fs->nr_orphans++] = ino;
    note_dirty(fs);
}

void cfs_orphan_remove(struct cfs *fs, uint64_t ino)
{
    for (unsigned i = 0; i < fs->nr_orphans; i++) {
        if (fs->orphans[i] != ino)
            continue;
        fs->orphans[i] = fs->orphans[fs->nr_orphans - 1];
        fs->nr_orphans--;
        return;
    }
}

void cfs_free_block_deferred(struct cfs *fs, uint64_t blk)
{
    if (!cfs_dva_valid(fs, blk))
        return;
    if (fs->nr_pending == fs->pending_cap) {
        unsigned cap = fs->pending_cap ? fs->pending_cap * 2 : 64;
        uint64_t *n = krealloc(fs->pending_free, cap * sizeof(*n), 0);
        if (n == NULL) {
            kerror("cosmofs: leaking block %llu (no memory for the free list)", (unsigned long long)blk);
            return;
        }
        fs->pending_free = n;
        fs->pending_cap = cap;
    }
    fs->pending_free[fs->nr_pending++] = blk;
    note_dirty(fs);
}

/* --- inode map ----------------------------------------------------------- */

static uint64_t *ptrs(struct cfs_buf *b)
{
    return (uint64_t *)(b->data + CFS_MHDR_SIZE);
}

/* Fetch (and when `writable`, CoW) the L1, L0 and inode block for `ino`.
 * Missing L0/inode blocks are created when `create` is set. */
/* `imap_root` is the live tree's, except for a read of a snapshot's
 * inode, which passes that snapshot's root instead: its trees are
 * ordinary trees, so nothing else in the walk changes
 * (design.md, "Reading a snapshot"). */
static int inode_block_at(struct cfs *fs, uint64_t imap_root, uint64_t ino, bool writable, bool create,
                          struct cfs_buf **out)
{
    if (ino == 0 || ino >= CFS_MAX_INODES)
        return -EIO;
    struct cfs_buf *l1, *l0, *ib;
    int rc = cfs_buf_get(fs, imap_root, CFS_KIND_IMAP1, &l1);
    if (rc)
        return rc;
    if (writable && (rc = cfs_buf_cow(fs, &l1, &fs->sb.imap_root)) != 0) {
        cfs_buf_put(fs, l1);
        return rc;
    }
    uint64_t *l1p = &ptrs(l1)[cfs_imap_l1_index(ino)];
    if (*l1p == 0) {
        if (!create) {
            cfs_buf_put(fs, l1);
            return -ENOENT;
        }
        rc = cfs_buf_new(fs, CFS_KIND_IMAP0, &l0);
        if (rc) {
            cfs_buf_put(fs, l1);
            return rc;
        }
        *l1p = l0->blkno;
    } else {
        rc = cfs_buf_get(fs, *l1p, CFS_KIND_IMAP0, &l0);
        if (rc) {
            cfs_buf_put(fs, l1);
            return rc;
        }
        if (writable && (rc = cfs_buf_cow(fs, &l0, l1p)) != 0) {
            cfs_buf_put(fs, l0);
            cfs_buf_put(fs, l1);
            return rc;
        }
    }
    uint64_t *l0p = &ptrs(l0)[cfs_imap_l0_index(ino)];
    if (*l0p == 0) {
        if (!create) {
            rc = -ENOENT;
            goto out;
        }
        rc = cfs_buf_new(fs, CFS_KIND_INODES, &ib);
        if (rc)
            goto out;
        *l0p = ib->blkno;
    } else {
        rc = cfs_buf_get(fs, *l0p, CFS_KIND_INODES, &ib);
        if (rc)
            goto out;
        if (writable && (rc = cfs_buf_cow(fs, &ib, l0p)) != 0) {
            cfs_buf_put(fs, ib);
            goto out;
        }
    }
    *out = ib;
out:
    cfs_buf_put(fs, l0);
    cfs_buf_put(fs, l1);
    return rc;
}

static struct cfs_inode *inode_slot(struct cfs_buf *ib, uint64_t ino)
{
    return (struct cfs_inode *)(ib->data + CFS_MHDR_SIZE + cfs_inode_slot(ino) * CFS_INODE_SIZE);
}

static int inode_block(struct cfs *fs, uint64_t ino, bool writable, bool create, struct cfs_buf **out)
{
    return inode_block_at(fs, fs->sb.imap_root, ino, writable, create, out);
}

int cfs_inode_read_at(struct cfs *fs, uint64_t imap_root, uint64_t next_ino, uint64_t ino,
                      struct cfs_inode *out)
{
    if (ino == 0 || ino >= next_ino)
        return -ENOENT;
    struct cfs_buf *ib;
    int rc = inode_block_at(fs, imap_root, ino, false, false, &ib);
    if (rc)
        return rc;
    memcpy(out, inode_slot(ib, ino), sizeof(*out));
    cfs_buf_put(fs, ib);
    if (out->ino != ino || out->nlink == 0)
        return -ENOENT;
    return 0;
}

/* The slot as it is, links or no links. The ordinary read reports an
 * inode with no links as absent, which is right for a lookup and wrong
 * for the structural check: an inode with no links and blocks still
 * allocated is exactly what it is looking for. */
int cfs_inode_read_raw(struct cfs *fs, uint64_t ino, struct cfs_inode *out)
{
    /* The same range the ordinary read accepts: numbers 1 to next_ino-1
     * have been handed out, and the two readers must agree about which
     * numbers exist or the check would ask about a slot no lookup can. */
    if (ino == 0 || ino >= fs->sb.next_ino)
        return -ENOENT;
    struct cfs_buf *ib;
    int rc = inode_block(fs, ino, false, false, &ib);
    if (rc)
        return rc;
    memcpy(out, inode_slot(ib, ino), sizeof(*out));
    cfs_buf_put(fs, ib);
    return out->ino == ino ? 0 : -ENOENT;
}

int cfs_inode_read(struct cfs *fs, uint64_t ino, struct cfs_inode *out)
{
    if (ino == 0 || ino >= fs->sb.next_ino)
        return -ENOENT;
    struct cfs_buf *ib;
    int rc = inode_block(fs, ino, false, false, &ib);
    if (rc)
        return rc;
    memcpy(out, inode_slot(ib, ino), sizeof(*out));
    cfs_buf_put(fs, ib);
    if (out->ino != ino || out->nlink == 0)
        return -ENOENT;   /* free slot */
    return 0;
}

int cfs_inode_write(struct cfs *fs, uint64_t ino, const struct cfs_inode *in)
{
    struct cfs_buf *ib;
    int rc = inode_block(fs, ino, true, true, &ib);
    if (rc)
        return rc;
    struct cfs_inode *slot = inode_slot(ib, ino);
    memcpy(slot, in, sizeof(*slot));
    slot->generation = fs->gen;
    cfs_buf_put(fs, ib);
    return 0;
}

int cfs_inode_alloc(struct cfs *fs, uint64_t *ino)
{
    if (fs->sb.next_ino >= CFS_MAX_INODES)
        return -ENOSPC;
    *ino = fs->sb.next_ino++;
    fs->sb.inode_count++;
    return 0;
}

/*
 * Give back an inode number a failed creation took. The allocator is a
 * bump allocator and the caller holds fs->lock, so the number is still
 * the last one handed out and rolling back is exact; a number that is
 * not (a later allocation intervened, which cannot happen under the
 * lock) is simply left, and the slot stays free because nothing ever
 * wrote an inode with a nonzero nlink there.
 */
void cfs_inode_discard(struct cfs *fs, uint64_t ino)
{
    if (ino != 0 && ino + 1 == fs->sb.next_ino)
        fs->sb.next_ino--;
    if (fs->sb.inode_count > 0)
        fs->sb.inode_count--;
}

/* --- commit ----------------------------------------------------------------- */

/* Write the in-memory bitmap chunks that changed, copy-on-write, until
 * no allocation made by the writing itself leaves a chunk dirty. */
/* One member's dirty bitmap chunks, into fresh blocks on that member.
 * Its index is CoW'd like any other metadata block; the parent pointer
 * is the member's own alloc_root, which is why a member's allocation
 * metadata never lives on another member. */
static int commit_member_bitmap(struct cfs *fs, unsigned v, uint64_t *reserved)
{
    struct cfs_memstate *m = &fs->mem[v];
    struct cfs_buf *idx;
    int rc = cfs_buf_get(fs, m->alloc_root, CFS_KIND_ALLOCIDX, &idx);
    if (rc)
        return rc;
    rc = cfs_buf_cow(fs, &idx, &m->alloc_root);
    if (rc)
        goto out;
    uint64_t *slots = ptrs(idx);
    for (unsigned c = 0; c < m->nchunks; c++) {
        unsigned gc = m->chunk0 + c;
        if (!fs->bitmap_dirty[gc])
            continue;
        if (reserved[gc] == 0) {
            /* A dirty chunk the reservation pass did not see. Writing it
             * to DVA 0 would put a bitmap on member 0's superblock, so
             * the transaction fails instead. */
            kerror("cosmofs: bitmap chunk %u has no reserved block", gc);
            rc = -EIO;
            goto out;
        }
        struct cfs_buf *nb = buf_alloc(fs, reserved[gc]);
        if (nb == NULL) {
            rc = -ENOMEM;
            goto out;
        }
        size_t bytes = CFS_BITS_PER_BITMAP / 8;
        size_t off = (size_t)gc * bytes;
        size_t total = (fs->nblocks + 7) / 8;
        size_t n = off + bytes <= total ? bytes : total - off;
        memset(nb->data, 0, CFS_BLOCK);
        memcpy(nb->data + CFS_MHDR_SIZE, fs->bitmap + off, n);
        mhdr_seal(fs, nb->data, CFS_KIND_BITMAP, reserved[gc]);
        buf_mark_dirty(fs, nb);
        cfs_buf_put(fs, nb);
        if (slots[c])
            cfs_free_block_deferred(fs, slots[c]);
        slots[c] = reserved[gc];
        fs->bitmap_dirty[gc] = 0;
    }
    /* Re-seal the index (it was CoW'd or dirtied above). */
    mhdr_seal(fs, idx->data, CFS_KIND_ALLOCIDX, idx->blkno);
out:
    cfs_buf_put(fs, idx);
    return rc;
}

static int commit_bitmap(struct cfs *fs)
{
    uint64_t *reserved = kzalloc(fs->nr_chunks * sizeof(*reserved));
    if (reserved == NULL)
        return -ENOMEM;

    /* Reserve a destination block for every dirty chunk before writing
     * any of them; reserving may dirty further chunks, so iterate to a
     * fixpoint. Each chunk's replacement comes from its own member, so
     * that a member's bitmap stays on the member it describes. */
    /* The member table is copy-on-written first, so the block that
     * takes is already counted in the bitmaps written below. Its
     * contents are filled in again at the end, once every member's new
     * allocation root is known; by then the block is writable in this
     * generation and the second call allocates nothing. */
    int rc = cfs_members_store(fs);

    /* Every allocation index this commit will write is copied first, for
     * the same reason: a copy takes a block, and that block has to be
     * counted in the bitmaps this commit writes. Doing it inside the
     * write loop instead dirties a chunk the reservation pass has
     * already been over -- and on a pool of several members the block
     * comes from whichever member has the most room, so the chunk it
     * dirties need not even belong to the member being written. */
    for (unsigned v = 0; v < fs->nmembers && rc == 0; v++) {
        struct cfs_buf *idx;
        rc = cfs_buf_get(fs, fs->mem[v].alloc_root, CFS_KIND_ALLOCIDX, &idx);
        if (rc == 0) {
            rc = cfs_buf_cow(fs, &idx, &fs->mem[v].alloc_root);
            cfs_buf_put(fs, idx);
        }
    }
    for (bool progress = true; progress && rc == 0;) {
        progress = false;
        for (unsigned v = 0; v < fs->nmembers && rc == 0; v++) {
            struct cfs_memstate *m = &fs->mem[v];
            for (unsigned c = 0; c < m->nchunks; c++) {
                unsigned gc = m->chunk0 + c;
                if (!fs->bitmap_dirty[gc] || reserved[gc] != 0)
                    continue;
                uint64_t got;
                rc = cfs_alloc_run(fs, CFS_ALLOC_META, CFS_DVA(v, m->first_usable), 1, &reserved[gc], &got);
                if (rc)
                    break;
                progress = true;
            }
        }
    }
    for (unsigned v = 0; v < fs->nmembers && rc == 0; v++)
        rc = commit_member_bitmap(fs, v, reserved);
    if (rc == 0)
        rc = cfs_members_store(fs);
    kfree(reserved);
    return rc;
}

/* The root write: flushed before (every block of the transaction is
 * stable first) and durable when it completes (BIO_PREFLUSH | BIO_FUA). */
static int super_write(struct cfs *fs, unsigned slot, unsigned flags)
{
    uint8_t *block = kmalloc(CFS_BLOCK, KMEM_ZERO);
    if (block == NULL)
        return -ENOMEM;
    struct cfs_super *sb = (struct cfs_super *)block;
    *sb = fs->sb;
    sb->crc = 0;
    sb->crc = block_crc(block, offsetof(struct cfs_super, crc));
    int rc = pool_write_flags(fs->pool, slot, block, flags);
    kfree(block);
    return rc;
}

void cfs_fail(struct cfs *fs, int rc)
{
    if (fs->failed == 0) {
        fs->failed = rc ? rc : -EIO;
        kerror("cosmofs: transaction abandoned after error %d; the last committed root stays current", rc);
    }
}

/* --- the record of what a transaction freed ------------------------------
 *
 * docs/audit/next-subsystem-unmount-leak.md. A commit clears the freed
 * blocks' bits only after its root is durable, and marks those bitmap
 * chunks for the next commit -- which at an unmount never comes. The
 * record is what carries them across: written before the root, made
 * true by it, and replayed by a mount that finds one.
 */

/* Entries a FREELOG block holds: the deadlist's shape, reused rather
 * than twinned. */
#define CFS_FREELOG_PER_BLOCK CFS_DEAD_PER_BLOCK

/* No record is this long in a sound filesystem; one that is has a cycle,
 * and the walk says so rather than following it forever. */
#define CFS_FREELOG_MAX_CHAIN 4096u

/*
 * Apply the record at mount: the blocks this root freed, whose bits the
 * last commit cleared only in memory.
 *
 * Read before the bitmap is trusted for allocation, so the space is
 * available in this session rather than after another mount. A block
 * named here is free by the root's own word, so a bit that is already
 * clear is not an error -- it means a commit got there first.
 */
static int freelog_replay(struct cfs *fs)
{
    if (fs->sb.version < 9)
        return 0;
    uint64_t at = fs->sb.free_root;
    unsigned guard = 0, applied = 0;
    while (at != 0) {
        if (guard++ > CFS_FREELOG_MAX_CHAIN) {
            kerror("cosmofs: free record chain too long at %llu", (unsigned long long)at);
            return -EIO;
        }
        struct cfs_buf *b;
        int rc = cfs_buf_get(fs, at, CFS_KIND_FREELOG, &b);
        if (rc) {
            /*
             * The root says these blocks are free and will not say which.
             * Mounting anyway would leave an allocator that cannot be
             * trusted, so the mount fails and the operator has a checker.
             */
            kerror("cosmofs: free record at %llu unreadable (%d); refusing the mount",
                   (unsigned long long)at, rc);
            return -EIO;
        }
        const struct cfs_dead_block *d = (const struct cfs_dead_block *)(b->data + CFS_MHDR_SIZE);
        uint64_t next = d->next;
        uint64_t count = d->count;
        if (count > CFS_FREELOG_PER_BLOCK) {
            /*
             * The block's checksum is good and its contents are not: a
             * count past the payload is a record this kernel cannot
             * read, which is the unreadable case and gets the same
             * answer. Treating it as empty would silently drop every
             * free it names and report them later as ordinary leaks.
             */
            kerror("cosmofs: free record at %llu names %llu blocks, more than the %u a block holds; "
                   "refusing the mount",
                   (unsigned long long)at, (unsigned long long)count,
                   (unsigned)CFS_FREELOG_PER_BLOCK);
            cfs_buf_put(fs, b);
            return -EIO;
        }
        for (uint64_t i = 0; i < count; i++) {
            uint64_t lin = cfs_dva_lin(fs, d->blk[i]);
            if (lin == CFS_DVA_NONE || !bit_test(fs->bitmap, lin))
                continue;                      /* a commit already applied it */
            bit_clear(fs->bitmap, lin);
            fs->bitmap_dirty[lin / CFS_BITS_PER_BITMAP] = 1;
            fs->free_blocks++;
            fs->mem[CFS_DVA_VDEV(d->blk[i])].free_blocks++;
            applied++;
        }
        cfs_buf_put(fs, b);
        at = next;
    }
    if (applied)
        kinfo("cosmofs: reclaimed %u block(s) the last commit freed and could not write",
              applied);
    return 0;
}

/*
 * Release the chain the current root names. It is the *old* root's
 * statement and the new root replaces it, so without this every commit
 * leaks its predecessor's record -- this unit's own defect, one level up.
 *
 * Deliberately not through the snapshot filter. That asks whether a
 * snapshot's recorded bitmap marks the block allocated, and these blocks
 * were allocated when an older snapshot was taken, so the generic path
 * would hold them and append them to a deadlist. No snapshot can reach a
 * record: a snapshot preserves imap_root and alloc_root, not free_root.
 * The rule the exception rests on: the snapshot filter is for blocks a
 * snapshot's tree might name, and a block reachable only from a
 * superblock field that snapshots do not copy is not one of those.
 */
static int freelog_release_previous(struct cfs *fs)
{
    uint64_t at = fs->sb.version >= 9 ? fs->sb.free_root : 0;
    unsigned guard = 0;
    while (at != 0) {
        if (guard++ > CFS_FREELOG_MAX_CHAIN) {
            kerror("cosmofs: free record chain too long at %llu", (unsigned long long)at);
            return -EIO;
        }
        struct cfs_buf *b;
        int rc = cfs_buf_get(fs, at, CFS_KIND_FREELOG, &b);
        if (rc)
            return rc;
        uint64_t next = ((const struct cfs_dead_block *)(b->data + CFS_MHDR_SIZE))->next;
        cfs_buf_put(fs, b);
        cfs_free_block_exempt(fs, at);     /* freed for the new root, and no snapshot's to hold */
        at = next;
    }
    fs->sb.free_root = 0;
    return 0;
}

/*
 * Allocate the record's blocks, without filling them. The bound is
 * computable here and the fill is not: commit_bitmap frees every chunk
 * and index it copies, so the final set is only known after it runs --
 * and it must run after every allocation, or the bitmap it writes does
 * not know about these blocks.
 *
 * The bound is what is pending now, plus what commit_bitmap can add: one
 * block per chunk it may rewrite and one index per member. Generous by
 * design; freelog_fill puts the leftovers in the record itself.
 */
/*
 * The commit's reservation: blocks taken before the bitmap fixpoint and
 * handed out after it.
 *
 * Two consumers, and the second is optional. The deadlist fill takes
 * from the front and needs its blocks on **every** filesystem that has a
 * snapshot, whatever the format version; the record takes what is left
 * and exists only from version 9. So `with_record` widens the
 * reservation rather than gating it: a version-8 filesystem with a
 * snapshot still reserves the deadlist's share, because the alternative
 * is a release loop that frees a snapshot's blocks.
 */
/* Defined with the rest of the orphan record below; needed here because
 * the reservation is where its blocks are counted. */
static unsigned orphan_reserve_bound(const struct cfs *fs);

static int commit_reserve(struct cfs *fs, struct cfs_res *res, bool with_record)
{
    res->blk = NULL;
    res->n = 0;
    res->used = 0;
    /*
     * What the record must be able to name, and what the snapshot list
     * may add to it: the blocks its copies supersede are freed by this
     * root like any other, so they are entries here too.
     */
    unsigned pending_bound = (unsigned)(fs->nr_pending + fs->nr_chunks + fs->nmembers);
    uint64_t bound = with_record
                         ? (uint64_t)pending_bound + fs->nr_exempt + cfs_snapshot_exempt_bound(fs)
                         : 0;
    /*
     * And the blocks the snapshot list's own work needs, which are
     * blocks rather than entries: the chain's copy, a copied deadlist
     * head and a fresh deadlist block per CFS_DEAD_PER_BLOCK holds. One
     * reservation, two consumers -- the deadlist fill takes from the
     * front and the record gets what is left, so a block nobody needed
     * is a leftover the record names as free, which is the mechanism
     * that already keeps the record's own bound from having to be tight.
     */
    unsigned snap_extra = cfs_snapshot_reserve_bound(fs, pending_bound);
    /* And the orphan record's, which is exact rather than generous:
     * nr_orphans is what it is before the fixpoint and commit_bitmap
     * cannot add to it. */
    unsigned orphan_extra = orphan_reserve_bound(fs);
    if (bound == 0 && snap_extra == 0 && orphan_extra == 0)
        return 0;
    unsigned n = (unsigned)((bound + CFS_FREELOG_PER_BLOCK - 1) / CFS_FREELOG_PER_BLOCK) + snap_extra + orphan_extra;
    if (n == 0)
        return 0;
    uint64_t *blk = kmalloc((size_t)n * sizeof(*blk), KMEM_ZERO);
    if (blk == NULL)
        return -ENOMEM;
    for (unsigned i = 0; i < n; i++) {
        uint64_t got = 0;
        int rc = cfs_alloc_run(fs, CFS_ALLOC_META, 0, 1, &blk[i], &got);
        if (rc) {
            /* Give back what was taken: a failed commit leaves nothing. */
            for (unsigned k = 0; k < i; k++)
                cfs_free_block_deferred(fs, blk[k]);
            kfree(blk);
            return rc;
        }
    }
    res->blk = blk;
    res->n = n;
    return 0;
}

/*
 * Fill the reserved blocks with what is now final, and chain them. A
 * block the bound over-reserved is listed in the record as free: a block
 * that describes its own release, which is what keeps the bound from
 * having to be tight.
 */
static int freelog_fill(struct cfs *fs, uint64_t *blk, unsigned n, const bool *held, unsigned held_n)
{
    if (n == 0) {
        /* Nothing reserved for the record. That is right when there is
         * nothing to record and a silent unmount leak when there is, so
         * the second case stops the commit rather than writing no
         * record and reporting success. */
        if (fs->nr_pending != 0 || fs->nr_exempt != 0) {
            kerror("cosmofs: no reservation left for the free record (%u pending, %u exempt)", fs->nr_pending,
                   fs->nr_exempt);
            return -EIO;
        }
        fs->sb.free_root = 0;
        return 0;
    }
    if (fs->test_fail_freelog)
        return -EIO;   /* test hook: fail with the reservation outstanding */
    unsigned total = fs->nr_pending + fs->nr_exempt;
    /*
     * Room for the entries *and* for the leftovers, which are entries
     * too. `total / CFS_FREELOG_PER_BLOCK` is not enough: when the last
     * block comes out exactly full the leftovers have nowhere to go, and
     * dropping them there strands a block on every such commit, which a
     * clean unmount then loses for good.
     *
     * Each block used is one fewer leftover, so the requirement is
     * `need * PER_BLOCK >= total + (n - need)`, that is
     * `need >= (total + n) / (PER_BLOCK + 1)`, rounded up.
     */
    unsigned need = (total + n + CFS_FREELOG_PER_BLOCK) / (CFS_FREELOG_PER_BLOCK + 1);
    if (need == 0)
        need = 1;                            /* the leftovers still need somewhere to be said */
    if (need > n)
        need = n;                            /* then there are no leftovers, and n blocks hold `bound` */

    /*
     * Through the buffer cache, not pool_write: a block just allocated
     * may have a stale buffer from its previous life further down the
     * list, and cfs_buf_get would find that one. buf_alloc puts a fresh
     * one at the front, which is what cfs_buf_cow does for the same
     * reason, and the commit's dirty loop writes it.
     */
    unsigned at = 0;                         /* entries of pending_free written so far */
    unsigned x = need;                       /* leftovers recorded, once the last block is written */
    for (unsigned i = 0; i < need; i++) {
        struct cfs_buf *b = buf_alloc(fs, blk[i]);
        if (b == NULL)
            return -ENOMEM;   /* cfs_commit gives every reserved block back */
        struct cfs_dead_block *d = (struct cfs_dead_block *)(b->data + CFS_MHDR_SIZE);
        d->next = (i + 1 < need) ? blk[i + 1] : 0;
        unsigned k = 0;
        while (k < CFS_FREELOG_PER_BLOCK && at < total) {
            /* The exempt list after the pending one, as one sequence:
             * both are freed by this root and both must be in what it
             * says it freed, or an unmount loses them. */
            unsigned idx = at;
            bool exempt = idx >= fs->nr_pending;
            uint64_t dva = exempt ? fs->pending_exempt[idx - fs->nr_pending]
                                  : fs->pending_free[idx];
            at++;
            uint64_t lin = cfs_dva_lin(fs, dva);
            if (lin == CFS_DVA_NONE || !bit_test(fs->bitmap, lin))
                continue;
            /*
             * Exactly what the release loop will clear, and not asked
             * again: cfs_snapshot_fill took the verdict once, before
             * this, and recorded the held blocks on a deadlist. The
             * record names what is left. Recording a block a snapshot
             * keeps would tell the next mount to free it, and asking a
             * second time would be two verdicts where one will do.
             */
            if (!exempt && held != NULL && idx < held_n && held[idx])
                continue;
            d->blk[k++] = dva;
        }
        /* The reserved blocks nobody needed say so here, in the last
         * block, whose capacity `need` was chosen to leave room in. */
        if (i + 1 == need) {
            for (x = need; x < n && k < CFS_FREELOG_PER_BLOCK; x++)
                d->blk[k++] = blk[x];
            if (x < n) {
                /* The arithmetic above says this cannot happen. If it
                 * ever does, a block would be freed in memory and named
                 * by nothing, so refuse the commit instead. */
                kerror("cosmofs: free record has no room for %u leftover block(s)", n - x);
                buf_mark_clean(fs, b);
                cfs_buf_put(fs, b);
                return -EIO;
            }
        }
        d->count = k;
        mhdr_seal(fs, b->data, CFS_KIND_FREELOG, blk[i]);
        buf_mark_dirty(fs, b);
        cfs_buf_put(fs, b);
    }
    /*
     * The leftovers are named by the record, so they are free from the
     * moment this root lands -- and the in-memory bitmap must agree, or
     * the allocator will not hand them out until a remount.
     */
    for (unsigned y = need; y < n; y++)
        cfs_free_block_deferred(fs, blk[y]);
    fs->sb.free_root = blk[0];
    return 0;
}

/* --- the record of what this root still owes -----------------------------
 *
 * docs/audit/next-subsystem-orphan.md. An inode whose last name has gone
 * while something still references it has nlink 0 on disk, its extents
 * intact and no directory entry reaching it; the only thing that will
 * free it is a cfs_evict call that lives in this mount's memory. A crash
 * or a forced unmount before that loses the inode and its blocks for
 * good, because no mount reconsiders them.
 *
 * Unlike the snapshot list, this is not a structure a commit edits: the
 * set is derived from memory, so the record is written out whole each
 * time and the previous one released. Nothing is copied because nothing
 * is edited.
 */

/* No sound record is this long; one that is has a cycle. */
#define CFS_ORPHAN_MAX_CHAIN 4096u

static int orphan_release_previous(struct cfs *fs)
{
    uint64_t at = fs->sb.version >= 10 ? fs->sb.orphan_root : 0;
    unsigned guard = 0;
    while (at != 0) {
        if (guard++ > CFS_ORPHAN_MAX_CHAIN) {
            kerror("cosmofs: orphan record chain too long at %llu", (unsigned long long)at);
            return -EIO;
        }
        struct cfs_buf *b;
        int rc = cfs_buf_get(fs, at, CFS_KIND_ORPHAN, &b);
        if (rc)
            return rc;
        uint64_t next = ((const struct cfs_dead_block *)(b->data + CFS_MHDR_SIZE))->next;
        cfs_buf_put(fs, b);
        cfs_free_block_exempt(fs, at);   /* no snapshot's tree reaches a superblock record */
        at = next;
    }
    fs->sb.orphan_root = 0;
    return 0;
}

/*
 * Blocks the record needs. Unlike the free record's, this bound is
 * exact: nr_orphans is what it is before the fixpoint and commit_bitmap
 * cannot add to it.
 */
static unsigned orphan_reserve_bound(const struct cfs *fs)
{
    if (fs->sb.version < 10 || fs->nr_orphans == 0)
        return 0;
    return (fs->nr_orphans + CFS_ORPHANS_PER_BLOCK - 1) / CFS_ORPHANS_PER_BLOCK;
}

/* Write the set out whole, chained, from the reservation. */
static int orphan_fill(struct cfs *fs, struct cfs_res *res)
{
    fs->sb.orphan_root = 0;
    if (fs->sb.version < 10 || fs->nr_orphans == 0)
        return 0;
    if (fs->test_fail_orphan)
        return -EIO;   /* test hook: fail with the reservation outstanding */
    unsigned need = (fs->nr_orphans + CFS_ORPHANS_PER_BLOCK - 1) / CFS_ORPHANS_PER_BLOCK;
    uint64_t head = 0, prev = 0;
    unsigned at = 0;
    for (unsigned i = 0; i < need; i++) {
        uint64_t blk = cfs_res_take(res);
        if (blk == 0) {
            kerror("cosmofs: the commit's reservation is short of an orphan record block");
            return -ENOSPC;
        }
        struct cfs_buf *b;
        int rc = cfs_buf_new_at(fs, CFS_KIND_ORPHAN, blk, &b);
        if (rc) {
            cfs_res_untake(res);
            return rc;
        }
        struct cfs_dead_block *d = (struct cfs_dead_block *)(b->data + CFS_MHDR_SIZE);
        d->next = 0;
        unsigned k = 0;
        while (k < CFS_ORPHANS_PER_BLOCK && at < fs->nr_orphans)
            d->blk[k++] = fs->orphans[at++];
        d->count = k;
        mhdr_seal(fs, b->data, CFS_KIND_ORPHAN, blk);
        buf_mark_dirty(fs, b);
        cfs_buf_put(fs, b);
        /* Chain forward: the block before this one names it, and it is
         * still dirty and unwritten, so the edit reaches the disk. */
        if (prev != 0) {
            struct cfs_buf *pb = buf_find(fs, prev);
            if (pb == NULL) {
                kerror("cosmofs: the orphan record's previous block %llu went away", (unsigned long long)prev);
                return -EIO;
            }
            ((struct cfs_dead_block *)(pb->data + CFS_MHDR_SIZE))->next = blk;
        } else {
            head = blk;
        }
        prev = blk;
    }
    fs->sb.orphan_root = head;
    return 0;
}

/*
 * Apply the record at mount: do what cfs_evict would have done for every
 * inode the root still owed.
 *
 * An inode whose slot is already empty is skipped *before* the
 * inode_count decrement, not after it. That is what makes a repeated
 * replay harmless -- a mount that replays and goes away without
 * committing leaves the record standing, and the next mount reads it
 * again -- and it is the one place the superblock's count could be taken
 * twice off the same inode.
 */
static int orphan_replay(struct cfs *fs)
{
    if (fs->sb.version < 10)
        return 0;
    uint64_t at = fs->sb.orphan_root;
    unsigned guard = 0, applied = 0, gone = 0, suspect = 0;
    while (at != 0) {
        if (guard++ > CFS_ORPHAN_MAX_CHAIN) {
            kerror("cosmofs: orphan record chain too long at %llu", (unsigned long long)at);
            return -EIO;
        }
        struct cfs_buf *b;
        int rc = cfs_buf_get(fs, at, CFS_KIND_ORPHAN, &b);
        if (rc) {
            /*
             * The root says it owes these inodes and will not say which.
             * Mounting anyway would leave an inode map that cannot be
             * trusted, so the mount fails and the operator has a checker
             * -- the same answer the free record gives.
             */
            kerror("cosmofs: orphan record at %llu unreadable (%d); refusing the mount", (unsigned long long)at,
                   rc);
            return -EIO;
        }
        const struct cfs_dead_block *d = (const struct cfs_dead_block *)(b->data + CFS_MHDR_SIZE);
        uint64_t next = d->next;
        uint64_t count = d->count;
        if (count > CFS_ORPHANS_PER_BLOCK) {
            kerror("cosmofs: orphan record at %llu names %llu inodes, more than the %u a block holds; "
                   "refusing the mount",
                   (unsigned long long)at, (unsigned long long)count, (unsigned)CFS_ORPHANS_PER_BLOCK);
            cfs_buf_put(fs, b);
            return -EIO;
        }
        uint64_t *inos = count ? kmalloc((size_t)count * sizeof(*inos), 0) : NULL;
        if (count && inos == NULL) {
            cfs_buf_put(fs, b);
            return -ENOMEM;
        }
        for (uint64_t i = 0; i < count; i++)
            inos[i] = d->blk[i];
        cfs_buf_put(fs, b);
        for (uint64_t i = 0; i < count; i++) {
            struct cfs_inode in;
            /*
             * Raw, because every inode this record names has nlink 0 and
             * the ordinary read calls that absent -- the same reason the
             * structural check reads orphans this way.
             */
            if (cfs_inode_read_raw(fs, inos[i], &in) != 0) {
                /* One inode, not the map: report it and carry on, and
                 * the checker still finds it as an orphan. */
                kwarn("cosmofs: orphan record names inode %llu, which does not read", (unsigned long long)inos[i]);
                continue;
            }
            if (in.mode == 0 && in.size == 0 && in.ino == 0) {
                gone++;      /* a commit got there first: not an error */
                continue;
            }
            /*
             * The record says this inode has no name. Check rather than
             * believe: a record whose checksum is good and whose
             * contents are wrong would otherwise have this delete a file
             * somebody is still using, which is the worst thing in this
             * file. Skipped rather than refused, because skipping leaves
             * the filesystem exactly as it was and mountable, and the
             * checker still reports the inode -- while refusing the
             * mount would take the filesystem away from the operator to
             * protect it from a leak.
             */
            if (in.nlink != 0) {
                kerror("cosmofs: orphan record names inode %llu, which has %u link(s); refusing to reclaim it",
                       (unsigned long long)inos[i], (unsigned)in.nlink);
                suspect++;
                continue;
            }
            if (cfs_truncate_blocks(fs, &in, 0) != 0) {
                kwarn("cosmofs: orphan inode %llu could not be released", (unsigned long long)inos[i]);
                continue;   /* nothing changed for it: the checker still finds it */
            }
            /*
             * Past here the blocks are queued to be freed, so leaving
             * the slot pointing at them is not an option: the first
             * commit would publish the frees under an inode that still
             * names them. Nothing has been published yet, so failing the
             * mount discards the whole transaction and leaves the
             * filesystem as it was found.
             */
            struct cfs_inode empty;
            memset(&empty, 0, sizeof(empty));
            int wrc = cfs_inode_write(fs, inos[i], &empty);
            if (wrc != 0) {
                kerror("cosmofs: orphan inode %llu was released but its slot could not be cleared (%d); "
                       "refusing the mount",
                       (unsigned long long)inos[i], wrc);
                kfree(inos);
                return wrc;
            }
            if (fs->sb.inode_count > 0)
                fs->sb.inode_count--;
            applied++;
        }
        kfree(inos);
        at = next;
    }
    if (applied || gone)
        kinfo("cosmofs: reclaimed %u inode(s) unlinked with a handle open (%u already gone)", applied, gone);
    if (suspect)
        kerror("cosmofs: the orphan record named %u inode(s) that still have links; run the structural check",
               suspect);
    return 0;
}

int cfs_commit(struct cfs *fs)
{
    if (fs->failed)
        return fs->failed;
    if (fs->nr_dirty == 0 && fs->nr_pending == 0 && fs->nr_exempt == 0) {
        bool any = false;
        for (unsigned c = 0; c < fs->nr_chunks; c++)
            any = any || fs->bitmap_dirty[c];
        if (!any)
            return 0;
    }
    /*
     * The record of what this transaction frees
     * (docs/audit/next-subsystem-unmount-leak.md). The order below is
     * the design, and both halves of it are load-bearing:
     *
     *  - Everything that allocates must happen *before* commit_bitmap,
     *    which is the one pass that makes the on-disk bitmap agree with
     *    the bits in memory. Allocating after it publishes a root whose
     *    bitmap does not know about the blocks, so the next allocation
     *    hands them out and the filesystem eats its own metadata.
     *  - The set to record is not final until commit_bitmap has run,
     *    because it frees every chunk and index it copies.
     *
     * Reserve before, fill after: that breaks the circle without a
     * second pass and without a loop.
     */
    /*
     * Only from version 9. Below it the superblock word is the first of
     * `reserved[5]` and writing a chain head there would put a pointer
     * in a field an older kernel does not know about -- and this kernel
     * would not read it back either, since the gate is on reading too,
     * so every record would be a leak nobody could see.
     */
    bool record_frees = fs->sb.version >= 9;
    int rc = 0;
    struct cfs_res res = { .blk = NULL, .n = 0, .used = 0 };
    bool *held = NULL;
    unsigned held_n = 0;
    if (record_frees) {
        rc = freelog_release_previous(fs);
        if (rc)
            return rc;
    }
    /* The same for what this root still owes: the old record's blocks
     * are freed by this transaction, so they are entries in the new
     * free record like any other. */
    if (fs->sb.version >= 10) {
        rc = orphan_release_previous(fs);
        if (rc)
            return rc;
    }
    /*
     * Always, not only from version 9: the deadlist fill below needs its
     * blocks on any filesystem that has a snapshot, and a release loop
     * that cannot record a hold is a release loop that frees a
     * snapshot's blocks.
     */
    rc = commit_reserve(fs, &res, record_frees);
    if (rc)
        return rc;

    rc = commit_bitmap(fs);
    /*
     * The deadlist, before the record and before the root. It takes the
     * verdict for every pending free once and puts the held ones on a
     * copy of the snapshot's deadlist; the record then names what is
     * left, and the release loop below clears exactly that
     * (docs/audit/next-subsystem-snap-deadlist.md).
     */
    if (rc == 0 && fs->nr_pending > 0 && fs->snap_count > 0) {
        held_n = fs->nr_pending;
        held = kzalloc((size_t)held_n * sizeof(*held));
        if (held == NULL)
            rc = -ENOMEM;
        else
            rc = cfs_snapshot_fill(fs, &res, held, held_n);
        if (rc == 0 && fs->nr_pending != held_n) {
            /* Nothing in the fill defers a free, so this cannot move.
             * If it ever does, the record would name a block a snapshot
             * holds, so the commit stops rather than guessing. */
            kerror("cosmofs: the pending list moved during the deadlist fill (%u -> %u)", held_n,
                   fs->nr_pending);
            rc = -EIO;
        }
    }
    /* Before the free record and after the deadlist, for the same
     * reason the deadlist is before the record: it takes blocks from the
     * shared reservation and its predecessor's blocks are frees this
     * root must be able to name. The record is last because it names the
     * leftovers. */
    if (rc == 0)
        rc = orphan_fill(fs, &res);
    unsigned snap_used = res.used;
    if (rc == 0 && record_frees) {
        rc = freelog_fill(fs, res.blk + res.used, res.n - res.used, held, held_n);
    } else if (rc == 0) {
        /* No record on this filesystem, so nothing will name the blocks
         * the reservation did not need: give them back. They are free
         * from the next commit, which is the same bargain a version-8
         * filesystem already makes with every other deferred free. */
        for (unsigned i = res.used; i < res.n; i++)
            cfs_free_block_deferred(fs, res.blk[i]);
        res.used = res.n;
    }
    if (rc && res.blk != NULL) {
        /*
         * This root will not be published, so nothing names the blocks
         * the record's share of the reservation took. Left allocated
         * they are a leak the *next* successful commit makes durable,
         * because that commit writes this bitmap. Give them back the way
         * every other rolled-back allocation in a transaction does --
         * deferred, so the bit survives until a root says they are free
         * -- and drop whatever `freelog_fill` had already written into
         * them, which must not reach the disk under an owner that no
         * longer exists.
         *
         * The snapshot list's share -- `res.blk[0 .. snap_used)` -- is
         * *not* given back, because `fs->sb.snap_root` names it. The
         * copy stays in the still-open transaction, where the next
         * attempt finds it stamped with this generation and copies
         * nothing; giving it back would leave the root pointing at a
         * block the allocator had handed away.
         */
        for (unsigned i = snap_used; i < res.n; i++) {
            struct cfs_buf *b = buf_find(fs, res.blk[i]);
            if (b != NULL)
                buf_mark_clean(fs, b);
            cfs_free_block_deferred(fs, res.blk[i]);
        }
        fs->sb.free_root = 0;
        fs->sb.orphan_root = 0;
    }
    kfree(res.blk);
    if (rc) {
        kfree(held);
        return rc;
    }

    /* Every dirty metadata block, re-sealed with its final contents. */
    struct cfs_buf *b;
    list_for_each_entry(b, &fs->bufs, link) {
        if (!b->dirty)
            continue;
        struct cfs_mhdr *h = cfs_buf_hdr(b);
        mhdr_seal(fs, b->data, h->kind, b->blkno);
        rc = pool_write(fs->pool, b->blkno, b->data);
        if (rc) {
            kfree(held);
            return rc;
        }
    }

    /* The root, into the other slot, flushed before and after (one call:
     * the block layer runs flush, write, flush). free_blocks excludes
     * this transaction's pending frees: they are still referenced by the
     * previous root until this one is on disk. */
    fs->sb.generation = fs->gen;
    fs->sb.free_blocks = fs->free_blocks;
    /* Every device's label carries the commit it took part in, stamped
     * after that commit's blocks are stable and before the root that
     * publishes them: a label is never newer than the root it belongs
     * to, so a device that misses a commit can be told apart at the
     * next mount (design.md, "Format version 5"). */
    rc = cfs_labels_update(fs);
    if (rc) {
        kfree(held);
        return rc;
    }
    /* Now make all of it stable, on every device. The root write's own
     * preflush reaches only the devices carrying the superblock, which
     * is member 0's; a root that names blocks still sitting in another
     * member's write cache is a root that can outlive them. This is not
     * about labels alone -- it has been true of every block on another
     * member since a pool could have more than one. */
    rc = pool_flush(fs->pool);
    if (rc) {
        kfree(held);
        return rc;
    }
    unsigned slot = fs->sb_slot == CFS_SUPER_A ? CFS_SUPER_B : CFS_SUPER_A;
    rc = super_write(fs, slot, BIO_PREFLUSH | BIO_FUA);
    if (rc) {
        kfree(held);
        return rc;
    }
    fs->sb_slot = slot;
    fs->commits++;
    fs->first_dirty_ns = 0;

    /* The new root is durable: the old generation's blocks are free. */
    list_for_each_entry(b, &fs->bufs, link) {
        if (b->dirty) {
            b->dirty = false;
        }
    }
    fs->nr_dirty = 0;
    /* A block reaches pending_free exactly when the previous tree named
     * it and the new one does not -- which is what a snapshot still
     * names. While one exists the block is remembered on its deadlist
     * and its bitmap bit stays set, so the allocator never hands it out
     * (design.md, "Not freeing what a snapshot names").
     *
     * What is left here is the clearing. The remembering happened before
     * the root, in cfs_snapshot_fill, and `held` is that verdict: this
     * loop neither allocates, nor writes, nor asks the snapshot list
     * anything. It used to do all three, and that is what a crash and an
     * unmount each lost half of
     * (docs/audit/next-subsystem-snap-deadlist.md). */
    for (unsigned i = 0; i < fs->nr_pending; i++) {
        if (held != NULL && i < held_n && held[i])
            continue;   /* a snapshot's, and already on its deadlist */
        uint64_t dva = fs->pending_free[i];
        uint64_t lin = cfs_dva_lin(fs, dva);
        if (lin == CFS_DVA_NONE || !bit_test(fs->bitmap, lin))
            continue;
        bit_clear(fs->bitmap, lin);
        fs->bitmap_dirty[lin / CFS_BITS_PER_BITMAP] = 1;
        fs->free_blocks++;
        fs->mem[CFS_DVA_VDEV(dva)].free_blocks++;
    }
    fs->nr_pending = 0;
    kfree(held);
    held = NULL;
    /* And the blocks no snapshot may hold, with no filter at all. */
    for (unsigned i = 0; i < fs->nr_exempt; i++) {
        uint64_t dva = fs->pending_exempt[i];
        uint64_t lin = cfs_dva_lin(fs, dva);
        if (lin == CFS_DVA_NONE || !bit_test(fs->bitmap, lin))
            continue;
        bit_clear(fs->bitmap, lin);
        fs->bitmap_dirty[lin / CFS_BITS_PER_BITMAP] = 1;
        fs->free_blocks++;
        fs->mem[CFS_DVA_VDEV(dva)].free_blocks++;
    }
    fs->nr_exempt = 0;
    fs->gen++;
    /* The frees dirtied bitmap chunks for the next commit; that is
     * bookkeeping of this commit, not a new change to age. */
    fs->first_dirty_ns = 0;
    kdebug("cosmofs: committed generation %llu (%llu free blocks)", (unsigned long long)fs->sb.generation,
           (unsigned long long)fs->free_blocks);
    return 0;
}

/* Write back every dirty page of every cached regular file. Called with
 * no cfs lock held (writepage takes it). */
int cfs_sync_vnodes(struct cfs *fs)
{
    struct mount *mnt = fs->mnt;
    int rc = 0;
    for (unsigned b = 0; b < VNODE_HASH; b++) {
        for (;;) {
            struct vnode *vn = NULL;
            /* The hash lock is a spinlock; a hashed vnode always holds a
             * reference (vnode_put unhashes before the last drop). */
            arch_irq_state_t s = spin_lock_irqsave(&mnt->lock);
            struct vnode *it;
            list_for_each_entry(it, &mnt->vnodes[b], hash_link) {
                if (it->type == VNODE_REG && it->pc.nr_dirty) {
                    vn = it;
                    vnode_get(vn);
                    break;
                }
            }
            spin_unlock_irqrestore(&mnt->lock, s);
            if (vn == NULL)
                break;
            mutex_lock(&vn->lock);
            int r = pagecache_sync(vn);
            mutex_unlock(&vn->lock);
            vnode_put(vn);
            if (r) {
                rc = r;
                break;
            }
        }
    }
    return rc;
}

/* --- format ------------------------------------------------------------------ */

/*
 * Format `n` devices as one pool. Member 0 carries the superblocks, the
 * member table and its own allocation metadata; every other member
 * carries a label at block 0 and its own allocation metadata, so that
 * losing one member does not take another's bitmap with it (design.md,
 * "The member table").
 */
/*
 * `copies` devices per member, taken from `bd` member by member: with
 * copies = 2 and n = 2, bd[0] and bd[1] are member 0's mirror and bd[2]
 * and bd[3] are member 1's.
 */
/* The user key an encrypted format wraps its master key with; NULL for
 * a filesystem in the clear. */
static const void *g_format_key;
static size_t g_format_key_len;

static int format_at(struct blkdev **bd, unsigned n, unsigned copies, unsigned version)
{
    if (n == 0 || n > CFS_MAX_MEMBERS || copies == 0 || copies > CFS_MAX_COPIES)
        return -EINVAL;
    if (version < 4 && (n != 1 || copies != 1))
        return -EINVAL;   /* one device is all the older formats can name */
    if (version < 5 && copies != 1)
        return -EINVAL;   /* a mirror group is what version 5 adds */
    struct spool *pool;
    int rc = pool_open(bd[0], &pool);
    if (rc)
        return rc;
    for (unsigned c = 1; c < copies && rc == 0; c++)
        rc = pool_add_copy(pool, 0, bd[c]);
    for (unsigned v = 1; v < n && rc == 0; v++) {
        rc = pool_add_member(pool, bd[v * copies], NULL);
        for (unsigned c = 1; c < copies && rc == 0; c++)
            rc = pool_add_copy(pool, v, bd[v * copies + c]);
    }
    if (rc) {
        pool_close(pool);
        return rc;
    }

    struct cfs_member mem[CFS_MAX_MEMBERS];
    memset(mem, 0, sizeof(mem[0]) * n);
    uint8_t uuid[16];
    random_get_bytes(uuid, sizeof(uuid));
    uint64_t total = 0, free_total = 0;
    for (unsigned v = 0; v < n; v++) {
        uint64_t nb = pool->m[v].nblocks;
        if (nb < CFS_MIN_BLOCKS || nb > CFS_MAX_BLOCKS) {
            pool_close(pool);
            return -EINVAL;
        }
        unsigned chunks = (unsigned)((nb + CFS_BITS_PER_BITMAP - 1) / CFS_BITS_PER_BITMAP);
        if (chunks > CFS_PTRS_PER_BLOCK) {
            pool_close(pool);
            return -EINVAL;
        }
        memcpy(mem[v].uuid, uuid, 16);
        mem[v].nblocks = nb;
        mem[v].copies = copies;
        mem[v].first_usable = v == 0 ? 2 : 1;
        total += nb;
    }

    /* Member 0: 0,1 superblocks; 2 member table; 3 alloc index; 4..
     * bitmaps; then imap L1, imap L0, inode block 0.
     * Member v>0: 0 label; 1 alloc index; 2.. bitmaps. */
    unsigned chunks0 = (unsigned)((mem[0].nblocks + CFS_BITS_PER_BITMAP - 1) / CFS_BITS_PER_BITMAP);
    bool v4 = version >= 4;
    bool crypt = version >= 7 && g_format_key != NULL;
    /* Before version 4 there is no member table, and the allocation
     * index is at block 2 where that format put it. */
    uint64_t members_blk = 2, alloc_idx = v4 ? 3 : 2, bitmap0 = v4 ? 4 : 3;
    uint64_t imap1 = bitmap0 + chunks0, imap0 = imap1 + 1, inodes0 = imap0 + 1;
    uint64_t keys_blk = inodes0 + 1;
    uint64_t used0 = crypt ? keys_blk + 1 : inodes0 + 1;

    uint8_t *block = kmalloc(CFS_BLOCK, KMEM_ZERO);
    if (block == NULL) {
        pool_close(pool);
        return -ENOMEM;
    }
    struct cfs tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.gen = 1;
    tmp.pool = pool;

    for (unsigned v = 0; v < n && rc == 0; v++) {
        uint64_t nb = mem[v].nblocks;
        unsigned chunks = (unsigned)((nb + CFS_BITS_PER_BITMAP - 1) / CFS_BITS_PER_BITMAP);
        uint64_t idx_blk = v == 0 ? alloc_idx : 1;
        (void)members_blk;
        uint64_t bm0 = v == 0 ? bitmap0 : 2;
        uint64_t used = v == 0 ? used0 : bm0 + chunks;
        for (unsigned c = 0; c < chunks && rc == 0; c++) {
            memset(block, 0, CFS_BLOCK);
            uint8_t *bits = block + CFS_MHDR_SIZE;
            for (uint64_t i = (uint64_t)c * CFS_BITS_PER_BITMAP; i < ((uint64_t)c + 1) * CFS_BITS_PER_BITMAP; i++) {
                if (i >= nb || i < used)
                    bit_set(bits, i - (uint64_t)c * CFS_BITS_PER_BITMAP);   /* past-the-end bits stay allocated */
            }
            mhdr_seal(&tmp, block, CFS_KIND_BITMAP, CFS_DVA(v, bm0 + c));
            rc = pool_write(pool, CFS_DVA(v, bm0 + c), block);
        }
        if (rc == 0) {
            memset(block, 0, CFS_BLOCK);
            uint64_t *p = (uint64_t *)(block + CFS_MHDR_SIZE);
            for (unsigned c = 0; c < chunks; c++)
                p[c] = CFS_DVA(v, bm0 + c);
            mhdr_seal(&tmp, block, CFS_KIND_ALLOCIDX, CFS_DVA(v, idx_blk));
            rc = pool_write(pool, CFS_DVA(v, idx_blk), block);
        }
        /* Generation 1: the commit this device took part in. */
        for (unsigned c = 0; c < copies && rc == 0 && v > 0; c++)
            rc = cfs_label_write(pool, v, c, 1, uuid, nb);
        mem[v].alloc_root = CFS_DVA(v, idx_blk);
        mem[v].free_blocks = nb - used;
        free_total += nb - used;
    }

    /* The member table. */
    if (rc == 0 && v4) {
        memset(block, 0, CFS_BLOCK);
        struct cfs_member_block *mb = (struct cfs_member_block *)(block + CFS_MHDR_SIZE);
        mb->count = n;
        for (unsigned v = 0; v < n; v++)
            mb->m[v] = mem[v];
        mhdr_seal(&tmp, block, CFS_KIND_MEMBERS, CFS_DVA(0, members_blk));
        rc = pool_write(pool, CFS_DVA(0, members_blk), block);
    }
    /* Inode block 0 with the root directory (inode 1). */
    if (rc == 0) {
        memset(block, 0, CFS_BLOCK);
        struct cfs_inode *root = (struct cfs_inode *)(block + CFS_MHDR_SIZE + CFS_ROOT_INO * CFS_INODE_SIZE);
        root->mode = CFS_MODE(CFS_TYPE_DIR, 0755);
        root->nlink = 2;
        root->ino = CFS_ROOT_INO;
        root->parent = CFS_ROOT_INO;
        root->generation = 1;
        /* An encrypted filesystem authenticates rather than checksums:
         * a CRC cannot tell a deliberate change from an accident. */
        root->csum_algo = crypt ? CFS_CSUM_POLY1305 : CFS_CSUM_CRC32C;
        root->compress_algo = CFS_COMPRESS_NONE;
        mhdr_seal(&tmp, block, CFS_KIND_INODES, inodes0);
        rc = pool_write(pool, inodes0, block);
    }
    if (rc == 0) {
        memset(block, 0, CFS_BLOCK);
        ((uint64_t *)(block + CFS_MHDR_SIZE))[0] = inodes0;
        mhdr_seal(&tmp, block, CFS_KIND_IMAP0, imap0);
        rc = pool_write(pool, imap0, block);
    }
    if (rc == 0) {
        memset(block, 0, CFS_BLOCK);
        ((uint64_t *)(block + CFS_MHDR_SIZE))[0] = imap0;
        mhdr_seal(&tmp, block, CFS_KIND_IMAP1, imap1);
        rc = pool_write(pool, imap1, block);
    }
    uint8_t master[CHACHA20_KEY_SIZE];
    if (rc == 0 && crypt) {
        random_get_bytes(master, sizeof(master));
        rc = cfs_keys_write(pool, CFS_DVA(0, keys_blk), 1, master, g_format_key, g_format_key_len);
        memset(master, 0, sizeof(master));
    }
    if (rc == 0)
        rc = pool_flush(pool);
    /* Superblock A at generation 1; slot B zeroed. Member 0's addresses
     * are numerically what they always were, so a version-3 reader sees
     * the same layout it would have written. */
    if (rc == 0) {
        memset(&tmp.sb, 0, sizeof(tmp.sb));
        memcpy(tmp.sb.magic, CFS_MAGIC, 8);
        tmp.sb.version = version;
        tmp.sb.block_size = CFS_BLOCK;
        tmp.sb.total_blocks = total;
        tmp.sb.generation = 1;
        tmp.sb.imap_root = imap1;
        tmp.sb.alloc_root = v4 ? 0 : CFS_DVA(0, alloc_idx);
        tmp.sb.next_ino = 2;
        tmp.sb.inode_count = 1;
        tmp.sb.free_blocks = free_total;
        tmp.sb.members = v4 ? CFS_DVA(0, members_blk) : 1;
        tmp.sb.key_root = crypt ? CFS_DVA(0, keys_blk) : 0;
        if (v4)
            memcpy(tmp.sb.uuid, uuid, 16);
        memset(block, 0, CFS_BLOCK);
        rc = pool_write(pool, CFS_SUPER_B, block);
        if (rc == 0)
            rc = super_write(&tmp, CFS_SUPER_A, BIO_PREFLUSH | BIO_FUA);
    }
    kfree(block);
    pool_close(pool);
    if (rc == 0)
        kinfo("cosmofs: formatted %s%s: %u member(s), %llu blocks, %llu free", bd[0]->name,
              n > 1 ? " and others" : "", n, (unsigned long long)total, (unsigned long long)free_total);
    return rc;
}

int cosmofs_format_pool(struct blkdev **bd, unsigned n)
{
    return format_at(bd, n, 1, CFS_VERSION);
}

int cosmofs_format_mirror(struct blkdev **bd, unsigned n, unsigned copies)
{
    return format_at(bd, n, copies, CFS_VERSION);
}

int cosmofs_format(struct blkdev *bd)
{
    return format_at(&bd, 1, 1, CFS_VERSION);
}

/* Format with encryption: a random master key, wrapped with `key`. */
int cosmofs_format_encrypted(struct blkdev *bd, const void *key, size_t len)
{
    if (key == NULL || len == 0)
        return -EINVAL;
    g_format_key = key;
    g_format_key_len = len;
    int rc = format_at(&bd, 1, 1, CFS_VERSION);
    g_format_key = NULL;
    g_format_key_len = 0;
    return rc;
}

/* Test hook: write an older format, so that "versions 2 and 3 mount
 * unchanged" is a claim something checks (design.md, "The DVA is 64
 * bits"). */
int cosmofs_test_format_version(struct blkdev *bd, unsigned version)
{
    if (version < CFS_VERSION_MIN || version > CFS_VERSION)
        return -EINVAL;
    return format_at(&bd, 1, 1, version);
}

/*
 * Test hook: write a value into the superblock word that version 9 calls
 * `free_root`, on a filesystem too old to have one.
 *
 * Every image this tree formats zeroes its reserved words, so the gate
 * that stops an older filesystem's reserved zero being read as a chain
 * head is unobservable without this: the bug-proof for that gate passes
 * on a zero either way. An image from another writer -- a later version
 * that used the word, a different implementation -- is what the gate is
 * actually for, and this manufactures one.
 */
/*
 * Write a count past the payload into the record the live root names: a
 * block whose checksum is good and whose contents are not. The live root
 * is the valid slot with the higher generation, which is not always slot
 * 0 -- a commit writes the other one.
 */
int cosmofs_test_poison_freelog_count(struct blkdev *bd, uint64_t count)
{
    struct spool *pool = NULL;
    int rc = pool_open(bd, &pool);
    if (rc)
        return rc;
    uint8_t *block = kmalloc(CFS_BLOCK, KMEM_ZERO);
    if (block == NULL) {
        pool_close(pool);
        return -ENOMEM;
    }
    uint64_t at = 0, best_gen = 0;
    for (unsigned slot = 0; slot < 2; slot++) {
        if (pool_read(pool, slot, block))
            continue;
        const struct cfs_super *sb = (const struct cfs_super *)block;
        uint32_t want = sb->crc;
        ((struct cfs_super *)block)->crc = 0;
        uint32_t have = block_crc(block, offsetof(struct cfs_super, crc));
        if (want != have || sb->generation == 0 || sb->generation < best_gen)
            continue;
        best_gen = sb->generation;
        at = sb->free_root;
    }
    rc = at == 0 ? -ENOENT : 0;
    if (rc == 0)
        rc = pool_read(pool, at, block);
    if (rc == 0) {
        struct cfs_dead_block *d = (struct cfs_dead_block *)(block + CFS_MHDR_SIZE);
        d->count = count;
        struct cfs_mhdr *h = (struct cfs_mhdr *)block;
        h->crc = 0;
        h->crc = block_crc(block, offsetof(struct cfs_mhdr, crc));
        rc = pool_write(pool, at, block);
        if (rc == 0)
            rc = pool_flush(pool);
    }
    kfree(block);
    pool_close(pool);
    return rc;
}

int cosmofs_test_poison_free_root(struct blkdev *bd, uint64_t value)
{
    struct spool *pool = NULL;
    int rc = pool_open(bd, &pool);
    if (rc)
        return rc;
    uint8_t *block = kmalloc(CFS_BLOCK, KMEM_ZERO);
    if (block == NULL) {
        pool_close(pool);
        return -ENOMEM;
    }
    rc = pool_read(pool, 0, block);
    if (rc == 0) {
        struct cfs_super *sb = (struct cfs_super *)block;
        sb->free_root = value;
        sb->crc = 0;
        sb->crc = block_crc(block, offsetof(struct cfs_super, crc));
        rc = pool_write(pool, 0, block);
        if (rc == 0)
            rc = pool_flush(pool);
    }
    kfree(block);
    pool_close(pool);
    return rc;
}

/*
 * Break the filesystem in one named way (kernel/include/kernel/cosmofs.h).
 * Each case manufactures exactly one finding of the structural check, so
 * that every class it can report has a test that produced it on purpose.
 */
int cosmofs_test_corrupt(struct mount *mnt, enum cosmofs_corruption kind, uint64_t ino, uint64_t *what)
{
    struct cfs *fs = cfs_of(mnt);
    if (fs == NULL)
        return -EINVAL;

    /*
     * An inode number rather than a path: a filesystem-level hook has no
     * business resolving names, it has to work on a filesystem whose
     * directories are the broken part, and resolving one here would put
     * a VFS symbol in this file that the fuzz harness has to link.
     */
    struct cfs_inode in;
    if (ino != 0 && cfs_inode_read(fs, ino, &in) != 0)
        return -ENOENT;

    mutex_lock(&fs->lock);
    int rc = 0;
    uint64_t token = 0;
    switch (kind) {
    case COSMOFS_CORRUPT_LEAK: {
        uint64_t blk = 0, got = 0;
        rc = cfs_alloc_run(fs, CFS_ALLOC_META, 0, 1, &blk, &got);
        token = blk;
        break;
    }
    case COSMOFS_CORRUPT_FREE_IN_USE: {
        uint64_t lin = cfs_dva_lin(fs, in.direct[0].start);
        if (lin == CFS_DVA_NONE) {
            rc = -EINVAL;
            break;
        }
        bit_clear(fs->bitmap, lin);
        fs->bitmap_dirty[lin / CFS_BITS_PER_BITMAP] = 1;
        fs->free_blocks++;
        fs->sb.free_blocks++;
        token = in.direct[0].start;
        break;
    }
    case COSMOFS_CORRUPT_CROSSLINK: {
        /* A second inode whose first extent is the named file's: two
         * live claims on one block, which is the cross-link. */
        uint64_t second = 0;
        rc = cfs_inode_alloc(fs, &second);
        if (rc)
            break;
        struct cfs_inode twin;
        memset(&twin, 0, sizeof(twin));
        twin.mode = CFS_MODE(CFS_TYPE_REG, 0644);
        twin.nlink = 1;
        twin.ino = second;
        twin.parent = CFS_ROOT_INO;
        twin.csum_algo = in.csum_algo;
        twin.size = CFS_BLOCK;
        twin.direct[0] = in.direct[0];
        rc = cfs_inode_write(fs, second, &twin);
        token = in.direct[0].start;
        break;
    }
    case COSMOFS_CORRUPT_NLINK:
        in.nlink++;
        rc = cfs_inode_write(fs, ino, &in);
        token = ino;
        break;
    case COSMOFS_CORRUPT_ORPHAN: {
        /* An inode with a block and no name: what a crash between an
         * unlink and the eviction that frees the blocks leaves. */
        uint64_t lost = 0;
        rc = cfs_inode_alloc(fs, &lost);
        if (rc)
            break;
        struct cfs_inode dead;
        memset(&dead, 0, sizeof(dead));
        dead.mode = CFS_MODE(CFS_TYPE_REG, 0644);
        dead.nlink = 0;
        dead.ino = lost;
        dead.parent = CFS_ROOT_INO;
        dead.csum_algo = CFS_CSUM_CRC32C;
        uint64_t blk = 0, got = 0;
        rc = cfs_alloc_run(fs, CFS_ALLOC_DATA, 0, 1, &blk, &got);
        if (rc)
            break;
        dead.size = CFS_BLOCK;
        dead.direct[0].start = blk;
        dead.direct[0].count = 1;
        dead.direct[0].lblk = 0;
        rc = cfs_inode_write(fs, lost, &dead);
        token = lost;
        break;
    }
    case COSMOFS_CORRUPT_DANGLING:
    case COSMOFS_CORRUPT_DIRENT: {
        /* Both rewrite one entry of the root directory: the first to
         * name a slot nothing allocated, the second to claim a type its
         * inode does not have. */
        struct cfs_inode root;
        if (cfs_inode_read(fs, CFS_ROOT_INO, &root) != 0) {
            rc = -EIO;
            break;
        }
        uint8_t *block = kmalloc(CFS_BLOCK, 0);
        if (block == NULL) {
            rc = -ENOMEM;
            break;
        }
        rc = cfs_dir_read_block_at(fs, &root, 0, block);
        if (rc == 0) {
            struct cfs_dirent *d = (struct cfs_dirent *)block;
            bool done = false;
            for (unsigned i = 0; i < CFS_DIRENTS_PER_BLOCK && !done; i++) {
                if (d[i].ino == 0)
                    continue;
                if (kind == COSMOFS_CORRUPT_DANGLING) {
                    token = fs->sb.next_ino + 1000;
                    d[i].ino = token;
                } else {
                    token = d[i].ino;
                    d[i].type = d[i].type == CFS_TYPE_DIR ? CFS_TYPE_REG : CFS_TYPE_DIR;
                }
                done = true;
            }
            rc = done ? cfs_dir_write_block_at(fs, &root, 0, block) : -ENOENT;
        }
        kfree(block);
        break;
    }
    case COSMOFS_CORRUPT_INO_SLOT:
        /* The slot keeps its position and loses its identity: the walk
         * reaches it by position and must not believe the number in it.
         * Nothing else changes, so a check that trusts the field sees an
         * ordinary inode at a number that does not exist. */
        in.ino = ino + 1000;
        rc = cfs_inode_write(fs, ino, &in);
        token = ino;
        break;
    case COSMOFS_CORRUPT_COUNTER:
        /* Both of them, in opposite directions: the check must report one
         * finding per counter rather than one for "the superblock". */
        fs->free_blocks++;
        fs->sb.free_blocks++;
        if (fs->sb.inode_count > 0)
            fs->sb.inode_count--;
        token = fs->sb.free_blocks;
        break;
    default:
        rc = -EINVAL;
        break;
    }
    mutex_unlock(&fs->lock);
    if (rc == 0 && what)
        *what = token;
    return rc;
}

/* --- mount / unmount ------------------------------------------------------------ */

static int super_read(struct cfs *fs, unsigned slot, struct cfs_super *out)
{
    uint8_t *block = kmalloc(CFS_BLOCK, 0);
    if (block == NULL)
        return -ENOMEM;
    int rc = pool_read(fs->pool, slot, block);
    if (rc == 0) {
        const struct cfs_super *sb = (const struct cfs_super *)block;
        /* Version 2 mounts unchanged: snapshots only use fields it
         * already reserved, and its snap_root is 0 (no snapshots). */
        bool version_ok = sb->version >= CFS_VERSION_MIN && sb->version <= CFS_VERSION;
        if (memcmp(sb->magic, CFS_MAGIC, 8) == 0 && !version_ok && sb->version != 0)
            kerror("cosmofs: slot %u is format version %u; this kernel reads versions %u to %u (reformat)", slot,
                   sb->version, CFS_VERSION_MIN, CFS_VERSION);
        /* The member table is not read yet -- it is named by this very
         * block -- so a pointer can only be checked for shape here.
         * What it actually addresses is checked when it is read: every
         * metadata block carries its own DVA (mhdr_check). Before
         * version 4 there is one member and the old exact checks hold. */
        uint64_t dev0 = fs->pool->m[0].nblocks;
        bool ptrs_ok;
        if (sb->version >= 4)
            ptrs_ok = sb->total_blocks <= CFS_MAX_BLOCKS * (uint64_t)CFS_MAX_MEMBERS &&
                      CFS_DVA_VDEV(sb->imap_root) < CFS_MAX_MEMBERS && CFS_DVA_BLK(sb->imap_root) < CFS_MAX_BLOCKS &&
                      CFS_DVA_VDEV(sb->members) < CFS_MAX_MEMBERS && CFS_DVA_BLK(sb->members) < CFS_MAX_BLOCKS;
        else
            ptrs_ok = sb->total_blocks == dev0 && sb->imap_root < dev0 && sb->alloc_root < dev0;
        if (memcmp(sb->magic, CFS_MAGIC, 8) != 0 || !version_ok || sb->block_size != CFS_BLOCK ||
            sb->total_blocks == 0 || sb->generation == 0 ||
            block_crc(block, offsetof(struct cfs_super, crc)) != sb->crc || !ptrs_ok)
            rc = -EIO;
        else
            *out = *sb;
    }
    kfree(block);
    return rc;
}

static int load_bitmap(struct cfs *fs)
{
    /* fs->nblocks and fs->nr_chunks describe the linear space the
     * members were laid out in (cfs_members_load). */
    fs->bitmap = kzalloc((size_t)((fs->nblocks + 7) / 8));
    fs->bitmap_dirty = kzalloc(fs->nr_chunks);
    if (fs->bitmap == NULL || fs->bitmap_dirty == NULL)
        return -ENOMEM;
    int rc = 0;
    uint64_t free = 0;
    for (unsigned v = 0; v < fs->nmembers && rc == 0; v++) {
        struct cfs_memstate *m = &fs->mem[v];
        struct cfs_buf *idx;
        rc = cfs_buf_get(fs, m->alloc_root, CFS_KIND_ALLOCIDX, &idx);
        if (rc)
            break;
        uint64_t *slots = ptrs(idx);
        for (unsigned c = 0; c < m->nchunks && rc == 0; c++) {
            struct cfs_buf *bm;
            rc = cfs_buf_get(fs, slots[c], CFS_KIND_BITMAP, &bm);
            if (rc)
                break;
            size_t bytes = CFS_BITS_PER_BITMAP / 8;
            size_t off = (size_t)(m->chunk0 + c) * bytes;
            size_t total = (size_t)((fs->nblocks + 7) / 8);
            size_t n = off + bytes <= total ? bytes : total - off;
            memcpy(fs->bitmap + off, bm->data + CFS_MHDR_SIZE, n);
            cfs_buf_put(fs, bm);
        }
        cfs_buf_put(fs, idx);
        if (rc)
            break;
        /* The tail of a member's last chunk addresses nothing: mark it
         * taken so the allocator can never reach it. */
        for (uint64_t b = m->nblocks; b < (uint64_t)m->nchunks * CFS_BITS_PER_BITMAP; b++)
            bit_set(fs->bitmap, m->base + b);
        uint64_t mfree = 0;
        for (uint64_t b = m->first_usable; b < m->nblocks; b++)
            mfree += bit_test(fs->bitmap, m->base + b) ? 0 : 1;
        m->free_blocks = mfree;
        m->alloc_hint = m->first_usable;
        free += mfree;
    }
    if (rc)
        return rc;
    fs->free_blocks = free;

    /*
     * Finish what the last commit started. It cleared these blocks' bits
     * in memory after its root was durable and marked the chunks for the
     * next commit, which at an unmount never came -- so on disk they are
     * still allocated and reachable from nothing. The root that freed
     * them says which they are, and this is where that is read
     * (docs/audit/next-subsystem-unmount-leak.md).
     *
     * Idempotent on purpose: the record is not cleared here. A mount
     * that replays and unmounts without committing changes nothing, and
     * the next mount replays the same list to the same effect. The
     * commit that supersedes the record is what retires it.
     */
    rc = freelog_replay(fs);
    if (rc)
        return rc;

    if (fs->free_blocks != fs->sb.free_blocks)
        kwarn("cosmofs: free block count %llu differs from the superblock's %llu; using the bitmap",
              (unsigned long long)fs->free_blocks, (unsigned long long)fs->sb.free_blocks);
    return 0;
}

static void cfs_destroy(struct cfs *fs)
{
    struct cfs_buf *b, *tmp;
    list_for_each_entry_safe(b, tmp, &fs->bufs, link) {
        list_remove(&b->link);
        kfree(b->data);
        kfree(b);
    }
    kfree(fs->bitmap);
    kfree(fs->bitmap_dirty);
    kfree(fs->pending_free);
    kfree(fs->pending_exempt);
    kfree(fs->orphans);
    cfs_members_free(fs);
    if (fs->pool)
        pool_close(fs->pool);
    kfree(fs);
}

/* Drop every buffer and the bitmap so a different root can be loaded. */
static void cfs_reset_root(struct cfs *fs)
{
    struct cfs_buf *b, *tmp;
    list_for_each_entry_safe(b, tmp, &fs->bufs, link) {
        list_remove(&b->link);
        kfree(b->data);
        kfree(b);
    }
    fs->nr_bufs = fs->nr_dirty = 0;
    kfree(fs->bitmap);
    kfree(fs->bitmap_dirty);
    fs->bitmap = NULL;
    fs->bitmap_dirty = NULL;
    /* The other root may describe a different set of members; the pool
     * keeps its devices, the table is read again. */
    cfs_members_free(fs);
}

/* Load the allocator and the root directory of the root in fs->sb. */
static int load_root(struct cfs *fs, struct vnode **root)
{
    fs->gen = fs->sb.generation + 1;
    fs->alloc_hint = 2;
    int rc = cfs_members_load(fs);
    if (rc)
        return rc;
    /* The key, if this filesystem has one. A mount without it still
     * works for everything that is not a file's contents: the metadata
     * is plaintext by design, and every data read answers -ENOKEY
     * (design.md, "Boot-time unlock"). */
    fs->encrypted = fs->sb.version >= 7 && fs->sb.key_root != 0;
    if (fs->encrypted && !fs->have_key) {
        char key[128];
        if (fwcfg_get_string("fskey", key, sizeof(key)) && key[0]) {
            rc = cfs_keys_load(fs, key, strlen(key));
            memset(key, 0, sizeof(key));
            if (rc == -EKEYREJECTED)
                kerror("cosmofs: the key in opt/cosmo/fskey does not unwrap this filesystem");
            else if (rc)
                return rc;
        }
        if (!fs->have_key)
            kwarn("cosmofs: encrypted and locked; metadata only until a key arrives");
    }
    rc = 0;
    /* From the blocks that exist, not the linear span: that is rounded
     * up to whole bitmap chunks per member and is mostly padding on a
     * small device. */
    uint64_t tot = fs->sb.total_blocks;
    fs->reserve = tot / 32 > 32 ? tot / 32 : 32;
    rc = load_bitmap(fs);
    if (rc)
        return rc;
    /* How many snapshots exist decides whether a commit may free the
     * blocks it releases (design.md, "Format version 3"). */
    unsigned n = 0;
    if (cfs_snapshot_list(fs, NULL, 0, &n) == 0)
        fs->snap_count = n;
    if (n)
        kinfo("cosmofs: %u snapshot(s)", n);
    /*
     * And what the last root still owed: inodes unlinked while something
     * held them, whose eviction the crash or the forced unmount threw
     * away. After the bitmap and after the snapshot count, because the
     * frees this does go through both (docs/audit/next-subsystem-orphan.md).
     */
    rc = orphan_replay(fs);
    if (rc)
        return rc;
    return cfs_vnode_get(fs, CFS_ROOT_INO, root);
}

static int cosmofs_sync(struct mount *mnt);

/*
 * The writeback thread (design.md): commits when the open transaction
 * has grown past a threshold or aged past the interval. It takes the
 * mount's sync lock with a trylock so it never waits on an unmount or a
 * vfs_sync in progress, and never commits when the test hook has turned
 * autonomous commits off.
 */
static bool wb_due(struct cfs *fs)
{
    if (fs->failed || fs->first_dirty_ns == 0)
        return false;
    if (fs->nr_dirty >= CFS_WB_DIRTY_BUFS || fs->nr_pending >= CFS_WB_PENDING)
        return true;
    if (__atomic_load_n(&fs->mnt->cache_dirty, __ATOMIC_RELAXED) >= CFS_WB_DIRTY_PAGES)
        return true;
    return clock_now_ns() - fs->first_dirty_ns >= (uint64_t)fs->wb_interval_ms * 1000000ull;
}

static void cfs_writeback_thread(void *arg)
{
    struct cfs *fs = arg;
    struct mount *mnt = fs->mnt;
    while (!__atomic_load_n(&fs->wb_stop, __ATOMIC_ACQUIRE)) {
        thread_sleep_ms(CFS_WB_POLL_MS);
        if (!fs->wb_enabled || !wb_due(fs))
            continue;
        if (!mutex_trylock(&mnt->sync_lock))
            continue;   /* an unmount or vfs_sync is at it: they commit */
        if (!mnt->unmounted && !__atomic_load_n(&fs->wb_stop, __ATOMIC_ACQUIRE)) {
            int rc = cosmofs_sync(mnt);
            if (rc == 0)
                fs->wb_commits++;
            else
                kwarn("cosmofs: writeback commit failed (%d)", rc);
        }
        mutex_unlock(&mnt->sync_lock);
    }
}

static int cosmofs_mount(struct fs_type *fst, struct blkdev *bdev, unsigned flags, struct mount *mnt)
{
    (void)fst;
    (void)flags;
    if (bdev == NULL)
        return -EINVAL;
    struct cfs *fs = kzalloc(sizeof(*fs));
    if (fs == NULL)
        return -ENOMEM;
    list_init(&fs->bufs);
    mutex_init(&fs->lock, "cosmofs");
    fs->mnt = mnt;
    fs->wb_enabled = true;
    fs->wb_interval_ms = CFS_WB_INTERVAL_MS;
    int rc = pool_open(bdev, &fs->pool);
    if (rc) {
        kfree(fs);
        return rc;
    }
    /* fs->nblocks and the reserve follow from the member table, which
     * the root's own generation names (load_root). */

    struct cfs_super a, b;
    int ra = super_read(fs, CFS_SUPER_A, &a);
    int rb = super_read(fs, CFS_SUPER_B, &b);
    if (ra && rb) {
        kerror("cosmofs: %s: no valid superblock", bdev->name);
        cfs_destroy(fs);
        return -EIO;
    }
    /* The newer valid root first; if its tree does not load, the older
     * one (design.md, "Older-slot fallback at mount"). */
    unsigned first = (ra == 0 && (rb != 0 || a.generation >= b.generation)) ? CFS_SUPER_A : CFS_SUPER_B;
    unsigned other = first == CFS_SUPER_A ? CFS_SUPER_B : CFS_SUPER_A;
    int rother = first == CFS_SUPER_A ? rb : ra;
    fs->sb = first == CFS_SUPER_A ? a : b;
    fs->sb_slot = first;
    mnt->fs_priv = fs;
    struct vnode *root = NULL;
    rc = load_root(fs, &root);
    if (rc && rother == 0) {
        kwarn("cosmofs: %s: generation %llu does not load (%d); falling back to generation %llu", bdev->name,
              (unsigned long long)fs->sb.generation, rc, (unsigned long long)(other == CFS_SUPER_A ? a : b).generation);
        cfs_reset_root(fs);
        fs->sb = other == CFS_SUPER_A ? a : b;
        fs->sb_slot = other;
        rc = load_root(fs, &root);
    }
    if (rc) {
        mnt->fs_priv = NULL;
        cfs_destroy(fs);
        return rc;
    }
    mnt->root = root;
    kinfo("cosmofs: %s: generation %llu, %llu/%llu blocks free (%llu reserved), %llu inodes", bdev->name,
          (unsigned long long)fs->sb.generation, (unsigned long long)fs->free_blocks,
          (unsigned long long)fs->sb.total_blocks, (unsigned long long)fs->reserve,
          (unsigned long long)fs->sb.inode_count);
    return 0;
}

static int cosmofs_sync(struct mount *mnt)
{
    struct cfs *fs = cfs_of(mnt);
    if (fs == NULL)
        return 0;
    if (fs->discard_on_unmount)
        return 0;   /* test hook: behave as if the root write never happened */
    int rc = cfs_sync_vnodes(fs);
    if (rc)
        return rc;
    mutex_lock(&fs->lock);
    rc = cfs_commit(fs);
    mutex_unlock(&fs->lock);
    return rc;
}

/* The VFS committed through cosmofs_sync before calling this; what is
 * left is either nothing, a deliberately discarded transaction (test
 * hook) or an abandoned one (cfs_fail), and both are dropped here so the
 * on-disk state stays at the last committed root. The writeback thread
 * is stopped first (it skips a mount whose sync lock is held, which it
 * is here, so the join is short). */
static int cosmofs_unmount(struct mount *mnt)
{
    struct cfs *fs = cfs_of(mnt);
    if (fs->wb_thread) {
        __atomic_store_n(&fs->wb_stop, true, __ATOMIC_RELEASE);
        thread_join(fs->wb_thread);
        fs->wb_thread = NULL;
    }
    if (fs->discard_on_unmount)
        kwarn("cosmofs: discarding the open transaction (test hook)");
    else if (fs->failed)
        kwarn("cosmofs: dropping the abandoned transaction (%d)", fs->failed);
    mnt->fs_priv = NULL;   /* the root's eviction sees no filesystem */
    cfs_destroy(fs);
    return 0;
}

int cosmofs_stats(struct mount *mnt, struct cosmofs_stats *out)
{
    struct cfs *fs = cfs_of(mnt);
    if (fs == NULL)
        return -EINVAL;
    mutex_lock(&fs->lock);
    out->generation = fs->sb.generation;
    out->free_blocks = fs->free_blocks;
    out->total_blocks = fs->sb.total_blocks;
    out->inode_count = fs->sb.inode_count;
    out->version = fs->sb.version;
    out->free_root = fs->sb.version >= 9 ? fs->sb.free_root : 0;
    out->orphan_root = fs->sb.version >= 10 ? fs->sb.orphan_root : 0;
    out->pending_orphans = fs->nr_orphans;
    out->dirty_buffers = fs->nr_dirty;
    out->pending_frees = fs->nr_pending + fs->nr_exempt;
    out->reserve_blocks = fs->reserve;
    out->commits = fs->commits;
    out->wb_commits = fs->wb_commits;
    out->csum_failures = fs->csum_failures;
    out->members = fs->nmembers;
    out->devices = 0;
    for (unsigned v = 0; v < fs->nmembers; v++)
        out->devices += pool_copies(fs->pool, CFS_DVA(v, 0));
    out->repairs = fs->repairs;
    out->degraded = fs->degraded;
    mutex_unlock(&fs->lock);
    return 0;
}

void cosmofs_test_fail_freelog(struct mount *mnt, bool on)
{
    struct cfs *fs = cfs_of(mnt);
    if (fs)
        fs->test_fail_freelog = on;
}

void cosmofs_test_fail_snapfill(struct mount *mnt, bool on)
{
    struct cfs *fs = cfs_of(mnt);
    if (fs)
        fs->test_fail_snapfill = on;
}

/*
 * Put an inode into the orphan set by hand, so that the next commit
 * writes a record naming an inode that is still linked. Nothing else can
 * build that image: it is what a record whose checksum is good and whose
 * contents are wrong looks like, and the replay must refuse to act on it
 * rather than delete a file somebody is using.
 */
void cosmofs_test_orphan_add(struct mount *mnt, uint64_t ino)
{
    struct cfs *fs = cfs_of(mnt);
    if (fs == NULL)
        return;
    mutex_lock(&fs->lock);
    if (cfs_orphan_reserve(fs))
        cfs_orphan_add(fs, ino);
    mutex_unlock(&fs->lock);
}

void cosmofs_test_fail_orphan(struct mount *mnt, bool on)
{
    struct cfs *fs = cfs_of(mnt);
    if (fs)
        fs->test_fail_orphan = on;
}

uint64_t cosmofs_test_snap_walks(struct mount *mnt)
{
    struct cfs *fs = cfs_of(mnt);
    return fs ? fs->snap_walks : 0;
}

uint64_t cosmofs_test_snap_verdicts(struct mount *mnt)
{
    struct cfs *fs = cfs_of(mnt);
    return fs ? fs->snap_verdicts : 0;
}

void cosmofs_test_discard_on_unmount(struct mount *mnt, bool discard)
{
    struct cfs *fs = cfs_of(mnt);
    if (fs)
        fs->discard_on_unmount = discard;
}

int cosmofs_test_unlock(struct mount *mnt, const void *key, size_t len)
{
    struct cfs *fs = cfs_of(mnt);
    if (fs == NULL)
        return -EINVAL;
    if (!fs->encrypted)
        return -ENOTSUP;
    mutex_lock(&fs->lock);
    int rc = fs->have_key ? 0 : cfs_keys_load(fs, key, len);
    mutex_unlock(&fs->lock);
    return rc;
}

int cosmofs_rekey(struct mount *mnt, const void *key, size_t len)
{
    struct cfs *fs = cfs_of(mnt);
    if (fs == NULL)
        return -EINVAL;
    if (!fs->encrypted)
        return -ENOTSUP;
    mutex_lock(&fs->lock);
    int rc = cfs_keys_rotate(fs, key, len);
    mutex_unlock(&fs->lock);
    return rc;
}

int cosmofs_test_block_of(struct mount *mnt, uint64_t ino, uint64_t lblk, uint64_t *dva)
{
    struct cfs *fs = cfs_of(mnt);
    if (fs == NULL)
        return -EINVAL;
    mutex_lock(&fs->lock);
    struct cfs_inode in;
    int rc = cfs_inode_read(fs, ino, &in);
    if (rc == 0) {
        uint64_t pblk = 0;
        rc = cfs_map(fs, &in, lblk, &pblk);
        if (rc == 1) {
            *dva = pblk;
            rc = 0;
        } else if (rc == 0) {
            rc = -ENOENT;   /* a hole */
        }
    }
    mutex_unlock(&fs->lock);
    return rc;
}

uint64_t cosmofs_test_deadlist_len(struct mount *mnt, uint64_t of)
{
    struct cfs *fs = cfs_of(mnt);
    if (fs == NULL)
        return 0;
    mutex_lock(&fs->lock);
    uint64_t n = cfs_snapshot_deadlist_len(fs, of);
    mutex_unlock(&fs->lock);
    return n;
}

uint64_t cosmofs_test_deadlist_dups(struct mount *mnt, uint64_t *examined, uint64_t *first)
{
    if (examined != NULL)
        *examined = 0;
    if (first != NULL)
        *first = 0;
    struct cfs *fs = cfs_of(mnt);
    if (fs == NULL)
        return 0;
    mutex_lock(&fs->lock);
    uint64_t n = cfs_snapshot_deadlist_dups(fs, examined, first);
    mutex_unlock(&fs->lock);
    return n;
}

uint64_t cosmofs_test_snap_root(struct mount *mnt)
{
    struct cfs *fs = cfs_of(mnt);
    if (fs == NULL)
        return 0;
    mutex_lock(&fs->lock);
    uint64_t r = fs->sb.snap_root;
    mutex_unlock(&fs->lock);
    return r;
}

uint64_t cosmofs_test_member_free(struct mount *mnt, unsigned vdev)
{
    struct cfs *fs = cfs_of(mnt);
    if (fs == NULL || vdev >= fs->nmembers)
        return UINT64_MAX;
    mutex_lock(&fs->lock);
    uint64_t n = fs->mem[vdev].free_blocks;
    mutex_unlock(&fs->lock);
    return n;
}

void cosmofs_test_set_writeback(struct mount *mnt, bool on)
{
    struct cfs *fs = cfs_of(mnt);
    if (fs)
        fs->wb_enabled = on;
}

void cosmofs_test_set_writeback_interval(struct mount *mnt, unsigned ms)
{
    struct cfs *fs = cfs_of(mnt);
    if (fs)
        fs->wb_interval_ms = ms;
}

struct fs_type cosmofs_fs_type = {
    .name = "cosmofs",
    .mount = cosmofs_mount,
    .unmount = cosmofs_unmount,
    .sync = cosmofs_sync,
    /* The maintenance passes, reachable from /dev/fsctl. Until this the
     * only caller of either was a self-test. */
    .check = cosmofs_check,
    .scrub = cosmofs_scrub,
};

void cosmofs_init(void)
{
    if (vfs_register_fs(&cosmofs_fs_type))
        panic("cosmofs: cannot register");
}
