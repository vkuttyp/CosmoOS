/*
 * cosmofs_snap.c - Snapshots
 * (docs/kernel-services/filesystem/cosmofs/design.md, "Format version 3").
 *
 * A snapshot is the tuple a commit publishes -- imap_root, alloc_root,
 * next_ino, inode_count and the generation -- kept, plus a promise not
 * to free what it still names. Nothing is copied to take one: every
 * tree it points at is already copy-on-write.
 *
 * While a snapshot exists, the commit's release loop appends the blocks
 * it would have freed to the newest snapshot's deadlist instead of
 * clearing their bitmap bits, because a block reaches pending_free
 * exactly when the previous tree named it and the new one does not.
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/string.h>
#include <kernel/timer.h>

#include "cosmofs_internal.h"

/* --- the snapshot list ---------------------------------------------------- */

static struct cfs_snap_block *snap_payload(struct cfs_buf *b)
{
    return (struct cfs_snap_block *)(b->data + CFS_MHDR_SIZE);
}

static struct cfs_dead_block *dead_payload(struct cfs_buf *b)
{
    return (struct cfs_dead_block *)(b->data + CFS_MHDR_SIZE);
}

/* Walk the list, calling `fn` for every live entry until it returns
 * false. `fn` may modify the entry; the block is marked dirty then. */
static int snap_walk(struct cfs *fs, bool (*fn)(struct cfs_snapshot *s, void *arg), void *arg, bool write)
{
    uint64_t blkno = fs->sb.snap_root;
    while (blkno) {
        struct cfs_buf *b;
        int rc = cfs_buf_get(fs, blkno, CFS_KIND_SNAPLIST, &b);
        if (rc)
            return rc;
        struct cfs_snap_block *sb = snap_payload(b);
        uint64_t next = sb->next;
        for (unsigned i = 0; i < CFS_SNAPS_PER_BLOCK; i++) {
            if (sb->snap[i].name[0] == '\0')
                continue;
            if (!fn(&sb->snap[i], arg)) {
                if (write)
                    cfs_buf_mark_dirty(fs, b);
                cfs_buf_put(fs, b);
                return 0;
            }
        }
        cfs_buf_put(fs, b);
        blkno = next;
    }
    return 0;
}

struct find_ctx {
    const char *name;
    struct cfs_snapshot found;
    bool ok;
};

static bool find_one(struct cfs_snapshot *s, void *arg)
{
    struct find_ctx *c = arg;
    if (strcmp(s->name, c->name) == 0) {
        c->found = *s;
        c->ok = true;
        return false;
    }
    return true;
}

bool cfs_snapshot_find(struct cfs *fs, const char *name, struct cfs_snapshot *out)
{
    struct find_ctx c = { .name = name, .ok = false };
    if (snap_walk(fs, find_one, &c, false))
        return false;
    if (c.ok && out)
        *out = c.found;
    return c.ok;
}

struct count_ctx {
    struct cfs_snapshot *out;
    unsigned max, n;
    uint64_t newest_gen;
};

static bool collect(struct cfs_snapshot *s, void *arg)
{
    struct count_ctx *c = arg;
    if (c->out && c->n < c->max)
        c->out[c->n] = *s;
    if (s->generation >= c->newest_gen)
        c->newest_gen = s->generation;
    c->n++;
    return true;
}

int cfs_snapshot_list(struct cfs *fs, struct cfs_snapshot *out, unsigned max, unsigned *count)
{
    struct count_ctx c = { .out = out, .max = max, .n = 0, .newest_gen = 0 };
    int rc = snap_walk(fs, collect, &c, false);
    if (rc)
        return rc;
    *count = c.n;
    return 0;
}

bool cfs_has_snapshots(struct cfs *fs)
{
    unsigned n = 0;
    if (cfs_snapshot_list(fs, NULL, 0, &n))
        return false;
    return n > 0;
}

/* --- what a snapshot references ------------------------------------------- */

/* A snapshot's bitmap is the set of blocks its tree occupies: the
 * allocator's bit is set for a block exactly while something reaches it,
 * and a commit publishes the two together. So "does this snapshot still
 * name that block" is one bitmap lookup in the tree the snapshot already
 * recorded -- no birth times, no reference counts
 * (design.md, "Not freeing what a snapshot names"). */
/* The ALLOCIDX root covering `vdev` in the tree a snapshot recorded:
 * from version 4 that record is the member table of its generation, so
 * each member's bitmap is found through the table the snapshot pinned;
 * before it there was one member and one index. */
