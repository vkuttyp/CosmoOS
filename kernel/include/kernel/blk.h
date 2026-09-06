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
#include <kernel/spinlock.h>
#include <kernel/types.h>

struct device;
struct blkdev;

enum bio_dir {
    BIO_READ,
    BIO_WRITE,
    BIO_FLUSH,   /* no data; sector and nsectors are 0 */
};

/* bio flags (docs/kernel-services/filesystem/cosmofs/design.md, "The block
 * layer"): implemented by the block layer as a flush before / after the
 * write, so drivers never see them. */
#define BIO_PREFLUSH (1u << 0)   /* flush the volatile cache before this write */
#define BIO_FUA      (1u << 1)   /* the write is on stable media when it completes */

/* One segment of a multi-segment bio (docs/kernel/device/design.md, "The
 * block layer for NVMe"): every segment but the first starts on a page
 * boundary, every segment but the last ends on one, each is DMA-able. */
struct bio_vec {
    void *buf;
    uint32_t len;
};

struct bio {
    struct blkdev *dev;
    uint64_t sector;
    uint32_t nsectors;
    enum bio_dir dir;
    unsigned flags;                  /* BIO_PREFLUSH, BIO_FUA (writes only) */
    void *buf;                       /* the single segment when nr_vecs == 0 */
    struct bio_vec *vecs;            /* else the segments, in transfer order */
    unsigned nr_vecs;
    void (*done)(struct bio *bio);   /* may run in interrupt context */
    void *arg;
    int status;                      /* 0 or -errno once done ran */
    struct list_node link;           /* for the driver's queue */
    void *drvpriv;                   /* for the driver */
    struct list_node inflight_link;  /* the block layer's in-flight list */
    uint64_t issued_ns;              /* when the driver accepted it */
    unsigned issue_cpu;              /* the CPU that handed it to the driver */
};

/* Segment `i` of a bio (0 .. bio_segments(bio) - 1), in either shape. */
static inline unsigned bio_segments(const struct bio *bio) { return bio->nr_vecs ? bio->nr_vecs : 1u; }
void bio_segment(const struct bio *bio, unsigned i, struct bio_vec *out);

#define BLK_TIMEOUT_NS (30ull * 1000000000ull)   /* default request timeout */

struct blkdev_ops {
    /* Take ownership of the bio until bio_complete(). Returns 0 or a
     * negative errno without completing it. Thread context. */
    int (*submit)(struct blkdev *dev, struct bio *bio);
    /* Mandatory: the last reference dropped (after blk_unregister);
     * free the memory the blkdev is embedded in. */
    void (*release)(struct blkdev *dev);
    /* Optional: `bio` has been in flight longer than dev->timeout_ns.
     * Thread context (the blk-timeout thread). Make the device forget the
     * request, then complete it (-ETIMEDOUT) through bio_complete as
     * usual. Without it the layer only warns and counts. */
    void (*timeout)(struct blkdev *dev, struct bio *bio);
    /* Optional, tests only: make the device DMA into `addr` (a device
     * address the caller chose, typically one no IOMMU mapping covers)
     * with a harmless command and return the errno of its status. Used
     * to provoke a translation fault on purpose. Returns the errno of the
     * command's status, which may be 0: a device is free not to notice that
     * its own DMA was dropped. Thread context. */
    int (*debug_dma)(struct blkdev *dev, uint64_t addr);
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
    unsigned max_segments;           /* per bio; 0 means 1 (single-buffer drivers) */
    uint64_t timeout_ns;             /* per request; 0 at registration takes BLK_TIMEOUT_NS, UINT64_MAX disables */
    uint64_t reads, writes, flushes, errors;   /* completed bios */
    uint64_t timeouts;               /* bios the timeout thread reported */
    uint64_t completed_local, completed_remote;   /* completions on the issuing CPU / elsewhere (queue locality) */
    unsigned nr_queues;              /* informational: hardware queues the driver uses (1 when it says nothing) */
    bool gone;                       /* unregistered: blk_submit refuses (-ENODEV) */
    uint32_t submitting;             /* blk_submit calls inside ops->submit right now */
    struct list_node pending;        /* bios the driver refused with -EAGAIN, resubmitted in order */
    struct list_node inflight;       /* bios the driver holds, oldest first */
    spinlock_t qlock;                /* the pending and in-flight lists */
    uint64_t requeued;               /* bios that waited in `pending` */
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
/* Same, under exactly `name` ("nvme0n1"); -EEXIST when taken. */
int blk_register_named(struct blkdev *bd, const char *name);
/* Remove from the registry, refuse new bios, wait for blk_submit calls in
 * progress to leave the driver, drop the registry's reference. On return
 * the driver may tear down its queues; bios already accepted must still
 * be completed (bio_complete with -EIO after a reset). Sleeps. */
void blk_unregister(struct blkdev *bd);

/* Validate and hand to the driver. -EINVAL (range, alignment, size,
 * buffer), -ENODEV after blk_unregister, -EROFS, or the driver's error;
 * on success `done` will run exactly once. A driver that answers -EAGAIN
 * (its queue is full) is not an error: the bio waits in the device's
 * pending list and is resubmitted as completions free slots. A write
 * with BIO_PREFLUSH/BIO_FUA becomes flush, write, flush; the caller's
 * `done` runs once, after the last piece. Thread context. */
int blk_submit(struct bio *bio);

/* Driver side: finish a bio. Any context. */
void bio_complete(struct bio *bio, int status);

/* Synchronous helpers; split into max_sectors pieces. Sleep. */
int blk_read(struct blkdev *bd, uint64_t sector, uint32_t nsectors, void *buf);
int blk_write(struct blkdev *bd, uint64_t sector, uint32_t nsectors, const void *buf);
int blk_write_flags(struct blkdev *bd, uint64_t sector, uint32_t nsectors, const void *buf, unsigned flags);
int blk_flush(struct blkdev *bd);

/* Referenced pointer or NULL. Sleeps. */
struct blkdev *blk_find(const char *name);
static inline void blkdev_get(struct blkdev *bd) { kobject_get(&bd->obj); }
static inline void blkdev_put(struct blkdev *bd) { kobject_put(&bd->obj); }
/* The i'th registered device, referenced, or NULL past the end; for
 * enumeration that tolerates a changing registry (pool assembly). */
struct blkdev *blk_nth(unsigned i);
unsigned blk_count(void);
void blk_dump(void);

#endif /* KERNEL_BLK_H */
