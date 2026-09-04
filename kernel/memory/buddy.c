/*
 * buddy.c - Binary buddy allocator over one zone.
 *
 * A free block of order k is 2^k naturally aligned pages whose head page
 * carries PG_BUDDY and the order, and sits on free_area[k]. The buddy of
 * a block is the block at pfn ^ (1 << k); two free buddies of the same
 * order merge into one block of order k+1. All bookkeeping is in the page
 * descriptors, never in the frames themselves.
 *
 * Locking: the caller holds zone->lock. The functions assert it so a
 * missing lock is a panic, not a silent race.
 */

#include <kernel/panic.h>
#include <kernel/string.h>

#include "buddy.h"

static inline bool pfn_in_zone(const struct pmm_zone *zone, pfn_t pfn)
{
    return pfn >= zone->start_pfn && pfn < zone->end_pfn;
}

static inline void free_area_push(struct pmm_zone *zone, struct page *page, unsigned order)
{
    page->order = (uint8_t)order;
    page->flags |= PG_BUDDY;
    list_push_front(&zone->free_area[order].list, &page->buddy);
    zone->free_area[order].nr_free++;
}

static inline void free_area_remove(struct pmm_zone *zone, struct page *page, unsigned order)
{
    list_remove(&page->buddy);
    page->flags &= ~PG_BUDDY;
    KASSERT(zone->free_area[order].nr_free > 0);
    zone->free_area[order].nr_free--;
}

void buddy_zone_init(struct pmm_zone *zone, const char *name, pfn_t start_pfn, pfn_t end_pfn)
{
    memset(zone, 0, sizeof(*zone));
    zone->name = name;
    zone->start_pfn = start_pfn;
    zone->end_pfn = end_pfn;
    for (unsigned o = 0; o < PMM_MAX_ORDER; o++)
        list_init(&zone->free_area[o].list);
    spinlock_init(&zone->lock, name);
}

struct page *buddy_alloc_block(struct pmm_zone *zone, unsigned order)
{
    KASSERT(spin_is_held(&zone->lock));
    KASSERT(order < PMM_MAX_ORDER);

    for (unsigned o = order; o < PMM_MAX_ORDER; o++) {
        struct list_node *n = list_pop_front(&zone->free_area[o].list);
        if (n == NULL)
            continue;

        struct page *page = list_entry(n, struct page, buddy);
        KASSERT(page->flags & PG_BUDDY);
        KASSERT(page->order == o);
        page->flags &= ~PG_BUDDY;
        zone->free_area[o].nr_free--;

        /* Split down to the requested order, freeing the upper halves. */
        while (o > order) {
            o--;
            struct page *half = page + ((pfn_t)1 << o);
            free_area_push(zone, half, o);
        }

        page->order = (uint8_t)order;
        page->refcount = 1;
        zone->nr_pages_free -= (pfn_t)1 << order;
        return page;
    }
    return NULL;
}

void buddy_free_block(struct pmm_zone *zone, struct page *page, unsigned order)
{
    KASSERT(spin_is_held(&zone->lock));
    KASSERT(order < PMM_MAX_ORDER);

    pfn_t pfn = page_to_pfn(page);
    KASSERT(pfn_in_zone(zone, pfn));
    KASSERT(pfn + ((pfn_t)1 << order) <= zone->end_pfn);
    KASSERT((pfn & (((pfn_t)1 << order) - 1)) == 0);
    KASSERT(page->refcount == 0);
    KASSERT((page->flags & (PG_BUDDY | PG_RESERVED)) == 0);

    /* Only the block being returned adds to the free count; buddies it
     * merges with were already counted when they were freed. */
    zone->nr_pages_free += (pfn_t)1 << order;

    while (order < PMM_MAX_ORDER - 1) {
        pfn_t buddy_pfn = pfn ^ ((pfn_t)1 << order);
        if (!pfn_in_zone(zone, buddy_pfn))
            break;
        if (buddy_pfn + ((pfn_t)1 << order) > zone->end_pfn)
            break;

        struct page *buddy = pfn_to_page(buddy_pfn);
        if ((buddy->flags & PG_BUDDY) == 0 || buddy->order != order)
            break;

        free_area_remove(zone, buddy, order);
        if (buddy_pfn < pfn)
            pfn = buddy_pfn;
        order++;
    }

    free_area_push(zone, pfn_to_page(pfn), order);
}

void buddy_free_range(struct pmm_zone *zone, pfn_t start_pfn, pfn_t end_pfn)
{
    KASSERT(spin_is_held(&zone->lock));
    KASSERT(start_pfn >= zone->start_pfn && end_pfn <= zone->end_pfn);
    KASSERT(start_pfn <= end_pfn);

    for (pfn_t p = start_pfn; p < end_pfn; p++) {
        struct page *page = pfn_to_page(p);
        page->flags &= ~(PG_RESERVED | PG_DEFERRED);
        page->refcount = 0;
    }
    zone->nr_pages_total += end_pfn - start_pfn;

    pfn_t pfn = start_pfn;
    while (pfn < end_pfn) {
        /* Largest order that is both aligned at pfn and fits before end. */
        unsigned order = PMM_MAX_ORDER - 1;
        if (pfn != 0) {
            unsigned align_order = (unsigned)__builtin_ctzll(pfn);
            if (align_order < order)
                order = align_order;
        }
        while (order > 0 && pfn + ((pfn_t)1 << order) > end_pfn)
            order--;

        buddy_free_block(zone, pfn_to_page(pfn), order);
        pfn += (pfn_t)1 << order;
    }
}

bool buddy_zone_check(struct pmm_zone *zone)
{
    uint64_t free_pages = 0;

    for (unsigned o = 0; o < PMM_MAX_ORDER; o++) {
        uint64_t count = 0;
        struct list_node *n;
        list_for_each(n, &zone->free_area[o].list) {
            const struct page *page = list_entry(n, struct page, buddy);
            pfn_t pfn = page_to_pfn(page);
            if ((page->flags & PG_BUDDY) == 0 || page->order != o)
                return false;
            if (!pfn_in_zone(zone, pfn) || (pfn & (((pfn_t)1 << o) - 1)) != 0)
                return false;
            if (page->refcount != 0)
                return false;
            count++;
        }
        if (count != zone->free_area[o].nr_free)
            return false;
        free_pages += count << o;
    }
    return free_pages == zone->nr_pages_free && free_pages <= zone->nr_pages_total;
}