static bool snap_alloc_root(struct cfs *fs, const struct cfs_snapshot *s, unsigned vdev, uint64_t *out, bool *unknown)
{
    *unknown = false;
    if (fs->sb.version < 4) {
        if (vdev != 0)
            return false;
        *out = s->alloc_root;
        return s->alloc_root != 0;
    }
    struct cfs_buf *mb;
    if (cfs_buf_get(fs, s->alloc_root, CFS_KIND_MEMBERS, &mb)) {
        *unknown = true;   /* unreadable: the answer is not "free it" */
        return false;
    }
    const struct cfs_member_block *t = (const struct cfs_member_block *)(mb->data + CFS_MHDR_SIZE);
    bool ok = false;
    if (vdev < t->count && t->count <= CFS_MEMBERS_PER_BLOCK) {
        *out = t->m[vdev].alloc_root;
        ok = *out != 0;
    }
    cfs_buf_put(fs, mb);
    return ok;
}

bool cfs_snapshot_references(struct cfs *fs, const struct cfs_snapshot *s, uint64_t dva)
{
    if (!cfs_dva_valid(fs, dva) || s->alloc_root == 0)
        return false;
    uint64_t root;
    bool unknown;
    if (!snap_alloc_root(fs, s, CFS_DVA_VDEV(dva), &root, &unknown))
        return unknown;   /* unreadable: assume it is needed rather than free it */
    /* A member's index covers that member's blocks, so the chunk is
     * counted from the member's own block 0. */
    uint64_t blk = CFS_DVA_BLK(dva);
    unsigned chunk = (unsigned)(blk / CFS_BITS_PER_BITMAP);
    struct cfs_buf *idx;
    if (cfs_buf_get(fs, root, CFS_KIND_ALLOCIDX, &idx))
        return true;   /* unreadable: assume it is needed rather than free it */
    uint64_t bmblk = ((uint64_t *)(idx->data + CFS_MHDR_SIZE))[chunk];
    cfs_buf_put(fs, idx);
    if (bmblk == 0)
        return false;
    struct cfs_buf *bm;
    if (cfs_buf_get(fs, bmblk, CFS_KIND_BITMAP, &bm))
        return true;
    uint64_t bit = blk % CFS_BITS_PER_BITMAP;
    const uint8_t *bits = bm->data + CFS_MHDR_SIZE;
    bool set = (bits[bit / 8] >> (bit % 8)) & 1u;
    cfs_buf_put(fs, bm);
    return set;
}

#define CFS_SNAP_MAX_CHAIN 4096u   /* no sound list is this long */

/* --- copying the list ------------------------------------------------------
 *
 * The snapshot list is named by `cfs_super.snap_root` and by nothing
 * else, so a copy of it is published exactly when the superblock is --
 * atomically, by the same root write as every other change in the
 * transaction. Before this it was the one metadata chain in the
 * filesystem edited where it lay, which meant a crash between the
 * block's write and the root left the *previous* root naming a list
 * that belongs to a transaction that did not happen
 * (docs/audit/next-subsystem-snap-deadlist.md).
 *
 * The whole chain is copied, not the one block that changes: the chain
 * is singly linked, so copying a block in the middle means rewriting
 * the `next` of the block before it, and that is a copy too. At 42
 * entries a block the chain is one block on every filesystem anyone
 * has, so "copy the chain" is the cheap answer as well as the honest
 * one. Idempotent within a transaction, because cfs_buf_cow_exempt is:
 * a block already stamped with this generation is marked dirty and left
 * where it is.
 */
static int snap_cow(struct cfs *fs, struct cfs_res *res)
{
    uint64_t *slot = &fs->sb.snap_root;
    struct cfs_buf *prev = NULL;
    int rc = 0;
    unsigned guard = 0;
    while (*slot) {
        if (guard++ > CFS_SNAP_MAX_CHAIN) {
            kerror("cosmofs: snapshot list chain too long at %llu", (unsigned long long)*slot);
            rc = -EIO;
            break;
        }
        struct cfs_buf *b;
        rc = cfs_buf_get(fs, *slot, CFS_KIND_SNAPLIST, &b);
        if (rc)
            break;
        /* The parent slot is `fs->sb.snap_root` for the head and a
         * `next` inside the block before it otherwise -- which is held,
         * so the pointer stays good, and already dirty from its own
         * copy, so the write through it reaches the disk. */
        rc = cfs_buf_cow_exempt(fs, &b, slot, res);
        if (rc) {
            cfs_buf_put(fs, b);
            break;
        }
        if (prev != NULL)
            cfs_buf_put(fs, prev);
        prev = b;
        slot = &snap_payload(b)->next;
    }
    if (prev != NULL)
        cfs_buf_put(fs, prev);
    return rc;
}

/* --- deadlists ------------------------------------------------------------ */

