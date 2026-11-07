/*
 * blkctl_floor.c - The C under blkctl.cee (roadmap 2.0 SA7): a file's
 * state, a C struct round a struct mutex whose size the build's lock
 * debugging decides, with where its fields are; the operation table, C
 * data naming the operations by address; and the values blkctl.cee writes
 * as its own constants, asserted. /dev/blkctl itself is CeeSafe
 * (blkctl.cee, generated into blkctl.c).
 */

#include <kernel/cred.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/mutex.h>
#include <kernel/part.h>
#include <kernel/vfs.h>

#include <uapi/cosmo/blkctl.h>

_Static_assert(EPERM == 1 && ENOMEM == 12 && EBUSY == 16 && ENODEV == 19 && EINVAL == 22 && ERANGE == 34 &&
                   KMEM_ZERO == 1 && PRIV_SYS_ADMIN == 2,
               "blkctl.cee's errno.h, kmalloc.h and cred.h values");
_Static_assert(COSMO_BLKCTL_VERSION == 1 && COSMO_BLKCTL_LIST == 1 && COSMO_BLKCTL_READ == 2 &&
                   COSMO_BLKCTL_WRITE == 3 && COSMO_BLKCTL_FLUSH == 4 && COSMO_BLKCTL_RESCAN == 5 &&
                   COSMO_BLKCTL_FORMAT == 6 && COSMO_BLKCTL_FORMAT_MIRROR == 7 && COSMO_BLKCTL_FORMAT_KEYED == 8 &&
                   COSMO_BLKCTL_IO_MAX == 32768 && COSMO_BLKCTL_NAME == 16 && COSMO_BLKCTL_LABEL == 16 &&
                   COSMO_BLKCTL_PART == 1 && COSMO_BLKCTL_RDONLY == 2 && COSMO_BLKCTL_BOOT == 4 &&
                   COSMO_BLKCTL_MOUNTED == 8 && COSMO_BLKCTL_TEST == 16 && BLKDEV_NAME_MAX == 16,
               "blkctl.cee's blkctl.h values");
_Static_assert(sizeof(struct cosmo_blkctl) == 48 && sizeof(struct cosmo_blkctl_keyed) == 848 &&
                   offsetof(struct cosmo_blkctl_keyed, slot) == 80 && sizeof(struct cosmo_blkctl_dev) == 96 &&
                   sizeof(struct cosmo_blkctl_list) == 8 && sizeof(struct cosmo_blkctl_rescan) == 8 &&
                   sizeof(struct blk_part_info) <= 8 * 8,
               "blkctl.cee's struct sizes and slots");

/* The operations, CeeSafe (blkctl.cee). */
int blkctl_open_file(struct vnode *vn, struct file *f);
void blkctl_release(struct vnode *vn, struct file *f);
int64_t blkctl_read_file(struct vnode *vn, struct file *f, uint64_t off, void *buf, size_t len);
int64_t blkctl_write_file(struct vnode *vn, struct file *f, uint64_t off, const void *buf, size_t len);

/* What blkctl.cee calls here. */
const struct chrdev_ops *blkctl_floor_ops(void);
size_t blkctl_floor_open_layout(unsigned which);

/* The result of this file's last command, read back until the next one. */
struct blkctl_open {
    struct mutex lock;
    void *result;
    size_t len;
};
_Static_assert(offsetof(struct blkctl_open, lock) == 0, "blkctl.cee's lock of a file");

/* blkctl.cee's OPEN_SIZE (0), OPEN_RESULT (1) and OPEN_LEN (2). */
size_t blkctl_floor_open_layout(unsigned which)
{
    return which == 0 ? sizeof(struct blkctl_open)
         : which == 1 ? offsetof(struct blkctl_open, result)
                      : offsetof(struct blkctl_open, len);
}

static const struct chrdev_ops blkctl_ops = {
    .open = blkctl_open_file,
    .release = blkctl_release,
    .read_file = blkctl_read_file,
    .write_file = blkctl_write_file,
};

const struct chrdev_ops *blkctl_floor_ops(void)
{
    return &blkctl_ops;
}
