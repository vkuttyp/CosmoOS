/*
 * ttydev.c - `/dev/console` and `/dev/tty` (docs/kernel/tty/design.md,
 * "The terminal as a file").
 *
 * Until this file the console was reachable only as an inherited
 * handle: a process that closed handle 0 could never get it back, and
 * nothing could open the terminal by name. Two character nodes fix
 * that, on the mechanism `/dev/vmm` already uses.
 *
 * The two are not the same node, and the difference is the whole reason
 * `/dev/tty` exists:
 *
 *   /dev/console  the machine's console, to whoever opens it. It is a
 *                 particular device.
 *   /dev/tty      *the caller's controlling terminal*, which is a
 *                 different thing for different callers and nothing at
 *                 all for a process whose session has no terminal --
 *                 -ENXIO, as POSIX says. That question is only
 *                 answerable because the signals unit built sessions.
 */

#include <kernel/console.h>
#include <kernel/errno.h>
#include <kernel/log.h>
#include <kernel/process.h>
#include <kernel/tty.h>
#include <kernel/vfs.h>

static struct vnode *g_console_vnode;
static struct vnode *g_tty_vnode;

static int64_t console_dev_read(struct vnode *vn, struct file *f, uint64_t off, void *buf, size_t len)
{
    (void)vn;
    (void)off;
    return tty_read_nb(tty_console(), buf, len, file_nonblocking(f));
}

/*
 * Readiness (the device-readiness unit): what the console object answers,
 * asked through the file -- readable when a read would return, always
 * writable, the readers' queue the thing to wait on. The non-blocking
 * bit is the open file's, per open, which the console object never had
 * (it is shared by every process that inherited it).
 */
static unsigned tty_ready_of(struct tty *t)
{
    return COSMO_IO_WRITABLE | (tty_read_ready(t) ? COSMO_IO_READABLE : 0);
}

static unsigned console_dev_ready(struct vnode *vn, struct file *f)
{
    (void)vn;
    (void)f;
    return tty_ready_of(tty_console());
}

static struct waitqueue *console_dev_poll_wq(struct vnode *vn, struct file *f, unsigned events)
{
    (void)vn;
    (void)f;
    return (events & COSMO_IO_READABLE) ? &tty_console()->readers : NULL;   /* always writable */
}

static int dev_set_nonblock(struct vnode *vn, struct file *f, int on)
{
    (void)vn;
    return file_set_nonblock(f, on);
}

static int64_t console_dev_write(struct vnode *vn, uint64_t off, const void *buf, size_t len)
{
    (void)vn;
    (void)off;
    console_write(buf, len);
    return (int64_t)len;
}

/* The caller's controlling terminal, or NULL when its session has none. */
static struct tty *controlling_tty(void)
{
    struct tty *t = tty_console();
    pid_t sid = process_current_sid();
    return (sid != 0 && tty_session_of(t) == sid) ? t : NULL;
}

static int64_t tty_dev_read(struct vnode *vn, struct file *f, uint64_t off, void *buf, size_t len)
{
    (void)vn;
    (void)off;
    struct tty *t = controlling_tty();
    return t ? tty_read_nb(t, buf, len, file_nonblocking(f)) : -ENXIO;
}

/* No controlling terminal: a read would be -ENXIO, which to a poller is an
 * error condition and no queue to wait on. */
static unsigned tty_dev_ready(struct vnode *vn, struct file *f)
{
    (void)vn;
    (void)f;
    struct tty *t = controlling_tty();
    return t ? tty_ready_of(t) : COSMO_IO_ERROR;
}

static struct waitqueue *tty_dev_poll_wq(struct vnode *vn, struct file *f, unsigned events)
{
    (void)vn;
    (void)f;
    struct tty *t = controlling_tty();
    return (t != NULL && (events & COSMO_IO_READABLE)) ? &t->readers : NULL;
}

static int64_t tty_dev_write(struct vnode *vn, uint64_t off, const void *buf, size_t len)
{
    (void)vn;
    (void)off;
    if (controlling_tty() == NULL)
        return -ENXIO;
    console_write(buf, len);
    return (int64_t)len;
}

static const struct chrdev_ops console_dev_ops = {
    .read_file = console_dev_read, .write = console_dev_write,
    .ready = console_dev_ready, .poll_wq = console_dev_poll_wq, .set_nonblock = dev_set_nonblock,
};
static const struct chrdev_ops tty_dev_ops = {
    .read_file = tty_dev_read, .write = tty_dev_write,
    .ready = tty_dev_ready, .poll_wq = tty_dev_poll_wq, .set_nonblock = dev_set_nonblock,
};

/* The tty behind one of these nodes, for the terminal system calls;
 * NULL when the vnode is some other character device. `/dev/tty`
 * resolves per caller, so a process with no controlling terminal is
 * told the node is not a terminal for it. */
struct tty *tty_of_vnode(const struct vnode *vn)
{
    if (vn == g_console_vnode)
        return tty_console();
    if (vn == g_tty_vnode)
        return controlling_tty();
    return NULL;
}

/*
 * The tty behind whatever a handle holds. There are two ways to hold a
 * terminal -- the console kobject that init inherits as handles 0, 1
 * and 2, and an open file on `/dev/console` or `/dev/tty` -- and every
 * caller that asks a handle "are you a terminal" has to accept both.
 * One function so that adding a third way changes one place, and so
 * that the native and Linux system calls cannot answer differently.
 *
 * `file_from_kobject` converts without taking a reference: the caller's
 * reference on `obj` is what keeps the file alive.
 */
struct tty *tty_of_open(struct kobject *obj)
{
    struct tty *t = tty_of_object(obj);
    if (t != NULL)
        return t;
    struct file *f = file_from_kobject(obj);
    return f != NULL ? tty_of_vnode(f->vn) : NULL;
}

void tty_dev_init(void)
{
    int rc = ramfs_mkchr("/dev/console", 0600, &console_dev_ops, NULL, &g_console_vnode);
    if (rc)
        kwarn("tty: /dev/console: %d", rc);
    rc = ramfs_mkchr("/dev/tty", 0666, &tty_dev_ops, NULL, &g_tty_vnode);
    if (rc)
        kwarn("tty: /dev/tty: %d", rc);
}