/*
 * Append one block number to a snapshot's deadlist.
 *
 * The head block is always **copied** before it is added to, because
 * every caller runs inside a transaction and the block it would
 * otherwise edit is one the *current* root names: a crash between that
 * write and the next root leaves that root seeing an addition its own
 * transaction never made -- one block on two deadlists, which a later
 * pair of deletions frees twice, the second time under whatever the
 * allocator has since put there.
 *
 * There used to be one caller that could not copy, the commit's release
 * loop, because it ran *after* its root was published and a copy there
 * would allocate after the bitmap fixpoint. That caller is gone: the
 * append now runs before the root, which is what makes copying both
 * possible and necessary (docs/audit/next-subsystem-snap-deadlist.md).
 *
 * `res` is where a block comes from -- the commit's reservation, or
 * NULL to allocate, which only a caller running before the fixpoint may
 * do. The caller holds the mount lock.
 */
static int deadlist_append(struct cfs *fs, uint64_t *head, uint64_t blk, struct cfs_res *res)
{
    struct cfs_buf *b = NULL;
    if (*head) {
        int rc = cfs_buf_get(fs, *head, CFS_KIND_DEADLIST, &b);
        if (rc)
            return rc;
        struct cfs_dead_block *d = dead_payload(b);
        if (d->count < CFS_DEAD_PER_BLOCK) {
            rc = cfs_buf_cow_exempt(fs, &b, head, res);
            if (rc) {
                cfs_buf_put(fs, b);
                return rc;
            }
            d = dead_payload(b);
            d->blk[d->count++] = blk;
            cfs_buf_mark_dirty(fs, b);
            cfs_buf_put(fs, b);
            return 0;
        }
        cfs_buf_put(fs, b);
    }
    int rc;
    if (res != NULL) {
        uint64_t nb = cfs_res_take(res);
        if (nb == 0) {
            kerror("cosmofs: the commit's reservation is short of a deadlist block");
            return -ENOSPC;
        }
        rc = cfs_buf_new_at(fs, CFS_KIND_DEADLIST, nb, &b);
    } else {
        rc = cfs_buf_new(fs, CFS_KIND_DEADLIST, &b);
    }
    if (rc)
        return rc;
    struct cfs_dead_block *d = dead_payload(b);
    d->next = *head;
    d->count = 1;
    d->blk[0] = blk;
    *head = b->blkno;
    cfs_buf_put(fs, b);
    return 0;
}

/* Settle a doomed snapshot's deadlist against the snapshots that remain:
 * every block still occupied by one of them is handed to `keeper` (the
 * oldest remaining snapshot, which will be asked the same question when
 * it goes), and everything else goes back to the allocator. The chain's
 * own blocks always go: nothing else names them.
 *
 * This is exact. The first version of this code handed the whole list to
 * the previous snapshot, which was safe but held blocks nothing
 * referenced until that snapshot was deleted too; the bitmap each
 * snapshot already records answers the question directly. */
static int deadlist_settle(struct cfs *fs, uint64_t head, const struct cfs_snapshot *remaining,
                           unsigned nr_remaining, uint64_t *keeper_deadlist, uint64_t *freed,
                           uint64_t *kept)
{
    while (head) {
        struct cfs_buf *b;
        int rc = cfs_buf_get(fs, head, CFS_KIND_DEADLIST, &b);
        if (rc)
            return rc;
        struct cfs_dead_block *d = dead_payload(b);
        uint64_t next = d->next;
        uint64_t count = d->count < CFS_DEAD_PER_BLOCK ? d->count : CFS_DEAD_PER_BLOCK;
        /* Copy out: appending to the keeper's list may reuse buffers. */
        uint64_t *blks = kmalloc(count * sizeof(uint64_t), 0);
        if (blks == NULL) {
            cfs_buf_put(fs, b);
            return -ENOMEM;
        }
        memcpy(blks, d->blk, count * sizeof(uint64_t));
        cfs_buf_put(fs, b);
        for (uint64_t i = 0; i < count; i++) {
            bool needed = false;
            for (unsigned s = 0; s < nr_remaining && !needed; s++)
                needed = cfs_snapshot_references(fs, &remaining[s], blks[i]);
            if (needed) {
                rc = deadlist_append(fs, keeper_deadlist, blks[i], NULL);
                if (rc) {
                    kfree(blks);
                    return rc;
                }
                (*kept)++;
            } else {
                cfs_free_block_deferred(fs, blks[i]);
                (*freed)++;
            }
        }
        kfree(blks);
        /* The chain's own block, which no snapshot's tree reaches: the
         * filter would hold it on the deadlist it is part of. */
        cfs_free_block_exempt(fs, head);
        head = next;
    }
    return 0;
}

