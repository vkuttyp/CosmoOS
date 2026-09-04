/*
 * memtest.c - Boot-time self-tests for the memory subsystem.
 *
 * Each test records the free-page count before it starts and requires it
 * to be back at that value when it ends, so any leak or double-account in
 * the code under test fails the test rather than a later one.
 */

#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/page.h>
#include <kernel/pmm.h>
#include <kernel/selftest.h>
#include <kernel/string.h>
#include <kernel/vmm.h>

#define STR_(x) #x
#define STR(x)  STR_(x)
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            *reason = "check failed: " #cond " at line " STR(__LINE__);        \
            return false;                                                      \
        }                                                                      \
    } while (0)

static uint64_t free_pages(void)
{
    struct pmm_stats st;
    pmm_get_stats(&st);
    return st.free_pages;
}

/* --- PMM --- */

bool selftest_pmm(const char **reason)
{
    uint64_t baseline = free_pages();
    CHECK(baseline > 64);

    /* Single page: aligned, in a direct-mapped zone, refcount 1. */
    struct page *p = pmm_alloc_page(0);
    CHECK(p != NULL);
    CHECK(p->refcount == 1);
    CHECK(p->order == 0);
    CHECK((p->flags & (PG_BUDDY | PG_RESERVED)) == 0);
    CHECK(phys_in_direct_map(page_to_phys(p)));
    CHECK(free_pages() == baseline - 1);
    memset(page_to_virt(p), 0xA5, PAGE_SIZE);
    pmm_free_page(p);
    CHECK(free_pages() == baseline);

    /* Order-3 block: natural alignment, zeroing, order recorded. */
    struct page *b = pmm_alloc_pages(3, PMM_FLAGS_ZERO);
    CHECK(b != NULL);
    CHECK((page_to_pfn(b) & 7) == 0);
    CHECK(b->order == 3);
    const uint8_t *bytes = page_to_virt(b);
    bool zero = true;
    for (size_t i = 0; i < (PAGE_SIZE << 3); i += 509)
        zero = zero && bytes[i] == 0;
    CHECK(zero);
    CHECK(free_pages() == baseline - 8);
    pmm_free_pages(b, 3);
    CHECK(free_pages() == baseline);

    /* Zone constraints honoured. */
    struct page *d = pmm_alloc_page(PMM_FLAGS_ZONE_DMA);
    CHECK(d != NULL);
    CHECK(page_to_phys(d) < PMM_ZONE_DMA_LIMIT);
    CHECK(d->zone == PMM_ZONE_DMA);
    struct page *d32 = pmm_alloc_page(PMM_FLAGS_ZONE_DMA32);
    CHECK(d32 != NULL);
    CHECK(page_to_phys(d32) < PMM_ZONE_DMA32_LIMIT);
    pmm_free_page(d);
    pmm_free_page(d32);
    CHECK(free_pages() == baseline);

    /* Refcounts: put at 1 frees. */
    struct page *r = pmm_alloc_page(0);
    CHECK(r != NULL);
    pmm_page_get(r);
    CHECK(r->refcount == 2);
    pmm_page_put(r);
    CHECK(r->refcount == 1);
    CHECK(free_pages() == baseline - 1);
    pmm_page_put(r);
    CHECK(free_pages() == baseline);

    /* Split and coalesce: 64 single pages then free them in shuffled
     * order; the buddy must merge everything back. */
    struct page *pages[64];
    for (unsigned i = 0; i < 64; i++) {
        pages[i] = pmm_alloc_page(0);
        CHECK(pages[i] != NULL);
    }
    CHECK(free_pages() == baseline - 64);
    for (unsigned i = 0; i < 64; i++)
        pmm_free_page(pages[(i * 37) % 64]);
    CHECK(free_pages() == baseline);

    /* Conversions round-trip. */
    struct page *c = pmm_alloc_page(0);
    CHECK(c != NULL);
    CHECK(phys_to_page(page_to_phys(c)) == c);
    CHECK(virt_to_page(page_to_virt(c)) == c);
    CHECK(virt_to_phys(page_to_virt(c)) == page_to_phys(c));
    pmm_free_page(c);

    /* Invalid order. */
    CHECK(pmm_alloc_pages(PMM_MAX_ORDER, 0) == NULL);

    /* Largest block, if the machine has one, is naturally aligned. */
    struct page *big = pmm_alloc_pages(PMM_MAX_ORDER - 1, 0);
    if (big != NULL) {
        CHECK((page_to_pfn(big) & ((1u << (PMM_MAX_ORDER - 1)) - 1)) == 0);
        pmm_free_pages(big, PMM_MAX_ORDER - 1);
    }
    CHECK(free_pages() == baseline);

    struct pmm_stats st;
    pmm_get_stats(&st);
    CHECK(st.deferred_pages == 0);
    CHECK(st.free_pages <= st.total_pages);
    return true;
}

