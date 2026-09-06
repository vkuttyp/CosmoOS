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

int cfs_label_write(struct spool *pool, unsigned vdev, unsigned copy, uint64_t generation, const uint8_t uuid[16],
                    uint64_t nblocks)
{
    uint8_t *block = kmalloc(CFS_BLOCK, KMEM_ZERO);
    if (block == NULL)
        return -ENOMEM;
    struct cfs_label *l = (struct cfs_label *)block;
    memcpy(l->magic, CFS_LABEL_MAGIC, sizeof(l->magic));
    l->version = CFS_VERSION;
    l->index = vdev;
    l->copy = copy;
    l->generation = generation;
    memcpy(l->uuid, uuid, 16);
    l->nblocks = nblocks;
    l->crc = label_crc(block);
    /* To that copy alone: every device of a group carries its own. */
    int rc = pool_write_copy(pool, CFS_DVA(vdev, 0), copy, block);
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
            l->index >= CFS_MAX_MEMBERS || l->copy >= CFS_MAX_COPIES)
            rc = -ENOENT;
        else
            *out = *l;
    }
    kfree(block);
    pool_close(p);
    return rc;
}

/*
 * A device is current if its label names the generation being mounted --
 * or the one after it. Labels are stamped after a commit's blocks are
 * stable and before the root that publishes them, so a commit
 * interrupted between the two leaves labels one ahead of the durable
 * root. Those devices did take part in everything that root names; a
 * device that *missed* a commit is the one that is behind, and treating
 * an interrupted commit as staleness would degrade a healthy mirror for
 * nothing.
 */
static bool label_current(const struct cfs *fs, const struct cfs_label *l)
{
    return l->generation >= fs->sb.generation;
}

/* True if `bd` carries a current copy of member `vdev`. `stale` says it
 * is ours and behind; `wrong_size` says it is this pool's member and the
 * wrong disk, which is a different thing from the member being absent
 * and is worth a different answer. */
static bool copy_here(struct cfs *fs, struct blkdev *bd, unsigned vdev, const struct cfs_member *want, bool *stale,
                      bool *wrong_size)
{
    struct cfs_label l;
    *stale = false;
    *wrong_size = false;
    if (label_read(bd, &l) != 0 || l.index != vdev || memcmp(l.uuid, fs->sb.uuid, 16) != 0)
        return false;
    if (l.nblocks != want->nblocks) {
        kerror("cosmofs: %s carries member %u but %llu blocks, not %llu", bd->name, vdev,
               (unsigned long long)l.nblocks, (unsigned long long)want->nblocks);
        *wrong_size = true;
        return false;
    }
    if (!label_current(fs, &l)) {
        kwarn("cosmofs: %s last took part in generation %llu, not %llu; not mirroring it", bd->name,
              (unsigned long long)l.generation, (unsigned long long)fs->sb.generation);
        *stale = true;
        return false;
    }
    return true;
}

/*
 * Assemble member `vdev`: the first *current* device found becomes the
 * pool's next member (members are added in order, so the pool's
 * numbering is the table's). Which copy it was labelled is not the
 * question -- copies of a mirror are interchangeable, and preferring
 * the one labelled 0 would serve a stale disk in preference to a good
 * one, which is precisely what the generation is recorded to prevent.
 */
static int assemble_member(struct cfs *fs, unsigned vdev, const struct cfs_member *want)
{
    for (unsigned i = 0;; i++) {
        struct blkdev *bd = blk_nth(i);
        if (bd == NULL)
            break;
        bool stale, wrong_size;
        if (!copy_here(fs, bd, vdev, want, &stale, &wrong_size)) {
            blkdev_put(bd);
            if (wrong_size)
                return -EIO;   /* this pool's member, and the wrong disk */
            (void)stale;       /* counted once at the end, from what the pool has */
            continue;
        }
        unsigned got;
        int rc = pool_add_member(fs->pool, bd, &got);
        blkdev_put(bd);
        if (rc)
            return rc;
        if (got != vdev)
            return -EIO;   /* the caller assembles in order; this cannot happen */
        return 0;
    }
    kerror("cosmofs: member %u of the pool has no current device", vdev);
    return -ENODEV;
}

/* Member 0's devices carry the superblock rather than a label -- block 0
 * is the superblock -- so they are recognised by it: the pool's uuid,
 * and a generation that says this device did not miss the commit the
 * mount is coming up on. */
