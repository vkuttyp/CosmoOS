/*
 * cosmofs_rollback_floor.c - The C under cosmofs_rollback.cee (roadmap 2.0
 * SA7): the one call whose prototype CeeSafe cannot write,
 * cfs_snapshot_new_uuid, whose `also` is a pointer to arrays of 16 bytes.
 * The rollback is CeeSafe (cosmofs_rollback.cee, generated into
 * cosmofs_rollback.c).
 */

#include "cosmofs_internal.h"

void cfs_rollback_floor_new_uuid(struct cfs *fs, uint8_t *out);

void cfs_rollback_floor_new_uuid(struct cfs *fs, uint8_t *out) { cfs_snapshot_new_uuid(fs, out, NULL, 0); }
