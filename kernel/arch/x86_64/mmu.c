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
#include <kernel/ipi.h>
#include <kernel/page.h>
#include <kernel/panic.h>
#include <kernel/percpu.h>
#include <kernel/pmm.h>
#include <kernel/spinlock.h>
#include <kernel/timer.h>

#include <arch/cpu.h>
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

/*
 * W^X is not optional. Without NX every writable mapping would be
 * executable, so a processor that lacks it is refused here, before any
 * kernel-owned table exists. The loader checks the same bit earlier and
 * reports it on the firmware console; this is the second line.
 */
static void probe(void)
{
    if (g_probed)
        return;
    struct cpuid_regs r;
    bool has_nx = false;
    cpuid(0x80000000u, 0, &r);
    if (r.eax >= 0x80000001u) {
        cpuid(0x80000001u, 0, &r);
        g_has_1g = (r.edx & (1u << 26)) != 0;
        has_nx = (r.edx & (1u << 20)) != 0;
    }
    if (!has_nx)
        panic("mmu: processor has no NX support; W^X cannot be enforced, refusing to continue");
    if ((rdmsr(MSR_EFER) & EFER_NXE) == 0)
        panic("mmu: EFER.NXE is clear; CPU init must enable it before paging setup");
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
    if (!(prot & VM_PROT_EXEC))
        f |= PTE_NX;
    if (flags & ARCH_MMU_MAP_GLOBAL)
        f |= PTE_G;
    if (flags & ARCH_MMU_MAP_USER)
        f |= PTE_US;
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
 * at that level, or NULL with *rc set on conflict/OOM. Intermediate
 * entries for user mappings carry U/S (the leaf decides the rest);
 * kernel intermediates never do. */
static pte_t *descend(struct arch_mmu_context *ctx, vaddr_t va, unsigned target_level, bool user, int *rc)
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
            *e = child | PTE_P | PTE_RW | (user ? PTE_US : 0);
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
    /* The root must sit below 4 GiB: the AP trampoline loads it into
     * CR3 from 32-bit protected mode before entering long mode. */
    struct page *page = pmm_alloc_page(PMM_FLAGS_ZERO | PMM_FLAGS_ZONE_DMA32);
    if (page == NULL)
        return -ENOMEM;
    page->flags |= PG_PAGETABLE;
    ctx->root = page_to_phys(page);
    return 0;
}

int arch_mmu_context_init_user(struct arch_mmu_context *ctx, const struct arch_mmu_context *kernel)
{
    int rc = arch_mmu_context_init(ctx);
    if (rc)
        return rc;
    /* Kernel half: share the kernel's PDPTs. Those PML4 entries are
     * fixed after vmm_init (invariant P9), so the copy stays valid. */
    pte_t *dst = table_at(ctx->root);
    const pte_t *src = table_at(kernel->root);
    for (unsigned i = PT_ENTRIES / 2; i < PT_ENTRIES; i++)
        dst[i] = src[i];
    return 0;
}

static void free_table_tree(paddr_t table_pa, unsigned level)
{
    pte_t *t = table_at(table_pa);
    if (level > LEVEL_PT) {
        for (unsigned i = 0; i < PT_ENTRIES; i++) {
            if ((t[i] & PTE_P) == 0 || (t[i] & PTE_PS))
                continue;
            free_table_tree(t[i] & PTE_ADDR_MASK, level - 1);
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
    KASSERT(read_cr3() != ctx->root);

    /* Lower half only: the upper-half PDPTs belong to the kernel. */
    pte_t *pml4 = table_at(ctx->root);
    for (unsigned i = 0; i < PT_ENTRIES / 2; i++) {
        if ((pml4[i] & PTE_P) == 0)
            continue;
        free_table_tree(pml4[i] & PTE_ADDR_MASK, LEVEL_PDPT);
        pml4[i] = 0;
    }

    struct page *root = phys_to_page(ctx->root);
    KASSERT(root != NULL);
    root->flags &= ~PG_PAGETABLE;
    pmm_free_page(root);
    ctx->root = 0;
}

/* --- cross-CPU shootdown --- */

static spinlock_t g_shootdown_lock = SPINLOCK_INIT("shootdown");
static vaddr_t g_shootdown_va;
static size_t g_shootdown_len;
static volatile uint32_t g_shootdown_acks;
static struct arch_mmu_shootdown_stats g_shootdown_stats[CONFIG_MAX_CPUS];

/* IPI_TLB_FLUSH handler: invalidate the pending range and acknowledge. */
void arch_mmu_shootdown_ipi_handler(void)
{
    vaddr_t va = g_shootdown_va;
    size_t len = g_shootdown_len;
    barrier();
    arch_mmu_invalidate(NULL, va, len);
    g_shootdown_stats[arch_cpu_id()].handled++;
    __atomic_fetch_add(&g_shootdown_acks, 1u, __ATOMIC_ACQ_REL);
}

void arch_mmu_shootdown(const struct arch_mmu_context *ctx, vaddr_t va, size_t len)
{
    cpumask_t online = cpu_online_mask();
    unsigned targets = (unsigned)__builtin_popcountll(online) - 1;

    if (targets == 0) {
        arch_mmu_invalidate(ctx, va, len);
        return;
    }

    /* Waiting with interrupts disabled would deadlock against a target
     * that is itself spinning with interrupts off; the caller must have
     * released any such lock. */
    KASSERT(arch_irq_enabled());

    spin_lock(&g_shootdown_lock); /* preemption off, interrupts on */
    g_shootdown_va = va;
    g_shootdown_len = len;
    __atomic_store_n(&g_shootdown_acks, 0u, __ATOMIC_RELEASE);
    g_shootdown_stats[arch_cpu_id()].initiated++;

    ipi_broadcast_others(IPI_TLB_FLUSH);
    arch_mmu_invalidate(ctx, va, len);

    uint64_t deadline = clock_now_ns() + 1000000000ULL;
    while (__atomic_load_n(&g_shootdown_acks, __ATOMIC_ACQUIRE) < targets) {
        if (clock_now_ns() > deadline) {
            unsigned got = __atomic_load_n(&g_shootdown_acks, __ATOMIC_ACQUIRE);
            spin_unlock(&g_shootdown_lock);
            panic("mmu: TLB shootdown of %p+0x%zx acknowledged by %u of %u CPUs", (void *)va, len, got,
                  targets);
        }
        arch_cpu_relax();
    }
    g_shootdown_stats[arch_cpu_id()].acks_received += targets;
    spin_unlock(&g_shootdown_lock);
}

void arch_mmu_shootdown_stats(struct arch_mmu_shootdown_stats *out)
{
    *out = g_shootdown_stats[arch_cpu_id()];
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
        pte_t *e = descend(ctx, va, level, (flags & ARCH_MMU_MAP_USER) != 0, &rc);
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
        if (!(prot & VM_PROT_EXEC))
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
        if (e & PTE_US)
            p |= VM_PROT_USER;
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

void arch_mmu_near_arena(vaddr_t *lo, vaddr_t *hi)
{
    *lo = (vaddr_t)0xFFFFFFFF88000000ULL;   /* -mcmodel=kernel: anywhere in the top 2 GiB */
    *hi = (vaddr_t)0xFFFFFFFFFF000000ULL;
}
