/*
 * pty.c - Pseudo-terminals (docs/kernel/tty/design.md, "Pseudo-terminals";
 * roadmap M4).
 *
 * Opening /dev/ptmx makes a pair: the open file is the master, and the
 * slave appears as /dev/pts/N on the pts filesystem (a ramfs instance
 * mounted at /dev/pts that holds nothing else). The slave is a `struct
 * tty` like the console's -- the same line discipline, modes, sessions and
 * job control -- whose input is what the master writes (tty_input) and
 * whose output, echo included, goes into the master's queue, which the
 * master reads. Closing the master hangs the slave up (tty_hangup: SIGHUP
 * to its session, reads end of file, writes -EIO) and removes its name;
 * closing every slave open makes the master's read -EIO, as on Linux, so
 * a server reading the master learns its session's terminal is gone.
 *
 * Lock order: tty.lock -> pty.olock (echo runs under the line discipline's
 * lock and queues into the master's); nothing takes them the other way.
 * A pair lives until its master and every slave open are closed and no
 * caller holds its terminal (tty_get): one kobject reference each.
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/object.h>
#include <kernel/printf.h>
#include <kernel/process.h>
#include <kernel/pty.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/tty.h>
#include <kernel/vfs.h>
#include <kernel/wait.h>

struct pty {
    struct kobject obj;
    unsigned index;
    char name[16];                 /* "pts/N": the tty's name */
    struct tty tty;                /* the slave's terminal */
    spinlock_t olock;              /* the output queue and the open counts */
    uint8_t out[PTY_OUT_MAX];
    unsigned ohead, otail, oused;
    struct waitqueue mreaders;     /* the master waits for output */
    struct waitqueue owriters;     /* slave writers wait for room */
    bool master_open;
    bool slave_ever;               /* a slave was opened once: its last close is the master's EIO */
    bool locked;                   /* TIOCSPTLCK: the slave may not be opened yet */
    unsigned slave_opens;
    struct vnode *node;            /* /dev/pts/N, until the master closes */
};

/* The index table, and every slave node's pointer to its pair: a slave's
 * open takes its reference under this lock, and the master's close clears
 * the node's pointer under it, so an open after (or racing) the close sees
 * NULL rather than a pair being freed. Order: g_pty_lock -> pty.olock. */
static spinlock_t g_pty_lock = SPINLOCK_INIT("pty-index");
static bool g_pty_used[PTY_MAX];
static struct vnode *g_ptmx_vnode;
static struct vnode *g_pts_dir;   /* the pts filesystem's root: slaves are made and removed here, by name */
static struct pty_stats g_stats;

static void pty_release_obj(struct kobject *obj)
{
    struct pty *p = container_of(obj, struct pty, obj);
    arch_irq_state_t s = spin_lock_irqsave(&g_pty_lock);
    g_pty_used[p->index] = false;
    spin_unlock_irqrestore(&g_pty_lock, s);
    __atomic_fetch_add(&g_stats.freed, 1, __ATOMIC_RELAXED);
    kfree(p);
}

static const struct kobject_type pty_type = { .name = "pty", .release = pty_release_obj };

/* --- the master's queue ---------------------------------------------------- */

/* Olock held. Queue one byte, or false when there is no room. */
static bool out_put1(struct pty *p, uint8_t c)
{
    if (p->oused >= PTY_OUT_MAX)
        return false;
    p->out[p->otail] = c;
    p->otail = (p->otail + 1) % PTY_OUT_MAX;
    p->oused++;
    return true;
}

/* Olock held. Queue `c` as the output processing says (OPOST: a newline
 * goes out as CR LF), whole or not at all; false when there is no room. */
static bool out_put(struct pty *p, uint8_t c, bool opost)
{
    if (c == '\n' && opost) {
        if (PTY_OUT_MAX - p->oused < 2)
            return false;
        out_put1(p, '\r');
    }
    return out_put1(p, c);
}

/* The line discipline's echo: tty.lock held, never blocks; what does not
 * fit is dropped, as the console drops nothing only because its sink is a
 * wire. */
