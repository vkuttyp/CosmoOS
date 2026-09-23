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
#include <kernel/wait.h>
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
    VNODE_LNK = COSMO_DT_LNK,
    VNODE_SOCK = COSMO_DT_SOCK,   /* a unix socket's name: made by bind through mknod; open() is -ENXIO */
    VNODE_FIFO = COSMO_DT_FIFO,   /* a named pipe: the pipe's ring behind a node (kernel/ipc/fifo.c) */
};

/* How many symbolic links one path resolution may expand before it is
 * called a loop (docs/audit/next-subsystem-symlink.md). The component
 * count bounds the walk as well, and both answer -ELOOP. */
#define VFS_MAX_SYMLINKS 8u

/* readdir callback: return nonzero to stop. */
typedef int (*vfs_dirent_cb)(void *arg, const char *name, size_t len, uint64_t ino, enum vnode_type type);

struct file;   /* defined below; named by the per-open vnode ops */

struct vnode_ops {
    int (*lookup)(struct vnode *dir, const char *name, size_t len, struct vnode **out);
    int (*create)(struct vnode *dir, const char *name, size_t len, uint32_t mode, struct vnode **out);
    /* Optional. Create `name` in `dir` as a symbolic link to `target`, a
     * NUL-terminated path which is never validated: a link to nothing is
     * a link. A filesystem without this has no links (-EPERM). */
    int (*symlink)(struct vnode *dir, const char *name, size_t len, const char *target, struct vnode **out);
    /* Optional, required of a filesystem that has links. Copies the
     * target into `buf`, at most `len` bytes and NOT NUL terminated;
     * returns the bytes copied, or -errno. */
    int (*readlink)(struct vnode *vn, char *buf, size_t len);
    int (*mkdir)(struct vnode *dir, const char *name, size_t len, uint32_t mode, struct vnode **out);
    /* A special node -- VNODE_SOCK, a unix socket's name, or VNODE_FIFO,
     * a named pipe. Optional: a filesystem without it refuses with
     * -EOPNOTSUPP (cosmofs has no on-disk type for one). Under dir->lock,
     * like create. */
    int (*mknod)(struct vnode *dir, const char *name, size_t len, uint32_t mode, enum vnode_type type,
                 struct vnode **out);
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
     * many it took (at least 1 on success, at most `n`); the pages it
     * did not take stay dirty and are offered again from the first of
     * them, so a filesystem that takes one page at a time makes the
     * same progress it would have through writepage. Called with the
     * page cache's lock held, so it must not call back into the
     * cache. */
    int (*writepages)(struct vnode *vn, uint64_t index, void *const *pages, unsigned n, unsigned *done);
    int (*truncate)(struct vnode *vn, uint64_t size);
    /* Optional per-open lifecycle (devices with an instance per open). open
     * runs on every open of the node and may refuse; release runs once, when
     * the last reference to that struct file drops. read_file/write_file are
     * preferred over read/write by file_pread/pwrite and carry the file, so a
     * device reaches its per-open state (f->priv). Per-open state lives and
     * dies with the struct file, never with the vnode. */
    int (*open)(struct vnode *vn, struct file *f);
    void (*release)(struct vnode *vn, struct file *f);
    int64_t (*read_file)(struct vnode *vn, struct file *f, uint64_t off, void *buf, size_t len);
    int64_t (*write_file)(struct vnode *vn, struct file *f, uint64_t off, const void *buf, size_t len);
    int64_t (*read)(struct vnode *vn, uint64_t off, void *buf, size_t len);      /* VNODE_CHR */
    int64_t (*write)(struct vnode *vn, uint64_t off, const void *buf, size_t len);
    /* Optional readiness, for a file that can block (a FIFO, a device):
     * the file kobject type delegates SYS_ioready, poll, the async ring
     * and SYS_setnonblock to these. Without them a file is always
     * readable and writable, never changes, and cannot be made
     * non-blocking (-EOPNOTSUPP) -- right for a regular file. */
    unsigned (*ready)(struct vnode *vn, struct file *f);
    struct waitqueue *(*poll_wq)(struct vnode *vn, struct file *f, unsigned events);
    int (*set_nonblock)(struct vnode *vn, struct file *f, int on);
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
    /* The mounts covering this directory, at most one per mount
     * namespace, protected by this vnode's lock. A list rather than a
     * pointer because a directory can be a mountpoint in one namespace
     * and an ordinary directory in another
     * (docs/kernel-services/vfs/design.md, "Mounts and namespaces"). */
    struct list_node covers;
    struct list_node hash_link;
    unsigned flags;
};

