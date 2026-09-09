/*
 * memtest.c - Boot-time self-tests for the memory subsystem.
 *
 * Each test records the free-page count before it starts and requires it
 * to be back at that value when it ends, so any leak or double-account in
 * the code under test fails the test rather than a later one.
 */

#include <kernel/asid.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/page.h>
#include <kernel/percpu.h>
#include <kernel/pmm.h>
#include <kernel/selftest.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/vmm.h>

#include <arch/mmu.h>
#include <arch/user.h>

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

    /* Zone constraints honoured. A platform whose RAM starts above 16 MiB
     * (QEMU virt on AArch64) has an empty DMA zone: the request must fail
     * cleanly there instead of falling upward. */
    struct pmm_stats zs;
    pmm_get_stats(&zs);
    struct page *d = pmm_alloc_page(PMM_FLAGS_ZONE_DMA);
    if (zs.zone_free[PMM_ZONE_DMA] > 0) {
        CHECK(d != NULL);
        CHECK(page_to_phys(d) < PMM_ZONE_DMA_LIMIT);
        CHECK(d->zone == PMM_ZONE_DMA);
    } else {
        CHECK(d == NULL);
    }
    struct page *d32 = pmm_alloc_page(PMM_FLAGS_ZONE_DMA32);
    CHECK(d32 != NULL);
    CHECK(page_to_phys(d32) < PMM_ZONE_DMA32_LIMIT);
    if (d)
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

    /* Arena allocations do not overlap and are page aligned. The first use
     * of an arena region may create a page-table page that is kept (M19),
     * so the region is warmed and the baseline retaken before the count is
     * compared (the IOMMU's register windows moved the arena's layout). */
    {
        vaddr_t w1 = vm_kernel_alloc(PAGE_SIZE, VM_KALLOC_GUARD, VM_PROT_RW);
        vaddr_t w2 = vm_kernel_alloc(PAGE_SIZE, VM_KALLOC_GUARD, VM_PROT_RW);
        CHECK(w1 != 0 && w2 != 0);
        vm_kernel_free(w1);
        vm_kernel_free(w2);
        baseline = free_pages();
    }
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

/* --- user regions: PROT_NONE, split, merge, strict and lenient unmap, the shootdown mask --- */

#include <kernel/percpu.h>
#include <arch/cpu.h>

static unsigned online_cpus(void)
{
    return (unsigned)__builtin_popcountll(cpu_online_mask());
}

