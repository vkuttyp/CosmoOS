/* pt.c - The IOMMU page-table walker (pt.h; docs/kernel/iommu/design.md §3). */

#include <kernel/errno.h>
#include <kernel/page.h>
#include <kernel/pmm.h>
#include <kernel/string.h>

#include <kernel/iommu_pt.h>

static uint64_t *table_va(paddr_t pa)
{
    return phys_to_virt(pa);
}

static unsigned index_at(uint64_t iova, unsigned level)   /* level 0 = top */
{
    return (unsigned)((iova >> (12 + 9 * (IOMMU_PT_LEVELS - 1 - level))) & (IOMMU_PT_ENTRIES - 1));
}

paddr_t iommu_pt_alloc_table(void)
{
    struct page *pg = pmm_alloc_page(PMM_FLAGS_ZERO);
    return pg ? page_to_phys(pg) : 0;
}

/* The leaf table for `iova`, allocating the levels above when asked. */
static uint64_t *leaf_table(paddr_t root, const struct iommu_pt_fmt *fmt, uint64_t iova, bool alloc)
{
    paddr_t table = root;
    for (unsigned level = 0; level < IOMMU_PT_LEVELS - 1; level++) {
        uint64_t *t = table_va(table);
        unsigned i = index_at(iova, level);
        uint64_t e = __atomic_load_n(&t[i], __ATOMIC_ACQUIRE);
        if (!fmt->present(e)) {
            if (!alloc)
                return NULL;
            paddr_t next = iommu_pt_alloc_table();
            if (next == 0)
                return NULL;
            e = fmt->make_table(next);
            __atomic_store_n(&t[i], e, __ATOMIC_RELEASE);
        }
        table = fmt->addr_of(e);
    }
    return table_va(table);
}

int iommu_pt_map(paddr_t root, const struct iommu_pt_fmt *fmt, uint64_t iova, paddr_t pa, size_t pages,
                 unsigned prot)
{
    if ((iova & (PAGE_SIZE - 1)) || (pa & (PAGE_SIZE - 1)) || pages == 0 || iova >> 48)
        return -EINVAL;
    for (size_t k = 0; k < pages; k++) {
        uint64_t va = iova + (uint64_t)k * PAGE_SIZE;
        uint64_t *t = leaf_table(root, fmt, va, true);
        int rc = 0;
        if (t == NULL)
            rc = -ENOMEM;
        else if (fmt->present(t[index_at(va, IOMMU_PT_LEVELS - 1)]))
            rc = -EEXIST;
        if (rc) {
            iommu_pt_unmap(root, fmt, iova, k);
            return rc;
        }
        __atomic_store_n(&t[index_at(va, IOMMU_PT_LEVELS - 1)], fmt->make_leaf(pa + (paddr_t)k * PAGE_SIZE, prot),
                         __ATOMIC_RELEASE);
    }
    return 0;
}

size_t iommu_pt_unmap(paddr_t root, const struct iommu_pt_fmt *fmt, uint64_t iova, size_t pages)
{
    size_t cleared = 0;
    for (size_t k = 0; k < pages; k++) {
        uint64_t va = iova + (uint64_t)k * PAGE_SIZE;
        uint64_t *t = leaf_table(root, fmt, va, false);
        if (t == NULL)
            continue;
        unsigned i = index_at(va, IOMMU_PT_LEVELS - 1);
        if (fmt->present(t[i])) {
            __atomic_store_n(&t[i], 0, __ATOMIC_RELEASE);
            cleared++;
        }
    }
    return cleared;
}

bool iommu_pt_lookup(paddr_t root, const struct iommu_pt_fmt *fmt, uint64_t iova, paddr_t *pa)
{
    uint64_t *t = leaf_table(root, fmt, iova, false);
    if (t == NULL)
        return false;
    uint64_t e = __atomic_load_n(&t[index_at(iova, IOMMU_PT_LEVELS - 1)], __ATOMIC_ACQUIRE);
    if (!fmt->present(e))
        return false;
    if (pa)
        *pa = fmt->addr_of(e) | (iova & (PAGE_SIZE - 1));
    return true;
}

static void free_level(paddr_t table, const struct iommu_pt_fmt *fmt, unsigned level)
{
    if (level < IOMMU_PT_LEVELS - 1) {
        uint64_t *t = table_va(table);
        for (unsigned i = 0; i < IOMMU_PT_ENTRIES; i++)
            if (fmt->present(t[i]))
                free_level(fmt->addr_of(t[i]), fmt, level + 1);
    }
    pmm_free_page(phys_to_page(table));
}

void iommu_pt_free(paddr_t root, const struct iommu_pt_fmt *fmt)
{
    if (root)
        free_level(root, fmt, 0);
}
