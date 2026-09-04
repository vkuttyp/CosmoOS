/*
 * pipe.c - Anonymous pipes (docs/kernel/ipc/design.md).
 *
 * One spinlock per pipe, never held while blocking or while touching user
 * memory (the system-call layer copies through a kernel buffer). Waits are
 * killable: a blocked reader or writer whose process is killed returns
 * -EINTR.
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/pipe.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/wait.h>

#include <uapi/cosmo/syscall.h>

struct pipe;

struct pipe_end {
    struct kobject obj;
    struct pipe *pipe;
};

struct pipe {
    spinlock_t lock;
    uint8_t *buf;
    unsigned head, tail, used;
    unsigned readers, writers;     /* live end objects */
    struct waitqueue rd_wq, wr_wq;
    struct pipe_end rd, wr;
};

static struct pipe_stats g_stats;
static spinlock_t g_stats_lock = SPINLOCK_INIT("pipe-stats");

static void stat_add(uint64_t *f, int64_t d)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_stats_lock);
    *f = (uint64_t)((int64_t)*f + d);
    spin_unlock_irqrestore(&g_stats_lock, s);
}

static void pipe_free(struct pipe *p)
{
    kfree(p->buf);
    kfree(p);
    stat_add(&g_stats.alive, -1);
}

/* --- the end objects ------------------------------------------------------ */

static void read_end_release(struct kobject *obj)
{
    struct pipe_end *e = container_of(obj, struct pipe_end, obj);
    struct pipe *p = e->pipe;
    arch_irq_state_t s = spin_lock_irqsave(&p->lock);
    p->readers--;
    bool last = p->readers == 0 && p->writers == 0;
    spin_unlock_irqrestore(&p->lock, s);
    waitqueue_wake_all(&p->wr_wq);   /* writers learn -EPIPE */
    if (last)
        pipe_free(p);
}

static void write_end_release(struct kobject *obj)
{
    struct pipe_end *e = container_of(obj, struct pipe_end, obj);
    struct pipe *p = e->pipe;
    arch_irq_state_t s = spin_lock_irqsave(&p->lock);
    p->writers--;
    bool last = p->readers == 0 && p->writers == 0;
    spin_unlock_irqrestore(&p->lock, s);
    waitqueue_wake_all(&p->rd_wq);   /* readers learn EOF */
    if (last)
        pipe_free(p);
}

static int64_t pipe_read(struct kobject *obj, void *buf, size_t len)
{
    struct pipe *p = container_of(obj, struct pipe_end, obj)->pipe;
    if (len == 0)
        return 0;
    int rc = wait_event_killable(&p->rd_wq, p->used > 0 || p->writers == 0);
    if (rc)
        return rc;
    arch_irq_state_t s = spin_lock_irqsave(&p->lock);
    unsigned n = p->used < len ? p->used : (unsigned)len;
    unsigned first = PIPE_SIZE - p->head;
    if (first > n)
        first = n;
    memcpy(buf, p->buf + p->head, first);
    memcpy((uint8_t *)buf + first, p->buf, n - first);
    p->head = (p->head + n) % PIPE_SIZE;
    p->used -= n;
    spin_unlock_irqrestore(&p->lock, s);
    if (n > 0) {
        waitqueue_wake_all(&p->wr_wq);
        stat_add(&g_stats.bytes, (int64_t)n);
    }
    return (int64_t)n;   /* 0 only when drained and no writer remains */
}

static int64_t pipe_write(struct kobject *obj, const void *buf, size_t len)
{
    struct pipe *p = container_of(obj, struct pipe_end, obj)->pipe;
    if (p->readers == 0)
        return -EPIPE;
    if (len == 0)
        return 0;
    size_t done = 0;
    while (done < len) {
        size_t left = len - done;
        unsigned need = left <= PIPE_BUF ? (unsigned)left : 1u;   /* small writes land whole */
        int rc = wait_event_killable(&p->wr_wq, PIPE_SIZE - p->used >= need || p->readers == 0);
        if (rc)
            return done ? (int64_t)done : rc;
        arch_irq_state_t s = spin_lock_irqsave(&p->lock);
        if (p->readers == 0) {
            spin_unlock_irqrestore(&p->lock, s);
            return done ? (int64_t)done : -EPIPE;
        }
        unsigned space = PIPE_SIZE - p->used;
        if (space < need) {
            spin_unlock_irqrestore(&p->lock, s);
            continue;   /* another writer got there first */
        }
        unsigned n = left < space ? (unsigned)left : space;
        unsigned first = PIPE_SIZE - p->tail;
        if (first > n)
            first = n;
        memcpy(p->buf + p->tail, (const uint8_t *)buf + done, first);
        memcpy(p->buf, (const uint8_t *)buf + done + first, n - first);
        p->tail = (p->tail + n) % PIPE_SIZE;
        p->used += n;
        spin_unlock_irqrestore(&p->lock, s);
        waitqueue_wake_all(&p->rd_wq);
        done += n;
    }
    return (int64_t)done;
}

static int pipe_stat(struct kobject *obj, struct cosmo_stat *st)
{
    struct pipe *p = container_of(obj, struct pipe_end, obj)->pipe;
    memset(st, 0, sizeof(*st));
    st->type = COSMO_DT_FIFO;
    st->mode = 0600;
    st->nlink = 1;
    arch_irq_state_t s = spin_lock_irqsave(&p->lock);
    st->size = p->used;
    spin_unlock_irqrestore(&p->lock, s);
    return 0;
}

static const struct kobject_io_type pipe_read_type = {
    .base = { .name = "pipe-read", .release = read_end_release },
    .read = pipe_read,
    .write = NULL,
    .stat = pipe_stat,
};

static const struct kobject_io_type pipe_write_type = {
    .base = { .name = "pipe-write", .release = write_end_release },
    .read = NULL,
    .write = pipe_write,
    .stat = pipe_stat,
};

int pipe_create(struct kobject **read_end, struct kobject **write_end)
{
    struct pipe *p = kzalloc(sizeof(*p));
    if (p == NULL)
        return -ENOMEM;
    p->buf = kmalloc(PIPE_SIZE, 0);
    if (p->buf == NULL) {
        kfree(p);
        return -ENOMEM;
    }
    spinlock_init(&p->lock, "pipe");
    waitqueue_init(&p->rd_wq, "pipe-rd");
    waitqueue_init(&p->wr_wq, "pipe-wr");
    kobject_init(&p->rd.obj, &pipe_read_type.base);
    kobject_init(&p->wr.obj, &pipe_write_type.base);
    p->rd.pipe = p;
    p->wr.pipe = p;
    p->readers = 1;
    p->writers = 1;
    stat_add(&g_stats.created, 1);
    stat_add(&g_stats.alive, 1);
    *read_end = &p->rd.obj;
    *write_end = &p->wr.obj;
    return 0;
}

void pipe_get_stats(struct pipe_stats *out)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_stats_lock);
    *out = g_stats;
    spin_unlock_irqrestore(&g_stats_lock, s);
}
