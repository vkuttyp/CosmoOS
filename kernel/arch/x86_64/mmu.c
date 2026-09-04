/*
 * mmu.c - 4-level page tables for x86-64: the arch/mmu.h implementation.
 *
 * Tables are reached through the direct map, so every table page must be
 * RAM the direct map covers. That holds by construction: before the VMM
 * takes over only frames below 4 GiB are allocatable, and afterwards the
 * direct map covers all RAM.
 *
 * Intermediate entries are always present+writable and never NX; the
 * leaf carries the effective permissions. Large pages are used only when
 * the caller allows them and alignment permits. Empty intermediate
 * tables are not reclaimed on unmap (documented gap).
 */

#include <kernel/errno.h>
#include <kernel/page.h>
#include <kernel/panic.h>
#include <kernel/pmm.h>

#include <arch/mmu.h>

#include <x86/cpu.h>
#include <x86/paging.h>

#define LEVEL_PML4 4u
#define LEVEL_PDPT 3u
#define LEVEL_PD   2u
#define LEVEL_PT   1u

/* Bytes covered by one entry at each level. */
static const uint64_t level_size[5] = {
    0,
    (uint64_t)1 << 12,  /* PT   */
    (uint64_t)1 << 21,  /* PD   */
    (uint64_t)1 << 30,  /* PDPT */
    (uint64_t)1 << 39,  /* PML4 */
};

static bool g_probed;
static bool g_has_1g;
static bool g_has_nx;

static void probe(void)
{
    if (g_probed)
        return;
    struct cpuid_regs r;
    cpuid(0x80000000u, 0, &r);
    if (r.eax >= 0x80000001u) {
        cpuid(0x80000001u, 0, &r);
        g_has_1g = (r.edx & (1u << 26)) != 0;
        g_has_nx = (r.edx & (1u << 20)) != 0;
    }
    g_probed = true;
}

static inline pte_t *table_at(paddr_t pa)
{
    KASSERT(phys_in_direct_map(pa));
    return (pte_t *)phys_to_virt(pa);
}

static paddr_t alloc_table(void)
{
    struct page *page = pmm_alloc_page(PMM_FLAGS_ZERO);
    if (page == NULL)
        return 0;
    page->flags |= PG_PAGETABLE;
    return page_to_phys(page);
}

static pte_t leaf_flags(vm_prot_t prot, vm_cache_t cache, unsigned flags, bool large)
{
    pte_t f = PTE_P;
    if (prot & VM_PROT_WRITE)
        f |= PTE_RW;
    if (!(prot & VM_PROT_EXEC) && g_has_nx)
        f |= PTE_NX;
    if (flags & ARCH_MMU_MAP_GLOBAL)
        f |= PTE_G;
    if (large)
        f |= PTE_PS;
    switch (cache) {
    case VM_CACHE_UC: f |= PTE_PCD | PTE_PWT; break;
    case VM_CACHE_WT: f |= PTE_PWT; break;
    case VM_CACHE_WB:
    default: break;
    }
    return f;
}

struct walk {
    pte_t *entry;    /* the deepest entry examined */
    unsigned level;  /* level of that entry */
    bool present;
    bool leaf;       /* present and terminal (PS or PT level) */
};

static void walk(const struct arch_mmu_context *ctx, vaddr_t va, struct walk *w)
{
    pte_t *t = table_at(ctx->root);
    pte_t *e = &t[PML4_INDEX(va)];
    w->level = LEVEL_PML4;
    w->entry = e;
    w->present = (*e & PTE_P) != 0;
    w->leaf = false;
    if (!w->present)
        return;

    for (unsigned level = LEVEL_PDPT; level >= LEVEL_PT; level--) {
        t = table_at(*e & PTE_ADDR_MASK);
        unsigned idx = level == LEVEL_PDPT ? PDPT_INDEX(va) : level == LEVEL_PD ? PD_INDEX(va) : PT_INDEX(va);
        e = &t[idx];
        w->entry = e;
        w->level = level;
        w->present = (*e & PTE_P) != 0;
        if (!w->present)
            return;
        if (level == LEVEL_PT || (*e & PTE_PS)) {
            w->leaf = true;
            return;
        }
    }
}

/* Descend to `target_level`, creating tables. Returns the entry pointer
 * at that level, or NULL with *rc set on conflict/OOM. */
static pte_t *descend(struct arch_mmu_context *ctx, vaddr_t va, unsigned target_level, int *rc)
{
    pte_t *t = table_at(ctx->root);
    pte_t *e = &t[PML4_INDEX(va)];

    for (unsigned level = LEVEL_PML4; level > target_level; level--) {
        if ((*e & PTE_P) == 0) {
            paddr_t child = alloc_table();
            if (child == 0) {
                *rc = -ENOMEM;
                return NULL;
            }
            *e = child | PTE_P | PTE_RW;
        } else if (level != LEVEL_PML4 && (*e & PTE_PS)) {
            *rc = -EEXIST; /* a large page already covers this range */
            return NULL;
        }
        t = table_at(*e & PTE_ADDR_MASK);
        unsigned next = level - 1;
        unsigned idx = next == LEVEL_PDPT ? PDPT_INDEX(va) : next == LEVEL_PD ? PD_INDEX(va) : PT_INDEX(va);
        e = &t[idx];
    }
    return e;
}

int arch_mmu_context_init(struct arch_mmu_context *ctx)
{
    probe();
    ctx->root = alloc_table();
    return ctx->root ? 0 : -ENOMEM;
}