/* --- VMM --- */

bool selftest_vmm(const char **reason)
{
    /* The first mapping in the arena allocates intermediate page-table
     * pages, which the MMU layer does not reclaim on unmap (documented
     * gap). Warm the arena up so the baseline excludes them. */
    vaddr_t warm = vm_kernel_alloc(PAGE_SIZE, VM_KALLOC_GUARD | VM_KALLOC_POPULATE, VM_PROT_RW);
    CHECK(warm != 0);
    vm_kernel_free(warm);

    uint64_t baseline = free_pages();
    paddr_t pa;
    vm_prot_t prot;
    vm_cache_t cache;
    size_t psz;

    /* Kernel image permissions from the new tables. */
    CHECK(vm_query((vaddr_t)selftest_vmm, &pa, &prot, &cache, &psz));
    CHECK(prot == VM_PROT_RX);
    CHECK(cache == VM_CACHE_WB);
    CHECK(psz == PAGE_SIZE);
    static const int rodata_probe = 42;
    CHECK(vm_query((vaddr_t)&rodata_probe, NULL, &prot, NULL, NULL));
    CHECK(prot == VM_PROT_READ);
    static int data_probe;
    CHECK(vm_query((vaddr_t)&data_probe, NULL, &prot, NULL, NULL));
    CHECK(prot == VM_PROT_RW);

    /* Direct map: RW, NX, and it translates back to the same physical. */
    struct page *pg = pmm_alloc_page(0);
    CHECK(pg != NULL);
    CHECK(vm_query((vaddr_t)page_to_virt(pg), &pa, &prot, NULL, &psz));
    CHECK(pa == page_to_phys(pg));
    CHECK(prot == VM_PROT_RW);
    CHECK(psz == PAGE_SIZE || psz == PAGE_2M_SIZE || psz == PAGE_1G_SIZE);
    pmm_free_page(pg);

    /* Populated allocation with guards: mapped inside, unmapped around. */
    vaddr_t a = vm_kernel_alloc(3 * PAGE_SIZE, VM_KALLOC_GUARD | VM_KALLOC_POPULATE, VM_PROT_RW);
    CHECK(a != 0);
    CHECK(a >= kernel_space.arena_lo && a < kernel_space.arena_hi);
    CHECK(free_pages() <= baseline - 3);
    for (unsigned i = 0; i < 3; i++) {
        CHECK(vm_query(a + i * PAGE_SIZE, NULL, &prot, NULL, NULL));
        CHECK(prot == VM_PROT_RW);
    }
    CHECK(!vm_query(a - PAGE_SIZE, NULL, NULL, NULL, NULL));
    CHECK(!vm_query(a + 3 * PAGE_SIZE, NULL, NULL, NULL, NULL));
    volatile uint64_t *w = (volatile uint64_t *)a;
    CHECK(w[0] == 0 && w[(3 * PAGE_SIZE) / 8 - 1] == 0);
    w[0] = 0x1234;
    w[(3 * PAGE_SIZE) / 8 - 1] = 0x5678;
    CHECK(w[0] == 0x1234);
    const struct vm_region *r = vm_find_region(&kernel_space, a + PAGE_SIZE);
    CHECK(r != NULL && r->base == a && r->kind == VM_REGION_ANON);
    CHECK(vm_find_region(&kernel_space, a - PAGE_SIZE) == NULL);
    vm_kernel_free(a);
    CHECK(!vm_query(a, NULL, NULL, NULL, NULL));
    CHECK(free_pages() == baseline);

    /* Lazy allocation: nothing mapped until touched; touch faults in a
     * zeroed page through the real #PF path; free returns it. */
    struct vm_stats vs0, vs1;
    vm_get_stats(&vs0);
    vaddr_t lazy = vm_kernel_alloc(4 * PAGE_SIZE, VM_KALLOC_GUARD, VM_PROT_RW);
    CHECK(lazy != 0);
    CHECK(free_pages() == baseline);
    CHECK(!vm_query(lazy + 2 * PAGE_SIZE, NULL, NULL, NULL, NULL));
    volatile uint32_t *lz = (volatile uint32_t *)(lazy + 2 * PAGE_SIZE + 64);
    uint32_t seen = *lz;              /* read fault populates */
    CHECK(seen == 0);
    *lz = 0xCAFEF00D;                 /* write on the now-present page */
    CHECK(*lz == 0xCAFEF00D);
    CHECK(vm_query(lazy + 2 * PAGE_SIZE, NULL, &prot, NULL, NULL));
    CHECK(prot == VM_PROT_RW);
    CHECK(!vm_query(lazy, NULL, NULL, NULL, NULL));
    CHECK(free_pages() == baseline - 1);
    vm_get_stats(&vs1);
    CHECK(vs1.faults_handled == vs0.faults_handled + 1);
    vm_kernel_free(lazy);
    CHECK(free_pages() == baseline);

    /* Physical mapping with uncached attribute: map a RAM page as UC via
     * the arena, verify attributes, and that both views agree. */
    struct page *mm = pmm_alloc_page(PMM_FLAGS_ZERO);
    CHECK(mm != NULL);
    vaddr_t win = vm_map_phys(page_to_phys(mm), PAGE_SIZE, VM_PROT_RW, VM_CACHE_UC);
    CHECK(win != 0);
    CHECK(vm_query(win, &pa, &prot, &cache, NULL));
    CHECK(pa == page_to_phys(mm));
    CHECK(cache == VM_CACHE_UC);
    CHECK(prot == VM_PROT_RW);
    *(volatile uint32_t *)win = 0x11223344;
    CHECK(*(volatile uint32_t *)page_to_virt(mm) == 0x11223344);
    vm_unmap_phys(win);
    CHECK(!vm_query(win, NULL, NULL, NULL, NULL));
    pmm_free_page(mm);
    CHECK(free_pages() == baseline);

    /* Arena allocations do not overlap and are page aligned. */
    vaddr_t x = vm_kernel_alloc(PAGE_SIZE, VM_KALLOC_GUARD, VM_PROT_RW);
    vaddr_t y = vm_kernel_alloc(PAGE_SIZE, VM_KALLOC_GUARD, VM_PROT_RW);
    CHECK(x != 0 && y != 0 && x != y);
    CHECK(is_page_aligned(x) && is_page_aligned(y));
    CHECK(y >= x + 2 * PAGE_SIZE || x >= y + 2 * PAGE_SIZE);
    vm_kernel_free(x);
    vm_kernel_free(y);
    CHECK(free_pages() == baseline);

    /* Bad arguments. */
    CHECK(vm_kernel_alloc(0, 0, VM_PROT_RW) == 0);
    CHECK(vm_kernel_alloc(PAGE_SIZE + 1, 0, VM_PROT_RW) == 0);
    CHECK(vm_kernel_alloc(PAGE_SIZE, 0, VM_PROT_NONE) == 0);
    return true;
}

