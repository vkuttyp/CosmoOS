/*
 * shim_fs.c - Host services for running cosmofs (cosmofs_core.c and
 * cosmofs.c, unchanged) under the fuzzer (docs/verification/design.md).
 *
 * Supplies: the storage pool over a memory image, the VFS services the
 * filesystem glue calls (vnode allocation, hash, references, page-cache
 * entry points, credentials, time), no-op mutexes (one thread) and a
 * plain-count kobject. The heap is the kernel's slab and kmalloc over the
 * harness's page arena; panic and klog come from harness.c.
 */

#include "shim_fs.h"

#include <kernel/blk.h>
#include <kernel/cred.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/mutex.h>
#include <kernel/object.h>
#include <kernel/pagecache.h>
#include <kernel/panic.h>
#include <kernel/storage.h>
#include <kernel/string.h>
#include <kernel/vfs.h>

#include <stdlib.h>
#include <string.h>

/* --- the memory image behind the pool --- */

static uint8_t *g_image;
static uint64_t g_image_blocks;

void shim_image_set(const uint8_t *data, size_t size, uint64_t nblocks)
{
    free(g_image);
    g_image_blocks = nblocks;
    g_image = calloc(nblocks, POOL_BLOCK);
    size_t n = size < nblocks * POOL_BLOCK ? size : nblocks * POOL_BLOCK;
    memcpy(g_image, data, n);
}

const uint8_t *shim_image_get(uint64_t *nblocks)
{
    *nblocks = g_image_blocks;
    return g_image;
}

int pool_open(struct blkdev *bd, struct spool **out)
{
    if (bd == NULL || bd->sector_size == 0 || POOL_BLOCK % bd->sector_size != 0)
        return -EINVAL;
    struct spool *p = kzalloc(sizeof(*p));
    if (p == NULL)
        return -ENOMEM;
    p->dev = bd;
    p->block_size = POOL_BLOCK;
    p->sectors_per_block = POOL_BLOCK / bd->sector_size;
    p->nblocks = bd->capacity / p->sectors_per_block;
    if (p->nblocks > g_image_blocks)
        p->nblocks = g_image_blocks;
    *out = p;
    return 0;
}

void pool_close(struct spool *p)
{
    kfree(p);
}

int pool_read(struct spool *p, uint64_t blk, void *buf)
{
    if (blk >= p->nblocks)
        return -EINVAL;
    p->reads++;
    memcpy(buf, g_image + blk * POOL_BLOCK, POOL_BLOCK);
    return 0;
}

int pool_write(struct spool *p, uint64_t blk, const void *buf)
{
    if (blk >= p->nblocks)
        return -EINVAL;
    p->writes++;
    memcpy(g_image + blk * POOL_BLOCK, buf, POOL_BLOCK);
    return 0;
}

int pool_flush(struct spool *p)
{
    p->flushes++;
    return 0;
}

/* --- credentials and time --- */

const struct credentials cred_kernel = { 0 };

const struct credentials *cred_current(void)
{
    return &cred_kernel;
}

uint64_t vfs_now_ns(void)
{
    static uint64_t t;
    return t += 1000;
}

int vfs_register_fs(struct fs_type *fs)
{
    (void)fs;
    return 0;
}

/* --- mutexes: one thread, so ownership is all that is tracked --- */

void mutex_init(struct mutex *m, const char *name)
{
    memset(m, 0, sizeof(*m));
    m->name = name;
}

void mutex_lock(struct mutex *m)
{
    if (m->owner)
        panic("host mutex '%s' locked twice", m->name ? m->name : "?");
    m->owner = (struct thread *)m;
}

void mutex_lock_nested(struct mutex *m, unsigned subclass)
{
    (void)subclass;
    mutex_lock(m);
}

bool mutex_trylock(struct mutex *m)
{
    if (m->owner)
        return false;
    m->owner = (struct thread *)m;
    return true;
}

