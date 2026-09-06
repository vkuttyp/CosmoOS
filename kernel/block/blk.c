/*
 * blk.c - Block device registry, request validation, synchronous helpers.
 */

#include <kernel/blk.h>
#include <kernel/completion.h>
#include <kernel/dma.h>
#include <kernel/errno.h>
#include <kernel/faultinject.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/mutex.h>
#include <kernel/sched.h>
#include <kernel/panic.h>
#include <kernel/printf.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <arch/cpu.h>

static struct mutex g_blk_lock;
static LIST_HEAD(g_blkdevs);
static unsigned g_count;
static struct thread *g_timeout_thread;   /* started at the first registration */
static void blk_timeout_thread(void *arg);

/* Internal bio flag: reported by the timeout thread once. */
#define BIO_TIMED_OUT (1u << 30)

static void blkdev_release(struct kobject *obj)
{
    struct blkdev *bd = container_of(obj, struct blkdev, obj);
    KASSERT(list_empty(&bd->link));   /* unregistered before the last put */
    bd->ops->release(bd);
}

static const struct kobject_type blkdev_type = {
    .name = "blkdev",
    .release = blkdev_release,
};

void blk_init(void)
{
    mutex_init(&g_blk_lock, "blkdevs");
}

static bool name_taken(const char *name)
{
    struct blkdev *b;
    list_for_each_entry(b, &g_blkdevs, link) {
        if (strcmp(b->name, name) == 0)
            return true;
    }
    return false;
}

static bool geometry_ok(const struct blkdev *bd)
{
    return bd->ops != NULL && bd->ops->submit != NULL && bd->ops->release != NULL && bd->sector_size >= 512 &&
           (bd->sector_size & (bd->sector_size - 1)) == 0 && bd->capacity != 0 && bd->max_sectors != 0;
}

/* Registry lock held, name chosen. Only now does the object exist
 * (reference 1 to the creator, the owner module's live-object count
 * raised); a failed registration leaves nothing to balance. */
static void register_locked(struct blkdev *bd)
{
    kobject_init(&bd->obj, &blkdev_type);
    kobject_track_code(&bd->obj, (uintptr_t)bd->ops->release);
    list_init(&bd->link);
    bd->reads = bd->writes = bd->flushes = bd->errors = bd->timeouts = 0;
    bd->completed_local = bd->completed_remote = 0;
    if (bd->nr_queues == 0)
        bd->nr_queues = 1;
    bd->gone = false;
    bd->submitting = 0;
    list_init(&bd->pending);
    list_init(&bd->inflight);
    spinlock_init(&bd->qlock, "blk-pending");
    bd->requeued = 0;
    if (bd->max_segments == 0)
        bd->max_segments = 1;
    if (bd->timeout_ns == 0)
        bd->timeout_ns = BLK_TIMEOUT_NS;
    list_push_back(&g_blkdevs, &bd->link);
    g_count++;
    kobject_get(&bd->obj);   /* the registry's reference */
    if (g_timeout_thread == NULL)
        g_timeout_thread = thread_create(blk_timeout_thread, NULL, "blk-timeout", SCHED_PRIO_DEFAULT);
}

static void announce(const struct blkdev *bd)
{
    kinfo("blk: %s: %llu sectors of %u bytes (%llu MiB)%s", bd->name, (unsigned long long)bd->capacity,
          bd->sector_size, (unsigned long long)((bd->capacity * bd->sector_size) >> 20),
          bd->read_only ? ", read-only" : "");
}

int blk_register(struct blkdev *bd, const char *prefix)
{
    if (!geometry_ok(bd) || strlen(prefix) + 2 > BLKDEV_NAME_MAX)
        return -EINVAL;

    mutex_lock(&g_blk_lock);
    char letter = 'a';
    for (; letter <= 'z'; letter++) {
        ksnprintf(bd->name, sizeof(bd->name), "%s%c", prefix, letter);
        if (!name_taken(bd->name))
            break;
    }
    if (letter > 'z') {
        mutex_unlock(&g_blk_lock);
        return -ENOSPC;   /* the object is untouched: no kobject, no owner count; the caller frees its storage */
    }
    register_locked(bd);
    mutex_unlock(&g_blk_lock);
    announce(bd);
    return 0;
}

