/*
 * iommu_pt.h - The IOMMU page-table walker (docs/kernel/iommu/design.md §3):
 * a 4-level, 4 KiB-granule tree of 512-entry tables over 48 bits, the
 * entry encoding supplied by the unit's driver.
 */

#ifndef KERNEL_IOMMU_PT_H
#define KERNEL_IOMMU_PT_H

#include <kernel/types.h>

struct iommu_pt_fmt {
    uint64_t (*make_table)(paddr_t next);                 /* a non-leaf entry pointing at `next` */
    uint64_t (*make_leaf)(paddr_t pa, unsigned prot);     /* a 4 KiB leaf */
    bool (*present)(uint64_t e);
    paddr_t (*addr_of)(uint64_t e);
};

#define IOMMU_PT_LEVELS 4u
#define IOMMU_PT_ENTRIES 512u

paddr_t iommu_pt_alloc_table(void);   /* a zeroed page, 0 without memory */
/* Map `pages` 4 KiB pages; -EEXIST when one is already mapped (the pages
 * mapped by this call are undone), -ENOMEM (same). Tables are allocated
 * on demand and never freed before iommu_pt_free. Any context. */
int iommu_pt_map(paddr_t root, const struct iommu_pt_fmt *fmt, uint64_t iova, paddr_t pa, size_t pages,
                 unsigned prot);
/* Clear the leaves of `pages` pages; returns how many were present. */
size_t iommu_pt_unmap(paddr_t root, const struct iommu_pt_fmt *fmt, uint64_t iova, size_t pages);
bool iommu_pt_lookup(paddr_t root, const struct iommu_pt_fmt *fmt, uint64_t iova, paddr_t *pa);
/* Free every table of the tree, the root included (no leaf need be clear). */
void iommu_pt_free(paddr_t root, const struct iommu_pt_fmt *fmt);

#endif /* KERNEL_IOMMU_PT_H */
