/*
 * cosmofs_resilver_floor.c - The C under cosmofs_resilver.cee (roadmap 2.0
 * SA7): g_cosmofs_resilver_rate, the sysctl's variable (fs.resilver_rate),
 * which CeeSafe cannot export, and the call to cfs_pace, which CeeSafe cannot
 * declare beside struct cfs_pace (one name for both). The resilver is CeeSafe
 * (cosmofs_resilver.cee, generated into cosmofs_resilver.c).
 */

#include <kernel/cosmofs.h>

#include "cosmofs_internal.h"

unsigned g_cosmofs_resilver_rate = COSMOFS_RESILVER_RATE_DEFAULT;

uint32_t *cfs_resilver_floor_rate(void);
bool cfs_resilver_floor_pace(struct cfs *fs, struct cfs_pace *p);

uint32_t *cfs_resilver_floor_rate(void) { return &g_cosmofs_resilver_rate; }

bool cfs_resilver_floor_pace(struct cfs *fs, struct cfs_pace *p) { return cfs_pace(fs, p); }
