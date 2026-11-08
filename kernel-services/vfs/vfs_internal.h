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
 * thread calls it, and a test may): how many. vnode_put's nudge to it. */
unsigned vfs_mount_reap_orphans(void);
void vfs_mount_orphan_kick(void);

/* The held-walk seam's walk half (vfs_vnode.cee; a release build's stand-in
 * in vfs_floor.c): 0 to proceed, -EINTR if the held wait was killed.
 * `start` is a relative walk's directory, read and not yet referenced. */
int vfs_cwd_hold_walk(struct vnode *start);

/* The mount covering `dir` in any namespace, or in `ns`; whether `vn`,
 * which may be one of the two directories a rename holds, is a mountpoint
 * (vfs_mount.cee). The caller holds dir->lock for the first two. */
struct mount_ns;
struct mount *vfs_covering_mount(const struct vnode *dir);
struct mount *vfs_covering_mount_ns(const struct vnode *dir, const struct mount_ns *ns);
bool vfs_entry_is_mountpoint(struct vnode *vn, struct vnode *held1, struct vnode *held2);

/* The walk to the parent of `path`'s last component, which is copied into
 * `namebuf` (VFS_NAME_MAX + 1 bytes; empty for the root): *parent is
 * referenced on success (vfs_walk.cee). Whether `name` is "." or "..". */
int vfs_walk_parent(struct vnode *start, const char *path, char *namebuf, struct vnode **parent, size_t *last_len,
                    bool *trailing_slash);
bool vfs_dot_name(const char *name, size_t len);

/* The files the open's walk makes (vfs_file.cee): a file for `vn` (taking
 * the reference), its open hook run (on refusal the file is dropped);
 * whether a regular file's text is executing (vn->lock held). */
struct file *vfs_file_alloc(struct vnode *vn, unsigned flags);
int vfs_file_run_open(struct file *f);
bool vfs_text_busy_locked(struct vnode *vn);

#endif /* VFS_INTERNAL_H */
