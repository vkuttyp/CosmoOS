/*
 * cosmofs_hooks_floor.c - The C under cosmofs_hooks.cee (roadmap 2.0
 * SA7): the three test hooks whose fields a build configuration decides,
 * which CeeSafe cannot name -- the commit budget and the snapshot raise a
 * debug build's struct cfs has (CONFIG_DEBUG), and the run bound a
 * self-test build's has (CONFIG_SELFTEST). The other hooks are CeeSafe
 * (cosmofs_hooks.cee, generated into cosmofs_hooks.c).
 */

#include <kernel/cosmofs.h>
#include <kernel/mutex.h>
#include <kernel/vfs.h>

#include "cosmofs_internal.h"

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
