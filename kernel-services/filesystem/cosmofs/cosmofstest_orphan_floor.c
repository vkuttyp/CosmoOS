/*
 * cosmofstest_orphan_floor.c - The C under cosmofstest_orphan.cee
 * (roadmap 2.0 SA7): the layout it writes as its own constants -- the
 * orphan record's capacity, a directory entry, and /dev/fsctl's
 * command and records -- asserted here, since a self-test has no host
 * test to assert it. Its cosmofs_stats is cosmofstest_check_floor.c's.
 */

#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/vfs.h>

#include <uapi/cosmo/fsctl.h>
#include <uapi/cosmo/syscall.h>

#include "cosmofs_format.h"

_Static_assert(ENOENT == 2 && EEXIST == 17 && EINVAL == 22 && EOPNOTSUPP == 95 && COSMO_O_RDONLY == 0 &&
                   COSMO_O_RDWR == 2 && VFS_UMOUNT_FORCE == 1 && KMEM_ZERO == 1,
               "cosmofstest_orphan.cee's errno.h, syscall.h, vfs.h and kmalloc.h values");
_Static_assert(CFS_BLOCK == 4096 && CFS_ORPHANS_PER_BLOCK == 506 && CFS_CHECK_NAMES == 8 &&
                   COSMOFS_CHECK_REPAIR == 1 && COSMOFS_CORRUPT_LEAK == 0 && COSMOFS_CORRUPT_ORPHAN == 4 &&
                   COSMOFS_CORRUPT_INO_SLOT == 8 && sizeof(struct cfs_dirent) == 64 &&
                   offsetof(struct cfs_dirent, ino) == 0 && offsetof(struct cfs_dirent, namelen) == 9 &&
                   offsetof(struct cfs_dirent, name) == 16,
               "cosmofstest_orphan.cee's cosmofs_format.h and cosmofs.h values");
_Static_assert(COSMO_FSCTL_VERSION == 3 && COSMO_FSCTL_LIST == 1 && COSMO_FSCTL_CHECK == 2 &&
                   COSMO_FSCTL_SCRUB == 3 && COSMO_FSCTL_F_REPAIR == 1 && COSMO_FSCTL_CLASSES == 16 &&
                   COSMO_FSCTL_R_PARTIAL == 1 && COSMO_FSCTL_R_CLEAN == 2 && COSMO_FSCTL_R_REPAIR_REFUSED == 4,
               "cosmofstest_orphan.cee's fsctl.h values");
_Static_assert(sizeof(struct cosmo_fsctl) == 88 && sizeof(struct cosmo_fsctl) <= 11 * 8 &&
                   offsetof(struct cosmo_fsctl, version) == 0 && offsetof(struct cosmo_fsctl, op) == 2 &&
                   offsetof(struct cosmo_fsctl, flags) == 4 && offsetof(struct cosmo_fsctl, mount_id) == 8 &&
                   sizeof(struct cosmo_fsctl_result) == 16 && offsetof(struct cosmo_fsctl_result, kind) == 2 &&
                   offsetof(struct cosmo_fsctl_result, count) == 4 && offsetof(struct cosmo_fsctl_result, bytes) == 12,
               "cosmofstest_orphan.cee's fsctl.h command and result layout");
_Static_assert(sizeof(struct cosmo_fsctl_check) == 1480 && offsetof(struct cosmo_fsctl_check, nclasses) == 0 &&
                   offsetof(struct cosmo_fsctl_check, flags) == 4 &&
                   offsetof(struct cosmo_fsctl_check, blocks_seen) == 8 &&
                   offsetof(struct cosmo_fsctl_check, inodes_seen) == 16 &&
                   offsetof(struct cosmo_fsctl_check, counted_free) == 40 &&
                   offsetof(struct cosmo_fsctl_check, class) == 72 && offsetof(struct cosmo_fsctl_class, count) == 0 &&
                   offsetof(struct cosmo_fsctl_class, repaired) == 8 &&
                   offsetof(struct cosmo_fsctl_class, name) == 16 && offsetof(struct cosmo_fsctl_class, named) == 80 &&
                   sizeof(struct cosmo_fsctl_scrub) == 32 && offsetof(struct cosmo_fsctl_scrub, blocks_read) == 0 &&
                   offsetof(struct cosmo_fsctl_scrub, unrecoverable) == 24,
               "cosmofstest_orphan.cee's fsctl.h check and scrub records");
