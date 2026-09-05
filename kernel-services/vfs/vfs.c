/*
 * vfs.c - Mount table, path resolution, vnodes, files.
 *
 * Every entry validates before it calls a filesystem; every filesystem
 * callback runs with the vnode locks described in
 * docs/kernel-services/vfs/design.md ("Locking").
 */

#include <kernel/blk.h>
#include <kernel/cred.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/lockdep.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/printf.h>
#include <kernel/string.h>
#include <kernel/timer.h>
#include <kernel/vfs.h>

static struct mutex g_mounts_lock;
static LIST_HEAD(g_fs_types);
static LIST_HEAD(g_mounts);
static struct mount *g_root_mount;
static unsigned g_nr_mounts;
static bool g_initialized;

uint64_t vfs_now_ns(void)
{
    return clock_now_ns();
}

/* --- vnodes -------------------------------------------------------------- */

static void vnode_release(struct kobject *obj)
{
    struct vnode *vn = container_of(obj, struct vnode, obj);

    /* vnode_put unhashed it before dropping the last reference, so no
     * lookup can find it now; the release takes no mount lock. */
    KASSERT(list_empty(&vn->hash_link));

    /* A vnode whose filesystem gave up between vnode_alloc and setting
     * `ops` (an allocation failed) is released with ops NULL: found by
     * fault-kmalloc as a NULL dereference in ramfs_new's failure path. */
    if (vn->type == VNODE_REG) {
        if (vn->pc.nr_dirty && vn->ops && vn->ops->writepage)
            pagecache_sync(vn);
        pagecache_drop(vn);
    }
    if (vn->ops && vn->ops->evict)
        vn->ops->evict(vn);
    kfree(vn);
}

static const struct kobject_type vnode_type = {
    .name = "vnode",
    .release = vnode_release,
};

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
    arch_irq_state_t s = spin_lock_irqsave(&mnt->lock);
    list_push_back(&mnt->vnodes[vn->ino % VNODE_HASH], &vn->hash_link);
    mnt->nr_vnodes++;
    spin_unlock_irqrestore(&mnt->lock, s);
}

/*
 * A hashed vnode always holds at least one reference: vnode_put unhashes
 * under the hash lock before the count can reach zero (below). So a plain
 * get here is safe, and there is no window in which a vnode at count zero
 * is skipped and a second vnode instantiated for the inode (the audit's
 * check-then-get finding).
 */
struct vnode *vnode_lookup_cached(struct mount *mnt, uint64_t ino)
{
    arch_irq_state_t s = spin_lock_irqsave(&mnt->lock);
    struct vnode *vn;
    list_for_each_entry(vn, &mnt->vnodes[ino % VNODE_HASH], hash_link) {
        if (vn->ino == ino) {
            vnode_get(vn);
            spin_unlock_irqrestore(&mnt->lock, s);
            return vn;
        }
    }
    spin_unlock_irqrestore(&mnt->lock, s);
    return NULL;
}

void vnode_put(struct vnode *vn)
{
    if (kobject_refcount(&vn->obj) == 1) {
        /* Ours is the only reference, so the count can only rise, and only
         * through a hash lookup, which needs this lock: once we hold it a
         * re-read of 1 is final and the unhash is safe. */
        struct mount *mnt = vn->mnt;
        arch_irq_state_t s = spin_lock_irqsave(&mnt->lock);
        if (kobject_refcount(&vn->obj) == 1 && !list_empty(&vn->hash_link)) {
            list_remove(&vn->hash_link);
            list_init(&vn->hash_link);
            mnt->nr_vnodes--;
        }
        spin_unlock_irqrestore(&mnt->lock, s);
    }
    kobject_put(&vn->obj);
}

void vnode_stat(struct vnode *vn, struct cosmo_stat *st)
{
    memset(st, 0, sizeof(*st));
    st->ino = vn->ino;
    st->type = (uint32_t)vn->type;
    st->mode = vn->mode;
    st->nlink = vn->nlink;
    st->uid = vn->uid;
    st->gid = vn->gid;
    st->size = vn->size;
    st->mtime_ns = vn->mtime_ns;
    st->ctime_ns = vn->ctime_ns;
}

/* --- mounts -------------------------------------------------------------- */

static void mount_release(struct kobject *obj)
{
    struct mount *mnt = container_of(obj, struct mount, obj);
    kfree(mnt);
}

static const struct kobject_type mount_type = {
    .name = "mount",
    .release = mount_release,
};

int vfs_register_fs(struct fs_type *fs)
{
    if (fs->name == NULL || fs->mount == NULL || fs->unmount == NULL)
        return -EINVAL;
    mutex_lock(&g_mounts_lock);
    struct fs_type *f;
    list_for_each_entry(f, &g_fs_types, link) {
        if (strcmp(f->name, fs->name) == 0) {
            mutex_unlock(&g_mounts_lock);
            return -EEXIST;
        }
    }
    list_init(&fs->link);
    list_push_back(&g_fs_types, &fs->link);
    mutex_unlock(&g_mounts_lock);
    kdebug("vfs: filesystem %s registered", fs->name);
    return 0;
}

struct fs_type *vfs_find_fs(const char *name)
{
    mutex_lock(&g_mounts_lock);
    struct fs_type *f, *found = NULL;
    list_for_each_entry(f, &g_fs_types, link) {
        if (strcmp(f->name, name) == 0) {
            found = f;
            break;
        }
    }
    mutex_unlock(&g_mounts_lock);
    return found;
}

