/*
 * ramblk.c - RAM block device with a write recorder
 * (docs/verification/design.md). Debug builds; used by the cosmofs
 * crash-consistency harness and the block fault-injection test.
 *
 * Storage is one kmalloc'd 4 KiB block per device block, so every buffer
 * is DMA-mappable and blk_submit's check passes. submit copies and
 * completes synchronously, under the device lock, so the recorded log is
 * in completion order.
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/ramblk.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/wait.h>

#if CONFIG_DEBUG

struct ramblk {
    struct blkdev bd;
    uint8_t **blocks;
    uint64_t nblocks;
    spinlock_t lock;
    struct ramblk_log *log;   /* recording when non-NULL */
    /* Deferred mode (tests of the block layer's queueing): completions
     * run on a worker thread and submit refuses with -EAGAIN above
     * `limit` requests in flight. */
    unsigned limit;           /* 0: synchronous completion */
    unsigned inflight;
    struct list_node deferred;
    struct thread *worker;
    bool stop;
    bool stall;               /* deferred mode: the worker completes nothing */
};

static struct ramblk *of(struct blkdev *bd)
{
    return bd->priv;
}

static void record(struct ramblk *r, const struct bio *bio)
{
    struct ramblk_log *log = r->log;
    if (log == NULL)
        return;
    if (log->n == log->cap) {
        log->dropped++;
        return;
    }
    struct ramblk_write *w = &log->w[log->n];
    w->sector = bio->sector;
    w->nsectors = bio->dir == BIO_FLUSH ? 0 : bio->nsectors;
    w->data = NULL;
    if (w->nsectors) {
        w->data = kmalloc((size_t)w->nsectors * 512, 0);
        if (w->data == NULL) {
            log->dropped++;
            return;
        }
        size_t done = 0;
        for (unsigned i = 0; i < bio_segments(bio); i++) {
            struct bio_vec v;
            bio_segment(bio, i, &v);
            memcpy(w->data + done, v.buf, v.len);
            done += v.len;
        }
    }
    log->n++;
}

static int ramblk_submit(struct blkdev *bd, struct bio *bio)
{
    struct ramblk *r = of(bd);
    arch_irq_state_t s = spin_lock_irqsave(&r->lock);
    if (r->limit && r->inflight >= r->limit) {
        spin_unlock_irqrestore(&r->lock, s);
        return -EAGAIN;   /* the queue is full, like a virtqueue with every slot taken */
    }
    if (bio->dir != BIO_FLUSH) {
        uint64_t byte = bio->sector * 512;
        for (unsigned i = 0; i < bio_segments(bio); i++) {
            struct bio_vec v;
            bio_segment(bio, i, &v);
            size_t len = v.len;
            uint8_t *buf = v.buf;
            while (len) {
                uint64_t blk = byte / RAMBLK_BLOCK, off = byte % RAMBLK_BLOCK;
                size_t chunk = RAMBLK_BLOCK - off < len ? RAMBLK_BLOCK - off : len;
                if (bio->dir == BIO_WRITE)
                    memcpy(r->blocks[blk] + off, buf, chunk);
                else
                    memcpy(buf, r->blocks[blk] + off, chunk);
                byte += chunk;
                buf += chunk;
                len -= chunk;
            }
        }
    }
    if (bio->dir != BIO_READ)
        record(r, bio);
    if (r->limit) {
        r->inflight++;
        list_push_back(&r->deferred, &bio->link);   /* the worker completes it */
        spin_unlock_irqrestore(&r->lock, s);
        return 0;
    }
    spin_unlock_irqrestore(&r->lock, s);
    bio_complete(bio, 0);
    return 0;
}

