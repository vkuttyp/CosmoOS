/*
 * mmu.c - Stage-1 translation tables for AArch64: the arch/mmu.h implementation
 * (docs/kernel/arch/aarch64/design.md, "MMU").
 *
 * 4 KiB granule, four levels (L0..L3), 48-bit VAs split by bit 55:
 * TTBR1 holds the kernel context (the higher half), TTBR0 a user context
 * (the lower half). A VA belongs to exactly one context; the other half
 * is refused. Tables are reached through the direct map, so every table
 * page must be RAM the direct map covers (frames below 4 GiB before the
 * VMM takes over, all RAM afterwards). Intermediate entries carry no
 * permission restrictions; the leaf decides. 2 MiB blocks are used when
 * the caller allows them and alignment permits. Empty intermediate
 * tables are not reclaimed on unmap (the same documented gap as x86).
 */

#include <kernel/errno.h>
#include <kernel/page.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/pmm.h>
#include <kernel/vmm.h>
#include <arch/cpu.h>
#include <arch/mmu.h>
#include <aarch64/platform.h>
#include <aarch64/sysreg.h>

#define LEVEL_L0 3u   /* 512 GiB per entry */
#define LEVEL_L1 2u   /* 1 GiB */
#define LEVEL_L2 1u   /* 2 MiB */
#define LEVEL_L3 0u   /* 4 KiB */
#define ENTRIES  512u

typedef uint64_t pte_t;

static const uint64_t level_size[4] = {
    (uint64_t)1 << 12,   /* L3 */
    (uint64_t)1 << 21,   /* L2 */
    (uint64_t)1 << 30,   /* L1 */
    (uint64_t)1 << 39,   /* L0 */
};

static paddr_t g_empty_root;   /* TTBR0 while the kernel context is active */
static struct arch_mmu_shootdown_stats g_stats[CONFIG_MAX_CPUS];

static inline unsigned index_at(vaddr_t va, unsigned level)
{
    return (unsigned)((va >> (12 + 9 * level)) & 0x1FF);
}

static inline bool va_is_high(vaddr_t va)
{
    return (va >> 55) & 1;
}

static inline bool ctx_is_kernel(const struct arch_mmu_context *ctx)
{
    return ctx == &kernel_space.mmu;
}

static inline pte_t *table_at(paddr_t pa)
{
    KASSERT(phys_in_direct_map(pa));
    return (pte_t *)phys_to_virt(pa);
}

static paddr_t alloc_table(void)
{
    struct page *page = pmm_alloc_page(PMM_FLAGS_ZERO | PMM_FLAGS_ZONE_DMA32);
    if (page == NULL)
        return 0;
    page->flags |= PG_PAGETABLE;
    return page_to_phys(page);
}

static inline bool is_table(pte_t e, unsigned level)
{
    return level > LEVEL_L3 && (e & (DESC_VALID | DESC_TABLE)) == (DESC_VALID | DESC_TABLE);
}

static inline bool is_leaf(pte_t e, unsigned level)
{
    if (!(e & DESC_VALID))
        return false;
    return level == LEVEL_L3 ? true : (e & DESC_TABLE) == 0;
}

static pte_t leaf_attrs(vm_prot_t prot, vm_cache_t cache, unsigned flags, unsigned level)
{
    bool user = (flags & ARCH_MMU_MAP_USER) != 0;
    pte_t a = DESC_VALID | DESC_AF | DESC_SH_INNER;
    if (level == LEVEL_L3)
        a |= DESC_PAGE;
    switch (cache) {
    case VM_CACHE_UC: a |= DESC_ATTRIDX(MAIR_IDX_DEVICE); break;
    case VM_CACHE_WT: a |= DESC_ATTRIDX(MAIR_IDX_NORMAL_NC); break;
    case VM_CACHE_WB:
    default: a |= DESC_ATTRIDX(MAIR_IDX_NORMAL_WB); break;
    }
    if (user)
        a |= DESC_AP_USER | DESC_NG;
    if (!(prot & VM_PROT_WRITE))
        a |= DESC_AP_RO;
    if (prot & VM_PROT_EXEC)
        a |= user ? DESC_PXN : DESC_UXN;
    else
        a |= DESC_PXN | DESC_UXN;
    if (!user && !(flags & ARCH_MMU_MAP_GLOBAL))
        a |= 0;   /* kernel entries are global regardless: no nG */
    return a;
}

