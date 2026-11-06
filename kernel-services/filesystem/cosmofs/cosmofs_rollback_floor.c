/*
 * cosmofs_rollback_floor.c - The C under cosmofs_rollback.cee (roadmap 2.0
 * SA7): cfs_snapshot_new_uuid with no `also`, from when that parameter
 * was a pointer to arrays of 16 bytes, which CeeSafe could not declare.
 * The rollback is CeeSafe (cosmofs_rollback.cee, generated into
 * cosmofs_rollback.c).
 */

#include "cosmofs_internal.h"

void cfs_rollback_floor_new_uuid(struct cfs *fs, uint8_t *out);

void cfs_rollback_floor_new_uuid(struct cfs *fs, uint8_t *out) { cfs_snapshot_new_uuid(fs, out, NULL, 0); }
