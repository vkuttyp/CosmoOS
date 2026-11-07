/*
 * cosmofstest_media_floor.c - The C under cosmofstest_media.cee (roadmap
 * 2.0 SA7): the layout the CeeSafe half writes as its own constants,
 * asserted here, since a self-test has no host test to assert it, and
 * the self-tests' stand-ins in a build without CONFIG_DEBUG. The tests
 * are CeeSafe (cosmofstest_media.cee, generated into cosmofstest_media.c).
 */

#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/selftest.h>
#include <kernel/vfs.h>

#include <uapi/cosmo/syscall.h>

#include "cosmofs_format.h"

_Static_assert(EIO == 5 && ENOENT == 2 && EBUSY == 16 && EEXIST == 17 && ENODEV == 19 && EROFS == 30 &&
                   COSMO_O_RDONLY == 0 && COSMO_O_RDWR == 2 && COSMO_O_CREAT == 0x40 && VFS_UMOUNT_FORCE == 1 &&
                   COSMOFS_SCRUB_REPAIR == 1,
               "cosmofstest_media.cee's errno.h, syscall.h, vfs.h and cosmofs.h values");
_Static_assert(CFS_BLOCK == 4096 && CFS_MHDR_SIZE == 32 && offsetof(struct cfs_mhdr, crc) == 24 && CFS_SUPER_A == 0 &&
                   CFS_SUPER_B == 1 && offsetof(struct cfs_super, magic) == 0 &&
                   offsetof(struct cfs_super, generation) == 24 && offsetof(struct cfs_super, imap_root) == 32 &&
                   offsetof(struct cfs_super, crc) == 160 && offsetof(struct cfs_label, magic) == 0 &&
                   sizeof(((struct cfs_label *)0)->magic) == 8 && offsetof(struct cfs_label, generation) == 48 &&
                   offsetof(struct cfs_label, crc) == 72,
               "cosmofstest_media.cee's superblock and label offsets");
_Static_assert(CFS_INODE_SIZE == 256 && CFS_INODES_PER_BLOCK == 15 && CFS_PTRS_PER_BLOCK == 508 &&
                   offsetof(struct cfs_inode, ino) == 48 && offsetof(struct cfs_inode, direct) == 56 &&
                   sizeof(struct cfs_extent) == 16 && offsetof(struct cfs_extent, count) == 8 &&
                   offsetof(struct cfs_extent, lblk) == 12 && CFS_DVA_VDEV_SHIFT == 56,
               "cosmofstest_media.cee's inode and DVA layout");
_Static_assert(sizeof(struct cosmofs_stats) <= 128 && offsetof(struct cosmofs_stats, generation) == 0 &&
                   offsetof(struct cosmofs_stats, free_blocks) == 8 &&
                   offsetof(struct cosmofs_stats, total_blocks) == 16 &&
                   offsetof(struct cosmofs_stats, members) == 72 && offsetof(struct cosmofs_stats, devices) == 76 &&
                   offsetof(struct cosmofs_stats, repairs) == 80 && offsetof(struct cosmofs_stats, degraded) == 88,
               "cosmofstest_media.cee's struct cosmofs_stats offsets");
_Static_assert(sizeof(struct cosmofs_scrub_stats) <= 128 && offsetof(struct cosmofs_scrub_stats, blocks_read) == 0 &&
                   offsetof(struct cosmofs_scrub_stats, inodes) == 8 &&
                   offsetof(struct cosmofs_scrub_stats, repaired) == 16 &&
                   offsetof(struct cosmofs_scrub_stats, unrecoverable) == 24 && sizeof(struct cosmo_stat) <= 64,
               "cosmofstest_media.cee's struct cosmofs_scrub_stats offsets and struct cosmo_stat's size");

#if !CONFIG_DEBUG
bool selftest_cosmofs_badmap(const char **reason)
{
    (void)reason;
    return true;
}

bool selftest_cosmofs_snapshot(const char **reason)
{
    (void)reason;
    return true;
}
#endif

int cfst_media_floor_stats(struct mount *mnt, void *out);

/* cosmofs_stats for the CeeSafe tests: its name is its struct's too, so
 * they cannot declare both. `out` is a struct cosmofs_stats. */
int cfst_media_floor_stats(struct mount *mnt, void *out)
{
    return cosmofs_stats(mnt, out);
}