struct cosmofs_check_report;
struct cosmofs_scrub_stats;

struct fs_type {
    const char *name;
    int (*mount)(struct fs_type *fs, struct blkdev *bdev, unsigned flags, struct mount *mnt);
    int (*unmount)(struct mount *mnt);
    int (*sync)(struct mount *mnt);
    /*
     * The maintenance passes, both optional: the VFS learns that a
     * filesystem has one, never what one is. A filesystem with none
     * leaves them null and /dev/fsctl refuses a command against it
     * before anything is locked (docs/audit/next-subsystem-fsctl.md).
     *
     * The out-structs are cosmofs's and stay so -- declared above and
     * never dereferenced here, because the VFS hands the pointer to the
     * filesystem and copies the bytes out by size. One implementation
     * does not earn a filesystem-neutral result type.
     */
    int (*check)(struct mount *mnt, struct cosmofs_check_report *out, unsigned flags);
    int (*scrub)(struct mount *mnt, struct cosmofs_scrub_stats *out);
    struct list_node link;
};

#define MOUNT_RDONLY (1u << 0)
#define MOUNT_CACHE_IS_STORE (1u << 1)   /* the page cache holds the only copy (ramfs): never reclaimed */

struct mount {
    struct kobject obj;
    /*
     * The mount's name, for anything that acts on one mount rather than
     * on a path (docs/audit/next-subsystem-fsctl.md). Never reused: a
     * counter and not an index, so an operator holding a stale id
     * commands nothing rather than a filesystem they never listed. It
     * is the same number in every namespace that can see the mount, and
     * the path is not -- which is the whole reason it exists.
     */
    uint64_t id;
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
    struct list_node cover_link;   /* on its mountpoint's `covers` list */
    /* The namespaces that can see this mount, as struct mount_ns_ref.
     * Under the mountpoint's lock, the same lock as cover_link: where a
     * mount is attached and who can see it are one question, and a
     * walker holding that lock must be able to ask both without taking
     * g_mounts_lock inside a vnode lock. The root mount is not on this
     * machinery at all -- every namespace has it. */
    struct list_node ns_refs;
    unsigned nr_vnodes;
    uint64_t next_ino;        /* for filesystems that number in memory */
    uint64_t cache_pages;     /* pages the page cache holds for this mount (atomic) */
    uint64_t cache_dirty;     /* of which dirty (atomic); a filesystem's writeback threshold */
    uint64_t cache_limit_pages;   /* a miss beyond this is -ENOSPC; 0: no budget (docs/kernel/security/design.md §3) */
    /*
     * Maintenance passes running against this mount right now
     * (docs/audit/next-subsystem-fsctl.md). Written under g_mounts_lock,
     * read atomically by the drain below with that lock dropped. An
     * unmount sets `unmounting`, which stops a new pass from starting,
     * and then waits on `passes_quiet` for this to reach zero: a pass
     * walks the filesystem's buffers and tearing them down under it is
     * a crash rather than a wrong answer.
     */
    uint64_t passes_running;
    struct waitqueue passes_quiet;
    bool unmounting;          /* set under mountpoint->lock while vfs_umount decides */
    bool unmounted;           /* set under sync_lock once fs->unmount ran */
};

