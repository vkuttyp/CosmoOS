/*
 * cosmofs_alloc.c - Allocation and the inode map: the in-memory bitmap's
 * first-fit runs across a pool's members and the metadata reserve, the
 * lists of blocks the open transaction frees (deferred and exempt) and of
 * inodes it orphans, and the two-level map from an inode number to its
 * slot. Moved out of cosmofs_core.c (roadmap 2.0 SA7) so that it can be
 * held to a digest and made CeeSafe on its own.
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/string.h>

#include "cosmofs_internal.h"

/* --- allocation ------------------------------------------------------------ */

bool cfs_bitmap_test(const struct cfs *fs, uint64_t lin)
{
    return lin < fs->nblocks && (fs->bitmap[lin / 8] & (1u << (lin % 8))) != 0;
}


/* The first free run in blocks [lo, hi) of member `v`, claimed. `hi` is
 * never past the member's last block, so the padding at the end of its
 * last bitmap chunk is unreachable. */
static bool scan_member(struct cfs *fs, unsigned v, uint64_t lo, uint64_t hi, uint32_t want, uint64_t *start,
                        uint64_t *got)
{
    struct cfs_memstate *m = &fs->mem[v];
    for (uint64_t b = lo; b < hi; b++) {
        if (cfs_bit_test(fs->bitmap, m->base + b))
            continue;
        uint64_t len = 1;
        while (len < want && b + len < hi && !cfs_bit_test(fs->bitmap, m->base + b + len))
            len++;
        for (uint64_t k = 0; k < len; k++) {
            cfs_bit_set(fs->bitmap, m->base + b + k);
            fs->bitmap_dirty[m->chunk0 + (b + k) / CFS_BITS_PER_BITMAP] = 1;
        }
        fs->free_blocks -= len;
        m->free_blocks -= len;
        m->alloc_hint = b + len;
        fs->alloc_hint = m->base + b + len;
        cfs_note_dirty(fs);
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
 * consecutive free blocks. Data allocations stop at half the reserve so
 * deletion and commit always have metadata blocks (design.md,
 * "Allocation: contiguity and the metadata reserve"). The whole reserve
 * is kept at the write, where a dirty page is charged (cfs_dirty_check,
 * CF34); the metadata a transaction takes after that charge comes from
 * the same free blocks, so the write-back of a page charged may find
 * them under the reserve, and takes from its upper half. A run never
 * crosses a member: an extent's `count` counts blocks on one device
 * (design.md, "One bitmap, many members"). */
int cfs_alloc_run(struct cfs *fs, enum cfs_alloc_class cls, uint64_t hint, uint32_t want, uint64_t *start,
                  uint64_t *got)
{
    uint64_t floor = cls == CFS_ALLOC_DATA ? fs->reserve / 2 : 0;
    uint64_t usable = fs->free_blocks > floor ? fs->free_blocks - floor : 0;
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
    cfs_note_dirty(fs);
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
    /* The orphan record is the live tree's. A clone's unlinked-while-open
     * inode is not recorded: its blocks are the clone's, and deleting the
     * clone walks its whole tree, names or not (design.md, "Clones"). */
    if (fs->cur_clone != NULL)
        return true;
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
    if (fs->cur_clone != NULL)
        return;
    if (ino == 0 || cfs_orphan_named(fs, ino))
        return;
    if (fs->nr_orphans == fs->orphan_cap) {
        kerror("cosmofs: orphan record has no room for inode %llu; it was not reserved",
               (unsigned long long)ino);
        cfs_fail(fs, -ENOSPC);   /* rather than commit a deletion nothing records */
        return;
    }
    fs->orphans[fs->nr_orphans++] = ino;
    cfs_note_dirty(fs);
}

void cfs_orphan_remove(struct cfs *fs, uint64_t ino)
{
    if (fs->cur_clone != NULL)
        return;
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
    /* A clone's frees are not the live tree's: no snapshot of the live
     * tree is asked about them (design.md, "Clones"). */
    if (fs->cur_clone != NULL) {
        cfs_clone_free(fs, blk);
        return;
    }
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
    cfs_note_dirty(fs);
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
    if (!cfs_inode_slot_ok(out, ino))
        return -ENOENT;
    return 0;
}

/* The slot as it is, links or no links. The ordinary read reports an
 * inode with no links as absent, which is right for a lookup and wrong
 * for the structural check: an inode with no links and blocks still
 * allocated is exactly what it is looking for. */
int cfs_inode_read_raw(struct cfs *fs, uint64_t ino, struct cfs_inode *out)
{
    ino = CFS_INO_OF(ino);   /* a clone's vnode carries its tag (design.md, "Clones") */
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
    ino = CFS_INO_OF(ino);
    if (ino == 0 || ino >= fs->sb.next_ino)
        return -ENOENT;
    struct cfs_buf *ib;
    int rc = inode_block(fs, ino, false, false, &ib);
    if (rc)
        return rc;
    memcpy(out, inode_slot(ib, ino), sizeof(*out));
    cfs_buf_put(fs, ib);
    if (!cfs_inode_slot_ok(out, ino))
        return -ENOENT;   /* free slot */
    return 0;
}

int cfs_inode_write(struct cfs *fs, uint64_t ino, const struct cfs_inode *in)
{
    /* A tagged number names one tree: it is written only while that tree
     * is the one entered. An untagged one is the entered tree's own. A
     * snapshot's vnode, or a clone's written outside its clone, would
     * otherwise land on the live inode with the same number. */
    unsigned tag = CFS_SNAP_TAG(ino);
    if (tag != 0 && (fs->cur_clone == NULL || fs->cur_clone->id != tag)) {
        kerror("cosmofs: inode %llu of tree %u written outside that tree; refused",
               (unsigned long long)CFS_INO_OF(ino), tag);
        return -EROFS;
    }
    ino = CFS_INO_OF(ino);
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

/*
 * Make an inode's map path writable now, before an operation changes
 * anything (CFS-6): every block from the root to the inode's slot is
 * copied into this transaction, so the inode write that ends the
 * operation only writes into dirty buffers -- which stay cached -- and
 * cannot fail. The copies say what the old blocks said, so an operation
 * that stops after this leaves nothing a commit should not publish.
 */
int cfs_inode_prepare(struct cfs *fs, uint64_t ino)
{
    unsigned tag = CFS_SNAP_TAG(ino);
    if (tag != 0 && (fs->cur_clone == NULL || fs->cur_clone->id != tag))
        return -EROFS;   /* as cfs_inode_write refuses it */
    struct cfs_buf *ib;
    int rc = inode_block(fs, CFS_INO_OF(ino), true, true, &ib);
    if (rc == 0)
        cfs_buf_put(fs, ib);
    return rc;
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
