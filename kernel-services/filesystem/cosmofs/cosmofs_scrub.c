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

/* One inode's data blocks, each against the checksum the inode records.
 * Reading the inode at all has already walked (and repaired) the imap
 * and inode blocks that lead to it. */
static void scrub_inode(struct cfs *fs, uint64_t ino, uint8_t *page, struct cosmofs_scrub_stats *st)
{
    struct cfs_inode in;
    if (cfs_inode_read(fs, ino, &in) != 0)
        return;
    if (in.nlink == 0 || in.ino != ino)
        return;
    st->inodes++;
    scrub_meta(fs, in.indirect, CFS_KIND_EXTENTS, page, st);
    scrub_meta(fs, in.csum_root, CFS_KIND_CSUMIDX, page, st);
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
    scrub_alloc(fs, page, &st);
    scrub_snapshots(fs, page, &st);
    for (uint64_t ino = CFS_ROOT_INO; ino < fs->sb.next_ino; ino++)
        scrub_inode(fs, ino, page, &st);
    mutex_unlock(&fs->lock);
    kfree(page);

    kinfo("cosmofs: scrub read %llu blocks over %llu inodes: %llu repaired, %llu unrecoverable",
          (unsigned long long)st.blocks_read, (unsigned long long)st.inodes, (unsigned long long)st.repaired,
          (unsigned long long)st.unrecoverable);
    if (out)
        *out = st;
    return st.unrecoverable ? -EIO : 0;
}
