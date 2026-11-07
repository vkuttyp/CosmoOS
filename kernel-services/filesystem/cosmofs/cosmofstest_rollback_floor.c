/*
 * cosmofstest_rollback_floor.c - The C under cosmofstest_rollback.cee
 * (roadmap 2.0 SA7): the layout the CeeSafe half writes as its own
 * constants, asserted here, since a self-test has no host test to assert
 * it, and the self-tests' stand-ins in a build without CONFIG_DEBUG. The
 * tests are CeeSafe (cosmofstest_rollback.cee, generated into
 * cosmofstest_rollback.c).
 */

#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/selftest.h>
#include <kernel/vfs.h>

#include <uapi/cosmo/syscall.h>

#include "cosmofs_format.h"

_Static_assert(ENOENT == 2 && EIO == 5 && EBUSY == 16 && EEXIST == 17 && EXDEV == 18 && EINVAL == 22 &&
                   EROFS == 30 && COSMO_O_RDONLY == 0 && COSMO_O_RDWR == 2,
               "cosmofstest_rollback.cee's errno.h and syscall.h values");
_Static_assert(COSMOFS_SNAP_CREATE == 1 && COSMOFS_SNAP_LIST == 2 && COSMOFS_SNAP_DELETE == 3 &&
                   COSMOFS_SNAP_ROLLBACK == 4 && COSMOFS_SNAP_CANCEL == 5 && COSMOFS_SNAP_CLONE == 6 &&
                   COSMOFS_SNAP_BOOT_OK == 7 && COSMOFS_POOL_PENDING == 1 && COSMOFS_POOL_HELD == 2 &&
                   COSMOFS_SNAP_I_TARGET == 2 && COSMOFS_SNAP_I_SAFETY == 4 && COSMOFS_SNAP_I_HELD == 8 &&
                   COSMOFS_CORRUPT_ROLLBACK_CRC == 18 && COSMOFS_CHECK_REPAIR == 1,
               "cosmofstest_rollback.cee's cosmofs.h values");
_Static_assert(sizeof(struct cosmofs_snap_info) <= 13 * 8 && sizeof(struct cosmofs_snap_req) <= 12 * 8 &&
                   sizeof(struct cosmofs_check_report) <= 200 * 8 && sizeof(struct cosmo_stat) <= 8 * 8 &&
                   sizeof(struct cosmofs_stats) <= 32 * 8 && offsetof(struct cosmofs_stats, free_blocks) == 8,
               "cosmofstest_rollback.cee's slots for cosmofs.h's and syscall.h's structs");
_Static_assert(sizeof(struct cfs_rollback) == 120 && offsetof(struct cfs_rollback, magic) == 0 &&
                   offsetof(struct cfs_rollback, version) == 4 && offsetof(struct cfs_rollback, state) == 8 &&
                   offsetof(struct cfs_rollback, target_uuid) == 16 &&
                   offsetof(struct cfs_rollback, safety_uuid) == 32 &&
                   offsetof(struct cfs_rollback, target_id) == 48 && offsetof(struct cfs_rollback, safety_id) == 56 &&
                   offsetof(struct cfs_rollback, generation) == 64 &&
                   offsetof(struct cfs_rollback, requested_ns) == 72 &&
                   offsetof(struct cfs_rollback, crc) == 112 && CFS_ROLLBACK_MAGIC == 0x42524643u &&
                   CFS_ROLLBACK_PENDING == 1 && CFS_ROLLBACK_HELD == 2,
               "cosmofstest_rollback.cee's cosmofs_format.h values and offsets");

#if !CONFIG_DEBUG
bool selftest_cosmofs_rollback(const char **reason)
{
    (void)reason;
    return true;
}

bool selftest_cosmofs_rollback_shared(const char **reason)
{
    (void)reason;
    return true;
}

bool selftest_cosmofs_rollback_atomic(const char **reason)
{
    (void)reason;
    return true;
}

bool selftest_cosmofs_rollback_record(const char **reason)
{
    (void)reason;
    return true;
}

bool selftest_cosmofs_clone(const char **reason)
{
    (void)reason;
    return true;
}
#endif
