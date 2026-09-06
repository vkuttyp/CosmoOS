/*
 * vfs.h - The virtual filesystem: mounts, vnodes, files, filesystems.
 *
 * The VFS knows a filesystem only through struct fs_type and a file only
 * through struct vnode_ops (constitution invariant 4). Paths are walked
 * here; filesystems implement per-directory lookup and the mutations.
 * struct file is a kobject_io_type so handles read and write files with
 * the Phase 4 system calls unchanged. See docs/kernel-services/vfs/.
 */

#ifndef KERNEL_VFS_H
#define KERNEL_VFS_H

#include <kernel/list.h>
#include <kernel/mutex.h>
#include <kernel/spinlock.h>
#include <kernel/object.h>
#include <kernel/pagecache.h>
#include <kernel/types.h>

#include <uapi/cosmo/syscall.h>

#define VFS_NAME_MAX 255
#define VFS_PATH_MAX 1024
#define VFS_MAX_COMPONENTS 40
#define VNODE_HASH 64

struct blkdev;
struct mount;
struct vnode;

enum vnode_type {
    VNODE_REG = COSMO_DT_REG,
    VNODE_DIR = COSMO_DT_DIR,
    VNODE_CHR = COSMO_DT_CHR,
};

/* readdir callback: return nonzero to stop. */
typedef int (*vfs_dirent_cb)(void *arg, const char *name, size_t len, uint64_t ino, enum vnode_type type);

struct vnode_ops {
    int (*lookup)(struct vnode *dir, const char *name, size_t len, struct vnode **out);
    int (*create)(struct vnode *dir, const char *name, size_t len, uint32_t mode, struct vnode **out);
    int (*mkdir)(struct vnode *dir, const char *name, size_t len, uint32_t mode, struct vnode **out);
    int (*unlink)(struct vnode *dir, const char *name, size_t len, struct vnode *victim);
    int (*rmdir)(struct vnode *dir, const char *name, size_t len, struct vnode *victim);
    int (*rename)(struct vnode *odir, const char *oname, size_t olen, struct vnode *victim, struct vnode *ndir,
                  const char *nname, size_t nlen, struct vnode *replaced);
    int (*readdir)(struct vnode *dir, uint64_t *pos, vfs_dirent_cb cb, void *arg);
    int (*readpage)(struct vnode *vn, uint64_t index, void *buf);
    int (*writepage)(struct vnode *vn, uint64_t index, const void *buf);
    /* Optional: `n` consecutive dirty pages from `index`, offered
     * together so a filesystem can write them as one object -- which is
     * what compression needs, since a single block that compresses to a
     * quarter of itself still occupies a block. Reports in `done` how
     * many it took (at least 1 on success); the cache writes the rest
     * through writepage. Called with the page cache's lock held, so it
     * must not call back into the cache. */
    int (*writepages)(struct vnode *vn, uint64_t index, void *const *pages, unsigned n, unsigned *done);
    int (*truncate)(struct vnode *vn, uint64_t size);
    int64_t (*read)(struct vnode *vn, uint64_t off, void *buf, size_t len);      /* VNODE_CHR */
    int64_t (*write)(struct vnode *vn, uint64_t off, const void *buf, size_t len);
    int (*sync)(struct vnode *vn);
    void (*evict)(struct vnode *vn);
};

#define VNODE_PINNED (1u << 0)   /* the filesystem holds a reference while linked */
#define VNODE_DEAD   (1u << 1)   /* unlinked; no new lookups */

struct vnode {
    struct kobject obj;
    struct mount *mnt;
    uint64_t ino;
    enum vnode_type type;
    uint32_t mode;
    uint32_t uid, gid;
    uint32_t nlink;
    uint64_t size;
    uint64_t mtime_ns, ctime_ns;
    const struct vnode_ops *ops;
    void *fs_priv;
    struct pagecache pc;
    struct mutex lock;
    struct mount *covered_by;
    struct list_node hash_link;
    unsigned flags;
};

struct fs_type {
    const char *name;
    int (*mount)(struct fs_type *fs, struct blkdev *bdev, unsigned flags, struct mount *mnt);
    int (*unmount)(struct mount *mnt);
    int (*sync)(struct mount *mnt);
    struct list_node link;
};

