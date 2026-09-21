/*
 * ramfs.c - In-memory filesystem: the root at boot, /tmp, /boot.
 *
 * Directories are lists of entries pointing at pinned vnodes; regular
 * file data lives in the vnode's page cache (readpage zero-fills,
 * writepage keeps the page resident). Inode numbers are per mount.
 */

#include <kernel/bootarchive.h>
#include <kernel/cred.h>
#include <kernel/errno.h>
#include <kernel/fifo.h>
#include <kernel/kmalloc.h>
#include <kernel/lockdep.h>
#include <kernel/log.h>
#include <kernel/page.h>
#include <kernel/printf.h>
#include <kernel/string.h>
#include <kernel/vfs.h>

#define RAMFS_MAX_FILE  (64ull << 20)
#define RAMFS_MAX_PAGES 16384u

struct ramfs_dirent {
    struct list_node link;
    struct vnode *child;     /* pinned reference */
    uint8_t len;
    char name[VFS_NAME_MAX + 1];
};

struct ramfs_node {
    struct list_node entries;   /* directories */
    struct vnode *parent;       /* unreferenced back pointer, NULL for root */
    unsigned nr_entries;
    const struct chrdev_ops *chr;   /* character nodes */
    void *chr_priv;
    char *target;               /* symbolic links: the path, NUL terminated */
    struct fifo *fifo;          /* named pipes: the ring's holder (kernel/ipc/fifo.c) */
};

static const struct vnode_ops ramfs_dir_ops;
static const struct vnode_ops ramfs_file_ops;
static const struct vnode_ops ramfs_lnk_ops;
static const struct vnode_ops ramfs_sock_ops;
static const struct vnode_ops ramfs_fifo_ops;

static struct vnode *ramfs_new(struct mount *mnt, enum vnode_type type, uint32_t mode, struct vnode *parent)
{
    struct vnode *vn = vnode_alloc(mnt, mnt->next_ino++);
    if (vn == NULL)
        return NULL;
    struct ramfs_node *n = kzalloc(sizeof(*n));
    if (n == NULL) {
        vnode_put(vn);
        return NULL;
    }
    list_init(&n->entries);
    n->parent = parent;
    vn->type = type;
    vn->mode = mode;
    /* Owned by whoever creates it: the kernel (root) for the boot
     * namespace, the calling process afterwards. */
    vn->uid = cred_current()->euid;
    vn->gid = cred_current()->egid;
    vn->ops = type == VNODE_DIR ? &ramfs_dir_ops : type == VNODE_LNK ? &ramfs_lnk_ops
            : type == VNODE_SOCK ? &ramfs_sock_ops : type == VNODE_FIFO ? &ramfs_fifo_ops : &ramfs_file_ops;
    vn->fs_priv = n;
    vn->flags |= VNODE_PINNED;   /* the reference from vnode_alloc is the pin */
    vn->nlink = type == VNODE_DIR ? 2 : 1;
    vnode_hash_insert(vn);
    return vn;
}

static struct ramfs_dirent *find_entry(struct vnode *dir, const char *name, size_t len)
{
    struct ramfs_node *n = dir->fs_priv;
    struct ramfs_dirent *e;
    list_for_each_entry(e, &n->entries, link) {
        if (e->len == len && memcmp(e->name, name, len) == 0)
            return e;
    }
    return NULL;
}

static int ramfs_lookup(struct vnode *dir, const char *name, size_t len, struct vnode **out)
{
    if (len == 2 && name[0] == '.' && name[1] == '.') {
        struct ramfs_node *n = dir->fs_priv;
        struct vnode *p = n->parent ? n->parent : dir;
        vnode_get(p);
        *out = p;
        return 0;
    }
    struct ramfs_dirent *e = find_entry(dir, name, len);
    if (e == NULL)
        return -ENOENT;
    vnode_get(e->child);
    *out = e->child;
    return 0;
}

static int add_entry(struct vnode *dir, const char *name, size_t len, struct vnode *child)
{
    struct ramfs_dirent *e = kzalloc(sizeof(*e));
    if (e == NULL)
        return -ENOMEM;
    list_init(&e->link);
    e->child = child;
    e->len = (uint8_t)len;
    memcpy(e->name, name, len);
    struct ramfs_node *n = dir->fs_priv;
    list_push_back(&n->entries, &e->link);
    n->nr_entries++;
    dir->mtime_ns = vfs_now_ns();
    return 0;
}

