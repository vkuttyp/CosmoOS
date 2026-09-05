/*
 * vmm.c - Kernel address space, regions, the VA arena, and page faults.
 *
 * The region list is sorted by base and scanned linearly; with a handful
 * of kernel regions that is the right structure. It becomes a tree when a
 * process has thousands of mappings, behind the same functions.
 */

#include <kernel/bootinfo.h>
#include <kernel/errno.h>
#include <kernel/interrupt.h>
#include <kernel/kernel.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/page.h>
#include <kernel/panic.h>
#include <kernel/pmm.h>
#include <kernel/printf.h>
#include <kernel/string.h>
#include <kernel/vmm.h>

#include <arch/trap.h>

#define KERNEL_ARENA_LO 0xFFFFC00000000000ULL
#define KERNEL_ARENA_HI 0xFFFFE00000000000ULL

/* The near arena holds modules where the architecture's code model and
 * direct branches reach the kernel image: its bounds come from
 * arch_mmu_near_arena (x86-64: the top 2 GiB above the image for
 * -mcmodel=kernel; AArch64: within +-128 MiB of the image for CALL26). */

struct vm_space kernel_space;

static struct kmem_cache *g_region_cache;
static struct vm_stats g_stats;
static bool g_initialized;

/* --- region bookkeeping (space lock held) --- */

static void region_footprint(const struct vm_region *r, vaddr_t *lo, vaddr_t *hi)
{
    *lo = r->base - ((r->flags & VM_REGION_GUARD_BELOW) ? PAGE_SIZE : 0);
    *hi = r->base + r->size + ((r->flags & VM_REGION_GUARD_ABOVE) ? PAGE_SIZE : 0);
}

static struct vm_region *space_find(struct vm_space *space, vaddr_t va)
{
    struct vm_region *r;
    list_for_each_entry(r, &space->regions, link) {
        if (va >= r->base && va < r->base + r->size)
            return r;
        if (r->base > va)
            break;
    }
    return NULL;
}

static int space_insert(struct vm_space *space, struct vm_region *region)
{
    vaddr_t lo, hi;
    region_footprint(region, &lo, &hi);

    struct vm_region *r;
    list_for_each_entry(r, &space->regions, link) {
        vaddr_t rlo, rhi;
        region_footprint(r, &rlo, &rhi);
        if (lo < rhi && rlo < hi)
            return -EEXIST;
        if (r->base > region->base) {
            list_insert_before(&r->link, &region->link);
            return 0;
        }
    }
    list_push_back(&space->regions, &region->link);
    return 0;
}

/* First-fit gap of `footprint` bytes inside [lo, hi). 0 if none. */
static vaddr_t range_find_free(struct vm_space *space, vaddr_t lo, vaddr_t hi, size_t footprint)
{
    vaddr_t cursor = lo;
    struct vm_region *r;

    list_for_each_entry(r, &space->regions, link) {
        vaddr_t rlo, rhi;
        region_footprint(r, &rlo, &rhi);
        if (rhi <= lo)
            continue;
        if (rlo >= hi)
            break;
        if (rlo >= cursor && rlo - cursor >= footprint)
            return cursor;
        if (rhi > cursor)
            cursor = rhi;
    }
    if (cursor < hi && hi - cursor >= footprint)
        return cursor;
    return 0;
}

static vaddr_t arena_find_free(struct vm_space *space, size_t footprint)
{
    return range_find_free(space, space->arena_lo, space->arena_hi, footprint);
}

static struct vm_region *region_new(vaddr_t base, size_t size, vm_prot_t prot, vm_cache_t cache,
                                    enum vm_region_kind kind, unsigned flags, paddr_t phys,
                                    const char *name)
{
    struct vm_region *r = kmem_cache_alloc(g_region_cache, KMEM_ZERO);
    if (r == NULL)
        return NULL;
    list_init(&r->link);
    r->base = base;
    r->size = size;
    r->prot = prot;
    r->cache = cache;
    r->kind = kind;
    r->flags = flags;
    r->phys = phys;
    r->name = name;
    return r;
}