struct file {
    struct kobject obj;
    struct vnode *vn;
    uint64_t pos;
    unsigned flags;           /* O_*, including O_NONBLOCK as given at open */
    struct mutex lock;
    void *priv;               /* a device's per-open instance (chrdev open/release) */
    bool dev_open;            /* the vnode's open hook ran and succeeded; release will run */
    uint32_t wb_seq_seen;     /* the write-back failure sequence this file has been told about
                                 (pagecache.wb_seq at open; advanced by each report) */
    /* A directory file's normalised absolute path, as the door that
     * opened it resolved it; NULL for anything else. Set once, before the
     * file is installed in a handle table, and never changed, so a reader
     * holding a reference needs no lock. What fchdir publishes as the
     * working directory's name (docs/audit/next-subsystem-dirfd.md, P31);
     * stale after a rename of the directory or of any ancestor, as the
     * cwd's own name is (P27). */
    char *dir_path;
};

/* --- lifecycle ---------------------------------------------------------- */

void vfs_init(void);                           /* registers ramfs, mounts the root */
int vfs_register_fs(struct fs_type *fs);
struct fs_type *vfs_find_fs(const char *name);
struct vnode *vfs_root(void);                  /* referenced */
/*
 * The root the calling process sees, referenced. Every absolute path
 * starts here and ".." stops here, so a process given a root below the
 * global one cannot name anything outside it -- filesystem isolation,
 * the first of the container primitives that needs more than a handle
 * (docs/kernel/process/design.md, "Per-process roots").
 *
 * Supplied by the process layer, the way cred_current supplies
 * credentials: the VFS asks for the caller's context rather than having
 * it threaded through every entry point. Falls back to the global root
 * for kernel threads and before there are processes.
 */
struct vnode *vfs_current_root(void);

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
/* `n` references in one add (a requeue's moved waiters, counted under the bucket locks). */
static inline void vnode_get_n(struct vnode *vn, unsigned n) { kobject_get_n(&vn->obj, n); }
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
 * referenced vnode. Follows mounts, and symbolic links -- including one
 * named by the last component. An absolute target restarts at the
 * calling process's root, so a link is bounded by the root a leading
 * slash is bounded by. */
int vfs_lookup(struct vnode *start, const char *path, struct vnode **out);
/* The same, stopping at a link named by the last component. */
int vfs_lookup_nofollow(struct vnode *start, const char *path, struct vnode **out);
int vfs_open(struct vnode *start, const char *path, unsigned flags, uint32_t mode, struct file **out);
/* An open file over an already resolved vnode (the reference is consumed,
 * also on failure). No permission check: the caller made its own (exec). */
int vfs_open_vnode(struct vnode *vn, unsigned flags, struct file **out);
int vfs_mkdir(struct vnode *start, const char *path, uint32_t mode);
int vfs_unlink(struct vnode *start, const char *path);
/* Make a special node (VNODE_SOCK or VNODE_FIFO) at `path`: -EEXIST if the name exists,
 * -EOPNOTSUPP if the filesystem has no mknod, -EACCES without write
 * permission on the directory; the new node referenced in *out. */
int vfs_mknod(struct vnode *start, const char *path, uint32_t mode, enum vnode_type type, struct vnode **out);
int vfs_rmdir(struct vnode *start, const char *path);
int vfs_rename(struct vnode *start, const char *oldpath, const char *newpath);
/* renameat: the two paths resolve from two starts. vfs_rename(s, a, b)
 * is vfs_rename2(s, a, s, b). */
int vfs_rename2(struct vnode *ostart, const char *oldpath, struct vnode *nstart, const char *newpath);
/* Record a directory file's path (a no-op for anything else, or on
 * allocation failure: the file then has no name to give fchdir, which
 * refuses it rather than invent one). Before the file is shared. */
