/*
 * cosmofstest_check_floor.c - The C under cosmofstest_check.cee (roadmap
 * 2.0 SA7): the layout cosmofstest_check.cee and cosmofstest_orphan.cee
 * write as their own constants, asserted here, since a self-test has no
 * host test to assert it.
 */

#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/random.h>
#include <kernel/vfs.h>

#include <uapi/cosmo/syscall.h>

#include "cosmofs_format.h"

_Static_assert(EIO == 5 && EAGAIN == 11 && EEXIST == 17 && ENOTSUP == 95 && ENOKEY == 126 && EKEYREJECTED == 129 &&
                   COSMO_O_RDONLY == 0 && VFS_UMOUNT_FORCE == 1 && KMEM_ZERO == 1 && COSMO_DT_DIR == 2 &&
                   RANDOM_KEYGEN_WAIT_NS == 5000000000ull,
               "cosmofstest_check.cee's errno.h, syscall.h, vfs.h, kmalloc.h and random.h values");
_Static_assert(CFS_BLOCK == 4096 && CFS_SUPER_A == 0 && CFS_MHDR_SIZE == 32 && CFS_MAX_BLOCKS == 16516096 &&
                   CFS_ROOT_INO == 1 && sizeof(CFS_MAGIC) == 9 && offsetof(struct cfs_mhdr, crc) == 24 &&
                   offsetof(struct cfs_super, generation) == 24 && offsetof(struct cfs_super, members) == 88 &&
                   offsetof(struct cfs_member_block, m) == 8 && sizeof(struct cfs_member) == 64 &&
                   offsetof(struct cfs_member, nblocks) == 16 && offsetof(struct cfs_member, first_usable) == 24,
               "cosmofstest_check.cee's cosmofs_format.h values and offsets");
_Static_assert(COSMOFS_SCRUB_REPAIR == 1 && COSMOFS_CHECK_REPAIR == 1 && CFS_CHECK_NAMES == 8 &&
                   COSMOFS_CORRUPT_LEAK == 0 && COSMOFS_CORRUPT_FREE_IN_USE == 1 && COSMOFS_CORRUPT_CROSSLINK == 2 &&
                   COSMOFS_CORRUPT_NLINK == 3 && COSMOFS_CORRUPT_ORPHAN == 4 && COSMOFS_CORRUPT_DANGLING == 5 &&
                   COSMOFS_CORRUPT_DIRENT == 6 && COSMOFS_CORRUPT_COUNTER == 7 && COSMOFS_CORRUPT_INO_SLOT == 8 &&
                   COSMOFS_CORRUPT_EXTENT_ORDER == 9 && COSMOFS_CORRUPT_BAD_PTR == 10 &&
                   COSMOFS_CORRUPT_TWO_PARENTS == 11 && COSMOFS_CORRUPT_SNAP_MEMBERS == 12 &&
                   COSMOFS_CORRUPT_DUP_NAME == 13 && COSMOFS_CORRUPT_CHAIN_CYCLE == 14 &&
                   COSMOFS_CORRUPT_NAMELEN == 15 && COSMOFS_CORRUPT_EXTENT_OVERLAP == 16,
               "cosmofstest_check.cee's and cosmofstest_orphan.cee's cosmofs.h values");
/* The slots the CeeSafe halves hold these in. */
_Static_assert(sizeof(struct cosmofs_check_report) <= 256 * 8 && sizeof(struct cosmofs_stats) <= 24 * 8 &&
                   sizeof(struct cosmo_stat) <= 8 * 8 && sizeof(struct cosmofs_scrub_stats) <= 8 * 8,
               "cosmofstest_check.cee's and cosmofstest_orphan.cee's slot sizes");
