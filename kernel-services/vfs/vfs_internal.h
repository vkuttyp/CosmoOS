/*
 * vfs_internal.h - What mountns.c may touch inside vfs.c.
 *
 * The mount table is private to vfs.c and stays that way; this is the
 * narrow surface the namespace code needs, and it exists so that code
 * can live in its own file without the mount table becoming public.
 * Nothing outside kernel-services/vfs/ includes this.
 */

#ifndef VFS_INTERNAL_H
#define VFS_INTERNAL_H

#include <kernel/list.h>
#include <kernel/mutex.h>
#include <kernel/vfs.h>

/* The mount table: every mount, in the order they were made. Under
 * g_mounts_lock, which is taken before any vnode lock. */
extern struct mutex g_mounts_lock;
extern struct list_node g_mounts;

/* The root filesystem. Every namespace has it and none may unmount it,
 * which is why it carries no visibility refs. */
extern struct mount *g_root_mount;

/*
 * A mount no namespace can see any more. Called with no locks held and
 * with the mount already off `g_mounts` and off its mountpoint's
 * `covers` list -- it is unreachable by the time this runs, so unlike
 * `vfs_umount2` there is nobody to hand a failure back to: a commit
 * that fails is reported loudly and the transaction dropped, because
 * the alternative is a filesystem left open that nothing will ever
 * close.
 */
void vfs_mount_orphaned(struct mount *mnt);

/* Take a mount off its mountpoint and out of the table, keeping the
 * count right. g_mounts_lock held; takes the mountpoint's lock. */
void vfs_mount_detach(struct mount *mnt);

#endif /* VFS_INTERNAL_H */
