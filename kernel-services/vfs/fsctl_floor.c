/*
 * fsctl_floor.c - The C under fsctl.cee (roadmap 2.0 SA7): a file's
 * state, a C struct round a struct mutex whose size the build's lock
 * debugging decides, with where its fields are; the operation table, C
 * data naming the operations by address; the spinlock /proc/mounts is
 * rendered under, a C global (vfs_internal.h); and the values fsctl.cee
 * writes as its own constants, asserted. /dev/fsctl itself is CeeSafe
 * (fsctl.cee, generated into fsctl.c).
 */

#include <kernel/cosmofs.h>
#include <kernel/cred.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/mutex.h>
#include <kernel/vfs.h>

#include <uapi/cosmo/fsctl.h>

#include "vfs_internal.h"

_Static_assert(EPERM == 1 && ENOENT == 2 && ENOMEM == 12 && EBUSY == 16 && ENODEV == 19 && EINVAL == 22 &&
                   ERANGE == 34 && EOPNOTSUPP == 95 && KMEM_ZERO == 1 && PRIV_SYS_ADMIN == 2 && CRED_ROOT_UID == 0 &&
                   MOUNT_RDONLY == 1 && MOUNT_STATE_DEGRADED == 1,
               "fsctl.cee's errno.h, kmalloc.h, cred.h and vfs.h values");
_Static_assert(COSMOFS_CHECK_REPAIR == 1 && COSMOFS_CHECK_BOOT == 4 && COSMOFS_SCRUB_REPAIR == 1 &&
                   COSMOFS_SNAP_I_CLONE == 1 && COSMOFS_SNAP_I_TARGET == 2 && COSMOFS_SNAP_I_SAFETY == 4 &&
                   COSMOFS_SNAP_I_HELD == 8 && COSMOFS_SNAP_I_LIVE == 16 && COSMOFS_POOL_PENDING == 1 &&
                   COSMOFS_POOL_HELD == 2 && COSMOFS_COPY_I_WHOLE == 1 && COSMOFS_COPY_I_DETACH == 2 &&
                   CFS_CHECK_NAMES == COSMO_FSCTL_NAMES && COSMOFS_SNAP_NAME_MAX + 1 == 32,
               "fsctl.cee's cosmofs.h values");
_Static_assert(COSMO_FSCTL_VERSION == 3 && COSMO_FSCTL_LIST == 1 && COSMO_FSCTL_CHECK == 2 &&
                   COSMO_FSCTL_SCRUB == 3 && COSMO_FSCTL_SNAP_CREATE == 4 && COSMO_FSCTL_SNAP_LIST == 5 &&
                   COSMO_FSCTL_SNAP_ROLLBACK == 7 && COSMO_FSCTL_SNAP_CLONE == 9 && COSMO_FSCTL_BOOT_OK == 10 &&
                   COSMO_FSCTL_CHECK_DEV == 11 && COSMO_FSCTL_SCRUB_EX == 12 && COSMO_FSCTL_POOL_STATUS == 13 &&
                   COSMO_FSCTL_POOL_SPARE_REMOVE == 20 && COSMO_FSCTL_KEY_INFO == 21 && COSMO_FSCTL_KEY_SLOT == 24 &&
                   COSMO_FSCTL_F_REPAIR == 1 && COSMO_FSCTL_F_SAFETY_NAME == 2 && COSMO_FSCTL_F_NO_SIZES == 4 &&
                   COSMO_FSCTL_F_BOOT == 8 && COSMO_FSCTL_CAP_CHECK == 1 && COSMO_FSCTL_CAP_SCRUB == 2 &&
                   COSMO_FSCTL_CAP_SNAP == 4 && COSMO_FSCTL_CAP_POOL == 8 && COSMO_FSCTL_R_PARTIAL == 1 &&
                   COSMO_FSCTL_R_CLEAN == 2 && COSMO_FSCTL_R_REPAIR_REFUSED == 4 && COSMO_FSCTL_DEV_F_CHECKED == 1 &&
                   COSMO_FSCTL_POOL_F_PENDING == 1 && COSMO_FSCTL_POOL_F_HELD == 2 &&
                   COSMO_FSCTL_SNAP_F_CLONE == 1 && COSMO_FSCTL_SNAP_F_TARGET == 2 && COSMO_FSCTL_SNAP_F_SAFETY == 4 &&
                   COSMO_FSCTL_SNAP_F_HELD == 8 && COSMO_FSCTL_SNAP_F_LIVE == 16 && COSMO_FSCTL_POOL_DEGRADED == 1 &&
                   COSMO_FSCTL_POOL_RESILVER == 2 && COSMO_FSCTL_COPY_F_WHOLE == 1 &&
                   COSMO_FSCTL_COPY_F_DETACH == 2 && COSMO_FSCTL_CLASSES == 16 && COSMO_FSCTL_NAMES == 8 &&
                   COSMO_FSCTL_SNAP_PAGE == 1024 && COSMO_FSCTL_NAME_MAX + 1 == 32,
               "fsctl.cee's fsctl.h values");
