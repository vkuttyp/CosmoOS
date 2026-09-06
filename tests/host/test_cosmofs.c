/*
 * test_cosmofs.c - cosmofs on-disk layout: structure sizes and the pure
 * extent mapping helpers, on the host.
 */

#include "harness.h"

#include "cosmofs_format.h"

static void test_layout_sizes(void)
{
    EXPECT(sizeof(struct cfs_mhdr) == CFS_MHDR_SIZE);
    EXPECT(sizeof(struct cfs_inode) == CFS_INODE_SIZE);
    EXPECT(sizeof(struct cfs_dirent) == CFS_DIRENT_SIZE);
    EXPECT(sizeof(struct cfs_extent) == 16);
    EXPECT(sizeof(struct cfs_super) <= CFS_BLOCK);
    EXPECT(CFS_PTRS_PER_BLOCK == 508);
    EXPECT(CFS_INODES_PER_BLOCK == 15);
    EXPECT(CFS_BITS_PER_BITMAP == 32512);
    EXPECT(CFS_EXTENTS_PER_BLOCK == 253);
    EXPECT(sizeof(struct cfs_extent_block) <= CFS_PAYLOAD);
    EXPECT(CFS_DIRENTS_PER_BLOCK == 64);
    EXPECT(CFS_CSUMS_PER_BLOCK == 1016);
    EXPECT(CFS_VERSION == 3 && CFS_VERSION_MIN == 2);
    /* The snapshot structures the version adds. */
    EXPECT(sizeof(struct cfs_snapshot) == 96);
    EXPECT(CFS_SNAPS_PER_BLOCK >= 40 && sizeof(struct cfs_snap_block) <= CFS_BLOCK - CFS_MHDR_SIZE);
    EXPECT(sizeof(struct cfs_dead_block) <= CFS_BLOCK - CFS_MHDR_SIZE);
    EXPECT(CFS_SNAP_INO(3, 7) == ((3ull << 48) | 7) && CFS_INO_OF(CFS_SNAP_INO(3, 7)) == 7);
    EXPECT(CFS_SNAP_TAG(CFS_SNAP_INO(3, 7)) == 3 && CFS_SNAP_TAG(7) == 0);
    EXPECT(CFS_MHDR_SIZE + CFS_INODES_PER_BLOCK * CFS_INODE_SIZE <= CFS_BLOCK);
}

static void test_inode_indices(void)
{
    EXPECT(cfs_inode_block_index(0) == 0 && cfs_inode_slot(0) == 0);
    EXPECT(cfs_inode_block_index(14) == 0 && cfs_inode_slot(14) == 14);
    EXPECT(cfs_inode_block_index(15) == 1 && cfs_inode_slot(15) == 0);
    EXPECT(cfs_imap_l0_index(15) == 1 && cfs_imap_l1_index(15) == 0);
    uint64_t ino = (uint64_t)508 * 15;           /* first inode of L1 index 1 */
    EXPECT(cfs_imap_l1_index(ino) == 1 && cfs_imap_l0_index(ino) == 0);
    EXPECT(cfs_imap_l1_index(CFS_MAX_INODES - 1) == 507);
}

static void test_extent_mapping(void)
{
    /* Runs carry their logical position (version 2): [0,3) -> 100..,
     * [3,4) -> 200, a hole at 4..9, [10,12) -> 50.. */
    struct cfs_extent ext[3] = { { 100, 3, 0 }, { 200, 1, 3 }, { 50, 2, 10 } };
    uint64_t p;
    EXPECT(cfs_extent_blocks(ext, 3) == 12);   /* one past the highest mapped block */
    EXPECT(cfs_map_block(ext, 3, 0, &p) == 1 && p == 100);
    EXPECT(cfs_map_block(ext, 3, 2, &p) == 1 && p == 102);
    EXPECT(cfs_map_block(ext, 3, 3, &p) == 1 && p == 200);
    EXPECT(cfs_map_block(ext, 3, 4, &p) == 0);   /* a hole */
    EXPECT(cfs_map_block(ext, 3, 9, &p) == 0);
    EXPECT(cfs_map_block(ext, 3, 10, &p) == 1 && p == 50);
    EXPECT(cfs_map_block(ext, 3, 11, &p) == 1 && p == 51);
    EXPECT(cfs_map_block(ext, 3, 12, &p) == 0);
    EXPECT(cfs_map_block(ext, 0, 0, &p) == 0);
    EXPECT(cfs_extent_blocks(ext, 0) == 0);
}

static void test_csum_index(void)
{
    EXPECT(cfs_csum_index(0) == 0 && cfs_csum_slot(0) == 0);
    EXPECT(cfs_csum_index(1015) == 0 && cfs_csum_slot(1015) == 1015);
    EXPECT(cfs_csum_index(1016) == 1 && cfs_csum_slot(1016) == 0);
    EXPECT(cfs_csum_index(CFS_CSUM_MAX_BLOCKS - 1) == 507);
    EXPECT(CFS_CSUM_MAX_BLOCKS == 516128);   /* one index level: files up to 516128 blocks (about 1.97 GiB) */
}

static const struct host_test tests[] = {
    { "cosmofs-layout", test_layout_sizes },
    { "cosmofs-inode-index", test_inode_indices },
    { "cosmofs-extents", test_extent_mapping },
    { "cosmofs-csum-index", test_csum_index },
};

int main(void)
{
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
