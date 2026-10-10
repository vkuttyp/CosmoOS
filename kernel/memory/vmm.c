/*
 * vmm.c - Kernel address space, regions, the VA arena, and page faults.
 *
 * The region list is sorted by base and scanned linearly; with a handful
 * of kernel regions that is the right structure. It becomes a tree when a
 * process has thousands of mappings, behind the same functions.
 */

#include <kernel/asid.h>
#include <kernel/bootinfo.h>
#include <kernel/completion.h>
#include <kernel/errno.h>
#include <kernel/futex.h>
#include <kernel/lockdep.h>
#include <kernel/pagecache.h>
#include <kernel/signal.h>
#include <kernel/vfs.h>
#include <kernel/interrupt.h>
#include <kernel/kernel.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/page.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/pmm.h>
#include <kernel/sched.h>
#include <kernel/printf.h>
#include <kernel/string.h>
#include <kernel/vmm.h>

#include <arch/cpu.h>
#include <arch/irq.h>
#include <arch/mmu.h>
#include <arch/trap.h>
#include <arch/user.h>
#include <kernel/faultinject.h>

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
static uint64_t g_map_tag = 1;   /* unique id per file-map record (0 = none) */

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

    /* Address-space tags. The width is the boot CPU's, fixed by its
     * feature setup long before this; no space exists yet to be tagged. */
    asid_init(arch_mmu_asid_bits());
    asid_boot_config();   /* opt/cosmo/asid=paranoid, if the boot asked for it */

    map_kernel_image(info);
    map_direct_map(info);
    /* Every kernel-half top-level entry a later mapping could need exists
     * now, before the first user root copies the kernel half (P9,
     * design.md §6.5). */
    if (arch_mmu_prepopulate(&kernel_space.mmu, kernel_space.arena_lo, kernel_space.arena_hi - kernel_space.arena_lo))
        panic("vmm: cannot pre-populate the arena's page tables");

    /* Switch. From here the loader's tables are unreferenced. */
    arch_mmu_activate(&kernel_space.mmu, true);
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
              r->kind == VM_REGION_ANON ? "anon" : r->kind == VM_REGION_FILE ? "file" : "phys",
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

static void user_shootdown(struct vm_space *space, vaddr_t va, size_t len);

/* --- the debug seam: a FILE fault held between its two phases --- */

#if CONFIG_DEBUG
static struct {
    unsigned state;               /* 0 idle, 1 armed, 2 held */
    struct vm_space *space;       /* held: whose fault */
    struct thread *holder;
    struct completion released;
    spinlock_t lock;
} g_file_hold = { .lock = SPINLOCK_INIT("vm-file-hold") };

void vm_test_file_hold_arm(void)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_file_hold.lock);
    completion_init(&g_file_hold.released, "vm-file-hold");
    g_file_hold.space = NULL;
    g_file_hold.holder = NULL;
    g_file_hold.state = 1;
    spin_unlock_irqrestore(&g_file_hold.lock, s);
}

unsigned vm_test_file_hold_state(void)
{
    return __atomic_load_n(&g_file_hold.state, __ATOMIC_ACQUIRE);
}

/* After phase one: if armed, this fault becomes the held one and waits. */
static void file_hold_seam(struct vm_space *space)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_file_hold.lock);
    bool take = g_file_hold.state == 1;
    if (take) {
        g_file_hold.state = 2;
        g_file_hold.space = space;
        g_file_hold.holder = thread_current();
    }
    spin_unlock_irqrestore(&g_file_hold.lock, s);
    if (!take)
        return;
    wait_for_completion(&g_file_hold.released);
    s = spin_lock_irqsave(&g_file_hold.lock);
    g_file_hold.state = 0;
    g_file_hold.space = NULL;
    g_file_hold.holder = NULL;
    spin_unlock_irqrestore(&g_file_hold.lock, s);
}

/* The two events that release a held fault: another FILE fault in the
 * same space installed, or part of the space was unmapped. */
static void file_hold_release(struct vm_space *space)
{
    if (__atomic_load_n(&g_file_hold.state, __ATOMIC_ACQUIRE) != 2)
        return;
    arch_irq_state_t s = spin_lock_irqsave(&g_file_hold.lock);
    bool fire = g_file_hold.state == 2 && g_file_hold.space == space && g_file_hold.holder != thread_current();
    spin_unlock_irqrestore(&g_file_hold.lock, s);
    if (fire)
        complete(&g_file_hold.released);
}
#else
void vm_test_file_hold_arm(void) {}
unsigned vm_test_file_hold_state(void) { return 0; }
static inline void file_hold_seam(struct vm_space *space) { (void)space; }
static inline void file_hold_release(struct vm_space *space) { (void)space; }
#endif

/* --- the debug seam: an ANON fault held before it installs --- */

/*
 * The anonymous arm services a fault under the space lock in one
 * stretch, so it cannot be held in the middle the way a FILE fault is.
 * What this seam holds is the fault BEFORE it installs, with the fault
 * flags the hardware already gave it -- which is the whole point: while
 * it waits, another thread installs that very page, and the held thread
 * then resumes still believing the page is absent. That is the race the
 * present-check stands against, and it is why the seam must resume into
 * the same fault rather than return and take a fresh one.
 */
#if CONFIG_DEBUG
static struct {
    unsigned state;               /* 0 idle, 1 armed, 2 held */
    vaddr_t va;                   /* the one page this seam is about */
    struct vm_space *space;       /* held: whose fault */
    struct thread *holder;
    struct completion released;
    spinlock_t lock;
} g_anon_hold = { .lock = SPINLOCK_INIT("vm-anon-hold") };

void vm_test_anon_hold_arm(vaddr_t va)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_anon_hold.lock);
    completion_init(&g_anon_hold.released, "vm-anon-hold");
    g_anon_hold.va = page_align_down(va);
    g_anon_hold.space = NULL;
    g_anon_hold.holder = NULL;
    g_anon_hold.state = 1;
    spin_unlock_irqrestore(&g_anon_hold.lock, s);
}

unsigned vm_test_anon_hold_state(void)
{
    return __atomic_load_n(&g_anon_hold.state, __ATOMIC_ACQUIRE);
}

/* True when this fault has become the held one; the caller must then
 * drop the space lock and call anon_hold_wait(). */
static bool anon_hold_take(struct vm_space *space, vaddr_t va)
{
    if (__atomic_load_n(&g_anon_hold.state, __ATOMIC_ACQUIRE) != 1)
        return false;
    arch_irq_state_t s = spin_lock_irqsave(&g_anon_hold.lock);
    bool take = g_anon_hold.state == 1 && g_anon_hold.va == va;
    if (take) {
        g_anon_hold.state = 2;
        g_anon_hold.space = space;
        g_anon_hold.holder = thread_current();
    }
    spin_unlock_irqrestore(&g_anon_hold.lock, s);
    return take;
}

/*
 * Disarm a seam nobody has taken yet. A seam that IS taken (state 2)
 * cannot be dropped this way: a thread is asleep inside it, and the
 * only thing that wakes it is the install it is waiting for. So a test
 * abandoning the race disarms FIRST -- so its own touch is not caught
 * -- and then touches the page, which installs it and releases any
 * holder. Returns the state it found.
 */
unsigned vm_test_anon_hold_disarm(void)
{
    arch_irq_state_t s = spin_lock_irqsave(&g_anon_hold.lock);
    unsigned was = g_anon_hold.state;
    if (was == 1) {
        g_anon_hold.state = 0;
        g_anon_hold.va = 0;
    }
    spin_unlock_irqrestore(&g_anon_hold.lock, s);
    return was;
}

static void anon_hold_wait(void)
{
    wait_for_completion(&g_anon_hold.released);
    arch_irq_state_t s = spin_lock_irqsave(&g_anon_hold.lock);
    g_anon_hold.state = 0;
    g_anon_hold.space = NULL;
    g_anon_hold.holder = NULL;
    spin_unlock_irqrestore(&g_anon_hold.lock, s);
}

/* The event that releases a held fault: another thread of the same space
 * installed THE armed page -- the collision itself, not any nearby
 * fault. Called with no lock held. */
static void anon_hold_release(struct vm_space *space, vaddr_t va)
{
    if (__atomic_load_n(&g_anon_hold.state, __ATOMIC_ACQUIRE) != 2)
        return;
    arch_irq_state_t s = spin_lock_irqsave(&g_anon_hold.lock);
    bool fire = g_anon_hold.state == 2 && g_anon_hold.space == space && g_anon_hold.va == va &&
                g_anon_hold.holder != thread_current();
    spin_unlock_irqrestore(&g_anon_hold.lock, s);
    if (fire)
        complete(&g_anon_hold.released);
}
#else
void vm_test_anon_hold_arm(vaddr_t va) { (void)va; }
unsigned vm_test_anon_hold_state(void) { return 0; }
unsigned vm_test_anon_hold_disarm(void) { return 0; }
static inline bool anon_hold_take(struct vm_space *space, vaddr_t va) { (void)space; (void)va; return false; }
static inline void anon_hold_wait(void) {}
static inline void anon_hold_release(struct vm_space *space, vaddr_t va) { (void)space; (void)va; }
#endif

/* --- the FILE fault, phases two and three --- */

/*
 * A frame installed in a user space is owned two ways: a page-cache
 * frame (PG_PAGECACHE) is the cache's, and the mapping holds one
 * reference to it per PTE; an anonymous frame -- demand-zero, populated,
 * or a copy-on-write copy -- is the mapping's alone at reference 1. Both
 * are released with pmm_page_put, which frees on the last reference, so
 * the teardown does not need to know which it holds beyond the counter
 * it credits.
 */
static void frame_uncount(struct vm_space *space, struct page *page)
{
    if (page->flags & PG_PAGECACHE)
        space->file_pages--;
    else
        space->anon_pages--;
}

/*
 * Phase two under the cache mutex, phase three under the space lock
 * nested inside it (docs/audit/next-subsystem-file-regions.md, "The
 * fault, in two phases under one lock order"). Returns 0 when the page
 * is installed OR when the world changed and the instruction should
 * simply retry; -ENOMEM when a private copy could not be allocated (the
 * anonymous rule); any other error when the file could not supply the
 * page (SIGBUS).
 */
static int file_fault(struct vm_space *space, vaddr_t va, unsigned fl, struct vnode *vn, uint64_t index,
                      bool shared, bool from_user)
{
    (void)from_user;
    bool write = (fl & VM_FAULT_WRITE) != 0;
    might_sleep();   /* readpage may sleep; the copies assert this rule for themselves */
    file_hold_seam(space);

    pagecache_lock(vn);
    struct page *cache = NULL, *copy = NULL, *to_put = NULL;
    int rc = pagecache_fault_page(vn, index, shared && write, &cache);
    if (rc) {
        pagecache_unlock(vn);
        return rc;
    }
    if (!shared && write) {
        /* Copy-on-write: the copy is what this mapping owns from here;
         * the cache frame is read under the mutex and not referenced. */
        copy = pmm_alloc_page(0);
        if (copy == NULL) {
            pmm_page_put(cache);
            pagecache_unlock(vn);
            return -ENOMEM;
        }
        memcpy(page_to_virt(copy), page_to_virt(cache), PAGE_SIZE);
        pmm_page_put(cache);
        cache = NULL;
    }

    /* Phase three: the region is found again, and identity is by what
     * it maps -- vnode, index, sharing -- not by pointer. */
    bool shoot = false;
    arch_irq_state_t s = spin_lock_irqsave(&space->lock);
    struct vm_region *r = space_find(space, va);
    bool same = r != NULL && r->kind == VM_REGION_FILE && !(r->flags & VM_REGION_QUIESCED) &&
                r->fmap->vn == vn && r->fmap->shared == shared &&
                (va - r->fmap->base + r->fmap->off) / PAGE_SIZE == index && access_allowed(r, fl);
    if (!same) {
        spin_unlock_irqrestore(&space->lock, s);
        if (copy)
            pmm_free_page(copy);
        if (cache)
            pmm_page_put(cache);
        pagecache_unlock(vn);
        __atomic_fetch_add(&g_stats.file_fault_retries, 1, __ATOMIC_RELAXED);
        return 0;
    }

    paddr_t pa;
    unsigned mflags = ARCH_MMU_MAP_USER;
    if (arch_mmu_query(&space->mmu, va, &pa, NULL, NULL, NULL)) {
        struct page *present = phys_to_page(pa);
        if (shared && write) {
            /* Installed clean, dirty now (phase two marked it): the one
             * in-place upgrade. The PTE already holds its reference. */
            KASSERT(present == cache);
            int prc = arch_mmu_protect(&space->mmu, va, PAGE_SIZE, r->prot);
            KASSERT(prc == 0);
            (void)prc;
            pmm_page_put(cache);
            shoot = true;
            __atomic_fetch_add(&g_stats.file_dirty_faults, 1, __ATOMIC_RELAXED);
        } else if (!shared && write && (present->flags & PG_PAGECACHE)) {
            /* A private read installed the cache frame read-only; the
             * write must not go through it. The PTE is REPLACED by the
             * copy, never raised. The copy is anonymous memory and
             * counts against COSMO_RLIMIT_MEM like the not-present
             * copy below: a process could otherwise read every page of
             * a private mapping and then write them all past its limit
             * (review found the check missing here). */
            if (space->anon_pages >= space->limit_anon_pages) {   /* COSMO_RLIMIT_MEM */
                spin_unlock_irqrestore(&space->lock, s);
                pmm_free_page(copy);   /* the present frame's reference is the PTE's, not ours */
                pagecache_unlock(vn);
                return -ENOMEM;
            }
            int urc = arch_mmu_unmap(&space->mmu, va, PAGE_SIZE);
            KASSERT(urc == 0);
            (void)urc;
            space->file_pages--;
            to_put = present;
            int mrc = arch_mmu_map(&space->mmu, va, page_to_phys(copy), PAGE_SIZE, r->prot, r->cache, mflags);
            if (mrc)
                panic("cannot map %p in region '%s' (%d)", (void *)va, r->name, mrc);
            space->anon_pages++;
            copy = NULL;
            shoot = true;
            __atomic_fetch_add(&g_stats.file_cow_faults, 1, __ATOMIC_RELAXED);
        } else {
            /* Another thread installed this page first, or a private
             * copy already stands here: nothing is mapped twice. */
            if (copy)
                pmm_free_page(copy);
            if (cache)
                pmm_page_put(cache);
            __atomic_fetch_add(&g_stats.file_fault_retries, 1, __ATOMIC_RELAXED);
        }
    } else if (!shared && write) {
        if (space->anon_pages >= space->limit_anon_pages) {   /* COSMO_RLIMIT_MEM */
            spin_unlock_irqrestore(&space->lock, s);
            pmm_free_page(copy);
            pagecache_unlock(vn);
            return -ENOMEM;
        }
        int mrc = arch_mmu_map(&space->mmu, va, page_to_phys(copy), PAGE_SIZE, r->prot, r->cache, mflags);
        if (mrc)
            panic("cannot map %p in region '%s' (%d)", (void *)va, r->name, mrc);
        space->anon_pages++;
        if (r->prot & VM_PROT_EXEC)
            arch_mmu_sync_icache_user(va, PAGE_SIZE);   /* M41: bytes just copied become instructions */
        __atomic_fetch_add(&g_stats.file_cow_faults, 1, __ATOMIC_RELAXED);
    } else {
        /* The cache frame itself. Writable only for a shared write that
         * dirtied it; otherwise read-only, so the first write faults and
         * the page is marked dirty then (the rule: a shared page's PTE
         * gains write only here). */
        vm_prot_t prot = (shared && write) ? r->prot : (r->prot & ~VM_PROT_WRITE);
        int mrc = arch_mmu_map(&space->mmu, va, page_to_phys(cache), PAGE_SIZE, prot, r->cache, mflags);
        if (mrc)
            panic("cannot map %p in region '%s' (%d)", (void *)va, r->name, mrc);
        space->file_pages++;
        if (r->prot & VM_PROT_EXEC)
            arch_mmu_sync_icache_user(va, PAGE_SIZE);   /* M41: the page was filled by readpage */
        __atomic_fetch_add(&g_stats.file_faults, 1, __ATOMIC_RELAXED);
        if (shared && write)
            __atomic_fetch_add(&g_stats.file_dirty_faults, 1, __ATOMIC_RELAXED);
    }
    spin_unlock_irqrestore(&space->lock, s);
    if (shoot)
        user_shootdown(space, va, PAGE_SIZE);
    if (to_put)
        pmm_page_put(to_put);
    pagecache_unlock(vn);
    file_hold_release(space);
    return 0;
}

