/*
 * blk.h - The block layer: block devices and bio requests.
 *
 * A driver registers a struct blkdev with a submit operation; users
 * build a struct bio (sector range, buffer, direction, completion) and
 * submit it, or use the synchronous blk_read/blk_write helpers. The
 * layer knows nothing about any driver or any filesystem (constitution
 * section 28). Buffers must be DMA-able: dma_map() must succeed on
 * them (kmalloc or dma_alloc memory).
 */

#ifndef KERNEL_BLK_H
#define KERNEL_BLK_H

#include <kernel/list.h>
#include <kernel/object.h>
#include <kernel/types.h>

struct device;
struct blkdev;

enum bio_dir {
    BIO_READ,
    BIO_WRITE,
    BIO_FLUSH,   /* no data; sector and nsectors are 0 */
};

struct bio {
    struct blkdev *dev;
    uint64_t sector;
    uint32_t nsectors;
    enum bio_dir dir;
    void *buf;
    void (*done)(struct bio *bio);   /* may run in interrupt context */
    void *arg;
    int status;                      /* 0 or -errno once done ran */
    struct list_node link;           /* for the driver's queue */
    void *drvpriv;                   /* for the driver */
};

struct blkdev_ops {
    /* Take ownership of the bio until bio_complete(). Returns 0 or a
     * negative errno without completing it. Thread context. */
    int (*submit)(struct blkdev *dev, struct bio *bio);
    /* Mandatory: the last reference dropped (after blk_unregister);
     * free the memory the blkdev is embedded in. */
    void (*release)(struct blkdev *dev);
};

#define BLKDEV_NAME_MAX 16

struct blkdev {
    struct kobject obj;
    char name[BLKDEV_NAME_MAX];
    struct device *dev;              /* the hardware behind it, may be NULL */
    const struct blkdev_ops *ops;
    uint32_t sector_size;            /* power of two, >= 512 */
    uint64_t capacity;               /* sectors */
    unsigned max_sectors;            /* per bio */
    bool read_only;
    void *priv;
    struct list_node link;
    uint64_t reads, writes, flushes, errors;   /* completed bios */
    bool gone;                       /* unregistered: blk_submit refuses (-ENODEV) */
    uint32_t submitting;             /* blk_submit calls inside ops->submit right now */
};

void blk_init(void);

/* Fill obj/name and add to the registry, which takes its own reference.
 * `prefix` such as "vd" gets the next free letter appended ("vda"). The
 * caller keeps the creator's reference and drops it with blkdev_put when
 * its own teardown is done; ops->release frees the memory when the last
 * holder (a blk_find caller, a mounted filesystem) is gone. Sleeps.
 * -EINVAL for bad geometry or a missing release, -ENOSPC when out of
 * letters. */
int blk_register(struct blkdev *bd, const char *prefix);
/* Remove from the registry, refuse new bios, wait for blk_submit calls in
 * progress to leave the driver, drop the registry's reference. On return
 * the driver may tear down its queues; bios already accepted must still
 * be completed (bio_complete with -EIO after a reset). Sleeps. */
void blk_unregister(struct blkdev *bd);

/* Validate and hand to the driver. -EINVAL (range, alignment, size,
 * buffer), -ENODEV after blk_unregister, -EROFS, or the driver's error;
 * on success `done` will run exactly once. Thread context. */
int blk_submit(struct bio *bio);

/* Driver side: finish a bio. Any context. */
void bio_complete(struct bio *bio, int status);

/* Synchronous helpers; split into max_sectors pieces. Sleep. */
int blk_read(struct blkdev *bd, uint64_t sector, uint32_t nsectors, void *buf);
int blk_write(struct blkdev *bd, uint64_t sector, uint32_t nsectors, const void *buf);
int blk_flush(struct blkdev *bd);

/* Referenced pointer or NULL. Sleeps. */
struct blkdev *blk_find(const char *name);
static inline void blkdev_get(struct blkdev *bd) { kobject_get(&bd->obj); }
static inline void blkdev_put(struct blkdev *bd) { kobject_put(&bd->obj); }
unsigned blk_count(void);
void blk_dump(void);

#endif /* KERNEL_BLK_H */