int blk_register_named(struct blkdev *bd, const char *name)
{
    if (!geometry_ok(bd) || name == NULL || name[0] == '\0' || strlen(name) >= BLKDEV_NAME_MAX)
        return -EINVAL;
    mutex_lock(&g_blk_lock);
    if (name_taken(name)) {
        mutex_unlock(&g_blk_lock);
        return -EEXIST;
    }
    strlcpy(bd->name, name, sizeof(bd->name));
    register_locked(bd);
    mutex_unlock(&g_blk_lock);
    announce(bd);
    return 0;
}

void bio_segment(const struct bio *bio, unsigned i, struct bio_vec *out)
{
    if (bio->nr_vecs == 0) {
        KASSERT(i == 0);
        out->buf = bio->buf;
        out->len = bio->nsectors * bio->dev->sector_size;
        return;
    }
    KASSERT(i < bio->nr_vecs);
    *out = bio->vecs[i];
}

/* The data buffer(s) of a read or write: total length, the segment
 * rules (docs/kernel/device/design.md, "Multi-segment bios"), DMA-able. */
static bool data_ok(const struct blkdev *bd, const struct bio *bio)
{
    size_t total = (size_t)bio->nsectors * bd->sector_size;
    if (bio->nr_vecs == 0)
        return bio->buf != NULL && dma_mappable(bd->dev, bio->buf, total);
    if (bio->vecs == NULL || bio->nr_vecs > bd->max_segments)
        return false;
    size_t sum = 0;
    for (unsigned i = 0; i < bio->nr_vecs; i++) {
        const struct bio_vec *v = &bio->vecs[i];
        if (v->buf == NULL || v->len == 0)
            return false;
        if (i > 0 && ((uintptr_t)v->buf & (PAGE_SIZE - 1)) != 0)
            return false;   /* every segment but the first starts on a page */
        if (i + 1 < bio->nr_vecs && (((uintptr_t)v->buf + v->len) & (PAGE_SIZE - 1)) != 0)
            return false;   /* every segment but the last ends on one */
        if (!dma_mappable(bd->dev, v->buf, v->len))
            return false;
        sum += v->len;
    }
    return sum == total;
}

/* qlock held. */
static void inflight_add_locked(struct blkdev *bd, struct bio *bio)
{
    bio->issued_ns = clock_now_ns();
    bio->issue_cpu = arch_cpu_id();
    bio->flags &= ~BIO_TIMED_OUT;
    list_push_back(&bd->inflight, &bio->inflight_link);
}

static void inflight_remove(struct blkdev *bd, struct bio *bio)
{
    arch_irq_state_t s = spin_lock_irqsave(&bd->qlock);
    if (!list_empty(&bio->inflight_link)) {
        list_remove(&bio->inflight_link);
        list_init(&bio->inflight_link);
    }
    spin_unlock_irqrestore(&bd->qlock, s);
}

/*
 * Every 500 ms: bios older than their device's timeout are reported once
 * and handed to the driver's timeout operation (thread context). The
 * driver receives a pointer it must find in its own in-flight records
 * under its own lock before touching it: the request may complete on
 * another CPU at any moment, and once it has the memory is the owner's.
 */
static void blk_timeout_thread(void *arg)
{
    (void)arg;
    for (;;) {
        thread_sleep_ms(500);
        mutex_lock(&g_blk_lock);
        struct blkdev *bd;
        list_for_each_entry(bd, &g_blkdevs, link) {
            if (bd->timeout_ns == UINT64_MAX || bd->ops->timeout == NULL)
                continue;
            struct bio *expired[8];
            unsigned n = 0;
            uint64_t now = clock_now_ns();
            arch_irq_state_t s = spin_lock_irqsave(&bd->qlock);
            struct bio *b;
            list_for_each_entry(b, &bd->inflight, inflight_link) {
                if (now - b->issued_ns < bd->timeout_ns)
                    break;   /* oldest first: the rest are younger */
                if (b->flags & BIO_TIMED_OUT)
                    continue;
                b->flags |= BIO_TIMED_OUT;
                bd->timeouts++;
                expired[n++] = b;
                if (n == ARRAY_SIZE(expired))
                    break;
            }
            spin_unlock_irqrestore(&bd->qlock, s);
            for (unsigned i = 0; i < n; i++) {
                kwarn("blk: %s: request timed out after %llu ms", bd->name,
                      (unsigned long long)(bd->timeout_ns / 1000000));
                bd->ops->timeout(bd, expired[i]);
            }
        }
        mutex_unlock(&g_blk_lock);
    }
}

