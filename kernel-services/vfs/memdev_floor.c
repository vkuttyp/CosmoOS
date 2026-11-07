/*
 * memdev_floor.c - The C under memdev.cee (roadmap 2.0 SA7): the operation
 * tables of /dev/null and /dev/zero, C data that names the operations by
 * address, and the warning's format. The devices themselves are CeeSafe
 * (memdev.cee, generated into memdev.c).
 */

#include <kernel/log.h>
#include <kernel/vfs.h>

/* The operations, CeeSafe (memdev.cee). */
int64_t memdev_null_read(struct vnode *vn, uint64_t off, void *buf, size_t len);
int64_t memdev_sink_write(struct vnode *vn, uint64_t off, const void *buf, size_t len);
int64_t memdev_zero_read(struct vnode *vn, uint64_t off, void *buf, size_t len);

/* What memdev.cee calls here. */
const struct chrdev_ops *memdev_floor_ops(unsigned which);
void memdev_floor_warn(const char *path, int rc);

static const struct chrdev_ops null_ops = { .read = memdev_null_read, .write = memdev_sink_write };
static const struct chrdev_ops zero_ops = { .read = memdev_zero_read, .write = memdev_sink_write, .fills = true };

/* memdev.cee's OPS_NULL (0) and OPS_ZERO (1). */
const struct chrdev_ops *memdev_floor_ops(unsigned which)
{
    return which == 0 ? &null_ops : &zero_ops;
}

void memdev_floor_warn(const char *path, int rc)
{
    kwarn("memdev: %s: %d", path, rc);
}
