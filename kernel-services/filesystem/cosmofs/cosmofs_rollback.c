/*
 * cosmofs_rollback.c - Rolling the live tree back to a snapshot
 * (docs/kernel-services/filesystem/cosmofs/design.md, "Rollback").
 *
 * A rollback never changes a mounted tree. `rollback NAME` writes a
 * pending record and a safety snapshot of the live tree in one
 * transaction; the next mount makes the target's tree the live tree in
 * its first transaction and leaves a hold, which BOOT_OK clears. At every
 * instant the superblock names one live root and at most one record,
 * through committed transactions (CF19), so a crash leaves a mount that
 * can pick one consistent tree.
 */

#include <kernel/blk.h>
#include <kernel/crc32c.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/printf.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/timer.h>

#include "cosmofs_internal.h"

/* --- the record ------------------------------------------------------------ */

static uint32_t rb_crc(const struct cfs_rollback *r)
{
    struct cfs_rollback tmp = *r;
    tmp.crc = 0;
    return crc32c(&tmp, sizeof(tmp));
}

void cfs_rollback_seal(struct cfs_rollback *r)
{
    r->magic = CFS_ROLLBACK_MAGIC;
    r->version = 1;
    r->crc = rb_crc(r);
}

int cfs_rollback_parse(const void *payload, struct cfs_rollback *out)
{
    memcpy(out, payload, sizeof(*out));
    if (out->magic != CFS_ROLLBACK_MAGIC || out->version != 1 || rb_crc(out) != out->crc ||
        (out->state != CFS_ROLLBACK_PENDING && out->state != CFS_ROLLBACK_HELD))
        return -EIO;
    return 0;
}

int cfs_rollback_read(struct cfs *fs, struct cfs_rollback *out)
{
    if (fs->sb.version < 12 || fs->sb.rollback_root == 0)
        return -ENOENT;
    struct cfs_buf *b;
    if (cfs_buf_get(fs, fs->sb.rollback_root, CFS_KIND_ROLLBACK, &b))
        return -EIO;
    int rc = cfs_rollback_parse(b->data + CFS_MHDR_SIZE, out);
    cfs_buf_put(fs, b);
    return rc;
}

/* Replace the record (NULL: remove it), in the open transaction. The old
 * block is named by the superblock alone, which no snapshot preserves:
 * exempt. Allocates, so before a commit's fixpoint only. */
static int rb_write(struct cfs *fs, const struct cfs_rollback *r)
{
    if (fs->sb.rollback_root != 0) {
        cfs_free_block_exempt(fs, fs->sb.rollback_root);
        fs->sb.rollback_root = 0;
    }
    if (r == NULL)
        return 0;
    struct cfs_buf *b;
    int rc = cfs_buf_new(fs, CFS_KIND_ROLLBACK, &b);
    if (rc)
        return rc;
    struct cfs_rollback rec = *r;
    cfs_rollback_seal(&rec);
    memcpy(b->data + CFS_MHDR_SIZE, &rec, sizeof(rec));
    fs->sb.rollback_root = b->blkno;
    cfs_buf_put(fs, b);
    return 0;
}

/* The commit every operation here ends with: one that fails after the
 * transaction changed the record cannot be retried into a different
 * state, so the mount is marked failed (the snapshot list's rule). */
static int rb_commit(struct cfs *fs)
{
    int rc = cfs_commit(fs);
    if (rc && !fs->failed) {
        fs->failed = rc;
        kerror("cosmofs: a rollback change could not be committed (%d); the mount is now read-only", rc);
    }
    return rc;
}

bool cfs_rollback_protects(struct cfs *fs, const struct cfs_snapshot *s)
{
    struct cfs_rollback r;
    if (cfs_rollback_read(fs, &r) != 0)
        return false;
    bool safety = memcmp(r.safety_uuid, s->uuid, 16) == 0;
    bool target = memcmp(r.target_uuid, s->uuid, 16) == 0;
    return r.state == CFS_ROLLBACK_PENDING ? (safety || target) : safety;
}

/* --- the request ----------------------------------------------------------- */

