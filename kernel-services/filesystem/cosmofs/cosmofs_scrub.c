/*
 * cosmofs_scrub.c - Read everything, before somebody needs it
 * (docs/kernel-services/filesystem/cosmofs/design.md, "Scrub").
 *
 * Repair on read only finds what somebody reads. A scrub reads the whole
 * filesystem through exactly the same path -- metadata by its own
 * header, data by the checksum its inode records -- so a copy that has
 * rotted is found and put right while there is still a good one to put
 * it right from. Nothing here has its own idea of what a correct block
 * is; that would be a second implementation to keep in step.
 */

#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/string.h>

#include "cosmofs_internal.h"

struct meta_want {
    uint64_t dva;
    uint32_t kind;
};

static bool meta_ok(const void *block, void *arg)
{
    const struct meta_want *w = arg;
    return cfs_mhdr_ok(block, w->dva, w->kind);
}

/* One metadata block, every copy of it. */
static void scrub_meta(struct cfs *fs, uint64_t dva, uint32_t kind, uint8_t *page,
                       struct cosmofs_scrub_stats *st)
{
    if (dva == 0 || !cfs_dva_valid(fs, dva))
        return;
    struct meta_want w = { .dva = dva, .kind = kind };
    unsigned fixed = 0;
    int rc = cfs_verify_all(fs, dva, page, meta_ok, &w, &fixed);
    st->blocks_read++;
    if (rc)
        st->unrecoverable++;
    else
        st->repaired += fixed;
}

/* Every allocation index and bitmap block: the map of what is in use is
 * the one thing whose loss costs the whole filesystem. */
static void scrub_alloc(struct cfs *fs, uint8_t *page, struct cosmofs_scrub_stats *st)
{
    for (unsigned v = 0; v < fs->nmembers; v++) {
        uint64_t root = fs->mem[v].alloc_root;
        scrub_meta(fs, root, CFS_KIND_ALLOCIDX, page, st);
        struct cfs_buf *idx;
        if (cfs_buf_get(fs, root, CFS_KIND_ALLOCIDX, &idx))
            continue;
        const uint64_t *slots = (const uint64_t *)(idx->data + CFS_MHDR_SIZE);
        for (unsigned c = 0; c < fs->mem[v].nchunks; c++)
            scrub_meta(fs, slots[c], CFS_KIND_BITMAP, page, st);
        cfs_buf_put(fs, idx);
    }
}

/* The snapshot list and every deadlist hanging off it. */
static void scrub_snapshots(struct cfs *fs, uint8_t *page, struct cosmofs_scrub_stats *st)
{
    uint64_t blkno = fs->sb.snap_root;
    unsigned guard = 0;
    while (blkno && guard++ < CFS_MAX_MEMBERS * 64u) {
        scrub_meta(fs, blkno, CFS_KIND_SNAPLIST, page, st);
        struct cfs_buf *b;
        if (cfs_buf_get(fs, blkno, CFS_KIND_SNAPLIST, &b))
            return;
        const struct cfs_snap_block *sb = (const struct cfs_snap_block *)(b->data + CFS_MHDR_SIZE);
        uint64_t next = sb->next;
        for (unsigned i = 0; i < CFS_SNAPS_PER_BLOCK; i++) {
            uint64_t dead = sb->snap[i].name[0] ? sb->snap[i].deadlist : 0;
            unsigned dguard = 0;
            while (dead && dguard++ < 4096u) {
                scrub_meta(fs, dead, CFS_KIND_DEADLIST, page, st);
                struct cfs_buf *db;
                if (cfs_buf_get(fs, dead, CFS_KIND_DEADLIST, &db))
                    break;
                dead = ((const struct cfs_dead_block *)(db->data + CFS_MHDR_SIZE))->next;
                cfs_buf_put(fs, db);
            }
        }
        cfs_buf_put(fs, b);
        blkno = next;
    }
}

