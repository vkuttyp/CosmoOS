/*
 * bootmem.c - One-shot allocator used while the page array is being built.
 *
 * Free ranges come from USABLE and LOADER_RECLAIMABLE memory-map entries
 * clipped to [1 MiB, direct-map limit). Allocation is top-down from the
 * highest range that fits, keeping low memory (ZONE_DMA) untouched.
 * Allocated memory is never freed: the page descriptors default to
 * reserved, so a bootmem allocation is reserved by omission when the
 * remaining ranges are released into the buddy.
 */

#include <kernel/bootinfo.h>
#include <kernel/log.h>
#include <kernel/page.h>
#include <kernel/panic.h>
#include <kernel/pmm.h>
#include <kernel/string.h>

#include "bootmem.h"

#define BOOTMEM_MAX_RANGES 256
#define BOOTMEM_LOW_LIMIT  ((paddr_t)1 << 20)

struct bootmem_range {
    paddr_t start;
    paddr_t end;   /* exclusive */
};

static struct bootmem_range g_ranges[BOOTMEM_MAX_RANGES];
static unsigned g_range_count;
static uint64_t g_allocated_bytes;
static bool g_sealed;

static void add_range(paddr_t start, paddr_t end)
{
    if (start >= end)
        return;
    /* Merge with the previous range when adjacent; the map is sorted. */
    if (g_range_count > 0 && g_ranges[g_range_count - 1].end == start) {
        g_ranges[g_range_count - 1].end = end;
        return;
    }
    if (g_range_count >= BOOTMEM_MAX_RANGES)
        panic("bootmem: more than %u free ranges", BOOTMEM_MAX_RANGES);
    g_ranges[g_range_count].start = start;
    g_ranges[g_range_count].end = end;
    g_range_count++;
}

void bootmem_init(void)
{
    uint32_t n;
    const struct cosmoboot_mem_entry *map = bootinfo_mem_map(&n);
    paddr_t limit = pmm_hhdm_limit;

    g_range_count = 0;
    g_allocated_bytes = 0;
    g_sealed = false;

    for (uint32_t i = 0; i < n; i++) {
        if (map[i].type != COSMOBOOT_MEM_USABLE && map[i].type != COSMOBOOT_MEM_LOADER_RECLAIMABLE)
            continue;
        paddr_t start = map[i].base;
        paddr_t end = map[i].base + map[i].length;
        if (start < BOOTMEM_LOW_LIMIT)
            start = BOOTMEM_LOW_LIMIT;
        if (end > limit)
            end = limit;
        add_range(start, end);
    }

    uint64_t total = 0;
    for (unsigned i = 0; i < g_range_count; i++)
        total += g_ranges[i].end - g_ranges[i].start;
    kdebug("bootmem: %u ranges, %llu KiB available", g_range_count,
           (unsigned long long)(total >> 10));
}

void *bootmem_alloc(size_t size, size_t align)
{
    KASSERT(!g_sealed);
    KASSERT(size > 0);
    KASSERT(align >= 1 && (align & (align - 1)) == 0);

    /* Highest range first so the DMA zone is the last to be touched. */
    for (unsigned i = g_range_count; i-- > 0;) {
        struct bootmem_range *r = &g_ranges[i];
        if (r->end - r->start < size)
            continue;
        paddr_t top = (r->end - size) & ~((paddr_t)align - 1);
        if (top < r->start)
            continue;

        paddr_t alloc_end = top + size;
        if (alloc_end < r->end) {
            /* Leftover above the allocation becomes its own range. */
            if (g_range_count >= BOOTMEM_MAX_RANGES)
                panic("bootmem: range table full during split");
            g_ranges[g_range_count].start = alloc_end;
            g_ranges[g_range_count].end = r->end;
            g_range_count++;
        }
        r->end = top;

        g_allocated_bytes += size;
        void *va = phys_to_virt(top);
        memset(va, 0, size);
        return va;
    }

    panic("bootmem: no free range holds %zu bytes (align %zu)", size, align);
}

void bootmem_release_all(bootmem_release_fn release)
{
    KASSERT(!g_sealed);
    for (unsigned i = 0; i < g_range_count; i++) {
        struct bootmem_range *r = &g_ranges[i];
        if (r->end > r->start)
            release(phys_to_pfn(page_align_up(r->start)), phys_to_pfn(page_align_down(r->end)));
    }
}

void bootmem_seal(void)
{
    g_sealed = true;
    kdebug("bootmem: sealed, %llu KiB permanently allocated",
           (unsigned long long)(g_allocated_bytes >> 10));
}

uint64_t bootmem_allocated_bytes(void)
{
    return g_allocated_bytes;
}
