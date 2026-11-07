/*
 * cosmofs_floor.c - The C under cosmofs.cee (roadmap 2.0 SA7): the
 * vnode operation tables of a directory, a regular file and a link --
 * C data that names the operations by address. The operations are
 * CeeSafe (cosmofs_dir.cee, cosmofs_file.cee); so are the directory
 * helpers beside them (cosmofs.cee, generated into cosmofs.c).
 */

#include <kernel/vfs.h>

#include "cosmofs_internal.h"

const struct vnode_ops cfs_dir_ops = {
    .lookup = cfs_lookup,
    .create = cfs_create,
    .symlink = cfs_symlink,
    .mkdir = cfs_mkdir,
    .unlink = cfs_unlink,
    .link = cfs_link,
    .rmdir = cfs_rmdir,
    .rename = cfs_rename,
    .readdir = cfs_readdir,
    .sync = cfs_vnode_sync,
    .evict = cfs_evict,
    .setattr = cfs_vnode_setattr,
};

const struct vnode_ops cfs_file_ops = {
    .readpage = cfs_readpage,
    .writepage = cfs_writepage,
    .writepages = cfs_writepages,
    .dirty_check = cfs_dirty_check,
    .truncate = cfs_truncate,
    .sync = cfs_vnode_sync,
    .evict = cfs_evict,
    .setattr = cfs_vnode_setattr,
};

/* A link is never a page: its target is read from its block directly,
 * so there is no readpage or writepage here. */
const struct vnode_ops cfs_lnk_ops = {
    .readlink = cfs_readlink,
    .evict = cfs_evict,
    .setattr = cfs_vnode_setattr,   /* lchown */
};