void file_set_dir_path(struct file *f, const char *abs);
/* Set a regular file's length, dropping what is above it and reading as
 * zeros below a length it grew to. O_TRUNC is this with a size of zero;
 * this is the rest of it, which a filesystem that stores several blocks
 * as one object has to implement anyway (a record cut in half is
 * nothing). -EISDIR, -EACCES, -ENOTSUP. */
int vfs_truncate(struct vnode *start, const char *path, uint64_t size);
int vfs_stat(struct vnode *start, const char *path, struct cosmo_stat *st);
/* stat without following a link named by the last component. */
int vfs_lstat(struct vnode *start, const char *path, struct cosmo_stat *st);
/* Create `path` as a symbolic link to `target`. -EEXIST, -EPERM where
 * the filesystem has no links, -ENAMETOOLONG, -EROFS, -ENOSPC. */
int vfs_symlink(struct vnode *start, const char *path, const char *target);
/* The target of the link named by `path`, at most `len` bytes and not
 * NUL terminated. Returns the bytes copied, or -EINVAL when the last
 * component is not a link. */
int vfs_readlink(struct vnode *start, const char *path, char *buf, size_t len);
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
/* The file type's flush, run by handle_close before it drops the handle's
 * reference: write dirty pages back, then report a write-back failure
 * recorded since this file last heard (its own attempt's or a
 * neighbour's), once. Its result is close's result; the handle closes
 * regardless. */
int file_flush(struct file *f);
/* A device's per-open non-blocking mode is the open file's COSMO_O_NONBLOCK
 * bit, stored by open and switched by SYS_setnonblock through the device's
 * set_nonblock (the device-readiness unit): a device's read_file asks
 * file_nonblocking(f), its set_nonblock is file_set_nonblock. Nothing is
 * allocated per open; the FIFO keeps its own record for its side. */
bool file_nonblocking(const struct file *f);
int file_set_nonblock(struct file *f, int on);   /* 1/0 sets, -1 asks; returns the previous value */
/* Every reader of an open file's flags goes through this: the non-blocking
 * bit is switched with an atomic read-modify-write while I/O runs, and a
 * plain read beside that is a data race by the letter. */
static inline unsigned file_flags(const struct file *f) { return __atomic_load_n(&f->flags, __ATOMIC_RELAXED); }
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
    /* Optional: an instance per open. open sets f->priv (or refuses);
     * release runs once on the last close; read_file/write_file, when set,
     * are used instead of read/write and receive the file. */
    int (*open)(struct vnode *vn, struct file *f);
    void (*release)(struct vnode *vn, struct file *f);
    int64_t (*read_file)(struct vnode *vn, struct file *f, uint64_t off, void *buf, size_t len);
    int64_t (*write_file)(struct vnode *vn, struct file *f, uint64_t off, const void *buf, size_t len);
    /* Optional readiness (the named-pipes unit): a device whose read_file
     * can block may say so. No existing device sets them yet. */
    unsigned (*ready)(struct vnode *vn, struct file *f);
    struct waitqueue *(*poll_wq)(struct vnode *vn, struct file *f, unsigned events);
    int (*set_nonblock)(struct vnode *vn, struct file *f, int on);
};
int ramfs_mkchr(const char *path, uint32_t mode, const struct chrdev_ops *ops, void *priv, struct vnode **out);
void *ramfs_chr_priv(const struct vnode *vn);

/* Diagnostics. */
unsigned vfs_mount_count(void);

/* Create /dev/fsctl. Called once at boot, after the ramfs has /dev. */
void fsctl_dev_init(void);

/*
 * Name a mount for an operation that acts on one filesystem rather than
 * on a path. Takes a reference and counts a pass, so the mount cannot be
 * freed and an unmount waits rather than tearing down underneath.
 *
 * -ENOENT if the calling process's mount namespace does not hold that
 * id (which is not the same as "no such mount", and is the right answer:
 * a mount another namespace holds is not this caller's), -EBUSY if it is
 * already being unmounted.
 *
 * The caller must not take g_mounts_lock between these two: the mount is
 * acquired and released with it dropped, and an unmount draining the
 * count holds it while it waits.
 */
