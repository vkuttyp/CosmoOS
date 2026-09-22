/*
 * fifo.c - Named pipes: the pipe's ring behind a filesystem node
 * (docs/kernel/ipc/design.md, "Named pipes"; invariant I9).
 *
 * The ring is made by the first open and freed by the last release, and
 * its reader and writer counts are the live opens of each side. Both
 * facts change together, so they change under one lock order: the
 * fifo's lock (the ring pointer) outside the ring's (the counts). Open
 * waits for its counterpart on the fifo's own queue with no lock held;
 * the wait is killable, and an open that fails -- killed, refused, out
 * of memory -- takes back what it did before it returns, because the
 * VFS runs the release hook only for an open that succeeded.
 */

#include <kernel/errno.h>
#include <kernel/fifo.h>
#include <kernel/kmalloc.h>
#include <kernel/panic.h>
#include <kernel/pipe.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/vfs.h>
#include <kernel/wait.h>

#include <uapi/cosmo/syscall.h>

#define FIFO_RD 1u
#define FIFO_WR 2u

struct fifo {
    spinlock_t lock;             /* the ring pointer and the generations; taken outside the ring's lock */
    struct pipe *ring;           /* NULL while nobody has the FIFO open */
    struct waitqueue openers;    /* an open waiting for the other side */
    /*
     * Opens ever, per side. A blocking open waits for the OTHER side's
     * generation to move past what it was when this open joined, not
     * for the other side's count to be nonzero: a writer that opens,
     * writes and closes before the woken reader gets to run has still
     * "had the FIFO open", and the reader returns to read those bytes
     * and end of file. Waiting on the count instead lost exactly that
     * writer and left the reader asleep for good (found by the fifo
     * section's writer child on the first CI-shaped run). Linux keeps
     * the same two counters for the same reason.
     */
    unsigned r_gen, w_gen;
};

struct fifo_open {
    struct pipe *ring;           /* held alive by this open's count */
    unsigned side;               /* FIFO_RD, FIFO_WR or both */
    bool nonblock;               /* this open's mode (POSIX: per open) */
};

static unsigned g_rings;         /* live rings held by fifos; atomic, since each fifo has its own lock */

struct fifo *fifo_alloc(void)
{
    struct fifo *fifo = kzalloc(sizeof(*fifo));
    if (fifo == NULL)
        return NULL;
    spinlock_init(&fifo->lock, "fifo");
    waitqueue_init(&fifo->openers, "fifo-open");
    return fifo;
}

void fifo_free(struct fifo *fifo)
{
    if (fifo->ring != NULL)
        panic("fifo: freed with its ring live (an open outlived the node)");
    kfree(fifo);
}

/*
 * Take `side`'s counts off the ring, under both locks. Returns the ring
 * to free when this was the last open, or NULL. The caller wakes the
 * queues afterwards, with the locks dropped and before it frees
 * anything.
 */
static struct pipe *drop_locked(struct fifo *fifo, struct pipe *ring, unsigned side)
{
    spin_lock(&ring->lock);
    if (side & FIFO_RD)
        ring->readers--;
    if (side & FIFO_WR)
        ring->writers--;
    bool last = ring->readers == 0 && ring->writers == 0;
    spin_unlock(&ring->lock);
    if (last) {
        fifo->ring = NULL;
        __atomic_sub_fetch(&g_rings, 1, __ATOMIC_RELAXED);
    }
    return last ? ring : NULL;
}

static void drop(struct fifo *fifo, struct pipe *ring, unsigned side)
{
    arch_irq_state_t s = spin_lock_irqsave(&fifo->lock);
    struct pipe *gone = drop_locked(fifo, ring, side);
    spin_unlock_irqrestore(&fifo->lock, s);
    if (side & FIFO_RD)
        waitqueue_wake_all(&ring->wr_wq);   /* writers learn -EPIPE */
    if (side & FIFO_WR)
        waitqueue_wake_all(&ring->rd_wq);   /* readers learn EOF */
    waitqueue_wake_all(&fifo->openers);     /* an opener's condition may have changed */
    if (gone != NULL)
        pipe_ring_free(gone);               /* nobody waits on a ring with no opens */
}

/* The other side's generation, read without the lock by the waiter. */
static unsigned peer_gen(const struct fifo *fifo, unsigned side)
{
    return __atomic_load_n(side == FIFO_RD ? &fifo->w_gen : &fifo->r_gen, __ATOMIC_ACQUIRE);
}