static int ramfs_create_common(struct vnode *dir, const char *name, size_t len, uint32_t mode,
                               enum vnode_type type, struct vnode **out)
{
    if (find_entry(dir, name, len))
        return -EEXIST;
    struct vnode *vn = ramfs_new(dir->mnt, type, mode, dir);
    if (vn == NULL)
        return -ENOMEM;
    int rc = add_entry(dir, name, len, vn);   /* the pin moves into the entry */
    if (rc) {
        vn->flags &= ~VNODE_PINNED;
        vnode_put(vn);
        return rc;
    }
    if (type == VNODE_DIR)
        dir->nlink++;
    vnode_get(vn);   /* the caller's reference */
    *out = vn;
    return 0;
}

static int ramfs_create(struct vnode *dir, const char *name, size_t len, uint32_t mode, struct vnode **out)
{
    return ramfs_create_common(dir, name, len, mode, VNODE_REG, out);
}

static int ramfs_mkdir(struct vnode *dir, const char *name, size_t len, uint32_t mode, struct vnode **out)
{
    return ramfs_create_common(dir, name, len, mode, VNODE_DIR, out);
}

/* A unix socket's name: a node with no contents and no operations but
 * its own removal; open() refuses it in the VFS. A named pipe: a node
 * whose opens share the pipe's ring (kernel/ipc/fifo.c), the fifo made
 * here and freed with the node. */
static int ramfs_mknod(struct vnode *dir, const char *name, size_t len, uint32_t mode, enum vnode_type type,
                       struct vnode **out)
{
    if (type == VNODE_SOCK)
        return ramfs_create_common(dir, name, len, mode, VNODE_SOCK, out);
    if (type != VNODE_FIFO)
        return -EINVAL;
    struct fifo *fifo = fifo_alloc();
    if (fifo == NULL)
        return -ENOMEM;
    int rc = ramfs_create_common(dir, name, len, mode, VNODE_FIFO, out);
    if (rc) {
        fifo_free(fifo);
        return rc;
    }
    struct ramfs_node *n = (*out)->fs_priv;
    n->fifo = fifo;
    return 0;
}

/*
 * A link's target is the node's own bytes, not a page: ramfs_lnk_ops has
 * no readpage or writepage, so a link is never in the page cache (which
 * for ramfs is the store) and `size` is the target's length, as stat
 * reports it.
 */
static int ramfs_symlink(struct vnode *dir, const char *name, size_t len, const char *target,
                         struct vnode **out)
{
    size_t tlen = strnlen(target, VFS_PATH_MAX);
    if (tlen == 0 || tlen >= VFS_PATH_MAX)
        return -ENAMETOOLONG;
    char *copy = kmalloc(tlen + 1, 0);
    if (copy == NULL)
        return -ENOMEM;
    memcpy(copy, target, tlen);
    copy[tlen] = '\0';
    int rc = ramfs_create_common(dir, name, len, 0777, VNODE_LNK, out);
    if (rc) {
        kfree(copy);
        return rc;
    }
    struct ramfs_node *n = (*out)->fs_priv;
    n->target = copy;
    (*out)->size = tlen;
    return 0;
}

static int ramfs_readlink(struct vnode *vn, char *buf, size_t len)
{
    const struct ramfs_node *n = vn->fs_priv;
    if (n->target == NULL)
        return -EIO;
    size_t tlen = strlen(n->target);
    if (tlen > len)
        tlen = len;
    memcpy(buf, n->target, tlen);
    return (int)tlen;
}

/* Drop the entry and its pin. Caller holds dir and victim locks. */
static void drop_entry(struct vnode *dir, struct ramfs_dirent *e)
{
    struct ramfs_node *n = dir->fs_priv;
    struct vnode *child = e->child;
    list_remove(&e->link);
    n->nr_entries--;
    kfree(e);
    dir->mtime_ns = vfs_now_ns();
    child->nlink = child->type == VNODE_DIR ? 0 : child->nlink - 1;
    child->flags &= ~VNODE_PINNED;
    if (child->type == VNODE_DIR)
        dir->nlink--;
    vnode_put(child);   /* the pin; open files keep it alive */
}

static int ramfs_unlink(struct vnode *dir, const char *name, size_t len, struct vnode *victim)
{
    struct ramfs_dirent *e = find_entry(dir, name, len);
    if (e == NULL || e->child != victim)
        return -ENOENT;
    drop_entry(dir, e);
    return 0;
}