bool selftest_user_vmm(const char **reason)
{
    struct vm_space *sp = NULL;
    CHECK(vm_space_create_user(&sp) == 0);
    CHECK(sp != NULL && sp->tlb_cpus == 0);

    const uint64_t A = 0x0000300000000000ULL;   /* far from anything a process maps */
    paddr_t pa;
    vm_prot_t prot;

    /* Four populated RW pages: one region, four frames. */
    CHECK(vm_user_map_anon(sp, A, 4 * PAGE_SIZE, VM_PROT_RW, VM_REGION_POPULATED, "t") == 0);
    CHECK(vm_user_region_count(sp) == 1);
    CHECK(sp->anon_pages == 4);
    CHECK(vm_user_range_mapped(sp, A, 4 * PAGE_SIZE, VM_PROT_RW));

    /* Unmap the middle two: two regions, two frames, a gap the strict
     * form refuses to unmap again and the lenient form skips. */
    CHECK(vm_user_unmap(sp, A + PAGE_SIZE, 2 * PAGE_SIZE, VM_UNMAP_STRICT) == 0);
    CHECK(vm_user_region_count(sp) == 2);
    CHECK(sp->anon_pages == 2);
    CHECK(!vm_user_range_mapped(sp, A, 4 * PAGE_SIZE, VM_PROT_READ));
    CHECK(vm_user_range_mapped(sp, A, PAGE_SIZE, VM_PROT_RW));
    CHECK(vm_user_range_mapped(sp, A + 3 * PAGE_SIZE, PAGE_SIZE, VM_PROT_RW));
    CHECK(!arch_mmu_query(&sp->mmu, A + PAGE_SIZE, &pa, NULL, NULL, NULL));
    CHECK(vm_user_unmap(sp, A + PAGE_SIZE, 2 * PAGE_SIZE, VM_UNMAP_STRICT) == -EINVAL);
    CHECK(vm_user_unmap(sp, A, 4 * PAGE_SIZE, VM_UNMAP_STRICT) == -EINVAL);   /* nothing changed */
    CHECK(vm_user_region_count(sp) == 2 && sp->anon_pages == 2);
    CHECK(vm_user_unmap(sp, A + PAGE_SIZE, 2 * PAGE_SIZE, 0) == 0);           /* lenient: no-op */
    CHECK(vm_user_region_count(sp) == 2 && sp->anon_pages == 2);

    /* Fill the gap with the same attributes and name: the three merge. */
    CHECK(vm_user_map_anon(sp, A + PAGE_SIZE, 2 * PAGE_SIZE, VM_PROT_RW, VM_REGION_POPULATED, "t") == 0);
    CHECK(vm_user_region_count(sp) == 1);
    CHECK(sp->anon_pages == 4);
    /* A different name does not merge; the same name adjacent does. */
    CHECK(vm_user_map_anon(sp, A + 4 * PAGE_SIZE, PAGE_SIZE, VM_PROT_RW, 0, "u") == 0);
    CHECK(vm_user_region_count(sp) == 2);
    CHECK(vm_user_unmap(sp, A + 4 * PAGE_SIZE, PAGE_SIZE, VM_UNMAP_STRICT) == 0);
    CHECK(vm_user_region_count(sp) == 1);

    /* mprotect of the middle two pages to PROT_NONE: three regions, the
     * frames stay attached and are reported with no permissions; back to
     * RW merges again. */
    struct arch_mmu_shootdown_stats sd0, sd1;
    arch_mmu_shootdown_stats(&sd0);
    CHECK(vm_user_protect(sp, A + PAGE_SIZE, 2 * PAGE_SIZE, VM_PROT_NONE) == 0);
    arch_mmu_shootdown_stats(&sd1);
    CHECK(vm_user_region_count(sp) == 3);
    CHECK(sp->anon_pages == 4);
    CHECK(arch_mmu_query(&sp->mmu, A + PAGE_SIZE, &pa, &prot, NULL, NULL));
    CHECK(pa != 0 && (prot & ~VM_PROT_USER) == VM_PROT_NONE);
    CHECK(arch_mmu_query(&sp->mmu, A, NULL, &prot, NULL, NULL));
    CHECK((prot & ~VM_PROT_USER) == VM_PROT_RW);
    CHECK(vm_user_range_mapped(sp, A + PAGE_SIZE, PAGE_SIZE, VM_PROT_NONE));
    CHECK(!vm_user_range_mapped(sp, A + PAGE_SIZE, PAGE_SIZE, VM_PROT_READ));
    /* No other CPU runs this space: the shootdown was local, no acks. */
    CHECK(sd1.acks_received == sd0.acks_received);
    CHECK(online_cpus() == 1 || sd1.initiated == sd0.initiated);
    CHECK(vm_user_map_anon(sp, A + PAGE_SIZE, PAGE_SIZE, VM_PROT_READ, 0, "t") == -EEXIST);   /* occupied */

    CHECK(vm_user_protect(sp, A + PAGE_SIZE, 2 * PAGE_SIZE, VM_PROT_RW) == 0);
    CHECK(vm_user_region_count(sp) == 1);
    CHECK(arch_mmu_query(&sp->mmu, A + 2 * PAGE_SIZE, NULL, &prot, NULL, NULL));
    CHECK((prot & ~VM_PROT_USER) == VM_PROT_RW);

    /* Protect across a gap is refused whole; W+X refused; a reservation
     * (PROT_NONE at creation) has no frames and merges with nothing. */
    CHECK(vm_user_protect(sp, A, 6 * PAGE_SIZE, VM_PROT_READ) == -ENOMEM);
    CHECK(vm_user_protect(sp, A, PAGE_SIZE, VM_PROT_RW | VM_PROT_EXEC) == -EINVAL);
    CHECK(vm_user_map_anon(sp, A + 8 * PAGE_SIZE, 2 * PAGE_SIZE, VM_PROT_NONE, 0, "t") == 0);
    CHECK(vm_user_region_count(sp) == 2 && sp->anon_pages == 4);
    CHECK(vm_user_protect(sp, A + 8 * PAGE_SIZE, PAGE_SIZE, VM_PROT_READ) == 0);   /* splits, no frames to flip */
    CHECK(vm_user_region_count(sp) == 3);

    /* Unmap a range that straddles two regions and a gap (lenient). */
    CHECK(vm_user_unmap(sp, A + 3 * PAGE_SIZE, 6 * PAGE_SIZE, 0) == 0);
    CHECK(vm_user_region_count(sp) == 2);   /* [A, A+3P) and [A+9P, A+10P) */
    CHECK(sp->anon_pages == 3);
    CHECK(vm_user_range_mapped(sp, A, 3 * PAGE_SIZE, VM_PROT_RW));
    CHECK(!vm_user_range_mapped(sp, A + 3 * PAGE_SIZE, PAGE_SIZE, VM_PROT_NONE));
    CHECK(vm_user_range_mapped(sp, A + 9 * PAGE_SIZE, PAGE_SIZE, VM_PROT_NONE));

    uint64_t before = free_pages();
    vm_space_destroy(sp);
    CHECK(free_pages() >= before + 3);
    kinfo("selftest: user-vmm: split, merge, PROT_NONE and masked shootdown on a private space");
    return true;
}

/* --- resource limits at the VMM and handle-table level (docs/kernel/security/design.md §2) --- */

#include <kernel/handle.h>
#include <kernel/process.h>

