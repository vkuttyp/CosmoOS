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
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/panic.h>
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
    int rc = pool_read(fs->pool, blkno, b->data);
    if (rc == 0)
        rc = mhdr_check(b->data, blkno, kind);
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

int cfs_buf_cow(struct cfs *fs, struct cfs_buf **bp, uint64_t *parent_slot)
{
    struct cfs_buf *b = *bp;
    struct cfs_mhdr *h = cfs_buf_hdr(b);
    if (h->generation == fs->gen) {
        buf_mark_dirty(fs, b);
        return 0;
    }
    /* The copy stays on the member the block was on where that member
     * has room. Otherwise a metadata tree drifts onto whichever member
     * is emptiest, and a member's own allocation index could end up on
     * a different device than the blocks it describes. */
    unsigned v = CFS_DVA_VDEV(b->blkno);
    uint64_t hint = v < fs->nmembers ? CFS_DVA(v, fs->mem[v].first_usable) : 0;
    uint64_t nblk, got;
    int rc = cfs_alloc_run(fs, CFS_ALLOC_META, hint, 1, &nblk, &got);
    if (rc)
        return rc;
    struct cfs_buf *nb = buf_alloc(fs, nblk);
    if (nb == NULL) {
        cfs_free_block_deferred(fs, nblk);
        return -ENOMEM;
    }
    memcpy(nb->data, b->data, CFS_BLOCK);
    mhdr_seal(fs, nb->data, h->kind, nblk);
    buf_mark_dirty(fs, nb);
    cfs_free_block_deferred(fs, b->blkno);
    cfs_buf_put(fs, b);
    *parent_slot = nblk;
    *bp = nb;
    return 0;
}

/* --- allocation ------------------------------------------------------------ */

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

int cfs_commit(struct cfs *fs)
{
    if (fs->failed)
        return fs->failed;
    if (fs->nr_dirty == 0 && fs->nr_pending == 0) {
        bool any = false;
        for (unsigned c = 0; c < fs->nr_chunks; c++)
            any = any || fs->bitmap_dirty[c];
        if (!any)
            return 0;
    }
    int rc = commit_bitmap(fs);
    if (rc)
        return rc;

    /* Every dirty metadata block, re-sealed with its final contents. */
    struct cfs_buf *b;
    list_for_each_entry(b, &fs->bufs, link) {
        if (!b->dirty)
            continue;
        struct cfs_mhdr *h = cfs_buf_hdr(b);
        mhdr_seal(fs, b->data, h->kind, b->blkno);
        rc = pool_write(fs->pool, b->blkno, b->data);
        if (rc)
            return rc;
    }

    /* The root, into the other slot, flushed before and after (one call:
     * the block layer runs flush, write, flush). free_blocks excludes
     * this transaction's pending frees: they are still referenced by the
     * previous root until this one is on disk. */
    fs->sb.generation = fs->gen;
    fs->sb.free_blocks = fs->free_blocks;
    unsigned slot = fs->sb_slot == CFS_SUPER_A ? CFS_SUPER_B : CFS_SUPER_A;
    rc = super_write(fs, slot, BIO_PREFLUSH | BIO_FUA);
    if (rc)
        return rc;
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
     * (design.md, "Not freeing what a snapshot names"). */
    bool snapshots = fs->snap_count > 0;
    for (unsigned i = 0; i < fs->nr_pending; i++) {
        uint64_t dva = fs->pending_free[i];
        uint64_t lin = cfs_dva_lin(fs, dva);
        if (lin == CFS_DVA_NONE || !bit_test(fs->bitmap, lin))
            continue;
        if (snapshots && cfs_snapshot_hold_block(fs, dva))
            continue;
        bit_clear(fs->bitmap, lin);
        fs->bitmap_dirty[lin / CFS_BITS_PER_BITMAP] = 1;
        fs->free_blocks++;
        fs->mem[CFS_DVA_VDEV(dva)].free_blocks++;
    }
    fs->nr_pending = 0;
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
static int format_at(struct blkdev **bd, unsigned n, unsigned version)
{
    if (n == 0 || n > CFS_MAX_MEMBERS)
        return -EINVAL;
    if (version < 4 && n != 1)
        return -EINVAL;   /* one member is all the older formats can name */
    struct spool *pool;
    int rc = pool_open(bd[0], &pool);
    if (rc)
        return rc;
    for (unsigned v = 1; v < n && rc == 0; v++)
        rc = pool_add_member(pool, bd[v], NULL);
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
        mem[v].first_usable = v == 0 ? 2 : 1;
        total += nb;
    }

    /* Member 0: 0,1 superblocks; 2 member table; 3 alloc index; 4..
     * bitmaps; then imap L1, imap L0, inode block 0.
     * Member v>0: 0 label; 1 alloc index; 2.. bitmaps. */
    unsigned chunks0 = (unsigned)((mem[0].nblocks + CFS_BITS_PER_BITMAP - 1) / CFS_BITS_PER_BITMAP);
    bool v4 = version >= 4;
    /* Before version 4 there is no member table, and the allocation
     * index is at block 2 where that format put it. */
    uint64_t members_blk = 2, alloc_idx = v4 ? 3 : 2, bitmap0 = v4 ? 4 : 3;
    uint64_t imap1 = bitmap0 + chunks0, imap0 = imap1 + 1, inodes0 = imap0 + 1;
    uint64_t used0 = inodes0 + 1;

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
        if (rc == 0 && v > 0)
            rc = cfs_label_write(pool, v, uuid, nb);
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
        root->csum_algo = CFS_CSUM_CRC32C;
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
    return format_at(bd, n, CFS_VERSION);
}

int cosmofs_format(struct blkdev *bd)
{
    return format_at(&bd, 1, CFS_VERSION);
}

/* Test hook: write an older format, so that "versions 2 and 3 mount
 * unchanged" is a claim something checks (design.md, "The DVA is 64
 * bits"). */
int cosmofs_test_format_version(struct blkdev *bd, unsigned version)
{
    if (version < CFS_VERSION_MIN || version > CFS_VERSION)
        return -EINVAL;
    return format_at(&bd, 1, version);
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
    if (free != fs->sb.free_blocks)
        kwarn("cosmofs: free block count %llu differs from the superblock's %llu; using the bitmap",
              (unsigned long long)free, (unsigned long long)fs->sb.free_blocks);
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
    int rc = cfs_members_load(fs, fs->pool->m[0].dev);
    if (rc)
        return rc;
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
    out->dirty_buffers = fs->nr_dirty;
    out->pending_frees = fs->nr_pending;
    out->reserve_blocks = fs->reserve;
    out->commits = fs->commits;
    out->wb_commits = fs->wb_commits;
    out->csum_failures = fs->csum_failures;
    out->members = fs->nmembers;
    mutex_unlock(&fs->lock);
    return 0;
}

void cosmofs_test_discard_on_unmount(struct mount *mnt, bool discard)
{
    struct cfs *fs = cfs_of(mnt);
    if (fs)
        fs->discard_on_unmount = discard;
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
};

void cosmofs_init(void)
{
    if (vfs_register_fs(&cosmofs_fs_type))
        panic("cosmofs: cannot register");
}
