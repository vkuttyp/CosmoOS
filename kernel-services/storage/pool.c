/*
 * pool.c - Storage pool over one or more member block devices.
 *
 * A block is named by a DVA (member, block). A member is a mirror group
 * of one or more devices holding the same blocks at the same offsets,
 * so the address does not change when a group gains a copy. Member 0 at
 * block b is the same sector as the single-member pool always
 * addressed, so a filesystem written before members existed reads back
 * unchanged.
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

static int attach(struct spool_member *m, unsigned slot, struct blkdev *bd)
{
    if (bd == NULL || bd->sector_size == 0 || POOL_BLOCK % bd->sector_size != 0)
        return -EINVAL;
    unsigned spb = POOL_BLOCK / bd->sector_size;
    blkdev_get(bd);
    m->dev[slot] = bd;
    m->sectors_per_block[slot] = spb;
    return 0;
}

int pool_add_member(struct spool *p, struct blkdev *bd, unsigned *vdev)
{
    if (p->nmembers >= POOL_MAX_MEMBERS)
        return -ENOSPC;
    unsigned v = p->nmembers;
    int rc = attach(&p->m[v], 0, bd);
    if (rc)
        return rc;
    p->m[v].ncopies = 1;
    p->m[v].nblocks = bd->capacity / p->m[v].sectors_per_block[0];
    p->nmembers = v + 1;
    if (v == 0)
        p->nblocks = p->m[0].nblocks;
    if (vdev)
        *vdev = v;
    return 0;
}

/*
 * A copy must cover the group's blocks. A larger device is allowed and
 * its tail is unused: the group's size is decided by the member table,
 * not by whichever disk happens to be biggest.
 */
int pool_add_copy(struct spool *p, unsigned vdev, struct blkdev *bd)
{
    if (vdev >= p->nmembers)
        return -EINVAL;
    struct spool_member *m = &p->m[vdev];
    if (m->ncopies >= POOL_MAX_COPIES)
        return -ENOSPC;
    if (bd == NULL || bd->sector_size == 0 || POOL_BLOCK % bd->sector_size != 0)
        return -EINVAL;
    if (bd->capacity / (POOL_BLOCK / bd->sector_size) < m->nblocks)
        return -EINVAL;
    int rc = attach(m, m->ncopies, bd);
    if (rc)
        return rc;
    m->ncopies++;
    return 0;
}

unsigned pool_copies(const struct spool *p, uint64_t dva)
{
    unsigned v = POOL_DVA_VDEV(dva);
    return v < p->nmembers ? p->m[v].ncopies : 0;
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
        for (unsigned c = 0; c < p->m[v].ncopies; c++)
            blkdev_put(p->m[v].dev[c]);
    kfree(p);
}

int pool_read_copy(struct spool *p, uint64_t dva, unsigned copy, void *buf)
{
    uint64_t blk;
    struct spool_member *m = member_of(p, dva, &blk);
    if (m == NULL || copy >= m->ncopies)
        return -EINVAL;
    p->reads++;
    return blk_read(m->dev[copy], blk * m->sectors_per_block[copy], m->sectors_per_block[copy], buf);
}

int pool_write_copy(struct spool *p, uint64_t dva, unsigned copy, const void *buf)
{
    uint64_t blk;
    struct spool_member *m = member_of(p, dva, &blk);
    if (m == NULL || copy >= m->ncopies)
        return -EINVAL;
    p->writes++;
    return blk_write(m->dev[copy], blk * m->sectors_per_block[copy], m->sectors_per_block[copy], buf);
}

int pool_read(struct spool *p, uint64_t dva, void *buf)
{
    return pool_read_copy(p, dva, 0, buf);
}

/*
 * Every copy, and every one is attempted even after a failure: a device
 * that refuses one write is not a reason to leave the others holding an
 * older block. The first error is returned, and the caller is inside a
 * transaction that has not published a root, so a half-written mirror
 * is never something a later mount can find.
 */
int pool_write(struct spool *p, uint64_t dva, const void *buf)
{
    uint64_t blk;
    struct spool_member *m = member_of(p, dva, &blk);
    if (m == NULL)
        return -EINVAL;
    int first = 0;
    for (unsigned c = 0; c < m->ncopies; c++) {
        int rc = pool_write_copy(p, dva, c, buf);
        if (rc && first == 0)
            first = rc;
    }
    return first;
}

int pool_write_flags(struct spool *p, uint64_t dva, const void *buf, unsigned flags)
{
    uint64_t blk;
    struct spool_member *m = member_of(p, dva, &blk);
    if (m == NULL)
        return -EINVAL;
    int first = 0;
    for (unsigned c = 0; c < m->ncopies; c++) {
        p->writes++;
        if (flags & BIO_PREFLUSH)
            p->flushes++;
        if (flags & BIO_FUA)
            p->flushes++;
        int rc = blk_write_flags(m->dev[c], blk * m->sectors_per_block[c], m->sectors_per_block[c], buf, flags);
        if (rc && first == 0)
            first = rc;
    }
    return first;
}

/*
 * Every device of every member, and the first error wins: a commit
 * durable on one device and in another's cache is not durable at all.
 */
int pool_flush(struct spool *p)
{
    int first = 0;
    for (unsigned v = 0; v < p->nmembers; v++) {
        for (unsigned c = 0; c < p->m[v].ncopies; c++) {
            p->flushes++;
            int rc = blk_flush(p->m[v].dev[c]);
            if (rc && first == 0)
                first = rc;
        }
    }
    return first;
}