void blk_unregister(struct blkdev *bd)
{
    mutex_lock(&g_blk_lock);
    list_remove(&bd->link);
    list_init(&bd->link);
    g_count--;
    mutex_unlock(&g_blk_lock);

    /* Refuse new submissions, then wait for the ones inside the driver.
     * Both sides are sequentially consistent: a submitter that did not
     * see `gone` has raised `submitting` before we read it, or we saw
     * its increment (docs/kernel/quiesce/design.md, "Block devices"). */
    __atomic_store_n(&bd->gone, true, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&bd->submitting, __ATOMIC_SEQ_CST) != 0)
        sched_yield();
    /* Nothing waiting in the pending list will ever reach the driver. */
    for (;;) {
        arch_irq_state_t s = spin_lock_irqsave(&bd->qlock);
        struct list_node *n = list_empty(&bd->pending) ? NULL : list_pop_front(&bd->pending);
        spin_unlock_irqrestore(&bd->qlock, s);
        if (n == NULL)
            break;
        bio_complete(container_of(n, struct bio, link), -ENODEV);
    }
    kinfo("blk: %s removed", bd->name);
    kobject_put(&bd->obj);   /* the registry's reference */
}

static int submit_checked(struct blkdev *bd, struct bio *bio);
static int submit_flagged(struct blkdev *bd, struct bio *bio);
static int to_driver(struct blkdev *bd, struct bio *bio);

int blk_submit(struct bio *bio)
{
    struct blkdev *bd = bio->dev;
    if (bd == NULL || bio->done == NULL)
        return -EINVAL;
    __atomic_fetch_add(&bd->submitting, 1u, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&bd->gone, __ATOMIC_SEQ_CST)) {
        __atomic_fetch_sub(&bd->submitting, 1u, __ATOMIC_SEQ_CST);
        return -ENODEV;
    }
    int rc = (bio->flags & (BIO_PREFLUSH | BIO_FUA)) ? submit_flagged(bd, bio) : submit_checked(bd, bio);
    __atomic_fetch_sub(&bd->submitting, 1u, __ATOMIC_SEQ_CST);
    return rc;
}

/*
 * The pending queue (docs/kernel-services/filesystem/cosmofs/design.md,
 * "The block layer"): a driver that refuses a bio with -EAGAIN has no
 * slot for it; the bio waits here and every completion resubmits from
 * the head. No lock is held across ops->submit (a driver may complete
 * synchronously and re-enter through bio_complete), so a resubmission
 * that is refused again goes back to the head and the next completion
 * tries once more. The caller's `done` runs exactly once, when the bio
 * finally completes.
 */
static void drain_pending(struct blkdev *bd)
{
    for (;;) {
        arch_irq_state_t s = spin_lock_irqsave(&bd->qlock);
        if (list_empty(&bd->pending)) {
            spin_unlock_irqrestore(&bd->qlock, s);
            return;
        }
        struct bio *bio = container_of(list_pop_front(&bd->pending), struct bio, link);
        spin_unlock_irqrestore(&bd->qlock, s);
        int rc = to_driver(bd, bio);
        if (rc == -EAGAIN) {
            s = spin_lock_irqsave(&bd->qlock);
            list_push_front(&bd->pending, &bio->link);
            spin_unlock_irqrestore(&bd->qlock, s);
            return;
        }
        if (rc)
            bio_complete(bio, rc);   /* the driver never owned it */
    }
}