static struct mount *mount_alloc(struct fs_type *fs, struct blkdev *bdev, unsigned flags)
{
    struct mount *mnt = kzalloc(sizeof(*mnt));
    if (mnt == NULL)
        return NULL;
    kobject_init(&mnt->obj, &mount_type);
    mnt->fs = fs;
    mnt->bdev = bdev;
    mnt->flags = flags;
    mnt->next_ino = 1;
    for (unsigned i = 0; i < VNODE_HASH; i++)
        list_init(&mnt->vnodes[i]);
    spinlock_init(&mnt->lock, "mount-hash");
    mutex_init(&mnt->rename_lock, "rename");
    mutex_init(&mnt->sync_lock, "mount-sync");
    list_init(&mnt->link);
    return mnt;
}

struct vnode *vfs_root(void)
{
    KASSERT(g_root_mount != NULL);
    vnode_get(g_root_mount->root);
    return g_root_mount->root;
}

/* Instantiate the filesystem. On success the mount holds its root. */
static int do_mount(struct fs_type *fs, struct blkdev *bdev, unsigned flags, struct mount **out)
{
    struct mount *mnt = mount_alloc(fs, bdev, flags);
    if (mnt == NULL)
        return -ENOMEM;
    int rc = fs->mount(fs, bdev, flags, mnt);
    if (rc) {
        kobject_put(&mnt->obj);
        return rc;
    }
    KASSERT(mnt->root != NULL && mnt->root->type == VNODE_DIR);
    if (bdev)
        blkdev_get(bdev);
    *out = mnt;
    return 0;
}

int vfs_mount(const char *path, const char *fsname, struct blkdev *bdev, unsigned flags)
{
    KASSERT(g_initialized);
    struct fs_type *fs = vfs_find_fs(fsname);
    if (fs == NULL)
        return -ENODEV;

    struct vnode *dir;
    int rc = vfs_lookup(NULL, path, &dir);
    if (rc)
        return rc;
    if (dir->type != VNODE_DIR) {
        vnode_put(dir);
        return -ENOTDIR;
    }

    struct mount *mnt;
    rc = do_mount(fs, bdev, flags, &mnt);
    if (rc) {
        vnode_put(dir);
        return rc;
    }

    mutex_lock(&g_mounts_lock);
    mutex_lock(&dir->lock);
    /* No stacking: a directory that is already a mountpoint (the lookup
     * followed it, so `dir` is that mount's root) or the global root. */
    if (dir->covered_by != NULL || dir->mnt->root == dir) {
        mutex_unlock(&dir->lock);
        mutex_unlock(&g_mounts_lock);
        fs->unmount(mnt);
        vnode_put(mnt->root);
        if (bdev)
            blkdev_put(bdev);
        kobject_put(&mnt->obj);
        vnode_put(dir);
        return -EBUSY;
    }
    mnt->mountpoint = dir;          /* keeps the lookup reference */
    mnt->parent = dir->mnt;
    dir->covered_by = mnt;
    mutex_unlock(&dir->lock);
    list_push_back(&g_mounts, &mnt->link);
    g_nr_mounts++;
    mutex_unlock(&g_mounts_lock);
    kinfo("vfs: mounted %s on %s from %s", fsname, path, bdev ? bdev->name : "memory");
    return 0;
}

int vfs_umount2(const char *path, unsigned flags)
{
    struct vnode *root;
    int rc = vfs_lookup(NULL, path, &root);
    if (rc)
        return rc;
    struct mount *mnt = root->mnt;
    if (mnt->root != root) {
        vnode_put(root);
        return -EINVAL;   /* not a mount root */
    }
    if (mnt == g_root_mount) {
        vnode_put(root);
        return -EBUSY;    /* the root filesystem stays */
    }
    vnode_put(root);   /* the lookup reference; the mount still holds one */

    mutex_lock(&g_mounts_lock);
    /* Turn new walkers away first: follow_mount refuses (-EBUSY) while
     * `unmounting` is set and takes the root reference under the same
     * lock, so the reference scan below is final and nothing falls
     * through to the covered directory while the decision is pending. */
    struct vnode *mp = mnt->mountpoint;
    mutex_lock(&mp->lock);
    mnt->unmounting = true;
    mutex_unlock(&mp->lock);

    /* Busy if any vnode is referenced beyond what the filesystem itself
     * holds: a pinned vnode's own pin, the mount's reference on the root. */
    arch_irq_state_t hs = spin_lock_irqsave(&mnt->lock);
    bool busy = false;
    for (unsigned b = 0; b < VNODE_HASH && !busy; b++) {
        struct vnode *vn;
        list_for_each_entry(vn, &mnt->vnodes[b], hash_link) {
            uint32_t own = ((vn->flags & VNODE_PINNED) ? 1u : 0u) + (vn == mnt->root ? 1u : 0u);
            if (kobject_refcount(&vn->obj) > own) {
                busy = true;
                break;
            }
        }
    }
    spin_unlock_irqrestore(&mnt->lock, hs);
    if (busy) {
        rc = -EBUSY;
        goto restore;
    }
    /* Commit while the mount is still whole: if that fails the mount stays
     * and the caller can retry (or fix the device); nothing is lost. */
    if (mnt->fs->sync && !(flags & VFS_UMOUNT_FORCE)) {
        rc = mnt->fs->sync(mnt);
        if (rc) {
            kerror("vfs: %s: commit failed (%d); mount kept (VFS_UMOUNT_FORCE drops the transaction)", path,
                   rc);
            goto restore;
        }
    }
    if (flags & VFS_UMOUNT_FORCE)
        kwarn("vfs: %s: forced unmount, open transaction dropped", path);
    mutex_lock(&mp->lock);
    mp->covered_by = NULL;
    mutex_unlock(&mp->lock);
    list_remove(&mnt->link);
    g_nr_mounts--;
    mutex_unlock(&g_mounts_lock);

    /* The filesystem releases its pins and private state while the root
     * is still alive; the root's eviction follows and must tolerate
     * mnt->fs_priv being gone. Under sync_lock: a vfs_sync that snapshotted
     * this mount either finishes its fs->sync first or sees `unmounted`. */
    mutex_lock(&mnt->sync_lock);
    int urc = mnt->fs->unmount(mnt);
    mnt->unmounted = true;
    mutex_unlock(&mnt->sync_lock);
    struct vnode *root_vn = mnt->root;
    mnt->root = NULL;
    vnode_put(root_vn);
    if (mnt->bdev)
        blkdev_put(mnt->bdev);
    vnode_put(mp);
    if (urc)
        kerror("vfs: %s: the filesystem reported %d while unmounting; uncommitted changes may be lost", path,
               urc);
    else
        kinfo("vfs: unmounted %s", path);
    kobject_put(&mnt->obj);
    return urc;

restore:
    /* Refused or failed: the mount is whole again and reachable. */
    mutex_lock(&mp->lock);
    mnt->unmounting = false;
    mutex_unlock(&mp->lock);
    mutex_unlock(&g_mounts_lock);
    return rc;
}