int fifo_open(struct fifo *fifo, struct file *f)
{
    unsigned acc = f->flags & COSMO_O_ACCMODE;
    unsigned side = acc == COSMO_O_RDONLY ? FIFO_RD : acc == COSMO_O_WRONLY ? FIFO_WR : FIFO_RD | FIFO_WR;
    bool nonblock = (f->flags & COSMO_O_NONBLOCK) != 0;
    struct fifo_open *fo = kzalloc(sizeof(*fo));
    if (fo == NULL)
        return -ENOMEM;
    fo->side = side;
    fo->nonblock = nonblock;

    /* The first open makes the ring. The allocator is not a thing to
     * call with a spinlock held, so an open that finds no ring drops the
     * lock, allocates one and looks again; an open that finds a ring
     * allocates nothing, and one that lost the race to make it gives
     * its ring back after the lock. */
    struct pipe *fresh = NULL;
    arch_irq_state_t s = spin_lock_irqsave(&fifo->lock);
    while (fifo->ring == NULL && fresh == NULL) {
        spin_unlock_irqrestore(&fifo->lock, s);
        fresh = pipe_ring_alloc();
        if (fresh == NULL) {
            kfree(fo);
            return -ENOMEM;
        }
        s = spin_lock_irqsave(&fifo->lock);
    }
    if (fifo->ring == NULL) {
        fifo->ring = fresh;
        fresh = NULL;
        __atomic_add_fetch(&g_rings, 1, __ATOMIC_RELAXED);
    }
    struct pipe *ring = fifo->ring;
    spin_lock(&ring->lock);
    if (side & FIFO_RD) {
        ring->readers++;
        fifo->r_gen++;
    }
    if (side & FIFO_WR) {
        ring->writers++;
        fifo->w_gen++;
    }
    /* The other side is here now, or this open waits for its generation
     * to move past this. */
    bool peer = side == FIFO_RD ? ring->writers > 0 : side == FIFO_WR ? ring->readers > 0 : true;
    unsigned gen = side == FIFO_RD ? fifo->w_gen : fifo->r_gen;
    bool refused = side == FIFO_WR && nonblock && ring->readers == 0;   /* POSIX: -ENXIO */
    spin_unlock(&ring->lock);
    struct pipe *gone = refused ? drop_locked(fifo, ring, side) : NULL;
    spin_unlock_irqrestore(&fifo->lock, s);
    if (fresh != NULL)
        pipe_ring_free(fresh);   /* another opener made the ring first */
    if (refused) {
        if (gone != NULL)
            pipe_ring_free(gone);
        kfree(fo);
        return -ENXIO;
    }
    waitqueue_wake_all(&fifo->openers);   /* the other side's openers have their peer */
    fo->ring = ring;

    if (!peer && side != (FIFO_RD | FIFO_WR) && !(side == FIFO_RD && nonblock)) {
        /* Our count holds the ring; the wait touches nothing else. */
        int rc = wait_event_killable(&fifo->openers, peer_gen(fifo, side) != gen);
        if (rc) {
            drop(fifo, ring, side);   /* as if this open never happened */
            kfree(fo);
            return rc;
        }
    }
    f->priv = fo;
    return 0;
}

void fifo_release(struct fifo *fifo, struct file *f)
{
    struct fifo_open *fo = f->priv;
    f->priv = NULL;
    drop(fifo, fo->ring, fo->side);
    kfree(fo);
}

int64_t fifo_read(struct fifo *fifo, struct file *f, void *buf, size_t len)
{
    (void)fifo;
    struct fifo_open *fo = f->priv;
    return pipe_ring_read(fo->ring, buf, len, __atomic_load_n(&fo->nonblock, __ATOMIC_RELAXED));
}

int64_t fifo_write(struct fifo *fifo, struct file *f, const void *buf, size_t len)
{
    (void)fifo;
    struct fifo_open *fo = f->priv;
    return pipe_ring_write(fo->ring, buf, len, __atomic_load_n(&fo->nonblock, __ATOMIC_RELAXED));
}

unsigned fifo_ready(struct fifo *fifo, struct file *f)
{
    (void)fifo;
    struct fifo_open *fo = f->priv;
    unsigned r = 0;
    if (fo->side & FIFO_RD)
        r |= pipe_ring_ready_rd(fo->ring);
    if (fo->side & FIFO_WR)
        r |= pipe_ring_ready_wr(fo->ring);
    return r;
}

struct waitqueue *fifo_poll_wq(struct fifo *fifo, struct file *f, unsigned events)
{
    (void)fifo;
    struct fifo_open *fo = f->priv;
    if (fo->side == FIFO_RD)
        return &fo->ring->rd_wq;
    if (fo->side == FIFO_WR)
        return &fo->ring->wr_wq;
    return (events & COSMO_IO_WRITABLE) && !(events & COSMO_IO_READABLE) ? &fo->ring->wr_wq : &fo->ring->rd_wq;
}

int fifo_set_nonblock(struct fifo *fifo, struct file *f, int on)
{
    (void)fifo;
    struct fifo_open *fo = f->priv;
    int was = __atomic_load_n(&fo->nonblock, __ATOMIC_RELAXED) ? 1 : 0;
    if (on >= 0)
        __atomic_store_n(&fo->nonblock, on != 0, __ATOMIC_RELAXED);
    return was;
}

unsigned fifo_count(void)
{
    return __atomic_load_n(&g_rings, __ATOMIC_ACQUIRE);
}