/* Map a fixed physical range and record it. Used during init. */
static void map_phys_region(vaddr_t va, paddr_t pa, size_t size, vm_prot_t prot, unsigned map_flags,
                            const char *name)
{
    int rc = arch_mmu_map(&kernel_space.mmu, va, pa, size, prot, VM_CACHE_WB, map_flags);
    if (rc)
        panic("vmm: cannot map %s at %p (%d)", name, (void *)va, rc);

    struct vm_region *r = region_new(va, size, prot, VM_CACHE_WB, VM_REGION_PHYS, 0, pa, name);
    if (r == NULL || space_insert(&kernel_space, r))
        panic("vmm: cannot record region %s", name);
}

/* --- init --- */

static void map_kernel_image(const struct cosmoboot_info *info)
{
    struct {
        vaddr_t start, end;
        vm_prot_t prot;
        const char *name;
    } sections[] = {
        { (vaddr_t)__text_start, (vaddr_t)__text_end, VM_PROT_RX, "kernel-text" },
        { (vaddr_t)__rodata_start, (vaddr_t)__rodata_end, VM_PROT_READ, "kernel-rodata" },
        { (vaddr_t)__data_start, (vaddr_t)__bss_end, VM_PROT_RW, "kernel-data" },
    };

    for (size_t i = 0; i < ARRAY_SIZE(sections); i++) {
        vaddr_t va = sections[i].start;
        size_t size = sections[i].end - sections[i].start;
        if (size == 0)
            continue;
        KASSERT(is_page_aligned(va) && is_page_aligned(size));
        paddr_t pa = info->kernel_phys_base + (va - (vaddr_t)info->kernel_virt_base);
        map_phys_region(va, pa, size, sections[i].prot, ARCH_MMU_MAP_GLOBAL, sections[i].name);
    }
}

static void map_direct_map(const struct cosmoboot_info *info)
{
    uint32_t n;
    const struct cosmoboot_mem_entry *map = bootinfo_mem_map(&n);
    paddr_t limit = page_align_up(bootinfo_phys_limit());

    for (uint32_t i = 0; i < n; i++) {
        if (!bootinfo_mem_type_is_ram(map[i].type))
            continue;
        int rc = arch_mmu_map(&kernel_space.mmu, (vaddr_t)info->hhdm_base + map[i].base, map[i].base,
                              map[i].length, VM_PROT_RW, VM_CACHE_WB,
                              ARCH_MMU_MAP_LARGE | ARCH_MMU_MAP_GLOBAL);
        if (rc)
            panic("vmm: cannot map direct map for 0x%llx+0x%llx (%d)",
                  (unsigned long long)map[i].base, (unsigned long long)map[i].length, rc);
    }

    struct vm_region *r = region_new((vaddr_t)info->hhdm_base, (size_t)limit, VM_PROT_RW, VM_CACHE_WB,
                                     VM_REGION_PHYS, 0, 0, "direct-map");
    if (r == NULL || space_insert(&kernel_space, r))
        panic("vmm: cannot record direct map region");
}

static void vm_fault_handler(unsigned vector, struct arch_trap_frame *frame, void *arg);