/*
 * A write to a present, read-only anonymous frame in a private region
 * whose protection allows the write: a frame vm_space_fork left shared
 * (roadmap M3; design.md, "fork"). Space lock held, `r` found under it.
 *
 * While another mapping holds the frame (refcount above one) the writer
 * gets a copy; when this mapping is the last user it takes the frame back
 * writable. The count is safe to decide on: only a vm_space_fork of a
 * space that maps the frame can raise it, and for this space that fork
 * holds this lock; a fork of another space mapping it raises a count that
 * is already above one, where the answer -- copy -- is the same.
 *
 * Returns 0 when the write may proceed (copied, taken back, or already
 * writable because another thread got here first), -ENOMEM when no frame
 * could be had for the copy, -EAGAIN when the PTE is not this case (the
 * caller goes on to the other arms). *put is the frame whose reference
 * the caller drops after unlocking; *shoot asks for a shootdown of `va`.
 */
static int cow_write_locked(struct vm_space *space, const struct vm_region *r, vaddr_t va, struct page **put,
                            bool *shoot)
{
    paddr_t pa;
    vm_prot_t cur;
    *put = NULL;
    *shoot = false;
    if (!arch_mmu_query(&space->mmu, va, &pa, &cur, NULL, NULL))
        return -EAGAIN;   /* gone: the not-present arms decide */
    struct page *pg = phys_to_page(pa);
    if (pg->flags & PG_PAGECACHE)
        return -EAGAIN;   /* a cache frame: the FILE arm replaces it with a copy */
    if (cur & VM_PROT_WRITE)
        return 0;         /* another thread resolved it */
    if (__atomic_load_n(&pg->refcount, __ATOMIC_ACQUIRE) == 1) {
        int prc = arch_mmu_protect(&space->mmu, va, PAGE_SIZE, r->prot);
        KASSERT(prc == 0);
        (void)prc;
        *shoot = true;   /* a stale read-only translation must not keep faulting */
        __atomic_fetch_add(&g_stats.cow_reuses, 1, __ATOMIC_RELAXED);
        return 0;
    }
    struct page *copy = pmm_alloc_page(0);
    if (copy == NULL)
        return -ENOMEM;
    memcpy(page_to_virt(copy), page_to_virt(pg), PAGE_SIZE);
    int urc = arch_mmu_unmap(&space->mmu, va, PAGE_SIZE);
    KASSERT(urc == 0);
    (void)urc;
    int mrc = arch_mmu_map(&space->mmu, va, page_to_phys(copy), PAGE_SIZE, r->prot, r->cache, ARCH_MMU_MAP_USER);
    if (mrc)
        panic("cannot map %p in region '%s' (%d)", (void *)va, r->name, mrc);
    /* anon_pages is unchanged: one anonymous frame replaced another. */
    *put = pg;
    *shoot = true;
    __atomic_fetch_add(&g_stats.cow_copies, 1, __ATOMIC_RELAXED);
    return 0;
}

/* An anonymous frame another mapping also holds (fork). */
static bool frame_shared_anon(const struct page *pg)
{
    return !(pg->flags & PG_PAGECACHE) && __atomic_load_n(&pg->refcount, __ATOMIC_ACQUIRE) > 1;
}

static bool cow_candidate(const struct vm_space *space, const struct vm_region *r, unsigned fl)
{
    return r != NULL && space->user && !(r->flags & VM_REGION_QUIESCED) && (fl & VM_FAULT_WRITE) &&
           (fl & VM_FAULT_PRESENT) && !(fl & VM_FAULT_RESERVED) &&
           (r->kind == VM_REGION_ANON || (r->kind == VM_REGION_FILE && !r->fmap->shared));
}

#if CONFIG_SELFTEST
int vm_test_write_fault(struct vm_space *space, vaddr_t va)
{
    va = page_align_down(va);
    struct page *put;
    bool shoot;
    arch_irq_state_t s = spin_lock_irqsave(&space->lock);
    struct vm_region *r = space_find(space, va);
    int rc = -EFAULT;
    if (cow_candidate(space, r, VM_FAULT_WRITE | VM_FAULT_PRESENT | VM_FAULT_USER) &&
        access_allowed(r, VM_FAULT_WRITE | VM_FAULT_PRESENT | VM_FAULT_USER))
        rc = cow_write_locked(space, r, va, &put, &shoot);
    spin_unlock_irqrestore(&space->lock, s);
    if (rc == 0 && shoot)
        user_shootdown(space, va, PAGE_SIZE);
    if (rc == 0 && put)
        pmm_page_put(put);
    return rc;
}
#endif

