/*
 * paging.c - Bootstrap page tables for the x86-64 kernel handoff.
 *
 * Layout produced (4-level paging, 48-bit):
 *
 *   PML4[0..]  -> identity map of [0, hhdm_size) with 2 MiB pages,
 *                 RW + NX, except the 2 MiB pages holding the running
 *                 loader image, which stay executable so the final jump
 *                 can execute after CR3 is switched. hhdm_size is the
 *                 caller's (main.c direct_map_size), one PML4 entry per
 *                 512 GiB of it.
 *   PML4[256..] -> the same PDPTs: the higher-half direct map at
 *                 BOOT_HHDM_BASE. Sharing the PDPTs costs nothing and keeps
 *                 the two views identical by construction.
 *   PML4[511]  -> the kernel image at its ELF link addresses, 4 KiB pages,
 *                 permissions from ELF segment flags (W^X enforced by the
 *                 ELF loader before we get here).
 *
 * All table pages come from one pre-sized pool so the walk never needs to
 * allocate after the pool is handed over. The pool is typed
 * EFI_MEMORY_TYPE_COSMO_PAGETABLES so the kernel can find and keep it.
 *
 * MMIO inside the identity range is mapped write-back like everything
 * else; the kernel must not touch device memory through these tables
 * without remapping it with proper attributes first.
 */

#include "loader.h"

#define PTE_P    (1ULL << 0)
#define PTE_RW   (1ULL << 1)
#define PTE_US   (1ULL << 2)
#define PTE_PS   (1ULL << 7)
#define PTE_G    (1ULL << 8)
#define PTE_NX   (1ULL << 63)
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

#define PML4_INDEX(v) (((v) >> 39) & 0x1FF)
#define PDPT_INDEX(v) (((v) >> 30) & 0x1FF)
#define PD_INDEX(v)   (((v) >> 21) & 0x1FF)
#define PT_INDEX(v)   (((v) >> 12) & 0x1FF)

/* The widest direct map (one PML4 entry per 512 GiB, from BOOT_HHDM_BASE's)
 * keeps clear of PML4[511], the kernel image's; the identity entries from
 * 0 stay below the HHDM's. */
_Static_assert(PML4_INDEX(BOOT_HHDM_BASE) + (BOOT_HHDM_MAX >> 39) <= 511, "the direct map's PML4 entries reach the kernel's");
_Static_assert((BOOT_HHDM_MAX >> 39) <= PML4_INDEX(BOOT_HHDM_BASE), "the identity entries reach the HHDM's");

static uint64_t *table_at(uint64_t phys)
{
    /* Loader runs on the firmware identity map, so phys == virt here. */
    return (uint64_t *)(uintptr_t)phys;
}

static uint64_t pool_take(struct paging_ctx *ctx)
{
    if (ctx->pool_used >= ctx->pool_pages)
        die("page-table pool exhausted (loader bug: pool estimate too small)", EFI_OUT_OF_RESOURCES);
    uint64_t page = ctx->pool_phys + ctx->pool_used * PAGE_SIZE;
    ctx->pool_used++;
    /* alloc_pages_low already zeroed the pool. */
    return page;
}

/* Return the physical address of the next-level table for `entry`,
 * creating it if absent. Intermediate entries are always P+RW and never
 * NX; the leaf decides permissions. */
static uint64_t *next_level(struct paging_ctx *ctx, uint64_t *table, unsigned index)
{
    if ((table[index] & PTE_P) == 0) {
        uint64_t page = pool_take(ctx);
        table[index] = page | PTE_P | PTE_RW;
    }
    return table_at(table[index] & PTE_ADDR_MASK);
}

static void map_2m(struct paging_ctx *ctx, uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t *pml4 = table_at(ctx->root);
    uint64_t *pdpt = next_level(ctx, pml4, (unsigned)PML4_INDEX(virt));
    uint64_t *pd = next_level(ctx, pdpt, (unsigned)PDPT_INDEX(virt));
    pd[PD_INDEX(virt)] = (phys & PTE_ADDR_MASK) | PTE_P | PTE_PS | flags;
}