/*
 * Sync every mount without holding the mount list across a commit (a
 * cosmofs commit is block I/O; mount and unmount must not wait behind it).
 * The k-th mount is taken by position under the list lock with a
 * reference, synced under its own sync_lock (which unmount also takes
 * around fs->unmount, so a mount that went away is seen as `unmounted`
 * and skipped), and put. A mount unmounted meanwhile shifts the
 * positions: the one that moved into its place is synced again, which is
 * harmless, and the unmounted one was committed by vfs_umount itself.
 */
int vfs_sync(void)
{
    int rc = 0;
    for (unsigned k = 0;; k++) {
        mutex_lock(&g_mounts_lock);
        struct mount *mnt = NULL, *it;
        unsigned i = 0;
        list_for_each_entry(it, &g_mounts, link) {
            if (i++ == k) {
                mnt = it;
                kobject_get(&mnt->obj);
                break;
            }
        }
        mutex_unlock(&g_mounts_lock);
        if (mnt == NULL)
            break;
        mutex_lock(&mnt->sync_lock);
        if (mnt->fs->sync && !mnt->unmounted) {
            int r = mnt->fs->sync(mnt);
            if (r && rc == 0)
                rc = r;
        }
        mutex_unlock(&mnt->sync_lock);
        kobject_put(&mnt->obj);
    }
    return rc;
}

/* --- path walk ------------------------------------------------------------ */

/* Cross into a mount whose root covers *vnp. Consumes the reference on
 * failure. -EBUSY while an unmount of that mount is being decided: the
 * walker must not fall through to the covered directory, since the
 * unmount may yet be refused and the mount restored. */
static int follow_mount(struct vnode **vnp)
{
    struct vnode *dir = *vnp;
    for (unsigned depth = 0; depth < 16; depth++) {
        /* Taken under dir->lock: vfs_umount sets `unmounting` and, when
         * it proceeds, clears covered_by under the same lock, so a walker
         * either already holds the root (the reference scan sees it) or
         * is turned away. */
        mutex_lock(&dir->lock);
        struct mount *m = dir->covered_by;
        if (m && m->unmounting) {
            mutex_unlock(&dir->lock);
            vnode_put(dir);
            return -EBUSY;
        }
        struct vnode *root = m ? m->root : NULL;
        if (root)
            vnode_get(root);
        mutex_unlock(&dir->lock);
        if (root == NULL)
            break;
        vnode_put(dir);
        dir = root;
    }
    *vnp = dir;
    return 0;
}

/* --- permissions ------------------------------------------------------------ */

int vfs_permission(const struct vnode *vn, unsigned mask)
{
    const struct credentials *c = cred_current();
    if (cred_privileged(c)) {
        /* Root reads, writes and searches everything; executing a regular
         * file still needs it to be marked executable by someone. */
        if ((mask & VFS_MAY_EXEC) && vn->type == VNODE_REG && (vn->mode & 0111) == 0)
            return -EACCES;
        return 0;
    }
    unsigned bits;
    if (c->euid == vn->uid)
        bits = (vn->mode >> 6) & 7;
    else if (cred_in_group(c, vn->gid))
        bits = (vn->mode >> 3) & 7;
    else
        bits = vn->mode & 7;
    return (bits & mask) == mask ? 0 : -EACCES;
}

/* Search permission on a directory being entered; consumes `dir` on failure. */
static int may_search(struct vnode *dir)
{
    int rc = vfs_permission(dir, VFS_MAY_EXEC);
    if (rc)
        vnode_put(dir);
    return rc;
}

/* One component from `dir` (referenced, consumed). Returns a referenced
 * child. Entering a directory needs search permission on it. */