void vmm_init(void)
{
    const struct cosmoboot_info *info = bootinfo_get();

    KASSERT(!g_initialized);

    spinlock_init(&kernel_space.lock, "kernel_space");
    list_init(&kernel_space.regions);
    kernel_space.arena_lo = (vaddr_t)KERNEL_ARENA_LO;
    kernel_space.arena_hi = (vaddr_t)KERNEL_ARENA_HI;
    arch_mmu_near_arena(&kernel_space.near_lo, &kernel_space.near_hi);
    if ((vaddr_t)__kernel_end > kernel_space.near_lo)
        panic("vmm: kernel image ends at %p, past the near arena start %p", (void *)__kernel_end,
              (void *)kernel_space.near_lo);

    g_region_cache = kmem_cache_create("vm_region", sizeof(struct vm_region), 0);
    if (g_region_cache == NULL)
        panic("vmm: cannot create region cache");

    if (arch_mmu_context_init(&kernel_space.mmu))
        panic("vmm: cannot allocate root page table");

    map_kernel_image(info);
    map_direct_map(info);

    /* Switch. From here the loader's tables are unreferenced. */
    arch_mmu_activate(&kernel_space.mmu);
    pmm_hhdm_limit = page_align_up(bootinfo_phys_limit());
    kdebug("vmm: kernel page tables active, root 0x%llx, direct map covers %llu MiB",
           (unsigned long long)kernel_space.mmu.root, (unsigned long long)(pmm_hhdm_limit >> 20));

    uint32_t n;
    const struct cosmoboot_mem_entry *map = bootinfo_mem_map(&n);
    uint64_t freed = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (map[i].type != COSMOBOOT_MEM_BOOT_PAGETABLES)
            continue;
        pmm_free_reserved_range(map[i].base, (size_t)map[i].length);
        freed += map[i].length;
    }
    kdebug("vmm: freed %llu KiB of bootstrap page tables", (unsigned long long)(freed >> 10));

    pmm_release_deferred();

    int vec = arch_trap_vector(ARCH_TRAP_PAGE_FAULT);
    KASSERT(vec >= 0);
    int rc = interrupt_register((unsigned)vec, vm_fault_handler, NULL, "vm-fault");
    if (rc)
        panic("vmm: cannot register page fault handler (%d)", rc);

    g_initialized = true;

    struct pmm_stats st;
    pmm_get_stats(&st);
    kinfo("vmm: %llu MiB free after takeover, arena %p-%p",
          (unsigned long long)((st.free_pages * PAGE_SIZE) >> 20),
          (void *)kernel_space.arena_lo, (void *)kernel_space.arena_hi);
}

/* --- kernel allocations --- */

static void free_populated_frames(struct vm_region *r)
{
    for (vaddr_t va = r->base; va < r->base + r->size; va += PAGE_SIZE) {
        paddr_t pa;
        if (!arch_mmu_query(&kernel_space.mmu, va, &pa, NULL, NULL, NULL))
            continue;
        struct page *page = phys_to_page(pa);
        KASSERT(page != NULL);
        pmm_free_page(page);
        g_stats.anon_pages--;
    }
}

vaddr_t vm_kernel_alloc(size_t size, unsigned flags, vm_prot_t prot)
{
    KASSERT(g_initialized);
    if (size == 0 || !is_page_aligned(size) || prot == VM_PROT_NONE)
        return 0;

    bool guard = (flags & VM_KALLOC_GUARD) != 0;
    size_t footprint = size + (guard ? 2 * PAGE_SIZE : 0);
    unsigned rflags = guard ? (VM_REGION_GUARD_BELOW | VM_REGION_GUARD_ABOVE) : 0;
    if (flags & VM_KALLOC_POPULATE)
        rflags |= VM_REGION_POPULATED;

    arch_irq_state_t s = spin_lock_irqsave(&kernel_space.lock);

    vaddr_t fp = (flags & VM_KALLOC_NEAR_KERNEL)
                     ? range_find_free(&kernel_space, kernel_space.near_lo, kernel_space.near_hi, footprint)
                     : arena_find_free(&kernel_space, footprint);
    if (fp == 0) {
        spin_unlock_irqrestore(&kernel_space.lock, s);
        return 0;
    }
    vaddr_t base = fp + (guard ? PAGE_SIZE : 0);

    struct vm_region *r = region_new(base, size, prot, VM_CACHE_WB, VM_REGION_ANON, rflags, 0, "kalloc");
    if (r == NULL || space_insert(&kernel_space, r)) {
        if (r)
            kmem_cache_free(g_region_cache, r);
        spin_unlock_irqrestore(&kernel_space.lock, s);
        return 0;
    }

    if (flags & VM_KALLOC_POPULATE) {
        for (vaddr_t va = base; va < base + size; va += PAGE_SIZE) {
            struct page *page = pmm_alloc_page(PMM_FLAGS_ZERO);
            int rc = page ? arch_mmu_map(&kernel_space.mmu, va, page_to_phys(page), PAGE_SIZE, prot,
                                         VM_CACHE_WB, ARCH_MMU_MAP_GLOBAL)
                          : -ENOMEM;
            if (rc) {
                if (page)
                    pmm_free_page(page);
                free_populated_frames(r);
                arch_mmu_unmap(&kernel_space.mmu, base, size);
                list_remove(&r->link);
                kmem_cache_free(g_region_cache, r);
                spin_unlock_irqrestore(&kernel_space.lock, s);
                return 0;
            }
            g_stats.anon_pages++;
        }
    }

    spin_unlock_irqrestore(&kernel_space.lock, s);
    return base;
}