bool selftest_rlimit(const char **reason)
{
    struct vm_space *sp = NULL;
    CHECK(vm_space_create_user(&sp) == 0);
    const uint64_t A = 0x0000310000000000ULL;

    /* Address space: four pages allowed; a fifth is -ENOMEM, unmapping makes room. */
    vm_space_set_limits(sp, 4, UINT64_MAX);
    CHECK(vm_user_map_anon(sp, A, 5 * PAGE_SIZE, VM_PROT_RW, 0, "t") == -ENOMEM);
    CHECK(vm_user_map_anon(sp, A, 3 * PAGE_SIZE, VM_PROT_RW, 0, "t") == 0);
    CHECK(sp->mapped_pages == 3);
    CHECK(vm_user_map_anon(sp, A + 4 * PAGE_SIZE, 2 * PAGE_SIZE, VM_PROT_RW, 0, "t") == -ENOMEM);
    CHECK(vm_user_map_anon(sp, A + 4 * PAGE_SIZE, PAGE_SIZE, VM_PROT_RW, 0, "t") == 0);
    CHECK(sp->mapped_pages == 4);
    CHECK(vm_user_unmap(sp, A, 2 * PAGE_SIZE, VM_UNMAP_STRICT) == 0);
    CHECK(sp->mapped_pages == 2);
    CHECK(vm_user_map_anon(sp, A + 8 * PAGE_SIZE, 2 * PAGE_SIZE, VM_PROT_RW, 0, "t") == 0);
    CHECK(sp->mapped_pages == 4);
    /* Lowering below the current use changes nothing mapped; growth is refused. */
    vm_space_set_limits(sp, 1, UINT64_MAX);
    CHECK(vm_user_region_count(sp) == 3);
    CHECK(vm_user_map_anon(sp, A + 16 * PAGE_SIZE, PAGE_SIZE, VM_PROT_RW, 0, "t") == -ENOMEM);

    /* Resident memory: a populated map beyond the limit unwinds completely. */
    vm_space_set_limits(sp, UINT64_MAX, 2);
    CHECK(vm_user_map_anon(sp, A + 32 * PAGE_SIZE, 3 * PAGE_SIZE, VM_PROT_RW, VM_REGION_POPULATED, "p") == -ENOMEM);
    CHECK(sp->anon_pages == 0);   /* the two frames it did populate are back (table pages stay: M19) */
    CHECK(sp->mapped_pages == 4);
    CHECK(vm_user_region_count(sp) == 3);
    CHECK(vm_user_map_anon(sp, A + 32 * PAGE_SIZE, 2 * PAGE_SIZE, VM_PROT_RW, VM_REGION_POPULATED, "p") == 0);
    CHECK(sp->anon_pages == 2);
    vm_space_destroy(sp);

    /* Handles: the table refuses at its limit and again at the table size. */
    struct handle_table t;
    handle_table_init(&t);
    CHECK(t.limit == HANDLE_TABLE_SIZE);
    struct kobject *con = console_object();
    t.limit = 2;
    int h0 = handle_install(&t, con, HANDLE_RIGHT_READ);
    int h1 = handle_install(&t, con, HANDLE_RIGHT_READ);
    CHECK(h0 == 0 && h1 == 1);
    CHECK(handle_install(&t, con, HANDLE_RIGHT_READ) == -EMFILE);
    CHECK(handle_close(&t, h0) == 0);
    CHECK(handle_install(&t, con, HANDLE_RIGHT_READ) == 0);
    t.limit = HANDLE_TABLE_SIZE;
    for (int i = 2; i < HANDLE_TABLE_SIZE; i++)
        CHECK(handle_install(&t, con, HANDLE_RIGHT_READ) == i);
    CHECK(handle_install(&t, con, HANDLE_RIGHT_READ) == -EMFILE);
    handle_table_destroy(&t);

    /* The per-uid count sees no process for an unused uid. */
    CHECK(process_count_uid(0xFFFF1234u) == 0);
    kinfo("selftest: rlimit: address-space, resident-memory and handle limits bind where they are enforced");
    return true;
}

/* --- address-space tags (kernel/memory/asid.c) --- */

/*
 * The allocator, at a width narrow enough that rollover is reachable.
 * Interrupts stay off across the loop so that nothing else switches
 * address spaces and takes tags out of the pool being counted.
 */
/*
 * Allocate tags into `ctx[0..]` until the generation advances; return
 * how many the generation being left handed out, and clear `*distinct`
 * if it ever gave the same tag twice.
 *
 * Interrupts stay off for the loop so that no other thread switches
 * address spaces and takes a tag out of the pool being counted -- and
 * are restored before the caller checks anything, because a failed
 * CHECK returns, and returning with interrupts off hangs the kernel.
 */
static unsigned asid_fill_generation(struct arch_mmu_context *ctx, unsigned max, bool *distinct)
{
    static uint8_t seen[256];
    memset(seen, 0, sizeof(seen));
    uint64_t gen = asid_generation();
    unsigned n = 0;
    arch_irq_state_t s = arch_irq_save();
    for (; n < max; n++) {
        asid_switch_prepare(&ctx[n]);
        if (asid_generation() != gen)
            break;   /* this one already came from the next generation */
        if (distinct && (ctx[n].asid == 0 || ctx[n].asid > 255 || seen[ctx[n].asid]))
            *distinct = false;
        if (ctx[n].asid <= 255)
            seen[ctx[n].asid] = 1;
    }
    arch_irq_restore(s);
    return n;
}

