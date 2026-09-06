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
    EXPECT(CFS_VERSION == 7 && CFS_VERSION_MIN == 2);
    /* The snapshot structures the version adds. */
    EXPECT(sizeof(struct cfs_snapshot) == 96);
    EXPECT(CFS_SNAPS_PER_BLOCK >= 40 && sizeof(struct cfs_snap_block) <= CFS_BLOCK - CFS_MHDR_SIZE);
    EXPECT(sizeof(struct cfs_dead_block) <= CFS_BLOCK - CFS_MHDR_SIZE);
    EXPECT(CFS_SNAP_INO(3, 7) == ((3ull << 48) | 7) && CFS_INO_OF(CFS_SNAP_INO(3, 7)) == 7);
    EXPECT(CFS_SNAP_TAG(CFS_SNAP_INO(3, 7)) == 3 && CFS_SNAP_TAG(7) == 0);
    EXPECT(CFS_MHDR_SIZE + CFS_INODES_PER_BLOCK * CFS_INODE_SIZE <= CFS_BLOCK);

    /* Version 4: a DVA is a member and a block, packed into the eight
     * bytes every pointer already had -- which is what leaves every
     * structure above the size it was, and makes a version-3 pointer a
     * version-4 pointer on member 0. */
    EXPECT(CFS_DVA_VDEV(CFS_DVA(3, 7)) == 3 && CFS_DVA_BLK(CFS_DVA(3, 7)) == 7);
    EXPECT(CFS_DVA(0, 12345) == 12345);                  /* member 0 is the bare block number */
    EXPECT(CFS_DVA_VDEV(99) == 0 && CFS_DVA_BLK(99) == 99);
    EXPECT(CFS_DVA_BLK(CFS_DVA(254, CFS_DVA_BLK_MASK)) == CFS_DVA_BLK_MASK);
    EXPECT(CFS_DVA_VDEV(CFS_DVA_NONE) == 255 && CFS_MAX_MEMBERS == 255);
    EXPECT(sizeof(struct cfs_member) == 64);
    EXPECT(CFS_MEMBERS_PER_BLOCK >= 60 && sizeof(struct cfs_member_block) <= CFS_PAYLOAD);
    EXPECT(sizeof(struct cfs_label) <= CFS_BLOCK);

    /* Version 5: a member is a mirror group. The copy count sits in what
     * version 4 wrote as padding, so 0 and 1 mean the same thing and the
     * member entry stays 64 bytes. */
    EXPECT(sizeof(struct cfs_member) == 64);
    EXPECT(CFS_MAX_COPIES == 4);
    EXPECT(offsetof(struct cfs_member, copies) == 52);   /* what version 4 wrote as padding */
    EXPECT(offsetof(struct cfs_label, generation) < CFS_BLOCK);

    /* Version 6: a compressed record's physical size lives in the top
     * bits of `count`, which is why nothing above changed width. */
    struct cfs_extent plain = { 4096, 300, 0 };
    EXPECT(!cfs_ext_compressed(&plain));
    EXPECT(cfs_ext_count(&plain) == 300 && cfs_ext_psize(&plain) == 300);
    EXPECT(cfs_ext_algo(&plain) == CFS_COMPRESS_NONE);

    struct cfs_extent rec = { 4096, cfs_ext_pack(CFS_COMPRESS_LZ4, 3, 8), 16 };
    EXPECT(cfs_ext_compressed(&rec));
    EXPECT(cfs_ext_count(&rec) == 8 && cfs_ext_psize(&rec) == 3);
    EXPECT(cfs_ext_algo(&rec) == CFS_COMPRESS_LZ4);
    /* A record of one physical block, and the widest a record can be. */
    struct cfs_extent one = { 8, cfs_ext_pack(CFS_COMPRESS_LZ4, 1, 1), 0 };
    EXPECT(cfs_ext_psize(&one) == 1 && cfs_ext_count(&one) == 1);
    struct cfs_extent wide = { 8, cfs_ext_pack(CFS_COMPRESS_LZ4, 256, 0xFFFF), 0 };
    EXPECT(cfs_ext_psize(&wide) == 256 && cfs_ext_count(&wide) == 0xFFFF);
    EXPECT(CFS_RECORD_BLOCKS <= 0xFFFFu);

    /* A run from before version 6 has bit 31 clear, so it reads as an
     * ordinary run of exactly its count. */
    struct cfs_extent old = { 4096, 0x7FFFFFFFu, 0 };
    EXPECT(!cfs_ext_compressed(&old) && cfs_ext_count(&old) == 0x7FFFFFFFu);

    /* And a record maps nothing one-to-one: cfs_map_block says so
     * instead of computing an address inside it. */
    uint64_t pblk = 0;
    EXPECT(cfs_map_block(&rec, 1, 18, &pblk) == -1);
    EXPECT(cfs_map_block(&plain, 1, 12, &pblk) == 1 && pblk == 4096 + 12);

    /* Version 7: the authenticated checksum entry, and the key block. */
    EXPECT(sizeof(struct cfs_csum_aead) == 32);
    EXPECT(CFS_AEAD_PER_BLOCK == CFS_PAYLOAD / 32);
    EXPECT(cfs_aead_index(0) == 0 && cfs_aead_slot(0) == 0);
    EXPECT(cfs_aead_index(CFS_AEAD_PER_BLOCK) == 1 && cfs_aead_slot(CFS_AEAD_PER_BLOCK) == 0);
    EXPECT(cfs_aead_slot(CFS_AEAD_PER_BLOCK - 1) == CFS_AEAD_PER_BLOCK - 1);
    EXPECT(sizeof(struct cfs_keys) <= CFS_PAYLOAD);
    EXPECT(CFS_CSUM_POLY1305 == 2 && CFS_KDF_SHA512 == 1);
    EXPECT(sizeof(struct cfs_super) <= CFS_BLOCK);   /* key_root came out of reserved */
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