static bool is_member0_copy(struct cfs *fs, struct blkdev *bd)
{
    struct spool *p;
    if (pool_open(bd, &p))
        return false;
    uint8_t *block = kmalloc(CFS_BLOCK, KMEM_ZERO);
    if (block == NULL) {
        pool_close(p);
        return false;
    }
    uint64_t newest = 0;
    for (unsigned slot = 0; slot < 2; slot++) {
        if (pool_read(p, CFS_DVA(0, slot), block) != 0)
            continue;
        const struct cfs_super *sb = (const struct cfs_super *)block;
        if (memcmp(sb->magic, CFS_MAGIC, 8) != 0 || memcmp(sb->uuid, fs->sb.uuid, 16) != 0)
            continue;
        if (sb->generation > newest)
            newest = sb->generation;
    }
    kfree(block);
    pool_close(p);
    /* Same rule as a label, and the same reason: never older than the
     * root being mounted. Member 0 cannot do better than this, because
     * its root *is* the block a commit publishes -- a copy that missed
     * only that write is indistinguishable from one that was detached
     * for a whole commit, and promoting the wrong one would serve old
     * blocks. It stays out until something resilvers it. */
    return newest >= fs->sb.generation;
}

/*
 * Attach a member's other copies. A missing or stale copy is not a
 * missing member: the group still has one, so the pool comes up
 * degraded and says so rather than refusing to mount. That is the whole
 * point of keeping a second copy -- and refusing a stale one is the
 * point of the generation in its label, because its blocks would pass
 * every checksum and still be the wrong contents.
 */
static void attach_copies(struct cfs *fs, unsigned vdev, const struct cfs_member *want, unsigned copies)
{
    for (unsigned c = 1; c < copies; c++) {
        struct blkdev *bd = NULL;
        if (vdev == 0) {
            for (unsigned i = 0; bd == NULL; i++) {
                struct blkdev *cand = blk_nth(i);
                if (cand == NULL)
                    break;
                bool already = false;
                for (unsigned k = 0; k < fs->pool->m[0].ncopies; k++)
                    already = already || fs->pool->m[0].dev[k] == cand;
                if (!already && is_member0_copy(fs, cand))
                    bd = cand;   /* keep the reference */
                else
                    blkdev_put(cand);
            }
        } else {
            /* Any current device of this member that the pool does not
             * already hold; the label's copy number is how devices are
             * told apart, not an order to be honoured. */
            for (unsigned i = 0; bd == NULL; i++) {
                struct blkdev *cand = blk_nth(i);
                if (cand == NULL)
                    break;
                bool already = false;
                for (unsigned k = 0; k < fs->pool->m[vdev].ncopies; k++)
                    already = already || fs->pool->m[vdev].dev[k] == cand;
                bool stale, wrong_size;
                if (!already && copy_here(fs, cand, vdev, want, &stale, &wrong_size))
                    bd = cand;   /* keep the reference */
                else
                    blkdev_put(cand);
            }
        }
        if (bd == NULL) {
            kwarn("cosmofs: member %u copy %u is missing or stale; the pool is degraded", vdev, c);
            continue;
        }
        int rc = pool_add_copy(fs->pool, vdev, bd);
        blkdev_put(bd);
        if (rc)
            kwarn("cosmofs: member %u copy %u could not be attached (%d); the pool is degraded", vdev, c, rc);
    }
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
    if (m->copies > CFS_MAX_COPIES)
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

int cfs_members_load(struct cfs *fs)
{
    /* Member 0's first device is the one the mount was given. */
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
        m->copies = src->copies ? src->copies : 1;   /* version 4 wrote 0 and meant one */
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
        if (m->copies > 1)
            attach_copies(fs, v, src, m->copies);
    }
    kfree(block);
    if (rc) {
        kfree(fs->mem);
        fs->mem = NULL;
        fs->nmembers = 0;
        return rc;
    }
    layout(fs);
    /* What the table promised, less what the pool actually has: counting
     * as devices are passed over instead would count the same missing
     * copy twice, once where it was rejected and once where its slot
     * went unfilled. */
    fs->degraded = 0;
    for (unsigned v = 0; v < fs->nmembers; v++) {
        unsigned have = pool_copies(fs->pool, CFS_DVA(v, 0));
        if (fs->mem[v].copies > have)
            fs->degraded += fs->mem[v].copies - have;
    }
    if (fs->nmembers > 1 || fs->mem[0].copies > 1) {
        unsigned mirrored = 0;
        for (unsigned v = 0; v < fs->nmembers; v++)
            mirrored += pool_copies(fs->pool, CFS_DVA(v, 0));
        kinfo("cosmofs: pool of %u members over %u device(s), %llu blocks%s", fs->nmembers, mirrored,
              (unsigned long long)fs->sb.total_blocks, fs->degraded ? " (degraded)" : "");
    }
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
        dst->copies = m->copies;
    }
    cfs_buf_mark_dirty(fs, b);
    cfs_buf_put(fs, b);
    return 0;
}

