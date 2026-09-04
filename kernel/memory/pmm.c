/*
 * pmm.c - Physical memory manager front end.
 *
 * Turns the boot memory map into the page array and zones, then serves
 * allocations from the buddy with zone fallback. Reserved and deferred
 * accounting lives here; the buddy itself is in buddy.c.
 */

#include <kernel/bootinfo.h>
#include <kernel/log.h>
#include <kernel/page.h>
#include <kernel/panic.h>
#include <kernel/pmm.h>
#include <kernel/string.h>

#include "bootmem.h"
#include "buddy.h"

struct page *pmm_page_array;
pfn_t pmm_max_pfn;
vaddr_t pmm_hhdm_base;
paddr_t pmm_hhdm_limit;

static struct pmm_node g_node;
static uint64_t g_reserved_pages;
static uint64_t g_deferred_pages;
static bool g_initialized;

static const char *const zone_names[PMM_ZONE_COUNT] = {
    [PMM_ZONE_DMA] = "DMA",
    [PMM_ZONE_DMA32] = "DMA32",
    [PMM_ZONE_NORMAL] = "Normal",
};

enum pmm_zone_id pmm_zone_of(paddr_t pa)
{
    if (pa < PMM_ZONE_DMA_LIMIT)
        return PMM_ZONE_DMA;
    if (pa < PMM_ZONE_DMA32_LIMIT)
        return PMM_ZONE_DMA32;
    return PMM_ZONE_NORMAL;
}

const char *pmm_zone_name(enum pmm_zone_id id)
{
    return (unsigned)id < PMM_ZONE_COUNT ? zone_names[id] : "?";
}

static struct pmm_zone *zone_for_pfn(pfn_t pfn)
{
    return &g_node.zones[pmm_zone_of(pfn_to_phys(pfn))];
}

/* Release [start, end) into the buddy, splitting at zone boundaries.
 * Pages must currently be reserved or deferred. */
static void release_range(pfn_t start, pfn_t end)
{
    while (start < end) {
        struct pmm_zone *zone = zone_for_pfn(start);
        pfn_t chunk_end = end < zone->end_pfn ? end : zone->end_pfn;

        for (pfn_t p = start; p < chunk_end; p++) {
            struct page *page = pfn_to_page(p);
            KASSERT(page->flags & (PG_RESERVED | PG_DEFERRED));
            KASSERT(page->refcount == 0);
            if (page->flags & PG_RESERVED)
                g_reserved_pages--;
            if (page->flags & PG_DEFERRED)
                g_deferred_pages--;
        }

        arch_irq_state_t s = spin_lock_irqsave(&zone->lock);
        buddy_free_range(zone, start, chunk_end);
        spin_unlock_irqrestore(&zone->lock, s);

        start = chunk_end;
    }
}

static void init_zones(pfn_t max_pfn)
{
    pfn_t dma_end = phys_to_pfn(PMM_ZONE_DMA_LIMIT);
    pfn_t dma32_end = phys_to_pfn(PMM_ZONE_DMA32_LIMIT);

    if (dma_end > max_pfn)
        dma_end = max_pfn;
    if (dma32_end > max_pfn)
        dma32_end = max_pfn;

    buddy_zone_init(&g_node.zones[PMM_ZONE_DMA], zone_names[PMM_ZONE_DMA], 0, dma_end);
    buddy_zone_init(&g_node.zones[PMM_ZONE_DMA32], zone_names[PMM_ZONE_DMA32], dma_end, dma32_end);
    buddy_zone_init(&g_node.zones[PMM_ZONE_NORMAL], zone_names[PMM_ZONE_NORMAL], dma32_end, max_pfn);
}

static void mark_deferred(void)
{
    uint32_t n;
    const struct cosmoboot_mem_entry *map = bootinfo_mem_map(&n);

    for (uint32_t i = 0; i < n; i++) {
        if (map[i].type != COSMOBOOT_MEM_USABLE && map[i].type != COSMOBOOT_MEM_LOADER_RECLAIMABLE)
            continue;
        paddr_t start = map[i].base;
        paddr_t end = map[i].base + map[i].length;
        if (start < pmm_hhdm_limit)
            start = pmm_hhdm_limit;
        if (start >= end)
            continue;
        for (pfn_t p = phys_to_pfn(start); p < phys_to_pfn(end); p++) {
            struct page *page = pfn_to_page(p);
            page->flags = (page->flags & ~PG_RESERVED) | PG_DEFERRED;
            g_reserved_pages--;
            g_deferred_pages++;
        }
    }
}