/*
 * The allocator at 8 bits, where the 255-tag pool can be exhausted in a
 * test rather than in the sixty-five-thousandth process.
 *
 * `asid_test_set_bits` re-initialises: a fresh generation and an empty
 * bitmap. Every count below starts from that known state, so the numbers
 * are exact rather than arithmetic about a cursor.
 */
bool selftest_asid_alloc(const char **reason)
{
    if (arch_mmu_asid_bits() == 0) {
        kinfo("selftest: asid-alloc: no address-space tags on this machine; skipping");
        return true;
    }
    static struct arch_mmu_context ctx[300];
    struct asid_stats st0, st1;

    /* An empty pool gives out every one of its tags, each exactly once,
     * and the next request rolls the generation over. */
    CHECK(asid_test_set_bits(8));
    memset(ctx, 0, sizeof(ctx));
    uint64_t gen = asid_generation();
    asid_get_stats(&st0);
    bool distinct = true;
    unsigned n = asid_fill_generation(ctx, 300, &distinct);
    asid_get_stats(&st1);
    CHECK(distinct);
    CHECK(n == 255);                              /* 1..255; tag 0 is the kernel's */
    CHECK(asid_generation() == gen + 1);
    CHECK(st1.rollovers == st0.rollovers + 1);
    CHECK(st1.allocs == st0.allocs + 256);        /* the 256th is the one that rolled over */

    /* A tag released while its generation is current is free again --
     * checked with the pool full, so the tag that comes back can only be
     * the one just released. */
    CHECK(asid_test_set_bits(8));
    memset(ctx, 0, sizeof(ctx));
    gen = asid_generation();
    unsigned filled = 0;
    uint32_t released = 0;
    uint32_t reused = 0;
    arch_irq_state_t s = arch_irq_save();
    while (filled < 255) {
        asid_switch_prepare(&ctx[filled]);
        if (asid_generation() != gen)
            break;
        filled++;
    }
    if (filled == 255) {
        released = ctx[100].asid;
        asid_release(&ctx[100]);
        struct arch_mmu_context back;
        memset(&back, 0, sizeof(back));
        asid_switch_prepare(&back);
        reused = back.asid;
    }
    uint64_t gen_after = asid_generation();
    arch_irq_restore(s);
    CHECK(filled == 255);
    CHECK(gen_after == gen);        /* a free bit existed, so nothing rolled over */
    CHECK(reused == released);      /* and it was the one released */

    /* Releasing a tag whose generation has passed frees nothing: that
     * bit belongs to a later generation's bitmap, or to nobody. */
    CHECK(asid_test_set_bits(8));
    struct arch_mmu_context stale;
    memset(&stale, 0, sizeof(stale));
    s = arch_irq_save();
    asid_switch_prepare(&stale);
    arch_irq_restore(s);
    CHECK(asid_test_set_bits(8));   /* a new generation: `stale` is now stale */
    asid_get_stats(&st0);
    asid_release(&stale);
    asid_get_stats(&st1);
    CHECK(st1.releases == st0.releases);
    CHECK(stale.asid == 0 && stale.asid_gen == 0);

    /* A context from an older generation is re-tagged, not trusted. */
    CHECK(ctx[0].asid_gen != asid_generation());
    s = arch_irq_save();
    asid_switch_prepare(&ctx[0]);
    arch_irq_restore(s);
    CHECK(ctx[0].asid_gen == asid_generation());

    CHECK(asid_test_set_bits(arch_mmu_asid_bits()));   /* back to the machine's width */
    kinfo("selftest: asid-alloc: 255 tags per generation, exhaustion rolls over, release exact");
    return true;
}

/*
 * Isolation without a flush, which is the whole point of a tag.
 *
 * Two spaces map the same user address to different frames holding
 * different bytes. This CPU switches between them with interrupts off --
 * so nothing else runs and nothing else flushes -- and reads the address
 * through the user mapping each time. If the switch did not carry a tag,
 * the second space would read the first's byte out of a TLB entry that
 * should not apply to it.
 */