static int step(struct vnode *dir, const char *name, size_t len, struct vnode **out)
{
    if (len == 1 && name[0] == '.') {
        int rc = may_search(dir);
        if (rc)
            return rc;
        *out = dir;
        return 0;
    }
    if (len == 2 && name[0] == '.' && name[1] == '.') {
        /* At a mount root, ".." leaves through the mountpoint. */
        struct vnode *base = dir;
        if (dir->mnt->root == dir && dir->mnt->mountpoint) {
            base = dir->mnt->mountpoint;
            vnode_get(base);
            vnode_put(dir);
        }
        int rc = may_search(base);
        if (rc)
            return rc;
        if (base->mnt->root == base) {   /* the global root: stays put */
            *out = base;
            return 0;
        }
        mutex_lock(&base->lock);
        rc = base->ops->lookup(base, "..", 2, out);
        mutex_unlock(&base->lock);
        vnode_put(base);
        return rc;
    }
    int prc = may_search(dir);
    if (prc)
        return prc;
    mutex_lock(&dir->lock);
    int rc = (dir->flags & VNODE_DEAD) ? -ENOENT : dir->ops->lookup(dir, name, len, out);
    mutex_unlock(&dir->lock);
    vnode_put(dir);
    if (rc)
        return rc;
    return follow_mount(out);
}

/* Walk every component but the last. On success *parent is referenced
 * and the last component is returned through last and last_len (empty
 * for the root). */
static int walk_parent(struct vnode *start, const char *path, struct vnode **parent, const char **last,
                       size_t *last_len, bool *trailing_slash)
{
    if (path == NULL || strnlen(path, VFS_PATH_MAX) >= VFS_PATH_MAX)
        return -ENAMETOOLONG;
    if (trailing_slash)
        *trailing_slash = false;

    struct vnode *cur;
    if (path[0] == '/' || start == NULL) {
        cur = vfs_root();
    } else {
        cur = start;
        vnode_get(cur);
    }
    while (*path == '/')
        path++;

    unsigned components = 0;
    for (;;) {
        const char *end = path;
        while (*end && *end != '/')
            end++;
        size_t len = (size_t)(end - path);
        const char *next = end;
        while (*next == '/')
            next++;

        if (len == 0) {              /* the root itself, or a trailing separator */
            *parent = cur;
            *last = path;
            *last_len = 0;
            return 0;
        }
        if (len > VFS_NAME_MAX) {
            vnode_put(cur);
            return -ENAMETOOLONG;
        }
        if (*next == '\0') {
            *parent = cur;
            *last = path;
            *last_len = len;
            if (trailing_slash)
                *trailing_slash = *end == '/';
            return 0;
        }
        if (++components > VFS_MAX_COMPONENTS) {
            vnode_put(cur);
            return -ELOOP;
        }
        if (cur->type != VNODE_DIR) {
            vnode_put(cur);
            return -ENOTDIR;
        }
        struct vnode *child;
        int rc = step(cur, path, len, &child);
        if (rc)
            return rc;
        cur = child;
        path = next;
    }
}

int vfs_lookup(struct vnode *start, const char *path, struct vnode **out)
{
    struct vnode *parent;
    const char *last;
    size_t len;
    bool trailing;
    int rc = walk_parent(start, path, &parent, &last, &len, &trailing);
    if (rc)
        return rc;
    if (len == 0) {
        *out = parent;
        return 0;
    }
    if (parent->type != VNODE_DIR) {
        vnode_put(parent);
        return -ENOTDIR;
    }
    rc = step(parent, last, len, out);
    if (rc == 0 && trailing && (*out)->type != VNODE_DIR) {
        vnode_put(*out);
        return -ENOTDIR;
    }
    return rc;
}

/* --- files ---------------------------------------------------------------- */

static void file_release(struct kobject *obj)
{
    struct file *f = container_of(obj, struct file, obj);
    if (f->vn->type == VNODE_REG && f->vn->pc.nr_dirty && f->vn->ops->writepage) {
        mutex_lock(&f->vn->lock);
        pagecache_sync(f->vn);
        mutex_unlock(&f->vn->lock);
    }
    vnode_put(f->vn);
    kfree(f);
}

static int64_t file_obj_read(struct kobject *obj, void *buf, size_t len)
{
    return file_read(container_of(obj, struct file, obj), buf, len);
}

static int64_t file_obj_write(struct kobject *obj, const void *buf, size_t len)
{
    return file_write(container_of(obj, struct file, obj), buf, len);
}

static const struct kobject_io_type file_type = {
    .base = { .name = "file", .release = file_release },
    .read = file_obj_read,
    .write = file_obj_write,
};

struct file *file_from_kobject(struct kobject *obj)
{
    return obj->type == &file_type.base ? container_of(obj, struct file, obj) : NULL;
}

static struct file *file_alloc(struct vnode *vn, unsigned flags)
{
    struct file *f = kzalloc(sizeof(*f));
    if (f == NULL)
        return NULL;
    kobject_init(&f->obj, &file_type.base);
    f->vn = vn;   /* takes the caller's reference */
    f->flags = flags;
    mutex_init(&f->lock, "file");
    return f;
}