/* Complete deferred bios in order, one batch per millisecond. */
static void ramblk_worker(void *arg)
{
    struct ramblk *r = arg;
    for (;;) {
        for (;;) {
            arch_irq_state_t s = spin_lock_irqsave(&r->lock);
            struct bio *bio = (list_empty(&r->deferred) || r->stall)
                                  ? NULL
                                  : container_of(list_pop_front(&r->deferred), struct bio, link);
            if (bio)
                r->inflight--;
            spin_unlock_irqrestore(&r->lock, s);
            if (bio == NULL)
                break;
            bio_complete(bio, 0);
        }
        if (__atomic_load_n(&r->stop, __ATOMIC_ACQUIRE))
            break;
        thread_sleep_ms(1);
    }
}

/* The block layer's timeout thread: the request is still ours only if it
 * is on the deferred list; find it by pointer under the lock, never
 * dereference it first (it may have completed on another CPU). */
static void ramblk_timeout(struct blkdev *bd, struct bio *victim)
{
    struct ramblk *r = of(bd);
    struct bio *found = NULL, *b;
    arch_irq_state_t s = spin_lock_irqsave(&r->lock);
    list_for_each_entry(b, &r->deferred, link) {
        if (b == victim) {
            found = b;
            break;
        }
    }
    if (found) {
        list_remove(&found->link);
        list_init(&found->link);
        r->inflight--;
    }
    spin_unlock_irqrestore(&r->lock, s);
    if (found)
        bio_complete(found, -ETIMEDOUT);
}

void ramblk_set_stall(struct blkdev *bd, bool stall)
{
    struct ramblk *r = of(bd);
    arch_irq_state_t s = spin_lock_irqsave(&r->lock);
    r->stall = stall;
    spin_unlock_irqrestore(&r->lock, s);
}

void ramblk_set_deferred(struct blkdev *bd, unsigned limit)
{
    struct ramblk *r = of(bd);
    if (limit && r->worker == NULL) {
        r->stop = false;
        r->limit = limit;
        r->worker = thread_create(ramblk_worker, r, "ramblk-worker", SCHED_PRIO_DEFAULT);
        if (r->worker == NULL)
            r->limit = 0;
        return;
    }
    if (limit == 0 && r->worker) {
        __atomic_store_n(&r->stop, true, __ATOMIC_RELEASE);
        thread_join(r->worker);   /* drains what is still deferred */
        r->worker = NULL;
        r->limit = 0;
    }
}

static void ramblk_release(struct blkdev *bd)
{
    struct ramblk *r = of(bd);
    for (uint64_t i = 0; i < r->nblocks; i++)
        kfree(r->blocks[i]);
    kfree(r->blocks);
    kfree(r);
}

static const struct blkdev_ops ramblk_ops = {
    .submit = ramblk_submit,
    .release = ramblk_release,
    .timeout = ramblk_timeout,
};

struct blkdev *ramblk_create(uint64_t nblocks)
{
    struct ramblk *r = kzalloc(sizeof(*r));
    if (r == NULL)
        return NULL;
    r->nblocks = nblocks;
    r->blocks = kzalloc(nblocks * sizeof(*r->blocks));
    if (r->blocks == NULL) {
        kfree(r);
        return NULL;
    }
    for (uint64_t i = 0; i < nblocks; i++) {
        r->blocks[i] = kzalloc(RAMBLK_BLOCK);
        if (r->blocks[i] == NULL) {
            ramblk_release(&r->bd);   /* frees what was allocated; NULL blocks are fine to kfree */
            return NULL;
        }
    }
    spinlock_init(&r->lock, "ramblk");
    list_init(&r->deferred);
    r->bd.ops = &ramblk_ops;
    r->bd.sector_size = 512;
    r->bd.capacity = nblocks * (RAMBLK_BLOCK / 512);
    r->bd.max_sectors = 128;
    r->bd.max_segments = 8;
    r->bd.priv = r;
    if (blk_register(&r->bd, "ram") != 0) {
        ramblk_release(&r->bd);
        return NULL;
    }
    return &r->bd;
}