void mutex_unlock(struct mutex *m)
{
    if (m->owner == NULL)
        panic("host mutex '%s' unlocked while free", m->name ? m->name : "?");
    m->owner = NULL;
}

bool mutex_is_locked(struct mutex *m)
{
    return m->owner != NULL;
}

/* --- kobjects: a plain count --- */

void kobject_init(struct kobject *obj, const struct kobject_type *type)
{
    obj->type = type;
    obj->refcount = 1;
    obj->owner = NULL;
}

void kobject_get(struct kobject *obj)
{
    if (obj->refcount == 0)
        panic("kobject_get on a released object");
    obj->refcount++;
}

bool kobject_tryget(struct kobject *obj)
{
    if (obj->refcount == 0)
        return false;
    obj->refcount++;
    return true;
}

void kobject_put(struct kobject *obj)
{
    if (obj->refcount == 0)
        panic("kobject_put underflow");
    if (--obj->refcount == 0)
        obj->type->release(obj);
}

uint32_t kobject_refcount(const struct kobject *obj)
{
    return obj->refcount;
}

void kobject_track_code(struct kobject *obj, uintptr_t code)
{
    (void)obj;
    (void)code;
}

/* --- vnodes and the mount's hash, as vfs.c does them --- */

static void vnode_release(struct kobject *obj)
{
    struct vnode *vn = container_of(obj, struct vnode, obj);
    if (!list_empty(&vn->hash_link))
        panic("vnode released while hashed");
    if (vn->ops && vn->ops->evict)
        vn->ops->evict(vn);
    kfree(vn);
}

static const struct kobject_type vnode_type = { .name = "vnode", .release = vnode_release };

struct vnode *vnode_alloc(struct mount *mnt, uint64_t ino)
{
    struct vnode *vn = kzalloc(sizeof(*vn));
    if (vn == NULL)
        return NULL;
    kobject_init(&vn->obj, &vnode_type);
    vn->mnt = mnt;
    vn->ino = ino;
    vn->nlink = 1;
    vn->mtime_ns = vn->ctime_ns = vfs_now_ns();
    mutex_init(&vn->lock, "vnode");
    pagecache_init(&vn->pc);
    list_init(&vn->hash_link);
    return vn;
}

void vnode_hash_insert(struct vnode *vn)
{
    struct mount *mnt = vn->mnt;
    list_push_back(&mnt->vnodes[vn->ino % VNODE_HASH], &vn->hash_link);
    mnt->nr_vnodes++;
}

struct vnode *vnode_lookup_cached(struct mount *mnt, uint64_t ino)
{
    struct vnode *vn;
    list_for_each_entry(vn, &mnt->vnodes[ino % VNODE_HASH], hash_link) {
        if (vn->ino == ino) {
            vnode_get(vn);
            return vn;
        }
    }
    return NULL;
}

void vnode_put(struct vnode *vn)
{
    if (kobject_refcount(&vn->obj) == 1 && !list_empty(&vn->hash_link)) {
        list_remove(&vn->hash_link);
        list_init(&vn->hash_link);
        vn->mnt->nr_vnodes--;
    }
    kobject_put(&vn->obj);
}

void vnode_stat(struct vnode *vn, struct cosmo_stat *st)
{
    memset(st, 0, sizeof(*st));
    st->ino = vn->ino;
    st->size = vn->size;
}

/* --- page cache: the filesystem never gets a page through it here --- */

void pagecache_init(struct pagecache *pc)
{
    memset(pc, 0, sizeof(*pc));
    mutex_init(&pc->lock, "pagecache");
}

int pagecache_sync(struct vnode *vn)
{
    (void)vn;
    return 0;
}

void pagecache_truncate(struct vnode *vn, uint64_t size)
{
    (void)vn;
    (void)size;
}

void pagecache_drop(struct vnode *vn)
{
    (void)vn;
}