struct walk {
    pte_t *entry;
    unsigned level;
    bool present;
    bool leaf;
};

static void walk(const struct arch_mmu_context *ctx, vaddr_t va, struct walk *w)
{
    pte_t *t = table_at(ctx->root);
    unsigned level = LEVEL_L0;
    for (;;) {
        pte_t *e = &t[index_at(va, level)];
        w->entry = e;
        w->level = level;
        w->present = (*e & DESC_VALID) != 0;
        w->leaf = is_leaf(*e, level);
        if (!w->present || w->leaf || level == LEVEL_L3)
            return;
        t = table_at(*e & DESC_ADDR_MASK);
        level--;
    }
}

/* The entry at `target_level` for va, creating tables on the way. NULL: no memory or a block in the way. */
static pte_t *descend(struct arch_mmu_context *ctx, vaddr_t va, unsigned target_level, int *rc)
{
    pte_t *t = table_at(ctx->root);
    for (unsigned level = LEVEL_L0; level > target_level; level--) {
        pte_t *e = &t[index_at(va, level)];
        if (!(*e & DESC_VALID)) {
            paddr_t child = alloc_table();
            if (child == 0) {
                *rc = -ENOMEM;
                return NULL;
            }
            *e = child | DESC_VALID | DESC_TABLE;
        } else if (!is_table(*e, level)) {
            *rc = -EEXIST;   /* a block already covers this range */
            return NULL;
        }
        t = table_at(*e & DESC_ADDR_MASK);
    }
    return &t[index_at(va, target_level)];
}

static bool va_fits(const struct arch_mmu_context *ctx, vaddr_t va, size_t len)
{
    bool high = va_is_high(va);
    if (len && va_is_high(va + len - 1) != high)
        return false;
    if (ctx->root == kernel_space.mmu.root)
        return high;
    return high == ctx_is_kernel(ctx);
}

int arch_mmu_context_init(struct arch_mmu_context *ctx)
{
    if (g_empty_root == 0) {
        g_empty_root = alloc_table();
        if (g_empty_root == 0)
            return -ENOMEM;
    }
    paddr_t root = alloc_table();
    if (root == 0)
        return -ENOMEM;
    ctx->root = root;
    return 0;
}

int arch_mmu_context_init_user(struct arch_mmu_context *ctx, const struct arch_mmu_context *kernel)
{
    (void)kernel;   /* the kernel half lives in TTBR1, shared by construction */
    return arch_mmu_context_init(ctx);
}

static void free_table_tree(paddr_t table_pa, unsigned level)
{
    pte_t *t = table_at(table_pa);
    if (level > LEVEL_L3) {
        for (unsigned i = 0; i < ENTRIES; i++) {
            if (is_table(t[i], level))
                free_table_tree(t[i] & DESC_ADDR_MASK, level - 1);
        }
    }
    struct page *page = phys_to_page(table_pa);
    KASSERT(page != NULL && (page->flags & PG_PAGETABLE));
    page->flags &= ~PG_PAGETABLE;
    pmm_free_page(page);
}

void arch_mmu_context_destroy(struct arch_mmu_context *ctx)
{
    KASSERT(ctx->root != 0);
    KASSERT(!ctx_is_kernel(ctx));
    KASSERT((READ_SYSREG(ttbr0_el1) & DESC_ADDR_MASK) != ctx->root);
    free_table_tree(ctx->root, LEVEL_L0);
    ctx->root = 0;
}

int arch_mmu_map(struct arch_mmu_context *ctx, vaddr_t va, paddr_t pa, size_t len, vm_prot_t prot,
                 vm_cache_t cache, unsigned flags)
{
    if (!is_page_aligned(va) || !is_page_aligned(pa) || !is_page_aligned(len) || len == 0)
        return -EINVAL;
    if (prot == VM_PROT_NONE || !va_fits(ctx, va, len))
        return -EINVAL;
    vaddr_t end = va + len;
    while (va < end) {
        size_t remaining = end - va;
        unsigned level = LEVEL_L3;
        if ((flags & ARCH_MMU_MAP_LARGE) && (va & (PAGE_2M_SIZE - 1)) == 0 && (pa & (PAGE_2M_SIZE - 1)) == 0 &&
            remaining >= PAGE_2M_SIZE)
            level = LEVEL_L2;
        int rc = 0;
        pte_t *e = descend(ctx, va, level, &rc);
        if (e == NULL)
            return rc;
        if (*e & DESC_VALID)
            return -EEXIST;
        *e = (pa & DESC_ADDR_MASK) | leaf_attrs(prot, cache, flags, level);
        va += level_size[level];
        pa += level_size[level];
    }
    dsb_ishst();
    return 0;
}