#define MOUNT_RDONLY (1u << 0)
#define MOUNT_CACHE_IS_STORE (1u << 1)   /* the page cache holds the only copy (ramfs): never reclaimed */

struct mount {
    struct kobject obj;
    struct fs_type *fs;
    struct vnode *root;
    struct vnode *mountpoint;
    struct mount *parent;
    struct blkdev *bdev;
    void *fs_priv;
    unsigned flags;
    struct list_node vnodes[VNODE_HASH];
    spinlock_t lock;          /* the vnode hash and nr_vnodes only: a leaf below every mutex */
    struct mutex rename_lock; /* serialises renames on this mount; keeps the ancestry stable */
    struct mutex sync_lock;   /* fs->sync against fs->unmount */
    struct list_node link;
    unsigned nr_vnodes;
    uint64_t next_ino;        /* for filesystems that number in memory */
    uint64_t cache_pages;     /* pages the page cache holds for this mount (atomic) */
    uint64_t cache_dirty;     /* of which dirty (atomic); a filesystem's writeback threshold */
    uint64_t cache_limit_pages;   /* a miss beyond this is -ENOSPC; 0: no budget (docs/kernel/security/design.md §3) */
    bool unmounting;          /* set under mountpoint->lock while vfs_umount decides */
    bool unmounted;           /* set under sync_lock once fs->unmount ran */
};

struct file {
    struct kobject obj;
    struct vnode *vn;
    uint64_t pos;
    unsigned flags;           /* O_* */
    struct mutex lock;
};

/* --- lifecycle ---------------------------------------------------------- */

void vfs_init(void);                           /* registers ramfs, mounts the root */
int vfs_register_fs(struct fs_type *fs);
struct fs_type *vfs_find_fs(const char *name);
struct vnode *vfs_root(void);                  /* referenced */

/* Mount `fsname` (backed by bdev or NULL) on the directory `path`.
 * -ENOENT/-ENOTDIR for the target, -EBUSY if already a mountpoint,
 * -ENODEV for an unknown filesystem, or the filesystem's error. */
int vfs_mount(const char *path, const char *fsname, struct blkdev *bdev, unsigned flags);
/* Commit, then dismantle. -EBUSY while any vnode is referenced beyond
 * the filesystem's own references; a failed commit returns its error
 * and leaves the mount in place. VFS_UMOUNT_FORCE skips the commit and
 * drops whatever transaction is open (recovery from an abandoned one). */
#define VFS_UMOUNT_FORCE (1u << 0)
int vfs_umount2(const char *path, unsigned flags);
static inline int vfs_umount(const char *path) { return vfs_umount2(path, 0); }
int vfs_sync(void);

/* --- vnodes (for filesystems) ----------------------------------------- */

/* Allocate and initialise a vnode with one reference; the filesystem
 * fills type/size/ops/fs_priv, then vnode_hash_insert publishes it. */
struct vnode *vnode_alloc(struct mount *mnt, uint64_t ino);
void vnode_hash_insert(struct vnode *vn);
struct vnode *vnode_lookup_cached(struct mount *mnt, uint64_t ino);   /* referenced or NULL */
/* True if any vnode of `mnt` satisfies `pred`. A hashed vnode always
 * holds a reference, so the cache is exactly the set of vnodes in use,
 * and this answers "is anything of this kind open?" - which is how a
 * filesystem refuses to dismantle storage someone is still reading
 * (docs/kernel-services/vfs/invariants.md, V23). The mount's hash lock
 * is held across the walk: `pred` must not sleep, and the vnode it is
 * shown is not referenced for it and must not be kept. */
bool vnode_cache_any(struct mount *mnt, bool (*pred)(const struct vnode *vn, void *arg), void *arg);
static inline void vnode_get(struct vnode *vn) { kobject_get(&vn->obj); }
/* Drop a reference. The last one unhashes the vnode under the mount's hash
 * lock before it falls to zero, so a hashed vnode always has a reference
 * (docs/kernel/lockdep/design.md, "the vnode cache"). */
