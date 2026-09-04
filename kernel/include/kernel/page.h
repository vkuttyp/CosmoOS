/*
 * page.h - Page frame descriptor and physical/virtual conversions.
 *
 * One `struct page` exists per 4 KiB frame of RAM from 0 to the end of
 * the highest RAM range. The array is owned by the PMM and lives in
 * bootmem-allocated memory for the life of the kernel.
 *
 * Free-list links live in the descriptor, not in the frame, so a frame
 * can be tracked before it is mapped (see PG_DEFERRED).
 */

#ifndef KERNEL_PAGE_H
#define KERNEL_PAGE_H

#include <kernel/list.h>
#include <kernel/types.h>

struct slab;

#define PG_RESERVED      (1u << 0) /* never allocatable */
#define PG_BUDDY         (1u << 1) /* head of a free block in a buddy list */
#define PG_SLAB          (1u << 2) /* part of a slab; page->slab is valid */
#define PG_KMALLOC_LARGE (1u << 3) /* page-backed kmalloc; order in page->order */
#define PG_PAGETABLE     (1u << 4) /* holds an MMU translation table */
#define PG_DEFERRED      (1u << 5) /* RAM not yet reachable through the direct map */

struct page {
    uint32_t flags;
    uint32_t refcount;   /* 0 when free or reserved; atomic once allocated */
    uint8_t  order;      /* free: buddy order; allocated: allocation order */
    uint8_t  zone;       /* enum pmm_zone_id */
    uint16_t reserved0;
    uint32_t reserved1;
    union {
        struct list_node buddy;  /* PG_BUDDY */
        struct slab *slab;       /* PG_SLAB */
    };
};

STATIC_ASSERT(sizeof(struct page) == 32, "struct page must stay 32 bytes");

/* Set by pmm_init. */
extern struct page *pmm_page_array;
extern pfn_t pmm_max_pfn;

static inline bool pfn_valid(pfn_t pfn)
{
    return pfn < pmm_max_pfn;
}

static inline pfn_t page_to_pfn(const struct page *page)
{
    return (pfn_t)(page - pmm_page_array);
}

static inline struct page *pfn_to_page(pfn_t pfn)
{
    return &pmm_page_array[pfn];
}

static inline paddr_t page_to_phys(const struct page *page)
{
    return pfn_to_phys(page_to_pfn(page));
}

/* NULL if the address has no descriptor (beyond RAM). */
static inline struct page *phys_to_page(paddr_t pa)
{
    pfn_t pfn = phys_to_pfn(pa);
    return pfn_valid(pfn) ? pfn_to_page(pfn) : NULL;
}

/* Direct-map conversions. Valid for RAM the direct map covers: below
 * 4 GiB before vmm_init, all RAM after. The VMM sets the bounds. */
extern vaddr_t pmm_hhdm_base;
extern paddr_t pmm_hhdm_limit;

static inline void *phys_to_virt(paddr_t pa)
{
    return (void *)(pmm_hhdm_base + pa);
}

static inline paddr_t virt_to_phys(const void *va)
{
    return (paddr_t)((vaddr_t)va - pmm_hhdm_base);
}

static inline void *page_to_virt(const struct page *page)
{
    return phys_to_virt(page_to_phys(page));
}

static inline struct page *virt_to_page(const void *va)
{
    return phys_to_page(virt_to_phys(va));
}

static inline bool phys_in_direct_map(paddr_t pa)
{
    return pa < pmm_hhdm_limit;
}

/* True for a virtual address inside the direct map's RAM span. */
static inline bool virt_is_direct_map(vaddr_t va)
{
    return va >= pmm_hhdm_base && va - pmm_hhdm_base < pmm_hhdm_limit;
}

#endif /* KERNEL_PAGE_H */