static void vm_fault_handler(unsigned vector, struct arch_trap_frame *frame, void *arg)
{
    (void)vector;
    (void)arg;

    vaddr_t addr = (vaddr_t)arch_trap_fault_address(frame);
    unsigned fl = arch_trap_fault_flags(frame);
    vaddr_t page = page_align_down(addr);
    bool kernel_addr = addr >= arch_mmu_kernel_base();
    bool from_user = (fl & VM_FAULT_USER) != 0;
    struct vm_space *space = NULL;
    const struct vm_region *r = NULL;
    char desc[128];

    /* User code touching the kernel half is fatal at once: kernel_space is
     * never selected on a user frame's behalf (a lazily mapped kernel
     * region must not be populated by an unprivileged process). */
    if (from_user && kernel_addr && g_user_hooks != NULL) {
        g_user_hooks->fatal(addr, fl, frame, SIGSEGV);
        return;   /* a handler frame was set up */
    }
    int fatal_sig = SIGSEGV;

    if (kernel_addr)
        space = &kernel_space;
    else if (g_user_hooks != NULL)
        space = g_user_hooks->current_space(); /* NULL for a kernel thread */

    bool oom = false;
    if (space != NULL && g_initialized) {
        if (spin_is_held(&space->lock))
            panic_frame(frame, "page fault at %p while holding the vm_space lock", (void *)addr);

        arch_irq_state_t s = spin_lock_irqsave(&space->lock);
        r = space_find(space, addr);

        /*
         * A replacement owns this range and is tearing its pages down.
         * Installing one now would hand the teardown a page belonging to
         * the mapping that replaces it -- it would query it, unmap it and
         * free it, leaving a live region holding a freed frame. So:
         * nothing is installed. A user fault returns and the instruction
         * runs again, by which time the claim is normally gone; the yield
         * is there so a single CPU cannot spin out the replacer. A
         * kernel-mode fault inside a user copy must NOT retry -- it could
         * be uninterruptible -- so it takes the fixup and reports -EFAULT,
         * which is the truth: that memory is being replaced.
         */
        if (r != NULL && (r->flags & VM_REGION_QUIESCED)) {
            spin_unlock_irqrestore(&space->lock, s);
            if (!from_user)
                goto unserviced;
            sched_yield();
            return;
        }

        /* A write to a frame fork left shared (cow_write_locked). Only
         * for a fault taken with interrupts enabled -- every user-mode
         * fault, and a kernel copy in an ordinary system call: resolving
         * it needs a shootdown, which needs them on. A kernel copy with
         * them masked takes its fixup (-EFAULT) below rather than
         * resolve a fault it cannot complete safely. */
        if (cow_candidate(space, r, fl) && access_allowed(r, fl) && arch_trap_frame_irqs_enabled(frame)) {
            struct page *put;
            bool shoot;
            int crc = cow_write_locked(space, r, page, &put, &shoot);
            if (crc != -EAGAIN) {
                spin_unlock_irqrestore(&space->lock, s);
                if (crc == -ENOMEM) {
                    oom = true;   /* the anonymous rule: the process runs out, not the kernel */
                    goto unserviced;
                }
                if (shoot) {
                    /* With interrupts on, as the interrupted context had
                     * them (the FILE arm does the same). */
                    arch_irq_enable();
                    user_shootdown(space, page, PAGE_SIZE);
                    arch_irq_disable();
                }
                if (put)
                    pmm_page_put(put);
                return;
            }
        }

        /*
         * A file's page: phase one of the FILE fault. Everything the
         * cache half needs is copied out under the lock -- the vnode
         * (referenced), the file index, the sharing, the region's
         * protection -- and the lock is released, because the page may
         * have to be read from disk and that sleeps. The region is found
         * AGAIN under the lock before anything is installed (file_fault).
         */
        if (r != NULL && r->kind == VM_REGION_FILE && !(fl & VM_FAULT_RESERVED) && access_allowed(r, fl) &&
            space->user) {
            struct vm_file_map *m = r->fmap;
            struct vnode *vn = m->vn;
            vnode_get(vn);
            uint64_t index = (page - m->base + m->off) / PAGE_SIZE;
            bool shared = m->shared;
            spin_unlock_irqrestore(&space->lock, s);

            /*
             * A trap runs with interrupts masked as the hardware left
             * them, which the anonymous arm never minded: it neither
             * sleeps nor shoots down. This arm does both, so it runs
             * with interrupts as the interrupted context had them --
             * enabled for user code and for a copy inside a system
             * call, and left masked for a fault taken with them masked,
             * which is a context that must not sleep and which
             * might_sleep() then reports. Found by the shared-futex
             * unit's tests, as a shootdown asserting arch_irq_enabled().
             */
            bool enable = arch_trap_frame_irqs_enabled(frame);
            if (enable)
                arch_irq_enable();
            int frc = file_fault(space, page, fl, vn, index, shared, from_user);
            if (enable)
                arch_irq_disable();
            vnode_put(vn);
            if (frc == 0)
                return;   /* installed, or the world changed and the instruction retries */
            if (frc == -ENOMEM) {
                oom = true;   /* memory for a private copy: the anonymous rule applies */
                r = NULL;
                goto unserviced;
            }
            /* The file could not supply the page: past its end, or a read
             * that failed. SIGBUS for user code; a kernel copy takes its
             * fixup and reports -EFAULT, which is the truth. */
            __atomic_fetch_add(&g_stats.file_sigbus, 1, __ATOMIC_RELAXED);
            fatal_sig = SIGBUS;
            r = NULL;
            goto unserviced;
        }

        if (r != NULL && r->kind == VM_REGION_ANON && !(fl & (VM_FAULT_PRESENT | VM_FAULT_RESERVED)) &&
            access_allowed(r, fl)) {
            /*
             * The fault flags are the hardware's snapshot from the moment
             * the trap was taken, so "not present" is what WAS true, not
             * what is true now. Two threads of one process faulting the
             * same anonymous page both arrive here saying not-present;
             * the second takes the lock after the first has installed the
             * page. The PTE is the authority, so it is asked under the
             * lock: a page already standing here means another thread
             * served this fault, and the instruction simply runs again.
             * Mapping over it would return -EEXIST, which this path used
             * to panic on -- reachable since the ELF loader began
             * demand-paging the zero tail of a segment (the bss) instead
             * of populating it at load time.
             */
            /* Held only for a fault taken with interrupts enabled:
             * waiting sleeps, and a fault taken with them masked is a
             * context that must not. The region is found again on the
             * way out, because the wait let the world move. */
            if (arch_trap_frame_irqs_enabled(frame) && anon_hold_take(space, page)) {
                spin_unlock_irqrestore(&space->lock, s);
                arch_irq_enable();
                anon_hold_wait();
                arch_irq_disable();
                s = spin_lock_irqsave(&space->lock);
                r = space_find(space, addr);
                if (r == NULL || r->kind != VM_REGION_ANON || (r->flags & VM_REGION_QUIESCED) ||
                    !access_allowed(r, fl)) {
                    spin_unlock_irqrestore(&space->lock, s);
                    return;   /* the world changed; the instruction runs again */
                }
            }
            if (arch_mmu_query(&space->mmu, page, NULL, NULL, NULL, NULL)) {
                spin_unlock_irqrestore(&space->lock, s);
                __atomic_fetch_add(&g_stats.anon_fault_retries, 1, __ATOMIC_RELAXED);
                return;
            }
            struct page *frame_page = NULL;
            /* A page already attached with PROT_NONE cannot reach here (its
             * region's prot forbids the access); a fresh page is allocated.
             * Debug builds can inject the allocation failure on user spaces
             * (FI_DEMAND_PAGE for user-mode faults, FI_DEMAND_COPY for a
             * kernel-mode fault inside a user copy) to exercise the paths
             * below. */
            bool over_limit = space->user && space->anon_pages >= space->limit_anon_pages;   /* COSMO_RLIMIT_MEM */
            if (!over_limit && !(space->user && faultinject_should_fail(from_user ? FI_DEMAND_PAGE : FI_DEMAND_COPY)))
                frame_page = pmm_alloc_page(PMM_FLAGS_ZERO);
            if (frame_page == NULL) {
                spin_unlock_irqrestore(&space->lock, s);
                oom = true;
                goto unserviced;
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
            anon_hold_release(space, page);   /* a fault held on THIS page waits for exactly this */
            return;
        }

        describe_region(r, desc, sizeof(desc));
        spin_unlock_irqrestore(&space->lock, s);
    } else {
        strlcpy(desc, kernel_addr ? "vmm not initialised" : "user address from a kernel thread", sizeof(desc));
    }

unserviced:
    /* A fault raised by user code that no region services ends the
     * process; the kernel never panics on user behaviour. This includes
     * running out of memory on a demand-zero page: the process, not the
     * kernel, is what runs out. */
    if (from_user && g_user_hooks != NULL) {
        g_user_hooks->fatal(addr, fl, frame, fatal_sig);
        return;   /* a handler frame was set up */
    }

    /* A kernel-mode fault on a user address inside a user copy resumes at
     * the copy's fixup, which reports -EFAULT (design.md §6.1). Kernel
     * addresses never have a fixup: a hit there would hide a kernel bug. */
    if (!kernel_addr && arch_trap_fixup(frame)) {
        g_stats.fixups++;
        return;
    }

    if (oom)
        panic_frame(frame, "out of memory populating %p in region '%s'", (void *)addr, r ? r->name : "?");
    if (fatal_sig == SIGBUS)
        panic_frame(frame, "kernel fault at %p: a file page the file could not supply", (void *)addr);
    panic_frame(frame, "page fault: %s %s at %p (%s): %s",
                from_user ? "user" : "kernel",
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
    mutex_init(&space->replace_lock, "vm_replace");
    list_init(&space->regions);
    space->arena_lo = 0;
    space->arena_hi = 0;
    space->user = true;
    space->users = 1;
    space->tlb_cpus = 0;
    space->mapped_pages = 0;
    space->limit_mapped_pages = UINT64_MAX;
    space->limit_anon_pages = UINT64_MAX;

    int rc = arch_mmu_context_init_user(&space->mmu, &kernel_space.mmu);
    if (rc) {
        kmem_cache_free(g_space_cache, space);
        return rc;
    }
    *out = space;
    return 0;
}

/*
 * The CPUs that must be told about a PTE change in a user space: those
 * whose root is the space right now, plus the caller. The fence orders
 * the PTE write (already done under the lock) before the mask read; a
 * CPU switching in sets its bit (a full barrier) before it loads the root,
 * so it is either in the mask or loads the changed table (design.md §6.4).
 */
static cpumask_t user_shootdown_targets(struct vm_space *space)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return __atomic_load_n(&space->tlb_cpus, __ATOMIC_SEQ_CST) | CPUMASK_OF(arch_cpu_id());
}

static void user_shootdown(struct vm_space *space, vaddr_t va, size_t len)
{
    /* Preemption off from the mask to the return: the mask names this
     * CPU as the one that flushes locally, and a sender that moved in
     * between would flush the wrong one (S25; design.md §6.4 covers a
     * CPU that joins the space meanwhile, not a sender that leaves). */
    preempt_disable();
    arch_mmu_shootdown_cpus(&space->mmu, va, len, user_shootdown_targets(space));
    preempt_enable();
}

void vm_space_set_limits(struct vm_space *space, uint64_t mapped_pages, uint64_t anon_pages)
{
    KASSERT(space->user);
    arch_irq_state_t s = spin_lock_irqsave(&space->lock);
    space->limit_mapped_pages = mapped_pages;
    space->limit_anon_pages = anon_pages;
    spin_unlock_irqrestore(&space->lock, s);
}

void vm_space_switch(struct vm_space *prev, struct vm_space *next)
{
    unsigned cpu = arch_cpu_id();
    (void)prev;
    if (next->user)
        __atomic_fetch_or(&next->tlb_cpus, CPUMASK_OF(cpu), __ATOMIC_SEQ_CST);
    /* The tag, and whether this CPU must drop every tag it holds before
     * using one of the current generation. The kernel's root runs under
     * tag 0 and is passed as NULL: giving it a tag of its own would
     * consume one per generation that nothing releases, and
     * `arch_mmu_activate` would ignore it anyway. */
    bool flush = asid_switch_prepare(next->user ? &next->mmu : NULL);
    arch_mmu_activate(&next->mmu, flush);
    /*
     * `prev`'s bit is deliberately not cleared. It was cleared when a
     * root switch dropped every translation of the outgoing space on
     * this CPU; a tagged switch drops nothing, so this CPU goes on
     * holding `prev`'s translations until something flushes them, and
     * the mask has to say so. See M35.
     */
}

/*
 * Release the mappings of a user address range in chunks: frames are
 * collected and the chunk unmapped under the lock, the range is shot
 * down on the CPUs running the space with the lock released, and only
 * then are the frames freed. The caller has already unlinked or shrunk
 * the regions covering the range, so a fault in the window finds no
 * region and cannot repopulate a page about to be freed.
 */
static void user_range_teardown(struct vm_space *space, vaddr_t base, size_t size)
{
    for (vaddr_t va = base; va < base + size; va += TEARDOWN_CHUNK_PAGES * PAGE_SIZE) {
        size_t chunk = MIN((size_t)(TEARDOWN_CHUNK_PAGES * PAGE_SIZE), (size_t)(base + size - va));
        struct page *frames[TEARDOWN_CHUNK_PAGES];
        unsigned n = 0;

        arch_irq_state_t s = spin_lock_irqsave(&space->lock);
        for (vaddr_t p = va; p < va + chunk; p += PAGE_SIZE) {
            paddr_t pa;
            if (!arch_mmu_query(&space->mmu, p, &pa, NULL, NULL, NULL))
                continue;
            struct page *page = phys_to_page(pa);
            KASSERT(page != NULL);
            frames[n++] = page;
            frame_uncount(space, page);
        }
        int rc = arch_mmu_unmap(&space->mmu, va, chunk);
        KASSERT(rc == 0);
        spin_unlock_irqrestore(&space->lock, s);

        user_shootdown(space, va, chunk);

        /* The mapping's reference: the last one frees, and for a cache
         * frame the cache's own is never the mapping's to drop. */
        for (unsigned i = 0; i < n; i++)
            pmm_page_put(frames[i]);
    }
}

/*
 * A dying space's teardown (docs/audit/next-subsystem-exit-space.md).
 * vm_space_destroy has already invalidated the space's tag on every CPU
 * that held it, and no CPU runs the space, so none holds a translation
 * of it and none can make one: no chunk is shot down, and each chunk's
 * frames are freed as soon as it is unmapped. What was never populated
 * is stepped over an absent table at a time (arch_mmu_absent_span)
 * rather than queried a page at a time -- the 8 MB stack reservation
 * alone was 2,048 queries for a handful of frames, and with a shootdown
 * per 32-page chunk a teardown cost 5.7-18.1 ms. Returns the pages it
 * queried.
 */
static uint64_t dying_range_teardown(struct vm_space *space, vaddr_t base, size_t size)
{
    uint64_t queried = 0;
    vaddr_t end = base + size, va = base;
    while (va < end) {
        size_t absent = arch_mmu_absent_span(&space->mmu, va);
        if (absent > PAGE_SIZE) {
            va = absent >= (size_t)(end - va) ? end : va + absent;
            continue;
        }
        size_t chunk = MIN((size_t)(TEARDOWN_CHUNK_PAGES * PAGE_SIZE), (size_t)(end - va));
        struct page *frames[TEARDOWN_CHUNK_PAGES];
        unsigned n = 0;

        arch_irq_state_t s = spin_lock_irqsave(&space->lock);
        for (vaddr_t p = va; p < va + chunk; p += PAGE_SIZE) {
            paddr_t pa;
            queried++;
            if (!arch_mmu_query(&space->mmu, p, &pa, NULL, NULL, NULL))
                continue;
            struct page *page = phys_to_page(pa);
            KASSERT(page != NULL);
            frames[n++] = page;
            frame_uncount(space, page);
        }
        int rc = arch_mmu_unmap(&space->mmu, va, chunk);
        KASSERT(rc == 0);
        spin_unlock_irqrestore(&space->lock, s);

        for (unsigned i = 0; i < n; i++)
            pmm_page_put(frames[i]);
        va += chunk;
    }
    return queried;
}

/*
 * Free a user region's record, never under the space lock. A FILE region
 * drops its count on the mapping record; the one that takes it to zero
 * unlinks the record from the vnode (under the cache mutex, which is why
 * this cannot run under the spinlock) and drops the vnode reference.
 */
static void region_put(struct vm_region *r)
{
    struct vm_file_map *m = r->fmap;
    kmem_cache_free(g_region_cache, r);
    if (m == NULL)
        return;
    if (__atomic_fetch_sub(&m->regions, 1u, __ATOMIC_ACQ_REL) != 1)
        return;
    struct vnode *vn = m->vn;
    if (m->shared)
        __atomic_fetch_sub(&m->space->shared_maps, 1u, __ATOMIC_ACQ_REL);
    pagecache_lock(vn);
    list_remove(&m->link);
    pagecache_unlock(vn);
    vnode_put(vn);
    kfree(m);
}

/* Drop every region on a local list gathered under the lock. */
static void regions_put_all(struct list_node *gone)
{
    while (!list_empty(gone)) {
        struct vm_region *r = list_first_entry(gone, struct vm_region, link);
        list_remove(&r->link);
        region_put(r);
    }
}

uint64_t vm_space_destroy_counted(struct vm_space *space)
{
    KASSERT(space != NULL && space->user);
    /*
     * Nothing runs this space -- the last thread is gone. `tlb_cpus` may
     * name this CPU all the same, because a CPU that leaves a tagged
     * space keeps its translations; what must be true is that this CPU
     * is not running it *now*.
     */
    /* Raw: whichever CPU runs this thread has the thread's own space loaded, so this says "not my space" anywhere. */
    KASSERT(raw_this_cpu()->cur_space != space);

    /*
     * First, whatever any CPU still holds under this space's tag goes.
     * No CPU runs the space -- its threads have all switched out, and a
     * switch loads the incoming thread's space -- so after this no CPU
     * holds a translation of it and none can make one, and the teardown
     * needs no shootdown before it frees a frame.
     */
    arch_mmu_invalidate_asid(&space->mmu, space->tlb_cpus);

    uint64_t queried = 0;
    for (;;) {
        arch_irq_state_t s = spin_lock_irqsave(&space->lock);
        if (list_empty(&space->regions)) {
            spin_unlock_irqrestore(&space->lock, s);
            break;
        }
        struct vm_region *r = list_first_entry(&space->regions, struct vm_region, link);
        list_remove(&r->link);
        space->mapped_pages -= r->size / PAGE_SIZE;
        vaddr_t base = r->base;
        size_t size = r->size;
        spin_unlock_irqrestore(&space->lock, s);

        queried += dying_range_teardown(space, base, size);
        region_put(r);
    }
    /* Every frame this space populated has been handed back: the loop
     * above tore down every region, and a region's range is where its
     * frames are. A residue names a leak (frames the space still owns
     * with nothing left to free them) and an underflow names a double
     * free, so the number is printed rather than asserted away. The
     * file-page count is checked the same way: a cache frame installed
     * and never put would keep a page the file no longer has. */
    if (space->anon_pages != 0 || space->file_pages != 0 || space->shared_maps != 0)
        panic("vm_space_destroy: %llu anon and %llu file pages, %llu shared maps unaccounted (mapped_pages %llu)",
              (unsigned long long)space->anon_pages, (unsigned long long)space->file_pages,
              (unsigned long long)space->shared_maps, (unsigned long long)space->mapped_pages);

    /*
     * And again before the tag is released. The other order is the bug
     * the report was reviewed for: a tag released while a CPU still
     * holds its translations is a tag whose next owner inherits them.
     *
     * The invalidate at the start already dropped everything, and
     * nothing could make a translation since, so this one has nothing
     * left to do and no test can distinguish its presence; that is
     * recorded rather than counted (docs/kernel/memory/testing.md). It
     * is kept so that the tag's release never depends on the reasoning
     * about the start -- that no CPU could have run the space meanwhile.
     */
    arch_mmu_invalidate_asid(&space->mmu, space->tlb_cpus);
    asid_release(&space->mmu);
    space->tlb_cpus = 0;

    arch_mmu_context_destroy(&space->mmu);
    kmem_cache_free(g_space_cache, space);
    return queried;
}

/* --- fork (roadmap M3; design.md, "fork") --- */

/* What fork remembers of one of the parent's file-mapping records, and
 * the child's record built from it. Copied, not pointed at: the parent's
 * record can go away while fork is between its two phases. */
struct fork_fmap {
    uint64_t tag;
    struct vnode *vn;            /* referenced */
    vaddr_t base;
    size_t size;
    uint64_t off;
    bool shared, text;
    vm_prot_t maxprot;
    struct vm_file_map *child;   /* built in phase A; NULL until then */
};

/* Unlink and free a child record no child region ended up using. */
static void fmap_free_unused(struct vm_file_map *m)
{
    KASSERT(m->regions == 0);
    if (m->shared)
        __atomic_fetch_sub(&m->space->shared_maps, 1u, __ATOMIC_ACQ_REL);
    pagecache_lock(m->vn);
    list_remove(&m->link);
    pagecache_unlock(m->vn);
    vnode_put(m->vn);
    kfree(m);
}

/* Phase A's snapshot: the parent's distinct mapping records, under its
 * lock. Returns how many there are, which is more than `cap` when the
 * array must be bigger (nothing referenced then). */
static unsigned fork_snapshot(struct vm_space *src, struct fork_fmap *out, unsigned cap)
{
    unsigned n = 0;
    arch_irq_state_t s = spin_lock_irqsave(&src->lock);
    struct vm_region *r;
    list_for_each_entry(r, &src->regions, link) {
        if (r->kind != VM_REGION_FILE)
            continue;
        bool seen = false;
        for (unsigned i = 0; i < n && i < cap; i++)
            seen = seen || out[i].tag == r->fmap->tag;
        if (seen)
            continue;
        if (n < cap) {
            struct vm_file_map *m = r->fmap;
            out[n] = (struct fork_fmap){ .tag = m->tag, .vn = m->vn, .base = m->base, .size = m->size,
                                         .off = m->off, .shared = m->shared, .text = m->text,
                                         .maxprot = m->maxprot, .child = NULL };
        }
        n++;
    }
    if (n <= cap)
        for (unsigned i = 0; i < n; i++)
            vnode_get(out[i].vn);
    spin_unlock_irqrestore(&src->lock, s);
    return n;
}

/*
 * Copy one region's present pages into `dst` (both locks held). Private
 * pages -- anonymous, copy-on-write copies and cache frames of a private
 * file mapping -- are mapped read-only in both, the parent's PTE lowered
 * where it allowed write (*shot set); shared file pages keep the parent's
 * protection. Each child PTE holds its own reference on the frame.
 */
static int fork_copy_pages(struct vm_space *src, struct vm_space *dst, const struct vm_region *r, bool *shot)
{
    bool shared = r->kind == VM_REGION_FILE && r->fmap->shared;
    vaddr_t end = r->base + r->size;
    for (vaddr_t va = r->base; va < end;) {
        size_t absent = arch_mmu_absent_span(&src->mmu, va);
        if (absent > 0) {
            va = absent >= end - va ? end : va + absent;
            continue;
        }
        paddr_t pa;
        vm_prot_t prot;
        if (arch_mmu_query(&src->mmu, va, &pa, &prot, NULL, NULL)) {
            struct page *pg = phys_to_page(pa);
            KASSERT(pg != NULL);
            vm_prot_t cprot = prot;
            if (!shared && (prot & VM_PROT_WRITE)) {
                int prc = arch_mmu_protect(&src->mmu, va, PAGE_SIZE, prot & ~VM_PROT_WRITE);
                KASSERT(prc == 0);
                (void)prc;
                *shot = true;
                cprot = prot & ~VM_PROT_WRITE;
            }
            pmm_page_get(pg);
            int mrc = arch_mmu_map(&dst->mmu, va, pa, PAGE_SIZE, cprot, r->cache, ARCH_MMU_MAP_USER);
            if (mrc) {
                pmm_page_put(pg);   /* the parent still maps it: never the last reference */
                return mrc;
            }
            if (pg->flags & PG_PAGECACHE)
                dst->file_pages++;
            else
                dst->anon_pages++;
        }
        va += PAGE_SIZE;
    }
    return 0;
}

int vm_space_fork(struct vm_space *src, struct vm_space **out)
{
    KASSERT(src->user);
    might_sleep();
    struct vm_space *dst = NULL;
    struct fork_fmap *maps = NULL;
    unsigned nmaps = 0, cap = 8;
    int rc;

    /* No replacement may hold a claimed range while the regions are copied. */
    mutex_lock(&src->replace_lock);
retry:
    /* Phase A: the parent's mapping records, and a child record for
     * each, on its vnode's list before any child PTE can map one of its
     * pages -- the rule map_file keeps, so a truncate or a write-back
     * never misses a page the child holds. */
    for (;;) {
        maps = kmalloc(sizeof(*maps) * cap, KMEM_ZERO);
        if (maps == NULL) {
            rc = -ENOMEM;
            goto out_unlock;
        }
        nmaps = fork_snapshot(src, maps, cap);
        if (nmaps <= cap)
            break;
        kfree(maps);
        cap = nmaps + 8;
    }
    rc = vm_space_create_user(&dst);
    if (rc)
        goto out_maps;
    for (unsigned i = 0; i < nmaps; i++) {
        struct vm_file_map *m = kzalloc(sizeof(*m));
        if (m == NULL) {
            rc = -ENOMEM;
            goto out_maps;
        }
        vnode_get(maps[i].vn);
        m->vn = maps[i].vn;
        m->space = dst;
        m->base = maps[i].base;
        m->size = maps[i].size;
        m->off = maps[i].off;
        m->shared = maps[i].shared;
        m->text = maps[i].text;
        m->maxprot = maps[i].maxprot;
        m->regions = 0;
        m->tag = maps[i].tag;   /* the same attach: SysV shm finds the child's by the parent's tag */
        if (m->shared)
            __atomic_fetch_add(&dst->shared_maps, 1u, __ATOMIC_ACQ_REL);
        /* Text keeps the -ETXTBSY interlock exact, as in map_file. */
        if (m->text)
            mutex_lock(&m->vn->lock);
        pagecache_lock(m->vn);
        list_push_back(&m->vn->pc.mappings, &m->link);
        pagecache_unlock(m->vn);
        if (m->text)
            mutex_unlock(&m->vn->lock);
        maps[i].child = m;
    }

    /* Phase B: the regions and their pages, under both space locks (the
     * child's nested: a truncate may reach it through a record above). */
    bool again = false;
    struct { vaddr_t base; size_t size; } *shots = NULL;
    unsigned nshots = 0;
    {
        unsigned nregions = vm_user_region_count(src) + 8;
        shots = kmalloc(sizeof(*shots) * nregions, 0);
        if (shots == NULL) {
            rc = -ENOMEM;
            goto out_maps;
        }
        arch_irq_state_t s = spin_lock_irqsave(&src->lock);
        spin_lock_nested(&dst->lock, 1);
        dst->limit_mapped_pages = src->limit_mapped_pages;
        dst->limit_anon_pages = src->limit_anon_pages;
        struct vm_region *r;
        list_for_each_entry(r, &src->regions, link) {
            KASSERT(!(r->flags & VM_REGION_QUIESCED));   /* replace_lock is held */
            KASSERT(r->kind != VM_REGION_PHYS);          /* user spaces map no physical ranges */
            struct vm_file_map *cm = NULL;
            if (r->kind == VM_REGION_FILE) {
                for (unsigned i = 0; i < nmaps && cm == NULL; i++)
                    if (maps[i].tag == r->fmap->tag)
                        cm = maps[i].child;
                if (cm == NULL) {
                    again = true;   /* mapped since the snapshot: start over */
                    break;
                }
            }
            struct vm_region *c = region_new(r->base, r->size, r->prot, r->cache, r->kind, r->flags, 0, r->name);
            if (c == NULL || nshots >= nregions) {
                if (c)
                    kmem_cache_free(g_region_cache, c);
                rc = c ? -EAGAIN : -ENOMEM;
                again = rc == -EAGAIN;   /* regions were added since they were counted */
                break;
            }
            if (cm) {
                c->fmap = cm;
                __atomic_fetch_add(&cm->regions, 1u, __ATOMIC_ACQ_REL);
            }
            list_push_back(&dst->regions, &c->link);   /* in order: the parent's list is sorted */
            dst->mapped_pages += r->size / PAGE_SIZE;
            bool shot = false;
            rc = fork_copy_pages(src, dst, r, &shot);
            if (shot) {
                shots[nshots].base = r->base;
                shots[nshots].size = r->size;
                nshots++;
            }
            if (rc)
                break;
        }
        if (rc == 0 && !again &&
            (dst->mapped_pages > dst->limit_mapped_pages || dst->anon_pages > dst->limit_anon_pages))
            rc = -ENOMEM;   /* the child's limits, inherited, refuse what it would start with */
        spin_unlock(&dst->lock);
        spin_unlock_irqrestore(&src->lock, s);
    }
    /* The parent's lowered PTEs, wherever they went read-only -- also on
     * failure: they are lowered, and a stale writable translation on
     * another CPU must not outlive that. */
    for (unsigned i = 0; i < nshots; i++)
        user_shootdown(src, shots[i].base, shots[i].size);
    kfree(shots);
    if (again)
        rc = -EAGAIN;

out_maps:
    for (unsigned i = 0; i < nmaps; i++) {
        vnode_put(maps[i].vn);   /* the snapshot's reference */
        if (maps[i].child != NULL && maps[i].child->regions == 0)
            fmap_free_unused(maps[i].child);
    }
    kfree(maps);
    maps = NULL;
    if (rc != 0 && dst != NULL) {
        vm_space_destroy(dst);   /* releases every reference the copies took */
        dst = NULL;
    }
    if (rc == -EAGAIN)
        goto retry;   /* the snapshot is retaken from the start */
out_unlock:
    mutex_unlock(&src->replace_lock);
    if (rc == 0) {
        __atomic_fetch_add(&g_stats.forks, 1, __ATOMIC_RELAXED);
        *out = dst;
    }
    return rc;
}

void vm_space_destroy(struct vm_space *space)
{
    (void)vm_space_destroy_counted(space);
}

void vm_space_share(struct vm_space *space)
{
    KASSERT(space->user);
    uint32_t was = __atomic_fetch_add(&space->users, 1, __ATOMIC_RELAXED);
    KASSERT(was > 0);
}

bool vm_space_put(struct vm_space *space)
{
    /* Release: every access this user made happens before the teardown
     * the last one runs (acquire on that side). */
    uint32_t was = __atomic_fetch_sub(&space->users, 1, __ATOMIC_ACQ_REL);
    KASSERT(was > 0);
    if (was != 1)
        return false;
    vm_space_destroy(space);
    return true;
}

static bool user_range_valid(uint64_t base, size_t size)
{
    return is_page_aligned(base) && is_page_aligned(size) && size > 0 && base >= VM_USER_LO &&
           base + size > base && base + size <= VM_USER_HI;
}

/* Two user regions that could be one: adjacent, no guard between, same
 * attributes and name. */
static bool regions_mergeable(const struct vm_region *a, const struct vm_region *b)
{
    return a->base + a->size == b->base && a->kind == VM_REGION_ANON && b->kind == VM_REGION_ANON &&
           a->prot == b->prot && a->cache == b->cache && a->flags == b->flags && a->name == b->name &&
           !(a->flags & VM_REGION_GUARD_ABOVE) && !(b->flags & VM_REGION_GUARD_BELOW);
}

/* Merge r with its successor while possible; returns the number of region
 * structs absorbed, which the caller frees (never under the lock). */
static unsigned region_merge_forward(struct vm_space *space, struct vm_region *r, struct vm_region **freed,
                                     unsigned cap)
{
    unsigned n = 0;
    while (n < cap && r->link.next != &space->regions) {
        struct vm_region *next = list_entry(r->link.next, struct vm_region, link);
        if (!regions_mergeable(r, next))
            break;
        r->size += next->size;
        list_remove(&next->link);
        freed[n++] = next;
    }
    return n;
}

/* Merge around r: with its predecessor and its successor. */
static unsigned region_merge_around(struct vm_space *space, struct vm_region *r, struct vm_region **freed,
                                    unsigned cap)
{
    unsigned n = 0;
    if (r->link.prev != &space->regions) {
        struct vm_region *prev = list_entry(r->link.prev, struct vm_region, link);
        if (regions_mergeable(prev, r)) {
            prev->size += r->size;
            list_remove(&r->link);
            freed[n++] = r;
            r = prev;
        }
    }
    return n + region_merge_forward(space, r, freed + n, cap - n);
}

/* Split r at `at` (inside r, page aligned): r keeps [base, at), `spare`
 * becomes [at, end) and is linked after r. */
static void region_split(struct vm_region *r, struct vm_region *spare, vaddr_t at)
{
    KASSERT(at > r->base && at < r->base + r->size && is_page_aligned(at));
    list_init(&spare->link);
    spare->base = at;
    spare->size = r->base + r->size - at;
    spare->prot = r->prot;
    spare->cache = r->cache;
    spare->kind = r->kind;
    spare->flags = r->flags & ~VM_REGION_GUARD_BELOW;
    spare->phys = r->phys;
    spare->fmap = r->fmap;
    if (r->fmap != NULL)
        __atomic_fetch_add(&r->fmap->regions, 1u, __ATOMIC_ACQ_REL);   /* the piece points at the record too */
    spare->name = r->name;
    r->size = at - r->base;
    r->flags &= ~VM_REGION_GUARD_ABOVE;
    list_insert_after(&r->link, &spare->link);
}

/*
 * Lowest free range of `size` bytes at or above `from` inside
 * [USER_LO, USER_HI), keeping one unmapped page between user regions; 0
 * if none. Lock held: the answer is only worth anything while it is, which
 * is why the placing callers insert before they let go (M46).
 */
static uint64_t range_first_fit_locked(struct vm_space *space, uint64_t from, size_t size)
{
    if (from < VM_USER_LO)
        from = VM_USER_LO;
    /*
     * Every comparison below is against VM_USER_HI by subtraction, never
     * by adding to a cursor: a page-aligned hint near the top of the
     * address space made `cursor + size + PAGE_SIZE` wrap below the
     * window and a region outside it was chosen and inserted (found in
     * review). The placing callers used to be saved by the map's own
     * user_range_valid, which a chosen base no longer passes through.
     */
    if (from >= VM_USER_HI || size > VM_USER_HI - VM_USER_LO)
        return 0;
    from = page_align_up(from);
    vaddr_t cursor = (vaddr_t)from;
    struct vm_region *r;
    list_for_each_entry(r, &space->regions, link) {
        vaddr_t rlo, rhi;
        region_footprint(r, &rlo, &rhi);
        rhi += PAGE_SIZE; /* keep one unmapped page between user regions */
        if (rhi <= cursor)
            continue;
        if (rlo >= cursor && rlo - cursor >= size + PAGE_SIZE)
            return user_range_valid(cursor, size) ? cursor : 0;
        if (rhi > cursor)
            cursor = rhi;
    }
    if (cursor < VM_USER_HI && VM_USER_HI - cursor >= size + PAGE_SIZE)
        return cursor;
    return 0;
}

/*
 * The anonymous map, at a given base or -- with `placed` -- at a base it
 * chooses. Choosing happens inside the hold that inserts: first fit from
 * `from`, the region's base set, the insert made, before the lock is
 * released, so no other placement can be handed the range in between
 * (M46). The two used to be two holds (vm_user_find_free, then this), and
 * about half of all concurrent placements then lost with -EEXIST for a
 * request that named no address (docs/audit/next-subsystem-mmap-place.md,
 * "Measured").
 */
static int map_anon(struct vm_space *space, uint64_t base, uint64_t from, size_t size, vm_prot_t prot,
                    unsigned flags, const char *name, uint64_t *placed)
{
    KASSERT(space->user);
    if (placed) {
        if (!is_page_aligned(size) || size == 0)
            return -EINVAL;
    } else if (!user_range_valid(base, size)) {
        return -EINVAL;
    }
    if ((prot & VM_PROT_WRITE) && (prot & VM_PROT_EXEC))
        return -EINVAL;

    unsigned rflags = VM_REGION_USER | (flags & (VM_REGION_POPULATED | VM_REGION_GUARD_BELOW));
    struct vm_region *r = region_new((vaddr_t)base, size, prot & ~VM_PROT_USER, VM_CACHE_WB, VM_REGION_ANON,
                                     rflags, 0, name);
    if (r == NULL)
        return -ENOMEM;

    uint64_t npages = size / PAGE_SIZE;
    arch_irq_state_t s = spin_lock_irqsave(&space->lock);
    if (space->mapped_pages + npages > space->limit_mapped_pages) {   /* COSMO_RLIMIT_AS */
        spin_unlock_irqrestore(&space->lock, s);
        kmem_cache_free(g_region_cache, r);
        return -ENOMEM;
    }
    if (placed) {
        base = range_first_fit_locked(space, from, size);
        if (base == 0) {
            spin_unlock_irqrestore(&space->lock, s);
            kmem_cache_free(g_region_cache, r);
            return -ENOMEM;   /* no gap above `from`: the caller may try a lower one */
        }
        r->base = (vaddr_t)base;
        KASSERT(user_range_valid(base, size));   /* range_first_fit_locked answers inside the window or 0 */
    }
    int rc = space_insert(space, r);
    if (rc) {
        /* Never for a placement: the range was free under this hold. */
        KASSERT(!placed);
        spin_unlock_irqrestore(&space->lock, s);
        kmem_cache_free(g_region_cache, r);
        return rc;
    }
    if (placed)
        *placed = base;
    space->mapped_pages += npages;

    if ((flags & VM_REGION_POPULATED) && prot != VM_PROT_NONE) {
        for (vaddr_t va = (vaddr_t)base; va < base + size; va += PAGE_SIZE) {
            struct page *page = NULL;
            if (space->anon_pages < space->limit_anon_pages)   /* COSMO_RLIMIT_MEM */
                page = pmm_alloc_page(PMM_FLAGS_ZERO);
            rc = page ? arch_mmu_map(&space->mmu, va, page_to_phys(page), PAGE_SIZE, r->prot, VM_CACHE_WB,
                                     ARCH_MMU_MAP_USER)
                      : -ENOMEM;
            if (rc) {
                if (page)
                    pmm_free_page(page);
                /* Unwind: drop the region, then tear down what was populated. */
                list_remove(&r->link);
                space->mapped_pages -= npages;
                spin_unlock_irqrestore(&space->lock, s);
                user_range_teardown(space, (vaddr_t)base, size);
                kmem_cache_free(g_region_cache, r);
                return rc;
            }
            space->anon_pages++;
        }
    }

    struct vm_region *freed[2];
    unsigned nf = region_merge_around(space, r, freed, 2);
    spin_unlock_irqrestore(&space->lock, s);
    for (unsigned i = 0; i < nf; i++)
        kmem_cache_free(g_region_cache, freed[i]);
    return 0;
}

int vm_user_map_anon(struct vm_space *space, uint64_t base, size_t size, vm_prot_t prot, unsigned flags,
                     const char *name)
{
    return map_anon(space, base, 0, size, prot, flags, name, NULL);
}

int vm_user_map_anon_free(struct vm_space *space, uint64_t from, size_t size, vm_prot_t prot, unsigned flags,
                          const char *name, uint64_t *base)
{
    return map_anon(space, 0, from, size, prot, flags, name, base);
}

/* Every page of [base, base+size) lies in some region. Lock held. */
static bool range_fully_mapped(struct vm_space *space, vaddr_t base, size_t size)
{
    vaddr_t cursor = base, end = base + size;
    struct vm_region *r;
    list_for_each_entry(r, &space->regions, link) {
        if (r->base + r->size <= cursor)
            continue;
        if (r->base > cursor)
            return false;
        cursor = r->base + r->size;
        if (cursor >= end)
            return true;
    }
    return false;
}

/* How many splits [base, base+size) needs: one per range end that falls
 * strictly inside a region. Lock held. */
static unsigned splits_needed(struct vm_space *space, vaddr_t base, size_t size)
{
    unsigned n = 0;
    struct vm_region *r = space_find(space, base);
    if (r != NULL && r->base < base)
        n++;
    vaddr_t last = base + size - PAGE_SIZE;
    r = space_find(space, last);
    if (r != NULL && r->base + r->size > base + size)
        n++;
    return n;
}

/* Cut the regions so that [base, base+size) is covered by whole regions
 * only, using the spares (at most two consumed). Lock held. */
static unsigned split_at_ends(struct vm_space *space, vaddr_t base, size_t size, struct vm_region **spares)
{
    unsigned used = 0;
    struct vm_region *r = space_find(space, base);
    if (r != NULL && r->base < base)
        region_split(r, spares[used++], base);
    r = space_find(space, base + size - PAGE_SIZE);
    if (r != NULL && r->base + r->size > base + size)
        region_split(r, spares[used++], base + size);
    return used;
}

/* Defined with the replacement primitive below, which is what sets the flag. */
static bool range_quiesced(struct vm_space *space, vaddr_t base, size_t size);

int vm_user_unmap(struct vm_space *space, uint64_t base, size_t size, unsigned flags)
{
    KASSERT(space->user);
    if (!user_range_valid(base, size))
        return -EINVAL;

    struct vm_region *spares[2] = { region_new(0, 0, 0, VM_CACHE_WB, VM_REGION_ANON, 0, 0, NULL),
                                    region_new(0, 0, 0, VM_CACHE_WB, VM_REGION_ANON, 0, 0, NULL) };
    LIST_HEAD(gone);   /* the unlinked records, freed after the lock: a FILE one takes a mutex to go */
    unsigned nr = 0, used = 0;
    int rc = 0;

    arch_irq_state_t s = spin_lock_irqsave(&space->lock);
    /*
     * A region another replacement has claimed is not ours to unlink:
     * that replacement is between its teardown and its swap and expects
     * to find exactly what it left. Report it rather than corrupt it --
     * the caller is racing munmap against MAP_FIXED on one range, which
     * is a bug in the caller, but it must not become a bug here.
     */
    if (range_quiesced(space, (vaddr_t)base, size)) {
        rc = -EBUSY;
        goto out;
    }
    if ((flags & VM_UNMAP_STRICT) && !range_fully_mapped(space, (vaddr_t)base, size)) {
        rc = -EINVAL;
        goto out;
    }
    unsigned need = splits_needed(space, (vaddr_t)base, size);
    if ((need > 0 && spares[0] == NULL) || (need > 1 && spares[1] == NULL)) {
        rc = -ENOMEM;
        goto out;
    }
    used = split_at_ends(space, (vaddr_t)base, size, spares);
    KASSERT(used == need);

    /* Unlink every region now inside the range. */
    struct vm_region *r, *tmp;
    list_for_each_entry_safe(r, tmp, &space->regions, link) {
        if (r->base + r->size <= base)
            continue;
        if (r->base >= base + size)
            break;
        KASSERT(r->base >= base && r->base + r->size <= base + size);
        list_remove(&r->link);
        space->mapped_pages -= r->size / PAGE_SIZE;
        list_push_back(&gone, &r->link);   /* only the record: frames are found by the tables */
        nr++;
    }
out:
    spin_unlock_irqrestore(&space->lock, s);

    if (rc == 0 && nr > 0) {
        user_range_teardown(space, (vaddr_t)base, size);
        file_hold_release(space);
    }
    regions_put_all(&gone);
    for (unsigned i = used; i < 2; i++)
        if (spares[i])
            kmem_cache_free(g_region_cache, spares[i]);
    return rc;
}

/* No region other than `self` has a footprint touching [lo, hi). Lock held. */
static bool range_free_except(struct vm_space *space, vaddr_t lo, vaddr_t hi, const struct vm_region *self)
{
    struct vm_region *r;
    list_for_each_entry(r, &space->regions, link) {
        if (r == self)
            continue;
        vaddr_t rlo, rhi;
        region_footprint(r, &rlo, &rhi);
        if (lo < rhi && rlo < hi)
            return false;
    }
    return true;
}

/* The region at `base` must be a whole anonymous mapping of exactly
 * old_size, and not one a replacement has claimed. Lock held. 0 and *out
 * on success; -EFAULT (nothing there), -EINVAL (not a whole anonymous
 * region) or -EBUSY (claimed). */
static int remap_lookup(struct vm_space *space, uint64_t base, size_t old_size, struct vm_region **out)
{
    struct vm_region *r = space_find(space, (vaddr_t)base);
    if (r == NULL)
        return -EFAULT;
    if (r->kind != VM_REGION_ANON || r->base != base || r->size != old_size)
        return -EINVAL;
    if (r->flags & VM_REGION_QUIESCED)
        return -EBUSY;
    *out = r;
    return 0;
}

/*
 * In-place resize of a whole anonymous user mapping -- Linux mremap(2)
 * with no move. The region named by [base, base+old_size) must be a single
 * VM_REGION_ANON region; it is found and validated under the space lock,
 * and the region record ITSELF is extended or trimmed, so its flags (guard
 * pages, name) ride along and there is never a second region to fail a
 * later whole-region check. Demand-zero throughout -- the grown span
 * faults in zero pages like any anonymous mapping; user anonymous regions
 * are never VM_REGION_POPULATED (only the in-kernel allocator sets it), and
 * even a populated one is served by the anon fault path.
 *
 * A grow extends the region under one hold of the space lock, after
 * checking the space above is free -- no window for a concurrent map. A
 * shrink has to free the tail, and user_range_teardown takes the lock per
 * chunk, so it runs outside the hold; the tail is first split into its own
 * region and marked VM_REGION_QUIESCED under space->replace_lock, exactly
 * as a MAP_FIXED replacement claims its range (map_replace). While the
 * teardown runs the tail stays claimed, so faults there back off and a
 * concurrent mmap(NULL, ...) cannot be handed the range and have its page
 * torn down underneath it; the record is unlinked only once teardown is
 * done.
 *
 * On success returns 0 and the mapping stays at `base`. Otherwise:
 *   -EFAULT  nothing is mapped at `base`
 *   -EINVAL  not anonymous, not the whole region (base and old_size must
 *            name it exactly), or new_size names an invalid user range
 *   -EBUSY   a MAP_FIXED replacement owns the region right now
 *   -ENOMEM  a grow the space after cannot absorb, or over COSMO_RLIMIT_AS
 *
 * The caller passes page-aligned, non-zero sizes (new_size may equal
 * old_size: the region is still validated, then left unchanged).
 */
int vm_user_remap(struct vm_space *space, uint64_t base, size_t old_size, size_t new_size)
{
    KASSERT(space->user);
    KASSERT(is_page_aligned(base) && is_page_aligned(old_size) && is_page_aligned(new_size));
    KASSERT(old_size > 0 && new_size > 0);

    /* The new extent must sit inside the user window; this also rejects a
     * base + new_size that would wrap past VM_USER_HI. */
    if (!user_range_valid(base, new_size))
        return -EINVAL;

    struct vm_region *r;
    int rc;

    if (new_size < old_size) {
        /* Shrink: split off the tail, claim it, tear it down outside the
         * lock, then unlink it -- serialized with the other teardown path
         * (map_replace) so two tear-downs cannot interleave. */
        struct vm_region *tail = region_new(0, 0, 0, VM_CACHE_WB, VM_REGION_ANON, 0, 0, NULL);
        if (tail == NULL)
            return -ENOMEM;

        mutex_lock(&space->replace_lock);
        arch_irq_state_t s = spin_lock_irqsave(&space->lock);
        rc = remap_lookup(space, base, old_size, &r);
        if (rc != 0) {
            spin_unlock_irqrestore(&space->lock, s);
            mutex_unlock(&space->replace_lock);
            kmem_cache_free(g_region_cache, tail);
            return rc;
        }
        region_split(r, tail, (vaddr_t)(base + new_size));   /* r = head, tail = [base+new, base+old) */
        tail->flags |= VM_REGION_QUIESCED;
        space->mapped_pages -= (old_size - new_size) / PAGE_SIZE;   /* charge the final state now */
        spin_unlock_irqrestore(&space->lock, s);

        /* Faults on the tail find the claim and install nothing; nothing
         * else can map the range while it is linked and claimed. */
        user_range_teardown(space, (vaddr_t)(base + new_size), old_size - new_size);

        s = spin_lock_irqsave(&space->lock);
        list_remove(&tail->link);   /* the range is now genuinely free */
        spin_unlock_irqrestore(&space->lock, s);
        mutex_unlock(&space->replace_lock);
        kmem_cache_free(g_region_cache, tail);
        return 0;
    }

    /* Grow, or a no-op equal-size request: one hold, no teardown. */
    struct vm_region *freed[2];
    unsigned nf = 0;
    arch_irq_state_t s = spin_lock_irqsave(&space->lock);
    rc = remap_lookup(space, base, old_size, &r);
    if (rc != 0 || new_size == old_size)
        goto out;   /* equal size: validated, nothing to change */

    /* The grown footprint (guard pages included) must be clear. */
    vaddr_t lo, hi;
    region_footprint(r, &lo, &hi);
    hi += new_size - old_size;
    if (!range_free_except(space, lo, hi, r)) {
        rc = -ENOMEM;   /* the space after is not free */
        goto out;
    }
    uint64_t delta_pages = (new_size - old_size) / PAGE_SIZE;
    if (space->mapped_pages + delta_pages > space->limit_mapped_pages) {
        rc = -ENOMEM;   /* COSMO_RLIMIT_AS */
        goto out;
    }
    r->size = new_size;
    space->mapped_pages += delta_pages;
    nf = region_merge_forward(space, r, freed, 2);   /* absorb an adjacent twin, if any */
out:
    spin_unlock_irqrestore(&space->lock, s);
    for (unsigned i = 0; i < nf; i++)
        kmem_cache_free(g_region_cache, freed[i]);
    return rc;
}

/*
 * Pages of [base, base+size) that the regions covering it account for.
 * Lock held. Used to charge the replacement before anything is changed.
 */
static uint64_t covered_pages(struct vm_space *space, vaddr_t base, size_t size)
{
    uint64_t n = 0;
    vaddr_t end = base + size;
    struct vm_region *r;
    list_for_each_entry(r, &space->regions, link) {
        if (r->base + r->size <= base)
            continue;
        if (r->base >= end)
            break;
        /*
         * The INTERSECTION, not the region. This runs before the split,
         * so a region may reach past either end of the range and only
         * the part inside it is being replaced. Counting the whole
         * region credited six pages for a two-page replacement in the
         * middle of a six-page mapping, and mapped_pages went backwards.
         */
        vaddr_t lo = r->base > base ? r->base : base;
        vaddr_t hi = (r->base + r->size) < end ? (r->base + r->size) : end;
        n += (hi - lo) / PAGE_SIZE;
    }
    return n;
}

/* Any region of [base, base+size) already claimed by a replacement.
 * Lock held. */
static bool range_quiesced(struct vm_space *space, vaddr_t base, size_t size)
{
    struct vm_region *r;
    list_for_each_entry(r, &space->regions, link) {
        if (r->base + r->size <= base)
            continue;
        if (r->base >= base + size)
            break;
        if (r->flags & VM_REGION_QUIESCED)
            return true;
    }
    return false;
}

/*
 * The replacement itself, for a prepared region `fresh` (anonymous or a
 * file mapping) covering [base, base+size). Takes ownership of `fresh`
 * on success and on failure alike: the caller never frees it. The
 * contract is vm_user_map_anon_replace's in vmm.h.
 */
static int map_replace(struct vm_space *space, struct vm_region *fresh)
{
    vaddr_t base = fresh->base;
    size_t size = fresh->size;
    struct vm_region *spares[2] = { region_new(0, 0, 0, VM_CACHE_WB, VM_REGION_ANON, 0, 0, NULL),
                                    region_new(0, 0, 0, VM_CACHE_WB, VM_REGION_ANON, 0, 0, NULL) };
    LIST_HEAD(gone);
    unsigned used = 0;
    uint64_t npages = size / PAGE_SIZE;
    int rc = 0;

    /* One replacement at a time: the teardown below cannot hold the
     * spinlock, so two of these would otherwise interleave. */
    mutex_lock(&space->replace_lock);
    if (spares[0] == NULL || spares[1] == NULL) {
        rc = -ENOMEM;
        goto out_free;
    }

    /* --- first critical section: every check, then claim the range --- */
    arch_irq_state_t s = spin_lock_irqsave(&space->lock);
    if (range_quiesced(space, (vaddr_t)base, size)) {
        /* Only reachable if some other path learns to quiesce; the
         * replace_lock already excludes the one writer there is. */
        spin_unlock_irqrestore(&space->lock, s);
        rc = -EBUSY;
        goto out_free;
    }
    uint64_t covered = covered_pages(space, (vaddr_t)base, size);
    if (space->mapped_pages - covered + npages > space->limit_mapped_pages) {   /* COSMO_RLIMIT_AS */
        spin_unlock_irqrestore(&space->lock, s);
        rc = -ENOMEM;
        goto out_free;
    }
    unsigned need = splits_needed(space, (vaddr_t)base, size);
    KASSERT(need <= 2);
    used = split_at_ends(space, (vaddr_t)base, size, spares);
    KASSERT(used == need);

    /*
     * Charge the final state now, so the swap needs no check. Between
     * here and it, mapped_pages describes where this is going rather
     * than where it is.
     */
    space->mapped_pages = space->mapped_pages - covered + npages;

    /*
     * Clear the range and put the NEW region in, claimed, before the
     * lock is released. One region then owns the whole interval --
     * including any HOLE it spanned, which is the part that matters:
     * an unclaimed hole is one `vm_user_find_free` will hand to a
     * concurrent `mmap(NULL, ...)`, and the swap would then collide
     * with a perfectly valid mapping and panic on its KASSERT. An
     * earlier version left the old regions linked instead and had
     * exactly that hole; review found it.
     *
     * The insert cannot collide: the range was cleared a few lines
     * above under this same hold of the lock.
     */
    struct vm_region *r, *tmp;
    list_for_each_entry_safe(r, tmp, &space->regions, link) {
        if (r->base + r->size <= base)
            continue;
        if (r->base >= base + size)
            break;
        KASSERT(r->base >= base && r->base + r->size <= base + size);
        list_remove(&r->link);
        list_push_back(&gone, &r->link);
    }
    fresh->flags |= VM_REGION_QUIESCED;
    int irc = space_insert(space, fresh);
    KASSERT(irc == 0);   /* just cleared, and nothing else holds this lock */
    (void)irc;
    spin_unlock_irqrestore(&space->lock, s);

    /*
     * The teardown works on the page tables, not the region list, so
     * the old pages go even though their records are already gone.
     * Faults on the range meanwhile find `fresh` and its claim, and
     * install nothing.
     */
    user_range_teardown(space, (vaddr_t)base, size);

    /*
     * Release the claim across the WHOLE RANGE, not through `fresh`.
     * `region_split` copies flags, so anything that split the claimed
     * region while the teardown ran left pieces carrying the claim,
     * and clearing one pointer would strand the others: faults there
     * would retry for ever, copies would take -EFAULT, and later
     * unmaps and replacements would see -EBUSY. Nothing splits it
     * today -- `vm_user_protect` refuses a claimed range, just below
     * -- and this loop is what keeps that from being load-bearing.
     */
    s = spin_lock_irqsave(&space->lock);
    struct vm_region *qr;
    list_for_each_entry(qr, &space->regions, link) {
        if (qr->base + qr->size <= base)
            continue;
        if (qr->base >= base + size)
            break;
        qr->flags &= ~VM_REGION_QUIESCED;
    }
    struct vm_region *merged[2];
    unsigned nm = region_merge_around(space, fresh, merged, 2);
    spin_unlock_irqrestore(&space->lock, s);

    mutex_unlock(&space->replace_lock);
    file_hold_release(space);
    regions_put_all(&gone);
    for (unsigned i = 0; i < nm; i++)
        region_put(merged[i]);
    for (unsigned i = used; i < 2; i++)
        if (spares[i])
            kmem_cache_free(g_region_cache, spares[i]);
    return 0;

out_free:
    mutex_unlock(&space->replace_lock);
    region_put(fresh);
    for (unsigned i = 0; i < 2; i++)
        if (spares[i])
            kmem_cache_free(g_region_cache, spares[i]);
    return rc;
}

int vm_user_map_anon_replace(struct vm_space *space, uint64_t base, size_t size, vm_prot_t prot,
                             unsigned flags, const char *name)
{
    KASSERT(space->user);
    if (!user_range_valid(base, size))
        return -EINVAL;
    if ((prot & VM_PROT_WRITE) && (prot & VM_PROT_EXEC))
        return -EINVAL;
    /*
     * Populating allocates frames and can fail; the swap must not be
     * able to. See the contract in vmm.h.
     */
    if (flags & VM_REGION_POPULATED)
        return -EINVAL;

    /*
     * Allocate before any lock, as unmap and map both do: a failure here
     * is a failure that has changed nothing.
     */
    unsigned rflags = VM_REGION_USER | (flags & VM_REGION_GUARD_BELOW);
    struct vm_region *fresh = region_new((vaddr_t)base, size, prot & ~VM_PROT_USER, VM_CACHE_WB,
                                         VM_REGION_ANON, rflags, 0, name);
    if (fresh == NULL)
        return -ENOMEM;
    return map_replace(space, fresh);
}

/*
 * The file map, at a given base or -- with `placed` -- at one it chooses.
 * A chosen base has to exist before the record goes on the vnode's list,
 * because a truncate or an instruction-cache sync reads `m->base` off that
 * list. So a placement is made first, under one hold of the space lock:
 * first fit, the region inserted CLAIMED (VM_REGION_QUIESCED -- a fault
 * there installs nothing and retries, which is what map_replace does for
 * the same reason), the pages counted. Then the record is linked exactly
 * as for a given base, and the claim is dropped; a refusal takes the
 * region out again. Every reader of the list rechecks that the space's
 * region points at this record, so one that meets it early sees a
 * claimed region and skips it (M46).
 */
static int map_file(struct vm_space *space, uint64_t base, uint64_t from, size_t size, vm_prot_t prot,
                    vm_prot_t maxprot, unsigned flags, struct vnode *vn, uint64_t off, const char *name,
                    uint64_t *placed, uint64_t *out_tag)
{
    KASSERT(space->user);
    if (placed) {
        if (!is_page_aligned(size) || size == 0 || (flags & VM_MAP_REPLACE))
            return -EINVAL;
    } else if (!user_range_valid(base, size)) {
        return -EINVAL;
    }
    if (!is_page_aligned(off) || off + size < off)
        return -EINVAL;
    prot &= ~VM_PROT_USER;
    maxprot &= ~VM_PROT_USER;
    if ((prot & VM_PROT_WRITE) && (prot & VM_PROT_EXEC))
        return -EINVAL;
    if (prot & ~maxprot)
        return -EINVAL;

    struct vm_file_map *m = kzalloc(sizeof(*m));
    struct vm_region *r = region_new((vaddr_t)base, size, prot, VM_CACHE_WB, VM_REGION_FILE, VM_REGION_USER, 0,
                                     name);
    if (m == NULL || r == NULL) {
        if (m)
            kfree(m);
        if (r)
            kmem_cache_free(g_region_cache, r);
        return -ENOMEM;
    }
    uint64_t npages = size / PAGE_SIZE;
    vnode_get(vn);
    m->vn = vn;
    m->space = space;
    m->base = (vaddr_t)base;
    m->size = size;
    m->off = off;
    m->shared = (flags & VM_MAP_SHARED) != 0;
    /*
     * Text, for the -ETXTBSY interlock, and only when the loader says
     * so. Two narrower rules were tried first and both were wrong:
     *
     *  - `maxprot & EXEC` -- maxprot is permissive by default (RWX for a
     *    shared mapping of a writable file), so *every* shared file
     *    mapping counted as text and ordinary writes were refused;
     *  - `prot & EXEC` -- a program may map a file executable and write
     *    to it deliberately, and the page cache syncs the instruction
     *    cache for exactly that case. Refusing those writes broke that
     *    behaviour and the test that proves it.
     *
     * What must not change underneath a process is the program it is
     * *running*, which only the loader can identify. Set here, where the
     * record is built and before it is linked, so the answer and the
     * mapping have one lifetime.
     */
    m->text = (flags & VM_MAP_TEXT) != 0;
    m->maxprot = maxprot;
    m->regions = 1;
    m->tag = __atomic_fetch_add(&g_map_tag, 1, __ATOMIC_RELAXED);   /* this attach's identity */
    if (out_tag)
        *out_tag = m->tag;
    if (m->shared)
        __atomic_fetch_add(&space->shared_maps, 1u, __ATOMIC_ACQ_REL);   /* the futex classifies only in a space that shares */
    r->fmap = m;

    /*
     * A chosen base: the placement, now that the region is whole. It is
     * published only once `r->fmap` is set and the record filled,
     * because msync and the futex-key lookup dereference `r->fmap` of
     * any FILE region they meet and do not look at the claim -- an
     * earlier version inserted first and set `fmap` after, and review
     * found the window. Claimed until the record is on the vnode's list,
     * for the readers of that list (see map_file's header).
     */
    if (placed) {
        arch_irq_state_t ps = spin_lock_irqsave(&space->lock);
        int prc = 0;
        if (space->mapped_pages + npages > space->limit_mapped_pages)   /* COSMO_RLIMIT_AS */
            prc = -ENOMEM;
        else if ((base = range_first_fit_locked(space, from, size)) == 0)
            prc = -ENOMEM;   /* no gap above `from`: the caller may try a lower one */
        if (prc == 0) {
            r->base = (vaddr_t)base;
            m->base = (vaddr_t)base;
            r->flags |= VM_REGION_QUIESCED;
            int irc = space_insert(space, r);
            KASSERT(irc == 0);   /* the range was free under this hold */
            (void)irc;
            space->mapped_pages += npages;
        }
        spin_unlock_irqrestore(&space->lock, ps);
        if (prc) {
            r->fmap = NULL;
            if (m->shared)
                __atomic_fetch_sub(&space->shared_maps, 1u, __ATOMIC_ACQ_REL);
            vnode_put(vn);
            kfree(m);
            kmem_cache_free(g_region_cache, r);
            return prc;
        }
    }

    /*
     * On the vnode's list before the region can take a fault, so a
     * truncate or a write-back never misses a page this mapping holds.
     *
     * **Text takes `vn->lock` to do it**, and that is what makes the
     * `-ETXTBSY` interlock exact rather than nearly exact. A writer
     * holds `vn->lock` across its busy check *and* its write
     * (`file_pwrite`), so without this a text mapping could be linked
     * between the two and the write would land in a file a process had
     * just begun executing. Taking it here means the loader waits for
     * any write in progress, and a write that starts later sees the
     * mapping. The order is the documented one, vnode -> pagecache, and
     * only the loader passes VM_MAP_TEXT -- it holds no vnode lock
     * (found in review of this unit).
     */
    bool text_lock = (flags & VM_MAP_TEXT) != 0;
    if (text_lock)
        mutex_lock(&vn->lock);
    pagecache_lock(vn);
    /*
     * Text and a writable shared mapping of the same file cannot
     * coexist, in either order.
     *
     * `file_pwrite` is not the only way to change a file: a store
     * through a writable `MAP_SHARED` mapping dirties the page cache's
     * own frame, and the text mapping *is* that frame -- so one process
     * could rewrite another's executing instructions without a write
     * ever reaching the VFS. Refusing the pair is what makes the
     * interlock about the file rather than about one syscall
     * (found in review of this unit).
     */
    bool clash = false;
    struct vm_file_map *other;
    list_for_each_entry(other, &vn->pc.mappings, link) {
        if (text_lock ? (other->shared && (other->maxprot & VM_PROT_WRITE))
                      : (other->text && m->shared && (maxprot & VM_PROT_WRITE)))
            clash = true;
    }
    if (!clash)
        list_push_back(&vn->pc.mappings, &m->link);
    pagecache_unlock(vn);
    if (text_lock)
        mutex_unlock(&vn->lock);
    if (clash && placed) {
        /* The claimed region comes out of the space again: nothing can
         * have faulted into it, a claim installs nothing. */
        arch_irq_state_t ps = spin_lock_irqsave(&space->lock);
        list_remove(&r->link);
        space->mapped_pages -= npages;
        spin_unlock_irqrestore(&space->lock, ps);
    }
    if (clash) {
        /* Undo what was counted before the record was built: the
         * refusal happens after `shared_maps` was incremented, and a
         * count left behind outlives the mapping and panics
         * `vm_space_destroy`, which checks it. */
        r->fmap = NULL;
        if (m->shared)
            __atomic_fetch_sub(&space->shared_maps, 1u, __ATOMIC_ACQ_REL);
        vnode_put(vn);
        kfree(m);
        kmem_cache_free(g_region_cache, r);
        return -ETXTBSY;
    }

    if (flags & VM_MAP_REPLACE)
        return map_replace(space, r);   /* owns `r` either way */

    if (placed) {
        /* Linked: drop the claim, and faults may install from here. */
        arch_irq_state_t ps = spin_lock_irqsave(&space->lock);
        r->flags &= ~VM_REGION_QUIESCED;
        spin_unlock_irqrestore(&space->lock, ps);
        *placed = base;
        return 0;
    }

    arch_irq_state_t s = spin_lock_irqsave(&space->lock);
    int rc = 0;
    if (space->mapped_pages + npages > space->limit_mapped_pages)   /* COSMO_RLIMIT_AS */
        rc = -ENOMEM;
    else
        rc = space_insert(space, r);
    if (rc == 0)
        space->mapped_pages += npages;
    spin_unlock_irqrestore(&space->lock, s);
    if (rc)
        region_put(r);   /* unlinks the record and drops the vnode */
    return rc;
}

int vm_user_map_file(struct vm_space *space, uint64_t base, size_t size, vm_prot_t prot, vm_prot_t maxprot,
                     unsigned flags, struct vnode *vn, uint64_t off, const char *name)
{
    return map_file(space, base, 0, size, prot, maxprot, flags, vn, off, name, NULL, NULL);
}

int vm_user_map_file_free(struct vm_space *space, uint64_t from, size_t size, vm_prot_t prot, vm_prot_t maxprot,
                          unsigned flags, struct vnode *vn, uint64_t off, const char *name, uint64_t *base)
{
    return map_file(space, 0, from, size, prot, maxprot, flags, vn, off, name, base, NULL);
}

/* shmat's map: like the two above but also hands back the mapping's unique tag,
 * which identifies this attach's region(s) for shmdt even after the program
 * munmaps parts of it or re-attaches the same segment elsewhere. `base` non-
 * NULL means place in a free gap (its value is the hint and receives the
 * result); otherwise map at `at`. */
int vm_user_map_shm(struct vm_space *space, uint64_t at, uint64_t *base, size_t size, vm_prot_t prot,
                    vm_prot_t maxprot, struct vnode *vn, const char *name, uint64_t *tag)
{
    if (base)
        return map_file(space, 0, *base, size, prot, maxprot, VM_MAP_SHARED, vn, 0, name, base, tag);
    return map_file(space, at, 0, size, prot, maxprot, VM_MAP_SHARED, vn, 0, name, NULL, tag);
}

int vm_user_msync(struct vm_space *space, uint64_t base, size_t size)
{
    KASSERT(space->user);
    if (!user_range_valid(base, size))
        return -EINVAL;

    /* First pass: nothing is written if any page is unmapped. */
    arch_irq_state_t s = spin_lock_irqsave(&space->lock);
    bool whole = range_fully_mapped(space, (vaddr_t)base, size);
    spin_unlock_irqrestore(&space->lock, s);
    if (!whole)
        return -ENOMEM;

    /* The cursor walk: the region containing the cursor, or the first
     * FILE region after it inside the range; its vnode referenced under
     * the lock, synced under the vnode lock with the space lock gone. */
    int rc = 0;
    vaddr_t cursor = (vaddr_t)base, end = (vaddr_t)base + size;
    while (cursor < end) {
        struct vnode *vn = NULL;
        vaddr_t next = end;
        s = spin_lock_irqsave(&space->lock);
        struct vm_region *r;
        list_for_each_entry(r, &space->regions, link) {
            if (r->base + r->size <= cursor)
                continue;
            if (r->base >= end)
                break;
            next = r->base + r->size;
            if (r->kind == VM_REGION_FILE) {
                vn = r->fmap->vn;
                vnode_get(vn);
                break;
            }
        }
        spin_unlock_irqrestore(&space->lock, s);
        cursor = next;
        if (vn == NULL)
            continue;
        mutex_lock(&vn->lock);
        int src = pagecache_sync(vn);
        mutex_unlock(&vn->lock);
        vnode_put(vn);
        if (src && rc == 0)
            rc = src;
    }
    return rc;
}

/*
 * The teardown of one record's pages from file index `keep` on, under the
 * cache mutex (pagecache_truncate). Regions are not unlinked -- the
 * mapping stays, and a touch past the end is SIGBUS from now on -- so
 * this cannot reuse the "no region can repopulate" argument of
 * user_range_teardown; what keeps a fault out is the mutex the caller
 * holds, which every install needs. Copy-on-write frames in the range go
 * too: the file no longer has those bytes.
 */
void vm_file_map_truncate(struct vm_file_map *m, uint64_t keep)
{
    uint64_t keep_off = keep * PAGE_SIZE;
    if (keep_off >= m->off + m->size)
        return;
    vaddr_t lo = keep_off > m->off ? m->base + (keep_off - m->off) : m->base;
    vaddr_t hi = m->base + m->size;
    struct vm_space *space = m->space;

    for (vaddr_t va = lo; va < hi; va += TEARDOWN_CHUNK_PAGES * PAGE_SIZE) {
        size_t chunk = MIN((size_t)(TEARDOWN_CHUNK_PAGES * PAGE_SIZE), (size_t)(hi - va));
        struct page *frames[TEARDOWN_CHUNK_PAGES];
        unsigned n = 0;

        arch_irq_state_t s = spin_lock_irqsave(&space->lock);
        for (vaddr_t p = va; p < va + chunk; p += PAGE_SIZE) {
            struct vm_region *r = space_find(space, p);
            if (r == NULL || r->fmap != m)
                continue;   /* a piece since unmapped or remapped to something else */
            paddr_t pa;
            if (!arch_mmu_query(&space->mmu, p, &pa, NULL, NULL, NULL))
                continue;
            struct page *page = phys_to_page(pa);
            frames[n++] = page;
            frame_uncount(space, page);
            int rc = arch_mmu_unmap(&space->mmu, p, PAGE_SIZE);
            KASSERT(rc == 0);
            (void)rc;
        }
        spin_unlock_irqrestore(&space->lock, s);

        if (n > 0)
            user_shootdown(space, va, chunk);
        for (unsigned i = 0; i < n; i++)
            pmm_page_put(frames[i]);
    }
}

int vm_user_futex_key(struct vm_space *space, uint64_t uaddr, bool private, struct futex_key *out)
{
    out->obj = space;
    out->off = uaddr;
    out->held = NULL;
    /* The program's promise (Linux's flag), or a space that has nothing
     * to share: the private key, and no walk. */
    if (private || __atomic_load_n(&space->shared_maps, __ATOMIC_ACQUIRE) == 0)
        return 0;
    arch_irq_state_t s = spin_lock_irqsave(&space->lock);
    struct vm_region *r = space_find(space, (vaddr_t)uaddr);
    if (r != NULL && r->kind == VM_REGION_FILE && r->fmap->shared) {
        struct vm_file_map *m = r->fmap;
        out->obj = m->vn;
        out->off = m->off + (uaddr - m->base);
        out->held = m->vn;
        vnode_get(m->vn);   /* an atomic increment: fine under the spinlock */
        __atomic_fetch_add(&g_stats.futex_shared_keys, 1, __ATOMIC_RELAXED);
    }
    spin_unlock_irqrestore(&space->lock, s);
    return 0;
}

bool vm_file_map_exec_at(struct vm_file_map *m, uint64_t index)
{
    uint64_t off = index * PAGE_SIZE;
    if (off < m->off || off >= m->off + m->size)
        return false;
    vaddr_t va = m->base + (off - m->off);
    struct vm_space *space = m->space;
    arch_irq_state_t s = spin_lock_irqsave(&space->lock);
    struct vm_region *r = space_find(space, va);
    bool exec = r != NULL && r->fmap == m && (r->prot & VM_PROT_EXEC);
    spin_unlock_irqrestore(&space->lock, s);
    return exec;
}

void vm_file_map_writeprotect(struct vm_file_map *m, uint64_t index, unsigned n)
{
    if (!m->shared)
        return;   /* a private record never maps a cache frame writable */
    uint64_t lo_off = index * PAGE_SIZE, hi_off = lo_off + (uint64_t)n * PAGE_SIZE;
    if (hi_off <= m->off || lo_off >= m->off + m->size)
        return;
    vaddr_t lo = lo_off > m->off ? m->base + (lo_off - m->off) : m->base;
    vaddr_t hi = hi_off < m->off + m->size ? m->base + (hi_off - m->off) : m->base + m->size;
    struct vm_space *space = m->space;
    bool changed = false;

    arch_irq_state_t s = spin_lock_irqsave(&space->lock);
    for (vaddr_t p = lo; p < hi; p += PAGE_SIZE) {
        struct vm_region *r = space_find(space, p);
        if (r == NULL || r->fmap != m)
            continue;
        vm_prot_t cur;
        if (!arch_mmu_query(&space->mmu, p, NULL, &cur, NULL, NULL) || !(cur & VM_PROT_WRITE))
            continue;
        int rc = arch_mmu_protect(&space->mmu, p, PAGE_SIZE, r->prot & ~VM_PROT_WRITE);
        KASSERT(rc == 0);
        (void)rc;
        changed = true;
    }
    spin_unlock_irqrestore(&space->lock, s);
    if (changed)
        user_shootdown(space, lo, hi - lo);   /* before the page is written: a stale writable entry would be a lost write */
}

int vm_user_protect(struct vm_space *space, uint64_t base, size_t size, vm_prot_t prot)
{
    KASSERT(space->user);
    if (!user_range_valid(base, size))
        return -EINVAL;
    if ((prot & VM_PROT_WRITE) && (prot & VM_PROT_EXEC))
        return -EINVAL;
    prot &= ~VM_PROT_USER;

    struct vm_region *spares[2] = { region_new(0, 0, 0, VM_CACHE_WB, VM_REGION_ANON, 0, 0, NULL),
                                    region_new(0, 0, 0, VM_CACHE_WB, VM_REGION_ANON, 0, 0, NULL) };
    struct vm_region *freed[4];
    unsigned nf = 0, used = 0;
    int rc = 0;

    arch_irq_state_t s = spin_lock_irqsave(&space->lock);
    /*
     * The same ownership rule `vm_user_unmap` keeps: a region a
     * replacement has claimed is not ours to touch. Splitting one
     * would be worse than unlinking it -- `region_split` copies
     * flags, so the pieces carry the claim, and the replacement
     * releases the range it knows about while any piece pushed
     * outside it stays claimed for ever: faults retrying without
     * end, copies taking -EFAULT, later unmaps refused. Review found
     * that; the rule was stated for unmap and applied only there.
     */
    if (range_quiesced(space, (vaddr_t)base, size)) {
        rc = -EBUSY;
        goto out;
    }
    if (!range_fully_mapped(space, (vaddr_t)base, size)) {
        rc = -ENOMEM;
        goto out;
    }
    /* A file mapping's ceiling: a shared mapping of a file opened
     * read-only cannot be made writable by a later call (POSIX: EACCES).
     * Checked over the whole range before anything changes. */
    struct vm_region *first = NULL, *r;
    list_for_each_entry(r, &space->regions, link) {
        if (r->base + r->size <= base)
            continue;
        if (r->base >= base + size)
            break;
        if (r->fmap != NULL && (prot & ~r->fmap->maxprot)) {
            rc = -EACCES;
            goto out;
        }
    }
    unsigned need = splits_needed(space, (vaddr_t)base, size);
    if ((need > 0 && spares[0] == NULL) || (need > 1 && spares[1] == NULL)) {
        rc = -ENOMEM;
        goto out;
    }
    used = split_at_ends(space, (vaddr_t)base, size, spares);
    KASSERT(used == need);

    list_for_each_entry(r, &space->regions, link) {
        if (r->base + r->size <= base)
            continue;
        if (r->base >= base + size)
            break;
        if (first == NULL)
            first = r;
        r->prot = prot;
        if (r->fmap == NULL && !(prot & VM_PROT_WRITE)) {
            rc = arch_mmu_protect(&space->mmu, r->base, r->size, prot);
            KASSERT(rc == 0);   /* whole 4 KiB user pages only: nothing to split */
            continue;
        }
        if (r->fmap == NULL) {
            /* Granting write: a frame fork left shared stays read-only,
             * and its first write copies it (cow_write_locked). Per
             * page, stepping over what was never populated. */
            for (vaddr_t p = r->base; p < r->base + r->size;) {
                size_t absent = arch_mmu_absent_span(&space->mmu, p);
                if (absent > 0) {
                    p = absent >= r->base + r->size - p ? r->base + r->size : p + absent;
                    continue;
                }
                paddr_t pa;
                if (arch_mmu_query(&space->mmu, p, &pa, NULL, NULL, NULL)) {
                    rc = arch_mmu_protect(&space->mmu, p, PAGE_SIZE,
                                          frame_shared_anon(phys_to_page(pa)) ? (prot & ~VM_PROT_WRITE) : prot);
                    KASSERT(rc == 0);
                }
                p += PAGE_SIZE;
            }
            continue;
        }
        /*
         * A cache frame's PTE gains write only in the fault handler --
         * which marks a shared page dirty as it raises it, and REPLACES
         * a private mapping's frame with a copy rather than raising it.
         * So here a cache frame gets everything but write whatever the
         * region is (a private region's read-installed frames too: raising
         * one would write the file through a private mapping), and only
         * a copy-on-write copy, the mapping's own anonymous frame, takes
         * the protection as asked. Per page, because a private region
         * holds both kinds.
         */
        for (vaddr_t p = r->base; p < r->base + r->size; p += PAGE_SIZE) {
            paddr_t pa;
            if (!arch_mmu_query(&space->mmu, p, &pa, NULL, NULL, NULL))
                continue;
            struct page *pg = phys_to_page(pa);
            bool keep_ro = (pg->flags & PG_PAGECACHE) != 0 || frame_shared_anon(pg);   /* a cache frame, or a copy fork shares */
            rc = arch_mmu_protect(&space->mmu, p, PAGE_SIZE, keep_ro ? (prot & ~VM_PROT_WRITE) : prot);
            KASSERT(rc == 0);
        }
    }

    /* Merge inside the range and with both neighbours. */
    if (first != NULL) {
        struct vm_region *prev = first->link.prev != &space->regions
                                     ? list_entry(first->link.prev, struct vm_region, link) : NULL;
        struct vm_region *anchor = (prev != NULL && regions_mergeable(prev, first)) ? prev : first;
        nf = region_merge_forward(space, anchor, freed, 4);
    }
out:
    spin_unlock_irqrestore(&space->lock, s);

    if (rc == 0)
        user_shootdown(space, (vaddr_t)base, size);
    for (unsigned i = 0; i < nf; i++)
        region_put(freed[i]);
    for (unsigned i = used; i < 2; i++)
        if (spares[i])
            kmem_cache_free(g_region_cache, spares[i]);
    return rc;
}

void vm_user_sync_icache(struct vm_space *space, uint64_t base, size_t size)
{
    KASSERT(space->user);
    /*
     * Present pages only, and a BOUNDED hold of the lock. Review found
     * the first version twice: syncing the whole range unconditionally
     * -- a demand-zero page never touched has no leaf translation, and
     * cache maintenance by such a VA is a translation fault at EL1 with
     * no fixup, so mmap(RW); mprotect(RX) was an unprivileged panic --
     * and then walking every page of the range in one critical section
     * with interrupts off, which for a lazy mapping at the 2 GiB limit
     * is 524,288 queries nothing can interrupt. QEMU showed neither: it
     * implements the maintenance as a no-op.
     *
     * So this walks in the teardown's chunks, taking the lock per chunk
     * as user_range_teardown does. Under the lock, because that same
     * teardown clears leaves under it per chunk: a page seen present
     * here stays present until its maintenance is done. Consecutive
     * present pages are synced as one run, so the barriers are paid per
     * run rather than per page. A whole-table skip for absent ranges
     * would be faster still and needs an arch walker this tree does not
     * have; the bound on the critical section is what removes the
     * hazard, and that is what this does.
     */
    for (vaddr_t va = (vaddr_t)base; va < base + size; va += TEARDOWN_CHUNK_PAGES * PAGE_SIZE) {
        vaddr_t end = MIN(va + TEARDOWN_CHUNK_PAGES * PAGE_SIZE, (vaddr_t)(base + size));
        arch_irq_state_t s = spin_lock_irqsave(&space->lock);
        vaddr_t run = 0;
        for (vaddr_t p = va; p < end; p += PAGE_SIZE) {
            paddr_t pa;
            bool present = arch_mmu_query(&space->mmu, p, &pa, NULL, NULL, NULL);
            if (present && run == 0)
                run = p;                                   /* a run begins */
            if (!present && run != 0) {
                arch_mmu_sync_icache_user(run, p - run);   /* ... and ends */
                run = 0;
            }
        }
        if (run != 0)
            arch_mmu_sync_icache_user(run, end - run);
        spin_unlock_irqrestore(&space->lock, s);
    }
}

uint64_t vm_user_mapped_pages_sum(struct vm_space *space)
{
    uint64_t n = 0;
    arch_irq_state_t s = spin_lock_irqsave(&space->lock);
    struct vm_region *r;
    list_for_each_entry(r, &space->regions, link)
        n += r->size / PAGE_SIZE;
    spin_unlock_irqrestore(&space->lock, s);
    return n;
}

bool vm_user_range_quiesced(struct vm_space *space, uint64_t base, size_t size)
{
    arch_irq_state_t s = spin_lock_irqsave(&space->lock);
    bool q = range_quiesced(space, (vaddr_t)base, size);
    spin_unlock_irqrestore(&space->lock, s);
    return q;
}

unsigned vm_user_region_count(struct vm_space *space)
{
    unsigned n = 0;
    arch_irq_state_t s = spin_lock_irqsave(&space->lock);
    struct vm_region *r;
    list_for_each_entry(r, &space->regions, link)
        n++;
    spin_unlock_irqrestore(&space->lock, s);
    return n;
}

/*
 * Advisory: the answer is true when the lock is released and not after.
 * A caller that will map what this returns, in a space another thread of
 * the same process can place into, must use vm_user_map_anon_free or
 * vm_user_map_file_free instead, which choose and insert under one hold
 * (invariant M46, docs/audit/next-subsystem-mmap-place.md). Two callers
 * may use it: process creation placing an ET_DYN interpreter, before the
 * process has a second thread, and memtest's filler racer, whose purpose
 * is to take what it is offered and see what happens.
 */
uint64_t vm_user_find_free(struct vm_space *space, uint64_t from, size_t size)
{
    KASSERT(space->user);
    if (!is_page_aligned(size) || size == 0)
        return 0;
    arch_irq_state_t s = spin_lock_irqsave(&space->lock);
    uint64_t result = range_first_fit_locked(space, from, size);
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

/*
 * Detach a System V shm attach: unmap every region whose file-map carries
 * `tag` -- the identity vm_user_map_shm handed back at shmat. The tag is
 * unique per map call and is copied to the pieces a split produces, so this
 * removes exactly the original attach's remaining mapping(s): a part the
 * program unmapped itself is simply gone, a re-attach of the same segment has
 * a different tag, and a mapping the program put over the attach has none of
 * ours. Each region is claimed (VM_REGION_QUIESCED) and torn down under the
 * map_replace discipline -- one at a time, serialized with the other teardown
 * path by replace_lock, the region claimed before the lock is released -- so
 * there is no window for a concurrent mmap to be handed the range and then be
 * removed. A whole region is unmapped each pass, so no split and no failure.
 */
void vm_user_unmap_tag(struct vm_space *space, uint64_t tag)
{
    KASSERT(space->user);
    if (tag == 0)
        return;

    mutex_lock(&space->replace_lock);
    for (;;) {
        struct vm_region *r = NULL;
        uint64_t base = 0, size = 0;
        arch_irq_state_t s = spin_lock_irqsave(&space->lock);
        struct vm_region *it;
        list_for_each_entry(it, &space->regions, link) {
            if (it->kind == VM_REGION_FILE && it->fmap != NULL && it->fmap->tag == tag &&
                !(it->flags & VM_REGION_QUIESCED)) {
                r = it;
                base = it->base;
                size = it->size;
                it->flags |= VM_REGION_QUIESCED;   /* claim: faults back off, no map takes it */
                space->mapped_pages -= size / PAGE_SIZE;
                break;
            }
        }
        spin_unlock_irqrestore(&space->lock, s);
        if (r == NULL)
            break;   /* no more of this attach's regions */

        user_range_teardown(space, (vaddr_t)base, size);
        file_hold_release(space);

        s = spin_lock_irqsave(&space->lock);
        list_remove(&r->link);
        spin_unlock_irqrestore(&space->lock, s);
        region_put(r);   /* frees the region and drops its fmap/vnode reference */
    }
    mutex_unlock(&space->replace_lock);
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
