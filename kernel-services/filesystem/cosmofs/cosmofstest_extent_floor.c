/*
 * cosmofstest_extent_floor.c - The C under cosmofstest_extent.cee
 * (roadmap 2.0 SA7): the faults cosmofs-extent-fail injects into
 * set_extent, armed and cleared here because only a build with
 * CONFIG_FAULTINJECT has them, and the layout the CeeSafe half writes as
 * its own constants, asserted here, since a self-test has no host test
 * to assert it. The tests are CeeSafe (cosmofstest_extent.cee, generated
 * into cosmofstest_extent.c).
 */

#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/faultinject.h>
#include <kernel/pagecache.h>
#include <kernel/thread.h>
#include <kernel/vfs.h>

#include <uapi/cosmo/syscall.h>

#include "cosmofs_format.h"

_Static_assert(EFBIG == 27 && ENOMEM == 12 && EEXIST == 17 && COSMO_O_RDONLY == 0 && COSMO_O_RDWR == 2 &&
                   COSMO_O_CREAT == 0x40 && COSMO_O_TRUNC == 0x200 && VFS_UMOUNT_FORCE == 1 &&
                   COSMOFS_SCRUB_REPAIR == 1 && CFS_RECORD_BLOCKS == 8,
               "cosmofstest_extent.cee's errno.h, syscall.h, vfs.h, cosmofs.h and cosmofs_format.h values");
_Static_assert(sizeof(struct cosmofs_check_report) <= 1536 && sizeof(struct cosmofs_stats) <= 128 &&
                   offsetof(struct cosmofs_stats, free_blocks) == 8 &&
                   offsetof(struct cosmofs_stats, csum_failures) == 64 && sizeof(struct cosmofs_scrub_stats) <= 128 &&
                   offsetof(struct cosmofs_scrub_stats, unrecoverable) == 24 &&
                   sizeof(struct pagecache_stats) <= 128 && offsetof(struct pagecache_stats, wb_errors) == 48 &&
                   offsetof(struct pagecache_stats, dropped_dirty) == 56,
               "cosmofstest_extent.cee's report, stats and page cache stats sizes and offsets");

bool cfst_extent_floor_faults(void);
void cfst_extent_floor_set(uint32_t k, uint32_t every, uint32_t count);
uint64_t cfst_extent_floor_clear(uint32_t k);
const char *cfst_extent_floor_name(uint32_t k);

bool cfst_extent_floor_faults(void)
{
    return CONFIG_FAULTINJECT != 0;
}

#if CONFIG_FAULTINJECT
/* cosmofstest_extent.cee's fault numbers: 0 and 1. */
static enum fi_kind extent_kind(uint32_t k)
{
    return k == 0 ? FI_COSMOFS_EXTENT_NOMEM : FI_COSMOFS_EXTENT_FBIG;
}

/* Fault `k` armed for this thread: the `every`th hit fails, `count` times. */
void cfst_extent_floor_set(uint32_t k, uint32_t every, uint32_t count)
{
    faultinject_set(extent_kind(k), every, count, thread_current());
}

/* Fault `k`'s hits since it was armed; then it is cleared. */
uint64_t cfst_extent_floor_clear(uint32_t k)
{
    struct fi_stats fst;
    faultinject_stats(extent_kind(k), &fst);
    faultinject_clear(extent_kind(k));
    return fst.hits;
}

const char *cfst_extent_floor_name(uint32_t k)
{
    return faultinject_kind_name(extent_kind(k));
}
#else
void cfst_extent_floor_set(uint32_t k, uint32_t every, uint32_t count)
{
    (void)k;
    (void)every;
    (void)count;
}

uint64_t cfst_extent_floor_clear(uint32_t k)
{
    (void)k;
    return 0;
}

const char *cfst_extent_floor_name(uint32_t k)
{
    (void)k;
    return "";
}
#endif