/*
 * Stamp every attached copy of every member past the first with this
 * generation. Called from the commit after every block of the
 * transaction is stable and before the root is written, so a label is
 * never newer than the root it belongs to: a device that misses a
 * commit keeps the older number and is refused at the next mount.
 *
 * Member 0 needs none of this -- its devices carry the superblock
 * itself, whose generation is the same evidence.
 */
int cfs_labels_update(struct cfs *fs)
{
    if (fs->sb.version < 5)
        return 0;
    for (unsigned v = 1; v < fs->nmembers; v++) {
        unsigned n = pool_copies(fs->pool, CFS_DVA(v, 0));
        for (unsigned c = 0; c < n; c++) {
            int rc = cfs_label_write(fs->pool, v, c, fs->gen, fs->sb.uuid, fs->mem[v].nblocks);
            if (rc)
                return rc;
        }
    }
    return 0;
}

/*
 * One block, from whichever copy of its member can satisfy `verify`.
 * Copy 0 first, because that is where a healthy pool answers from; then
 * the rest, and the first that verifies is written back over the ones
 * that did not. A mirror that is read without checking only doubles the
 * chance of returning something wrong.
 */
int cfs_read_repair(struct cfs *fs, uint64_t dva, void *buf, bool (*verify)(const void *blk, void *arg), void *arg,
                    bool *repaired)
{
    if (repaired)
        *repaired = false;
    unsigned copies = pool_copies(fs->pool, dva);
    if (copies == 0)
        return -EINVAL;
    bool bad[POOL_MAX_COPIES] = { false };
    int first_err = 0;
    for (unsigned c = 0; c < copies; c++) {
        int rc = pool_read_copy(fs->pool, dva, c, buf);
        if (rc == 0 && verify(buf, arg)) {
            if (c == 0)
                return 0;   /* the common case: nothing to repair */
            /* Write this copy back over every copy that failed. */
            unsigned fixed = 0;
            for (unsigned b = 0; b < c; b++) {
                if (!bad[b])
                    continue;
                if (pool_write_copy(fs->pool, dva, b, buf) == 0)
                    fixed++;
            }
            kwarn("cosmofs: block %llu:%llu recovered from copy %u (%u repaired)",
                  (unsigned long long)CFS_DVA_VDEV(dva), (unsigned long long)CFS_DVA_BLK(dva), c, fixed);
            fs->repairs += fixed;
            if (repaired)
                *repaired = fixed > 0;
            return 0;
        }
        bad[c] = true;
        if (first_err == 0)
            first_err = rc ? rc : -EIO;
    }
    return first_err;
}

/*
 * Every copy, checked. A read stops at the first copy that verifies,
 * because that is the answer; a scrub cannot, because the copy that
 * verifies may be hiding a rotted one behind it that nobody will notice
 * until it is the one answering.
 */
int cfs_verify_all(struct cfs *fs, uint64_t dva, void *buf, bool (*verify)(const void *blk, void *arg), void *arg,
                   unsigned *repaired)
{
    if (repaired)
        *repaired = 0;
    unsigned copies = pool_copies(fs->pool, dva);
    if (copies == 0)
        return -EINVAL;
    bool bad[POOL_MAX_COPIES] = { false };
    unsigned nbad = 0;
    int good = -1;
    uint8_t *scratch = kmalloc(CFS_BLOCK, 0);
    if (scratch == NULL)
        return -ENOMEM;
    for (unsigned c = 0; c < copies; c++) {
        int rc = pool_read_copy(fs->pool, dva, c, scratch);
        if (rc == 0 && verify(scratch, arg)) {
            if (good < 0) {
                good = (int)c;
                memcpy(buf, scratch, CFS_BLOCK);
            }
            continue;
        }
        bad[c] = true;
        nbad++;
    }
    int rc = 0;
    if (good < 0) {
        rc = -EIO;
    } else if (nbad) {
        unsigned fixed = 0;
        for (unsigned c = 0; c < copies; c++)
            if (bad[c] && pool_write_copy(fs->pool, dva, c, buf) == 0)
                fixed++;
        kwarn("cosmofs: block %llu:%llu: %u of %u copies rotted, %u repaired from copy %d",
              (unsigned long long)CFS_DVA_VDEV(dva), (unsigned long long)CFS_DVA_BLK(dva), nbad, copies, fixed, good);
        fs->repairs += fixed;
        if (repaired)
            *repaired = fixed;
    }
    kfree(scratch);
    return rc;
}

void cfs_members_free(struct cfs *fs)
{
    kfree(fs->mem);
    fs->mem = NULL;
    fs->nmembers = 0;
}