static void pty_echo(struct tty *t, const char *s, size_t n)
{
    struct pty *p = t->priv;
    bool opost = (t->flags & TTY_OPOST) != 0;
    arch_irq_state_t st = spin_lock_irqsave(&p->olock);
    for (size_t i = 0; i < n; i++)
        if (!out_put(p, (uint8_t)s[i], opost))
            break;
    spin_unlock_irqrestore(&p->olock, st);
    waitqueue_wake_all(&p->mreaders);
}

/* A program's write to the slave: as much as fits, then wait for the
 * master to read (or return what went, non-blocking). */
static int64_t pty_write(struct tty *t, const void *buf, size_t len, bool nonblock)
{
    struct pty *p = t->priv;
    const uint8_t *b = buf;
    size_t done = 0;
    for (;;) {
        arch_irq_state_t ts = spin_lock_irqsave(&t->lock);
        bool opost = (t->flags & TTY_OPOST) != 0;
        spin_unlock_irqrestore(&t->lock, ts);
        arch_irq_state_t s = spin_lock_irqsave(&p->olock);
        bool gone = !p->master_open;
        while (!gone && done < len && out_put(p, b[done], opost))
            done++;
        spin_unlock_irqrestore(&p->olock, s);
        if (done > 0)
            waitqueue_wake_all(&p->mreaders);
        if (done == len)
            return (int64_t)len;
        if (gone)
            return done > 0 ? (int64_t)done : -EIO;
        if (io_nonblocking(nonblock))
            return done > 0 ? (int64_t)done : -EAGAIN;
        /* Room for the next byte as it will go out: two slots for a
         * newline under OPOST, one for anything else. */
        unsigned need = (b[done] == '\n' && opost) ? 2u : 1u;
        int rc = wait_event_killable(&p->owriters, p->oused + need <= PTY_OUT_MAX || !p->master_open);
        if (rc)
            return done > 0 ? (int64_t)done : rc;
    }
}

static void pty_tty_get(struct tty *t) { kobject_get(&((struct pty *)t->priv)->obj); }
static void pty_tty_put(struct tty *t) { kobject_put(&((struct pty *)t->priv)->obj); }

/* The slave's output side, for TIOCOUTQ, TCFLSH and TCSETSW. */
static unsigned pty_out_queued(struct tty *t)
{
    struct pty *p = t->priv;
    arch_irq_state_t s = spin_lock_irqsave(&p->olock);
    unsigned n = p->oused;
    spin_unlock_irqrestore(&p->olock, s);
    return n;
}

static void pty_flush_out(struct tty *t)
{
    struct pty *p = t->priv;
    arch_irq_state_t s = spin_lock_irqsave(&p->olock);
    p->ohead = p->otail = p->oused = 0;
    spin_unlock_irqrestore(&p->olock, s);
    waitqueue_wake_all(&p->owriters);
    waitqueue_wake_all(&p->tty.readers);   /* a combined poll sleeps there (pts_poll_wq) */
}

/* Until the master has read everything the slave wrote, or is gone. */
static int pty_drain(struct tty *t)
{
    struct pty *p = t->priv;
    return wait_event_killable(&p->owriters, p->oused == 0 || !p->master_open);
}

static const struct tty_ops pty_tty_ops = {
    .echo = pty_echo, .write = pty_write, .get = pty_tty_get, .put = pty_tty_put,
    .out_queued = pty_out_queued, .flush_out = pty_flush_out, .drain = pty_drain,
};

/* --- the slave: /dev/pts/N --------------------------------------------------- */

static int pts_open(struct vnode *vn, struct file *f)
{
    /* The reference is taken under g_pty_lock, which the master's close
     * holds to clear the node's pointer: the pair cannot be freed between
     * finding it and holding it. */
    arch_irq_state_t gs = spin_lock_irqsave(&g_pty_lock);
    struct pty *p = ramfs_chr_priv(vn);
    int rc = 0;
    if (p == NULL) {
        rc = -EIO;   /* the master has closed */
    } else {
        arch_irq_state_t s = spin_lock_irqsave(&p->olock);
        if (!p->master_open || p->locked) {
            rc = -EIO;   /* Linux's answer for a pair whose master is gone or still locked */
        } else {
            p->slave_opens++;
            p->slave_ever = true;
            kobject_get(&p->obj);
        }
        spin_unlock_irqrestore(&p->olock, s);
    }
    spin_unlock_irqrestore(&g_pty_lock, gs);
    if (rc)
        return rc;
    f->priv = p;
    /* A session leader whose session has no terminal takes the one it
     * opens, unless it says O_NOCTTY: what getty, and every program that
     * opens a pty slave after setsid, relies on. */
    if (!(file_flags(f) & COSMO_O_NOCTTY))
        (void)tty_set_controlling(&p->tty);
    return 0;
}

