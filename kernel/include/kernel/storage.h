/*
 * storage.h - The storage pool: block addressing over member devices.
 *
 * A filesystem addresses pool blocks (4 KiB), never a block device. A
 * block is named by a DVA: which member holds it, and where on that
 * member. Member 0 alone is the single-device pool every earlier format
 * assumed, which is why a version-2 or -3 filesystem needs no
 * conversion (docs/kernel-services/filesystem/cosmofs/design.md,
 * "Format version 4: many members").
 *
 * The pool knows nothing about labels or filesystems: it is an ordered
 * set of devices with a common block size. Deciding *which* devices
 * belong together is the filesystem's, because the identity is written
 * in the filesystem's own format.
 */

#ifndef KERNEL_STORAGE_H
#define KERNEL_STORAGE_H

#include <kernel/types.h>

#define POOL_BLOCK 4096u
#define POOL_MAX_MEMBERS 255u   /* the DVA's vdev field is 8 bits, 255 reserved */

/* bits 63:56 name the member, bits 55:0 the block within it. */
#define POOL_DVA_SHIFT 56
#define POOL_DVA_BLK_MASK ((1ull << POOL_DVA_SHIFT) - 1)
#define POOL_DVA(vdev, blk) (((uint64_t)(vdev) << POOL_DVA_SHIFT) | ((uint64_t)(blk) & POOL_DVA_BLK_MASK))
#define POOL_DVA_VDEV(d) ((unsigned)((uint64_t)(d) >> POOL_DVA_SHIFT))
#define POOL_DVA_BLK(d) ((uint64_t)(d) & POOL_DVA_BLK_MASK)

struct blkdev;

struct spool_member {
    struct blkdev *dev;          /* referenced */
    unsigned sectors_per_block;
    uint64_t nblocks;
};

struct spool {
    struct spool_member m[POOL_MAX_MEMBERS];
    unsigned nmembers;
    uint32_t block_size;         /* POOL_BLOCK */
    uint64_t nblocks;            /* member 0's, the only one a v2/v3 filesystem knows */
    uint64_t reads, writes, flushes;
};

/* Take a reference on bd and describe it as a one-member pool. -EINVAL
 * if the device's sector size does not divide the pool block. Sleeps. */
int pool_open(struct blkdev *bd, struct spool **out);
/* Append a member; its index is the returned vdev number. The pool takes
 * a reference. -EINVAL for a bad sector size, -ENOSPC past
 * POOL_MAX_MEMBERS. Sleeps. */
int pool_add_member(struct spool *p, struct blkdev *bd, unsigned *vdev);
void pool_close(struct spool *p);

/* One pool block, addressed by DVA; buf is DMA-able (kmalloc/dma_alloc).
 * Sleep. -EINVAL for an unknown member or past that member's end. */
int pool_read(struct spool *p, uint64_t dva, void *buf);
int pool_write(struct spool *p, uint64_t dva, const void *buf);
/* The same with bio flags (BIO_PREFLUSH, BIO_FUA): the commit's root write. */
int pool_write_flags(struct spool *p, uint64_t dva, const void *buf, unsigned flags);
/* Flush every member: a commit is durable only when all of them are. */
int pool_flush(struct spool *p);

#endif /* KERNEL_STORAGE_H */
