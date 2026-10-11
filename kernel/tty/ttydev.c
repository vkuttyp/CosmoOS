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
#include <kernel/printf.h>
#include <kernel/process.h>
#include <kernel/pty.h>
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

static int64_t console_dev_write(struct vnode *vn, struct file *f, uint64_t off, const void *buf, size_t len)
{
    (void)vn;
    (void)off;
    return tty_write(tty_console(), buf, len, file_nonblocking(f));   /* TOSTOP's SIGTTOU, then the sinks */
}

/* The caller's controlling terminal, referenced, or NULL when its session
 * has none: the console or a pty slave (tty_controlling). */
static struct tty *controlling_tty(void)
{
    return tty_controlling(process_current_sid());
}

static int64_t tty_dev_read(struct vnode *vn, struct file *f, uint64_t off, void *buf, size_t len)
{
    (void)vn;
    (void)off;
    struct tty *t = controlling_tty();
    if (t == NULL)
        return -ENXIO;
    int64_t rc = tty_read_nb(t, buf, len, file_nonblocking(f));
    tty_put(t);
    return rc;
}

/* No controlling terminal: a read would be -ENXIO, which to a poller is an
 * error condition and no queue to wait on. */
static unsigned tty_dev_ready(struct vnode *vn, struct file *f)
{
    (void)vn;
    (void)f;
    struct tty *t = controlling_tty();
    if (t == NULL)
        return COSMO_IO_ERROR;
    unsigned r = tty_ready_of(t);
    tty_put(t);
    return r;
}

static struct waitqueue *tty_dev_poll_wq(struct vnode *vn, struct file *f, unsigned events)
{
    (void)vn;
    struct tty *t = controlling_tty();
    if (t == NULL)
        return NULL;
    /* A waiter stays on the queue after this returns, so the queue must
     * outlive it: the console's does, and a pty slave's does while this
     * open file pins it (tty_dev_open). A terminal the session took after
     * the open is not pinned here; its readiness is still answered, but
     * there is no queue to sleep on. */
    struct waitqueue *wq = NULL;
    if ((events & COSMO_IO_READABLE) && (t == tty_console() || t == f->priv))
        wq = &t->readers;
    tty_put(t);
    return wq;
}

/* Pin the terminal the opener's session controls now, if any, for the
 * life of the open (the poll queue above); the open succeeds either way
 * -- a process without a terminal is told so by each call. */
static int tty_dev_open(struct vnode *vn, struct file *f)
{
    (void)vn;
    f->priv = controlling_tty();
    return 0;
}

static void tty_dev_release(struct vnode *vn, struct file *f)
{
    (void)vn;
    if (f->priv != NULL)
        tty_put(f->priv);
}

static int64_t tty_dev_write(struct vnode *vn, struct file *f, uint64_t off, const void *buf, size_t len)
{
    (void)vn;
    (void)off;
    struct tty *t = controlling_tty();
    if (t == NULL)
        return -ENXIO;
    int64_t rc = tty_write(t, buf, len, file_nonblocking(f));
    tty_put(t);
    return rc;
}

static const struct chrdev_ops console_dev_ops = {
    .read_file = console_dev_read, .write_file = console_dev_write,
    .ready = console_dev_ready, .poll_wq = console_dev_poll_wq, .set_nonblock = dev_set_nonblock,
};
static const struct chrdev_ops tty_dev_ops = {
    .open = tty_dev_open, .release = tty_dev_release, .read_file = tty_dev_read, .write_file = tty_dev_write,
    .ready = tty_dev_ready, .poll_wq = tty_dev_poll_wq, .set_nonblock = dev_set_nonblock,
};

/* The tty behind one of these nodes, for the terminal system calls,
 * referenced; NULL when the vnode is some other character device.
 * `/dev/tty` resolves per caller, so a process with no controlling
 * terminal is told the node is not a terminal for it. */
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
        return t;   /* the console: static, its reference a no-op */
    struct file *f = file_from_kobject(obj);
    if (f == NULL)
        return NULL;
    t = pty_tty_of_file(f);   /* a pty's master or slave */
    return t != NULL ? t : tty_of_vnode(f->vn);
}

bool tty_name_of_open(struct kobject *obj, char *buf, size_t n)
{
    if (tty_of_object(obj) != NULL) {
        ksnprintf(buf, n, "/dev/console");
        return true;
    }
    struct file *f = file_from_kobject(obj);
    if (f == NULL)
        return false;
    if (pty_name_of_file(f, buf, n))
        return true;
    if (f->vn == g_console_vnode || f->vn == g_tty_vnode) {
        ksnprintf(buf, n, f->vn == g_console_vnode ? "/dev/console" : "/dev/tty");
        return true;
    }
    return false;
}

void tty_dev_init(void)
{
    int rc = ramfs_mkchr("/dev/console", 0600, &console_dev_ops, NULL, &g_console_vnode);
    if (rc)
        kwarn("tty: /dev/console: %d", rc);
    rc = ramfs_mkchr("/dev/tty", 0666, &tty_dev_ops, NULL, &g_tty_vnode);
    if (rc)
        kwarn("tty: /dev/tty: %d", rc);
    pty_init();   /* /dev/ptmx and the pts filesystem (roadmap M4) */
}