void pmm_init(void)
{
    const struct cosmoboot_info *info = bootinfo_get();

    KASSERT(!g_initialized);

    pmm_hhdm_base = (vaddr_t)info->hhdm_base;
    pmm_hhdm_limit = info->hhdm_size;

    paddr_t limit = bootinfo_phys_limit();
    pmm_max_pfn = phys_to_pfn(page_align_up(limit));
    if (pmm_max_pfn == 0)
        panic("pmm: memory map describes no RAM");

    bootmem_init();

    size_t array_bytes = (size_t)pmm_max_pfn * sizeof(struct page);
    pmm_page_array = bootmem_alloc(array_bytes, PAGE_SIZE);

    /* Everything starts reserved; releases below make memory usable. */
    for (pfn_t p = 0; p < pmm_max_pfn; p++) {
        struct page *page = &pmm_page_array[p];
        page->flags = PG_RESERVED;
        page->refcount = 0;
        page->order = 0;
        page->zone = (uint8_t)pmm_zone_of(pfn_to_phys(p));
    }
    g_reserved_pages = pmm_max_pfn;
    g_deferred_pages = 0;

    init_zones(pmm_max_pfn);
    mark_deferred();
    bootmem_release_all(release_range);
    bootmem_seal();
    g_initialized = true;

    struct pmm_stats st;
    pmm_get_stats(&st);
    kinfo("pmm: %llu MiB RAM span, %llu MiB free, %llu MiB reserved, %llu MiB deferred, page array %llu KiB",
          (unsigned long long)(pfn_to_phys(pmm_max_pfn) >> 20),
          (unsigned long long)((st.free_pages * PAGE_SIZE) >> 20),
          (unsigned long long)((st.reserved_pages * PAGE_SIZE) >> 20),
          (unsigned long long)((st.deferred_pages * PAGE_SIZE) >> 20),
          (unsigned long long)(array_bytes >> 10));
}

struct page *pmm_alloc_pages(unsigned order, unsigned flags)
{
    KASSERT(g_initialized);

    if (order >= PMM_MAX_ORDER)
        return NULL;

    int top = PMM_ZONE_NORMAL;
    if (flags & PMM_FLAGS_ZONE_DMA)
        top = PMM_ZONE_DMA;
    else if (flags & PMM_FLAGS_ZONE_DMA32)
        top = PMM_ZONE_DMA32;

    struct page *page = NULL;
    for (int z = top; z >= 0 && page == NULL; z--) {
        struct pmm_zone *zone = &g_node.zones[z];
        arch_irq_state_t s = spin_lock_irqsave(&zone->lock);
        page = buddy_alloc_block(zone, order);
        spin_unlock_irqrestore(&zone->lock, s);
    }
    if (page == NULL)
        return NULL;

    if (flags & PMM_FLAGS_ZERO) {
        size_t bytes = PAGE_SIZE << order;
        KASSERT(phys_in_direct_map(page_to_phys(page) + bytes - 1));
        memset(page_to_virt(page), 0, bytes);
    }
    return page;
}

void pmm_free_pages(struct page *page, unsigned order)
{
    KASSERT(g_initialized);
    KASSERT(page != NULL);
    KASSERT(order < PMM_MAX_ORDER);

    if (page->flags & PG_RESERVED)
        panic("pmm: freeing reserved page pfn %llu", (unsigned long long)page_to_pfn(page));
    if (page->flags & PG_BUDDY)
        panic("pmm: double free of pfn %llu", (unsigned long long)page_to_pfn(page));
    if (page->flags & (PG_SLAB | PG_KMALLOC_LARGE | PG_PAGETABLE))
        panic("pmm: pfn %llu still owned (flags 0x%x)", (unsigned long long)page_to_pfn(page), page->flags);
    if (page->refcount != 1)
        panic("pmm: freeing pfn %llu with refcount %u", (unsigned long long)page_to_pfn(page), page->refcount);
    if (page->order != order)
        panic("pmm: pfn %llu allocated order %u, freed as order %u",
              (unsigned long long)page_to_pfn(page), page->order, order);

    page->refcount = 0;

    struct pmm_zone *zone = zone_for_pfn(page_to_pfn(page));
    arch_irq_state_t s = spin_lock_irqsave(&zone->lock);
    buddy_free_block(zone, page, order);
    spin_unlock_irqrestore(&zone->lock, s);
}

