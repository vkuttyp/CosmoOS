/*
 * cosmofs_commit.c - The commit: the bitmap's reserve-then-write fixpoint,
 * the root write, the record of what a transaction freed and of what a
 * root still owes (and their replay at mount), the commit's reservation,
 * and the commit itself in the order that keeps a crash at any point
 * consistent. Moved out of cosmofs_core.c (roadmap 2.0 SA7) so that it can
 * be held to a digest and made CeeSafe on its own.
 *
 * Rules (docs/kernel-services/vfs/design.md): the in-memory bitmap is
 * authoritative during a transaction and its chunks are written at commit
 * through a reserve-then-write fixpoint; blocks freed in a transaction
 * become allocatable only after that transaction's root is on disk.
 */

#include <kernel/blk.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/list.h>
#include <kernel/log.h>
#include <kernel/storage.h>
#include <kernel/string.h>
#include <kernel/thread.h>

#include "cosmofs_internal.h"

static uint64_t *ptrs(struct cfs_buf *b)
{
    return (uint64_t *)(b->data + CFS_MHDR_SIZE);
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
        struct cfs_buf *nb = cfs_buf_alloc(fs, reserved[gc]);
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
        cfs_mhdr_seal(fs, nb->data, CFS_KIND_BITMAP, reserved[gc]);
        cfs_buf_mark_dirty(fs, nb);
        cfs_buf_put(fs, nb);
        if (slots[c])
            cfs_free_block_deferred(fs, slots[c]);
        slots[c] = reserved[gc];
        reserved[gc] = 0;   /* used: commit_bitmap gives back only the rest */
        fs->bitmap_dirty[gc] = 0;
    }
    /* Re-seal the index (it was CoW'd or dirtied above). */
    cfs_mhdr_seal(fs, idx->data, CFS_KIND_ALLOCIDX, idx->blkno);
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
    if (rc) {
        /* Reserved and never written: nothing names these blocks, and
         * left allocated they are a leak the next commit's bitmap makes
         * durable. Given back deferred, like every rolled-back
         * allocation (CF17). */
        for (unsigned gc = 0; gc < fs->nr_chunks; gc++)
            if (reserved[gc] != 0)
                cfs_free_block_deferred(fs, reserved[gc]);
    }
    kfree(reserved);
    return rc;
}

/* The root write: flushed before (every block of the transaction is
 * stable first) and durable when it completes (BIO_PREFLUSH | BIO_FUA). */
