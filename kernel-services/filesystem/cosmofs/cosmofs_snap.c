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

/* --- deadlists ------------------------------------------------------------ */

/* Append one block number to a snapshot's deadlist, allocating a block
 * when the head is full. The caller holds the mount lock and is inside
 * the commit, so this must not itself defer frees. */
static int deadlist_append(struct cfs *fs, uint64_t *head, uint64_t blk)
{
    struct cfs_buf *b = NULL;
    if (*head) {
        int rc = cfs_buf_get(fs, *head, CFS_KIND_DEADLIST, &b);
        if (rc)
            return rc;
        struct cfs_dead_block *d = dead_payload(b);
        if (d->count < CFS_DEAD_PER_BLOCK) {
            d->blk[d->count++] = blk;
            cfs_buf_mark_dirty(fs, b);
            cfs_buf_put(fs, b);
            return 0;
        }
        cfs_buf_put(fs, b);
    }
    int rc = cfs_buf_new(fs, CFS_KIND_DEADLIST, &b);
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
                rc = deadlist_append(fs, keeper_deadlist, blks[i]);
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
        cfs_free_block_deferred(fs, head);
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
bool cfs_snapshot_hold_block(struct cfs *fs, uint64_t blk)
{
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
        return false;   /* no snapshots at all: the caller frees it */
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
    if (!cfs_snapshot_references(fs, &sb->snap[best_index], blk)) {
        cfs_buf_put(fs, b);
        return false;   /* nothing holds it: the caller frees it as before */
    }
    uint64_t head = sb->snap[best_index].deadlist;
    int rc = deadlist_append(fs, &head, blk);
    if (rc == 0) {
        sb->snap[best_index].deadlist = head;
        cfs_buf_mark_dirty(fs, b);
    }
    cfs_buf_put(fs, b);
    if (rc) {
        kerror("cosmofs: snapshot deadlist full (%d); block %llu leaked until unmount", rc,
               (unsigned long long)blk);
        return true;   /* held anyway: never hand a snapshot's block back */
    }
    return true;
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
