/*
 * vfs_floor.c - The C under the VFS's CeeSafe parts (roadmap 2.0 SA7):
 * the values they write as their own constants, asserted; the vnode's
 * kobject type, whose release is vfs_vnode.cee's; a debug build's
 * poisoning of a freed vnode; the held-walk seam's lock, wait queues and
 * record, C data, and the timed killable wait it sleeps in (wait.h's
 * macro); and a release build's stand-ins for the seam, which only a
 * debug build has.
 */

#include <kernel/errno.h>
#include <kernel/object.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/timer.h>
#include <kernel/vfs.h>
#include <kernel/wait.h>

_Static_assert(EINTR == 4 && ETIMEDOUT == 110, "vfs_vnode.cee's errno.h values");
_Static_assert(VNODE_REG == 1 && VNODE_DIR == 2 && VNODE_DEAD == 2 && VNODE_HASH == 64 && PROCESS_NAME_MAX == 32,
               "vfs_vnode.cee's vfs.h and process.h values");
_Static_assert(sizeof(((struct process *)0)->pid) == 4 && sizeof(((struct process *)0)->nr_threads) == 4 &&
                   sizeof(((struct mount *)0)->nr_vnodes) == 4 && sizeof(((struct vnode *)0)->flags) == 4,
               "vfs_vnode.cee's field widths");

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
