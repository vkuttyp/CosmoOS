/*
 * cosmofstest_basic_floor.c - The C under cosmofstest_basic.cee (roadmap
 * 2.0 SA7): the engine tests' stand-ins in a build without
 * CONFIG_DEBUG, and the layout the CeeSafe half writes as its own
 * constants, asserted here, since a self-test has no host test to assert
 * it. The tests are CeeSafe (cosmofstest_basic.cee, generated into
 * cosmofstest_basic.c).
 */

#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/selftest.h>
#include <kernel/storage.h>
#include <kernel/vfs.h>

#include <uapi/cosmo/syscall.h>

#include "cosmofs_format.h"
#include "cosmofs_internal.h"

_Static_assert(ENOENT == 2 && EIO == 5 && EEXIST == 17 && EXDEV == 18 && EINVAL == 22 && ENOSPC == 28 &&
                   ENAMETOOLONG == 36 && ENOTEMPTY == 39,
               "cosmofstest_basic.cee's errno.h values");
_Static_assert(COSMO_O_RDONLY == 0 && COSMO_O_WRONLY == 1 && COSMO_O_RDWR == 2 && COSMO_O_CREAT == 0x40 &&
                   COSMO_O_TRUNC == 0x200 && COSMO_DT_DIR == 2 && POOL_DVA_SHIFT == 56 &&
                   sizeof(struct cosmo_stat) <= 64 && sizeof(struct cosmofs_stats) <= 128,
               "cosmofstest_basic.cee's syscall.h, storage.h and cosmofs.h values");
_Static_assert(CFS_BLOCK == 4096 && CFS_MHDR_SIZE == 32 && offsetof(struct cfs_mhdr, crc) == 24 &&
                   offsetof(struct cfs_mhdr, csum_algo) == 28 && CFS_KIND_INODES == 3 && CFS_CSUM_NONE == 0 &&
                   CFS_CSUM_CRC32C == 1 && CFS_CSUM_POLY1305 == 2 && MHDR_OK == 0 && MHDR_ALGO == 4 &&
                   MHDR_CRC == 5,
               "cosmofstest_basic.cee's metadata header values");
_Static_assert(sizeof(CFS_MAGIC) - 1 == 8 && offsetof(struct cfs_super, magic) == 0 &&
                   offsetof(struct cfs_super, generation) == 24 && offsetof(struct cfs_super, imap_root) == 32 &&
                   sizeof(struct cfs_dirent) == 64 && offsetof(struct cfs_dirent, ino) == 0 &&
                   offsetof(struct cfs_dirent, namelen) == 9 && offsetof(struct cfs_dirent, name) == 16,
               "cosmofstest_basic.cee's superblock and directory entry offsets");

#if !CONFIG_DEBUG
bool selftest_cosmofs_holes(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_csum(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_fsync(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_reserve(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_fallback(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_writeback(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_metadata_csum_id(const char **reason) { (void)reason; return true; }
#endif
