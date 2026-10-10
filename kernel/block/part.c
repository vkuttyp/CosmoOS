/*
 * part.c - Partitions as block devices (part.h; docs/kernel/device/design.md,
 * "Partitions").
 *
 * A partition is a struct blkdev whose driver is this file: it forwards
 * each bio to the disk as a new bio at the partition's offset and
 * completes the original when the forwarded one completes. Being an
 * ordinary block device is the point: the block layer validates every bio
 * against the partition's own capacity before this file sees it, mounts
 * and blk_find need nothing new, and pool assembly enumerates partitions
 * like any other device.
 *
 * Lifetime follows the driver rules in blk.h: the scan holds the
 * creator's reference; removal unregisters, waits for the forwarded bios
 * to complete (the disk completes them, with an error if it is going
 * away), and only then drops it. Each partition holds a reference on its
 * disk until it is released.
 */

#include <kernel/blk.h>
#include <kernel/errno.h>
#include <kernel/gpt.h>
#include <kernel/kmalloc.h>
#include <kernel/list.h>
#include <kernel/log.h>
#include <kernel/mutex.h>
#include <kernel/part.h>
#include <kernel/printf.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/wait.h>

struct blkpart {
    struct blkdev bd;
    struct blkdev *disk;        /* referenced until release */
    uint64_t start;
    uint32_t index;
    uint8_t uuid[16];
    uint8_t type[16];
    uint32_t inflight;          /* forwarded bios not yet completed (atomic) */
    struct list_node link;      /* g_parts, under g_part_lock */
};

/* Every partition the scans registered and have not removed. The lock is
 * taken outside the block registry's (blk_register and blk_unregister
 * run under it), never inside. */
static struct mutex g_part_lock;
static LIST_HEAD(g_parts);

static const struct blkdev_ops part_ops;

void blk_part_init(void)
{
    mutex_init(&g_part_lock, "blk-parts");
}

bool blk_is_partition(const struct blkdev *bd)
{
    return bd->ops == &part_ops;
}

/* --- the driver --------------------------------------------------------------- */

static void part_done(struct bio *fwd)
{
    struct bio *orig = fwd->arg;
    struct blkpart *p = orig->dev->priv;
    int status = fwd->status;
    kfree(fwd);
    bio_complete(orig, status);
    /* Last: removal waits for this to fall before dropping the reference
     * that keeps `p` -- and the block layer's state for `orig` -- alive. */
    __atomic_fetch_sub(&p->inflight, 1u, __ATOMIC_RELEASE);
}

static int part_submit(struct blkdev *bd, struct bio *bio)
{
    struct blkpart *p = bd->priv;
    /* blk_submit has checked the range against bd->capacity already
     * (D16); a translation is only ever made from a checked range, so it
     * is checked here as well rather than trusted from a distance. */
    if (bio->dir != BIO_FLUSH &&
        (bio->sector >= bd->capacity || bd->capacity - bio->sector < bio->nsectors))
        return -EINVAL;
    struct bio *fwd = kmalloc(sizeof(*fwd), KMEM_ZERO);
    if (fwd == NULL)
        return -ENOMEM;
    fwd->dev = p->disk;
    fwd->dir = bio->dir;
    fwd->sector = bio->dir == BIO_FLUSH ? 0 : p->start + bio->sector;
    fwd->nsectors = bio->nsectors;
    fwd->buf = bio->buf;
    fwd->vecs = bio->vecs;
    fwd->nr_vecs = bio->nr_vecs;
    fwd->done = part_done;
    fwd->arg = bio;
    list_init(&fwd->link);
    __atomic_fetch_add(&p->inflight, 1u, __ATOMIC_ACQ_REL);
    int rc = blk_submit(fwd);
    if (rc) {
        __atomic_fetch_sub(&p->inflight, 1u, __ATOMIC_RELEASE);
        kfree(fwd);
    }
    return rc;
}

