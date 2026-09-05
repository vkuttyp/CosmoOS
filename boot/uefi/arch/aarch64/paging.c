/*
 * arch/aarch64/paging.c - Bootstrap translation tables for AArch64
 * (docs/kernel/arch/aarch64/design.md, "The loader").
 *
 * Two roots, both 4 KiB granule, 48-bit:
 *   TTBR1 (root):      the direct map of PA 0..4 GiB at 0xFFFF800000000000
 *                      (RAM normal write-back, everything else device),
 *                      and the kernel image with its segment permissions.
 *   TTBR0 (root_user): the identity map of PA 0..4 GiB the loader keeps
 *                      running on while the tables are switched; RAM is
 *                      executable at EL1 there because this code is.
 * 2 MiB blocks everywhere except the kernel image (4 KiB pages).
 */

#include "loader.h"

#define DESC_VALID  (1ull << 0)
#define DESC_TABLE  (1ull << 1)
#define DESC_PAGE   (1ull << 1)
#define DESC_ATTR(i) ((uint64_t)(i) << 2)
#define DESC_AP_RO  (1ull << 7)
#define DESC_SH_INNER (3ull << 8)
#define DESC_AF     (1ull << 10)
#define DESC_PXN    (1ull << 53)
#define DESC_UXN    (1ull << 54)
#define DESC_ADDR_MASK 0x0000FFFFFFFFF000ull

#define ATTR_NORMAL 0u
#define ATTR_DEVICE 1u

#define L0_INDEX(v) (((v) >> 39) & 0x1FF)
#define L1_INDEX(v) (((v) >> 30) & 0x1FF)
#define L2_INDEX(v) (((v) >> 21) & 0x1FF)
#define L3_INDEX(v) (((v) >> 12) & 0x1FF)

static const uint8_t *g_mmap;
static UINTN g_mmap_size, g_desc_size;

static uint64_t *table_at(uint64_t phys)
{
    return (uint64_t *)(uintptr_t)phys;
}

static uint64_t pool_take(struct paging_ctx *ctx)
{
    if (ctx->pool_used >= ctx->pool_pages)
        die("page-table pool exhausted (loader bug: pool estimate too small)", EFI_OUT_OF_RESOURCES);
    uint64_t page = ctx->pool_phys + ctx->pool_used * PAGE_SIZE;
    ctx->pool_used++;
    return page;
}

static uint64_t *next_level(struct paging_ctx *ctx, uint64_t *table, unsigned index)
{
    if ((table[index] & DESC_VALID) == 0) {
        uint64_t page = pool_take(ctx);
        table[index] = page | DESC_VALID | DESC_TABLE;
    }
    return table_at(table[index] & DESC_ADDR_MASK);
}

static bool is_ram(uint64_t pa)
{
    for (UINTN off = 0; off + g_desc_size <= g_mmap_size; off += g_desc_size) {
        const EFI_MEMORY_DESCRIPTOR *d = (const EFI_MEMORY_DESCRIPTOR *)(g_mmap + off);
        uint64_t lo = d->PhysicalStart, hi = lo + d->NumberOfPages * PAGE_SIZE;
        if (pa < lo || pa >= hi)
            continue;
        switch (d->Type) {
        case EfiMemoryMappedIO:
        case EfiMemoryMappedIOPortSpace:
        case EfiReservedMemoryType:
        case EfiUnusableMemory:
            return false;
        default:
            return true;
        }
    }
    return false;   /* not described: a device the firmware does not list */
}

static uint64_t block_attrs(bool ram, bool exec_el1)
{
    uint64_t a = DESC_VALID | DESC_AF | DESC_SH_INNER | DESC_UXN;
    a |= ram ? DESC_ATTR(ATTR_NORMAL) : DESC_ATTR(ATTR_DEVICE);
    if (!exec_el1 || !ram)
        a |= DESC_PXN;
    return a;
}

static void map_2m(struct paging_ctx *ctx, uint64_t root, uint64_t virt, uint64_t phys, uint64_t attrs)
{
    uint64_t *l0 = table_at(root);
    uint64_t *l1 = next_level(ctx, l0, (unsigned)L0_INDEX(virt));
    uint64_t *l2 = next_level(ctx, l1, (unsigned)L1_INDEX(virt));
    l2[L2_INDEX(virt)] = (phys & DESC_ADDR_MASK) | attrs;   /* bit 1 clear: block */
}

static void map_4k(struct paging_ctx *ctx, uint64_t root, uint64_t virt, uint64_t phys, uint64_t attrs)
{
    uint64_t *l0 = table_at(root);
    uint64_t *l1 = next_level(ctx, l0, (unsigned)L0_INDEX(virt));
    uint64_t *l2 = next_level(ctx, l1, (unsigned)L1_INDEX(virt));
    uint64_t *l3 = next_level(ctx, l2, (unsigned)L2_INDEX(virt));
    l3[L3_INDEX(virt)] = (phys & DESC_ADDR_MASK) | attrs | DESC_PAGE;
}

UINTN paging_pool_size(const struct elf_image *img)
{
    UINTN gib = BOOT_HHDM_SIZE >> 30;
    UINTN kernel_span_2m = (img->virt_end - img->virt_base + PAGE_2M - 1) / PAGE_2M;
    /* TTBR1: L0 + L1 + one L2 per GiB of direct map; kernel: L1 + L2 + one L3 per 2 MiB.
     * TTBR0: L0 + L1 + one L2 per GiB. Plus slack. */
    return (2 + gib) + (2 + kernel_span_2m) + (2 + gib) + 4;
}

EFI_STATUS paging_build(struct paging_ctx *ctx, const struct elf_image *img, uint64_t loader_base,
                        uint64_t loader_size, const uint8_t *mmap, UINTN mmap_size, UINTN desc_size)
{
    (void)loader_base;
    (void)loader_size;
    g_mmap = mmap;
    g_mmap_size = mmap_size;
    g_desc_size = desc_size;
    ctx->root = pool_take(ctx);
    ctx->root_user = pool_take(ctx);
    for (uint64_t p = 0; p < BOOT_HHDM_SIZE; p += PAGE_2M) {
        bool ram = is_ram(p);
        map_2m(ctx, ctx->root, BOOT_HHDM_BASE + p, p, block_attrs(ram, false));
        map_2m(ctx, ctx->root_user, p, p, block_attrs(ram, true));
    }
    for (uint32_t i = 0; i < img->segment_count; i++) {
        const struct elf_segment *seg = &img->segments[i];
        uint64_t attrs = DESC_VALID | DESC_AF | DESC_SH_INNER | DESC_ATTR(ATTR_NORMAL) | DESC_UXN;
        if ((seg->flags & PF_W) == 0)
            attrs |= DESC_AP_RO;
        if ((seg->flags & PF_X) == 0)
            attrs |= DESC_PXN;
        for (uint64_t off = 0; off < seg->size; off += PAGE_SIZE) {
            uint64_t virt = seg->vaddr + off;
            uint64_t phys = img->phys_base + (virt - img->virt_base);
            map_4k(ctx, ctx->root, virt, phys, attrs);
        }
    }
    __asm__ volatile("dsb sy" ::: "memory");
    return EFI_SUCCESS;
}