#define TEARDOWN_CHUNK_PAGES 32u

/*
 * Tear down a region's mappings in chunks. Each chunk is unmapped under
 * the space lock (local invalidation), then, with the lock released so
 * other CPUs can take interrupts, shot down everywhere; only then are
 * the frames returned. Frames are never reused while a stale
 * translation to them may exist.
 */
static void region_teardown(struct vm_region *r, bool free_frames)
{
    vaddr_t base = r->base;
    size_t size = r->size;

    for (vaddr_t va = base; va < base + size; va += TEARDOWN_CHUNK_PAGES * PAGE_SIZE) {
        size_t chunk = MIN((size_t)(TEARDOWN_CHUNK_PAGES * PAGE_SIZE), (size_t)(base + size - va));
        struct page *frames[TEARDOWN_CHUNK_PAGES];
        unsigned n = 0;

        arch_irq_state_t s = spin_lock_irqsave(&kernel_space.lock);
        if (free_frames) {
            for (vaddr_t p = va; p < va + chunk; p += PAGE_SIZE) {
                paddr_t pa;
                if (!arch_mmu_query(&kernel_space.mmu, p, &pa, NULL, NULL, NULL))
                    continue;
                struct page *page = phys_to_page(pa);
                KASSERT(page != NULL);
                frames[n++] = page;
                g_stats.anon_pages--;
            }
        }
        int rc = arch_mmu_unmap(&kernel_space.mmu, va, chunk);
        KASSERT(rc == 0);
        spin_unlock_irqrestore(&kernel_space.lock, s);

        arch_mmu_shootdown(&kernel_space.mmu, va, chunk);

        for (unsigned i = 0; i < n; i++)
            pmm_free_page(frames[i]);
    }
}

void vm_kernel_free(vaddr_t base)
{
    KASSERT(g_initialized);

    arch_irq_state_t s = spin_lock_irqsave(&kernel_space.lock);
    struct vm_region *r = space_find(&kernel_space, base);
    if (r == NULL || r->base != base || r->kind != VM_REGION_ANON)
        panic("vm_kernel_free: %p is not a live kernel allocation", (void *)base);
    spin_unlock_irqrestore(&kernel_space.lock, s);

    region_teardown(r, true);

    s = spin_lock_irqsave(&kernel_space.lock);
    list_remove(&r->link);
    spin_unlock_irqrestore(&kernel_space.lock, s);
    kmem_cache_free(g_region_cache, r);
}

int vm_kernel_protect(vaddr_t base, vm_prot_t prot)
{
    KASSERT(g_initialized);
    if (prot == VM_PROT_NONE || (prot & VM_PROT_WRITE && prot & VM_PROT_EXEC) || (prot & VM_PROT_USER))
        return -EINVAL;

    arch_irq_state_t s = spin_lock_irqsave(&kernel_space.lock);
    struct vm_region *r = space_find(&kernel_space, base);
    if (r == NULL || r->base != base || r->kind != VM_REGION_ANON || (r->flags & VM_REGION_POPULATED) == 0) {
        spin_unlock_irqrestore(&kernel_space.lock, s);
        return -EINVAL;
    }
    int rc = arch_mmu_protect(&kernel_space.mmu, r->base, r->size, prot);
    if (rc == 0)
        r->prot = prot;
    size_t size = r->size;
    spin_unlock_irqrestore(&kernel_space.lock, s);
    if (rc)
        return rc;

    /* Dropping write or adding execute must reach every CPU before the
     * caller relies on it. */
    arch_mmu_shootdown(&kernel_space.mmu, base, size);
    return 0;
}

