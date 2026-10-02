/* shm.c - System V shared-memory segment registry.
 *
 * A segment's memory is an anonymous ramfs regular file (the memfd backing);
 * attaching one is mapping that file MAP_SHARED, which the Linux shmat door
 * does. This file owns only the key/id namespace and the lifecycle. The
 * segment record is reference counted: the registry holds one reference while
 * the segment is in the id/key table (not removed), each live attach holds
 * one, and a reference pins the backing vnode. IPC_RMID unlinks the segment
 * and drops the registry reference; the record and its pages are freed on the
 * last reference -- the last detach after removal -- which is SysV's
 * "destroyed on the last detach" obtained from reference counting alone. See
 * docs/audit/next-subsystem-shm.md.
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/list.h>
#include <kernel/panic.h>
#include <kernel/shm.h>
#include <kernel/spinlock.h>
#include <kernel/types.h>
#include <kernel/vfs.h>
#include <kernel/vmm.h>

#include "../../compat/linux/linux_abi.h"

struct shm_segment {
    struct list_node link;   /* in g_segments while not removed */
    int           id;
    int32_t       key;       /* 0 for an IPC_PRIVATE (keyless) segment */
    struct vnode *vn;        /* the anonymous ramfs file */
    size_t        size;      /* page-rounded */
    uint32_t      mode;      /* low nine bits of shmflg */
    uint32_t      cuid, cgid;
    uint32_t      nattch;    /* live attaches, for IPC_STAT */
    bool          removed;   /* IPC_RMID seen: unlinked, dropped on last ref */
    uint32_t      refs;      /* registry (while !removed) + one per live attach */
};

static spinlock_t g_shm_lock = SPINLOCK_INIT("shm-registry");
static struct list_node g_segments = LIST_HEAD_INIT(g_segments);
static int g_next_id = 1;

/* lock held */
static struct shm_segment *find_by_id_locked(int id)
{
    struct shm_segment *s;
    list_for_each_entry(s, &g_segments, link)
        if (s->id == id)
            return s;
    return NULL;
}

/* lock held */
static struct shm_segment *find_by_key_locked(int32_t key)
{
    struct shm_segment *s;
    list_for_each_entry(s, &g_segments, link)
        if (s->key == key)
            return s;
    return NULL;
}

/* lock held; returns the record to free outside the lock if this was its last
 * reference (it is already unlinked by then -- the registry reference, dropped
 * only by shm_rmid after it unlinks, is the one that keeps refs >= 1 while
 * linked). */
static struct shm_segment *unref_locked(struct shm_segment *s)
{
    KASSERT(s->refs > 0);
    return --s->refs == 0 ? s : NULL;
}

static void free_seg(struct shm_segment *s)
{
    vnode_put(s->vn);   /* the record's reference on the backing */
    kfree(s);
}

int shm_get(int32_t key, size_t size, unsigned shmflg, uint32_t uid, uint32_t gid)
{
    size_t rsize = page_align_up(size);

    if (key != LX_IPC_PRIVATE) {
        arch_irq_state_t s = spin_lock_irqsave(&g_shm_lock);
        struct shm_segment *seg = find_by_key_locked(key);
        if (seg != NULL) {
            int id = seg->id;
            size_t have = seg->size;
            spin_unlock_irqrestore(&g_shm_lock, s);
            if ((shmflg & LX_IPC_CREAT) && (shmflg & LX_IPC_EXCL))
                return -EEXIST;
            if (size > have)
                return -EINVAL;   /* asked for more than the segment holds */
            return id;
        }
        bool may_create = (shmflg & LX_IPC_CREAT) != 0;
        spin_unlock_irqrestore(&g_shm_lock, s);
        if (!may_create)
            return -ENOENT;
    }

    /* Create. The backing file is made outside the lock (it allocates and may
     * block); a keyed create re-checks for a racing creator under the lock. */
    if (size == 0 || rsize == 0 || rsize > (size_t)(VM_USER_HI - VM_USER_LO))
        return -EINVAL;
    struct vnode *vn;
    int rc = ramfs_anon_reg(shmflg & 0777, &vn);
    if (rc)
        return rc;
    rc = vfs_ftruncate(vn, rsize);
    if (rc) {
        vnode_put(vn);
        return rc;
    }
    struct shm_segment *seg = kzalloc(sizeof(*seg));
    if (seg == NULL) {
        vnode_put(vn);
        return -ENOMEM;
    }
    list_init(&seg->link);
    seg->key = (key == LX_IPC_PRIVATE) ? 0 : key;
    seg->vn = vn;
    seg->size = rsize;
    seg->mode = shmflg & 0777;
    seg->cuid = uid;
    seg->cgid = gid;
    seg->refs = 1;   /* the registry reference */

    arch_irq_state_t s = spin_lock_irqsave(&g_shm_lock);
    if (key != LX_IPC_PRIVATE) {
        struct shm_segment *race = find_by_key_locked(key);
        if (race != NULL) {
            int id = race->id;
            spin_unlock_irqrestore(&g_shm_lock, s);
            free_seg(seg);   /* discard ours; nothing else references it yet */
            return (shmflg & LX_IPC_EXCL) ? -EEXIST : id;
        }
    }
    seg->id = g_next_id++;
    list_push_back(&g_segments, &seg->link);
    int id = seg->id;
    spin_unlock_irqrestore(&g_shm_lock, s);
    return id;
}

int shm_lookup_ref(int shmid, struct shm_segment **out)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_shm_lock);
    struct shm_segment *seg = find_by_id_locked(shmid);
    if (seg != NULL)
        seg->refs++;
    spin_unlock_irqrestore(&g_shm_lock, s);
    if (seg == NULL)
        return -EINVAL;
    *out = seg;
    return 0;
}

struct vnode *shm_vnode(struct shm_segment *seg) { return seg->vn; }
size_t shm_size(struct shm_segment *seg) { return seg->size; }

void shm_attached(struct shm_segment *seg)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_shm_lock);
    seg->nattch++;
    spin_unlock_irqrestore(&g_shm_lock, s);
}

void shm_detach(struct shm_segment *seg)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_shm_lock);
    if (seg->nattch > 0)
        seg->nattch--;
    struct shm_segment *dead = unref_locked(seg);
    spin_unlock_irqrestore(&g_shm_lock, s);
    if (dead != NULL)
        free_seg(dead);
}

void shm_unref(struct shm_segment *seg)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_shm_lock);
    struct shm_segment *dead = unref_locked(seg);
    spin_unlock_irqrestore(&g_shm_lock, s);
    if (dead != NULL)
        free_seg(dead);
}

int shm_stat_id(int shmid, struct shm_stat *out)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_shm_lock);
    struct shm_segment *seg = find_by_id_locked(shmid);
    if (seg != NULL) {
        out->key = seg->key;
        out->size = seg->size;
        out->mode = seg->mode;
        out->cuid = seg->cuid;
        out->cgid = seg->cgid;
        out->nattch = seg->nattch;
    }
    spin_unlock_irqrestore(&g_shm_lock, s);
    return seg != NULL ? 0 : -EINVAL;
}

int shm_rmid(int shmid)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_shm_lock);
    struct shm_segment *seg = find_by_id_locked(shmid);
    if (seg == NULL) {
        spin_unlock_irqrestore(&g_shm_lock, s);
        return -EINVAL;
    }
    list_remove(&seg->link);
    seg->removed = true;
    struct shm_segment *dead = unref_locked(seg);   /* the registry reference */
    spin_unlock_irqrestore(&g_shm_lock, s);
    if (dead != NULL)
        free_seg(dead);
    return 0;
}