int cfs_rollback_request(struct cfs *fs, const char *target, const char *safety_name, struct cfs_snapshot *safety)
{
    if (fs->sb.version < 11)
        return -EOPNOTSUPP;
    struct cfs_snapshot t;
    if (!cfs_snapshot_find(fs, target, &t))
        return -ENOENT;
    if (t.flags & CFS_SNAP_F_CLONE)
        return -EINVAL;   /* a clone is not a point in the live tree's history (design.md, "Clones") */
    struct cfs_rollback cur;
    if (cfs_rollback_read(fs, &cur) == 0 && cur.state == CFS_ROLLBACK_PENDING)
        return -EBUSY;    /* one at a time: cancel the pending one first */

    char name[CFS_SNAP_NAME_MAX + 1];
    if (safety_name != NULL) {
        if (strlen(safety_name) > CFS_SNAP_NAME_MAX)
            return -ENAMETOOLONG;
        strlcpy(name, safety_name, sizeof(name));
        if (cfs_snapshot_find(fs, name, NULL))
            return -EEXIST;
    } else {
        /* pre-rollback-<seconds>, made unique if two land in one second. */
        unsigned long long secs = clock_realtime_ns() / 1000000000ull;
        ksnprintf(name, sizeof(name), "pre-rollback-%llu", secs);
        for (unsigned n = 2; cfs_snapshot_find(fs, name, NULL) && n < 100; n++)
            ksnprintf(name, sizeof(name), "pre-rollback-%llu-%u", secs, n);
    }

    /*
     * The safety snapshot and the record in one transaction (CF19): the
     * creation commits the open work first, then puts the entry in the
     * next transaction without committing it; the record joins it, and
     * one commit publishes both. Split in two, a crash between them
     * leaves a record whose safety snapshot does not exist -- or a
     * snapshot with no record, which is harmless but not what was asked.
     */
    int rc;
    struct cfs_rollback r;
    memset(&r, 0, sizeof(r));
    r.state = CFS_ROLLBACK_PENDING;
    /* The safety snapshot's uuid, drawn here so the record can name it:
     * unique against the list now, and a raise below gives no converted
     * entry the same one. */
    cfs_snapshot_new_uuid(fs, r.safety_uuid, NULL, 0);
    r.requested_ns = clock_realtime_ns();

    struct cfs_snapshot s;
    rc = cfs_snapshot_create_ex(fs, name, true, false, r.safety_uuid, &s);
    if (rc)
        return rc;
    /* The raise to 12 can have happened just now, so the target is read
     * again for its uuid. */
    if (!cfs_snapshot_find(fs, target, &t)) {
        cfs_fail(fs, -EIO);
        return -EIO;
    }
    memcpy(r.target_uuid, t.uuid, 16);
    r.target_id = t.id;
    r.safety_id = s.id;
    r.generation = fs->sb.generation;
    rc = rb_write(fs, &r);
    if (rc) {
        cfs_fail(fs, rc);   /* the safety entry is in the transaction: not without its record */
        return rc;
    }
    rc = rb_commit(fs);
    if (rc == 0) {
        if (safety != NULL)
            *safety = s;
        kinfo("cosmofs: rollback to snapshot '%s' pending; the live tree is snapshot '%s'; it applies at the next "
              "mount", t.name, s.name);
    }
    return rc;
}

/* The checker's repair: the record goes, whatever it says, in the open
 * transaction the check's caller commits. */
int cfs_rollback_drop(struct cfs *fs)
{
    if (fs->sb.version < 12 || fs->sb.rollback_root == 0)
        return 0;
    return rb_write(fs, NULL);
}

int cfs_rollback_cancel(struct cfs *fs)
{
    struct cfs_rollback r;
    int rc = cfs_rollback_read(fs, &r);
    if (rc == -ENOENT || (rc == 0 && r.state != CFS_ROLLBACK_PENDING))
        return -ENOENT;
    /* A record that does not check out is withdrawn too: it would only
     * be refused at the next mount. */
    rc = cfs_commit(fs);
    if (rc)
        return rc;
    rc = rb_write(fs, NULL);
    if (rc == 0)
        rc = rb_commit(fs);
    if (rc == 0)
        kinfo("cosmofs: the pending rollback is withdrawn (its safety snapshot stays)");
    return rc;
}

int cfs_rollback_boot_ok(struct cfs *fs, bool *released)
{
    *released = false;
    struct cfs_rollback r;
    if (cfs_rollback_read(fs, &r) != 0 || r.state != CFS_ROLLBACK_HELD)
        return 0;
    int rc = cfs_commit(fs);
    if (rc)
        return rc;
    rc = rb_write(fs, NULL);
    if (rc == 0)
        rc = rb_commit(fs);
    if (rc == 0) {
        *released = true;
        kinfo("cosmofs: the boot after the rollback completed; the hold on snapshot id %llu is released",
              (unsigned long long)r.safety_id);
    }
    return rc;
}

/* --- the switch ------------------------------------------------------------- */

struct reachset {
    struct cfs *fs;
    uint8_t *bits;
    uint64_t nbits;
    uint64_t released;
};

static void reach_set(void *arg, uint64_t lin)
{
    struct reachset *r = arg;
    if (lin < r->nbits)
        r->bits[lin / 8] |= (uint8_t)(1u << (lin % 8));
}