static int ramfs_rmdir(struct vnode *dir, const char *name, size_t len, struct vnode *victim)
{
    struct ramfs_dirent *e = find_entry(dir, name, len);
    if (e == NULL || e->child != victim)
        return -ENOENT;
    struct ramfs_node *vn_node = victim->fs_priv;
    if (vn_node->nr_entries != 0)
        return -ENOTEMPTY;
    drop_entry(dir, e);
    return 0;
}

static int ramfs_rename(struct vnode *odir, const char *oname, size_t olen, struct vnode *victim, struct vnode *ndir,
                        const char *nname, size_t nlen, struct vnode *replaced)
{
    struct ramfs_dirent *e = find_entry(odir, oname, olen);
    if (e == NULL || e->child != victim)
        return -ENOENT;
    if (replaced) {
        struct ramfs_dirent *r = find_entry(ndir, nname, nlen);
        if (r == NULL || r->child != replaced)
            return -ENOENT;
        if (replaced->type == VNODE_DIR && ((struct ramfs_node *)replaced->fs_priv)->nr_entries != 0)
            return -ENOTEMPTY;
        mutex_lock_nested(&replaced->lock, VNODE_NESTED_CHILD);   /* under both parents (V7) */
        drop_entry(ndir, r);
        mutex_unlock(&replaced->lock);
    }
    struct ramfs_node *on = odir->fs_priv, *nn = ndir->fs_priv;
    list_remove(&e->link);
    on->nr_entries--;
    e->len = (uint8_t)nlen;
    memcpy(e->name, nname, nlen);
    list_push_back(&nn->entries, &e->link);
    nn->nr_entries++;
    if (odir != ndir) {
        ((struct ramfs_node *)victim->fs_priv)->parent = ndir;
        if (victim->type == VNODE_DIR) {
            odir->nlink--;
            ndir->nlink++;
        }
    }
    odir->mtime_ns = ndir->mtime_ns = vfs_now_ns();
    return 0;
}

static int ramfs_readdir(struct vnode *dir, uint64_t *pos, vfs_dirent_cb cb, void *arg)
{
    struct ramfs_node *n = dir->fs_priv;
    uint64_t i = 0;
    if (*pos == 0) {
        if (cb(arg, ".", 1, dir->ino, VNODE_DIR))
            return 0;
        *pos = 1;
    }
    if (*pos == 1) {
        struct vnode *p = n->parent ? n->parent : dir;
        if (cb(arg, "..", 2, p->ino, VNODE_DIR))
            return 0;
        *pos = 2;
    }
    struct ramfs_dirent *e;
    list_for_each_entry(e, &n->entries, link) {
        if (i + 2 < *pos) {
            i++;
            continue;
        }
        if (cb(arg, e->name, e->len, e->child->ino, e->child->type))
            return 0;
        i++;
        *pos = i + 2;
    }
    return 0;
}

static int ramfs_readpage(struct vnode *vn, uint64_t index, void *buf)
{
    (void)vn;
    (void)index;
    memset(buf, 0, PAGE_SIZE);   /* holes; resident pages never miss */
    return 0;
}

static int ramfs_writepage(struct vnode *vn, uint64_t index, const void *buf)
{
    (void)vn;
    (void)index;
    (void)buf;
    return 0;   /* the page cache is the store */
}

static int ramfs_truncate(struct vnode *vn, uint64_t size)
{
    if (size > RAMFS_MAX_FILE)
        return -EFBIG;
    pagecache_truncate(vn, size);
    vn->size = size;
    vn->mtime_ns = vfs_now_ns();
    return 0;
}

static void ramfs_evict(struct vnode *vn)
{
    struct ramfs_node *n = vn->fs_priv;
    if (n != NULL) {
        kfree(n->target);
        if (n->fifo != NULL)
            fifo_free(n->fifo);   /* asserts no ring: the opens went before the node could */
    }
    kfree(n);
    vn->fs_priv = NULL;
}

/* --- named pipes: every operation is the fifo's, given the file --------- */

static struct fifo *fifo_of(const struct vnode *vn)
{
    return ((const struct ramfs_node *)vn->fs_priv)->fifo;
}

