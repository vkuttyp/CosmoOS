/*
 * cosmofstest_b2_floor.c - The C under cosmofstest_b2.cee (roadmap 2.0
 * SA7): what a build configuration decides -- cosmofs-nomem's fault
 * injection, which only a build with CONFIG_FAULTINJECT has, and the
 * self-tests' stand-ins in a build without CONFIG_DEBUG -- the scrub's
 * bound, a variable CeeSafe does not reach, and the layout the CeeSafe
 * half writes as its own constants, asserted here, since a self-test has
 * no host test to assert it. The tests are CeeSafe (cosmofstest_b2.cee,
 * generated into cosmofstest_b2.c).
 */

#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/faultinject.h>
#include <kernel/selftest.h>
#include <kernel/thread.h>
#include <kernel/vfs.h>

#include <uapi/cosmo/fsctl.h>
#include <uapi/cosmo/syscall.h>

#include "cosmofs_format.h"

_Static_assert(ENOENT == 2 && ENOMEM == 12 && EBUSY == 16 && EEXIST == 17 && ENOTDIR == 20 && EROFS == 30 &&
                   COSMO_O_RDONLY == 0 && COSMO_O_RDWR == 2 && COSMO_O_CREAT == 0x40 && VFS_UMOUNT_FORCE == 1 &&
                   MOUNT_RDONLY == 1,
               "cosmofstest_b2.cee's errno.h, syscall.h and vfs.h values");
_Static_assert(COSMOFS_CHECK_REPAIR == 1 && COSMOFS_CHECK_BOOT == 4 && COSMOFS_SCRUB_REPAIR == 1 &&
                   COSMOFS_SNAP_CREATE == 1 && COSMOFS_SNAP_DELETE == 3 && COSMOFS_CORRUPT_LEAK == 0 &&
                   COSMOFS_CORRUPT_ROOT_TYPE == 19 && CFS_ROOT_INO == 1,
               "cosmofstest_b2.cee's cosmofs.h values");
_Static_assert(COSMO_FSCTL_DEV_F_CHECKED == 1 && COSMO_FSCTL_DEV_F_SUPER_DISAGREE == 4 &&
                   COSMO_FSCTL_DEV_F_MOUNT_FAILED == 8 && COSMO_FSCTL_DEV_F_COMMITTED == 512 &&
                   COSMO_FSCTL_DEV_F_BOOT_REFUSED == 1024 && COSMO_FSCTL_STAGE_ROOT == 8 &&
                   COSMO_FSCTL_VERSION == 3 && COSMO_FSCTL_SCRUB == 3 && sizeof(struct cosmo_fsctl) == 88 &&
                   offsetof(struct cosmo_fsctl, version) == 0 && offsetof(struct cosmo_fsctl, op) == 2 &&
                   offsetof(struct cosmo_fsctl, mount_id) == 8 && sizeof(struct cosmo_fsctl_result) == 16 &&
                   sizeof(struct cosmo_fsctl_scrub) == 32 && offsetof(struct cosmo_fsctl_scrub, blocks_read) == 0 &&
                   offsetof(struct cosmo_fsctl_scrub, unrecoverable) == 24,
               "cosmofstest_b2.cee's fsctl.h values, sizes and offsets");
_Static_assert(sizeof(struct cosmofs_stats) <= 32 * 8 && offsetof(struct cosmofs_stats, dirty_buffers) == 32 &&
                   offsetof(struct cosmofs_stats, version) == 96 && offsetof(struct cosmofs_stats, free_root) == 104 &&
                   offsetof(struct cosmofs_stats, orphan_root) == 112 &&
                   sizeof(struct cosmofs_check_report) <= 200 * 8 && sizeof(struct cosmofs_dev_report) <= 12 * 8 &&
                   sizeof(struct cosmofs_scrub_stats) <= 8 * 8 && sizeof(struct cosmofs_snap_info) <= 13 * 8,
               "cosmofstest_b2.cee's slots and offsets for cosmofs.h's structs");
_Static_assert(CFS_BLOCK == 4096 && CFS_MHDR_SIZE == 32 && offsetof(struct cfs_mhdr, generation) == 8 &&
                   CFS_BITS_PER_BITMAP == 32512 && CFS_DVA_VDEV_SHIFT == 56 && CFS_KIND_BITMAP == 5 &&
                   offsetof(struct cfs_super, generation) == 24 && offsetof(struct cfs_super, members) == 88 &&
                   offsetof(struct cfs_super, uuid) == 96 && offsetof(struct cfs_super, crc) == 160 &&
                   offsetof(struct cfs_member_block, m) == 8 && offsetof(struct cfs_member, alloc_root) == 32 &&
                   sizeof(CFS_MAGIC) == 9,
               "cosmofstest_b2.cee's cosmofs_format.h values and offsets");

unsigned cfst_b2_floor_scrub_rate(void);
void cfst_b2_floor_set_scrub_rate(unsigned rate);
void cfst_b2_floor_fail_set(unsigned which);
uint64_t cfst_b2_floor_fail_end(unsigned which);

/* The scrub's bound (sysctl fs.scrub_rate). */
unsigned cfst_b2_floor_scrub_rate(void)
{
    return g_cosmofs_scrub_rate;
}

void cfst_b2_floor_set_scrub_rate(unsigned rate)
{
    g_cosmofs_scrub_rate = rate;
}

#if CONFIG_FAULTINJECT
static enum fi_kind nomem_kind(unsigned which)
{
    return which == 0 ? FI_COSMOFS_CHAIN_NOMEM : FI_COSMOFS_INODE_NOMEM;
}
#endif

/* Arm allocation failure `which` (0: a file's extent chain block, 1: an
 * inode's map path) once, for this thread. */
void cfst_b2_floor_fail_set(unsigned which)
{
#if CONFIG_FAULTINJECT
    faultinject_set(nomem_kind(which), 1, 1, thread_current());
#else
    (void)which;
#endif
}

/* How often `which` fired since it was armed; disarmed. */
uint64_t cfst_b2_floor_fail_end(unsigned which)
{
#if CONFIG_FAULTINJECT
    struct fi_stats fst;
    faultinject_stats(nomem_kind(which), &fst);
    faultinject_clear(nomem_kind(which));
    return fst.hits;
#else
    (void)which;
    return 0;
#endif
}

#if CONFIG_DEBUG
/* cosmofstest_b2.cee's: cosmofs-nomem's body. */
bool cfst_b2_nomem(const char **reason);

bool selftest_cosmofs_nomem(const char **reason)
{
#if CONFIG_FAULTINJECT
    return cfst_b2_nomem(reason);
#else
    (void)reason;
    return true;
#endif
}
#else
bool selftest_cosmofs_ro_replay(const char **reason)
{
    (void)reason;
    return true;
}

bool selftest_cosmofs_version_compress(const char **reason)
{
    (void)reason;
    return true;
}

bool selftest_cosmofs_scrub_counts(const char **reason)
{
    (void)reason;
    return true;
}

bool selftest_cosmofs_root_type(const char **reason)
{
    (void)reason;
    return true;
}

bool selftest_cosmofs_scrub_open(const char **reason)
{
    (void)reason;
    return true;
}

bool selftest_cosmofs_check_dev(const char **reason)
{
    (void)reason;
    return true;
}

bool selftest_cosmofs_scrub_modes(const char **reason)
{
    (void)reason;
    return true;
}

bool selftest_cosmofs_nomem(const char **reason)
{
    (void)reason;
    return true;
}
#endif
