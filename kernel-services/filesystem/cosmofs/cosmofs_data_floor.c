/*
 * cosmofs_data_floor.c - The C under cosmofs_data.cee (roadmap 2.0 SA7):
 * what a build configuration decides about a file's runs, which CeeSafe
 * cannot name -- the test seams at the top of a map change and of a map
 * store (kernel/faultinject.h makes them inline functions returning false
 * without fault injection), the debug build's note that the -EFBIG seam
 * fired, and the run bound a self-test may lower on its mount; and
 * cfs_set_extent, whose run comes by value, which CeeSafe cannot take of
 * a C struct. A file's blocks are CeeSafe (cosmofs_data.cee, generated
 * into cosmofs_data.c).
 */

#include <kernel/errno.h>
#include <kernel/faultinject.h>

#include "cosmofs_internal.h"

int cfs_data_floor_set_extent_fault(struct cfs *fs);
bool cfs_data_floor_chain_nomem(void);
unsigned cfs_data_floor_max_extents(const struct cfs *fs);
int cfs_data_set_extent(struct cfs *fs, struct cfs_inode *in, const struct cfs_extent *ne, uint64_t *old);

/* cosmofs_internal.h's cfs_set_extent, by pointer (cosmofs_data.cee). */
int cfs_set_extent(struct cfs *fs, struct cfs_inode *in, struct cfs_extent ne, uint64_t *old)
{
    return cfs_data_set_extent(fs, in, &ne, old);
}

/* The test seam at the top of cfs_set_extent (cosmofs-extent-fail): an
 * armed rule makes it fail as it can before it changes anything, -ENOMEM
 * (an allocation) or -EFBIG (a map that would not fit). One load per kind
 * when nothing is armed; nothing in a release build. */
int cfs_data_floor_set_extent_fault(struct cfs *fs)
{
    int rc = 0;
    if (faultinject_should_fail(FI_COSMOFS_EXTENT_NOMEM))
        rc = -ENOMEM;
    else if (faultinject_should_fail(FI_COSMOFS_EXTENT_FBIG))
        rc = -EFBIG;
#if CONFIG_DEBUG
    if (rc)
        fs->fbig_injected = rc == -EFBIG;
#else
    (void)fs;
#endif
    return rc;
}

/* The test seam (cosmofs-nomem): the chain block a map store needs cannot
 * be had. */
bool cfs_data_floor_chain_nomem(void)
{
    return faultinject_should_fail(FI_COSMOFS_CHAIN_NOMEM);
}

/* Runs an inode may have (cosmofs_internal.h, cfs_max_extents). */
unsigned cfs_data_floor_max_extents(const struct cfs *fs)
{
    return cfs_max_extents(fs);
}