static int ramfs_fifo_open(struct vnode *vn, struct file *f) { return fifo_open(fifo_of(vn), f); }
static void ramfs_fifo_release(struct vnode *vn, struct file *f) { fifo_release(fifo_of(vn), f); }
static int64_t ramfs_fifo_read(struct vnode *vn, struct file *f, uint64_t off, void *buf, size_t len)
{
    (void)off;
    return fifo_read(fifo_of(vn), f, buf, len);
}
static int64_t ramfs_fifo_write(struct vnode *vn, struct file *f, uint64_t off, const void *buf, size_t len)
{
    (void)off;
    return fifo_write(fifo_of(vn), f, buf, len);
}
static unsigned ramfs_fifo_ready(struct vnode *vn, struct file *f) { return fifo_ready(fifo_of(vn), f); }
static struct waitqueue *ramfs_fifo_poll_wq(struct vnode *vn, struct file *f, unsigned events)
{
    return fifo_poll_wq(fifo_of(vn), f, events);
}
static int ramfs_fifo_set_nonblock(struct vnode *vn, struct file *f, int on)
{
    return fifo_set_nonblock(fifo_of(vn), f, on);
}

static const struct vnode_ops ramfs_dir_ops = {
    .lookup = ramfs_lookup,
    .create = ramfs_create,
    .mkdir = ramfs_mkdir,
    .unlink = ramfs_unlink,
    .rmdir = ramfs_rmdir,
    .rename = ramfs_rename,
    .readdir = ramfs_readdir,
    .symlink = ramfs_symlink,
    .mknod = ramfs_mknod,
    .evict = ramfs_evict,
};

static const struct vnode_ops ramfs_sock_ops = {
    .evict = ramfs_evict,
};

static const struct vnode_ops ramfs_fifo_ops = {
    .open = ramfs_fifo_open,
    .release = ramfs_fifo_release,
    .read_file = ramfs_fifo_read,
    .write_file = ramfs_fifo_write,
    .ready = ramfs_fifo_ready,
    .poll_wq = ramfs_fifo_poll_wq,
    .set_nonblock = ramfs_fifo_set_nonblock,
    .evict = ramfs_evict,
};

static const struct vnode_ops ramfs_file_ops = {
    .readpage = ramfs_readpage,
    .writepage = ramfs_writepage,
    .truncate = ramfs_truncate,
    .evict = ramfs_evict,
};

static const struct vnode_ops ramfs_lnk_ops = {
    .readlink = ramfs_readlink,
    .evict = ramfs_evict,
};

static int64_t ramfs_chr_read(struct vnode *vn, uint64_t off, void *buf, size_t len)
{
    struct ramfs_node *n = vn->fs_priv;
    return n->chr->read ? n->chr->read(vn, off, buf, len) : -ENOTSUP;
}

static int64_t ramfs_chr_write(struct vnode *vn, uint64_t off, const void *buf, size_t len)
{
    struct ramfs_node *n = vn->fs_priv;
    return n->chr->write ? n->chr->write(vn, off, buf, len) : -ENOTSUP;
}

static int ramfs_chr_open(struct vnode *vn, struct file *f)
{
    struct ramfs_node *n = vn->fs_priv;
    return n->chr->open ? n->chr->open(vn, f) : 0;
}

static void ramfs_chr_release(struct vnode *vn, struct file *f)
{
    struct ramfs_node *n = vn->fs_priv;
    if (n->chr->release)
        n->chr->release(vn, f);
}

/* Devices with per-open state get the file; the others keep read/write. */
static int64_t ramfs_chr_read_file(struct vnode *vn, struct file *f, uint64_t off, void *buf, size_t len)
{
    struct ramfs_node *n = vn->fs_priv;
    if (n->chr->read_file)
        return n->chr->read_file(vn, f, off, buf, len);
    return n->chr->read ? n->chr->read(vn, off, buf, len) : -ENOTSUP;
}

static int64_t ramfs_chr_write_file(struct vnode *vn, struct file *f, uint64_t off, const void *buf, size_t len)
{
    struct ramfs_node *n = vn->fs_priv;
    if (n->chr->write_file)
        return n->chr->write_file(vn, f, off, buf, len);
    return n->chr->write ? n->chr->write(vn, off, buf, len) : -ENOTSUP;
}

/* Readiness, for a device that has an opinion; the file type's defaults
 * (always ready, never changes, -EOPNOTSUPP) for one that has not. */
static unsigned ramfs_chr_ready(struct vnode *vn, struct file *f)
{
    struct ramfs_node *n = vn->fs_priv;
    return n->chr->ready ? n->chr->ready(vn, f) : (COSMO_IO_READABLE | COSMO_IO_WRITABLE);
}

static struct waitqueue *ramfs_chr_poll_wq(struct vnode *vn, struct file *f, unsigned events)
{
    struct ramfs_node *n = vn->fs_priv;
    return n->chr->poll_wq ? n->chr->poll_wq(vn, f, events) : NULL;
}