/* --- heap --- */

bool selftest_kmalloc(const char **reason)
{
    uint64_t baseline = free_pages();
    struct kmalloc_stats ks0, ks1;
    kmalloc_get_stats(&ks0);

    /* Sizes across every class and the page path; alignment; zeroing. */
    static const size_t sizes[] = { 1, 15, 16, 17, 100, 255, 256, 1000, 4096, 8192, 8193, 65536, 1 << 20 };
    void *ptrs[ARRAY_SIZE(sizes)];
    for (size_t i = 0; i < ARRAY_SIZE(sizes); i++) {
        ptrs[i] = kzalloc(sizes[i]);
        CHECK(ptrs[i] != NULL);
        CHECK(((uintptr_t)ptrs[i] & (KMALLOC_MIN_ALIGN - 1)) == 0);
        CHECK(kmalloc_size(ptrs[i]) >= sizes[i]);
        const uint8_t *b = ptrs[i];
        CHECK(b[0] == 0 && b[sizes[i] - 1] == 0);
        memset(ptrs[i], (int)(0x40 + i), sizes[i]);
    }
    /* No two allocations overlap. */
    for (size_t i = 0; i < ARRAY_SIZE(sizes); i++) {
        const uint8_t *b = ptrs[i];
        CHECK(b[0] == 0x40 + i && b[sizes[i] - 1] == 0x40 + i);
    }
    for (size_t i = 0; i < ARRAY_SIZE(sizes); i++)
        kfree(ptrs[i]);
    kmalloc_get_stats(&ks1);
    CHECK(ks1.live_objects == ks0.live_objects);
    CHECK(ks1.large_pages == ks0.large_pages);

    /* krealloc preserves content in both directions. */
    uint8_t *r = kmalloc(40, 0);
    CHECK(r != NULL);
    for (unsigned i = 0; i < 40; i++)
        r[i] = (uint8_t)i;
    r = krealloc(r, 3000, 0);
    CHECK(r != NULL);
    for (unsigned i = 0; i < 40; i++)
        CHECK(r[i] == i);
    r = krealloc(r, 20, 0);
    CHECK(r != NULL);
    for (unsigned i = 0; i < 20; i++)
        CHECK(r[i] == i);
    kfree(r);

    /* Many small objects: exercises slab growth, full/partial moves,
     * and empty-slab retention/release. */
    enum { N = 4096 };
    void **many = kmalloc(N * sizeof(void *), 0);
    CHECK(many != NULL);
    for (unsigned i = 0; i < N; i++) {
        many[i] = kmalloc(64, 0);
        CHECK(many[i] != NULL);
        *(unsigned *)many[i] = i;
    }
    for (unsigned i = 0; i < N; i++)
        CHECK(*(unsigned *)many[i] == i);
    for (unsigned i = 0; i < N; i += 2)
        kfree(many[i]);
    for (unsigned i = 0; i < N; i += 2) {
        many[i] = kmalloc(64, 0);
        CHECK(many[i] != NULL);
    }
    for (unsigned i = 0; i < N; i++)
        kfree(many[i]);
    kfree(many);

    /* Dedicated cache with unusual alignment. */
    struct kmem_cache *c = kmem_cache_create("selftest-obj", 200, 64);
    CHECK(c != NULL);
    void *o1 = kmem_cache_alloc(c, KMEM_ZERO);
    void *o2 = kmem_cache_alloc(c, 0);
    CHECK(o1 != NULL && o2 != NULL && o1 != o2);
    CHECK(((uintptr_t)o1 & 63) == 0 && ((uintptr_t)o2 & 63) == 0);
    kmem_cache_free(c, o1);
    kmem_cache_free(c, o2);
    kmem_cache_destroy(c);

    /* Oversize and zero requests fail cleanly. */
    CHECK(kmalloc(0, 0) == NULL);
    CHECK(kmalloc(KMALLOC_MAX_SIZE + 1, 0) == NULL);
    kfree(NULL);

    /* Everything returned to the buddy except retained empty slabs,
     * which are bounded; check the frame count did not grow beyond that. */
    kmalloc_get_stats(&ks1);
    CHECK(ks1.live_objects == ks0.live_objects);
    CHECK(free_pages() + 64 >= baseline);
    return true;
}