static void pts_release(struct vnode *vn, struct file *f)
{
    (void)vn;
    struct pty *p = f->priv;
    arch_irq_state_t s = spin_lock_irqsave(&p->olock);
    p->slave_opens--;
    spin_unlock_irqrestore(&p->olock, s);
    waitqueue_wake_all(&p->mreaders);   /* the last close: the master's read is -EIO now */
    kobject_put(&p->obj);
}

static int64_t pts_read(struct vnode *vn, struct file *f, uint64_t off, void *buf, size_t len)
{
    (void)vn;
    (void)off;
    struct pty *p = f->priv;
    return tty_read_nb(&p->tty, buf, len, file_nonblocking(f));
}

static int64_t pts_write(struct vnode *vn, struct file *f, uint64_t off, const void *buf, size_t len)
{
    (void)vn;
    (void)off;
    struct pty *p = f->priv;
    return tty_write(&p->tty, buf, len, file_nonblocking(f));
}

static unsigned pts_ready(struct vnode *vn, struct file *f)
{
    (void)vn;
    struct pty *p = f->priv;
    unsigned r = tty_read_ready(&p->tty) ? COSMO_IO_READABLE : 0;
    arch_irq_state_t s = spin_lock_irqsave(&p->olock);
    if (p->oused + 2 <= PTY_OUT_MAX || !p->master_open)
        r |= COSMO_IO_WRITABLE;   /* room, or a write that fails at once (-EIO) */
    spin_unlock_irqrestore(&p->olock, s);
    return r;
}

static struct waitqueue *pts_poll_wq(struct vnode *vn, struct file *f, unsigned events)
{
    (void)vn;
    struct pty *p = f->priv;
    /* One queue per poll: readers', which the master's read also wakes when
     * it frees room, so a poll for both directions hears either. */
    if (events & COSMO_IO_READABLE)
        return &p->tty.readers;
    if (events & COSMO_IO_WRITABLE)
        return &p->owriters;
    return NULL;
}

static int pty_set_nonblock(struct vnode *vn, struct file *f, int on)
{
    (void)vn;
    return file_set_nonblock(f, on);
}

static const struct chrdev_ops pts_ops = {
    .open = pts_open, .release = pts_release, .read_file = pts_read, .write_file = pts_write,
    .ready = pts_ready, .poll_wq = pts_poll_wq, .set_nonblock = pty_set_nonblock,
};

/* --- the master: an open of /dev/ptmx ---------------------------------------- */

static int ptmx_open(struct vnode *vn, struct file *f)
{
    (void)vn;
    unsigned index = PTY_MAX;
    arch_irq_state_t s = spin_lock_irqsave(&g_pty_lock);
    for (unsigned i = 0; i < PTY_MAX; i++) {
        if (!g_pty_used[i]) {
            g_pty_used[i] = true;
            index = i;
            break;
        }
    }
    spin_unlock_irqrestore(&g_pty_lock, s);
    if (index == PTY_MAX)
        return -ENOSPC;
    struct pty *p = kzalloc(sizeof(*p));
    if (p == NULL) {
        s = spin_lock_irqsave(&g_pty_lock);
        g_pty_used[index] = false;
        spin_unlock_irqrestore(&g_pty_lock, s);
        return -ENOMEM;
    }
    kobject_init(&p->obj, &pty_type);
    p->index = index;
    ksnprintf(p->name, sizeof(p->name), "pts/%u", index);
    tty_setup(&p->tty, p->name);
    p->tty.ops = &pty_tty_ops;
    p->tty.priv = p;
    spinlock_init(&p->olock, "pty-out");
    waitqueue_init(&p->mreaders, "pty-master");
    waitqueue_init(&p->owriters, "pty-writers");
    p->master_open = true;
    p->locked = true;   /* until unlockpt (TIOCSPTLCK 0), as on Linux */
    char name[12];
    ksnprintf(name, sizeof(name), "%u", index);
    int rc = g_pts_dir ? ramfs_mkchr_at(g_pts_dir, name, 0620, &pts_ops, p, &p->node) : -ENODEV;
    if (rc) {
        kwarn("pty: cannot create /dev/pts/%s (%d)", name, rc);
        kobject_put(&p->obj);
        return rc;
    }
    tty_register(&p->tty);
    f->priv = p;   /* the master's reference: the creator's */
    __atomic_fetch_add(&g_stats.opened, 1, __ATOMIC_RELAXED);
    return 0;
}

