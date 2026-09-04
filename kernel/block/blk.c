/*
 * blk.c - Block device registry, request validation, synchronous helpers.
 */

#include <kernel/blk.h>
#include <kernel/completion.h>
#include <kernel/dma.h>
#include <kernel/errno.h>
#include <kernel/log.h>
#include <kernel/mutex.h>
#include <kernel/panic.h>
#include <kernel/printf.h>
#include <kernel/string.h>

static struct mutex g_blk_lock;
static LIST_HEAD(g_blkdevs);
static unsigned g_count;

static void blkdev_release(struct kobject *obj)
{
    (void)obj;   /* driver-owned storage */
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

int blk_register(struct blkdev *bd, const char *prefix)
{
    if (bd->ops == NULL || bd->ops->submit == NULL || bd->sector_size < 512 ||
        (bd->sector_size & (bd->sector_size - 1)) != 0 || bd->capacity == 0 || bd->max_sectors == 0 ||
        strlen(prefix) + 2 > BLKDEV_NAME_MAX)
        return -EINVAL;

    kobject_init(&bd->obj, &blkdev_type);
    list_init(&bd->link);
    bd->reads = bd->writes = bd->flushes = bd->errors = 0;

    mutex_lock(&g_blk_lock);
    char letter = 'a';
    for (; letter <= 'z'; letter++) {
        ksnprintf(bd->name, sizeof(bd->name), "%s%c", prefix, letter);
        if (!name_taken(bd->name))
            break;
    }
    if (letter > 'z') {
        mutex_unlock(&g_blk_lock);
        return -ENOSPC;
    }
    list_push_back(&g_blkdevs, &bd->link);
    g_count++;
    mutex_unlock(&g_blk_lock);
    kinfo("blk: %s: %llu sectors of %u bytes (%llu MiB)%s", bd->name, (unsigned long long)bd->capacity,
          bd->sector_size, (unsigned long long)((bd->capacity * bd->sector_size) >> 20),
          bd->read_only ? ", read-only" : "");
    return 0;
}

void blk_unregister(struct blkdev *bd)
{
    mutex_lock(&g_blk_lock);
    list_remove(&bd->link);
    list_init(&bd->link);
    g_count--;
    mutex_unlock(&g_blk_lock);
    kinfo("blk: %s removed", bd->name);
}

int blk_submit(struct bio *bio)
{
    struct blkdev *bd = bio->dev;
    if (bd == NULL || bio->done == NULL)
        return -EINVAL;
    if (bio->dir == BIO_FLUSH) {
        if (bio->nsectors != 0 || bio->sector != 0)
            return -EINVAL;
    } else {
        if (bio->nsectors == 0 || bio->nsectors > bd->max_sectors || bio->buf == NULL)
            return -EINVAL;
        if (bio->sector >= bd->capacity || bd->capacity - bio->sector < bio->nsectors)
            return -EINVAL;
        if (bio->dir == BIO_WRITE && bd->read_only)
            return -EROFS;
        if (bio->dir != BIO_READ && bio->dir != BIO_WRITE)
            return -EINVAL;
        if (dma_map(bd->dev, bio->buf, (size_t)bio->nsectors * bd->sector_size, DMA_BIDIRECTIONAL) == 0)
            return -EINVAL;   /* not DMA-able memory */
    }
    bio->status = -EAGAIN;   /* in flight */
    return bd->ops->submit(bd, bio);
}

void bio_complete(struct bio *bio, int status)
{
    struct blkdev *bd = bio->dev;
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

static int sync_io(struct blkdev *bd, enum bio_dir dir, uint64_t sector, uint32_t nsectors, void *buf)
{
    if (dir != BIO_FLUSH && (nsectors == 0 || buf == NULL))
        return -EINVAL;
    while (nsectors > 0 || dir == BIO_FLUSH) {
        uint32_t n = nsectors < bd->max_sectors ? nsectors : bd->max_sectors;
        struct sync_bio s;
        memset(&s.bio, 0, sizeof(s.bio));
        s.bio.dev = bd;
        s.bio.dir = dir;
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
    return sync_io(bd, BIO_READ, sector, nsectors, buf);
}

int blk_write(struct blkdev *bd, uint64_t sector, uint32_t nsectors, const void *buf)
{
    return sync_io(bd, BIO_WRITE, sector, nsectors, (void *)(uintptr_t)buf);
}

int blk_flush(struct blkdev *bd)
{
    return sync_io(bd, BIO_FLUSH, 0, 0, NULL);
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
EXPORT_SYMBOL(blk_unregister);
EXPORT_SYMBOL(blk_submit);
EXPORT_SYMBOL(bio_complete);
EXPORT_SYMBOL(blk_read);
EXPORT_SYMBOL(blk_write);
EXPORT_SYMBOL(blk_flush);
EXPORT_SYMBOL(blk_find);