static int ramfs_chr_set_nonblock(struct vnode *vn, struct file *f, int on)
{
    struct ramfs_node *n = vn->fs_priv;
    return n->chr->set_nonblock ? n->chr->set_nonblock(vn, f, on) : -EOPNOTSUPP;
}

static const struct vnode_ops ramfs_chr_ops = {
    .read = ramfs_chr_read,
    .write = ramfs_chr_write,
    .open = ramfs_chr_open,
    .release = ramfs_chr_release,
    .read_file = ramfs_chr_read_file,
    .write_file = ramfs_chr_write_file,
    .ready = ramfs_chr_ready,
    .poll_wq = ramfs_chr_poll_wq,
    .set_nonblock = ramfs_chr_set_nonblock,
    .evict = ramfs_evict,
};

void *ramfs_chr_priv(const struct vnode *vn)
{
    const struct ramfs_node *n = vn->fs_priv;
    return n->chr_priv;
}

int ramfs_mkchr(const char *path, uint32_t mode, const struct chrdev_ops *ops, void *priv, struct vnode **out)
{
    const char *slash = NULL;
    for (const char *c = path; *c; c++)
        if (*c == '/')
            slash = c;
    if (slash == NULL || slash[1] == '\0' || strlen(slash + 1) > VFS_NAME_MAX)
        return -EINVAL;
    char dirpath[VFS_PATH_MAX];
    size_t dl = (size_t)(slash - path);
    if (dl == 0) {
        dirpath[0] = '/';
        dl = 1;
    } else {
        if (dl >= sizeof(dirpath))
            return -ENAMETOOLONG;
        memcpy(dirpath, path, dl);
    }
    dirpath[dl] = '\0';
    struct vnode *dir;
    int rc = vfs_lookup(NULL, dirpath, &dir);
    if (rc)
        return rc;
    if (dir->type != VNODE_DIR || dir->ops != &ramfs_dir_ops) {
        vnode_put(dir);
        return -ENOTDIR;
    }
    const char *name = slash + 1;
    size_t nl = strlen(name);
    mutex_lock(&dir->lock);
    struct vnode *vn = NULL;
    rc = ramfs_create_common(dir, name, nl, mode, VNODE_CHR, &vn);
    if (rc == 0) {
        struct ramfs_node *n = vn->fs_priv;
        n->chr = ops;
        n->chr_priv = priv;
        vn->ops = &ramfs_chr_ops;
        /*
         * This lock used to be given its own lockdep class here, because
         * a device whose operations consult the mount table -- /dev/fsctl
         * does, by definition -- read as an inversion of
         * `mounts -> vnode` and panicked on its first boot. The split was
         * sound (nothing mounts onto a character device) and it answered
         * the wrong question: the lock was being held across the device's
         * operations at all, which is what made a mount-table lookup
         * inside one an ordering at all.
         *
         * file_pread and file_pwrite no longer hold it across a driver
         * (docs/audit/next-subsystem-chrdev-vnode-lock.md), so there is
         * no inversion to excuse and no reason to tell lockdep to look
         * away from this class. A device node's lock is an ordinary
         * vnode lock again.
         */
    }
    mutex_unlock(&dir->lock);
    vnode_put(dir);
    if (rc)
        return rc;
    if (out)
        *out = vn;      /* the caller's reference from ramfs_create_common */
    else
        vnode_put(vn);
    return 0;
}

static int ramfs_mount(struct fs_type *fs, struct blkdev *bdev, unsigned flags, struct mount *mnt)
{
    (void)fs;
    (void)flags;
    if (bdev != NULL)
        return -EINVAL;
    /* The page cache is this filesystem's store: its pages are never
     * reclaimed, and the mount has a page budget so no process fills RAM
     * through /tmp (docs/kernel/security/design.md §3). */
    mnt->flags |= MOUNT_CACHE_IS_STORE;
    mnt->cache_limit_pages = RAMFS_MAX_PAGES;
    struct vnode *root = ramfs_new(mnt, VNODE_DIR, 0755, NULL);
    if (root == NULL)
        return -ENOMEM;
    /* The pin doubles as the mount's reference on the root. */
    mnt->root = root;
    return 0;
}

