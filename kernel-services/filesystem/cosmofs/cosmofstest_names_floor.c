/*
 * cosmofstest_names_floor.c - The C under cosmofstest_names.cee (roadmap
 * 2.0 SA7): the values the CeeSafe half writes as its own constants,
 * asserted here, since a self-test has no host test to assert them, and
 * the stand-ins the selftest table names in a build without
 * CONFIG_DEBUG. The tests are CeeSafe (cosmofstest_names.cee, generated
 * into cosmofstest_names.c).
 */

#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/selftest.h>
#include <kernel/vfs.h>

#include <uapi/cosmo/syscall.h>

#include "cosmofs_format.h"

_Static_assert(EPERM == 1 && ENOENT == 2 && EEXIST == 17 && EINVAL == 22 && EROFS == 30 && ENAMETOOLONG == 36 &&
                   EOPNOTSUPP == 95 && COSMO_O_RDONLY == 0 && COSMO_O_WRONLY == 1 && COSMO_O_RDWR == 2 &&
                   COSMO_O_CREAT == 0x40 && VFS_UMOUNT_FORCE == 1 && COSMO_DT_LNK == 6,
               "cosmofstest_names.cee's errno.h, syscall.h and vfs.h values");
_Static_assert(CFS_BLOCK == 4096 && CFS_FORMAT_VERSION == 11 && CFS_NAME_MAX_LONG == 255 &&
                   CFS_DIRENTS_PER_BLOCK == 64 && COSMOFS_SNAP_CREATE == 1 && COSMOFS_SNAP_LIST == 2 &&
                   COSMOFS_SNAP_DELETE == 3 && COSMOFS_CHECK_REPAIR == 1 && COSMOFS_CORRUPT_DUP_NAME == 13 &&
                   COSMOFS_CORRUPT_SNAP_UUID == 17 && COSMOFS_CORRUPT_LONGNAME_EXTRA == 20 &&
                   COSMOFS_CORRUPT_LONGNAME_CONT == 21,
               "cosmofstest_names.cee's cosmofs_format.h and cosmofs.h values");
_Static_assert(sizeof(struct cosmofs_snap_info) == 104 && sizeof(struct cosmo_dirent) == 16 &&
                   offsetof(struct cosmo_dirent, reclen) == 8 && offsetof(struct cosmo_dirent, namelen) == 11 &&
                   offsetof(struct cosmo_dirent, name) == 12 && sizeof(struct cosmo_stat) <= 64 &&
                   offsetof(struct cosmo_stat, type) == 8 && offsetof(struct cosmo_stat, nlink) == 16 &&
                   sizeof(struct cosmofs_check_report) <= 2048,
               "cosmofstest_names.cee's sizes and offsets");

#if !CONFIG_DEBUG
bool selftest_cosmofs_snapshot_remount(const char **reason)
{
    (void)reason;
    return true;
}

bool selftest_cosmofs_snap_identity(const char **reason)
{
    (void)reason;
    return true;
}
#endif
