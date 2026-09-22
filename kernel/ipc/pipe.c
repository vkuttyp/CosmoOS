/*
 * pipe.c - The pipe's ring, and anonymous pipes over it
 * (docs/kernel/ipc/design.md).
 *
 * One spinlock per ring, never held while blocking or while touching user
 * memory (the system-call layer copies through a kernel buffer). Waits are
 * killable: a blocked reader or writer whose process is killed returns
 * -EINTR. The ring counts readers and writers and does not know who they
 * are: the anonymous pipe's two end objects count themselves here, a
 * named pipe (fifo.c) counts its opens.
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/pipe.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/wait.h>

#include <uapi/cosmo/syscall.h>

static struct pipe_stats g_stats;
static spinlock_t g_stats_lock = SPINLOCK_INIT("pipe-stats");

static void stat_add(uint64_t *f, int64_t d)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_stats_lock);
    *f = (uint64_t)((int64_t)*f + d);
    spin_unlock_irqrestore(&g_stats_lock, s);
}

/* --- the ring ------------------------------------------------------------- */

struct pipe *pipe_ring_alloc(void)
{
    struct pipe *p = kzalloc(sizeof(*p));
    if (p == NULL)
        return NULL;
    p->buf = kmalloc(PIPE_SIZE, 0);
    if (p->buf == NULL) {
        kfree(p);
        return NULL;
    }
    spinlock_init(&p->lock, "pipe");
    waitqueue_init(&p->rd_wq, "pipe-rd");
    waitqueue_init(&p->wr_wq, "pipe-wr");
    stat_add(&g_stats.created, 1);
    stat_add(&g_stats.alive, 1);
    return p;
}

void pipe_ring_free(struct pipe *p)
{
    kfree(p->buf);
    kfree(p);
    stat_add(&g_stats.alive, -1);
}