/* The newest snapshot is the one a released block belongs to. */
struct newest_ctx {
    uint64_t gen;
    uint64_t *deadlist;      /* pointer into the buffer, valid while it is held */
    struct cfs_buf *buf;
};

/* Called from the commit: append `blk` to the newest snapshot's
 * deadlist. Returns false when there is no snapshot, so the caller
 * clears the bitmap bit as it always did. */
/*
 * Does any snapshot still name this block? The question alone, with no
 * deadlist append and nothing written.
 *
 * cfs_snapshot_fill is this plus the append, and both now happen before
 * the root: the verdict is taken once, per block, and the record, the
 * deadlist and the release loop all read that one answer rather than
 * each asking the list again
 * (docs/audit/next-subsystem-snap-deadlist.md).
 *
 * `*best` and `*best_at` come back naming the newest snapshot, so the
 * caller that wants to append does not walk the list twice.
 */
static bool snapshot_holds(struct cfs *fs, uint64_t blk, uint64_t *best_at, unsigned *best_ix)
{
    fs->snap_walks++;
    uint64_t blkno = fs->sb.snap_root, best_block = 0;
    unsigned best_index = 0;
    uint64_t best_gen = 0;
    bool any = false;
    while (blkno) {
        struct cfs_buf *b;
        if (cfs_buf_get(fs, blkno, CFS_KIND_SNAPLIST, &b)) {
            /* The list cannot be read, so which snapshot needs this
             * block is unknown: hold it. Freeing on an unreadable list
             * would hand a live snapshot's block to the allocator. */
            kerror("cosmofs: snapshot list unreadable; holding block %llu", (unsigned long long)blk);
            return true;
        }
        struct cfs_snap_block *sb = snap_payload(b);
        uint64_t next = sb->next;
        for (unsigned i = 0; i < CFS_SNAPS_PER_BLOCK; i++) {
            if (sb->snap[i].name[0] == '\0')
                continue;
            if (!any || sb->snap[i].generation >= best_gen) {
                best_gen = sb->snap[i].generation;
                best_block = blkno;
                best_index = i;
                any = true;
            }
        }
        cfs_buf_put(fs, b);
        blkno = next;
    }
    if (!any)
        return false;
    struct cfs_buf *b;
    if (cfs_buf_get(fs, best_block, CFS_KIND_SNAPLIST, &b)) {
        kerror("cosmofs: snapshot list unreadable; holding block %llu", (unsigned long long)blk);
        return true;
    }
    struct cfs_snap_block *sb = snap_payload(b);
    /* Testing the newest snapshot alone is exact here: a block reaching
     * the commit's free list was in the live tree until now, so if the
     * newest snapshot does not occupy it, it was born after that
     * snapshot and no older one can name it either. */
    bool held = cfs_snapshot_references(fs, &sb->snap[best_index], blk);
    cfs_buf_put(fs, b);
    if (held && best_at != NULL) {
        *best_at = best_block;
        *best_ix = best_index;
    }
    return held;
}

bool cfs_snapshot_holds(struct cfs *fs, uint64_t blk)
{
    return snapshot_holds(fs, blk, NULL, NULL);
}

/*
 * Entries on every snapshot's deadlist: all of them when `of` is 0, or
 * the ones naming that block.
 *
 * A test hook, because nothing else can see this and one test needs it:
 * a block a snapshot holds is not a leak and not a finding -- the
 * checker claims a deadlist as metadata -- so a filesystem that held a
 * block it should have freed is `clean` all the way down.
 * `cosmofs-freelog-not-held` asks about one block by number, which is
 * the only statement that distinguishes "freed, as a record should be"
 * from "held, and gone until the snapshot is"
 * (docs/audit/next-subsystem-unmount-leak.md).
 */

uint64_t cfs_snapshot_deadlist_len(struct cfs *fs, uint64_t of)
{
    uint64_t blkno = fs->sb.snap_root, total = 0;
    unsigned guard = 0;
    while (blkno && guard++ < CFS_SNAP_MAX_CHAIN) {
        struct cfs_buf *b;
        if (cfs_buf_get(fs, blkno, CFS_KIND_SNAPLIST, &b))
            return total;
        struct cfs_snap_block *sb = snap_payload(b);
        uint64_t next = sb->next;
        for (unsigned i = 0; i < CFS_SNAPS_PER_BLOCK; i++) {
            if (sb->snap[i].name[0] == '\0')
                continue;
            uint64_t dl = sb->snap[i].deadlist;
            unsigned dguard = 0;
            while (dl && dguard++ < CFS_SNAP_MAX_CHAIN) {
                struct cfs_buf *db;
                if (cfs_buf_get(fs, dl, CFS_KIND_DEADLIST, &db))
                    break;
                const struct cfs_dead_block *d =
                    (const struct cfs_dead_block *)(db->data + CFS_MHDR_SIZE);
                uint32_t n = d->count <= CFS_DEAD_PER_BLOCK ? d->count : 0;
                if (of == 0) {
                    total += n;
                } else {
                    for (uint32_t k = 0; k < n; k++)
                        if (d->blk[k] == of)
                            total++;
                }
                dl = d->next;
                cfs_buf_put(fs, db);
            }
        }
        cfs_buf_put(fs, b);
        blkno = next;
    }
    return total;
}

