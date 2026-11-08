/*
 * vfs_floor.c - The C under the VFS's CeeSafe parts (roadmap 2.0 SA7):
 * the values they write as their own constants, asserted; the mount
 * table, its lock, the namespaces' leaf lock and the root mount
 * (vfs_internal.h), the registry's and the claims' lists, the mount's
 * kobject type, the passes' wait queue and the page count, for
 * vfs_mount.cee; the vnode's kobject type, whose release is
 * vfs_vnode.cee's; a debug build's
 * poisoning of a freed vnode; the held-walk seam's lock, wait queues and
 * record, C data, and the timed killable wait it sleeps in (wait.h's
 * macro); and a release build's stand-ins for the seam, which only a
 * debug build has.
 */

#include <kernel/blk.h>
#include <kernel/cred.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/lockdep.h>
#include <kernel/mountns.h>
#include <kernel/object.h>
#include <kernel/pmm.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/vfs.h>
#include <kernel/wait.h>

#include <stddef.h>

#include "vfs_internal.h"

_Static_assert(EINTR == 4 && ETIMEDOUT == 110, "vfs_vnode.cee's errno.h values");
_Static_assert(VNODE_REG == 1 && VNODE_DIR == 2 && VNODE_DEAD == 2 && VNODE_HASH == 64 && PROCESS_NAME_MAX == 32,
               "vfs_vnode.cee's vfs.h and process.h values");
_Static_assert(sizeof(((struct process *)0)->pid) == 4 && sizeof(((struct process *)0)->nr_threads) == 4 &&
                   sizeof(((struct mount *)0)->nr_vnodes) == 4 && sizeof(((struct vnode *)0)->flags) == 4,
               "vfs_vnode.cee's field widths");

_Static_assert(SCHED_PRIO_DEFAULT == 32, "vfs_mount.cee's thread.h value");
_Static_assert(ENOENT == 2 && ENOMEM == 12 && EBUSY == 16 && EEXIST == 17 && ENODEV == 19 && ENOTDIR == 20 &&
                   EINVAL == 22 && ENOSPC == 28 && EOPNOTSUPP == 95 && KMEM_ZERO == 1,
               "vfs_mount.cee's errno.h and kmalloc.h values");
_Static_assert(VNODE_PINNED == 1 && VNODE_NESTED_CHILD == 2 && MOUNT_RDONLY == 1 && MOUNT_DETACHED == 8 &&
                   MOUNT_HELD_MAX == 12 && VFS_UMOUNT_FORCE == 1 && VFS_NAME_MAX == 255 && VFS_PATH_MAX == 1024 &&
                   BLKDEV_NAME_MAX == 16 && sizeof(((struct mount_ns_ref *)0)->path) == VFS_PATH_MAX,
               "vfs_mount.cee's vfs.h, lockdep.h and blk.h values");
_Static_assert(sizeof(((struct mount *)0)->nheld) == 4 && sizeof(((struct mount *)0)->flags) == 4 &&
                   sizeof(((struct mount *)0)->passes_running) == 8,
               "vfs_mount.cee's field widths");

_Static_assert(EPERM == 1 && EIO == 5 && ENXIO == 6 && EACCES == 13 && EISDIR == 21 && ETXTBSY == 26 &&
                   EROFS == 30 && ENAMETOOLONG == 36 && ELOOP == 40 && ENOTSUP == 95,
               "vfs_walk.cee's errno.h values");
_Static_assert(VNODE_SOCK == 5 && VNODE_LNK == 6 && VFS_MAX_COMPONENTS == 40 && VFS_MAX_SYMLINKS == 8 &&
                   VFS_MAY_EXEC == 1 && VFS_MAY_WRITE == 2 && VFS_MAY_READ == 4 && VFS_SET_MODE == 1 &&
                   VFS_SET_MTIME == 2 && VFS_SET_MTIME_NOW == 4 && VFS_ID_KEEP == 0xffffffffu &&
                   PRIV_SYS_ADMIN == 2 && CRED_ROOT_UID == 0,
               "vfs_walk.cee's vfs.h and cred.h values");
_Static_assert(COSMO_O_RDONLY == 0 && COSMO_O_WRONLY == 1 && COSMO_O_ACCMODE == 3 && COSMO_O_CREAT == 0x40 &&
                   COSMO_O_EXCL == 0x80 && COSMO_O_TRUNC == 0x200 && COSMO_O_DIRECTORY == 0x10000 &&
                   COSMO_O_NOFOLLOW == 0x20000,
               "vfs_walk.cee's syscall.h values");

_Static_assert(EBADF == 9 && EFAULT == 14 && ESPIPE == 29 && VNODE_CHR == 3 && VNODE_FIFO == 4 &&
                   COSMO_O_APPEND == 0x400 && COSMO_O_NONBLOCK == 0x800 && COSMO_IO_READABLE == 1 &&
                   COSMO_IO_WRITABLE == 2 && COSMO_SEEK_SET == 0 && COSMO_SEEK_CUR == 1 && COSMO_SEEK_END == 2,
               "vfs_file.cee's errno.h, vfs.h and syscall.h values");