/* The whole extent chain, not just its head. */
static void scrub_extent_chain(struct cfs *fs, uint64_t head, uint8_t *page, struct cosmofs_scrub_stats *st)
{
    uint64_t next = head;
    unsigned guard = 0;
    while (next && guard++ < CFS_MAX_EXTENTS / CFS_EXTENTS_PER_BLOCK + 2) {
        scrub_meta(fs, next, CFS_KIND_EXTENTS, page, st);
        struct cfs_buf *b;
        if (cfs_buf_get(fs, next, CFS_KIND_EXTENTS, &b))
            return;
        next = ((const struct cfs_extent_block *)(b->data + CFS_MHDR_SIZE))->next;
        cfs_buf_put(fs, b);
    }
}

/* The checksum index and every leaf under it: the tree that decides
 * whether a data block is good is itself worth checking. */
static void scrub_csum_tree(struct cfs *fs, uint64_t root, uint8_t *page, struct cosmofs_scrub_stats *st)
{
    if (root == 0)
        return;
    scrub_meta(fs, root, CFS_KIND_CSUMIDX, page, st);
    struct cfs_buf *idx;
    if (cfs_buf_get(fs, root, CFS_KIND_CSUMIDX, &idx))
        return;
    const uint64_t *slots = (const uint64_t *)(idx->data + CFS_MHDR_SIZE);
    for (unsigned i = 0; i < CFS_PTRS_PER_BLOCK; i++)
        if (slots[i])
            scrub_meta(fs, slots[i], CFS_KIND_CSUM, page, st);
    cfs_buf_put(fs, idx);
}

/* One inode's own trees and its data blocks. */
static void scrub_inode(struct cfs *fs, const struct cfs_inode *inp, uint8_t *page, struct cosmofs_scrub_stats *st)
{
    struct cfs_inode in = *inp;
    st->inodes++;
    scrub_extent_chain(fs, in.indirect, page, st);
    scrub_csum_tree(fs, in.csum_root, page, st);
    uint64_t blocks = (in.size + CFS_BLOCK - 1) / CFS_BLOCK;
    for (uint64_t lblk = 0; lblk < blocks; lblk++) {
        uint64_t pblk = 0;
        int rc = cfs_map(fs, &in, lblk, &pblk);
        if (rc != 1)
            continue;   /* a hole, or a mapping that could not be read */
        unsigned fixed = 0;
        rc = cfs_data_scrub_block(fs, &in, lblk, pblk, page, &fixed);
        st->blocks_read++;
        if (rc)
            st->unrecoverable++;
        else
            st->repaired += fixed;
    }
}

/*
 * Every block of the inode map, and every inode in it. Walking the tree
 * here rather than reading inodes one at a time by number is what lets
 * each imap and inode block be checked on *every* copy: an ordinary
 * read stops at the first copy that verifies, so rot on another copy of
 * a block that leads to an inode would never be seen.
 */
static void scrub_imap(struct cfs *fs, uint8_t *page, struct cosmofs_scrub_stats *st)
{
    uint64_t l1 = fs->sb.imap_root;
    scrub_meta(fs, l1, CFS_KIND_IMAP1, page, st);
    struct cfs_buf *b1;
    if (cfs_buf_get(fs, l1, CFS_KIND_IMAP1, &b1))
        return;
    for (unsigned i = 0; i < CFS_PTRS_PER_BLOCK; i++) {
        uint64_t l0 = ((const uint64_t *)(b1->data + CFS_MHDR_SIZE))[i];
        if (l0 == 0)
            continue;
        scrub_meta(fs, l0, CFS_KIND_IMAP0, page, st);
        struct cfs_buf *b0;
        if (cfs_buf_get(fs, l0, CFS_KIND_IMAP0, &b0))
            continue;
        for (unsigned j = 0; j < CFS_PTRS_PER_BLOCK; j++) {
            uint64_t ib = ((const uint64_t *)(b0->data + CFS_MHDR_SIZE))[j];
            if (ib == 0)
                continue;
            scrub_meta(fs, ib, CFS_KIND_INODES, page, st);
            struct cfs_buf *bi;
            if (cfs_buf_get(fs, ib, CFS_KIND_INODES, &bi))
                continue;
            for (unsigned k = 0; k < CFS_INODES_PER_BLOCK; k++) {
                const struct cfs_inode *in =
                    (const struct cfs_inode *)(bi->data + CFS_MHDR_SIZE + (size_t)k * CFS_INODE_SIZE);
                if (in->nlink == 0 || in->ino == 0)
                    continue;
                scrub_inode(fs, in, page, st);
            }
            cfs_buf_put(fs, bi);
        }
        cfs_buf_put(fs, b0);
    }
    cfs_buf_put(fs, b1);
}