/* fsctl.cee maps an op by subtraction: the three tables keep fsctl.h's order. */
_Static_assert(COSMOFS_SNAP_CREATE == COSMO_FSCTL_SNAP_CREATE - 3 && COSMOFS_SNAP_LIST == COSMO_FSCTL_SNAP_LIST - 3 &&
                   COSMOFS_SNAP_DELETE == COSMO_FSCTL_SNAP_DELETE - 3 &&
                   COSMOFS_SNAP_ROLLBACK == COSMO_FSCTL_SNAP_ROLLBACK - 3 &&
                   COSMOFS_SNAP_CANCEL == COSMO_FSCTL_SNAP_CANCEL - 3 &&
                   COSMOFS_SNAP_CLONE == COSMO_FSCTL_SNAP_CLONE - 3 && COSMOFS_SNAP_BOOT_OK == COSMO_FSCTL_BOOT_OK - 3,
               "fsctl.cee's snapshot ops");
_Static_assert((int)COSMOFS_POOL_STATUS == COSMO_FSCTL_POOL_STATUS - 13 &&
                   (int)COSMOFS_POOL_OFFLINE == COSMO_FSCTL_POOL_OFFLINE - 13 &&
                   (int)COSMOFS_POOL_ONLINE == COSMO_FSCTL_POOL_ONLINE - 13 &&
                   (int)COSMOFS_POOL_REPLACE == COSMO_FSCTL_POOL_REPLACE - 13 &&
                   (int)COSMOFS_POOL_ATTACH == COSMO_FSCTL_POOL_ATTACH - 13 &&
                   (int)COSMOFS_POOL_DETACH == COSMO_FSCTL_POOL_DETACH - 13 &&
                   (int)COSMOFS_POOL_SPARE_ADD == COSMO_FSCTL_POOL_SPARE_ADD - 13 &&
                   (int)COSMOFS_POOL_SPARE_REMOVE == COSMO_FSCTL_POOL_SPARE_REMOVE - 13 &&
                   (int)COSMOFS_KEY_INFO == COSMO_FSCTL_KEY_INFO - 21 &&
                   (int)COSMOFS_KEY_LOAD == COSMO_FSCTL_KEY_LOAD - 21 &&
                   (int)COSMOFS_KEY_UNLOAD == COSMO_FSCTL_KEY_UNLOAD - 21 &&
                   (int)COSMOFS_KEY_SLOT == COSMO_FSCTL_KEY_SLOT - 21,
               "fsctl.cee's pool and key ops");
/* The structs fsctl.cee keeps on its stack, as the C did, in slots of u64s. */
_Static_assert(sizeof(struct cosmofs_check_report) == 186 * 8 && sizeof(struct cosmofs_scrub_stats) == 8 * 8 &&
                   sizeof(struct cosmofs_snap_req) == 11 * 8 && sizeof(struct cosmofs_pool_req) == 11 * 8 &&
                   sizeof(struct cosmofs_key_req) == 4 * 8 && sizeof(struct cosmo_fsctl) == 11 * 8,
               "fsctl.cee's stack slots");

/* The operations, CeeSafe (fsctl.cee). */
int fsctl_open_file(struct vnode *vn, struct file *f);
void fsctl_release(struct vnode *vn, struct file *f);
int64_t fsctl_read_file(struct vnode *vn, struct file *f, uint64_t off, void *buf, size_t len);
int64_t fsctl_write_file(struct vnode *vn, struct file *f, uint64_t off, const void *buf, size_t len);

/* What fsctl.cee calls here. */
const struct chrdev_ops *fsctl_floor_ops(void);
size_t fsctl_floor_open_layout(unsigned which);
spinlock_t *fsctl_floor_list_lock(void);

/* The result of this file's last command, read back until the next one. */
struct fsctl_open {
    struct mutex lock;   /* the result, replaced by a write and copied by a read */
    void *result;
    size_t len;
};
_Static_assert(offsetof(struct fsctl_open, lock) == 0, "fsctl.cee's lock of a file");

/* fsctl.cee's size (0), result offset (1) and length offset (2). */
size_t fsctl_floor_open_layout(unsigned which)
{
    return which == 0 ? sizeof(struct fsctl_open)
         : which == 1 ? offsetof(struct fsctl_open, result)
                      : offsetof(struct fsctl_open, len);
}

static const struct chrdev_ops fsctl_ops = {
    .open = fsctl_open_file,
    .release = fsctl_release,
    .read_file = fsctl_read_file,
    .write_file = fsctl_write_file,
};

const struct chrdev_ops *fsctl_floor_ops(void)
{
    return &fsctl_ops;
}

spinlock_t *fsctl_floor_list_lock(void)
{
    return &g_mount_list_lock;
}