/* Call the driver with the bio on the in-flight list: a driver may
 * complete synchronously from inside submit, and the completion must
 * find the bio there to take it off. A refusal takes it off again. */
static int to_driver(struct blkdev *bd, struct bio *bio)
{
    arch_irq_state_t s = spin_lock_irqsave(&bd->qlock);
    inflight_add_locked(bd, bio);
    spin_unlock_irqrestore(&bd->qlock, s);
    int rc = bd->ops->submit(bd, bio);
    if (rc)
        inflight_remove(bd, bio);
    return rc;
}

/* Hand a validated bio to the driver; -EAGAIN parks it in the queue. */
static int driver_submit(struct blkdev *bd, struct bio *bio)
{
    list_init(&bio->inflight_link);
    arch_irq_state_t s = spin_lock_irqsave(&bd->qlock);
    bool waiting = !list_empty(&bd->pending);
    if (waiting) {
        list_push_back(&bd->pending, &bio->link);   /* keep the order behind those already waiting */
        bd->requeued++;
    }
    spin_unlock_irqrestore(&bd->qlock, s);
    if (waiting) {
        drain_pending(bd);   /* a slot may have freed since the queue formed */
        return 0;
    }
    int rc = to_driver(bd, bio);
    if (rc != -EAGAIN)
        return rc;
    s = spin_lock_irqsave(&bd->qlock);
    list_push_back(&bd->pending, &bio->link);
    bd->requeued++;
    spin_unlock_irqrestore(&bd->qlock, s);
    drain_pending(bd);   /* closes the window between the refusal and the enqueue */
    return 0;
}

/*
 * BIO_PREFLUSH / BIO_FUA as a sequence: flush, the write, flush. The
 * user's bio keeps its own fields; its `done` is parked in the sequence
 * and restored before it runs. A failure anywhere completes the user's
 * bio with that status.
 */
struct bio_seq {
    struct bio flush;
    struct bio *user;
    void (*user_done)(struct bio *bio);
    void *user_arg;               /* the caller's context, parked while the sequence borrows the field */
    unsigned pending_flags;
};

/* Give the user's bio its own done and arg back. */
static void seq_restore(struct bio_seq *seq)
{
    seq->user->done = seq->user_done;
    seq->user->arg = seq->user_arg;
}

static void seq_finish(struct bio_seq *seq, int status)
{
    struct bio *u = seq->user;
    seq_restore(seq);
    kfree(seq);
    bio_complete(u, status);
}

static void seq_post_flush_done(struct bio *bio)
{
    struct bio_seq *seq = bio->arg;
    seq_finish(seq, bio->status);
}

static void seq_write_done(struct bio *bio)
{
    struct bio_seq *seq = bio->arg;
    if (bio->status || !(seq->pending_flags & BIO_FUA)) {
        seq_finish(seq, bio->status);
        return;
    }
    memset(&seq->flush, 0, sizeof(seq->flush));
    seq->flush.dev = bio->dev;
    seq->flush.dir = BIO_FLUSH;
    seq->flush.done = seq_post_flush_done;
    seq->flush.arg = seq;
    list_init(&seq->flush.link);
    seq->flush.status = -EAGAIN;
    int rc = driver_submit(bio->dev, &seq->flush);
    if (rc)
        seq_finish(seq, rc);
}

static void seq_pre_flush_done(struct bio *bio)
{
    struct bio_seq *seq = bio->arg;
    if (bio->status) {
        seq_finish(seq, bio->status);
        return;
    }
    struct bio *u = seq->user;
    u->status = -EAGAIN;
    int rc = driver_submit(u->dev, u);
    if (rc)
        seq_finish(seq, rc);
}