_Static_assert(sizeof(struct cosmo_dirent) == 16 && offsetof(struct cosmo_dirent, name) == 12 &&
                   sizeof(((struct file *)0)->flags) == 4,
               "vfs_file.cee's dirent layout and field widths");

_Static_assert(EXDEV == 18 && EMLINK == 31 && VNODE_NESTED_PARENT2 == 1 && MOUNT_CACHE_IS_STORE == 2 &&
                   VFS_LINK_MAX == 65000 && PAGE_SIZE == 4096,
               "vfs_dir.cee's errno.h, lockdep.h, vfs.h and types.h values");

/* --- the file's object, a directory's listing, the driver's lock --------- */

void vfs_file_release(struct kobject *obj);
int64_t vfs_file_obj_read(struct kobject *obj, void *buf, size_t len);
int64_t vfs_file_obj_write(struct kobject *obj, const void *buf, size_t len);
int vfs_file_obj_flush(struct kobject *obj);
unsigned vfs_file_obj_ready(struct kobject *obj);
struct waitqueue *vfs_file_obj_poll_wq(struct kobject *obj, unsigned events);
int vfs_file_obj_set_nonblock(struct kobject *obj, int on);
int64_t vfs_file_obj_read_filled(struct kobject *obj, void *buf, size_t cap, size_t len,
                                 int (*put)(void *ctx, const void *piece, size_t n), void *ctx);

static const struct kobject_io_type file_type = {
    .base = { .name = "file", .release = vfs_file_release, .flags = KOBJECT_TYPE_IO },
    .read = vfs_file_obj_read,
    .read_filled = vfs_file_obj_read_filled,
    .write = vfs_file_obj_write,
    .flush = vfs_file_obj_flush,
    .ready = vfs_file_obj_ready,
    .poll_wq = vfs_file_obj_poll_wq,
    .set_nonblock = vfs_file_obj_set_nonblock,
};

const struct kobject_type *vfs_floor_file_type(void);
bool vfs_floor_is_file(const struct kobject *obj);
const struct kobject_type *vfs_floor_file_type(void) { return &file_type.base; }
bool vfs_floor_is_file(const struct kobject *obj) { return obj->type == &file_type.base; }

/* A stream's driver runs with no vnode lock held (lockdep.h's assertion, a
 * macro: nothing in a build without lockdep). */
void vfs_floor_assert_unlocked(struct vnode *vn);
void vfs_floor_assert_unlocked(struct vnode *vn)
{
    (void)vn;
    lockdep_assert_not_held(&vn->lock, LOCKDEP_KIND_MUTEX);
}

/* --- the mount table (vfs_internal.h), the registry and the claims ------- */

struct mutex g_mounts_lock;
spinlock_t g_mount_list_lock = SPINLOCK_INIT("mount-list");
LIST_HEAD(g_mounts);
struct mount *g_root_mount;
static LIST_HEAD(g_fs_types);
/* The block devices mounts hold (invariant V38): a leaf lock. */
static struct mutex g_claim_lock;
static LIST_HEAD(g_claims);
/* Orphans waiting for their last user (vfs_mount_orphaned), by `link`,
 * under g_mounts_lock; the reaper sleeps on the queue. */
static LIST_HEAD(g_orphans);
static struct waitqueue g_orphan_wq;

struct list_node *vfs_floor_mounts(void);
struct mutex *vfs_floor_mounts_lock(void);
spinlock_t *vfs_floor_mount_list_lock(void);
struct mount **vfs_floor_root_slot(void);
struct list_node *vfs_floor_fs_types(void);
struct list_node *vfs_floor_claims(void);
struct mutex *vfs_floor_claim_lock(void);
struct blkdev **vfs_floor_held(struct mount *mnt);

struct list_node *vfs_floor_mounts(void) { return &g_mounts; }
struct mutex *vfs_floor_mounts_lock(void) { return &g_mounts_lock; }
spinlock_t *vfs_floor_mount_list_lock(void) { return &g_mount_list_lock; }
struct mount **vfs_floor_root_slot(void) { return &g_root_mount; }
struct list_node *vfs_floor_fs_types(void) { return &g_fs_types; }
struct list_node *vfs_floor_claims(void) { return &g_claims; }
struct mutex *vfs_floor_claim_lock(void) { return &g_claim_lock; }
struct blkdev **vfs_floor_held(struct mount *mnt) { return mnt->held; }

struct list_node *vfs_floor_orphans(void);
struct waitqueue *vfs_floor_orphan_wq(void);
void vfs_floor_orphan_wait(bool (*cond)(void *), void *arg);
struct list_node *vfs_floor_orphans(void) { return &g_orphans; }
struct waitqueue *vfs_floor_orphan_wq(void) { return &g_orphan_wq; }
void vfs_floor_orphan_wait(bool (*cond)(void *), void *arg) { wait_event(&g_orphan_wq, cond(arg)); }

