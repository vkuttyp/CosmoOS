/*
 * cosmofstest_copies_floor.c - The C under cosmofstest_copies.cee
 * (roadmap 2.0 SA7): the sysctl's variable fs.fault_errors
 * (g_pool_fault_errors), which the tests lower and restore; a pool's
 * first spare, from an array of pointers CeeSafe cannot declare as a
 * field; the tests' stand-ins in a build without CONFIG_DEBUG; and the
 * layout the CeeSafe half writes as its own constants, asserted here,
 * since a self-test has no host test to assert it. The tests are CeeSafe
 * (cosmofstest_copies.cee, generated into cosmofstest_copies.c).
 */

#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/ramblk.h>
#include <kernel/selftest.h>
#include <kernel/storage.h>
#include <kernel/vfs.h>

#include <uapi/cosmo/syscall.h>

#include "cosmofs_format.h"
#include "cosmofs_internal.h"

_Static_assert(EIO == 5 && EAGAIN == 11 && EBUSY == 16 && EEXIST == 17 && ENODEV == 19 && COSMO_O_RDONLY == 0 &&
                   VFS_UMOUNT_FORCE == 1 && CFS_BLOCK == 4096 && CFS_FORMAT_VERSION == 11 &&
                   CFS_DVA_VDEV_SHIFT == 56 && CFS_DVA(0, 12345) == 12345,
               "cosmofstest_copies.cee's errno.h, vfs.h and cosmofs_format.h values");
_Static_assert(POOL_COPY_ONLINE == 0 && POOL_COPY_MISSING == 1 && POOL_COPY_OFFLINE == 2 && POOL_COPY_FAULTED == 3 &&
                   POOL_COPY_RESILVERING == 4 && COSMOFS_POOL_OFFLINE == 1 && COSMOFS_POOL_ONLINE == 2 &&
                   COSMOFS_POOL_REPLACE == 3 && COSMOFS_POOL_ATTACH == 4 && COSMOFS_POOL_DETACH == 5 &&
                   COSMOFS_POOL_SPARE_ADD == 6 && COSMOFS_SCRUB_REPAIR == 1,
               "cosmofstest_copies.cee's storage.h and cosmofs.h values");
_Static_assert(sizeof(struct ramblk_write) == 24 && offsetof(struct ramblk_write, sector) == 0 &&
                   offsetof(struct ramblk_write, nsectors) == 8 && sizeof(struct cosmofs_stats) <= 128 &&
                   sizeof(struct cosmofs_scrub_stats) <= 64 && sizeof(struct cosmofs_pool_req) <= 96 &&
                   sizeof(struct cfs_super) <= 21 * 8,
               "cosmofstest_copies.cee's record sizes");

uint32_t cfst_copies_floor_fault_errors(void);
void cfst_copies_floor_set_fault_errors(uint32_t n);
struct blkdev *cfst_copies_floor_spare0(struct spool *p);

uint32_t cfst_copies_floor_fault_errors(void)
{
    return g_pool_fault_errors;
}

void cfst_copies_floor_set_fault_errors(uint32_t n)
{
    g_pool_fault_errors = n;
}

struct blkdev *cfst_copies_floor_spare0(struct spool *p)
{
    return p->spare[0];
}

#if !CONFIG_DEBUG
bool selftest_pool_copy_states(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_degraded(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_copy_bitmap(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_copy_import(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_copy_geometry(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_resilver(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_resilver_resume(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_spare(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_attach(const char **reason) { (void)reason; return true; }
#endif
