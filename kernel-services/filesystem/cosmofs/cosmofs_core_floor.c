/*
 * cosmofs_core_floor.c - The C under cosmofs_core.cee (roadmap 2.0 SA7):
 * the fs_type table and its registration, which name the mount's
 * functions by address; the walk of a mount's vnode hash, whose lock,
 * list and reference are kernel/vfs.h's inline functions and macros; and
 * the lock assertion a lockdep build makes and a release build does not.
 * The mount itself is CeeSafe (cosmofs_core.cee, generated into
 * cosmofs_core.c).
 */

#include <kernel/cosmofs.h>
#include <kernel/lockdep.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/vfs.h>

#include "cosmofs_internal.h"

/* cosmofs_core.cee's. */
int cfs_core_mount(struct fs_type *fst, struct blkdev *bdev, unsigned flags, struct mount *mnt);
int cfs_core_unmount(struct mount *mnt);
int cfs_core_sync(struct mount *mnt);
int cfs_core_statfs(struct mount *mnt, struct vfs_statfs *out);

struct vnode *cfs_core_floor_dirty_vnode(struct mount *mnt, unsigned b);
void cfs_core_floor_assert_held(struct cfs *fs);

/* The next hashed regular file of bucket `b` with dirty pages, with a
 * reference for the caller; NULL when there is none. The hash lock is a
 * spinlock; a hashed vnode always holds a reference (vnode_put unhashes
 * before the last drop). */
struct vnode *cfs_core_floor_dirty_vnode(struct mount *mnt, unsigned b)
{
    struct vnode *vn = NULL;
    arch_irq_state_t s = spin_lock_irqsave(&mnt->lock);
    struct vnode *it;
    list_for_each_entry(it, &mnt->vnodes[b], hash_link) {
        if (it->type == VNODE_REG && it->pc.nr_dirty) {
            vn = it;
            vnode_get(vn);
            break;
        }
    }
    spin_unlock_irqrestore(&mnt->lock, s);
    return vn;
}

/* A commit counted as the writeback thread's is counted under the lock
 * that publishes it. */
void cfs_core_floor_assert_held(struct cfs *fs)
{
    lockdep_assert_held(&fs->lock, LOCKDEP_KIND_MUTEX);
#if !CONFIG_LOCKDEP
    (void)fs;
#endif
}

struct fs_type cosmofs_fs_type = {
    .name = "cosmofs",
    .magic = 0x4d534643u,   /* "CFSM", the metadata header's magic: Linux has no number for this one */
    .statfs = cfs_core_statfs,
    .mount = cfs_core_mount,
    .unmount = cfs_core_unmount,
    .sync = cfs_core_sync,
    /* The maintenance passes, reachable from /dev/fsctl. Until this the
     * only caller of either was a self-test. */
    .check = cosmofs_check,
    .scrub = cosmofs_scrub,
    .check_dev = cosmofs_check_dev,
    .snapctl = cosmofs_snapctl,
    .poolctl = cosmofs_poolctl,
    /* A single-device pool, unencrypted: what the installer makes
     * through /dev/blkctl (roadmap M2), named or not (S6). */
    .format = cosmofs_format_label,
    .format_mirror = cfs_format_mirror_devs,
    /* An encrypted pool's key, and making one (roadmap 1.2 B5). */
    .keyctl = cosmofs_keyctl,
    .format_keyed = cosmofs_format_keys,
};

/*
 * The newest format this kernel reads, as text in its image: cosmo-upgrade
 * looks for it in a release's kernel.elf before switching to that release,
 * and refuses one that could not mount the root (a kernel without the
 * marker reads 11 at most; design.md, "Format version 12"). Printed at
 * boot, which also keeps it in every build.
 */
_Static_assert(CFS_VERSION == 15, "update the format marker with CFS_VERSION");
static const char cfs_format_marker[] = "cosmofs-format-max=15";

void cosmofs_init(void)
{
    if (vfs_register_fs(&cosmofs_fs_type))
        panic("cosmofs: cannot register");
    kinfo("cosmofs: reads format versions 2 to 15 (%s)", cfs_format_marker);
}