/*
 * Both superblock slots on every device of member 0. The root is the one
 * block whose loss costs everything, and the one block no mount can have
 * checked across copies -- the copies are not known until it has been
 * read. A slot no copy holds a superblock in is the unused slot of a
 * freshly formatted filesystem, not damage.
 */
static void scrub_supers(struct cfs *fs, uint8_t *page, struct cosmofs_scrub_stats *st)
{
    unsigned copies = pool_copies(fs->pool, CFS_DVA(0, 0));
    uint8_t *good = kmalloc(CFS_BLOCK, 0);
    if (good == NULL)
        return;
    for (unsigned slot = 0; slot < 2; slot++) {
        uint64_t dva = CFS_DVA(0, slot);
        bool bad[POOL_MAX_COPIES] = { false };
        unsigned nbad = 0;
        int best = -1;
        uint64_t newest = 0;
        for (unsigned c = 0; c < copies; c++) {
            if (pool_read_copy(fs->pool, dva, c, page) == 0 && cfs_super_ok(page)) {
                uint64_t g = ((const struct cfs_super *)page)->generation;
                if (best < 0 || g > newest) {
                    newest = g;
                    best = (int)c;
                    memcpy(good, page, CFS_BLOCK);
                }
                continue;
            }
            bad[c] = true;
            nbad++;
        }
        if (best < 0)
            continue;   /* no copy holds one: the slot is unused */
        st->blocks_read++;
        if (nbad == 0)
            continue;
        unsigned fixed = 0;
        for (unsigned c = 0; c < copies; c++)
            if (bad[c] && pool_write_copy(fs->pool, dva, c, good) == 0)
                fixed++;
        kwarn("cosmofs: superblock slot %u: %u of %u copies bad, %u rewritten from copy %d", slot, nbad, copies,
              fixed, best);
        fs->repairs += fixed;
        st->repaired += fixed;
    }
    kfree(good);
}

/*
 * The whole filesystem, in the order a mount would need it: the
 * allocation maps, the snapshots, then every inode and its data.
 */
int cosmofs_scrub(struct mount *mnt, struct cosmofs_scrub_stats *out)
{
    struct cfs *fs = cfs_of(mnt);
    if (fs == NULL)
        return -EINVAL;
    uint8_t *page = kmalloc(CFS_BLOCK, 0);
    if (page == NULL)
        return -ENOMEM;
    struct cosmofs_scrub_stats st;
    memset(&st, 0, sizeof(st));

    mutex_lock(&fs->lock);
    if (fs->failed) {
        mutex_unlock(&fs->lock);
        kfree(page);
        return -EIO;
    }
    scrub_supers(fs, page, &st);
    scrub_alloc(fs, page, &st);
    scrub_snapshots(fs, page, &st);
    scrub_imap(fs, page, &st);
    mutex_unlock(&fs->lock);
    kfree(page);

    kinfo("cosmofs: scrub read %llu blocks over %llu inodes: %llu repaired, %llu unrecoverable",
          (unsigned long long)st.blocks_read, (unsigned long long)st.inodes, (unsigned long long)st.repaired,
          (unsigned long long)st.unrecoverable);
    if (out)
        *out = st;
    return st.unrecoverable ? -EIO : 0;
}