vaddr_t vm_map_phys(paddr_t pa, size_t size, vm_prot_t prot, vm_cache_t cache)
{
    KASSERT(g_initialized);
    if (size == 0 || !is_page_aligned(pa) || !is_page_aligned(size) || prot == VM_PROT_NONE)
        return 0;

    arch_irq_state_t s = spin_lock_irqsave(&kernel_space.lock);

    vaddr_t base = arena_find_free(&kernel_space, size);
    if (base == 0) {
        spin_unlock_irqrestore(&kernel_space.lock, s);
        return 0;
    }

    struct vm_region *r = region_new(base, size, prot, cache, VM_REGION_PHYS, 0, pa, "phys-map");
    if (r == NULL || space_insert(&kernel_space, r)) {
        if (r)
            kmem_cache_free(g_region_cache, r);
        spin_unlock_irqrestore(&kernel_space.lock, s);
        return 0;
    }

    int rc = arch_mmu_map(&kernel_space.mmu, base, pa, size, prot, cache,
                          ARCH_MMU_MAP_LARGE | ARCH_MMU_MAP_GLOBAL);
    if (rc) {
        arch_mmu_unmap(&kernel_space.mmu, base, size);
        list_remove(&r->link);
        kmem_cache_free(g_region_cache, r);
        spin_unlock_irqrestore(&kernel_space.lock, s);
        return 0;
    }

    spin_unlock_irqrestore(&kernel_space.lock, s);
    return base;
}

void vm_unmap_phys(vaddr_t base)
{
    KASSERT(g_initialized);

    arch_irq_state_t s = spin_lock_irqsave(&kernel_space.lock);

    struct vm_region *r = space_find(&kernel_space, base);
    if (r == NULL || r->base != base || r->kind != VM_REGION_PHYS ||
        base < kernel_space.arena_lo || base >= kernel_space.arena_hi)
        panic("vm_unmap_phys: %p is not a live physical mapping", (void *)base);
    spin_unlock_irqrestore(&kernel_space.lock, s);

    region_teardown(r, false);

    s = spin_lock_irqsave(&kernel_space.lock);
    list_remove(&r->link);
    spin_unlock_irqrestore(&kernel_space.lock, s);
    kmem_cache_free(g_region_cache, r);
}

const struct vm_region *vm_find_region(struct vm_space *space, vaddr_t va)
{
    arch_irq_state_t s = spin_lock_irqsave(&space->lock);
    const struct vm_region *r = space_find(space, va);
    spin_unlock_irqrestore(&space->lock, s);
    return r;
}

/* --- faults --- */

static void describe_region(const struct vm_region *r, char *buf, size_t len)
{
    if (r == NULL) {
        strlcpy(buf, "no region", len);
        return;
    }
    ksnprintf(buf, len, "region '%s' %p+0x%zx %s %c%c%c%s", r->name, (void *)r->base, r->size,
              r->kind == VM_REGION_ANON ? "anon" : "phys",
              (r->prot & VM_PROT_READ) ? 'r' : '-',
              (r->prot & VM_PROT_WRITE) ? 'w' : '-',
              (r->prot & VM_PROT_EXEC) ? 'x' : '-',
              (r->flags & (VM_REGION_GUARD_BELOW | VM_REGION_GUARD_ABOVE)) ? " guarded" : "");
}

static bool access_allowed(const struct vm_region *r, unsigned fl)
{
    if (fl & VM_FAULT_WRITE)
        return (r->prot & VM_PROT_WRITE) != 0;
    if (fl & VM_FAULT_EXEC)
        return (r->prot & VM_PROT_EXEC) != 0;
    return (r->prot & VM_PROT_READ) != 0;
}

static const struct vm_user_hooks *g_user_hooks;

void vm_set_user_hooks(const struct vm_user_hooks *hooks)
{
    g_user_hooks = hooks;
}