static int submit_flagged(struct blkdev *bd, struct bio *bio)
{
    if (bio->dir != BIO_WRITE)
        return -EINVAL;   /* flags belong to writes */
    /* Validate the write itself first, without submitting it. */
    if (bio->nsectors == 0 || bio->nsectors > bd->max_sectors)
        return -EINVAL;
    if (bio->sector >= bd->capacity || bd->capacity - bio->sector < bio->nsectors)
        return -EINVAL;
    if (bd->read_only)
        return -EROFS;
    if (!data_ok(bd, bio))
        return -EINVAL;
    struct bio_seq *seq = kzalloc(sizeof(*seq));
    if (seq == NULL)
        return -ENOMEM;
    seq->user = bio;
    seq->user_done = bio->done;
    seq->user_arg = bio->arg;
    seq->pending_flags = bio->flags;
    bio->done = seq_write_done;
    bio->arg = seq;   /* borrowed until seq_restore; the caller's arg is parked in the sequence */
    bio->status = -EAGAIN;
    if (bio->flags & BIO_PREFLUSH) {
        seq->flush.dev = bd;
        seq->flush.dir = BIO_FLUSH;
        seq->flush.done = seq_pre_flush_done;
        seq->flush.arg = seq;
        list_init(&seq->flush.link);
        seq->flush.status = -EAGAIN;
        int rc = driver_submit(bd, &seq->flush);
        if (rc) {
            seq_restore(seq);
            kfree(seq);
        }
        return rc;
    }
    int rc = driver_submit(bd, bio);
    if (rc) {
        seq_restore(seq);
        kfree(seq);
    }
    return rc;
}

static int submit_checked(struct blkdev *bd, struct bio *bio)
{
    if (bio->dir == BIO_FLUSH) {
        if (bio->nsectors != 0 || bio->sector != 0)
            return -EINVAL;
    } else {
        if (bio->nsectors == 0 || bio->nsectors > bd->max_sectors)
            return -EINVAL;
        if (bio->sector >= bd->capacity || bd->capacity - bio->sector < bio->nsectors)
            return -EINVAL;
        if (bio->dir == BIO_WRITE && bd->read_only)
            return -EROFS;
        if (bio->dir != BIO_READ && bio->dir != BIO_WRITE)
            return -EINVAL;
        if (!data_ok(bd, bio))
            return -EINVAL;   /* not DMA-able memory, or segments that break the rules */
    }
    if (faultinject_should_fail(FI_BLK_SUBMIT))
        return -EIO;   /* debug builds: an injected submission failure (docs/verification/) */
    bio->status = -EAGAIN;   /* in flight */
    return driver_submit(bd, bio);
}

void bio_complete(struct bio *bio, int status)
{
    struct blkdev *bd = bio->dev;
    inflight_remove(bd, bio);
    if (bio->issue_cpu == arch_cpu_id())
        __atomic_fetch_add(&bd->completed_local, 1, __ATOMIC_RELAXED);
    else
        __atomic_fetch_add(&bd->completed_remote, 1, __ATOMIC_RELAXED);
    if (status == 0 && faultinject_should_fail(FI_BLK_COMPLETE))
        status = -EIO;   /* debug builds: an injected device error (docs/verification/) */
    bio->status = status;
    if (status)
        __atomic_fetch_add(&bd->errors, 1, __ATOMIC_RELAXED);
    else if (bio->dir == BIO_READ)
        __atomic_fetch_add(&bd->reads, 1, __ATOMIC_RELAXED);
    else if (bio->dir == BIO_WRITE)
        __atomic_fetch_add(&bd->writes, 1, __ATOMIC_RELAXED);
    else
        __atomic_fetch_add(&bd->flushes, 1, __ATOMIC_RELAXED);
    bio->done(bio);
    drain_pending(bd);   /* a slot is free: the next waiting bio goes in */
}

struct sync_bio {
    struct bio bio;
    struct completion done;
};

static void sync_done(struct bio *bio)
{
    struct sync_bio *s = container_of(bio, struct sync_bio, bio);
    complete(&s->done);
}

