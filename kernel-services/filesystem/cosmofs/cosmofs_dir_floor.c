/*
 * cosmofs_dir_floor.c - The C under cosmofs_dir.cee (roadmap 2.0 SA7):
 * readdir's entry point and the call through its callback, whose type
 * (vfs_dirent_cb) takes an enum vnode_type that a CeeSafe function type
 * cannot spell. The walk is CeeSafe (cosmofs_dir.cee, generated into
 * cosmofs_dir.c); so is every other directory operation.
 */

#include <kernel/vfs.h>

#include "cosmofs_internal.h"

int cfs_dir_floor_emit(void *cb, void *arg, const char *name, size_t len, uint64_t ino, unsigned type);
int cfs_dir_readdir(struct vnode *dir, uint64_t *pos, void *cb, void *arg);

/* One entry to the reader: nonzero when it wants no more. */
int cfs_dir_floor_emit(void *cb, void *arg, const char *name, size_t len, uint64_t ino, unsigned type)
{
    vfs_dirent_cb f = (vfs_dirent_cb)cb;
    return f(arg, name, len, ino, (enum vnode_type)type);
}

int cfs_readdir(struct vnode *dir, uint64_t *pos, vfs_dirent_cb cb, void *arg)
{
    return cfs_dir_readdir(dir, pos, (void *)cb, arg);
}