/*
 * The invariant the copy makes true: **no block is named by more than
 * one deadlist entry.** A block is on exactly one snapshot's deadlist,
 * and a settle that hands it to the keeper takes it off the doomed
 * snapshot's list by freeing that list's blocks outright.
 *
 * Two entries for one block is what an interrupted keeper write-back
 * left behind before this unit: the entry was appended to the keeper's
 * head where it lay, so a crash before the root left the old root --
 * which still had the doomed snapshot, still naming the same block --
 * seeing both. Deleting the two snapshots in turn then frees it twice,
 * the second time after the allocator has handed it to something else.
 *
 * `cosmofs_check` cannot state this: it claims a deadlist's entries as
 * non-live (cosmofs_check.c), so the duplicate-claim finding never
 * fires on them. So the crash suite asks here instead, per prefix.
 *
 * `*examined` is how many entries were looked at. A caller needs it:
 * "no block is named twice" is also true of a filesystem with no
 * deadlist at all, and a test that cannot tell those apart proves
 * nothing (docs/audit/next-subsystem-snap-deadlist.md, "Vacuity").
 *
 * Quadratic in the number of entries, and a test hook only.
 */
uint64_t cfs_snapshot_deadlist_dups(struct cfs *fs, uint64_t *examined, uint64_t *first)
{
    uint64_t dups = 0, seen = 0;
    if (first != NULL)
        *first = 0;
    uint64_t blkno = fs->sb.snap_root;
    unsigned guard = 0;
    while (blkno && guard++ < CFS_SNAP_MAX_CHAIN) {
        struct cfs_buf *b;
        if (cfs_buf_get(fs, blkno, CFS_KIND_SNAPLIST, &b))
            break;
        struct cfs_snap_block *sb = snap_payload(b);
        uint64_t next = sb->next;
        for (unsigned i = 0; i < CFS_SNAPS_PER_BLOCK; i++) {
            if (sb->snap[i].name[0] == '\0')
                continue;
            uint64_t dl = sb->snap[i].deadlist;
            unsigned dguard = 0;
            while (dl && dguard++ < CFS_SNAP_MAX_CHAIN) {
                struct cfs_buf *db;
                if (cfs_buf_get(fs, dl, CFS_KIND_DEADLIST, &db))
                    break;
                const struct cfs_dead_block *d =
                    (const struct cfs_dead_block *)(db->data + CFS_MHDR_SIZE);
                uint32_t n = d->count <= CFS_DEAD_PER_BLOCK ? d->count : 0;
                uint64_t dnext = d->next;
                /* Copied out and the buffer released before the counts,
                 * which walk the whole list and may evict it. A block's
                 * worth, on the heap: the stack is not 4 KiB to spare. */
                uint64_t *blks = n ? kmalloc((size_t)n * sizeof(*blks), 0) : NULL;
                if (n && blks == NULL) {
                    cfs_buf_put(fs, db);
                    break;
                }
                for (uint32_t k = 0; k < n; k++)
                    blks[k] = d->blk[k];
                cfs_buf_put(fs, db);
                for (uint32_t k = 0; k < n; k++) {
                    seen++;
                    if (cfs_snapshot_deadlist_len(fs, blks[k]) > 1) {
                        if (dups == 0 && first != NULL)
                            *first = blks[k];
                        dups++;
                    }
                }
                kfree(blks);
                dl = dnext;
            }
        }
        cfs_buf_put(fs, b);
        blkno = next;
    }
    if (examined != NULL)
        *examined = seen;
    return dups;
}

/* --- what the commit reserves, and what it then writes ---------------------
 *
 * The deadlist append used to run in the commit's release loop, after
 * the root was published. Two things were wrong with that and only one
 * of them was the leak.
 *
 * The leak: the block it allocated and the entry it dirtied belonged to
 * a transaction already gone, so they waited for the next commit, and
 * at an unmount there is no next commit.
 *
 * The one the crash suite found once it had a snapshot in its workload:
 * when the append allocated a *new* head, it wrote that block number
 * into the snapshot list where the list lay -- a block the published
 * root still names. A crash after that write and before the next root
 * left the surviving root naming a deadlist head that had never been
 * written and whose bitmap bit it did not have: a block both unreadable
 * and reachable-and-free, which is the direction that hands live data
 * to the allocator. Prefix 125 of the replay suite, block 23.
 *
 * So the append moves in front of the root, where every other statement
 * a root makes is written -- which is possible only because the blocks
 * come from a reservation taken before the bitmap fixpoint, and safe
 * only because the list is now copied rather than edited.
 */

