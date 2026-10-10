/*
 * blkctl.c - /dev/blkctl: the operator's channel to block devices
 * (uapi/cosmo/blkctl.h; docs/kernel-services/vfs/design.md, "The
 * block-device channel"; roadmap M2).
 *
 * What the installer needs from the kernel to put a system on a blank
 * disk: the list of devices (with partitions and which one the loader
 * was read from), raw sector reads and writes, a flush, a partition
 * rescan, and an empty filesystem. It lives beside /dev/fsctl rather than
 * in the block layer because two of its answers are about mounts (D8:
 * the block layer knows no filesystem).
 *
 * A write is refused while the device, its disk or one of its partitions
 * is mounted: that is the one mistake the kernel can see and the
 * operator cannot undo. Everything about *what* is written is the
 * caller's.
 */

#include <kernel/blk.h>
#include <kernel/bootinfo.h>
#include <kernel/cred.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/mutex.h>
#include <kernel/part.h>
#include <kernel/string.h>
#include <kernel/vfs.h>

#include <uapi/cosmo/blkctl.h>

/* The result of this file's last command, read back until the next one. */
struct blkctl_open {
    struct mutex lock;
    void *result;
    size_t len;
};

static int blkctl_open_file(struct vnode *vn, struct file *f)
{
    (void)vn;
    if (!cred_privileged(cred_current()))
        return -EPERM;   /* the mode is 0600 as well; this is the check that cannot be chmod'ed away */
    struct blkctl_open *o = kmalloc(sizeof(*o), KMEM_ZERO);
    if (o == NULL)
        return -ENOMEM;
    mutex_init(&o->lock, "blkctl-open");
    f->priv = o;
    return 0;
}

static void blkctl_release(struct vnode *vn, struct file *f)
{
    (void)vn;
    struct blkctl_open *o = f->priv;
    if (o == NULL)
        return;
    kfree(o->result);
    kfree(o);
    f->priv = NULL;
}

/* Replace this file's result. Takes ownership of `buf`. */
static void set_result(struct blkctl_open *o, void *buf, size_t len)
{
    mutex_lock(&o->lock);
    void *old = o->result;
    o->result = buf;
    o->len = len;
    mutex_unlock(&o->lock);
    kfree(old);
}

static void fill_dev(struct cosmo_blkctl_dev *d, struct blkdev *bd, const uint8_t *boot, bool have_boot)
{
    memset(d, 0, sizeof(*d));
    strlcpy(d->name, bd->name, sizeof(d->name));
    d->sectors = bd->capacity;
    d->sector_size = bd->sector_size;
    if (bd->read_only)
        d->flags |= COSMO_BLKCTL_RDONLY;
    if (vfs_bdev_mounted(bd))
        d->flags |= COSMO_BLKCTL_MOUNTED;
    struct blk_part_info pi;
    if (blk_part_info(bd, &pi)) {
        d->flags |= COSMO_BLKCTL_PART;
        strlcpy(d->disk, pi.disk, sizeof(d->disk));
        d->start = pi.start;
        d->index = pi.index;
        memcpy(d->uuid, pi.uuid, 16);
        memcpy(d->type, pi.type, 16);
        if (have_boot && memcmp(pi.uuid, boot, 16) == 0)
            d->flags |= COSMO_BLKCTL_BOOT;
    }
}

static int64_t blkctl_list(struct blkctl_open *o)
{
    uint8_t boot[16];
    bool have_boot = bootinfo_boot_partuuid(boot);
    /* Sized now, filled by position: a device that registers in between
     * is left out, one that goes away shortens the list. Never more
     * records than were sized for. */
    unsigned want = blk_count();
    size_t bytes = sizeof(struct cosmo_blkctl_list) + (size_t)want * sizeof(struct cosmo_blkctl_dev);
    uint8_t *buf = kmalloc(bytes, KMEM_ZERO);
    if (buf == NULL)
        return -ENOMEM;
    struct cosmo_blkctl_list *hdr = (struct cosmo_blkctl_list *)buf;
    struct cosmo_blkctl_dev *rec = (struct cosmo_blkctl_dev *)(buf + sizeof(*hdr));
    unsigned n = 0;
    for (unsigned i = 0; n < want; i++) {
        struct blkdev *bd = blk_nth(i);
        if (bd == NULL)
            break;
        fill_dev(&rec[n++], bd, boot, have_boot);
        blkdev_put(bd);
    }
    hdr->version = COSMO_BLKCTL_VERSION;
    hdr->count = n;
    set_result(o, buf, sizeof(*hdr) + (size_t)n * sizeof(*rec));
    return 0;
}

/* A transfer's size in bytes, or a negative errno. */
static int64_t io_bytes(const struct blkdev *bd, const struct cosmo_blkctl *cmd)
{
    if (cmd->count == 0)
        return -EINVAL;
    uint64_t bytes = (uint64_t)cmd->count * bd->sector_size;
    if (bytes > COSMO_BLKCTL_IO_MAX)
        return -EINVAL;
    return (int64_t)bytes;
}

