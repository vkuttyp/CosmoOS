/*
 * ramfs_floor.c - The C under ramfs.cee (roadmap 2.0 SA7): the operation
 * tables, which ramfs.cee hands out and compares by address, and
 * ramfs_fs_type, which boot composition registers by name -- C data
 * naming the operations by address; readdir's entry point and the call
 * through its callback, whose type (vfs_dirent_cb) takes an enum
 * vnode_type a CeeSafe function type cannot spell; the boot archive's
 * entries, whose function has its struct's name; and the values
 * ramfs.cee writes as its own constants, asserted. ramfs itself is
 * CeeSafe (ramfs.cee, generated into ramfs.c).
 */

#include <kernel/bootarchive.h>
#include <kernel/cred.h>
#include <kernel/errno.h>
#include <kernel/lockdep.h>
#include <kernel/page.h>
#include <kernel/vfs.h>

_Static_assert(EPERM == 1 && ENOENT == 2 && EIO == 5 && ENOMEM == 12 && EEXIST == 17 && ENOTDIR == 20 &&
                   EINVAL == 22 && EFBIG == 27 && ENAMETOOLONG == 36 && ENOTEMPTY == 39 && ENOTSUP == 95 &&
                   EOPNOTSUPP == ENOTSUP,
               "ramfs.cee's errno.h values");
_Static_assert(VNODE_REG == 1 && VNODE_DIR == 2 && VNODE_CHR == 3 && VNODE_FIFO == 4 && VNODE_SOCK == 5 &&
                   VNODE_LNK == 6 && VNODE_PINNED == 1 && VNODE_DEAD == 2 && VNODE_NESTED_CHILD == 2 &&
                   MOUNT_CACHE_IS_STORE == 2 && MOUNT_SCRATCH == 4 && VFS_NAME_MAX == 255 && VFS_PATH_MAX == 1024 &&
                   PAGE_SIZE == 4096 && CRED_ROOT_UID == 0 && COSMO_O_WRONLY == 1 && COSMO_O_CREAT == 0x40 &&
                   COSMO_O_TRUNC == 0x200 && COSMO_IO_READABLE == 1 && COSMO_IO_WRITABLE == 2 &&
                   BOOTARCHIVE_NAME_MAX == 100,
               "ramfs.cee's vfs.h, lockdep.h, cred.h, syscall.h and bootarchive.h values");

/* The operations, CeeSafe (ramfs.cee). */
int ramfs_lookup(struct vnode *dir, const char *name, size_t len, struct vnode **out);
int ramfs_create(struct vnode *dir, const char *name, size_t len, uint32_t mode, struct vnode **out);
int ramfs_mkdir(struct vnode *dir, const char *name, size_t len, uint32_t mode, struct vnode **out);
int ramfs_mknod(struct vnode *dir, const char *name, size_t len, uint32_t mode, uint32_t type, struct vnode **out);

/* mknod's type is an enum vnode_type, which a CeeSafe function cannot
 * spell: the table's entry has the enum, and passes it on as the integer
 * it is (a call through a pointer of another type is what UBSan's
 * function check refuses, CI's clang 19, 2026-11-08). */
static int ramfs_mknod_op(struct vnode *dir, const char *name, size_t len, uint32_t mode, enum vnode_type type,
                          struct vnode **out)
{
    return ramfs_mknod(dir, name, len, mode, (uint32_t)type, out);
}
int ramfs_symlink(struct vnode *dir, const char *name, size_t len, const char *target, struct vnode **out);
int ramfs_readlink(struct vnode *vn, char *buf, size_t len);
int ramfs_link(struct vnode *dir, const char *name, size_t len, struct vnode *target);
int ramfs_unlink(struct vnode *dir, const char *name, size_t len, struct vnode *victim);
int ramfs_rmdir(struct vnode *dir, const char *name, size_t len, struct vnode *victim);
int ramfs_rename(struct vnode *odir, const char *oname, size_t olen, struct vnode *victim, struct vnode *ndir,
                 const char *nname, size_t nlen, struct vnode *replaced);
