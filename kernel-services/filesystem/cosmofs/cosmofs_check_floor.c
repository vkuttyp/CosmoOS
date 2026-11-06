/*
 * cosmofs_check_floor.c - The C under cosmofs_check.cee (roadmap 2.0 SA7): a
 * zeroed page from the page allocator, as the address the direct map gives
 * it, and the page back from that address. kernel/page.h's conversions are
 * inline functions over pmm_page_array and the direct map's base, which
 * CeeSafe cannot call. The check is CeeSafe (cosmofs_check.cee, generated
 * into cosmofs_check.c).
 */

#include <kernel/page.h>
#include <kernel/pmm.h>

#include "cosmofs_internal.h"

void *cfs_check_floor_page(void);
void cfs_check_floor_unpage(void *va);

void *cfs_check_floor_page(void)
{
    struct page *pg = pmm_alloc_page(PMM_FLAGS_ZERO);
    return pg == NULL ? NULL : phys_to_virt(page_to_phys(pg));
}

void cfs_check_floor_unpage(void *va)
{
    pmm_free_page(phys_to_page(virt_to_phys(va)));
}