static int64_t blkctl_read(struct blkctl_open *o, struct blkdev *bd, const struct cosmo_blkctl *cmd)
{
    int64_t bytes = io_bytes(bd, cmd);
    if (bytes < 0)
        return bytes;
    void *buf = kmalloc((size_t)bytes, 0);   /* DMA-able, unlike the syscall's bounce */
    if (buf == NULL)
        return -ENOMEM;
    int rc = blk_read(bd, cmd->sector, cmd->count, buf);
    if (rc) {
        kfree(buf);
        return rc;
    }
    set_result(o, buf, (size_t)bytes);
    return 0;
}

static int64_t blkctl_write(struct blkdev *bd, const struct cosmo_blkctl *cmd, const void *data, size_t len)
{
    int64_t bytes = io_bytes(bd, cmd);
    if (bytes < 0)
        return bytes;
    if (len != (size_t)bytes)
        return -EINVAL;   /* the data is exactly `count` sectors, in the same write */
    if (vfs_bdev_mounted(bd))
        return -EBUSY;
    void *buf = kmalloc(len, 0);
    if (buf == NULL)
        return -ENOMEM;
    memcpy(buf, data, len);
    int rc = blk_write(bd, cmd->sector, cmd->count, buf);
    kfree(buf);
    return rc;
}

static int64_t blkctl_write_file(struct vnode *vn, struct file *f, uint64_t off, const void *buf, size_t len)
{
    (void)vn;
    (void)off;
    struct blkctl_open *o = f->priv;
    if (o == NULL)
        return -EINVAL;
    if (!cred_privileged(cred_current()))
        return -EPERM;   /* the fd may have outlived the privilege that opened it */
    if (len < sizeof(struct cosmo_blkctl))
        return -EINVAL;
    struct cosmo_blkctl cmd;
    memcpy(&cmd, buf, sizeof(cmd));
    if (cmd.version != COSMO_BLKCTL_VERSION)
        return -EINVAL;
    size_t extra = len - sizeof(cmd);
    if (extra != 0 && cmd.op != COSMO_BLKCTL_WRITE)
        return -EINVAL;   /* only a WRITE carries data */
    if (cmd.op == COSMO_BLKCTL_LIST) {
        int64_t rc = blkctl_list(o);
        if (rc)
            set_result(o, NULL, 0);
        return rc ? rc : (int64_t)len;
    }

    cmd.name[sizeof(cmd.name) - 1] = '\0';
    cmd.fstype[sizeof(cmd.fstype) - 1] = '\0';
    struct blkdev *bd = blk_find(cmd.name);
    if (bd == NULL) {
        set_result(o, NULL, 0);
        return -ENODEV;
    }
    int64_t rc;
    switch (cmd.op) {
    case COSMO_BLKCTL_READ:
        rc = blkctl_read(o, bd, &cmd);
        break;
    case COSMO_BLKCTL_WRITE:
        rc = blkctl_write(bd, &cmd, (const uint8_t *)buf + sizeof(cmd), extra);
        if (rc == 0)
            set_result(o, NULL, 0);
        break;
    case COSMO_BLKCTL_FLUSH:
        rc = blk_flush(bd);
        if (rc == 0)
            set_result(o, NULL, 0);
        break;
    case COSMO_BLKCTL_RESCAN: {
        unsigned nparts = 0;
        rc = blk_part_scan(bd, &nparts);
        if (rc == 0) {
            struct cosmo_blkctl_rescan *r = kmalloc(sizeof(*r), KMEM_ZERO);
            if (r == NULL) {
                rc = -ENOMEM;
                break;
            }
            r->partitions = nparts;
            set_result(o, r, sizeof(*r));
        }
        break;
    }
    case COSMO_BLKCTL_FORMAT:
        rc = vfs_format(cmd.fstype, bd);
        if (rc == 0)
            set_result(o, NULL, 0);
        break;
    default:
        rc = -EINVAL;
        break;
    }
    blkdev_put(bd);
    if (rc)
        set_result(o, NULL, 0);
    return rc ? rc : (int64_t)len;
}

/* The result of this file's last command, whole or not at all; nothing
 * when the last command failed or had no result. */
static int64_t blkctl_read_file(struct vnode *vn, struct file *f, uint64_t off, void *buf, size_t len)
{
    (void)vn;
    (void)off;
    struct blkctl_open *o = f->priv;
    if (o == NULL)
        return 0;
    mutex_lock(&o->lock);
    int64_t n;
    if (o->result == NULL)
        n = 0;
    else if (len < o->len)
        n = -ERANGE;
    else {
        memcpy(buf, o->result, o->len);
        n = (int64_t)o->len;
    }
    mutex_unlock(&o->lock);
    return n;
}

static const struct chrdev_ops blkctl_ops = {
    .open = blkctl_open_file,
    .release = blkctl_release,
    .read_file = blkctl_read_file,
    .write_file = blkctl_write_file,
};

static struct vnode *g_blkctl_node;

void blkctl_dev_init(void)
{
    int rc = ramfs_mkchr("/dev/blkctl", 0600, &blkctl_ops, NULL, &g_blkctl_node);
    if (rc) {
        kerror("blkctl: /dev/blkctl: %d", rc);
        return;
    }
    kinfo("blkctl: /dev/blkctl ready (0600): list, read, write, flush, rescan, format");
}