/*
 * Release every entry under `dir` (recursively) so the vnodes can go. Runs
 * from unmount after vfs_umount's reference scan proved that nothing
 * outside the filesystem holds any vnode of this mount, and with the root
 * locked: the tree is exclusively ours, so the children are not locked
 * here (a lock per level would nest the vnode class to arbitrary depth,
 * which no annotation can express, and would exclude nothing).
 */
static void ramfs_release_tree(struct vnode *dir)
{
    struct ramfs_node *n = dir->fs_priv;
    struct ramfs_dirent *e, *tmp;
    list_for_each_entry_safe(e, tmp, &n->entries, link) {
        struct vnode *child = e->child;
        list_remove(&e->link);
        n->nr_entries--;
        kfree(e);
        if (child->type == VNODE_DIR)
            ramfs_release_tree(child);
        child->nlink = 0;
        child->flags &= ~VNODE_PINNED;
        vnode_put(child);   /* the pin; nothing else references it (unmount checked) */
    }
}

static int ramfs_unmount(struct mount *mnt)
{
    mutex_lock(&mnt->root->lock);
    ramfs_release_tree(mnt->root);
    mutex_unlock(&mnt->root->lock);
    return 0;
}

struct fs_type ramfs_fs_type = {
    .name = "ramfs",
    .mount = ramfs_mount,
    .unmount = ramfs_unmount,
    .sync = NULL,
};

/* --- boot population --------------------------------------------------- */

static int write_file(const char *path, const void *data, size_t len, uint32_t mode)
{
    struct file *f;
    int rc = vfs_open(NULL, path, COSMO_O_WRONLY | COSMO_O_CREAT | COSMO_O_TRUNC, mode, &f);
    if (rc)
        return rc;
    int64_t n = file_write(f, data, len);
    file_put(f);
    return n == (int64_t)len ? 0 : (n < 0 ? (int)n : -EIO);
}

/* mkdir -p for the parents of `path` (archive entries may be nested). */
static void ensure_parents(const char *path)
{
    char dir[VFS_PATH_MAX];
    strlcpy(dir, path, sizeof(dir));
    for (char *p = dir + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        int rc = vfs_mkdir(NULL, dir, 0755);
        if (rc && rc != -EEXIST)
            kwarn("ramfs: cannot create %s (%d)", dir, rc);
        *p = '/';
    }
}

void ramfs_populate_boot(void)
{
    static const struct {
        const char *path;
        uint32_t mode;
    } dirs[] = {
        { "/boot", 0755 }, { "/boot/modules", 0755 }, { "/boot/tests", 0755 },
        { "/tmp", 01777 },   /* world-writable scratch space (the sticky bit is recorded, not yet enforced) */
        { "/mnt", 0755 },  { "/dev", 0755 }, { "/bin", 0755 }, { "/sbin", 0755 }, { "/etc", 0755 },
        { "/proc", 0555 },
    };
    for (size_t i = 0; i < ARRAY_SIZE(dirs); i++) {
        int rc = vfs_mkdir(NULL, dirs[i].path, dirs[i].mode);
        if (rc)
            kwarn("ramfs: cannot create %s (%d)", dirs[i].path, rc);
    }
    unsigned copied = 0;
    for (unsigned i = 0; i < bootarchive_count(); i++) {
        const struct bootarchive_entry *e = bootarchive_entry(i);
        char path[VFS_PATH_MAX];
        /* The bootstrap namespace: bin/, sbin/ and etc/ entries are the
         * installed system (docs/userland/); everything else is /boot. */
        uint32_t mode = 0644;
        if (strcmp(e->name, "init") == 0) {
            ksnprintf(path, sizeof(path), "/boot/%s", e->name);
            mode = 0755;   /* the program the kernel starts; its self-test respawns it */
        } else if (strncmp(e->name, "bin/", 4) == 0 || strncmp(e->name, "sbin/", 5) == 0) {
            ksnprintf(path, sizeof(path), "/%s", e->name);
            mode = 0755;
        } else if (strncmp(e->name, "etc/", 4) == 0) {
            ksnprintf(path, sizeof(path), "/%s", e->name);
        } else {
            ksnprintf(path, sizeof(path), "/boot/%s", e->name);
            if (strncmp(e->name, "tests/", 6) == 0)
                mode = 0755;   /* test programs and fixtures run from the shell */
        }
        ensure_parents(path);
        int rc = write_file(path, e->data, e->size, mode);
        if (rc)
            kwarn("ramfs: cannot populate %s (%d)", path, rc);
        else
            copied++;
    }
    kinfo("ramfs: /boot holds %u file(s) from the boot archive", copied);
}