int64_t pipe_ring_read(struct pipe *p, void *buf, size_t len, bool nonblock)
{
    if (len == 0)
        return 0;
    if (!io_nonblocking(nonblock)) {
        int rc = wait_event_killable(&p->rd_wq, p->used > 0 || p->writers == 0);
        if (rc)
            return rc;
    }
    arch_irq_state_t s = spin_lock_irqsave(&p->lock);
    if (p->used == 0 && p->writers > 0) {
        spin_unlock_irqrestore(&p->lock, s);
        return -EAGAIN;   /* non-blocking and empty */
    }
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

int64_t pipe_ring_write(struct pipe *p, const void *buf, size_t len, bool nonblock)
{
    if (p->readers == 0)
        return -EPIPE;
    if (len == 0)
        return 0;
    nonblock = io_nonblocking(nonblock);
    size_t done = 0;
    while (done < len) {
        size_t left = len - done;
        unsigned need = left <= PIPE_BUF ? (unsigned)left : 1u;   /* small writes land whole */
        if (!nonblock) {
            int rc = wait_event_killable(&p->wr_wq, PIPE_SIZE - p->used >= need || p->readers == 0);
            if (rc)
                return done ? (int64_t)done : rc;
        }
        arch_irq_state_t s = spin_lock_irqsave(&p->lock);
        if (p->readers == 0) {
            spin_unlock_irqrestore(&p->lock, s);
            return done ? (int64_t)done : -EPIPE;
        }
        unsigned space = PIPE_SIZE - p->used;
        if (space < need) {
            spin_unlock_irqrestore(&p->lock, s);
            if (nonblock)
                return done ? (int64_t)done : -EAGAIN;
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

unsigned pipe_ring_ready_rd(struct pipe *p)
{
    unsigned r = 0;
    arch_irq_state_t s = spin_lock_irqsave(&p->lock);
    if (p->used > 0)
        r |= COSMO_IO_READABLE;
    if (p->writers == 0)
        r |= COSMO_IO_READABLE | COSMO_IO_HANGUP;   /* EOF reads at once */
    spin_unlock_irqrestore(&p->lock, s);
    return r;
}

unsigned pipe_ring_ready_wr(struct pipe *p)
{
    unsigned r = 0;
    arch_irq_state_t s = spin_lock_irqsave(&p->lock);
    if (PIPE_SIZE - p->used >= PIPE_BUF)
        r |= COSMO_IO_WRITABLE;
    if (p->readers == 0)
        r |= COSMO_IO_WRITABLE | COSMO_IO_ERROR;   /* -EPIPE at once */
    spin_unlock_irqrestore(&p->lock, s);
    return r;
}

/* --- the anonymous pipe: two end objects over one ring -------------------- */

struct pipe_pair;

struct pipe_end {
    struct kobject obj;
    struct pipe_pair *pair;
    bool nonblock;                 /* this end's mode, shared by every handle to it */
};

struct pipe_pair {
    struct pipe *ring;
    struct pipe_end rd, wr;        /* each one reader or one writer while it lives */
};

static struct pipe *ring_of(struct kobject *obj)
{
    return container_of(obj, struct pipe_end, obj)->pair->ring;
}

/* An end's release takes its count off the ring under the ring's lock;
 * whichever release sees both counts at zero frees the ring and the
 * pair (the other end is already gone, so nothing reaches either). */
static void end_release(struct kobject *obj, bool reader)
{
    struct pipe_end *e = container_of(obj, struct pipe_end, obj);
    struct pipe_pair *pair = e->pair;
    struct pipe *p = pair->ring;
    arch_irq_state_t s = spin_lock_irqsave(&p->lock);
    if (reader)
        p->readers--;
    else
        p->writers--;
    bool last = p->readers == 0 && p->writers == 0;
    spin_unlock_irqrestore(&p->lock, s);
    waitqueue_wake_all(reader ? &p->wr_wq : &p->rd_wq);   /* writers learn -EPIPE; readers EOF */
    if (last) {
        pipe_ring_free(p);
        kfree(pair);
    }
}

static void read_end_release(struct kobject *obj) { end_release(obj, true); }
static void write_end_release(struct kobject *obj) { end_release(obj, false); }

static int64_t pipe_read(struct kobject *obj, void *buf, size_t len)
{
    struct pipe_end *e = container_of(obj, struct pipe_end, obj);
    return pipe_ring_read(e->pair->ring, buf, len, __atomic_load_n(&e->nonblock, __ATOMIC_RELAXED));
}

static int64_t pipe_write(struct kobject *obj, const void *buf, size_t len)
{
    struct pipe_end *e = container_of(obj, struct pipe_end, obj);
    return pipe_ring_write(e->pair->ring, buf, len, __atomic_load_n(&e->nonblock, __ATOMIC_RELAXED));
}

static int pipe_stat(struct kobject *obj, struct cosmo_stat *st)
{
    struct pipe *p = ring_of(obj);
    memset(st, 0, sizeof(*st));
    st->type = COSMO_DT_FIFO;
    st->mode = 0600;
    st->nlink = 1;
    arch_irq_state_t s = spin_lock_irqsave(&p->lock);
    st->size = p->used;
    spin_unlock_irqrestore(&p->lock, s);
    return 0;
}

static unsigned pipe_read_ready(struct kobject *obj) { return pipe_ring_ready_rd(ring_of(obj)); }
static unsigned pipe_write_ready(struct kobject *obj) { return pipe_ring_ready_wr(ring_of(obj)); }

static struct waitqueue *pipe_read_poll_wq(struct kobject *obj, unsigned events)
{
    (void)events;
    return &ring_of(obj)->rd_wq;
}

static struct waitqueue *pipe_write_poll_wq(struct kobject *obj, unsigned events)
{
    (void)events;
    return &ring_of(obj)->wr_wq;
}

static int pipe_set_nonblock(struct kobject *obj, int on)
{
    struct pipe_end *e = container_of(obj, struct pipe_end, obj);
    int was = __atomic_load_n(&e->nonblock, __ATOMIC_RELAXED) ? 1 : 0;
    if (on >= 0)
        __atomic_store_n(&e->nonblock, on != 0, __ATOMIC_RELAXED);
    return was;
}

static const struct kobject_io_type pipe_read_type = {
    .base = { .name = "pipe-read", .release = read_end_release, .flags = KOBJECT_TYPE_IO },
    .read = pipe_read,
    .write = NULL,
    .stat = pipe_stat,
    .ready = pipe_read_ready,
    .set_nonblock = pipe_set_nonblock,
    .poll_wq = pipe_read_poll_wq,
};

static const struct kobject_io_type pipe_write_type = {
    .base = { .name = "pipe-write", .release = write_end_release, .flags = KOBJECT_TYPE_IO },
    .read = NULL,
    .write = pipe_write,
    .stat = pipe_stat,
    .ready = pipe_write_ready,
    .set_nonblock = pipe_set_nonblock,
    .poll_wq = pipe_write_poll_wq,
};

int pipe_create(struct kobject **read_end, struct kobject **write_end)
{
    struct pipe_pair *pair = kzalloc(sizeof(*pair));
    if (pair == NULL)
        return -ENOMEM;
    pair->ring = pipe_ring_alloc();
    if (pair->ring == NULL) {
        kfree(pair);
        return -ENOMEM;
    }
    kobject_init(&pair->rd.obj, &pipe_read_type.base);
    kobject_init(&pair->wr.obj, &pipe_write_type.base);
    pair->rd.pair = pair;
    pair->wr.pair = pair;
    pair->ring->readers = 1;
    pair->ring->writers = 1;
    *read_end = &pair->rd.obj;
    *write_end = &pair->wr.obj;
    return 0;
}

void pipe_get_stats(struct pipe_stats *out)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_stats_lock);
    *out = g_stats;
    spin_unlock_irqrestore(&g_stats_lock, s);
}