void vnode_put(struct vnode *vn);
uint64_t vfs_now_ns(void);

/* --- namespace operations ----------------------------------------------- */

/* Discretionary access control (docs/kernel-services/vfs/design.md,
 * "Permissions"): the caller's credentials (cred_current) against the
 * vnode's mode bits by owner, group (effective or supplementary) or
 * other. A privileged caller passes every check except execute, which
 * needs at least one x bit on a regular file. Search (traversal) of a
 * directory is VFS_MAY_EXEC; the path walk applies it to every component
 * it enters, the mutation paths require write and search on the parent,
 * open requires read and/or write per the access mode on the file itself. */
#define VFS_MAY_EXEC  1u
#define VFS_MAY_WRITE 2u
#define VFS_MAY_READ  4u
int vfs_permission(const struct vnode *vn, unsigned mask);

/* Resolve `path` (absolute, or relative to `start` when not NULL) to a
 * referenced vnode. Follows mounts; no symlinks. */
int vfs_lookup(struct vnode *start, const char *path, struct vnode **out);
int vfs_open(struct vnode *start, const char *path, unsigned flags, uint32_t mode, struct file **out);
/* An open file over an already resolved vnode (the reference is consumed,
 * also on failure). No permission check: the caller made its own (exec). */
int vfs_open_vnode(struct vnode *vn, unsigned flags, struct file **out);
int vfs_mkdir(struct vnode *start, const char *path, uint32_t mode);
int vfs_unlink(struct vnode *start, const char *path);
int vfs_rmdir(struct vnode *start, const char *path);
int vfs_rename(struct vnode *start, const char *oldpath, const char *newpath);
/* Set a regular file's length, dropping what is above it and reading as
 * zeros below a length it grew to. O_TRUNC is this with a size of zero;
 * this is the rest of it, which a filesystem that stores several blocks
 * as one object has to implement anyway (a record cut in half is
 * nothing). -EISDIR, -EACCES, -ENOTSUP. */
int vfs_truncate(struct vnode *start, const char *path, uint64_t size);
int vfs_stat(struct vnode *start, const char *path, struct cosmo_stat *st);
void vnode_stat(struct vnode *vn, struct cosmo_stat *st);

/* --- files ---------------------------------------------------------------- */

int64_t file_read(struct file *f, void *buf, size_t len);
int64_t file_write(struct file *f, const void *buf, size_t len);
int64_t file_pread(struct file *f, void *buf, size_t len, uint64_t off);
int64_t file_pwrite(struct file *f, const void *buf, size_t len, uint64_t off);
int64_t file_seek(struct file *f, int64_t off, int whence);
int file_stat(struct file *f, struct cosmo_stat *st);
/* Pack struct cosmo_dirent records; returns bytes, 0 at end. */
int64_t file_readdir(struct file *f, void *buf, size_t len);
int file_sync(struct file *f);
static inline void file_get(struct file *f) { kobject_get(&f->obj); }
static inline void file_put(struct file *f) { kobject_put(&f->obj); }
/* True if the kobject is a file (for handle-based system calls). */
struct file *file_from_kobject(struct kobject *obj);

/* ramfs: create /boot, /tmp, /mnt, /dev and copy the boot archive into /boot. */
void ramfs_populate_boot(void);

/* A character device node in the ramfs: reads and writes go to ops (the
 * vnode lock is held); `priv` is available as vn->fs_priv->chr_priv via
 * ramfs_chr_priv(). Used for /dev/vmm. */
struct chrdev_ops {
    int64_t (*read)(struct vnode *vn, uint64_t off, void *buf, size_t len);
    int64_t (*write)(struct vnode *vn, uint64_t off, const void *buf, size_t len);
};
int ramfs_mkchr(const char *path, uint32_t mode, const struct chrdev_ops *ops, void *priv, struct vnode **out);
void *ramfs_chr_priv(const struct vnode *vn);

/* Diagnostics. */
unsigned vfs_mount_count(void);
unsigned vfs_vnode_count(void);
void vfs_dump(void);

#endif /* KERNEL_VFS_H */