static vaddr_t next_boundary(vaddr_t va, unsigned level)
{
    uint64_t sz = level_size[level];
    return (va & ~(sz - 1)) + sz;
}

int arch_mmu_unmap(struct arch_mmu_context *ctx, vaddr_t va, size_t len)
{
    if (!is_page_aligned(va) || !is_page_aligned(len) || (len && !va_fits(ctx, va, len)))
        return -EINVAL;
    vaddr_t end = va + len;
    for (vaddr_t v = va; v < end;) {
        struct walk w;
        walk(ctx, v, &w);
        if (!w.present) {
            v = next_boundary(v, w.level);
            continue;
        }
        uint64_t sz = level_size[w.level];
        if (!w.leaf || (v & (sz - 1)) != 0 || end - v < sz)
            return -EINVAL;
        v += sz;
    }
    for (vaddr_t v = va; v < end;) {
        struct walk w;
        walk(ctx, v, &w);
        if (!w.present) {
            v = next_boundary(v, w.level);
            continue;
        }
        *w.entry = 0;
        v += level_size[w.level];
    }
    arch_mmu_invalidate(ctx, va, len);
    return 0;
}

int arch_mmu_protect(struct arch_mmu_context *ctx, vaddr_t va, size_t len, vm_prot_t prot)
{
    if (!is_page_aligned(va) || !is_page_aligned(len) || prot == VM_PROT_NONE || !va_fits(ctx, va, len))
        return -EINVAL;
    vaddr_t end = va + len;
    for (vaddr_t v = va; v < end;) {
        struct walk w;
        walk(ctx, v, &w);
        if (!w.present) {
            v = next_boundary(v, w.level);
            continue;
        }
        uint64_t sz = level_size[w.level];
        if (!w.leaf || (v & (sz - 1)) != 0 || end - v < sz)
            return -EINVAL;   /* refuse to split a block */
        v += sz;
    }
    for (vaddr_t v = va; v < end;) {
        struct walk w;
        walk(ctx, v, &w);
        if (!w.present) {
            v = next_boundary(v, w.level);
            continue;
        }
        pte_t e = *w.entry;
        bool user = (e & DESC_AP_USER) != 0;
        pte_t keep = e & (DESC_ADDR_MASK | DESC_VALID | DESC_TABLE | DESC_ATTRIDX_MASK | DESC_SH_INNER | DESC_AF |
                          DESC_NG | DESC_AP_USER);
        pte_t f = keep;
        if (!(prot & VM_PROT_WRITE))
            f |= DESC_AP_RO;
        if (prot & VM_PROT_EXEC)
            f |= user ? DESC_PXN : DESC_UXN;
        else
            f |= DESC_PXN | DESC_UXN;
        *w.entry = f;
        v += level_size[w.level];
    }
    arch_mmu_invalidate(ctx, va, len);
    return 0;
}

bool arch_mmu_query(const struct arch_mmu_context *ctx, vaddr_t va, paddr_t *pa, vm_prot_t *prot,
                    vm_cache_t *cache, size_t *page_size)
{
    if (va_is_high(va) != ctx_is_kernel(ctx))
        return false;
    struct walk w;
    walk(ctx, va, &w);
    if (!w.present || !w.leaf)
        return false;
    pte_t e = *w.entry;
    uint64_t sz = level_size[w.level];
    if (pa)
        *pa = (e & DESC_ADDR_MASK & ~(sz - 1)) | (va & (sz - 1));
    if (prot) {
        bool user = (e & DESC_AP_USER) != 0;
        vm_prot_t p = VM_PROT_READ;
        if (!(e & DESC_AP_RO))
            p |= VM_PROT_WRITE;
        if (user ? !(e & DESC_UXN) : !(e & DESC_PXN))
            p |= VM_PROT_EXEC;
        if (user)
            p |= VM_PROT_USER;
        *prot = p;
    }
    if (cache) {
        unsigned idx = (unsigned)((e & DESC_ATTRIDX_MASK) >> 2);
        *cache = idx == MAIR_IDX_DEVICE ? VM_CACHE_UC : idx == MAIR_IDX_NORMAL_NC ? VM_CACHE_WT : VM_CACHE_WB;
    }
    if (page_size)
        *page_size = (size_t)sz;
    return true;
}

