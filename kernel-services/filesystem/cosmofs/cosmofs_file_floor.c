/*
 * cosmofs_file_floor.c - The C under cosmofs_file.cee (roadmap 2.0 SA7):
 * what a build configuration decides about a regular file's operations,
 * which CeeSafe cannot name -- the debug build's check that a page the
 * run bound admitted can be written back (a field and a panic only that
 * build has), the run bound a self-test may lower, and the self-test
 * hook that counts a file's runs. The operations are CeeSafe
 * (cosmofs_file.cee, generated into cosmofs_file.c).
 */

#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/panic.h>
#include <kernel/vfs.h>

#include "cosmofs_internal.h"

void cfs_file_floor_stuck_page(struct cfs *fs, struct vnode *vn, uint64_t index, int rc);
unsigned cfs_file_floor_max_extents(const struct cfs *fs);

/* A page cfs_dirty_check let be dirtied that write-back cannot store is
 * this file's bug, not the writer's: named, under CONFIG_DEBUG, where it
 * happens (CFS-32). The extent-fail seam's -EFBIG is the one expected. */
void cfs_file_floor_stuck_page(struct cfs *fs, struct vnode *vn, uint64_t index, int rc)
{
#if CONFIG_DEBUG
    if ((rc == -EFBIG && !fs->fbig_injected) || rc == -EROFS)
        panic("cosmofs: inode %llu page %llu is dirty and write-back cannot store it (%d): cfs_dirty_check "
              "admitted it (CFS-32)",
              (unsigned long long)cfs_inode_of(vn)->ino, (unsigned long long)index, rc);
    fs->fbig_injected = false;
#else
    (void)fs;
    (void)vn;
    (void)index;
    (void)rc;
#endif
}

/* Runs an inode may have (cosmofs_internal.h, cfs_max_extents). */
unsigned cfs_file_floor_max_extents(const struct cfs *fs)
{
    return cfs_max_extents(fs);
}

int cosmofs_test_runs(struct vnode *vn, unsigned *n)
{
#if CONFIG_SELFTEST
    struct cfs *fs = cfs_of(vn->mnt);
    int rc = cfs_lock_tree_w(fs, vn);
    if (rc)
        return rc;
    rc = cfs_extents_count(fs, cfs_inode_of(vn), n);
    cfs_unlock_tree(fs);
    return rc;
#else
    (void)vn;
    (void)n;
    return -ENOTSUP;
#endif
}
