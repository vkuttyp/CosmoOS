/*
 * pool.c - Single-member storage pool over the block layer.
 */

#include <kernel/blk.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/storage.h>

int pool_open(struct blkdev *bd, struct spool **out)
{
    if (bd == NULL || bd->sector_size == 0 || POOL_BLOCK % bd->sector_size != 0)
        return -EINVAL;
    struct spool *p = kzalloc(sizeof(*p));
    if (p == NULL)
        return -ENOMEM;
    blkdev_get(bd);
    p->dev = bd;
    p->block_size = POOL_BLOCK;
    p->sectors_per_block = POOL_BLOCK / bd->sector_size;
    p->nblocks = bd->capacity / p->sectors_per_block;
    *out = p;
    return 0;
}

void pool_close(struct spool *p)
{
    blkdev_put(p->dev);
    kfree(p);
}

int pool_read(struct spool *p, uint64_t blk, void *buf)
{
    if (blk >= p->nblocks)
        return -EINVAL;
    p->reads++;
    return blk_read(p->dev, blk * p->sectors_per_block, p->sectors_per_block, buf);
}

int pool_write(struct spool *p, uint64_t blk, const void *buf)
{
    if (blk >= p->nblocks)
        return -EINVAL;
    p->writes++;
    return blk_write(p->dev, blk * p->sectors_per_block, p->sectors_per_block, buf);
}

int pool_flush(struct spool *p)
{
    p->flushes++;
    return blk_flush(p->dev);
}