static void ptmx_release(struct vnode *vn, struct file *f)
{
    (void)vn;
    struct pty *p = f->priv;
    arch_irq_state_t gs = spin_lock_irqsave(&g_pty_lock);
    arch_irq_state_t s = spin_lock_irqsave(&p->olock);
    p->master_open = false;
    spin_unlock_irqrestore(&p->olock, s);
    ramfs_chr_set_priv(p->node, NULL);   /* no open from here on reaches this pair */
    spin_unlock_irqrestore(&g_pty_lock, gs);
    /* The slave's session hears the line drop; its readers see end of
     * file, its writers -EIO, and the name goes -- removed by the kernel in
     * the pts directory it holds, whoever closed the master and wherever it
     * is confined, so the number is free again. */
    tty_hangup(&p->tty);
    tty_unregister(&p->tty);
    waitqueue_wake_all(&p->owriters);
    char name[12];
    ksnprintf(name, sizeof(name), "%u", p->index);
    int rc = ramfs_unlink_chr_at(g_pts_dir, name, p->node);
    if (rc)
        kwarn("pty: cannot remove /dev/pts/%s (%d)", name, rc);
    vnode_put(p->node);
    p->node = NULL;
    kobject_put(&p->obj);
}

static int64_t ptmx_read(struct vnode *vn, struct file *f, uint64_t off, void *buf, size_t len)
{
    (void)vn;
    (void)off;
    struct pty *p = f->priv;
    uint8_t *out = buf;
    if (len == 0)
        return 0;
    for (;;) {
        arch_irq_state_t s = spin_lock_irqsave(&p->olock);
        size_t n = 0;
        while (n < len && p->oused > 0) {
            out[n++] = p->out[p->ohead];
            p->ohead = (p->ohead + 1) % PTY_OUT_MAX;
            p->oused--;
        }
        bool closed = p->slave_ever && p->slave_opens == 0;
        spin_unlock_irqrestore(&p->olock, s);
        if (n > 0) {
            waitqueue_wake_all(&p->owriters);
            waitqueue_wake_all(&p->tty.readers);   /* a combined poll of the slave sleeps there */
            return (int64_t)n;
        }
        if (closed)
            return -EIO;   /* every slave open has closed: the session's terminal is gone */
        if (io_nonblocking(file_nonblocking(f)))
            return -EAGAIN;
        int rc = wait_event_killable(&p->mreaders, p->oused > 0 || (p->slave_ever && p->slave_opens == 0));
        if (rc)
            return rc;
    }
}

/* What the master writes is typed at the slave: the line discipline edits,
 * echoes (into this master's queue) and signals as for a keyboard. */
static int64_t ptmx_write(struct vnode *vn, struct file *f, uint64_t off, const void *buf, size_t len)
{
    (void)vn;
    (void)off;
    struct pty *p = f->priv;
    tty_input(&p->tty, buf, len);
    return (int64_t)len;
}

static unsigned ptmx_ready(struct vnode *vn, struct file *f)
{
    (void)vn;
    struct pty *p = f->priv;
    arch_irq_state_t s = spin_lock_irqsave(&p->olock);
    unsigned r = COSMO_IO_WRITABLE;
    if (p->oused > 0 || (p->slave_ever && p->slave_opens == 0))
        r |= COSMO_IO_READABLE;   /* output, or a read that answers -EIO at once */
    spin_unlock_irqrestore(&p->olock, s);
    return r;
}

