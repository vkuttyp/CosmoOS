/*
 * memdev.c - /dev/null and /dev/zero (roadmap M3: a shell redirects to
 * /dev/null, and BusyBox ash gives a background job's stdin /dev/null).
 * Character devices on the ramfs /dev, like /dev/console: null reads
 * end-of-file and swallows writes, zero reads zeros and swallows writes.
 */

#include <kernel/log.h>
#include <kernel/string.h>
#include <kernel/vfs.h>

static int64_t null_read(struct vnode *vn, uint64_t off, void *buf, size_t len)
{
    (void)vn;
    (void)off;
    (void)buf;
    (void)len;
    return 0;
}

static int64_t sink_write(struct vnode *vn, uint64_t off, const void *buf, size_t len)
{
    (void)vn;
    (void)off;
    (void)buf;
    return (int64_t)len;
}

static int64_t zero_read(struct vnode *vn, uint64_t off, void *buf, size_t len)
{
    (void)vn;
    (void)off;
    memset(buf, 0, len);
    return (int64_t)len;
}

static const struct chrdev_ops null_ops = { .read = null_read, .write = sink_write };
static const struct chrdev_ops zero_ops = { .read = zero_read, .write = sink_write };

void memdev_init(void)
{
    int rc = ramfs_mkchr("/dev/null", 0666, &null_ops, NULL, NULL);
    if (rc)
        kwarn("memdev: /dev/null: %d", rc);
    rc = ramfs_mkchr("/dev/zero", 0666, &zero_ops, NULL, NULL);
    if (rc)
        kwarn("memdev: /dev/zero: %d", rc);
}
