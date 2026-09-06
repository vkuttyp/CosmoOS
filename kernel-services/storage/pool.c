/*
 * pool.c - Storage pool over one or more member block devices.
 *
 * A block is named by a DVA (member, block). Member 0 at block b is the
 * same sector as the single-member pool always addressed, so a
 * filesystem written before members existed reads back unchanged.
 */

#include <kernel/blk.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/storage.h>

/* The member and offset a DVA names, or NULL when it names neither. */
static struct spool_member *member_of(struct spool *p, uint64_t dva, uint64_t *blk)
{
    unsigned v = POOL_DVA_VDEV(dva);
    if (v >= p->nmembers)
        return NULL;
    uint64_t b = POOL_DVA_BLK(dva);
    if (b >= p->m[v].nblocks)
        return NULL;
    *blk = b;
    return &p->m[v];
}

int pool_add_member(struct spool *p, struct blkdev *bd, unsigned *vdev)
{
    if (bd == NULL || bd->sector_size == 0 || POOL_BLOCK % bd->sector_size != 0)
        return -EINVAL;
    if (p->nmembers >= POOL_MAX_MEMBERS)
        return -ENOSPC;
    unsigned v = p->nmembers;
    blkdev_get(bd);
    p->m[v].dev = bd;
    p->m[v].sectors_per_block = POOL_BLOCK / bd->sector_size;
    p->m[v].nblocks = bd->capacity / p->m[v].sectors_per_block;
    p->nmembers = v + 1;
    if (v == 0)
        p->nblocks = p->m[0].nblocks;
    if (vdev)
        *vdev = v;
    return 0;
}

int pool_open(struct blkdev *bd, struct spool **out)
{
    struct spool *p = kzalloc(sizeof(*p));
    if (p == NULL)
        return -ENOMEM;
    p->block_size = POOL_BLOCK;
    int rc = pool_add_member(p, bd, NULL);
    if (rc) {
        kfree(p);
        return rc;
    }
    *out = p;
    return 0;
}

void pool_close(struct spool *p)
{
    for (unsigned v = 0; v < p->nmembers; v++)
        blkdev_put(p->m[v].dev);
    kfree(p);
}

int pool_read(struct spool *p, uint64_t dva, void *buf)
{
    uint64_t blk;
    struct spool_member *m = member_of(p, dva, &blk);
    if (m == NULL)
        return -EINVAL;
    p->reads++;
    return blk_read(m->dev, blk * m->sectors_per_block, m->sectors_per_block, buf);
}

int pool_write(struct spool *p, uint64_t dva, const void *buf)
{
    uint64_t blk;
    struct spool_member *m = member_of(p, dva, &blk);
    if (m == NULL)
        return -EINVAL;
    p->writes++;
    return blk_write(m->dev, blk * m->sectors_per_block, m->sectors_per_block, buf);
}

int pool_write_flags(struct spool *p, uint64_t dva, const void *buf, unsigned flags)
{
    uint64_t blk;
    struct spool_member *m = member_of(p, dva, &blk);
    if (m == NULL)
        return -EINVAL;
    p->writes++;
    if (flags & BIO_PREFLUSH)
        p->flushes++;
    if (flags & BIO_FUA)
        p->flushes++;
    return blk_write_flags(m->dev, blk * m->sectors_per_block, m->sectors_per_block, buf, flags);
}

/*
 * Every member, and the first error wins: a commit whose root is durable
 * on one device and in another's cache is not durable at all.
 */
int pool_flush(struct spool *p)
{
    int first = 0;
    for (unsigned v = 0; v < p->nmembers; v++) {
        p->flushes++;
        int rc = blk_flush(p->m[v].dev);
        if (rc && first == 0)
            first = rc;
    }
    return first;
}