void ramblk_destroy(struct blkdev *bd)
{
    ramblk_set_stall(bd, false);
    ramblk_set_deferred(bd, 0);   /* completes anything deferred first */
    blk_unregister(bd);
    blkdev_put(bd);   /* the creator's reference; release frees when the last holder is gone */
}

void ramblk_record_start(struct blkdev *bd, unsigned max_entries)
{
    struct ramblk *r = of(bd);
    struct ramblk_log *log = kzalloc(sizeof(*log));
    if (log == NULL)
        return;
    log->w = kzalloc((size_t)max_entries * sizeof(*log->w));
    if (log->w == NULL) {
        kfree(log);
        return;
    }
    log->cap = max_entries;
    arch_irq_state_t s = spin_lock_irqsave(&r->lock);
    r->log = log;
    spin_unlock_irqrestore(&r->lock, s);
}

struct ramblk_log *ramblk_record_stop(struct blkdev *bd)
{
    struct ramblk *r = of(bd);
    arch_irq_state_t s = spin_lock_irqsave(&r->lock);
    struct ramblk_log *log = r->log;
    r->log = NULL;
    spin_unlock_irqrestore(&r->lock, s);
    return log;
}

unsigned ramblk_record_count(struct blkdev *bd)
{
    struct ramblk *r = of(bd);
    arch_irq_state_t s = spin_lock_irqsave(&r->lock);
    unsigned n = r->log ? r->log->n : 0;
    spin_unlock_irqrestore(&r->lock, s);
    return n;
}

void ramblk_log_free(struct ramblk_log *log)
{
    if (log == NULL)
        return;
    for (unsigned i = 0; i < log->n; i++)
        kfree(log->w[i].data);
    kfree(log->w);
    kfree(log);
}

uint8_t *ramblk_snapshot(struct blkdev *bd)
{
    struct ramblk *r = of(bd);
    uint8_t *img = kmalloc((size_t)r->nblocks * RAMBLK_BLOCK, 0);
    if (img == NULL)
        return NULL;
    arch_irq_state_t s = spin_lock_irqsave(&r->lock);
    for (uint64_t i = 0; i < r->nblocks; i++)
        memcpy(img + i * RAMBLK_BLOCK, r->blocks[i], RAMBLK_BLOCK);
    spin_unlock_irqrestore(&r->lock, s);
    return img;
}

void ramblk_restore(struct blkdev *bd, const uint8_t *image)
{
    struct ramblk *r = of(bd);
    arch_irq_state_t s = spin_lock_irqsave(&r->lock);
    for (uint64_t i = 0; i < r->nblocks; i++)
        memcpy(r->blocks[i], image + i * RAMBLK_BLOCK, RAMBLK_BLOCK);
    spin_unlock_irqrestore(&r->lock, s);
}

void ramblk_replay(struct blkdev *bd, const struct ramblk_log *log, unsigned count, bool torn)
{
    struct ramblk *r = of(bd);
    if (count > log->n)
        count = log->n;
    arch_irq_state_t s = spin_lock_irqsave(&r->lock);
    for (unsigned i = 0; i < count; i++) {
        const struct ramblk_write *w = &log->w[i];
        if (w->nsectors == 0)
            continue;   /* a flush changes no bytes */
        uint32_t ns = w->nsectors;
        if (torn && i == count - 1 && ns > 1)
            ns = ns / 2;
        uint64_t byte = w->sector * 512;
        const uint8_t *src = w->data;
        size_t len = (size_t)ns * 512;
        while (len) {
            uint64_t blk = byte / RAMBLK_BLOCK, off = byte % RAMBLK_BLOCK;
            size_t chunk = RAMBLK_BLOCK - off < len ? RAMBLK_BLOCK - off : len;
            memcpy(r->blocks[blk] + off, src, chunk);
            byte += chunk;
            src += chunk;
            len -= chunk;
        }
    }
    spin_unlock_irqrestore(&r->lock, s);
}

#endif /* CONFIG_DEBUG */