static void vm_fault_handler(unsigned vector, struct arch_trap_frame *frame, void *arg)
{
    (void)vector;
    (void)arg;

    vaddr_t addr = (vaddr_t)arch_trap_fault_address(frame);
    unsigned fl = arch_trap_fault_flags(frame);
    vaddr_t page = page_align_down(addr);
    bool kernel_addr = addr >= arch_mmu_kernel_base();
    struct vm_space *space = NULL;
    const struct vm_region *r = NULL;
    char desc[128];

    if (kernel_addr)
        space = &kernel_space;
    else if (g_user_hooks != NULL)
        space = g_user_hooks->current_space(); /* NULL for a kernel thread */

    if (space != NULL && g_initialized) {
        if (spin_is_held(&space->lock))
            panic_frame(frame, "page fault at %p while holding the vm_space lock", (void *)addr);

        arch_irq_state_t s = spin_lock_irqsave(&space->lock);
        r = space_find(space, addr);

        if (r != NULL && r->kind == VM_REGION_ANON && !(fl & (VM_FAULT_PRESENT | VM_FAULT_RESERVED)) &&
            access_allowed(r, fl)) {
            struct page *frame_page = pmm_alloc_page(PMM_FLAGS_ZERO);
            if (frame_page == NULL) {
                spin_unlock_irqrestore(&space->lock, s);
                if (!kernel_addr && (fl & VM_FAULT_USER))
                    g_user_hooks->fatal(addr, fl, frame); /* the process, not the kernel, runs out */
                panic_frame(frame, "out of memory populating %p in region '%s'", (void *)addr, r->name);
            }
            unsigned mflags = space->user ? ARCH_MMU_MAP_USER : ARCH_MMU_MAP_GLOBAL;
            int rc = arch_mmu_map(&space->mmu, page, page_to_phys(frame_page), PAGE_SIZE, r->prot,
                                  r->cache, mflags);
            if (rc) {
                spin_unlock_irqrestore(&space->lock, s);
                panic_frame(frame, "cannot map %p in region '%s' (%d)", (void *)addr, r->name, rc);
            }
            if (space->user)
                space->anon_pages++;
            else
                g_stats.anon_pages++;
            g_stats.faults_handled++;
            spin_unlock_irqrestore(&space->lock, s);
            return;
        }

        describe_region(r, desc, sizeof(desc));
        spin_unlock_irqrestore(&space->lock, s);
    } else {
        strlcpy(desc, kernel_addr ? "vmm not initialised" : "user address from a kernel thread", sizeof(desc));
    }

    /* A fault raised by user code that no region services ends the
     * process; the kernel never panics on user behaviour. */
    if ((fl & VM_FAULT_USER) && g_user_hooks != NULL)
        g_user_hooks->fatal(addr, fl, frame);

    panic_frame(frame, "page fault: %s %s at %p (%s): %s",
                (fl & VM_FAULT_USER) ? "user" : "kernel",
                (fl & VM_FAULT_EXEC) ? "execute" : (fl & VM_FAULT_WRITE) ? "write" : "read",
                (void *)addr,
                (fl & VM_FAULT_RESERVED) ? "reserved bit" : (fl & VM_FAULT_PRESENT) ? "protection" : "not present",
                desc);
}

/* --- user address spaces --- */

static struct kmem_cache *g_space_cache;

int vm_space_create_user(struct vm_space **out)
{
    KASSERT(g_initialized);
    if (g_space_cache == NULL) {
        g_space_cache = kmem_cache_create("vm_space", sizeof(struct vm_space), 64);
        if (g_space_cache == NULL)
            return -ENOMEM;
    }

    struct vm_space *space = kmem_cache_alloc(g_space_cache, KMEM_ZERO);
    if (space == NULL)
        return -ENOMEM;

    spinlock_init(&space->lock, "user_space");
    list_init(&space->regions);
    space->arena_lo = 0;
    space->arena_hi = 0;
    space->user = true;

    int rc = arch_mmu_context_init_user(&space->mmu, &kernel_space.mmu);
    if (rc) {
        kmem_cache_free(g_space_cache, space);
        return rc;
    }
    *out = space;
    return 0;
}

