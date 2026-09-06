/*
 * cosmofs_member.c - The pool's members, and the addresses that name them
 * (docs/kernel-services/filesystem/cosmofs/design.md, "Format version 4:
 * many members").
 *
 * Every pointer on disk is a DVA: a member and a block on it. This file
 * owns the three things that follow from that -- the table that says
 * which members exist, the labels that let a mount find their devices,
 * and the conversion between a DVA and the linear index the allocator's
 * one flat bitmap is built on.
 */

#include <kernel/blk.h>
#include <kernel/crc32c.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/random.h>
#include <kernel/string.h>

#include "cosmofs_internal.h"

/* --- addresses ----------------------------------------------------------- */

bool cfs_dva_valid(const struct cfs *fs, uint64_t dva)
{
    unsigned v = CFS_DVA_VDEV(dva);
    if (v >= fs->nmembers)
        return false;
    uint64_t b = CFS_DVA_BLK(dva);
    return b >= fs->mem[v].first_usable && b < fs->mem[v].nblocks;
}

uint64_t cfs_dva_lin(const struct cfs *fs, uint64_t dva)
{
    unsigned v = CFS_DVA_VDEV(dva);
    if (v >= fs->nmembers)
        return CFS_DVA_NONE;
    uint64_t b = CFS_DVA_BLK(dva);
    if (b >= fs->mem[v].nblocks)
        return CFS_DVA_NONE;
    return fs->mem[v].base + b;
}

uint64_t cfs_lin_dva(const struct cfs *fs, uint64_t lin)
{
    for (unsigned v = 0; v < fs->nmembers; v++) {
        const struct cfs_memstate *m = &fs->mem[v];
        if (lin < m->base)
            break;
        if (lin < m->base + m->nblocks)
            return CFS_DVA(v, lin - m->base);
        /* Between a member's last block and the end of its last bitmap
         * chunk is padding: an address with no block behind it. */
        if (lin < m->base + (uint64_t)m->nchunks * CFS_BITS_PER_BITMAP)
            return CFS_DVA_NONE;
    }
    return CFS_DVA_NONE;
}

/* --- labels -------------------------------------------------------------- */

static uint32_t label_crc(const uint8_t *block)
{
    static const uint8_t zero4[4] = { 0 };
    size_t off = offsetof(struct cfs_label, crc);
    uint32_t c = crc32c(block, off);
    c = crc32c_update(c, zero4, 4);
    return crc32c_update(c, block + off + 4, CFS_BLOCK - off - 4);
}

int cfs_label_write(struct spool *pool, unsigned vdev, const uint8_t uuid[16], uint64_t nblocks)
{
    uint8_t *block = kmalloc(CFS_BLOCK, KMEM_ZERO);
    if (block == NULL)
        return -ENOMEM;
    struct cfs_label *l = (struct cfs_label *)block;
    memcpy(l->magic, CFS_LABEL_MAGIC, sizeof(l->magic));
    l->version = CFS_VERSION;
    l->index = vdev;
    memcpy(l->uuid, uuid, 16);
    l->nblocks = nblocks;
    l->crc = label_crc(block);
    int rc = pool_write(pool, CFS_DVA(vdev, 0), block);
    kfree(block);
    return rc;
}

/* The label at block 0 of `bd`, or -ENOENT when there is none. A device
 * that is not a member of anything reads as random data here, so every
 * field is checked before it is believed. */
static int label_read(struct blkdev *bd, struct cfs_label *out)
{
    struct spool *p;
    int rc = pool_open(bd, &p);
    if (rc)
        return rc;
    uint8_t *block = kmalloc(CFS_BLOCK, KMEM_ZERO);
    if (block == NULL) {
        pool_close(p);
        return -ENOMEM;
    }
    rc = pool_read(p, CFS_DVA(0, 0), block);
    if (rc == 0) {
        const struct cfs_label *l = (const struct cfs_label *)block;
        if (memcmp(l->magic, CFS_LABEL_MAGIC, sizeof(l->magic)) != 0 || label_crc(block) != l->crc ||
            l->index == 0 || l->index >= CFS_MAX_MEMBERS)
            rc = -ENOENT;
        else
            *out = *l;
    }
    kfree(block);
    pool_close(p);
    return rc;
}