int cfs_super_write(struct cfs *fs, unsigned slot, unsigned flags)
{
    uint8_t *block = kmalloc(CFS_BLOCK, KMEM_ZERO);
    if (block == NULL)
        return -ENOMEM;
    struct cfs_super *sb = (struct cfs_super *)block;
    *sb = fs->sb;
    cfs_super_seal(&fs->mac, block);
    /* ONLINE copies only: a copy being resilvered gets its superblocks
     * when it holds everything they name (design.md, "Resilvering"). */
    int rc = pool_write_flags(fs->pool, slot, block, flags | POOL_WRITE_ONLINE);
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
int cfs_freelog_replay(struct cfs *fs)
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
        if (!cfs_freelog_count_ok(count)) {
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
            if (lin == CFS_DVA_NONE || !cfs_bit_test(fs->bitmap, lin))
                continue;                      /* a commit already applied it */
            cfs_bit_clear(fs->bitmap, lin);
            fs->bitmap_dirty[lin / CFS_BITS_PER_BITMAP] = 1;
            fs->free_blocks++;
            fs->mem[CFS_DVA_VDEV(d->blk[i])].free_blocks++;
            applied++;
        }
        cfs_buf_put(fs, b);
        at = next;
    }
    fs->replay_freed = applied;
    if (applied && !fs->rdonly)
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
    for (unsigned i = 0; i < need; i++) {
        struct cfs_buf *b = cfs_buf_alloc(fs, blk[i]);
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
            if (lin == CFS_DVA_NONE || !cfs_bit_test(fs->bitmap, lin))
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
            unsigned x;
            for (x = need; x < n && k < CFS_FREELOG_PER_BLOCK; x++)
                d->blk[k++] = blk[x];
            if (x < n) {
                /* The arithmetic above says this cannot happen. If it
                 * ever does, a block would be freed in memory and named
                 * by nothing, so refuse the commit instead. */
                kerror("cosmofs: free record has no room for %u leftover block(s)", n - x);
                cfs_buf_mark_clean(fs, b);
                cfs_buf_put(fs, b);
                return -EIO;
            }
        }
        d->count = k;
        cfs_mhdr_seal(fs, b->data, CFS_KIND_FREELOG, blk[i]);
        cfs_buf_mark_dirty(fs, b);
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
        cfs_mhdr_seal(fs, b->data, CFS_KIND_ORPHAN, blk);
        cfs_buf_mark_dirty(fs, b);
        cfs_buf_put(fs, b);
        /* Chain forward: the block before this one names it, and it is
         * still dirty and unwritten, so the edit reaches the disk. */
        if (prev != 0) {
            struct cfs_buf *pb = cfs_buf_find(fs, prev);
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
int cfs_orphan_replay(struct cfs *fs)
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
        if (!cfs_orphan_count_ok(count)) {
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
            /*
             * From the truncate on, a failure cannot be skipped.
             * cfs_truncate_blocks frees every extent into pending_free
             * and *then* stores the shortened extent list, so a failure
             * leaves the blocks queued to be freed while the inode still
             * names them -- and the first commit would publish that. The
             * first version of this skipped such an inode under the
             * comment "nothing changed for it", which was not true.
             *
             * Nothing has been published at this point, so failing the
             * mount discards the whole transaction and leaves the
             * filesystem exactly as it was found.
             */
            int trc = cfs_truncate_blocks(fs, &in, 0);
            if (trc != 0) {
                kerror("cosmofs: orphan inode %llu could not be released (%d); refusing the mount",
                       (unsigned long long)inos[i], trc);
                kfree(inos);
                return trc;
            }
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
    fs->replay_orphans = applied;
    if ((applied || gone) && !fs->rdonly)
        kinfo("cosmofs: reclaimed %u inode(s) unlinked with a handle open (%u already gone)", applied, gone);
    if (suspect)
        kerror("cosmofs: the orphan record named %u inode(s) that still have links; run the structural check",
               suspect);
    return 0;
}

/*
 * A commit that fails after cfs_snapshot_fill added deadlist entries
 * cannot be retried: the entries stay in the open transaction, and the
 * next attempt's fill would add the same blocks to the same list again
 * (cfs_snapshot_deadlist_dups). The transaction is abandoned instead,
 * as the fill itself does when it fails partway (CF17).
 */
static int commit_unfilled(struct cfs *fs, int rc, unsigned appended)
{
    if (appended > 0)
        cfs_fail(fs, rc);
    return rc;
}

/* The state blocks again, sealed and written: after a copy changed state
 * during the commit that had already written them (cfs_commit_ex). */
static int copystate_rewrite(struct cfs *fs)
{
    int rc = cfs_copystate_fill(fs);
    for (unsigned v = 0; v < fs->nmembers && rc == 0; v++) {
        if (fs->mem[v].state_root == 0)
            continue;
        struct cfs_buf *b = cfs_buf_find(fs, fs->mem[v].state_root);
        if (b == NULL)
            return -EIO;   /* prepare made it dirty in this generation; it is cached */
        cfs_mhdr_seal(fs, b->data, CFS_KIND_COPYSTATE, b->blkno);
        rc = pool_write(fs->pool, b->blkno, b->data);
    }
    return rc;
}

/*
 * `published` (optional) says whether *this call* put a new root on
 * disk. It is not derivable from the generation number: a commit that
 * fails after `fs->sb.generation = fs->gen` -- at the label update, the
 * flush or the superblock write, all three below -- leaves the number
 * advanced and `fs->gen` where it was, because only success reaches the
 * `fs->gen++` at the end. A later retry then republishes the same
 * number, and a caller comparing before against after would see no
 * change and conclude nothing was published. Nor is `rc == 0` enough:
 * the early return below is "there was nothing to commit".
 */
int cfs_commit_ex(struct cfs *fs, bool *published)
{
    if (published)
        *published = false;
    if (fs->failed)
        return fs->failed;
    /* A read-only mount writes nothing (CFS-1): what its replay changed
     * stays in memory, and a writable mount replays it again. */
    if (fs->rdonly)
        return 0;
    if (fs->nr_dirty == 0 && fs->nr_pending == 0 && fs->nr_exempt == 0) {
        /* A copy that changed state is something to record, with or
         * without a change to the tree (design.md, "Format version 13"). */
        bool any = fs->pool->state_seq != fs->copystate_seq;
        for (unsigned c = 0; c < fs->nr_chunks; c++)
            any = any || fs->bitmap_dirty[c];
        if (!any)
            return 0;
    }
#if CONFIG_DEBUG
    /* Test hook: the machine "stops" at this commit. */
    if (fs->test_commit_armed) {
        if (fs->test_commits_left == 0) {
            cfs_fail(fs, -EIO);
            return -EIO;
        }
        fs->test_commits_left--;
    }
#endif
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
    /* The copy state's blocks, and the raise to 13 the first of them
     * needs: before anything is reserved, because they take blocks and a
     * raise rewrites the snapshot list (design.md, "The record, written
     * by the commit"). */
    int rc = cfs_copystate_prepare(fs);
    if (rc)
        return rc;
    record_frees = fs->sb.version >= 9;
    /* The clones' roots into their entries, on a copy of the list: before
     * anything below allocates past the fixpoint. */
    rc = cfs_clones_publish(fs);
    if (rc)
        return rc;
    struct cfs_res res = { .blk = NULL, .n = 0, .used = 0 };
    bool *held = NULL;
    unsigned appended = 0;   /* deadlist entries this attempt added: a retry would add them again */
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
            rc = cfs_snapshot_fill(fs, &res, held, held_n, &appended);
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
            struct cfs_buf *b = cfs_buf_find(fs, res.blk[i]);
            if (b != NULL)
                cfs_buf_mark_clean(fs, b);
            cfs_free_block_deferred(fs, res.blk[i]);
        }
        fs->sb.free_root = 0;
        fs->sb.orphan_root = 0;
    }
    kfree(res.blk);
    if (rc) {
        kfree(held);
        return commit_unfilled(fs, rc, appended);
    }

    /* What each copy that will miss this commit missed, this commit
     * included: the record is the last metadata to change (CF25). */
    rc = cfs_copystate_fill(fs);
    if (rc) {
        kfree(held);
        return commit_unfilled(fs, rc, appended);
    }
    uint64_t filled_seq = fs->pool->state_seq;

    /* Every dirty metadata block, re-sealed with its final contents. */
    struct cfs_buf *b;
    list_for_each_entry(b, &fs->bufs, link) {
        if (!b->dirty)
            continue;
        struct cfs_mhdr *h = cfs_buf_hdr(b);
        cfs_mhdr_seal(fs, b->data, h->kind, b->blkno);
        rc = pool_write(fs->pool, b->blkno, b->data);
        if (rc) {
            kfree(held);
            return commit_unfilled(fs, rc, appended);
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
        return commit_unfilled(fs, rc, appended);
    }
    /* Now make all of it stable, on every device. The root write's own
     * preflush reaches only the devices carrying the superblock, which
     * is member 0's; a root that names blocks still sitting in another
     * member's write cache is a root that can outlive them. This is not
     * about labels alone -- it has been true of every block on another
     * member since a pool could have more than one. */
    /* A copy that died while those were written is one the records just
     * written do not describe: write them again -- they are this
     * generation's blocks -- and flush again, until the state holds
     * still (CF25). Each round takes a copy out, so it ends. */
    for (unsigned round = 0;; round++) {
        if (fs->pool->state_seq != filled_seq) {
            rc = copystate_rewrite(fs);
            filled_seq = fs->pool->state_seq;
            if (rc) {
                kfree(held);
                return commit_unfilled(fs, rc, appended);
            }
        }
        rc = pool_flush(fs->pool);
        if (rc) {
            kfree(held);
            return commit_unfilled(fs, rc, appended);
        }
        if (fs->pool->state_seq == filled_seq || round >= 2 * POOL_MAX_COPIES)
            break;
    }
#if CONFIG_COSMOFS_STOP
    /* test-snapshot's crash point "during the switch": every block of the
     * rollback's transaction is written and flushed, its root is not. A
     * build with COSMOFS_STOP=1 only; the harness kills the machine here. */
    if (fs->switching) {
        kinfo("cosmofs: test stop: the rollback switch's blocks are written and its root is not");
        for (;;)
            thread_sleep_ms(1000);
    }
#endif
    unsigned slot = fs->sb_slot == CFS_SUPER_A ? CFS_SUPER_B : CFS_SUPER_A;
    rc = cfs_super_write(fs, slot, BIO_PREFLUSH | BIO_FUA);
    if (rc) {
        kfree(held);
        return commit_unfilled(fs, rc, appended);
    }
    fs->sb_slot = slot;
    fs->commits++;
    fs->first_dirty_ns = 0;
    pool_commit_done(fs->pool, fs->sb.generation);
    if (cfs_copystate_recorded(fs))
        fs->copystate_seq = filled_seq;

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
        if (lin == CFS_DVA_NONE || !cfs_bit_test(fs->bitmap, lin))
            continue;
        cfs_bit_clear(fs->bitmap, lin);
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
        if (lin == CFS_DVA_NONE || !cfs_bit_test(fs->bitmap, lin))
            continue;
        cfs_bit_clear(fs->bitmap, lin);
        fs->bitmap_dirty[lin / CFS_BITS_PER_BITMAP] = 1;
        fs->free_blocks++;
        fs->mem[CFS_DVA_VDEV(dva)].free_blocks++;
    }
    fs->nr_exempt = 0;
    fs->gen++;
    /* The frees dirtied bitmap chunks for the next commit; that is
     * bookkeeping of this commit, not a new change to age. */
    fs->first_dirty_ns = 0;
    /* A copy whose state this commit could not record is a commit still
     * due, whether or not anything else is written (Codex on #439): the
     * writeback thread commits only what is marked dirty. */
    if (fs->pool->state_seq != fs->copystate_seq)
        cfs_note_dirty(fs);
    kdebug("cosmofs: committed generation %llu (%llu free blocks)", (unsigned long long)fs->sb.generation,
           (unsigned long long)fs->free_blocks);
    if (published)
        *published = true;
    return 0;
}

int cfs_commit(struct cfs *fs)
{
    /* A commit publishes the live tree's roots and every clone's: an
     * operation inside a clone that commits (fsync) leaves it first. */
    struct cfs_clone *c = fs->cur_clone;
    if (c != NULL)
        cfs_clone_leave(fs);
    int rc = cfs_commit_ex(fs, NULL);
    if (c != NULL)
        cfs_clone_enter(fs, c);
    return rc;
}