int vfs_mount_acquire(uint64_t id, struct mount **out);
void vfs_mount_release(struct mount *mnt);
unsigned vfs_vnode_count(void);
void vfs_dump(void);

/*
 * The held-walk seam (docs/audit/next-subsystem-cwd-hold.md). CONFIG_DEBUG
 * only; every entry point is a no-op otherwise.
 *
 * Armed for a process NAME, the next relative-path walk made by a
 * process of that name -- by any thread but the one registered as the
 * swapper -- is held in walk_parent's relative branch, immediately
 * before it takes its reference on the starting directory, with the
 * pointer it read in hand. It resumes when that process's chdir has
 * published its new directory and put the old one. The order is
 * enforced here, not arranged by the test: chdir waits for the hold
 * BEFORE it publishes, and the walk waits for the put. One arm, one
 * hold; arm again for the next pass. Both waits are killable and
 * bounded, and a timeout is recorded, never hidden.
 *
 * What the seam records is derived from what it can observe, not
 * declared by the code under test: `released_after_put` is the count
 * the releasing side reads at the instant it releases being one lower
 * than what it read before its put -- read by the releaser, at the
 * release, because a walk woken before the put loses the race to it
 * every time and a count read on resume cannot tell the orders apart
 * (the release-before-put mutation survived that derivation).
 */
struct vfs_cwd_hold_record {
    bool held;                 /* a walk was held */
    bool held_matches_old;     /* the held walk's directory is the one the swapper replaced */
    bool released_after_put;   /* refcount at the release == ref_before_put - 1: the put preceded the release */
    bool resumed_dead;         /* the directory was VNODE_DEAD when the walk resumed (pass 2 expects it) */
    bool swapper_was_held;     /* the swapper's own relative walk was what got held: a wrong racer, named */
    bool walk_timed_out, swap_timed_out;
    bool interrupted;          /* a killable wait returned -EINTR: the racer was dying */
    uint32_t ref_at_hold;      /* the directory's refcount when the walk was held */
    uint32_t ref_before_put;   /* what the swapper saw before its put: 2 with the fix, 1 without (pass 2) */
    uint32_t ref_at_release;   /* what the releaser saw at the instant it released */
    uint32_t ref_at_resume;    /* what the walk saw when it resumed */
    int swap_rc;               /* the swapper's wait: 0, -ETIMEDOUT, -EINTR */
    uint64_t t_hold_ns, t_swap_wait_ns, t_swap_done_ns;   /* clock_now_ns at the hold, and around the swapper's wait */
};
void vfs_test_cwd_hold_arm(const char *process_name);
unsigned vfs_test_cwd_hold_state(void);                             /* 0 idle, 1 armed, 2 held, 3 released, 4 swapper waiting */
void vfs_test_cwd_hold_disarm(struct vfs_cwd_hold_record *out);     /* after every pass and on every exit */

/* The swap half, called by process_chdir in this order: enter before its
 * own lookup (registers the swapper), wait after the lookup and before
 * the publish, before_put and after_put around vnode_put(old). */
void vfs_cwd_hold_swapper_enter(void);
void vfs_cwd_hold_swap_wait(void);
void vfs_cwd_hold_before_put(struct vnode *old);
void vfs_cwd_hold_after_put(struct vnode *old);
/* A chdir or fchdir that registered as the swapper and then failed:
 * deregister, and release a walk held for it -- nothing will be put, and
 * a held walk must not wait out its bound for a swap that is not coming
 * (found in review of the dirfd unit). */
void vfs_cwd_hold_swapper_leave(void);

#endif /* KERNEL_VFS_H */
