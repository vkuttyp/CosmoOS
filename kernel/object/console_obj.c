/*
 * console_obj.c - The console kobject: writes go to the console sinks,
 * reads come from the console tty (docs/kernel/tty/). Installed as
 * handles 0, 1, 2 of kernel-created processes; children get it only by
 * inheritance through spawn.
 */

#include <kernel/console.h>
#include <kernel/object.h>
#include <kernel/panic.h>
#include <kernel/string.h>
#include <kernel/tty.h>

#include <uapi/cosmo/syscall.h>

static int64_t console_obj_read(struct kobject *obj, void *buf, size_t len)
{
    (void)obj;
    return tty_read(tty_console(), buf, len);
}

static int64_t console_obj_write(struct kobject *obj, const void *buf, size_t len)
{
    (void)obj;
    console_write((const char *)buf, len);
    return (int64_t)len;
}

static int console_obj_stat(struct kobject *obj, struct cosmo_stat *st)
{
    (void)obj;
    memset(st, 0, sizeof(*st));
    st->type = COSMO_DT_CHR;
    st->mode = 0620;
    st->nlink = 1;
    return 0;
}

static void console_obj_release(struct kobject *obj)
{
    panic("console object %p released; it must outlive the kernel", (void *)obj);
}

static unsigned console_obj_ready(struct kobject *obj)
{
    (void)obj;
    return COSMO_IO_WRITABLE | (tty_has_line(tty_console()) ? COSMO_IO_READABLE : 0);
}

static struct waitqueue *console_obj_poll_wq(struct kobject *obj, unsigned events)
{
    (void)obj;
    return (events & COSMO_IO_READABLE) ? &tty_console()->readers : NULL;   /* always writable */
}

static const struct kobject_io_type console_type = {
    .base = { .name = "console", .release = console_obj_release, .flags = KOBJECT_TYPE_IO },
    .read = console_obj_read,
    .write = console_obj_write,
    .stat = console_obj_stat,
    .ready = console_obj_ready,
    .poll_wq = console_obj_poll_wq,
};

static struct kobject g_console = { .type = &console_type.base, .refcount = 1 };

struct kobject *console_object(void)
{
    return &g_console;
}
