/*
 * cosmofs_csum_floor.c - The C under cosmofs_csum.cee (roadmap 2.0 SA7):
 * the one test seam the checksums have, cosmofs-encrypted-root's "a copy
 * of a checksum index block cannot be had", which kernel/faultinject.h
 * makes an inline function returning false in a build without fault
 * injection. The checksums are CeeSafe (cosmofs_csum.cee, generated into
 * cosmofs_csum.c).
 */

#include <kernel/faultinject.h>

#include "cosmofs_internal.h"

bool cfs_csum_floor_cow_fails(void);

bool cfs_csum_floor_cow_fails(void)
{
    return faultinject_should_fail(FI_COSMOFS_CSUM_COW);
}