int ramfs_readdir_cee(struct vnode *dir, uint64_t *pos, void *cb, void *arg);
int ramfs_readpage(struct vnode *vn, uint64_t index, void *buf);
int ramfs_writepage(struct vnode *vn, uint64_t index, const void *buf);
int ramfs_truncate(struct vnode *vn, uint64_t size);
void ramfs_evict(struct vnode *vn);
int ramfs_fifo_open(struct vnode *vn, struct file *f);
void ramfs_fifo_release(struct vnode *vn, struct file *f);
int64_t ramfs_fifo_read(struct vnode *vn, struct file *f, uint64_t off, void *buf, size_t len);
int64_t ramfs_fifo_write(struct vnode *vn, struct file *f, uint64_t off, const void *buf, size_t len);
unsigned ramfs_fifo_ready(struct vnode *vn, struct file *f);
struct waitqueue *ramfs_fifo_poll_wq(struct vnode *vn, struct file *f, unsigned events);
int ramfs_fifo_set_nonblock(struct vnode *vn, struct file *f, int on);
int64_t ramfs_chr_read(struct vnode *vn, uint64_t off, void *buf, size_t len);
int64_t ramfs_chr_write(struct vnode *vn, uint64_t off, const void *buf, size_t len);
int ramfs_chr_open(struct vnode *vn, struct file *f);
void ramfs_chr_release(struct vnode *vn, struct file *f);
int64_t ramfs_chr_read_file(struct vnode *vn, struct file *f, uint64_t off, void *buf, size_t len);
int64_t ramfs_chr_write_file(struct vnode *vn, struct file *f, uint64_t off, const void *buf, size_t len);
unsigned ramfs_chr_ready(struct vnode *vn, struct file *f);
struct waitqueue *ramfs_chr_poll_wq(struct vnode *vn, struct file *f, unsigned events);
int ramfs_chr_set_nonblock(struct vnode *vn, struct file *f, int on);
int ramfs_mount(struct fs_type *fs, struct blkdev *bdev, unsigned flags, struct mount *mnt);
int ramfs_unmount(struct mount *mnt);

/* What ramfs.cee calls here. */
const struct vnode_ops *ramfs_floor_ops(unsigned which);
int ramfs_floor_emit(void *cb, void *arg, const char *name, size_t len, uint64_t ino, unsigned type);
const struct bootarchive_entry *ramfs_floor_archive(unsigned index);

/* The archive's entry `index`: bootarchive_entry, a function CeeSafe cannot name beside its struct. */
const struct bootarchive_entry *ramfs_floor_archive(unsigned index)
{
    return bootarchive_entry(index);
}

static int ramfs_readdir(struct vnode *dir, uint64_t *pos, vfs_dirent_cb cb, void *arg)
{
    return ramfs_readdir_cee(dir, pos, (void *)cb, arg);
}

/* One entry to the reader: nonzero when it wants no more. */
int ramfs_floor_emit(void *cb, void *arg, const char *name, size_t len, uint64_t ino, unsigned type)
{
    vfs_dirent_cb f = (vfs_dirent_cb)cb;
    return f(arg, name, len, ino, (enum vnode_type)type);
}

static const struct vnode_ops ramfs_ops[] = {
    [0] = {   /* directories */
        .lookup = ramfs_lookup,
        .create = ramfs_create,
        .mkdir = ramfs_mkdir,
        .unlink = ramfs_unlink,
        .link = ramfs_link,
        .rmdir = ramfs_rmdir,
        .rename = ramfs_rename,
        .readdir = ramfs_readdir,
        .symlink = ramfs_symlink,
        .mknod = ramfs_mknod_op,
        .evict = ramfs_evict,
    },
    [1] = {   /* regular files */
        .readpage = ramfs_readpage,
        .writepage = ramfs_writepage,
        .truncate = ramfs_truncate,
        .evict = ramfs_evict,
    },
    [2] = {   /* symbolic links */
        .readlink = ramfs_readlink,
        .evict = ramfs_evict,
    },
    [3] = {   /* sockets' names */
        .evict = ramfs_evict,
    },
    [4] = {   /* named pipes */
        .open = ramfs_fifo_open,
        .release = ramfs_fifo_release,
        .read_file = ramfs_fifo_read,
        .write_file = ramfs_fifo_write,
        .ready = ramfs_fifo_ready,
        .poll_wq = ramfs_fifo_poll_wq,
        .set_nonblock = ramfs_fifo_set_nonblock,
        .evict = ramfs_evict,
    },
    [5] = {   /* character devices */
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
    },
};

/* ramfs.cee's OPS_DIR (0), OPS_FILE, OPS_LNK, OPS_SOCK, OPS_FIFO and OPS_CHR (5). */
const struct vnode_ops *ramfs_floor_ops(unsigned which)
{
    return &ramfs_ops[which];
}

struct fs_type ramfs_fs_type = {
    .magic = 0x858458f6u,   /* Linux's RAMFS_MAGIC */
    .name = "ramfs",
    .mount = ramfs_mount,
    .unmount = ramfs_unmount,
    .sync = NULL,
};
