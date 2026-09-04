/*
 * console_obj.c - The console as a kernel object: standard I/O for
 * processes until the VFS provides real files.
 *
 * Writes go to the kernel console. Reads return 0 (end of file): there
 * is no serial receive path yet. The object is static and never
 * released; its release hook exists only to satisfy the type contract.
 */

#include <kernel/console.h>
#include <kernel/object.h>
#include <kernel/panic.h>

static int64_t console_obj_read(struct kobject *obj, void *buf, size_t len)
{
    (void)obj;
    (void)buf;
    (void)len;
    return 0;
}

static int64_t console_obj_write(struct kobject *obj, const void *buf, size_t len)
{
    (void)obj;
    console_write((const char *)buf, len);
    return (int64_t)len;
}

static void console_obj_release(struct kobject *obj)
{
    panic("console object %p released; it must outlive the kernel", (void *)obj);
}

static const struct kobject_io_type console_type = {
    .base = { .name = "console", .release = console_obj_release },
    .read = console_obj_read,
    .write = console_obj_write,
};

static struct kobject g_console = { .type = &console_type.base, .refcount = 1 };

struct kobject *console_object(void)
{
    return &g_console;
}