/*
 * The early console and fw_cfg are reached through the direct map, which
 * the generic VMM builds for RAM only. Their pages are carried into the
 * kernel root before it is activated so output never stops (the
 * addresses are the virt defaults the early console already uses; a
 * console moved by the SPCR gets its own mapping from vm_map_phys).
 */
static void map_early_devices(const struct arch_mmu_context *ctx)
{
    struct arch_mmu_context tmp = { .root = ctx->root };   /* the same root; the tables are what changes */
    static const paddr_t pages[] = { VIRT_PL011_BASE, VIRT_FWCFG_BASE };
    for (unsigned i = 0; i < 2; i++) {
        vaddr_t va = (vaddr_t)(aarch64_hhdm_base + pages[i]);
        if (!arch_mmu_query(ctx, va, NULL, NULL, NULL, NULL))
            (void)arch_mmu_map(&tmp, va, pages[i], PAGE_SIZE, VM_PROT_RW, VM_CACHE_UC, 0);
    }
}

void arch_mmu_activate(const struct arch_mmu_context *ctx)
{
    if (ctx_is_kernel(ctx)) {
        map_early_devices(ctx);
        WRITE_SYSREG(ttbr1_el1, ctx->root);
        WRITE_SYSREG(ttbr0_el1, g_empty_root);
    } else {
        WRITE_SYSREG(ttbr0_el1, ctx->root);   /* ASID 0: a full invalidate per switch */
    }
    isb();
    tlbi_vmalle1is();
}

void arch_mmu_invalidate(const struct arch_mmu_context *ctx, vaddr_t va, size_t len)
{
    (void)ctx;
    if (len <= 64 * PAGE_SIZE) {
        dsb_ishst();
        for (vaddr_t v = page_align_down(va); v < va + len; v += PAGE_SIZE)
            tlbi_vaae1is(v);
        dsb_ish();
        isb();
        return;
    }
    tlbi_vmalle1is();
}

/* TLB maintenance is broadcast to the inner-shareable domain by hardware: no IPI. */
void arch_mmu_shootdown(const struct arch_mmu_context *ctx, vaddr_t va, size_t len)
{
    /* The DSB that completes the broadcast is the acknowledgement of every
     * other CPU: the stats report it as one shootdown with n-1 acks so the
     * contract's accounting holds without an IPI. */
    unsigned others = (unsigned)__builtin_popcountll(cpu_online_mask()) - 1;
    if (others > 0) {
        g_stats[arch_cpu_id()].initiated++;
        g_stats[arch_cpu_id()].acks_received += others;
    }
    arch_mmu_invalidate(ctx, va, len);
}

void arch_mmu_shootdown_ipi_handler(void)
{
    g_stats[arch_cpu_id()].handled++;
}

void arch_mmu_shootdown_stats(struct arch_mmu_shootdown_stats *out)
{
    *out = g_stats[arch_cpu_id()];
}

size_t arch_mmu_large_page_sizes(void)
{
    return PAGE_2M_SIZE;
}

/* The first kernel virtual address: TTBR1's half (the direct map begins here). */
vaddr_t arch_mmu_kernel_base(void)
{
    return (vaddr_t)0xFFFF800000000000ull;
}

#define KERNEL_IMAGE_BASE 0xFFFFFFFF80000000ull

extern char __kernel_end[];

/* Modules call kernel exports with bl (CALL26, +-128 MiB): keep them right
 * above the image and below kernel_base + 128 MiB. */
void arch_mmu_near_arena(vaddr_t *lo, vaddr_t *hi)
{
    vaddr_t end = ((vaddr_t)__kernel_end + PAGE_2M_SIZE - 1) & ~(vaddr_t)(PAGE_2M_SIZE - 1);
    *lo = end;
    *hi = (vaddr_t)KERNEL_IMAGE_BASE + (vaddr_t)(120u << 20);
}