static bool reach_has(const struct reachset *r, uint64_t lin)
{
    return lin < r->nbits && (r->bits[lin / 8] >> (lin % 8)) & 1u;
}

/* The old live tree's walk: every block it reaches that the target does
 * not is released, as a commit releases what the live tree drops. The
 * commit then asks the newest snapshot -- the safety snapshot, unless one
 * was taken since -- whether it still names the block, and holds it on
 * that snapshot's deadlist or frees it. */
static void release_unless_target(void *arg, uint64_t lin)
{
    struct reachset *r = arg;
    if (reach_has(r, lin))
        return;
    uint64_t dva = cfs_lin_dva(r->fs, lin);
    if (dva == CFS_DVA_NONE)
        return;
    cfs_free_block_deferred(r->fs, dva);
    r->released++;
}

/* Deadlist entries the new live tree reaches are live again. */
static bool target_reaches(void *arg, uint64_t dva)
{
    struct reachset *r = arg;
    uint64_t lin = cfs_dva_lin(r->fs, dva);
    return lin != CFS_DVA_NONE && reach_has(r, lin);
}

int cfs_rollback_at_mount(struct cfs *fs, bool *switched)
{
    *switched = false;
    struct cfs_rollback r;
    int rc = cfs_rollback_read(fs, &r);
    if (rc == -ENOENT)
        return 0;
    const char *dev = fs->mnt && fs->mnt->bdev ? fs->mnt->bdev->name : "cosmofs";
    if (rc) {
        kerror("cosmofs: %s: the rollback record does not check out; not applied, the current tree is mounted "
               "(fsctl check names it, and its repair removes it)", dev);
        return 0;
    }
    if (r.state != CFS_ROLLBACK_PENDING)
        return 0;   /* a hold: BOOT_OK clears it */
    struct cfs_snapshot t, s;
    if (!cfs_snapshot_find_uuid(fs, r.target_uuid, &t)) {
        kerror("cosmofs: %s: rollback refused: its target snapshot (id %llu) is not on the list; the current tree "
               "is mounted", dev, (unsigned long long)r.target_id);
        return 0;
    }
    if (!cfs_snapshot_find_uuid(fs, r.safety_uuid, &s)) {
        kerror("cosmofs: %s: rollback refused: its safety snapshot (id %llu) is not on the list, so the tree "
               "before it could not be kept; the current tree is mounted", dev, (unsigned long long)r.safety_id);
        return 0;
    }
    if (fs->mnt && (fs->mnt->flags & MOUNT_RDONLY)) {
        kwarn("cosmofs: %s: a rollback to snapshot '%s' is pending; not applied on a read-only mount", dev, t.name);
        return 0;
    }

    /* From here the transaction changes; a failure is reported to the
     * mount, which abandons it and loads the current tree again. */
    struct reachset set = { .fs = fs, .nbits = fs->nblocks };
    set.bits = kzalloc((size_t)((fs->nblocks + 7) / 8));
    if (set.bits == NULL)
        return -ENOMEM;
    mutex_lock(&fs->lock);
    rc = cfs_tree_walk(fs, t.imap_root, t.next_ino, reach_set, &set);
    if (rc == 0)
        rc = cfs_tree_walk(fs, fs->sb.imap_root, fs->sb.next_ino, release_unless_target, &set);
    uint64_t revived = 0;
    if (rc == 0)
        rc = cfs_snapshot_deadlists_filter(fs, target_reaches, &set, &revived);
    if (rc == 0) {
        /* The old live tree's orphan record goes with it: its inodes are
         * the safety snapshot's now. The commit retires the chain. */
        fs->nr_orphans = 0;
        fs->sb.imap_root = t.imap_root;
        fs->sb.next_ino = t.next_ino;
        fs->sb.inode_count = t.inode_count;
        struct cfs_rollback held = r;
        held.state = CFS_ROLLBACK_HELD;
        held.generation = fs->gen;
        rc = rb_write(fs, &held);
    }
    if (rc == 0) {
        fs->switching = true;
        rc = cfs_commit(fs);
        fs->switching = false;
    }
    mutex_unlock(&fs->lock);
    kfree(set.bits);
    if (rc) {
        kerror("cosmofs: %s: the rollback to snapshot '%s' failed (%d); abandoned, the current tree is mounted", dev,
               t.name, rc);
        return rc;
    }
    *switched = true;
    kinfo("cosmofs: %s: rolled back: the live tree is snapshot '%s' (generation %llu); the tree before it is "
          "snapshot '%s', held until this boot completes; %llu block(s) released, %llu deadlist entr(ies) live again",
          dev, t.name, (unsigned long long)t.generation, s.name, (unsigned long long)set.released,
          (unsigned long long)revived);
    return 0;
}
