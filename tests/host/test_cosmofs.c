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
    EXPECT(CFS_EXTENTS_PER_BLOCK == 254);
    EXPECT(CFS_DIRENTS_PER_BLOCK == 64);
    EXPECT(CFS_MAX_EXTENTS == 264);
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
    struct cfs_extent ext[3] = { { 100, 3, 0 }, { 200, 1, 0 }, { 50, 2, 0 } };
    uint64_t p;
    EXPECT(cfs_extent_blocks(ext, 3) == 6);
    EXPECT(cfs_map_block(ext, 3, 0, &p) == 1 && p == 100);
    EXPECT(cfs_map_block(ext, 3, 2, &p) == 1 && p == 102);
    EXPECT(cfs_map_block(ext, 3, 3, &p) == 1 && p == 200);
    EXPECT(cfs_map_block(ext, 3, 4, &p) == 1 && p == 50);
    EXPECT(cfs_map_block(ext, 3, 5, &p) == 1 && p == 51);
    EXPECT(cfs_map_block(ext, 3, 6, &p) == 0);
    EXPECT(cfs_map_block(ext, 0, 0, &p) == 0);
}

static const struct host_test tests[] = {
    { "cosmofs-layout", test_layout_sizes },
    { "cosmofs-inode-index", test_inode_indices },
    { "cosmofs-extents", test_extent_mapping },
};

int main(void)
{
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