static void part_release(struct blkdev *bd)
{
    struct blkpart *p = bd->priv;
    blkdev_put(p->disk);
    kfree(p);
}

static const struct blkdev_ops part_ops = {
    .submit = part_submit,
    .release = part_release,
};

/* --- scanning ----------------------------------------------------------------- */

static int read_disk(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    return blk_read(ctx, lba, count, buf);
}

static const char *type_name(const uint8_t type[16])
{
    if (memcmp(type, gpt_type_esp, 16) == 0)
        return "esp";
    if (memcmp(type, gpt_type_cosmo_root, 16) == 0)
        return "cosmo-root";
    return "other";
}

/* The scan's reference, dropped once nothing the partition forwarded is
 * still at the disk. A thread of its own because the last completion may
 * come from an interrupt, and the release this put can run may sleep. */
static void reap_when_idle(void *arg)
{
    struct blkpart *p = arg;
    while (__atomic_load_n(&p->inflight, __ATOMIC_ACQUIRE) != 0)
        thread_sleep_ms(1);
    blkdev_put(&p->bd);
    thread_exit(0);
}

/*
 * Unregister, then drop the scan's reference -- at once when nothing is
 * in flight, else from a thread once the disk has completed it all.
 * Never waits here: blk_unregister of a disk calls this first, and a
 * driver may complete what it holds only after that returns (blk.h).
 * g_part_lock held, `p` already off g_parts.
 */
static void remove_one(struct blkpart *p)
{
    blk_unregister(&p->bd);
    if (__atomic_load_n(&p->inflight, __ATOMIC_ACQUIRE) == 0) {
        blkdev_put(&p->bd);
        return;
    }
    struct thread *t = thread_create(reap_when_idle, p, "blk-part-reap", SCHED_PRIO_DEFAULT);
    if (t != NULL) {
        thread_put(t);
        return;
    }
    kwarn("part: %s: no thread to wait out its requests; waiting here", p->bd.name);
    while (__atomic_load_n(&p->inflight, __ATOMIC_ACQUIRE) != 0)
        thread_sleep_ms(1);
    blkdev_put(&p->bd);
}

/* g_part_lock held. Removes every partition of `disk` (none in use, the
 * caller checked). */
static void remove_all_locked(struct blkdev *disk)
{
    struct blkpart *p, *tmp;
    list_for_each_entry_safe(p, tmp, &g_parts, link) {
        if (p->disk != disk)
            continue;
        list_remove(&p->link);
        remove_one(p);
    }
}

static int register_one(struct blkdev *disk, const struct gpt_part *gp)
{
    char name[BLKDEV_NAME_MAX];
    size_t dl = strlen(disk->name);
    bool digit = dl > 0 && disk->name[dl - 1] >= '0' && disk->name[dl - 1] <= '9';
    int n = ksnprintf(name, sizeof(name), "%s%s%u", disk->name, digit ? "p" : "", gp->index);
    if (n < 0 || (size_t)n >= sizeof(name))
        return -ENAMETOOLONG;
    struct blkpart *p = kmalloc(sizeof(*p), KMEM_ZERO);
    if (p == NULL)
        return -ENOMEM;
    p->disk = disk;
    p->start = gp->first_lba;
    p->index = gp->index;
    memcpy(p->uuid, gp->uuid, 16);
    memcpy(p->type, gp->type, 16);
    p->bd.dev = disk->dev;   /* the same DMA rules as the disk the bios go to */
    p->bd.ops = &part_ops;
    p->bd.sector_size = disk->sector_size;
    p->bd.capacity = gp->last_lba - gp->first_lba + 1;
    p->bd.max_sectors = disk->max_sectors;
    p->bd.max_segments = disk->max_segments;
    p->bd.read_only = disk->read_only;
    p->bd.timeout_ns = UINT64_MAX;   /* the disk times out the forwarded bio; a second clock would race it */
    p->bd.priv = p;
    blkdev_get(disk);
    int rc = blk_register_named(&p->bd, name);
    if (rc) {
        blkdev_put(disk);
        kfree(p);
        return rc;
    }
    list_push_back(&g_parts, &p->link);
    char uuid[GPT_GUID_TEXT];
    gpt_guid_format(p->uuid, uuid);
    kinfo("part: %s: %s partition %u, sectors %llu-%llu, type %s, partuuid %s", name, disk->name, gp->index,
          (unsigned long long)gp->first_lba, (unsigned long long)gp->last_lba, type_name(gp->type), uuid);
    return 0;
}

