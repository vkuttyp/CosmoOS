/*
 * test_buddy.c - Host unit tests for kernel/memory/buddy.c.
 *
 * Uses a bare page array with no memory behind it: the buddy never
 * touches frames, only descriptors, and these tests prove exactly that.
 */

#include <stdlib.h>
#include <string.h>

#include <kernel/page.h>
#include <kernel/pmm.h>

#include "buddy.h"
#include "harness.h"

#define NPAGES 4096u  /* 16 MiB of imaginary RAM */

static struct pmm_zone z;

static void setup(pfn_t start, pfn_t end)
{
    pmm_max_pfn = NPAGES;
    pmm_page_array = calloc(NPAGES, sizeof(struct page));
    for (pfn_t p = 0; p < NPAGES; p++)
        pmm_page_array[p].flags = PG_RESERVED;
    buddy_zone_init(&z, "test", 0, NPAGES);
    spin_lock(&z.lock);
    buddy_free_range(&z, start, end);
}

static void teardown(void)
{
    spin_unlock(&z.lock);
    free(pmm_page_array);
    pmm_page_array = NULL;
}

static void test_free_range_maximal_blocks(void)
{
    /* [3, 4093): 3,4..7? No: 3 (o0), 4..7 (o2), 8..15 (o3), 16..31 (o4),
     * ... maximal aligned blocks. Total must equal the page count. */
    setup(3, 4093);
    EXPECT(z.nr_pages_total == 4090);
    EXPECT(z.nr_pages_free == 4090);
    EXPECT(buddy_zone_check(&z));
    EXPECT(z.free_area[0].nr_free == 2); /* pfn 3 and pfn 4092 */
    teardown();
}

static void test_alloc_free_roundtrip(void)
{
    setup(0, NPAGES);
    uint64_t before = z.nr_pages_free;

    struct page *p = buddy_alloc_block(&z, 0);
    EXPECT(p != NULL);
    EXPECT(p->refcount == 1);
    EXPECT(p->order == 0);
    EXPECT((p->flags & PG_BUDDY) == 0);
    EXPECT(z.nr_pages_free == before - 1);
    EXPECT(buddy_zone_check(&z));

    p->refcount = 0;
    buddy_free_block(&z, p, 0);
    EXPECT(z.nr_pages_free == before);
    EXPECT(buddy_zone_check(&z));
    /* Everything merged back into one maximal block per max-order chunk. */
    EXPECT(z.free_area[PMM_MAX_ORDER - 1].nr_free == NPAGES >> (PMM_MAX_ORDER - 1));
    teardown();
}

static void test_alignment_and_orders(void)
{
    setup(0, NPAGES);
    for (unsigned o = 0; o < PMM_MAX_ORDER; o++) {
        struct page *p = buddy_alloc_block(&z, o);
        EXPECT(p != NULL);
        EXPECT((page_to_pfn(p) & ((1u << o) - 1)) == 0);
        EXPECT(p->order == o);
        EXPECT(buddy_zone_check(&z));
        p->refcount = 0;
        buddy_free_block(&z, p, o);
        EXPECT(buddy_zone_check(&z));
    }
    EXPECT(z.nr_pages_free == NPAGES);
    teardown();
}

static void test_exhaustion(void)
{
    setup(0, 64);
    struct page *pages[64];
    for (unsigned i = 0; i < 64; i++) {
        pages[i] = buddy_alloc_block(&z, 0);
        EXPECT(pages[i] != NULL);
    }
    EXPECT(z.nr_pages_free == 0);
    EXPECT(buddy_alloc_block(&z, 0) == NULL);
    EXPECT(buddy_alloc_block(&z, 3) == NULL);
    for (unsigned i = 0; i < 64; i++) {
        pages[i]->refcount = 0;
        buddy_free_block(&z, pages[i], 0);
    }
    EXPECT(z.nr_pages_free == 64);
    EXPECT(z.free_area[6].nr_free == 1);
    EXPECT(buddy_zone_check(&z));
    teardown();
}

