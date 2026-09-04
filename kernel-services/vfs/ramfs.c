/*
 * ramfs.c - In-memory filesystem: the root at boot, /tmp, /boot.
 *
 * Directories are lists of entries pointing at pinned vnodes; regular
 * file data lives in the vnode's page cache (readpage zero-fills,
 * writepage keeps the page resident). Inode numbers are per mount.
 */

#include <kernel/bootarchive.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
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
};

struct ramfs {
    unsigned nr_pages_limit;
};

static const struct vnode_ops ramfs_dir_ops;
static const struct vnode_ops ramfs_file_ops;

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
    vn->ops = type == VNODE_DIR ? &ramfs_dir_ops : &ramfs_file_ops;
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
        mutex_lock(&replaced->lock);
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
    kfree(vn->fs_priv);
    vn->fs_priv = NULL;
}

static const struct vnode_ops ramfs_dir_ops = {
    .lookup = ramfs_lookup,
    .create = ramfs_create,
    .mkdir = ramfs_mkdir,
    .unlink = ramfs_unlink,
    .rmdir = ramfs_rmdir,
    .rename = ramfs_rename,
    .readdir = ramfs_readdir,
    .evict = ramfs_evict,
};

static const struct vnode_ops ramfs_file_ops = {
    .readpage = ramfs_readpage,
    .writepage = ramfs_writepage,
    .truncate = ramfs_truncate,
    .evict = ramfs_evict,
};

static int ramfs_mount(struct fs_type *fs, struct blkdev *bdev, unsigned flags, struct mount *mnt)
{
    (void)fs;
    (void)flags;
    if (bdev != NULL)
        return -EINVAL;
    struct vnode *root = ramfs_new(mnt, VNODE_DIR, 0755, NULL);
    if (root == NULL)
        return -ENOMEM;
    /* The pin doubles as the mount's reference on the root. */
    mnt->root = root;
    return 0;
}

/* Release every entry under `dir` (recursively) so the vnodes can go. */
static void ramfs_release_tree(struct vnode *dir)
{
    struct ramfs_node *n = dir->fs_priv;
    while (!list_empty(&n->entries)) {
        struct ramfs_dirent *e = list_entry(n->entries.next, struct ramfs_dirent, link);
        struct vnode *child = e->child;
        list_remove(&e->link);
        n->nr_entries--;
        kfree(e);
        mutex_lock(&child->lock);
        if (child->type == VNODE_DIR)
            ramfs_release_tree(child);
        child->nlink = 0;
        child->flags &= ~VNODE_PINNED;
        mutex_unlock(&child->lock);
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

static int write_file(const char *path, const void *data, size_t len)
{
    struct file *f;
    int rc = vfs_open(NULL, path, COSMO_O_WRONLY | COSMO_O_CREAT | COSMO_O_TRUNC, 0644, &f);
    if (rc)
        return rc;
    int64_t n = file_write(f, data, len);
    file_put(f);
    return n == (int64_t)len ? 0 : (n < 0 ? (int)n : -EIO);
}

void ramfs_populate_boot(void)
{
    static const char *const dirs[] = { "/boot", "/boot/modules", "/boot/tests", "/tmp", "/mnt", "/dev" };
    for (size_t i = 0; i < ARRAY_SIZE(dirs); i++) {
        int rc = vfs_mkdir(NULL, dirs[i], 0755);
        if (rc)
            kwarn("ramfs: cannot create %s (%d)", dirs[i], rc);
    }
    unsigned copied = 0;
    for (unsigned i = 0; i < bootarchive_count(); i++) {
        const struct bootarchive_entry *e = bootarchive_entry(i);
        char path[VFS_PATH_MAX];
        ksnprintf(path, sizeof(path), "/boot/%s", e->name);
        int rc = write_file(path, e->data, e->size);
        if (rc)
            kwarn("ramfs: cannot populate %s (%d)", path, rc);
        else
            copied++;
    }
    kinfo("ramfs: /boot holds %u file(s) from the boot archive", copied);
}
