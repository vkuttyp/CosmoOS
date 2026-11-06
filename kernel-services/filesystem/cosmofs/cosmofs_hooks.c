/*
 * cosmofs_hooks.c - A mount's figures (cosmofs_stats), the key operations
 * on a mounted pool, and the test hooks the self-tests reach a mount
 * through. Moved out of cosmofs_core.c (roadmap 2.0 SA7) so that it can be
 * held to a digest and made CeeSafe on its own.
 */

#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/mutex.h>
#include <kernel/storage.h>
#include <kernel/vfs.h>

#include "cosmofs_internal.h"

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
        for (unsigned c = 0; c < pool_copies(fs->pool, CFS_DVA(v, 0)); c++)
            out->devices += pool_copy_online(fs->pool, v, c) ? 1u : 0u;
    out->repairs = fs->repairs;
    out->degraded = cfs_copies_out(fs);
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

#if CONFIG_DEBUG
void cosmofs_test_commit_budget(struct mount *mnt, bool armed, unsigned left)
{
    struct cfs *fs = cfs_of(mnt);
    if (fs == NULL)
        return;
    mutex_lock(&fs->lock);
    fs->test_commit_armed = armed;
    fs->test_commits_left = left;
    mutex_unlock(&fs->lock);
}
void cosmofs_test_snap_no_raise(struct mount *mnt, bool on)
{
    struct cfs *fs = cfs_of(mnt);
    if (fs != NULL)
        fs->test_no_raise = on;
}
#endif

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
    if (fs->mac.on)
        return -EOPNOTSUPP;   /* version 14: slots, through COSMOFS_KEY_SLOT, not a version-1 rewrap */
    if (fs->rdonly)
        return -EROFS;
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

/*
 * What the mount's own replay did: how many dirty marks it made, and how
 * many of those happened with a writeback thread already running. The
 * second must be zero (no autonomous committer before the mount is
 * live); the first must not be, or the test asking is asking nothing.
 */
void cosmofs_test_mount_writeback(struct mount *mnt, uint64_t *dirty_notes, uint64_t *early)
{
    struct cfs *fs = cfs_of(mnt);
    if (dirty_notes)
        *dirty_notes = fs ? fs->mount_dirty_notes : 0;
    if (early)
        *early = fs ? fs->wb_early : 0;
}

/* The pool under a mount, for the copy-state self-tests to read (roadmap
 * 1.2 B3). Read with writeback off: nothing else moves it then. */
struct spool *cosmofs_test_pool(struct mount *mnt)
{
    struct cfs *fs = cfs_of(mnt);
    return fs ? fs->pool : NULL;
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

/* Test seam: the run bound on this mount (0 restores CFS_MAX_EXTENTS).
 * Nothing in a build without self-tests. */
void cosmofs_test_set_max_extents(struct mount *mnt, unsigned n)
{
#if CONFIG_SELFTEST
    struct cfs *fs = cfs_of(mnt);
    if (fs)
        fs->test_max_extents = n;
#else
    (void)mnt;
    (void)n;
#endif
}