bool selftest_asid_isolation(const char **reason)
{
    if (arch_mmu_asid_bits() == 0) {
        kinfo("selftest: asid-isolation: no address-space tags on this machine; skipping");
        return true;
    }
    const uint64_t VA = 0x0000300000000000ULL;   /* far from anything a process maps */
    struct vm_space *a = NULL, *b = NULL;
    CHECK(vm_space_create_user(&a) == 0);
    CHECK(vm_space_create_user(&b) == 0);
    CHECK(vm_user_map_anon(a, VA, PAGE_SIZE, VM_PROT_RW, VM_REGION_POPULATED, "asid-a") == 0);
    CHECK(vm_user_map_anon(b, VA, PAGE_SIZE, VM_PROT_RW, VM_REGION_POPULATED, "asid-b") == 0);

    paddr_t pa;
    CHECK(arch_mmu_query(&a->mmu, VA, &pa, NULL, NULL, NULL));
    memset(phys_to_virt(pa), 0xAA, PAGE_SIZE);
    CHECK(arch_mmu_query(&b->mmu, VA, &pa, NULL, NULL, NULL));
    memset(phys_to_virt(pa), 0xBB, PAGE_SIZE);

    struct vm_space *restore = this_cpu()->cur_space ? this_cpu()->cur_space : &kernel_space;
    struct vm_space *cur = restore;
    unsigned wrong = 0, faults = 0;

    arch_irq_state_t s = arch_irq_save();
    for (unsigned i = 0; i < 20; i++) {
        uint8_t v = 0;
        vm_space_switch(cur, a);
        cur = a;
        if (arch_copy_user_raw(&v, (const void *)(uintptr_t)VA, 1) != 0)
            faults++;
        else if (v != 0xAA)
            wrong++;
        vm_space_switch(cur, b);
        cur = b;
        if (arch_copy_user_raw(&v, (const void *)(uintptr_t)VA, 1) != 0)
            faults++;
        else if (v != 0xBB)
            wrong++;
    }
    vm_space_switch(cur, restore);
    arch_irq_restore(s);

    vm_space_destroy(a);
    vm_space_destroy(b);
    if (faults) {
        *reason = "a user read faulted with the space active";
        return false;
    }
    if (wrong) {
        *reason = "a space read another space's byte: the switch did not carry its tag";
        return false;
    }
    kinfo("selftest: asid-isolation: 40 tagged switches, each space read its own page");
    return true;
}

/*
 * The rollover, with live spaces rather than throwaway contexts.
 *
 * This is the case a normal boot never reaches: sixteen-bit tags give
 * 65,535 of them, so nothing exhausts the pool, and the flush that makes
 * a recycled tag safe is never exercised. Forced here, and arranged so
 * that the recycled tag is handed straight back to a *different* space
 * on the same CPU -- which is the only shape in which the missing flush
 * is visible:
 *
 *   A runs under tag 1 and this CPU caches its page under (1, VA).
 *   The generation rolls over; the bitmap empties and the cursor resets.
 *   B switches in, is given tag 1 of the new generation, and reads VA.
 *
 * B must read B's byte. It can only do so because the rollover made this
 * CPU flush before it trusted any tag of the new generation.
 */
bool selftest_asid_rollover(const char **reason)
{
    if (arch_mmu_asid_bits() == 0) {
        kinfo("selftest: asid-rollover: no address-space tags on this machine; skipping");
        return true;
    }
    const uint64_t VA = 0x0000300000000000ULL;
    struct vm_space *a = NULL, *b = NULL;
    CHECK(vm_space_create_user(&a) == 0);
    CHECK(vm_space_create_user(&b) == 0);
    CHECK(vm_user_map_anon(a, VA, PAGE_SIZE, VM_PROT_RW, VM_REGION_POPULATED, "roll-a") == 0);
    CHECK(vm_user_map_anon(b, VA, PAGE_SIZE, VM_PROT_RW, VM_REGION_POPULATED, "roll-b") == 0);
    paddr_t pa;
    CHECK(arch_mmu_query(&a->mmu, VA, &pa, NULL, NULL, NULL));
    memset(phys_to_virt(pa), 0xAA, PAGE_SIZE);
    CHECK(arch_mmu_query(&b->mmu, VA, &pa, NULL, NULL, NULL));
    memset(phys_to_virt(pa), 0xBB, PAGE_SIZE);

    /* A fresh generation with an empty bitmap and the cursor at 1, so
     * that the tag A is about to get is the tag B will be given after
     * the rollover. */
    CHECK(asid_test_set_bits(8));

    struct vm_space *restore = this_cpu()->cur_space ? this_cpu()->cur_space : &kernel_space;
    uint32_t tag_a = 0, tag_b = 0;
    uint8_t va = 0, vb = 0;
    size_t fa = 1, fb = 1;

    arch_irq_state_t s = arch_irq_save();
    vm_space_switch(restore, a);
    tag_a = a->mmu.asid;
    fa = arch_copy_user_raw(&va, (const void *)(uintptr_t)VA, 1);
    /* Not by allocating: that would stamp this CPU as flushed and eat
     * the flush under test. */
    bool rolled = asid_test_force_rollover();
    vm_space_switch(a, b);
    tag_b = b->mmu.asid;
    fb = arch_copy_user_raw(&vb, (const void *)(uintptr_t)VA, 1);
    vm_space_switch(b, restore);
    arch_irq_restore(s);

    vm_space_destroy(a);
    vm_space_destroy(b);
    CHECK(asid_test_set_bits(arch_mmu_asid_bits()));

    CHECK(rolled);
    CHECK(fa == 0 && fb == 0);
    CHECK(tag_a == tag_b);   /* the point: B really was handed A's tag */
    if (va != 0xAA || vb != 0xBB) {
        kwarn("selftest: asid-rollover: tag %u then %u, read 0x%02x then 0x%02x (wanted 0xaa then 0xbb)",
              tag_a, tag_b, va, vb);
        *reason = "a recycled tag carried the old space's translations across a rollover";
        return false;
    }
    kinfo("selftest: asid-rollover: tag %u reissued across a generation, each space read its own page",
          tag_a);
    return true;
}