static void test_no_merge_across_zone_end(void)
{
    /* Zone [0, 48): pfn 32..47 is an order-4 block whose buddy (48..63)
     * lies outside the zone; it must never merge to order 5. */
    buddy_zone_init(&z, "small", 0, 48);
    pmm_max_pfn = NPAGES;
    pmm_page_array = calloc(NPAGES, sizeof(struct page));
    for (pfn_t p = 0; p < NPAGES; p++)
        pmm_page_array[p].flags = PG_RESERVED;
    spin_lock(&z.lock);
    buddy_free_range(&z, 0, 48);
    EXPECT(z.free_area[5].nr_free == 1);
    EXPECT(z.free_area[4].nr_free == 1);
    struct page *p = buddy_alloc_block(&z, 4);
    EXPECT(p != NULL && page_to_pfn(p) == 32);
    p->refcount = 0;
    buddy_free_block(&z, p, 4);
    EXPECT(z.free_area[4].nr_free == 1);
    EXPECT(z.free_area[5].nr_free == 1);
    EXPECT(buddy_zone_check(&z));
    teardown();
}

/* Random alloc/free against a model: no overlap, all aligned, and the
 * free count is exact after every step. */
static void test_random_stress(void)
{
    setup(0, NPAGES);
    struct { struct page *page; unsigned order; } live[512];
    unsigned nlive = 0;
    uint64_t allocated_pages = 0;
    unsigned seed = 12345;
    uint8_t *owner = calloc(NPAGES, 1);

    for (unsigned step = 0; step < 20000; step++) {
        seed = seed * 1103515245u + 12345u;
        bool do_alloc = nlive == 0 || (nlive < 512 && (seed >> 16) % 3 != 0);

        if (do_alloc) {
            unsigned order = (seed >> 8) % 6;
            struct page *p = buddy_alloc_block(&z, order);
            if (p == NULL)
                continue;
            pfn_t pfn = page_to_pfn(p);
            EXPECT((pfn & ((1u << order) - 1)) == 0);
            for (unsigned i = 0; i < (1u << order); i++) {
                EXPECT(owner[pfn + i] == 0);
                owner[pfn + i] = 1;
            }
            live[nlive].page = p;
            live[nlive].order = order;
            nlive++;
            allocated_pages += 1u << order;
        } else {
            unsigned idx = (seed >> 8) % nlive;
            struct page *p = live[idx].page;
            unsigned order = live[idx].order;
            pfn_t pfn = page_to_pfn(p);
            for (unsigned i = 0; i < (1u << order); i++)
                owner[pfn + i] = 0;
            p->refcount = 0;
            buddy_free_block(&z, p, order);
            allocated_pages -= 1u << order;
            live[idx] = live[--nlive];
        }
        EXPECT(z.nr_pages_free == NPAGES - allocated_pages);
        if (step % 500 == 0)
            EXPECT(buddy_zone_check(&z));
    }

    while (nlive > 0) {
        nlive--;
        live[nlive].page->refcount = 0;
        buddy_free_block(&z, live[nlive].page, live[nlive].order);
    }
    EXPECT(z.nr_pages_free == NPAGES);
    EXPECT(buddy_zone_check(&z));
    EXPECT(z.free_area[PMM_MAX_ORDER - 1].nr_free == NPAGES >> (PMM_MAX_ORDER - 1));
    free(owner);
    teardown();
}

static void test_misuse_panics(void)
{
    setup(0, NPAGES);
    struct page *p = buddy_alloc_block(&z, 1);
    EXPECT(p != NULL);

    /* A panic releases every held lock (harness semantics), so the
     * zone lock this test holds must be re-taken after each one. */

    /* Freeing with a live refcount is a bug. */
    EXPECT_PANIC(buddy_free_block(&z, p, 1));
    spin_lock(&z.lock);
    /* Freeing an unaligned head is a bug. */
    p->refcount = 0;
    EXPECT_PANIC(buddy_free_block(&z, p + 1, 1));
    spin_lock(&z.lock);
    /* Double free: after a correct free the head is PG_BUDDY. */
    buddy_free_block(&z, p, 1);
    EXPECT_PANIC(buddy_free_block(&z, p, 1));
    spin_lock(&z.lock);
    /* Calling without the lock is itself a bug. */
    spin_unlock(&z.lock);
    EXPECT_PANIC(buddy_alloc_block(&z, 0));
    spin_lock(&z.lock);
    teardown();
}

int main(void)
{
    static const struct host_test tests[] = {
        { "free_range_maximal_blocks", test_free_range_maximal_blocks },
        { "alloc_free_roundtrip", test_alloc_free_roundtrip },
        { "alignment_and_orders", test_alignment_and_orders },
        { "exhaustion", test_exhaustion },
        { "no_merge_across_zone_end", test_no_merge_across_zone_end },
        { "random_stress", test_random_stress },
        { "misuse_panics", test_misuse_panics },
    };
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
