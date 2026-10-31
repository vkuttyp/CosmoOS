/*
 * cfsmount.c - cosmofs's decoders of the records a mount reads first
 * (roadmap 2.0 SA5, the cosmofs family, batch 2): the superblock's
 * identity, shape and CRC, a member label's, the member table's count and
 * each member's sanity, the free and orphan records' counts, and an inode
 * slot's identity. Moved unchanged from cosmofs_core.c and
 * cosmofs_member.c: the C step, to be replaced in cfsparse.cee once
 * tests/host/test_cfsparse.c's recorded digest agrees. The version-14 MAC
 * stays with the callers, which hold the key.
 */

#include <kernel/crc32c.h>
#include <kernel/string.h>

#include "cosmofs_internal.h"

/* Magic, and a version this kernel reads. */
bool cfs_super_ident_ok(const struct cfs_super *sb)
{
    return memcmp(sb->magic, CFS_MAGIC, 8) == 0 && sb->version >= CFS_VERSION_MIN && sb->version <= CFS_VERSION;
}

/* The mount's own check of a superblock slot, before its seal: identity,
 * block size, a size and a generation, and its pointers' shape. The
 * member table is not read yet -- it is named by this very block -- so a
 * pointer can only be checked for shape; before version 4 there is one
 * member, `dev0` blocks, and the old exact checks hold. */
bool cfs_super_shape_ok(const struct cfs_super *sb, uint64_t dev0)
{
    bool version_ok = sb->version >= CFS_VERSION_MIN && sb->version <= CFS_VERSION;
    bool ptrs_ok;
    if (sb->version >= 4)
        ptrs_ok = sb->total_blocks <= CFS_MAX_BLOCKS * (uint64_t)CFS_MAX_MEMBERS &&
                  CFS_DVA_VDEV(sb->imap_root) < CFS_MAX_MEMBERS && CFS_DVA_BLK(sb->imap_root) < CFS_MAX_BLOCKS &&
                  CFS_DVA_VDEV(sb->members) < CFS_MAX_MEMBERS && CFS_DVA_BLK(sb->members) < CFS_MAX_BLOCKS;
    else
        ptrs_ok = sb->total_blocks == dev0 && sb->imap_root < dev0 && sb->alloc_root < dev0;
    return !(memcmp(sb->magic, CFS_MAGIC, 8) != 0 || !version_ok || sb->block_size != CFS_BLOCK ||
             sb->total_blocks == 0 || sb->generation == 0 || !ptrs_ok);
}

/* The superblock's CRC32C (a CRC'd one: not version 14's MAC). */
bool cfs_super_crc_ok(const void *block)
{
    const struct cfs_super *sb = block;
    return cfs_block_crc(block, offsetof(struct cfs_super, crc)) == sb->crc;
}

/* cfs_super_peek's check: identity, a generation, and the CRC unless a MAC
 * (which it does not verify) seals it. */
bool cfs_super_peek_ok(const void *block)
{
    const struct cfs_super *sb = block;
    if (memcmp(sb->magic, CFS_MAGIC, 8) != 0 || sb->version < CFS_VERSION_MIN || sb->version > CFS_VERSION ||
        sb->generation == 0)
        return false;
    if (!cfs_super_is_mac(sb) && cfs_block_crc(block, offsetof(struct cfs_super, crc)) != sb->crc)
        return false;
    return true;
}

uint32_t cfs_label_crc(const uint8_t *block)
{
    static const uint8_t zero4[4] = { 0 };
    size_t off = offsetof(struct cfs_label, crc);
    uint32_t c = crc32c(block, off);
    c = crc32c_update(c, zero4, 4);
    return crc32c_update(c, block + off + 4, CFS_BLOCK - off - 4);
}

/* A label's CRC32C (a CRC'd one: the MAC is the caller's). */
bool cfs_label_crc_ok(const uint8_t *block)
{
    const struct cfs_label *l = (const struct cfs_label *)block;
    return cfs_label_crc(block) == l->crc;
}

/* A label's identity: magic, and a member index and copy in range. */
bool cfs_label_shape_ok(const void *block)
{
    const struct cfs_label *l = block;
    return memcmp(l->magic, CFS_LABEL_MAGIC, sizeof(l->magic)) == 0 && l->index < CFS_MAX_MEMBERS &&
           l->copy < CFS_MAX_COPIES;
}

bool cfs_member_sane(const struct cfs_member *m, unsigned v, unsigned nmembers)
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

/* The member table's count: at least one, no more than a block holds or the pool can address. */
bool cfs_members_count_ok(uint64_t count)
{
    return !(count == 0 || count > CFS_MEMBERS_PER_BLOCK || count > CFS_MAX_MEMBERS);
}

/* A free record's and an orphan record's counts: no more than a block holds. */
bool cfs_freelog_count_ok(uint64_t count)
{
    return !(count > CFS_DEAD_PER_BLOCK);   /* CFS_FREELOG_PER_BLOCK */
}

bool cfs_orphan_count_ok(uint64_t count)
{
    return !(count > CFS_ORPHANS_PER_BLOCK);
}

/* An inode slot read for inode `ino`: that inode's, and in use. */
bool cfs_inode_slot_ok(const struct cfs_inode *in, uint64_t ino)
{
    return !(in->ino != ino || in->nlink == 0);
}