/* Blocks in the snapshot list's chain. */
static unsigned snap_chain_len(struct cfs *fs)
{
    unsigned n = 0;
    uint64_t blkno = fs->sb.snap_root;
    while (blkno && n < CFS_SNAP_MAX_CHAIN) {
        struct cfs_buf *b;
        if (cfs_buf_get(fs, blkno, CFS_KIND_SNAPLIST, &b))
            break;
        blkno = snap_payload(b)->next;
        cfs_buf_put(fs, b);
        n++;
    }
    return n;
}

unsigned cfs_snapshot_reserve_bound(struct cfs *fs, unsigned pending_bound)
{
    if (fs->snap_count == 0)
        return 0;
    /* The chain's copy, the copied deadlist head, and a fresh deadlist
     * block for every CFS_DEAD_PER_BLOCK blocks this commit could hold.
     * Generous, like the record's own bound: what nobody takes is
     * recorded as free by the same record. */
    return snap_chain_len(fs) + 2 + pending_bound / CFS_DEAD_PER_BLOCK;
}

unsigned cfs_snapshot_exempt_bound(struct cfs *fs)
{
    if (fs->snap_count == 0)
        return 0;
    return snap_chain_len(fs) + 1;   /* the superseded chain, and the superseded head */
}

/*
 * A failure here is one of two things, and they need different answers.
 *
 * Before the first append has landed, nothing has been written that a
 * retry would write twice, so the error is returned and the commit fails
 * cleanly -- the reservation is given back and the filesystem is as it
 * was. `snap_cow` runs on the first held block, before that block's
 * append, so a failure there is always of this kind.
 *
 * After it, some of this transaction's holds are recorded and some are
 * not, and a retry would record the first ones a second time: one block
 * on two deadlists, the corruption this unit exists to prevent. The
 * transaction is abandoned instead, which is the answer this file
 * already gives to a snapshot change it cannot publish whole
 * (snap_abandon).
 */
int cfs_snapshot_fill(struct cfs *fs, struct cfs_res *res, bool *held, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        held[i] = false;
    if (fs->snap_count == 0)
        return 0;
    bool copied = false;
    unsigned appended = 0;
    for (unsigned i = 0; i < n; i++) {
        uint64_t dva = fs->pending_free[i];
        uint64_t lin = cfs_dva_lin(fs, dva);
        if (lin == CFS_DVA_NONE || !cfs_bitmap_test(fs, lin))
            continue;
        uint64_t at = 0;
        unsigned ix = 0;
        fs->snap_verdicts++;
        if (!snapshot_holds(fs, dva, &at, &ix))
            continue;
        /* The verdict, once. The record and the release loop both read
         * it out of `held` rather than asking the list again. */
        held[i] = true;
        if (at == 0)
            continue;   /* held, but the list could not be read: never hand it back */
        if (!copied) {
            int rc = snap_cow(fs, res);
            if (rc)
                return rc;
            copied = true;
            /* The chain moved, so where the entry lives moved with it. */
            at = 0;
            if (!snapshot_holds(fs, dva, &at, &ix) || at == 0)
                continue;
        }
        struct cfs_buf *b;
        int rc = cfs_buf_get(fs, at, CFS_KIND_SNAPLIST, &b);
        if (rc == 0) {
            struct cfs_snap_block *sb = snap_payload(b);
            uint64_t head = sb->snap[ix].deadlist;
            rc = fs->test_fail_snapfill ? -EIO : deadlist_append(fs, &head, dva, res);
            if (rc == 0) {
                sb->snap[ix].deadlist = head;
                cfs_buf_mark_dirty(fs, b);
                appended++;
            }
            cfs_buf_put(fs, b);
        }
        if (rc) {
            if (appended > 0)
                cfs_fail(fs, rc);   /* half-recorded: a retry would double an entry */
            return rc;
        }
    }
    return 0;
}

/* --- create and delete ---------------------------------------------------- */

/* Both operations end by committing. If that fails the list has already
 * been changed in memory, and a later commit would publish a snapshot
 * mkdir reported as failed (or drop one rmdir did): the mount is marked
 * failed instead, which is what every other unpublishable change here
 * does. */