int vfs_open(struct vnode *start, const char *path, unsigned flags, uint32_t mode, struct file **out)
{
    unsigned acc = flags & COSMO_O_ACCMODE;
    if (acc == COSMO_O_ACCMODE)
        return -EINVAL;

    struct vnode *parent;
    const char *last;
    size_t len;
    bool trailing;
    int rc = walk_parent(start, path, &parent, &last, &len, &trailing);
    if (rc)
        return rc;
    if (trailing)
        flags |= COSMO_O_DIRECTORY;

    struct vnode *vn = NULL;
    bool created = false;
    if (len == 0) {
        vn = parent;   /* the root itself */
    } else {
        if (parent->type != VNODE_DIR) {
            vnode_put(parent);
            return -ENOTDIR;
        }
        if ((len == 1 && last[0] == '.') || (len == 2 && last[0] == '.' && last[1] == '.')) {
            rc = step(parent, last, len, &vn);
            if (rc)
                return rc;
        } else {
            rc = vfs_permission(parent, VFS_MAY_EXEC);   /* search the last directory */
            if (rc) {
                vnode_put(parent);
                return rc;
            }
            mutex_lock(&parent->lock);
            rc = (parent->flags & VNODE_DEAD) ? -ENOENT : parent->ops->lookup(parent, last, len, &vn);
            if (rc == -ENOENT && (flags & COSMO_O_CREAT)) {
                if (parent->mnt->flags & MOUNT_RDONLY)
                    rc = -EROFS;
                else if (parent->ops->create == NULL)
                    rc = -ENOTSUP;
                else if (vfs_permission(parent, VFS_MAY_WRITE) != 0)
                    rc = -EACCES;   /* creating an entry writes the directory */
                else {
                    rc = parent->ops->create(parent, last, len, mode & 07777, &vn);
                    created = rc == 0;
                }
            } else if (rc == 0 && (flags & COSMO_O_CREAT) && (flags & COSMO_O_EXCL)) {
                vnode_put(vn);
                rc = -EEXIST;
            }
            mutex_unlock(&parent->lock);
            vnode_put(parent);
            if (rc)
                return rc;
            rc = follow_mount(&vn);
            if (rc)
                return rc;
        }
    }

    if (flags & COSMO_O_DIRECTORY) {
        if (vn->type != VNODE_DIR) {
            vnode_put(vn);
            return -ENOTDIR;
        }
    }
    if (vn->type == VNODE_DIR && acc != COSMO_O_RDONLY) {
        vnode_put(vn);
        return -EISDIR;
    }
    if (acc != COSMO_O_RDONLY && (vn->mnt->flags & MOUNT_RDONLY)) {
        vnode_put(vn);
        return -EROFS;
    }
    /* The file itself: read and/or write per the access mode. A file this
     * call just created is the caller's regardless of the mode it asked
     * for (POSIX), so the check is skipped for it. */
    if (!created) {
        unsigned need = 0;
        if (acc != COSMO_O_WRONLY)
            need |= VFS_MAY_READ;
        if (acc != COSMO_O_RDONLY)
            need |= VFS_MAY_WRITE;
        rc = vfs_permission(vn, need);
        if (rc) {
            vnode_put(vn);
            return rc;
        }
    }
    if ((flags & COSMO_O_TRUNC) && vn->type == VNODE_REG && acc != COSMO_O_RDONLY) {
        mutex_lock(&vn->lock);
        rc = vn->ops->truncate ? vn->ops->truncate(vn, 0) : -ENOTSUP;
        mutex_unlock(&vn->lock);
        if (rc) {
            vnode_put(vn);
            return rc;
        }
    }

    struct file *f = file_alloc(vn, flags);
    if (f == NULL) {
        vnode_put(vn);
        return -ENOMEM;
    }
    *out = f;
    return 0;
}

int vfs_open_vnode(struct vnode *vn, unsigned flags, struct file **out)
{
    struct file *f = file_alloc(vn, flags);
    if (f == NULL) {
        vnode_put(vn);
        return -ENOMEM;
    }
    *out = f;
    return 0;
}

int64_t file_pread(struct file *f, void *buf, size_t len, uint64_t off)
{
    struct vnode *vn = f->vn;
    if ((f->flags & COSMO_O_ACCMODE) == COSMO_O_WRONLY)
        return -EBADF;
    if (vn->type == VNODE_DIR)
        return -EISDIR;
    if (len == 0)
        return 0;
    int64_t n;
    mutex_lock(&vn->lock);
    if (vn->type == VNODE_CHR)
        n = vn->ops->read ? vn->ops->read(vn, off, buf, len) : -ENOTSUP;
    else
        n = pagecache_read(vn, off, buf, len);
    mutex_unlock(&vn->lock);
    return n;
}

int64_t file_pwrite(struct file *f, const void *buf, size_t len, uint64_t off)
{
    struct vnode *vn = f->vn;
    if ((f->flags & COSMO_O_ACCMODE) == COSMO_O_RDONLY)
        return -EBADF;
    if (vn->type == VNODE_DIR)
        return -EISDIR;
    if (vn->mnt->flags & MOUNT_RDONLY)
        return -EROFS;
    if (len == 0)
        return 0;
    int64_t n;
    mutex_lock(&vn->lock);
    if (vn->type == VNODE_CHR) {
        n = vn->ops->write ? vn->ops->write(vn, off, buf, len) : -ENOTSUP;
    } else {
        n = pagecache_write(vn, off, buf, len);
        if (n > 0)
            vn->mtime_ns = vfs_now_ns();
    }
    mutex_unlock(&vn->lock);
    return n;
}