static void user_region_teardown(struct vm_space *space, struct vm_region *r)
{
    /* Same chunked protocol as the kernel arena; frames are owned by
     * the region for ANON. */
    vaddr_t base = r->base;
    size_t size = r->size;

    for (vaddr_t va = base; va < base + size; va += TEARDOWN_CHUNK_PAGES * PAGE_SIZE) {
        size_t chunk = MIN((size_t)(TEARDOWN_CHUNK_PAGES * PAGE_SIZE), (size_t)(base + size - va));
        struct page *frames[TEARDOWN_CHUNK_PAGES];
        unsigned n = 0;

        arch_irq_state_t s = spin_lock_irqsave(&space->lock);
        if (r->kind == VM_REGION_ANON) {
            for (vaddr_t p = va; p < va + chunk; p += PAGE_SIZE) {
                paddr_t pa;
                if (!arch_mmu_query(&space->mmu, p, &pa, NULL, NULL, NULL))
                    continue;
                struct page *page = phys_to_page(pa);
                KASSERT(page != NULL);
                frames[n++] = page;
                space->anon_pages--;
            }
        }
        int rc = arch_mmu_unmap(&space->mmu, va, chunk);
        KASSERT(rc == 0);
        spin_unlock_irqrestore(&space->lock, s);

        arch_mmu_shootdown(&space->mmu, va, chunk);

        for (unsigned i = 0; i < n; i++)
            pmm_free_page(frames[i]);
    }
}

void vm_space_destroy(struct vm_space *space)
{
    KASSERT(space != NULL && space->user);

    for (;;) {
        arch_irq_state_t s = spin_lock_irqsave(&space->lock);
        if (list_empty(&space->regions)) {
            spin_unlock_irqrestore(&space->lock, s);
            break;
        }
        struct vm_region *r = list_first_entry(&space->regions, struct vm_region, link);
        spin_unlock_irqrestore(&space->lock, s);

        user_region_teardown(space, r);

        s = spin_lock_irqsave(&space->lock);
        list_remove(&r->link);
        spin_unlock_irqrestore(&space->lock, s);
        kmem_cache_free(g_region_cache, r);
    }
    KASSERT(space->anon_pages == 0);

    arch_mmu_context_destroy(&space->mmu);
    kmem_cache_free(g_space_cache, space);
}

static bool user_range_valid(uint64_t base, size_t size)
{
    return is_page_aligned(base) && is_page_aligned(size) && size > 0 && base >= VM_USER_LO &&
           base + size > base && base + size <= VM_USER_HI;
}

int vm_user_map_anon(struct vm_space *space, uint64_t base, size_t size, vm_prot_t prot, unsigned flags,
                     const char *name)
{
    KASSERT(space->user);
    if (!user_range_valid(base, size) || prot == VM_PROT_NONE)
        return -EINVAL;
    if ((prot & VM_PROT_WRITE) && (prot & VM_PROT_EXEC))
        return -EINVAL;

    unsigned rflags = VM_REGION_USER | (flags & (VM_REGION_POPULATED | VM_REGION_GUARD_BELOW));
    struct vm_region *r = region_new((vaddr_t)base, size, prot & ~VM_PROT_USER, VM_CACHE_WB, VM_REGION_ANON,
                                     rflags, 0, name);
    if (r == NULL)
        return -ENOMEM;

    arch_irq_state_t s = spin_lock_irqsave(&space->lock);
    int rc = space_insert(space, r);
    if (rc) {
        spin_unlock_irqrestore(&space->lock, s);
        kmem_cache_free(g_region_cache, r);
        return rc;
    }

    if (flags & VM_REGION_POPULATED) {
        for (vaddr_t va = (vaddr_t)base; va < base + size; va += PAGE_SIZE) {
            struct page *page = pmm_alloc_page(PMM_FLAGS_ZERO);
            rc = page ? arch_mmu_map(&space->mmu, va, page_to_phys(page), PAGE_SIZE, r->prot, VM_CACHE_WB,
                                     ARCH_MMU_MAP_USER)
                      : -ENOMEM;
            if (rc) {
                if (page)
                    pmm_free_page(page);
                spin_unlock_irqrestore(&space->lock, s);
                /* Unwind: tear down what was populated and drop the region. */
                user_region_teardown(space, r);
                s = spin_lock_irqsave(&space->lock);
                list_remove(&r->link);
                spin_unlock_irqrestore(&space->lock, s);
                kmem_cache_free(g_region_cache, r);
                return rc;
            }
            space->anon_pages++;
        }
    }
    spin_unlock_irqrestore(&space->lock, s);
    return 0;
}