static int snap_commit(struct cfs *fs, int rc_so_far)
{
    if (rc_so_far)
        return rc_so_far;
    int rc = cfs_commit(fs);
    if (rc && !fs->failed) {
        fs->failed = rc;
        kerror("cosmofs: a snapshot change could not be committed (%d); the mount is now read-only", rc);
    }
    return rc;
}

/* A deletion that has begun releasing blocks cannot simply return an
 * error: the frees are in the open transaction, and the entry that
 * still names them is too. Committing one without the other would hand
 * a live snapshot's blocks to the allocator, so a failure past that
 * point abandons the transaction instead -- the same answer the mount
 * gives to any other change it cannot publish whole. */
static int snap_abandon(struct cfs *fs, int rc)
{
    if (!fs->failed)
        fs->failed = rc ? rc : -EIO;
    kerror("cosmofs: a snapshot deletion failed partway (%d); the transaction is abandoned", rc);
    return rc;
}

struct maxid_ctx {
    uint64_t max;
    unsigned live;
};

static bool maxid_scan(struct cfs_snapshot *s, void *arg)
{
    struct maxid_ctx *c = arg;
    if (s->id > c->max)
        c->max = s->id;
    c->live++;
    return true;
}

/* The highest id in use, over the whole chain. Ids are never reused, so
 * this only rises; a deleted snapshot's id is not handed out again while
 * the filesystem is mounted, which is what keeps cached vnodes honest. */
static int snap_max_id(struct cfs *fs, uint64_t *max, unsigned *live)
{
    struct maxid_ctx c = { .max = fs->snap_max_id, .live = 0 };
    int rc = snap_walk(fs, maxid_scan, &c, false);
    if (rc)
        return rc;
    *max = c.max;
    *live = c.live;
    fs->snap_max_id = c.max;
    return 0;
}

int cfs_snapshot_create(struct cfs *fs, const char *name)
{
    if (name == NULL || name[0] == '\0' || strlen(name) > CFS_SNAP_NAME_MAX)
        return -EINVAL;
    if (strchr(name, '/') != NULL)
        return -EINVAL;
    if (cfs_snapshot_find(fs, name, NULL))
        return -EEXIST;

    /* A snapshot names a durable tree, so commit what is open first. */
    int rc = cfs_commit(fs);
    if (rc)
        return rc;

    /* An id no live snapshot has and none has had while this filesystem
     * has been mounted: it is the tag this snapshot's vnodes carry. */
    uint64_t max_id = 0;
    unsigned live = 0;
    rc = snap_max_id(fs, &max_id, &live);
    if (rc)
        return rc;
    if (max_id >= CFS_SNAP_ID_MAX)
        return -ENOSPC;

    /* Every change to the list from here is a change to a copy of it,
     * published by this transaction's root and by nothing earlier. */
    rc = snap_cow(fs, NULL);
    if (rc)
        return rc;

    struct cfs_snapshot s;
    memset(&s, 0, sizeof(s));
    s.id = max_id + 1;
    s.generation = fs->sb.generation;
    s.imap_root = fs->sb.imap_root;
    /* From version 4 a snapshot pins the member table of its
     * generation: every member's bitmap hangs off it, and the bitmap of
     * a member *is* the set of blocks that member's tree reaches. */
    s.alloc_root = fs->sb.version >= 4 ? fs->sb.members : fs->mem[0].alloc_root;
    s.next_ino = fs->sb.next_ino;
    s.inode_count = fs->sb.inode_count;
    s.created_ns = clock_realtime_ns();
    strlcpy(s.name, name, sizeof(s.name));

    /* Find a free slot, or start a new block. */
    uint64_t blkno = fs->sb.snap_root;
    while (blkno) {
        struct cfs_buf *b;
        rc = cfs_buf_get(fs, blkno, CFS_KIND_SNAPLIST, &b);
        if (rc)
            return rc;
        struct cfs_snap_block *sb = snap_payload(b);
        uint64_t next = sb->next;
        for (unsigned i = 0; i < CFS_SNAPS_PER_BLOCK; i++) {
            if (sb->snap[i].name[0] == '\0') {
                sb->snap[i] = s;
                cfs_buf_mark_dirty(fs, b);
                cfs_buf_put(fs, b);
                fs->snap_count++;
                fs->snap_max_id = s.id;
                return snap_commit(fs, 0);
            }
        }
        cfs_buf_put(fs, b);
        blkno = next;
    }
    struct cfs_buf *b;
    rc = cfs_buf_new(fs, CFS_KIND_SNAPLIST, &b);
    if (rc)
        return rc;
    struct cfs_snap_block *sb = snap_payload(b);
    sb->next = fs->sb.snap_root;
    sb->snap[0] = s;
    fs->sb.snap_root = b->blkno;
    cfs_buf_put(fs, b);
    fs->snap_count++;
    fs->snap_max_id = s.id;
    return snap_commit(fs, 0);
}