int64_t file_read(struct file *f, void *buf, size_t len)
{
    mutex_lock(&f->lock);
    int64_t n = file_pread(f, buf, len, f->pos);
    if (n > 0 && f->vn->type != VNODE_CHR)
        f->pos += (uint64_t)n;
    mutex_unlock(&f->lock);
    return n;
}

int64_t file_write(struct file *f, const void *buf, size_t len)
{
    mutex_lock(&f->lock);
    uint64_t off = f->pos;
    if (f->flags & COSMO_O_APPEND)
        off = f->vn->size;
    int64_t n = file_pwrite(f, buf, len, off);
    if (n > 0 && f->vn->type != VNODE_CHR)
        f->pos = off + (uint64_t)n;
    mutex_unlock(&f->lock);
    return n;
}

int64_t file_seek(struct file *f, int64_t off, int whence)
{
    if (f->vn->type == VNODE_CHR)
        return -ESPIPE;
    mutex_lock(&f->lock);
    int64_t base;
    switch (whence) {
    case COSMO_SEEK_SET: base = 0; break;
    case COSMO_SEEK_CUR: base = (int64_t)f->pos; break;
    case COSMO_SEEK_END: base = (int64_t)f->vn->size; break;
    default:
        mutex_unlock(&f->lock);
        return -EINVAL;
    }
    if (off > INT64_MAX - base || base + off < 0) {
        mutex_unlock(&f->lock);
        return -EINVAL;
    }
    f->pos = (uint64_t)(base + off);
    int64_t pos = (int64_t)f->pos;
    mutex_unlock(&f->lock);
    return pos;
}

int file_stat(struct file *f, struct cosmo_stat *st)
{
    mutex_lock(&f->vn->lock);
    vnode_stat(f->vn, st);
    mutex_unlock(&f->vn->lock);
    return 0;
}

int file_sync(struct file *f)
{
    struct vnode *vn = f->vn;
    int rc = 0;
    mutex_lock(&vn->lock);
    if (vn->type == VNODE_REG)
        rc = pagecache_sync(vn);
    if (rc == 0 && vn->ops->sync)
        rc = vn->ops->sync(vn);
    mutex_unlock(&vn->lock);
    return rc;
}

struct readdir_ctx {
    uint8_t *buf;
    size_t len;
    size_t used;
    unsigned emitted;
    bool too_small;   /* the first entry did not fit */
};

static int readdir_emit(void *arg, const char *name, size_t len, uint64_t ino, enum vnode_type type)
{
    struct readdir_ctx *c = arg;
    size_t reclen = ALIGN_UP(sizeof(struct cosmo_dirent) + len + 1, 8);
    if (c->used + reclen > c->len) {
        if (c->emitted == 0)
            c->too_small = true;
        return 1;   /* buffer full: stop, position stays before this entry */
    }
    struct cosmo_dirent *d = (struct cosmo_dirent *)(c->buf + c->used);
    memset(d, 0, reclen);
    d->ino = ino;
    d->reclen = (uint16_t)reclen;
    d->type = (uint8_t)type;
    d->namelen = (uint8_t)len;
    memcpy(d->name, name, len);
    c->used += reclen;
    c->emitted++;
    return 0;
}

int64_t file_readdir(struct file *f, void *buf, size_t len)
{
    struct vnode *vn = f->vn;
    if (vn->type != VNODE_DIR)
        return -ENOTDIR;
    if (vn->ops->readdir == NULL)
        return -ENOTSUP;
    if (len < sizeof(struct cosmo_dirent) + 2)
        return -EINVAL;
    struct readdir_ctx c = { .buf = buf, .len = len };
    mutex_lock(&f->lock);
    mutex_lock(&vn->lock);
    uint64_t pos = f->pos;
    int rc = vn->ops->readdir(vn, &pos, readdir_emit, &c);
    mutex_unlock(&vn->lock);
    if (rc == 0)
        f->pos = pos;
    mutex_unlock(&f->lock);
    if (rc)
        return rc;
    if (c.used == 0 && c.too_small)
        return -EINVAL;   /* the next entry needs a larger buffer */
    return (int64_t)c.used;
}

/* --- directory mutations ---------------------------------------------------- */