/*
 * The destroy path, and the review finding the report was corrected for:
 * a tag released before its translations are invalidated is a tag whose
 * next owner inherits them.
 *
 * Arranged so that the reuse is certain rather than likely. The pool is
 * filled to the brim while the old space holds its tag, so destroying it
 * leaves exactly one free tag in the whole machine -- the one it just
 * gave back -- and the next space to switch in must be given that one.
 * (Self-tests run on an otherwise idle machine; if another thread did
 * take that tag first, the check on the tag numbers below says so
 * instead of passing quietly.)
 */
bool selftest_asid_destroy_reuse(const char **reason)
{
    if (arch_mmu_asid_bits() == 0) {
        kinfo("selftest: asid-destroy-reuse: no address-space tags on this machine; skipping");
        return true;
    }
    const uint64_t VA = 0x0000300000000000ULL;
    struct vm_space *old_sp = NULL, *new_sp = NULL;
    paddr_t pa;

    /* Both frames exist before either space is destroyed, so the second
     * space cannot be handed the first's freed page and read its own
     * byte out of it by accident. */
    CHECK(vm_space_create_user(&old_sp) == 0);
    CHECK(vm_user_map_anon(old_sp, VA, PAGE_SIZE, VM_PROT_RW, VM_REGION_POPULATED, "d-old") == 0);
    CHECK(arch_mmu_query(&old_sp->mmu, VA, &pa, NULL, NULL, NULL));
    memset(phys_to_virt(pa), 0xAA, PAGE_SIZE);
    CHECK(vm_space_create_user(&new_sp) == 0);
    CHECK(vm_user_map_anon(new_sp, VA, PAGE_SIZE, VM_PROT_RW, VM_REGION_POPULATED, "d-new") == 0);
    CHECK(arch_mmu_query(&new_sp->mmu, VA, &pa, NULL, NULL, NULL));
    memset(phys_to_virt(pa), 0xBB, PAGE_SIZE);

    CHECK(asid_test_set_bits(8));
    struct vm_space *restore = this_cpu()->cur_space ? this_cpu()->cur_space : &kernel_space;
    uint32_t tag_old = 0, tag_new = 0;
    uint8_t v1 = 0, v2 = 0;
    size_t f1 = 1, f2 = 1;

    /* The old space runs and this CPU caches its page under its tag. */
    arch_irq_state_t s = arch_irq_save();
    vm_space_switch(restore, old_sp);
    tag_old = old_sp->mmu.asid;
    f1 = arch_copy_user_raw(&v1, (const void *)(uintptr_t)VA, 1);
    vm_space_switch(old_sp, restore);
    arch_irq_restore(s);

    /* Fill every other tag, so that the destroy below leaves exactly one. */
    static struct arch_mmu_context fill[255];
    memset(fill, 0, sizeof(fill));
    uint64_t gen = asid_generation();
    unsigned n = 0;
    s = arch_irq_save();
    while (n < 254) {
        asid_switch_prepare(&fill[n]);
        if (asid_generation() != gen)
            break;
        n++;
    }
    arch_irq_restore(s);

    vm_space_destroy(old_sp);   /* invalidate the tag, then release it */

    s = arch_irq_save();
    vm_space_switch(restore, new_sp);
    tag_new = new_sp->mmu.asid;
    f2 = arch_copy_user_raw(&v2, (const void *)(uintptr_t)VA, 1);
    vm_space_switch(new_sp, restore);
    uint64_t gen_end = asid_generation();
    arch_irq_restore(s);

    vm_space_destroy(new_sp);
    CHECK(asid_test_set_bits(arch_mmu_asid_bits()));

    CHECK(n == 254);
    CHECK(f1 == 0 && v1 == 0xAA);
    CHECK(f2 == 0);
    CHECK(gen_end == gen);            /* a tag was free: nothing rolled over */
    CHECK(tag_new == tag_old);        /* and it was the destroyed space's */
    if (v2 != 0xBB) {
        *reason = "a tag released at destroy carried the old space's translations to its next owner";
        return false;
    }
    kinfo("selftest: asid-destroy-reuse: tag %u reissued after destroy, the new space read its own page",
          tag_old);
    return true;
}

/*
 * Two CPUs starting the threads of one process reach an untagged space
 * at the same moment. Both may find it untagged; only one may allocate.
 *
 * The bug this pins down was a check made before the lock and not
 * remade under it: both CPUs allocated, the second write won, and the
 * first tag stayed reserved with nothing pointing at it until the next
 * rollover -- a pool emptying faster than it should and full flushes
 * that need not have happened. It is not an isolation failure (both
 * tags name the same tables), which is exactly why a counting test is
 * the one that catches it.
 *
 * Each round hands both CPUs the same fresh context and requires the
 * allocator to have handed out exactly one tag for it.
 */
#define ASID_RACE_ROUNDS 300u

