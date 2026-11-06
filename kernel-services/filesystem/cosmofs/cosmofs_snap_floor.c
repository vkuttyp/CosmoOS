/*
 * cosmofs_snap_floor.c - The C under cosmofs_snap.cee (roadmap 2.0 SA7):
 * the one field CeeSafe cannot name, `test_no_raise`, which struct cfs has
 * only in a debug build (CONFIG_DEBUG). The snapshots are CeeSafe
 * (cosmofs_snap.cee, generated into cosmofs_snap.c).
 */

#include "cosmofs_internal.h"

bool cfs_snap_floor_no_raise(const struct cfs *fs);

/* A test's request that a snapshot with identity not raise a version-11
 * pool; never in a release build. */
bool cfs_snap_floor_no_raise(const struct cfs *fs)
{
#if CONFIG_DEBUG
    return fs->test_no_raise;
#else
    (void)fs;
    return false;
#endif
}