static bool dot_name(const char *name, size_t len)
{
    return (len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.');
}

/* The sticky bit (01000) on a directory: an entry may be removed or
 * renamed only by the owner of the entry, the owner of the directory, or a
 * privileged caller (/tmp is 01777). */
static bool sticky_denies(const struct vnode *dir, const struct vnode *entry)
{
    if (!(dir->mode & 01000))
        return false;
    const struct credentials *c = cred_current();
    return !cred_privileged(c) && c->euid != entry->uid && c->euid != dir->uid;
}

/* Resolve the parent directory for a mutation; locks it. */
static int parent_for_mutation(struct vnode *start, const char *path, struct vnode **parent, const char **last,
                               size_t *len)
{
    int rc = walk_parent(start, path, parent, last, len, NULL);
    if (rc)
        return rc;
    if (*len == 0 || dot_name(*last, *len)) {
        vnode_put(*parent);
        return *len == 0 ? -EEXIST : -EINVAL;
    }
    if ((*parent)->type != VNODE_DIR) {
        vnode_put(*parent);
        return -ENOTDIR;
    }
    if ((*parent)->mnt->flags & MOUNT_RDONLY) {
        vnode_put(*parent);
        return -EROFS;
    }
    /* Adding, removing or renaming an entry writes the directory and
     * searches it for the name. */
    rc = vfs_permission(*parent, VFS_MAY_WRITE | VFS_MAY_EXEC);
    if (rc) {
        vnode_put(*parent);
        return rc;
    }
    return 0;
}

int vfs_mkdir(struct vnode *start, const char *path, uint32_t mode)
{
    struct vnode *parent;
    const char *last;
    size_t len;
    int rc = parent_for_mutation(start, path, &parent, &last, &len);
    if (rc)
        return rc;
    mutex_lock(&parent->lock);
    struct vnode *existing = NULL;
    if (parent->flags & VNODE_DEAD) {
        rc = -ENOENT;
    } else if (parent->ops->lookup(parent, last, len, &existing) == 0) {
        vnode_put(existing);
        rc = -EEXIST;
    } else if (parent->ops->mkdir == NULL) {
        rc = -ENOTSUP;
    } else {
        struct vnode *vn = NULL;
        rc = parent->ops->mkdir(parent, last, len, mode & 07777, &vn);
        if (rc == 0)
            vnode_put(vn);
    }
    mutex_unlock(&parent->lock);
    vnode_put(parent);
    return rc;
}

static int remove_entry(struct vnode *start, const char *path, bool dir)
{
    struct vnode *parent;
    const char *last;
    size_t len;
    int rc = parent_for_mutation(start, path, &parent, &last, &len);
    if (rc)
        return rc;
    mutex_lock(&parent->lock);
    struct vnode *victim = NULL;
    if (parent->flags & VNODE_DEAD)
        rc = -ENOENT;
    else
        rc = parent->ops->lookup(parent, last, len, &victim);
    if (rc == 0) {
        mutex_lock_nested(&victim->lock, VNODE_NESTED_CHILD);   /* child under its parent (V7) */
        if (dir && victim->type != VNODE_DIR)
            rc = -ENOTDIR;
        else if (!dir && victim->type == VNODE_DIR)
            rc = -EISDIR;
        else if (victim->covered_by != NULL || victim->mnt != parent->mnt)
            rc = -EBUSY;   /* a mountpoint or a mount root */
        else if (sticky_denies(parent, victim))
            rc = -EACCES;  /* a sticky directory: only the owner of the entry, the directory or root */
        else if (dir)
            rc = parent->ops->rmdir ? parent->ops->rmdir(parent, last, len, victim) : -ENOTSUP;
        else
            rc = parent->ops->unlink ? parent->ops->unlink(parent, last, len, victim) : -ENOTSUP;
        if (rc == 0)
            victim->flags |= VNODE_DEAD;
        mutex_unlock(&victim->lock);
        vnode_put(victim);
    }
    mutex_unlock(&parent->lock);
    vnode_put(parent);
    return rc;
}

int vfs_unlink(struct vnode *start, const char *path)
{
    return remove_entry(start, path, false);
}

int vfs_rmdir(struct vnode *start, const char *path)
{
    return remove_entry(start, path, true);
}

/*
 * True if `anc` is `vn` or one of its ancestors. The caller holds the
 * mount's rename_lock and no vnode lock: only rename changes a directory's
 * parent, so the ancestry is stable, and each ".." lookup takes that
 * directory's lock alone (a concurrent mkdir or unlink in it rewrites the
 * same directory data).
 */
static bool is_ancestor(struct vnode *anc, struct vnode *vn)
{
    struct vnode *p = vn;
    vnode_get(p);
    bool yes = false;
    for (unsigned d = 0; d < VFS_MAX_COMPONENTS; d++) {
        if (p == anc) {
            yes = true;
            break;
        }
        if (p->mnt->root == p)
            break;
        struct vnode *up;
        mutex_lock(&p->lock);
        int r = p->ops->lookup(p, "..", 2, &up);
        mutex_unlock(&p->lock);
        vnode_put(p);
        if (r)
            return false;
        p = up;
    }
    vnode_put(p);
    return yes;
}

/*
 * Rename, with the lock order the rest of the VFS uses: parent before
 * child, and for two parents the ancestor first (docs/kernel/lockdep/
 * design.md, "VFS: rename"). Under the mount's rename_lock the ancestry
 * cannot change, so it is decided, and the "directory under itself" rule
 * checked, before any parent is locked. Two unrelated directories are
 * locked in address order: no other path locks two directories without
 * an ancestry between them, and other renames are excluded by the mutex.
 */
int vfs_rename(struct vnode *start, const char *oldpath, const char *newpath)
{
    struct vnode *odir, *ndir;
    const char *oname, *nname;
    size_t olen, nlen;
    int rc = parent_for_mutation(start, oldpath, &odir, &oname, &olen);
    if (rc)
        return rc;
    rc = parent_for_mutation(start, newpath, &ndir, &nname, &nlen);
    if (rc) {
        vnode_put(odir);
        return rc;
    }
    if (odir->mnt != ndir->mnt) {
        rc = -EXDEV;
        goto out_dirs;
    }
    if (odir->ops->rename == NULL) {
        rc = -ENOTSUP;
        goto out_dirs;
    }
    struct mount *mnt = odir->mnt;
    mutex_lock(&mnt->rename_lock);

    for (unsigned attempt = 0;; attempt++) {
        /* Phase 1, no parent locked: find the victim, check it is not an
         * ancestor of the destination, decide the lock order. */
        struct vnode *victim0 = NULL;
        mutex_lock(&odir->lock);
        rc = (odir->flags & VNODE_DEAD) ? -ENOENT : odir->ops->lookup(odir, oname, olen, &victim0);
        mutex_unlock(&odir->lock);
        if (rc)
            break;
        if (victim0->type == VNODE_DIR && odir != ndir && is_ancestor(victim0, ndir)) {
            vnode_put(victim0);
            rc = -EINVAL;   /* a directory may not be moved under itself */
            break;
        }
        struct vnode *first = odir, *second = NULL;
        if (odir != ndir) {
            if (is_ancestor(odir, ndir)) {
                first = odir;
                second = ndir;
            } else if (is_ancestor(ndir, odir)) {
                first = ndir;
                second = odir;
            } else {
                first = odir < ndir ? odir : ndir;
                second = odir < ndir ? ndir : odir;
            }
        }

        /* Phase 2: the parents, then the entries under them. */
        mutex_lock(&first->lock);
        if (second)
            mutex_lock_nested(&second->lock, VNODE_NESTED_PARENT2);

        struct vnode *victim = NULL, *replaced = NULL;
        if ((odir->flags | ndir->flags) & VNODE_DEAD)
            rc = -ENOENT;
        else
            rc = odir->ops->lookup(odir, oname, olen, &victim);
        bool changed = rc == 0 && victim != victim0;   /* unlinked and re-created meanwhile */
        vnode_put(victim0);
        if (rc == 0 && !changed) {
            if (victim->covered_by || victim->mnt != odir->mnt) {
                rc = -EBUSY;
            } else if (sticky_denies(odir, victim)) {
                rc = -EACCES;
            } else if (victim->type == VNODE_DIR && odir != ndir && vfs_permission(victim, VFS_MAY_WRITE) != 0) {
                rc = -EACCES;   /* moving a directory rewrites its ".." */
            } else if (ndir->ops->lookup(ndir, nname, nlen, &replaced) == 0) {
                if (replaced == victim)
                    rc = 0;   /* same entry: nothing to do */
                else if (replaced->type == VNODE_DIR && victim->type != VNODE_DIR)
                    rc = -EISDIR;
                else if (replaced->type != VNODE_DIR && victim->type == VNODE_DIR)
                    rc = -ENOTDIR;
                else if (replaced->covered_by || replaced->mnt != odir->mnt)
                    rc = -EBUSY;
                else if (sticky_denies(ndir, replaced))
                    rc = -EACCES;
            }
            if (rc == 0 && replaced != victim) {
                rc = odir->ops->rename(odir, oname, olen, victim, ndir, nname, nlen, replaced);
                if (rc == 0 && replaced)
                    replaced->flags |= VNODE_DEAD;
            }
            if (replaced)
                vnode_put(replaced);
        }
        if (victim)
            vnode_put(victim);
        if (second)
            mutex_unlock(&second->lock);
        mutex_unlock(&first->lock);
        if (!changed)
            break;
        if (attempt == 8) {
            rc = -EBUSY;
            break;
        }
        /* The entry was replaced between the phases: the ancestry check
         * was for the old one, so start over. */
    }
    mutex_unlock(&mnt->rename_lock);
out_dirs:
    vnode_put(ndir);
    vnode_put(odir);
    return rc;
}

int vfs_stat(struct vnode *start, const char *path, struct cosmo_stat *st)
{
    struct vnode *vn;
    int rc = vfs_lookup(start, path, &vn);
    if (rc)
        return rc;
    mutex_lock(&vn->lock);
    vnode_stat(vn, st);
    mutex_unlock(&vn->lock);
    vnode_put(vn);
    return 0;
}

/* --- init and diagnostics ---------------------------------------------------- */

extern struct fs_type ramfs_fs_type;

void vfs_init(void)
{
    KASSERT(!g_initialized);
    mutex_init(&g_mounts_lock, "mounts");
    g_initialized = true;
    if (vfs_register_fs(&ramfs_fs_type))
        panic("vfs: cannot register ramfs");
    struct mount *root;
    int rc = do_mount(&ramfs_fs_type, NULL, 0, &root);
    if (rc)
        panic("vfs: cannot mount the root ramfs (%d)", rc);
    mutex_lock(&g_mounts_lock);
    g_root_mount = root;
    list_push_back(&g_mounts, &root->link);
    g_nr_mounts = 1;
    mutex_unlock(&g_mounts_lock);
    kinfo("vfs: root mounted (ramfs)");
}

unsigned vfs_mount_count(void)
{
    mutex_lock(&g_mounts_lock);
    unsigned n = g_nr_mounts;
    mutex_unlock(&g_mounts_lock);
    return n;
}

unsigned vfs_vnode_count(void)
{
    unsigned n = 0;
    mutex_lock(&g_mounts_lock);
    struct mount *mnt;
    list_for_each_entry(mnt, &g_mounts, link)
        n += mnt->nr_vnodes;
    mutex_unlock(&g_mounts_lock);
    return n;
}

void vfs_dump(void)
{
    mutex_lock(&g_mounts_lock);
    kprintf("mounts (%u):\n", g_nr_mounts);
    struct mount *mnt;
    list_for_each_entry(mnt, &g_mounts, link) {
        kprintf("  %-8s root ino %llu, %u vnodes, %s%s\n", mnt->fs->name, (unsigned long long)mnt->root->ino,
                mnt->nr_vnodes, mnt->bdev ? mnt->bdev->name : "no device",
                (mnt->flags & MOUNT_RDONLY) ? ", ro" : "");
    }
    mutex_unlock(&g_mounts_lock);
}