struct asid_race {
    struct arch_mmu_context *ctx;
    volatile uint32_t *round;    /* the round both sides are waiting for */
    volatile uint32_t *arrived;  /* how many have reached it */
    unsigned rounds;
};

static void asid_race_thread(void *arg)
{
    struct asid_race *r = arg;
    for (unsigned i = 1; i <= r->rounds; i++) {
        __atomic_fetch_add(r->arrived, 1u, __ATOMIC_ACQ_REL);
        while (__atomic_load_n(r->round, __ATOMIC_ACQUIRE) != i)
            arch_cpu_relax();
        asid_switch_prepare(r->ctx);
    }
    thread_exit(0);
}

bool selftest_asid_race(const char **reason)
{
    if (arch_mmu_asid_bits() == 0) {
        kinfo("selftest: asid-race: no address-space tags on this machine; skipping");
        return true;
    }
    if (__builtin_popcountll(cpu_online_mask()) < 2) {
        kinfo("selftest: asid-race: one CPU online; the race needs two");
        return true;
    }
    /* Anywhere but here, so the two callers are genuinely concurrent. */
    cpumask_t others = cpu_online_mask() & ~CPUMASK_OF(arch_cpu_id());
    static struct arch_mmu_context ctx;
    static volatile uint32_t round, arrived;
    round = 0;
    arrived = 0;
    struct asid_race r = { .ctx = &ctx, .round = &round, .arrived = &arrived, .rounds = ASID_RACE_ROUNDS };
    struct thread *t = thread_create_on(asid_race_thread, &r, "asid-race", SCHED_PRIO_DEFAULT, others);
    CHECK(t != NULL);

    struct asid_stats st0, st1;
    asid_get_stats(&st0);
    uint64_t gen0 = asid_generation();
    unsigned double_allocs = 0;

    for (unsigned i = 1; i <= ASID_RACE_ROUNDS; i++) {
        memset(&ctx, 0, sizeof(ctx));            /* untagged again */
        struct asid_stats a, b;
        asid_get_stats(&a);
        __atomic_fetch_add(&arrived, 1u, __ATOMIC_ACQ_REL);
        while (__atomic_load_n(&arrived, __ATOMIC_ACQUIRE) < 2u * i)
            arch_cpu_relax();
        __atomic_store_n(&round, i, __ATOMIC_RELEASE);   /* both go */
        asid_switch_prepare(&ctx);
        while (__atomic_load_n(&arrived, __ATOMIC_ACQUIRE) < 2u * i)
            arch_cpu_relax();
        asid_get_stats(&b);
        if (b.allocs - a.allocs > 1)
            double_allocs++;
    }
    thread_join(t);
    asid_get_stats(&st1);

    if (double_allocs != 0) {
        kwarn("selftest: asid-race: %u of %u rounds allocated two tags for one space", double_allocs,
              ASID_RACE_ROUNDS);
        *reason = "two CPUs both tagged the same space: the check before the lock was not remade under it";
        return false;
    }
    kinfo("selftest: asid-race: %u contested rounds, %llu tags for %u spaces, %llu rollovers",
          ASID_RACE_ROUNDS, (unsigned long long)(st1.allocs - st0.allocs), ASID_RACE_ROUNDS,
          (unsigned long long)(asid_generation() - gen0));
    return true;
}

/*
 * What the unit actually changed, counted rather than timed: the number
 * of full TLB flushes the switch path performs.
 *
 * It used to be one per user switch, and on AArch64 that flush was
 * inner-shareable -- every CPU in the machine emptied its user TLB
 * because one CPU changed process. It should now be zero between
 * rollovers, whatever the switch rate.
 *
 * Counting is the right instrument here and timing is not. Under TCG a
 * tagged switch measures *slower* than a flushing one (see
 * docs/kernel/memory/testing.md): QEMU's software TLB is not
 * ASID-tagged, so a root write that changes the ASID sends it down a
 * path an explicit TLBI short-circuits, and the figure swings threefold
 * between runs of one binary. The count does not depend on emulation
 * at all.
 */
/*
 * Deliberately small. The loop runs with interrupts off so that no other
 * switch on this CPU perturbs the counts, and each switch costs ~140 us
 * under TCG (QEMU's software TLB is not ASID-tagged, so changing the
 * ASID takes a slow path). At 200 rounds that held interrupts off for
 * nearly 60 ms -- long enough to stall timers on every CPU and then
 * release a burst of deferred allocation into whatever test ran next,
 * which is how this was found: `kmalloc`'s live-object equality failed
 * intermittently, and the burst was landing on either side of its own
 * snapshot. Fifty rounds prove the same property in a window the rest
 * of the machine need not notice.
 */
#define ASID_QUIET_ROUNDS 50u