int vm_user_unmap(struct vm_space *space, uint64_t base, size_t size)
{
    KASSERT(space->user);

    arch_irq_state_t s = spin_lock_irqsave(&space->lock);
    struct vm_region *r = space_find(space, (vaddr_t)base);
    if (r == NULL || r->base != base || r->size != size) {
        spin_unlock_irqrestore(&space->lock, s);
        return -EINVAL;
    }
    spin_unlock_irqrestore(&space->lock, s);

    user_region_teardown(space, r);

    s = spin_lock_irqsave(&space->lock);
    list_remove(&r->link);
    spin_unlock_irqrestore(&space->lock, s);
    kmem_cache_free(g_region_cache, r);
    return 0;
}

int vm_user_protect(struct vm_space *space, uint64_t base, size_t size, vm_prot_t prot)
{
    KASSERT(space->user);
    if ((prot & VM_PROT_WRITE) && (prot & VM_PROT_EXEC))
        return -EINVAL;
    if (prot == VM_PROT_NONE)
        return -EINVAL;

    arch_irq_state_t s = spin_lock_irqsave(&space->lock);
    struct vm_region *r = space_find(space, (vaddr_t)base);
    if (r == NULL || r->base != base || r->size != size) {
        spin_unlock_irqrestore(&space->lock, s);
        return -EINVAL;
    }
    r->prot = prot & ~VM_PROT_USER;
    int rc = arch_mmu_protect(&space->mmu, (vaddr_t)base, size, prot);
    spin_unlock_irqrestore(&space->lock, s);
    if (rc == 0)
        arch_mmu_shootdown(&space->mmu, (vaddr_t)base, size);
    return rc;
}

uint64_t vm_user_find_free(struct vm_space *space, uint64_t from, size_t size)
{
    KASSERT(space->user);
    if (!is_page_aligned(size) || size == 0)
        return 0;
    if (from < VM_USER_LO)
        from = VM_USER_LO;
    from = page_align_up(from);

    arch_irq_state_t s = spin_lock_irqsave(&space->lock);
    vaddr_t cursor = (vaddr_t)from;
    struct vm_region *r;
    uint64_t result = 0;
    list_for_each_entry(r, &space->regions, link) {
        vaddr_t rlo, rhi;
        region_footprint(r, &rlo, &rhi);
        rhi += PAGE_SIZE; /* keep one unmapped page between user regions */
        if (rhi <= cursor)
            continue;
        if (rlo >= cursor && rlo - cursor >= size + PAGE_SIZE) {
            result = cursor;
            break;
        }
        if (rhi > cursor)
            cursor = rhi;
    }
    if (result == 0 && cursor + size + PAGE_SIZE <= VM_USER_HI)
        result = cursor;
    spin_unlock_irqrestore(&space->lock, s);
    return result;
}

bool vm_user_range_mapped(struct vm_space *space, uint64_t addr, size_t len, vm_prot_t prot)
{
    KASSERT(space->user);
    if (len == 0)
        return true;
    uint64_t end = addr + len;
    if (end < addr)
        return false;

    arch_irq_state_t s = spin_lock_irqsave(&space->lock);
    uint64_t cursor = page_align_down(addr);
    struct vm_region *r;
    bool ok = false;
    list_for_each_entry(r, &space->regions, link) {
        if (r->base + r->size <= cursor)
            continue;
        if (r->base > cursor)
            break; /* gap */
        if ((r->prot & prot) != prot)
            break;
        cursor = r->base + r->size;
        if (cursor >= end) {
            ok = true;
            break;
        }
    }
    spin_unlock_irqrestore(&space->lock, s);
    return ok;
}

/* --- diagnostics --- */

void vm_get_stats(struct vm_stats *out)
{
    arch_irq_state_t s = spin_lock_irqsave(&kernel_space.lock);
    *out = g_stats;
    out->regions = 0;
    struct vm_region *r;
    list_for_each_entry(r, &kernel_space.regions, link)
        out->regions++;
    spin_unlock_irqrestore(&kernel_space.lock, s);
}

void vm_dump(struct vm_space *space)
{
    arch_irq_state_t s = spin_lock_irqsave(&space->lock);
    struct vm_region *r;
    list_for_each_entry(r, &space->regions, link) {
        char desc[128];
        describe_region(r, desc, sizeof(desc));
        kprintf("  %s\n", desc);
    }
    spin_unlock_irqrestore(&space->lock, s);
}
