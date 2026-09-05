/*
 * storage.h - The storage pool: block addressing over member devices.
 *
 * A filesystem addresses pool blocks (4 KiB), never a block device.
 * One member device in this phase; allocation groups, redundancy and a
 * member table attach here later (constitution section 29).
 */

#ifndef KERNEL_STORAGE_H
#define KERNEL_STORAGE_H

#include <kernel/types.h>

#define POOL_BLOCK 4096u

struct blkdev;

struct spool {
    struct blkdev *dev;          /* referenced */
    uint32_t block_size;         /* POOL_BLOCK */
    unsigned sectors_per_block;
    uint64_t nblocks;
    uint64_t reads, writes, flushes;
};

/* Take a reference on bd and describe it as a pool. -EINVAL if the
 * device's sector size does not divide the pool block. Sleeps. */
int pool_open(struct blkdev *bd, struct spool **out);
void pool_close(struct spool *p);

/* One pool block; buf is DMA-able (kmalloc/dma_alloc). Sleep. -EINVAL
 * past the end. */
int pool_read(struct spool *p, uint64_t blk, void *buf);
int pool_write(struct spool *p, uint64_t blk, const void *buf);
/* The same with bio flags (BIO_PREFLUSH, BIO_FUA): the commit's root write. */
int pool_write_flags(struct spool *p, uint64_t blk, const void *buf, unsigned flags);
int pool_flush(struct spool *p);

#endif /* KERNEL_STORAGE_H */