void pmm_page_get(struct page *page)
{
    uint32_t old = __atomic_fetch_add(&page->refcount, 1u, __ATOMIC_ACQ_REL);
    if (old == 0)
        panic("pmm: get on free pfn %llu", (unsigned long long)page_to_pfn(page));
}

void pmm_page_put(struct page *page)
{
    uint32_t old = __atomic_load_n(&page->refcount, __ATOMIC_ACQUIRE);
    if (old == 0)
        panic("pmm: put on free pfn %llu", (unsigned long long)page_to_pfn(page));
    if (old == 1) {
        /* Last reference: pmm_free_pages expects refcount 1. */
        pmm_free_pages(page, page->order);
        return;
    }
    __atomic_fetch_sub(&page->refcount, 1u, __ATOMIC_ACQ_REL);
}

void pmm_release_deferred(void)
{
    KASSERT(g_initialized);

    pfn_t run_start = 0;
    bool in_run = false;
    uint64_t released = 0;

    for (pfn_t p = 0; p <= pmm_max_pfn; p++) {
        bool deferred = p < pmm_max_pfn && (pmm_page_array[p].flags & PG_DEFERRED);
        if (deferred && !in_run) {
            run_start = p;
            in_run = true;
        } else if (!deferred && in_run) {
            KASSERT(phys_in_direct_map(pfn_to_phys(p) - 1));
            release_range(run_start, p);
            released += p - run_start;
            in_run = false;
        }
    }

    if (released)
        kinfo("pmm: released %llu MiB of deferred memory", (unsigned long long)((released * PAGE_SIZE) >> 20));
    KASSERT(g_deferred_pages == 0);
}

void pmm_free_reserved_range(paddr_t base, size_t size)
{
    KASSERT(g_initialized);
    KASSERT(is_page_aligned(base) && is_page_aligned(size));

    pfn_t start = phys_to_pfn(base);
    pfn_t end = phys_to_pfn(base + size);
    KASSERT(end <= pmm_max_pfn);

    for (pfn_t p = start; p < end; p++) {
        struct page *page = pfn_to_page(p);
        if ((page->flags & PG_RESERVED) == 0 || page->refcount != 0)
            panic("pmm: pfn %llu is not a reserved page", (unsigned long long)p);
    }
    release_range(start, end);
}

void pmm_get_stats(struct pmm_stats *out)
{
    memset(out, 0, sizeof(*out));
    for (unsigned z = 0; z < PMM_ZONE_COUNT; z++) {
        struct pmm_zone *zone = &g_node.zones[z];
        arch_irq_state_t s = spin_lock_irqsave(&zone->lock);
        out->total_pages += zone->nr_pages_total;
        out->free_pages += zone->nr_pages_free;
        out->zone_free[z] = zone->nr_pages_free;
        spin_unlock_irqrestore(&zone->lock, s);
    }
    out->reserved_pages = g_reserved_pages;
    out->deferred_pages = g_deferred_pages;
}

void pmm_dump(void)
{
    for (unsigned z = 0; z < PMM_ZONE_COUNT; z++) {
        struct pmm_zone *zone = &g_node.zones[z];
        arch_irq_state_t s = spin_lock_irqsave(&zone->lock);
        bool ok = buddy_zone_check(zone);
        kprintf("zone %-6s pfn [%llu, %llu) total %llu free %llu %s\n", zone->name,
                (unsigned long long)zone->start_pfn, (unsigned long long)zone->end_pfn,
                (unsigned long long)zone->nr_pages_total, (unsigned long long)zone->nr_pages_free,
                ok ? "" : "INCONSISTENT");
        kprintf("  order:");
        for (unsigned o = 0; o < PMM_MAX_ORDER; o++)
            kprintf(" %llu", (unsigned long long)zone->free_area[o].nr_free);
        kprintf("\n");
        spin_unlock_irqrestore(&zone->lock, s);
    }
    kprintf("reserved %llu pages, deferred %llu pages\n",
            (unsigned long long)g_reserved_pages, (unsigned long long)g_deferred_pages);
}