bool selftest_asid_quiet(const char **reason)
{
    if (arch_mmu_asid_bits() == 0) {
        kinfo("selftest: asid-quiet: no address-space tags on this machine; skipping");
        return true;
    }
    const uint64_t VA = 0x0000300000000000ULL;
    struct vm_space *a = NULL, *b = NULL;
    CHECK(vm_space_create_user(&a) == 0);
    CHECK(vm_space_create_user(&b) == 0);
    CHECK(vm_user_map_anon(a, VA, PAGE_SIZE, VM_PROT_RW, VM_REGION_POPULATED, "quiet") == 0);
    CHECK(vm_user_map_anon(b, VA, PAGE_SIZE, VM_PROT_RW, VM_REGION_POPULATED, "quiet") == 0);

    struct vm_space *restore = this_cpu()->cur_space ? this_cpu()->cur_space : &kernel_space;
    struct asid_stats st0, st1;

    /* One warm pass first: the very first switch into each space
     * allocates its tag, and this CPU's own first switch of a generation
     * legitimately flushes. What is counted is the steady state. */
    struct vm_space *cur = restore;
    for (unsigned i = 0; i < 4; i++) {
        struct vm_space *sp = (i & 1) ? b : a;
        vm_space_switch(cur, sp);
        cur = sp;
    }
    vm_space_switch(cur, restore);

    asid_get_stats(&st0);
    uint64_t hw0 = arch_mmu_activate_flushes();
    cur = restore;
    /*
     * Preemption off, interrupts on. Off, because the count that matters
     * is this CPU's own (`arch_mmu_activate_flushes`) and a thread that
     * migrated mid-measurement would compare two different CPUs'
     * counters. On, because disabling them was caution rather than
     * necessity: a loop long enough to be worth measuring stalled timers
     * on every CPU and released a burst of deferred allocation into the
     * next test's accounting.
     */
    preempt_disable();
    for (unsigned i = 0; i < ASID_QUIET_ROUNDS; i++) {
        struct vm_space *sp = (i & 1) ? b : a;
        vm_space_switch(cur, sp);
        cur = sp;
    }
    /*
     * And a few through the kernel's root, which must cost no tag: the
     * kernel runs under tag 0, and asking the allocator on its behalf
     * would consume one per generation that nothing releases (M39).
     * Only a few, because a switch to the kernel root re-walks the
     * early-device mappings and changes TTBR0 twice, which under TCG is
     * expensive enough that doing it hundreds of times disturbs the
     * whole machine -- it made the next test's allocation accounting
     * fail intermittently.
     */
    for (unsigned i = 0; i < 4; i++) {
        struct vm_space *sp = (i & 1) ? b : a;
        vm_space_switch(cur, &kernel_space);
        vm_space_switch(&kernel_space, sp);
        cur = sp;
    }
    vm_space_switch(cur, restore);
    uint64_t hw1 = arch_mmu_activate_flushes();
    preempt_enable();
    asid_get_stats(&st1);

    vm_space_destroy(a);
    vm_space_destroy(b);

    /*
     * The instruction count, not the decision count: a switch path that
     * flushes without asking the allocator is exactly the regression
     * this is here to catch, and it would leave the decision count at
     * zero.
     *
     * Paranoid mode inverts the expectation rather than excusing the
     * test -- there, every switch must flush, and a run that flushed
     * fewer times than it switched would mean the mode was not in force
     * for the whole loop.
     */
    uint64_t want = asid_paranoid() ? ASID_QUIET_ROUNDS + 9 : 0;
    if (hw1 - hw0 != want) {
        *reason = asid_paranoid() ? "paranoid mode did not flush on every switch"
                                  : "the switch path still flushes the TLB";
        return false;
    }
    /* The kernel's root runs under tag 0 and must never be given one:
     * a tag allocated for it is one per generation that nothing frees. */
    if (kernel_space.mmu.asid != 0) {
        *reason = "the kernel's root was given an address-space tag";
        return false;
    }
    /*
     * `asid_get_stats` counts the whole machine, and another CPU may
     * legitimately move its numbers while this runs: its first switch
     * after a generation change flushes, and a space it enters for the
     * first time is given a tag. Neither speaks to the property under
     * test, so they are reported rather than asserted -- CI found that
     * the hard way, on a machine whose other CPUs switched inside this
     * window. What is asserted is this CPU's own instruction count and
     * the kernel root's tag, both above.
     */
    kinfo("selftest: asid-quiet: %u switches (eight through the kernel's root), "
          "%llu TLB flushes performed here, %llu tags allocated machine-wide",
          ASID_QUIET_ROUNDS + 9, (unsigned long long)(hw1 - hw0),
          (unsigned long long)(st1.allocs - st0.allocs));
    return true;
}

/*
 * Paranoid mode must not change what anything sees -- it is a
 * performance switch, not a semantic one. Running the isolation property
 * with every switch flushing proves the test is about the rule and not
 * about a lucky TLB.
 */
bool selftest_asid_paranoid(const char **reason)
{
    if (arch_mmu_asid_bits() == 0) {
        kinfo("selftest: asid-paranoid: no address-space tags on this machine; skipping");
        return true;
    }
    bool was = asid_paranoid();
    asid_set_paranoid(true);
    bool ok = selftest_asid_isolation(reason);
    asid_set_paranoid(was);   /* restore, not force off: a whole boot may be paranoid */
    if (!ok)
        return false;
    kinfo("selftest: asid-paranoid: isolation holds with every switch flushing too");
    return true;
}