static int sync_io(struct blkdev *bd, enum bio_dir dir, uint64_t sector, uint32_t nsectors, void *buf,
                   unsigned flags)
{
    if (dir != BIO_FLUSH && (nsectors == 0 || buf == NULL))
        return -EINVAL;
    while (nsectors > 0 || dir == BIO_FLUSH) {
        uint32_t n = nsectors < bd->max_sectors ? nsectors : bd->max_sectors;
        struct sync_bio s;
        memset(&s.bio, 0, sizeof(s.bio));
        s.bio.dev = bd;
        s.bio.dir = dir;
        s.bio.flags = dir == BIO_WRITE ? flags : 0;
        s.bio.sector = dir == BIO_FLUSH ? 0 : sector;
        s.bio.nsectors = dir == BIO_FLUSH ? 0 : n;
        s.bio.buf = buf;
        s.bio.done = sync_done;
        list_init(&s.bio.link);
        completion_init(&s.done, "blk-sync");
        int rc = blk_submit(&s.bio);
        if (rc)
            return rc;
        wait_for_completion(&s.done);
        if (s.bio.status)
            return s.bio.status;
        if (dir == BIO_FLUSH)
            return 0;
        sector += n;
        nsectors -= n;
        buf = (uint8_t *)buf + (size_t)n * bd->sector_size;
    }
    return 0;
}

int blk_read(struct blkdev *bd, uint64_t sector, uint32_t nsectors, void *buf)
{
    return sync_io(bd, BIO_READ, sector, nsectors, buf, 0);
}

int blk_write(struct blkdev *bd, uint64_t sector, uint32_t nsectors, const void *buf)
{
    return sync_io(bd, BIO_WRITE, sector, nsectors, (void *)(uintptr_t)buf, 0);
}

int blk_write_flags(struct blkdev *bd, uint64_t sector, uint32_t nsectors, const void *buf, unsigned flags)
{
    return sync_io(bd, BIO_WRITE, sector, nsectors, (void *)(uintptr_t)buf, flags);
}

int blk_flush(struct blkdev *bd)
{
    return sync_io(bd, BIO_FLUSH, 0, 0, NULL, 0);
}

struct blkdev *blk_find(const char *name)
{
    mutex_lock(&g_blk_lock);
    struct blkdev *b, *found = NULL;
    list_for_each_entry(b, &g_blkdevs, link) {
        if (strcmp(b->name, name) == 0) {
            found = b;
            kobject_get(&b->obj);
            break;
        }
    }
    mutex_unlock(&g_blk_lock);
    return found;
}

/* The i'th registered device, referenced, or NULL past the end. The
 * registry can change between calls, so this is for enumeration that
 * tolerates a moving set -- pool assembly, which matches labels and
 * rejects what it does not recognise. */
struct blkdev *blk_nth(unsigned i)
{
    mutex_lock(&g_blk_lock);
    struct blkdev *b, *found = NULL;
    unsigned n = 0;
    list_for_each_entry(b, &g_blkdevs, link) {
        if (n++ == i) {
            found = b;
            kobject_get(&b->obj);
            break;
        }
    }
    mutex_unlock(&g_blk_lock);
    return found;
}

unsigned blk_count(void)
{
    mutex_lock(&g_blk_lock);
    unsigned n = g_count;
    mutex_unlock(&g_blk_lock);
    return n;
}

void blk_dump(void)
{
    mutex_lock(&g_blk_lock);
    kprintf("block devices (%u):\n", g_count);
    struct blkdev *b;
    list_for_each_entry(b, &g_blkdevs, link) {
        kprintf("  %-8s %llu x %u%s  reads %llu writes %llu flushes %llu errors %llu\n", b->name,
                (unsigned long long)b->capacity, b->sector_size, b->read_only ? " ro" : "",
                (unsigned long long)b->reads, (unsigned long long)b->writes, (unsigned long long)b->flushes,
                (unsigned long long)b->errors);
    }
    mutex_unlock(&g_blk_lock);
}

/* Module ABI v1 exports (docs/kernel/module/api.md). */
#include <kernel/module.h>
EXPORT_SYMBOL(blk_register);
EXPORT_SYMBOL(blk_register_named);
EXPORT_SYMBOL(bio_segment);
EXPORT_SYMBOL(blk_unregister);
EXPORT_SYMBOL(blk_submit);
EXPORT_SYMBOL(bio_complete);
EXPORT_SYMBOL(blk_read);
EXPORT_SYMBOL(blk_write);
EXPORT_SYMBOL(blk_write_flags);
EXPORT_SYMBOL(blk_flush);
EXPORT_SYMBOL(blk_find);
