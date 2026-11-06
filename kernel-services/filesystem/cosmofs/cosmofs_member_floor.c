/*
 * cosmofs_member_floor.c - The C under cosmofs_member.cee (roadmap 2.0 SA7):
 * the two calls whose prototypes CeeSafe cannot write, cfs_mac_seal and
 * cfs_mac_ok over a label, whose domain is a C char, and the checks that the
 * copy record holds the pool's bitmap and states. The members are CeeSafe
 * (cosmofs_member.cee, generated into cosmofs_member.c).
 */

#include <kernel/storage.h>

#include "cosmofs_internal.h"

_Static_assert(CFS_COPY_DIRTY_BYTES == POOL_DIRTY_BYTES, "the record holds the pool's bitmap");
_Static_assert(sizeof(struct cfs_copy_rec) == 1000, "struct cfs_copy_rec is 1000 bytes");
_Static_assert(sizeof(struct cfs_copystate) <= CFS_BLOCK - CFS_MHDR_SIZE, "a copy state fits one block");
_Static_assert(CFS_MAX_COPIES == 4 && POOL_MAX_COPIES == 4, "struct cfs_copystate holds four copies");
_Static_assert(CFS_COPY_ONLINE == POOL_COPY_ONLINE && CFS_COPY_MISSING == POOL_COPY_MISSING &&
                   CFS_COPY_OFFLINE == POOL_COPY_OFFLINE && CFS_COPY_FAULTED == POOL_COPY_FAULTED &&
                   CFS_COPY_RESILVERING == POOL_COPY_RESILVERING,
               "the record's states are the pool's");

void cfs_member_floor_seal(const struct cfs_mac *mac, uint8_t *block);
bool cfs_member_floor_sealed(const struct cfs_mac *mac, const uint8_t *block);

void cfs_member_floor_seal(const struct cfs_mac *mac, uint8_t *block)
{
    cfs_mac_seal(mac, CFS_MAC_LABEL, block, offsetof(struct cfs_label, crc));
}

bool cfs_member_floor_sealed(const struct cfs_mac *mac, const uint8_t *block)
{
    return cfs_mac_ok(mac, CFS_MAC_LABEL, block, offsetof(struct cfs_label, crc));
}
