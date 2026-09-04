/*
 * buddy.h - Buddy allocator core, private to kernel/memory and the host
 * unit tests.
 *
 * Pure algorithms over one zone's free lists and the page array. Every
 * function requires the caller to hold zone->lock (asserted). No logging,
 * no I/O, so tests/host/test_buddy.c compiles this file unchanged.
 */

#ifndef KERNEL_MEMORY_BUDDY_H
#define KERNEL_MEMORY_BUDDY_H

#include <kernel/pmm.h>

void buddy_zone_init(struct pmm_zone *zone, const char *name, pfn_t start_pfn, pfn_t end_pfn);

/* Take a block of `order` pages. Sets refcount 1, clears PG_BUDDY,
 * records the order. NULL if no block of that order or larger exists. */
struct page *buddy_alloc_block(struct pmm_zone *zone, unsigned order);

/* Return a block. `page` must be the naturally aligned head of a block
 * inside the zone with refcount 0 and no PG_RESERVED/PG_BUDDY flags. */
void buddy_free_block(struct pmm_zone *zone, struct page *page, unsigned order);

/* Add [start, end) to the zone as free memory. Clears PG_RESERVED and
 * PG_DEFERRED on each page, counts them as managed, and inserts maximal
 * naturally aligned blocks. The range must lie inside the zone. */
void buddy_free_range(struct pmm_zone *zone, pfn_t start_pfn, pfn_t end_pfn);

/* Verify free-list bookkeeping: every listed block is PG_BUDDY with the
 * right order and inside the zone, and the per-order counts and total
 * agree. For tests and pmm_dump. Returns true when consistent. */
bool buddy_zone_check(struct pmm_zone *zone);

#endif /* KERNEL_MEMORY_BUDDY_H */