static void map_4k(struct paging_ctx *ctx, uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t *pml4 = table_at(ctx->root);
    uint64_t *pdpt = next_level(ctx, pml4, (unsigned)PML4_INDEX(virt));
    uint64_t *pd = next_level(ctx, pdpt, (unsigned)PDPT_INDEX(virt));
    uint64_t *pt = next_level(ctx, pd, (unsigned)PD_INDEX(virt));
    pt[PT_INDEX(virt)] = (phys & PTE_ADDR_MASK) | PTE_P | flags;
}

uint64_t paging_pa_limit(void)
{
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000u), "c"(0));
    if (a < 0x80000008u)
        return 1ull << 36;   /* no leaf: the architectural minimum for long mode with PAE */
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000008u), "c"(0));
    unsigned bits = a & 0xFF;
    if (bits < 36 || bits > 52)
        bits = 36;
    return 1ull << bits;
}

UINTN paging_pool_size(const struct elf_image *img, UINTN mem_descriptors, uint64_t hhdm_size)
{
    /* x86-64 maps the whole direct map write-back and lets the MTRRs and
     * the PAT sort out device memory, so a mixed block costs nothing. */
    (void)mem_descriptors;
    /* PML4 + identity (one PDPT per 512 GiB + one PD per GiB) + kernel
     * (1 PDPT + 1 PD + one PT per 2 MiB of span, plus one for
     * misalignment) + slack. */
    UINTN identity_gib = (UINTN)(hhdm_size >> 30);
    UINTN identity_pdpts = (UINTN)((hhdm_size + (512ull << 30) - 1) >> 39);
    UINTN kernel_span_2m = (img->virt_end - img->virt_base + PAGE_2M - 1) / PAGE_2M;
    return 1 + (identity_pdpts + identity_gib) + (1 + 1 + kernel_span_2m + 1) + 4;
}

EFI_STATUS paging_build(struct paging_ctx *ctx, const struct elf_image *img, uint64_t loader_base,
                        uint64_t loader_size, const uint8_t *mmap, UINTN mmap_size, UINTN desc_size,
                        uint64_t hhdm_size)
{
    (void)mmap;
    (void)mmap_size;
    (void)desc_size;
    ctx->root = pool_take(ctx);
    uint64_t *pml4 = table_at(ctx->root);
    uint64_t nx = ctx->nx ? PTE_NX : 0;

    /* Identity map with 2 MiB pages. */
    uint64_t loader_lo = ALIGN_DOWN(loader_base, PAGE_2M);
    uint64_t loader_hi = ALIGN_UP(loader_base + loader_size, PAGE_2M);

    for (uint64_t p = 0; p < hhdm_size; p += PAGE_2M) {
        bool is_loader = (p >= loader_lo && p < loader_hi);
        uint64_t flags = PTE_RW | (is_loader ? 0 : nx);
        map_2m(ctx, p, p, flags);
    }

    /* HHDM alias: point at the same PDPTs. Because the PD tables are shared,
     * the loader's 2 MiB pages are executable through the HHDM as well
     * until the kernel replaces these tables. That is the documented
     * transient W+X exception in docs/boot/invariants.md. */
    for (uint64_t i = 0; i < (hhdm_size + (512ull << 30) - 1) >> 39; i++)
        pml4[PML4_INDEX(BOOT_HHDM_BASE) + i] = pml4[i];

    /* Kernel image with 4 KiB pages and ELF-derived permissions. */
    for (uint32_t i = 0; i < img->segment_count; i++) {
        const struct elf_segment *seg = &img->segments[i];
        uint64_t flags = 0;
        if (seg->flags & PF_W)
            flags |= PTE_RW;
        if ((seg->flags & PF_X) == 0)
            flags |= nx;
        flags |= PTE_G;

        for (uint64_t off = 0; off < seg->size; off += PAGE_SIZE) {
            uint64_t virt = seg->vaddr + off;
            uint64_t phys = img->phys_base + (virt - img->virt_base);
            map_4k(ctx, virt, phys, flags);
        }
    }

    return EFI_SUCCESS;
}