int arch_mmu_map(struct arch_mmu_context *ctx, vaddr_t va, paddr_t pa, size_t len,
                 vm_prot_t prot, vm_cache_t cache, unsigned flags)
{
    probe();
    if (!is_page_aligned(va) || !is_page_aligned(pa) || !is_page_aligned(len) || len == 0)
        return -EINVAL;
    if (prot == VM_PROT_NONE)
        return -EINVAL;

    vaddr_t end = va + len;
    while (va < end) {
        size_t remaining = end - va;
        unsigned level = LEVEL_PT;

        if (flags & ARCH_MMU_MAP_LARGE) {
            if (g_has_1g && (va & (PAGE_1G_SIZE - 1)) == 0 && (pa & (PAGE_1G_SIZE - 1)) == 0 &&
                remaining >= PAGE_1G_SIZE)
                level = LEVEL_PDPT;
            else if ((va & (PAGE_2M_SIZE - 1)) == 0 && (pa & (PAGE_2M_SIZE - 1)) == 0 &&
                     remaining >= PAGE_2M_SIZE)
                level = LEVEL_PD;
        }

        int rc = 0;
        pte_t *e = descend(ctx, va, level, &rc);
        if (e == NULL)
            return rc;
        if (*e & PTE_P)
            return -EEXIST;

        *e = (pa & PTE_ADDR_MASK) | leaf_flags(prot, cache, flags, level != LEVEL_PT);

        va += level_size[level];
        pa += level_size[level];
    }
    return 0;
}

/* Advance va to the start of the next entry at `level`. */
static vaddr_t next_boundary(vaddr_t va, unsigned level)
{
    uint64_t sz = level_size[level];
    return (va & ~(sz - 1)) + sz;
}

int arch_mmu_unmap(struct arch_mmu_context *ctx, vaddr_t va, size_t len)
{
    if (!is_page_aligned(va) || !is_page_aligned(len))
        return -EINVAL;

    vaddr_t start = va;
    vaddr_t end = va + len;

    /* Pass 1: refuse before changing anything if a large page would be split. */
    for (vaddr_t v = va; v < end;) {
        struct walk w;
        walk(ctx, v, &w);
        if (!w.present) {
            v = next_boundary(v, w.level);
            continue;
        }
        uint64_t sz = level_size[w.level];
        if ((v & (sz - 1)) != 0 || end - v < sz)
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

    arch_mmu_invalidate(ctx, start, len);
    return 0;
}

int arch_mmu_protect(struct arch_mmu_context *ctx, vaddr_t va, size_t len, vm_prot_t prot)
{
    if (!is_page_aligned(va) || !is_page_aligned(len) || prot == VM_PROT_NONE)
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
        if ((v & (sz - 1)) != 0 || end - v < sz)
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
        pte_t keep = *w.entry & (PTE_ADDR_MASK | PTE_PS | PTE_G | PTE_PCD | PTE_PWT | PTE_US | PTE_A | PTE_D);
        pte_t f = PTE_P;
        if (prot & VM_PROT_WRITE)
            f |= PTE_RW;
        if (!(prot & VM_PROT_EXEC) && g_has_nx)
            f |= PTE_NX;
        *w.entry = keep | f;
        v += level_size[w.level];
    }

    arch_mmu_invalidate(ctx, va, len);
    return 0;
}

bool arch_mmu_query(const struct arch_mmu_context *ctx, vaddr_t va, paddr_t *pa,
                    vm_prot_t *prot, vm_cache_t *cache, size_t *page_size)
{
    struct walk w;
    walk(ctx, va, &w);
    if (!w.present || !w.leaf)
        return false;

    pte_t e = *w.entry;
    uint64_t sz = level_size[w.level];

    if (pa)
        *pa = (e & PTE_ADDR_MASK & ~(sz - 1)) | (va & (sz - 1));
    if (prot) {
        vm_prot_t p = VM_PROT_READ;
        if (e & PTE_RW)
            p |= VM_PROT_WRITE;
        if ((e & PTE_NX) == 0)
            p |= VM_PROT_EXEC;
        *prot = p;
    }
    if (cache) {
        if ((e & (PTE_PCD | PTE_PWT)) == (PTE_PCD | PTE_PWT))
            *cache = VM_CACHE_UC;
        else if (e & PTE_PWT)
            *cache = VM_CACHE_WT;
        else
            *cache = VM_CACHE_WB;
    }
    if (page_size)
        *page_size = (size_t)sz;
    return true;
}

void arch_mmu_activate(const struct arch_mmu_context *ctx)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(ctx->root) : "memory");
}

void arch_mmu_invalidate(const struct arch_mmu_context *ctx, vaddr_t va, size_t len)
{
    (void)ctx; /* only the active context can be invalidated on x86 */

    if (len <= 64 * PAGE_SIZE) {
        for (vaddr_t v = page_align_down(va); v < va + len; v += PAGE_SIZE)
            __asm__ volatile("invlpg (%0)" : : "r"(v) : "memory");
        return;
    }

    /* Full flush including global entries: toggle CR4.PGE if enabled,
     * otherwise a CR3 reload suffices. */
    uint64_t cr4 = read_cr4();
    if (cr4 & CR4_PGE) {
        write_cr4(cr4 & ~CR4_PGE);
        write_cr4(cr4);
    } else {
        __asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
    }
}

size_t arch_mmu_large_page_sizes(void)
{
    probe();
    return PAGE_2M_SIZE | (g_has_1g ? PAGE_1G_SIZE : 0);
}

vaddr_t arch_mmu_kernel_base(void)
{
    return (vaddr_t)X86_KERNEL_BASE;
}