/*
 * Find the device carrying member `vdev` of this pool and append it.
 * Members are added in order, so the pool's vdev numbering is the
 * table's. A device whose label matches the uuid but not the recorded
 * size is refused rather than used: it is the right pool's member and
 * the wrong disk.
 */
static int assemble_member(struct cfs *fs, unsigned vdev, const struct cfs_member *want)
{
    for (unsigned i = 0;; i++) {
        struct blkdev *bd = blk_nth(i);
        if (bd == NULL)
            break;
        struct cfs_label l;
        int rc = label_read(bd, &l);
        if (rc == 0 && l.index == vdev && memcmp(l.uuid, fs->sb.uuid, 16) == 0) {
            if (l.nblocks != want->nblocks) {
                kerror("cosmofs: %s carries member %u but %llu blocks, not %llu", bd->name, vdev,
                       (unsigned long long)l.nblocks, (unsigned long long)want->nblocks);
                blkdev_put(bd);
                return -EIO;
            }
            unsigned got;
            rc = pool_add_member(fs->pool, bd, &got);
            blkdev_put(bd);
            if (rc)
                return rc;
            if (got != vdev)
                return -EIO;   /* the caller assembles in order; this cannot happen */
            return 0;
        }
        blkdev_put(bd);
    }
    kerror("cosmofs: member %u of the pool is missing", vdev);
    return -ENODEV;
}

/* --- the member table ---------------------------------------------------- */

/*
 * A member table is disk data. Its geometry decides how many bitmap
 * chunks are read and how far the allocator may reach, so every field
 * is checked against what the format can express before any of it is
 * believed -- the formatter's limits are not evidence about a disk this
 * kernel did not write.
 */
static bool member_sane(const struct cfs_member *m, unsigned v, unsigned nmembers)
{
    if (m->nblocks < CFS_MIN_BLOCKS || m->nblocks > CFS_MAX_BLOCKS)
        return false;
    /* One allocation index holds CFS_PTRS_PER_BLOCK chunk pointers, so a
     * member that claims more chunks than that would be read past the
     * end of its index block. */
    uint64_t chunks = (m->nblocks + CFS_BITS_PER_BITMAP - 1) / CFS_BITS_PER_BITMAP;
    if (chunks > CFS_PTRS_PER_BLOCK)
        return false;
    uint64_t first = v == 0 ? 2 : 1;   /* the superblocks, or the label */
    if (m->first_usable < first || m->first_usable >= m->nblocks)
        return false;
    /* A member's allocation index is normally on that member (that is
     * what the copy-on-write hint arranges), but a member with no free
     * block at all leaves its replacement elsewhere rather than wedging
     * the filesystem, so what is checked here is only that the address
     * could name a block of this pool. */
    if (CFS_DVA_VDEV(m->alloc_root) >= nmembers)
        return false;
    return m->free_blocks <= m->nblocks;
}

/* Lay the members out in the linear space the bitmap is indexed by:
 * each member starts at a whole chunk, so no chunk straddles two. */
static void layout(struct cfs *fs)
{
    uint64_t base = 0;
    unsigned chunk = 0;
    for (unsigned v = 0; v < fs->nmembers; v++) {
        struct cfs_memstate *m = &fs->mem[v];
        m->base = base;
        m->chunk0 = chunk;
        m->nchunks = (unsigned)((m->nblocks + CFS_BITS_PER_BITMAP - 1) / CFS_BITS_PER_BITMAP);
        base += (uint64_t)m->nchunks * CFS_BITS_PER_BITMAP;
        chunk += m->nchunks;
    }
    fs->nblocks = base;
    fs->nr_chunks = chunk;
}