struct clear_ctx {
    const char *name;
};

static bool clear_entry(struct cfs_snapshot *s, void *arg)
{
    const char *name = ((struct clear_ctx *)arg)->name;
    if (strcmp(s->name, name) != 0)
        return true;
    memset(s, 0, sizeof(*s));
    return false;
}

int cfs_snapshot_delete(struct cfs *fs, const char *name)
{
    if (name == NULL || name[0] == '\0')
        return -EINVAL;

    /* The whole list, however many blocks it spans: a snapshot left out
     * here would have its blocks freed while it still names them. Count
     * first, then take exactly that many. */
    unsigned total = 0;
    int rc = cfs_snapshot_list(fs, NULL, 0, &total);
    if (rc)
        return rc;
    if (total == 0)
        return -ENOENT;
    struct cfs_snapshot *all = kmalloc((size_t)total * sizeof(*all), 0);
    if (all == NULL)
        return -ENOMEM;
    unsigned n = 0;
    rc = cfs_snapshot_list(fs, all, total, &n);
    if (rc || n != total) {
        kfree(all);
        return rc ? rc : -EIO;   /* the list changed under the mount lock: refuse */
    }

    struct cfs_snapshot doomed;
    struct cfs_snapshot *remaining = kmalloc((size_t)total * sizeof(*remaining), 0);
    if (remaining == NULL) {
        kfree(all);
        return -ENOMEM;
    }
    unsigned nr_remaining = 0;
    bool found = false;
    unsigned oldest = 0;
    for (unsigned i = 0; i < n; i++) {
        if (!found && strcmp(all[i].name, name) == 0) {
            doomed = all[i];
            found = true;
            continue;
        }
        if (nr_remaining == 0 || all[i].generation < remaining[oldest].generation)
            oldest = nr_remaining;
        remaining[nr_remaining++] = all[i];
    }
    kfree(all);
    if (!found) {
        kfree(remaining);
        return -ENOENT;
    }

    /* The name exists and the transaction is about to change the list,
     * so copy it: the settle writes the keeper's new deadlist head into
     * its entry, and an entry edited where it lies is one the *current*
     * root can see. After the -ENOENT above, so a delete of a name that
     * is not there changes nothing. */
    rc = snap_cow(fs, NULL);
    if (rc) {
        kfree(remaining);
        return rc;
    }

    /* From here the open transaction holds both the frees and the entry
     * that still names them: they go together or not at all. */
    uint64_t freed = 0, kept = 0, keeper_head = 0;
    if (nr_remaining == 0) {
        /* Nothing remains: everything the snapshot held goes back. */
        rc = deadlist_settle(fs, doomed.deadlist, NULL, 0, &keeper_head, &freed, &kept);
    } else {
        /* The oldest remaining snapshot keeps what any of them still
         * occupies; it is asked the same question when it goes. */
        keeper_head = remaining[oldest].deadlist;
        rc = deadlist_settle(fs, doomed.deadlist, remaining, nr_remaining, &keeper_head, &freed, &kept);
        if (rc == 0 && keeper_head != remaining[oldest].deadlist) {
            /* Write the keeper's new deadlist head back into its entry. */
            uint64_t target_gen = remaining[oldest].generation;
            uint64_t blkno = fs->sb.snap_root;
            bool done = false;
            while (blkno && !done && rc == 0) {
                struct cfs_buf *b;
                rc = cfs_buf_get(fs, blkno, CFS_KIND_SNAPLIST, &b);
                if (rc)
                    break;   /* abandoned below: blocks are already released */
                struct cfs_snap_block *sb = snap_payload(b);
                uint64_t next = sb->next;
                for (unsigned i = 0; i < CFS_SNAPS_PER_BLOCK; i++) {
                    if (sb->snap[i].name[0] == '\0' || sb->snap[i].generation != target_gen)
                        continue;
                    sb->snap[i].deadlist = keeper_head;
                    cfs_buf_mark_dirty(fs, b);
                    done = true;
                    break;
                }
                cfs_buf_put(fs, b);
                blkno = next;
            }
        }
    }
    kfree(remaining);
    if (rc)
        return snap_abandon(fs, rc);

    struct clear_ctx cc = { .name = name };
    rc = snap_walk(fs, clear_entry, &cc, true);
    if (rc)
        return snap_abandon(fs, rc);
    kdebug("cosmofs: snapshot '%s': %llu block(s) freed, %llu still held", name, (unsigned long long)freed,
           (unsigned long long)kept);
    if (fs->snap_count)
        fs->snap_count--;
    return snap_commit(fs, 0);
}
