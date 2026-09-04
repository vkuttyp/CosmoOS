/*
 * pmm.h - Physical memory manager: zones and the buddy allocator.
 *
 * Contracts (see docs/kernel/memory/api.md for the full table):
 *   - All functions are non-blocking and interrupt-safe; they take the
 *     zone spinlock with interrupts disabled.
 *   - Allocation failure returns NULL. Misuse (double free, wrong order,
 *     freeing reserved memory) panics.
 *   - Returned blocks are naturally aligned to their order.
 */

#ifndef KERNEL_PMM_H
#define KERNEL_PMM_H

#include <kernel/list.h>
#include <kernel/page.h>
#include <kernel/spinlock.h>
#include <kernel/types.h>

/* Orders 0 .. PMM_MAX_ORDER-1: 4 KiB .. 4 MiB blocks. */
#define PMM_MAX_ORDER 11u

enum pmm_zone_id {
    PMM_ZONE_DMA,     /* [0, 16 MiB)   legacy ISA DMA */
    PMM_ZONE_DMA32,   /* [16 MiB, 4 GiB) 32-bit device DMA */
    PMM_ZONE_NORMAL,  /* [4 GiB, ...) */
    PMM_ZONE_COUNT
};

#define PMM_ZONE_DMA_LIMIT   ((paddr_t)16 << 20)
#define PMM_ZONE_DMA32_LIMIT ((paddr_t)4 << 30)

/* Allocation flags. */
#define PMM_FLAGS_ZONE_NORMAL 0u          /* highest zone; falls back downward */
#define PMM_FLAGS_ZONE_DMA32  (1u << 0)   /* must be below 4 GiB */
#define PMM_FLAGS_ZONE_DMA    (1u << 1)   /* must be below 16 MiB */
#define PMM_FLAGS_ZERO        (1u << 2)   /* zero the block through the direct map */
#define PMM_FLAGS_ZONE_MASK   (PMM_FLAGS_ZONE_DMA32 | PMM_FLAGS_ZONE_DMA)

struct pmm_free_area {
    struct list_node list;
    uint64_t nr_free;   /* blocks of this order */
};

struct pmm_zone {
    const char *name;
    pfn_t start_pfn;
    pfn_t end_pfn;      /* exclusive */
    struct pmm_free_area free_area[PMM_MAX_ORDER];
    uint64_t nr_pages_total;   /* managed (non-reserved) pages */
    uint64_t nr_pages_free;
    spinlock_t lock;
};

/* The future NUMA unit. There is exactly one. */
struct pmm_node {
    struct pmm_zone zones[PMM_ZONE_COUNT];
};

struct pmm_stats {
    uint64_t total_pages;      /* managed by the buddy */
    uint64_t free_pages;
    uint64_t reserved_pages;   /* PG_RESERVED */
    uint64_t deferred_pages;   /* PG_DEFERRED, not yet released */
    uint64_t zone_free[PMM_ZONE_COUNT];
};

/* One-time setup from the boot memory map. Panics if RAM cannot hold the
 * page array. Must run before interrupts are enabled. */
void pmm_init(void);

struct page *pmm_alloc_pages(unsigned order, unsigned flags);
void pmm_free_pages(struct page *page, unsigned order);

static inline struct page *pmm_alloc_page(unsigned flags)
{
    return pmm_alloc_pages(0, flags);
}

static inline void pmm_free_page(struct page *page)
{
    pmm_free_pages(page, 0);
}

/* Reference counting for shared frames. put() frees at zero using the
 * stored allocation order. */
void pmm_page_get(struct page *page);
void pmm_page_put(struct page *page);

/* Release PG_DEFERRED frames once the direct map covers them. */
void pmm_release_deferred(void);

/* Return a PG_RESERVED range to the buddy (boot page tables after the
 * VMM has taken over). Panics if any page is not reserved. */
void pmm_free_reserved_range(paddr_t base, size_t size);

void pmm_get_stats(struct pmm_stats *out);
void pmm_dump(void);

enum pmm_zone_id pmm_zone_of(paddr_t pa);
const char *pmm_zone_name(enum pmm_zone_id id);

#endif /* KERNEL_PMM_H */