int blk_part_scan(struct blkdev *disk, unsigned *nparts)
{
    if (nparts)
        *nparts = 0;
    if (blk_is_partition(disk))
        return -EINVAL;
    void *scratch = kmalloc(GPT_SCRATCH_BYTES, 0);
    struct gpt_table *t = kmalloc(sizeof(*t), KMEM_ZERO);
    if (scratch == NULL || t == NULL) {
        kfree(scratch);
        kfree(t);
        return -ENOMEM;
    }
    mutex_lock(&g_part_lock);
    /* In use: a reference beyond the registry's and the scan's. */
    struct blkpart *p;
    list_for_each_entry(p, &g_parts, link) {
        if (p->disk == disk && kobject_refcount(&p->bd.obj) > 2) {
            mutex_unlock(&g_part_lock);
            kfree(scratch);
            kfree(t);
            return -EBUSY;
        }
    }
    remove_all_locked(disk);

    const char *why = "";
    enum gpt_result r = gpt_parse(disk->sector_size, disk->capacity, read_disk, disk, scratch, t, &why);
    int rc = 0;
    if (r == GPT_IO) {
        kwarn("part: %s: cannot read the partition table", disk->name);
        rc = -EIO;
    } else if (r == GPT_BAD) {
        kwarn("part: %s: partition table refused: %s", disk->name, why);
        rc = -EINVAL;
    } else if (r == GPT_OK) {
        unsigned n = 0;
        for (unsigned i = 0; i < t->nparts; i++) {
            int e = register_one(disk, &t->parts[i]);
            if (e)
                kwarn("part: %s: partition %u not registered (%d)", disk->name, t->parts[i].index, e);
            else
                n++;
        }
        if (nparts)
            *nparts = n;
    }
    mutex_unlock(&g_part_lock);
    kfree(scratch);
    kfree(t);
    return rc;
}

void blk_part_scan_all(void)
{
    /* Partitions are appended to the registry as they register and are
     * skipped here, so walking by position while the set grows is safe. */
    for (unsigned i = 0;; i++) {
        struct blkdev *bd = blk_nth(i);
        if (bd == NULL)
            break;
        if (!blk_is_partition(bd))
            (void)blk_part_scan(bd, NULL);
        blkdev_put(bd);
    }
}

void blk_part_remove(struct blkdev *disk)
{
    if (blk_is_partition(disk))
        return;
    mutex_lock(&g_part_lock);
    remove_all_locked(disk);
    mutex_unlock(&g_part_lock);
}

bool blk_part_info(const struct blkdev *bd, struct blk_part_info *out)
{
    if (!blk_is_partition(bd))
        return false;
    const struct blkpart *p = bd->priv;
    strlcpy(out->disk, p->disk->name, sizeof(out->disk));
    out->index = p->index;
    out->start = p->start;
    memcpy(out->uuid, p->uuid, 16);
    memcpy(out->type, p->type, 16);
    return true;
}

struct blkdev *blk_find_partuuid(const uint8_t uuid[16])
{
    struct blkdev *found = NULL;
    mutex_lock(&g_part_lock);
    struct blkpart *p;
    list_for_each_entry(p, &g_parts, link) {
        if (memcmp(p->uuid, uuid, 16) == 0) {
            found = &p->bd;
            blkdev_get(found);
            break;
        }
    }
    mutex_unlock(&g_part_lock);
    return found;
}