/* The mount's type: the release is vfs_mount.cee's. */
void vfs_mount_release_obj(struct kobject *obj);
static const struct kobject_type mount_type = {
    .name = "mount",
    .release = vfs_mount_release_obj,
};

const struct kobject_type *vfs_floor_mount_type(void);
const struct kobject_type *vfs_floor_mount_type(void)
{
    return &mount_type;
}

/* The passes' wait queue, which an unmount's drain sleeps on until the
 * last pass is released (wait.h's wait_event: the waiter is queued and
 * BLOCKED before the count is read, so no release is missed). */
void vfs_floor_passes_init(struct mount *mnt);
void vfs_floor_passes_wait(struct mount *mnt);
void vfs_floor_passes_wake(struct mount *mnt);
void vfs_floor_passes_init(struct mount *mnt) { waitqueue_init(&mnt->passes_quiet, "mount-passes"); }
void vfs_floor_passes_wait(struct mount *mnt)
{
    wait_event(&mnt->passes_quiet, __atomic_load_n(&mnt->passes_running, __ATOMIC_ACQUIRE) == 0);
}
void vfs_floor_passes_wake(struct mount *mnt) { waitqueue_wake_all(&mnt->passes_quiet); }

/* The pages the buddy manages: the page cache's limit is a quarter. */
uint64_t vfs_floor_total_pages(void);
uint64_t vfs_floor_total_pages(void)
{
    struct pmm_stats pst;
    pmm_get_stats(&pst);
    return pst.total_pages;
}

/* The vnode's type: the release is vfs_vnode.cee's. */
void vfs_vnode_release(struct kobject *obj);
static const struct kobject_type vnode_type = {
    .name = "vnode",
    .release = vfs_vnode_release,
};

const struct kobject_type *vfs_floor_vnode_type(void);
const struct kobject_type *vfs_floor_vnode_type(void)
{
    return &vnode_type;
}

/* A freed vnode that still looks like a directory hides a use-after-free;
 * one full of 0x5a does not. The pmm poisons freed frames for the same
 * reason (kernel/memory/pmm.c) and the slab poisons nothing, so the
 * cwd-ref unit's regression test passed on a broken kernel every time
 * until this existed (docs/audit/next-subsystem-cwd-hold.md, "Measured"). */
void vfs_floor_vnode_poison(struct vnode *vn);
void vfs_floor_vnode_poison(struct vnode *vn)
{
#if CONFIG_DEBUG
    memset(vn, 0x5a, sizeof(*vn));
#else
    (void)vn;
#endif
}

/* --- the held-walk seam (docs/audit/next-subsystem-cwd-hold.md) --------- */

/* The seam's C data, in every build: the CeeSafe helpers that reach it are
 * compiled in a release build too, where nothing calls them. */
static spinlock_t g_cwd_lock = SPINLOCK_INIT("vfs-cwd-hold");
static struct waitqueue g_cwd_held_wq;   /* the walk waits here for the put */
static struct waitqueue g_cwd_swap_wq;   /* the swapper waits here for the hold */
static struct vfs_cwd_hold_record g_cwd_rec;

spinlock_t *vfs_floor_cwd_lock(void);
struct waitqueue *vfs_floor_cwd_held_wq(void);
struct waitqueue *vfs_floor_cwd_swap_wq(void);
struct vfs_cwd_hold_record *vfs_floor_cwd_record(void);
int vfs_floor_wait_killable_timeout(struct waitqueue *wq, bool (*cond)(void *), void *arg, uint64_t ns);

spinlock_t *vfs_floor_cwd_lock(void) { return &g_cwd_lock; }
struct waitqueue *vfs_floor_cwd_held_wq(void) { return &g_cwd_held_wq; }
struct waitqueue *vfs_floor_cwd_swap_wq(void) { return &g_cwd_swap_wq; }
struct vfs_cwd_hold_record *vfs_floor_cwd_record(void) { return &g_cwd_rec; }

/* 0, -EINTR when the process is being killed, -ETIMEDOUT after `ns`. */
int vfs_floor_wait_killable_timeout(struct waitqueue *wq, bool (*cond)(void *), void *arg, uint64_t ns)
{
    return wait_event_killable_timeout(wq, cond(arg), ns);
}

#if !CONFIG_DEBUG
/* A release build has no seam: every export answers as an idle one. */
void vfs_test_cwd_hold_arm(const char *process_name) { (void)process_name; }
unsigned vfs_test_cwd_hold_state(void) { return 0; }
void vfs_test_cwd_hold_disarm(struct vfs_cwd_hold_record *out) { if (out) memset(out, 0, sizeof(*out)); }
void vfs_cwd_hold_swapper_enter(void) {}
void vfs_cwd_hold_swap_wait(void) {}
void vfs_cwd_hold_before_put(struct vnode *old) { (void)old; }
void vfs_cwd_hold_after_put(struct vnode *old) { (void)old; }
void vfs_cwd_hold_swapper_leave(void) {}
int vfs_cwd_hold_walk(struct vnode *start);
int vfs_cwd_hold_walk(struct vnode *start) { (void)start; return 0; }
#endif