int cfs_members_load(struct cfs *fs, struct blkdev *first)
{
    (void)first;   /* member 0 is the device the mount was given */
    if (fs->sb.version < 4) {
        /* One member, and its allocation root is where it always was. */
        fs->mem = kzalloc(sizeof(*fs->mem));
        if (fs->mem == NULL)
            return -ENOMEM;
        fs->nmembers = 1;
        fs->mem[0].nblocks = fs->sb.total_blocks;
        fs->mem[0].first_usable = 2;
        fs->mem[0].alloc_root = fs->sb.alloc_root;
        layout(fs);
        return 0;
    }

    uint8_t *block = kmalloc(CFS_BLOCK, KMEM_ZERO);
    if (block == NULL)
        return -ENOMEM;
    int rc = pool_read(fs->pool, fs->sb.members, block);
    if (rc == 0) {
        const struct cfs_mhdr *h = (const struct cfs_mhdr *)block;
        if (h->magic != CFS_MHDR_MAGIC || h->kind != CFS_KIND_MEMBERS || h->blkno != fs->sb.members)
            rc = -EIO;
    }
    if (rc) {
        kfree(block);
        kerror("cosmofs: the member table is unreadable");
        return rc;
    }
    const struct cfs_member_block *mb = (const struct cfs_member_block *)(block + CFS_MHDR_SIZE);
    uint64_t count = mb->count;
    if (count == 0 || count > CFS_MEMBERS_PER_BLOCK || count > CFS_MAX_MEMBERS) {
        kfree(block);
        return -EIO;
    }
    fs->mem = kzalloc((size_t)count * sizeof(*fs->mem));
    if (fs->mem == NULL) {
        kfree(block);
        return -ENOMEM;
    }
    fs->nmembers = (unsigned)count;
    for (unsigned v = 0; v < fs->nmembers; v++) {
        const struct cfs_member *src = &mb->m[v];
        struct cfs_memstate *m = &fs->mem[v];
        if (!member_sane(src, v, fs->nmembers)) {
            kerror("cosmofs: member %u of the table is not a member this format can describe", v);
            rc = -EIO;
            break;
        }
        m->nblocks = src->nblocks;
        m->first_usable = src->first_usable;
        m->alloc_root = src->alloc_root;
        m->free_blocks = src->free_blocks;
        memcpy(m->uuid, src->uuid, 16);
        if (v > 0) {
            rc = assemble_member(fs, v, src);
            if (rc)
                break;
        }
        /* The table says how big the member is; the device says how big
         * it really is, and the smaller of those is not negotiable. */
        if (rc == 0 && m->nblocks > fs->pool->m[v].nblocks) {
            kerror("cosmofs: member %u claims %llu blocks, the device has %llu", v,
                   (unsigned long long)m->nblocks, (unsigned long long)fs->pool->m[v].nblocks);
            rc = -EIO;
        }
        if (rc)
            break;
    }
    kfree(block);
    if (rc) {
        kfree(fs->mem);
        fs->mem = NULL;
        fs->nmembers = 0;
        return rc;
    }
    layout(fs);
    if (fs->nmembers > 1)
        kinfo("cosmofs: pool of %u members, %llu blocks", fs->nmembers, (unsigned long long)fs->nblocks);
    return 0;
}

int cfs_members_store(struct cfs *fs)
{
    if (fs->sb.version < 4) {
        /* Nothing to store: the one member's root is the superblock's,
         * which the commit writes anyway. */
        fs->sb.alloc_root = fs->mem[0].alloc_root;
        return 0;
    }
    struct cfs_buf *b;
    int rc = cfs_buf_get(fs, fs->sb.members, CFS_KIND_MEMBERS, &b);
    if (rc)
        return rc;
    rc = cfs_buf_cow(fs, &b, &fs->sb.members);
    if (rc) {
        cfs_buf_put(fs, b);
        return rc;
    }
    struct cfs_member_block *mb = (struct cfs_member_block *)(b->data + CFS_MHDR_SIZE);
    mb->count = fs->nmembers;
    for (unsigned v = 0; v < fs->nmembers; v++) {
        struct cfs_member *dst = &mb->m[v];
        const struct cfs_memstate *m = &fs->mem[v];
        memcpy(dst->uuid, m->uuid, 16);
        dst->nblocks = m->nblocks;
        dst->first_usable = m->first_usable;
        dst->alloc_root = m->alloc_root;
        dst->free_blocks = m->free_blocks;
    }
    cfs_buf_mark_dirty(fs, b);
    cfs_buf_put(fs, b);
    return 0;
}

void cfs_members_free(struct cfs *fs)
{
    kfree(fs->mem);
    fs->mem = NULL;
    fs->nmembers = 0;
}
