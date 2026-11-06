/*
 * cosmofs_scrub_floor.c - The C under cosmofs_scrub.cee (roadmap 2.0 SA7):
 * g_cosmofs_scrub_rate, the sysctl's variable (fs.scrub_rate), which
 * CeeSafe cannot export. The scrub is CeeSafe (cosmofs_scrub.cee, generated
 * into cosmofs_scrub.c).
 */

#include <kernel/cosmofs.h>

unsigned g_cosmofs_scrub_rate = COSMOFS_SCRUB_RATE_DEFAULT;

uint32_t *cfs_scrub_floor_rate(void);

uint32_t *cfs_scrub_floor_rate(void) { return &g_cosmofs_scrub_rate; }
