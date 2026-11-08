/*
 * vfs_internal.h - What the VFS's own files may touch of one another.
 *
 * The mount table is private to the VFS and stays that way: its data is
 * vfs_floor.c's, vfs_mount.cee changes it, and mountns.cee and fsctl
 * read it through this narrow surface. The rest is what the VFS's
 * CeeSafe parts (vfs_*.cee) call between their files. Nothing outside
 * kernel-services/vfs/ includes this.
 */

#ifndef VFS_INTERNAL_H
#define VFS_INTERNAL_H

#include <kernel/list.h>
#include <kernel/mutex.h>
#include <kernel/spinlock.h>
#include <kernel/vfs.h>

/* The mount table: every mount, in the order they were made. Under
 * g_mounts_lock, which is taken before any vnode lock. */
extern struct mutex g_mounts_lock;
extern struct list_node g_mounts;

/* A visible namespace's `mounts` list and `g_root_mount` change under
 * this as well as g_mounts_lock, and /proc/mounts reads them under this
 * alone: procfs renders a file in its lookup, with the directory's vnode
 * lock held, where g_mounts_lock (taken before any vnode lock) may not be.
 * A leaf: nothing is taken under it. A namespace being built or torn down
 * is visible to no reader and changes under g_mounts_lock only. */
extern spinlock_t g_mount_list_lock;

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
 * close. A mount something still uses -- an open file, a process's
 * directory, another namespace's mount nested on it -- waits on the
 * orphans' list, whole, until its last such reference goes, and is torn
 * down then (by the caller, at once, when nothing does).
 */
void vfs_mount_orphaned(struct mount *mnt);

/* Take a mount off its mountpoint and out of the table, keeping the
 * count right. g_mounts_lock held; takes the mountpoint's lock. */
void vfs_mount_detach(struct mount *mnt);

/* Tear down every orphan nothing uses any more (vfs_mount.cee; its reaper
 * thread calls it, and a test may): how many. */
unsigned vfs_mount_reap_orphans(void);

/* The held-walk seam's walk half (vfs_vnode.cee; a release build's stand-in
 * in vfs_floor.c): 0 to proceed, -EINTR if the held wait was killed.
 * `start` is a relative walk's directory, read and not yet referenced. */
int vfs_cwd_hold_walk(struct vnode *start);

#endif /* VFS_INTERNAL_H */
