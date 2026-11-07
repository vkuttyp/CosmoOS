/*
 * cosmofs_vnode_floor.c - The C under cosmofs_vnode.cee (roadmap 2.0
 * SA7): the addresses of the vnode operation tables, C data that names C
 * functions; and the inode's fault seam, which kernel/faultinject.h makes
 * an inline function returning false without fault injection. The
 * vnodes are CeeSafe (cosmofs_vnode.cee, generated into cosmofs_vnode.c).
 */

#include <kernel/faultinject.h>
#include <kernel/vfs.h>

#include "cosmofs_internal.h"

const struct vnode_ops *cfs_vnode_floor_ops(unsigned type);
bool cfs_vnode_floor_inode_nomem(void);

/* The operations of a vnode of `type`: a directory's, a link's, or a
 * regular file's. */
const struct vnode_ops *cfs_vnode_floor_ops(unsigned type)
{
    return type == VNODE_DIR ? &cfs_dir_ops : type == VNODE_LNK ? &cfs_lnk_ops : &cfs_file_ops;
}

/* The test seam (cosmofs-nomem): the inode's map path cannot be had. */
bool cfs_vnode_floor_inode_nomem(void)
{
    return faultinject_should_fail(FI_COSMOFS_INODE_NOMEM);
}