static struct waitqueue *ptmx_poll_wq(struct vnode *vn, struct file *f, unsigned events)
{
    (void)vn;
    struct pty *p = f->priv;
    return (events & COSMO_IO_READABLE) ? &p->mreaders : NULL;   /* always writable */
}

static const struct chrdev_ops ptmx_ops = {
    .open = ptmx_open, .release = ptmx_release, .read_file = ptmx_read, .write_file = ptmx_write,
    .ready = ptmx_ready, .poll_wq = ptmx_poll_wq, .set_nonblock = pty_set_nonblock,
};

/* --- lookups and the master's ioctls --------------------------------------------- */

static struct pty *pty_of_file(struct file *f, bool *master)
{
    if (f->vn == g_ptmx_vnode && f->dev_open) {
        *master = true;
        return f->priv;
    }
    if (f->vn != NULL && f->dev_open && ramfs_chr_ops_of(f->vn) == &pts_ops) {
        *master = false;
        return f->priv;
    }
    return NULL;
}

struct tty *pty_tty_of_file(struct file *f)
{
    bool master;
    struct pty *p = pty_of_file(f, &master);
    if (p == NULL)
        return NULL;
    kobject_get(&p->obj);
    return &p->tty;
}

int pty_get_index(struct file *f, unsigned *out)
{
    bool master;
    struct pty *p = pty_of_file(f, &master);
    if (p == NULL || !master)
        return -ENOTTY;
    *out = p->index;
    return 0;
}

int pty_set_lock(struct file *f, bool locked)
{
    bool master;
    struct pty *p = pty_of_file(f, &master);
    if (p == NULL || !master)
        return -ENOTTY;
    arch_irq_state_t s = spin_lock_irqsave(&p->olock);
    p->locked = locked;
    spin_unlock_irqrestore(&p->olock, s);
    return 0;
}

int pty_get_lock(struct file *f, bool *out)
{
    bool master;
    struct pty *p = pty_of_file(f, &master);
    if (p == NULL || !master)
        return -ENOTTY;
    arch_irq_state_t s = spin_lock_irqsave(&p->olock);
    *out = p->locked;
    spin_unlock_irqrestore(&p->olock, s);
    return 0;
}

bool pty_name_of_file(struct file *f, char *buf, size_t n)
{
    bool master;
    struct pty *p = pty_of_file(f, &master);
    if (p == NULL)
        return false;
    if (master)
        ksnprintf(buf, n, "/dev/ptmx");
    else
        ksnprintf(buf, n, "/dev/pts/%u", p->index);
    return true;
}

unsigned pty_master_queued(struct file *f)
{
    bool master;
    struct pty *p = pty_of_file(f, &master);
    if (p == NULL || !master)
        return 0;
    arch_irq_state_t s = spin_lock_irqsave(&p->olock);
    unsigned n = p->oused;
    spin_unlock_irqrestore(&p->olock, s);
    return n;
}

void pty_get_stats(struct pty_stats *out)
{
    out->opened = __atomic_load_n(&g_stats.opened, __ATOMIC_RELAXED);
    out->freed = __atomic_load_n(&g_stats.freed, __ATOMIC_RELAXED);
}

void pty_init(void)
{
    int rc = vfs_mkdir(NULL, "/dev/pts", 0755);
    if (rc && rc != -EEXIST)
        kwarn("pty: /dev/pts: %d", rc);
    rc = vfs_mount("/dev/pts", "ramfs", NULL, 0);   /* the pts filesystem: holds the slaves and nothing else */
    if (rc)
        kwarn("pty: cannot mount /dev/pts (%d)", rc);
    rc = vfs_lookup(NULL, "/dev/pts", &g_pts_dir);   /* held for good: the root of the pts mount */
    if (rc) {
        kwarn("pty: /dev/pts: %d", rc);
        g_pts_dir = NULL;
    }
    rc = ramfs_mkchr("/dev/ptmx", 0666, &ptmx_ops, NULL, &g_ptmx_vnode);
    if (rc)
        kwarn("pty: /dev/ptmx: %d", rc);
}
