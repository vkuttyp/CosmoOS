/*
 * arch/aarch64/paging.c - Bootstrap translation tables for AArch64
 * (docs/kernel/arch/aarch64/design.md, "The loader").
 *
 * Two roots, both 4 KiB granule, 48-bit:
 *   TTBR1 (root):      the direct map of PA 0..hhdm_size at 0xFFFF800000000000
 *                      (RAM normal write-back, everything else device),
 *                      and the kernel image with its segment permissions.
 *   TTBR0 (root_user): the identity map of PA 0..hhdm_size the loader keeps
 *                      running on while the tables are switched; RAM is
 *                      executable at EL1 there because this code is.
 * hhdm_size is the caller's (main.c direct_map_size): it covers the
 * loader image wherever the firmware put it, so the PC stays mapped
 * across the switch. 2 MiB blocks everywhere except the kernel image
 * (4 KiB pages).
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

/* The widest direct map keeps clear of L0 entry 511, the kernel image's. */
_Static_assert(((BOOT_HHDM_BASE >> 39) & 0x1FF) + (BOOT_HHDM_MAX >> 39) <= 511,
               "the direct map's L0 entries reach the kernel's");

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

static bool type_is_ram(uint32_t type)
{
    switch (type) {
    case EfiMemoryMappedIO:
    case EfiMemoryMappedIOPortSpace:
    case EfiReservedMemoryType:
    case EfiUnusableMemory:
        return false;
    default:
        return true;
    }
}

static bool is_ram(uint64_t pa)
{
    for (UINTN off = 0; off + g_desc_size <= g_mmap_size; off += g_desc_size) {
        const EFI_MEMORY_DESCRIPTOR *d = (const EFI_MEMORY_DESCRIPTOR *)(g_mmap + off);
        uint64_t lo = d->PhysicalStart, hi = lo + d->NumberOfPages * PAGE_SIZE;
        if (pa >= lo && pa < hi)
            return type_is_ram(d->Type);
    }
    return false;   /* not described: a device the firmware does not list */
}

/*
 * How a 2 MiB block should be mapped. A block that is entirely RAM or
 * entirely device memory becomes one block entry; one that mixes the two
 * has to be mapped page by page, because the attributes differ and using
 * either for the whole block is wrong in a way that shows up much later:
 * device attributes on RAM make every unaligned access fault (the kernel's
 * own allocations, before it installs its tables), and normal attributes
 * on MMIO let the CPU reorder and merge accesses to a device.
 *
 * Mixed blocks appear wherever firmware puts a reserved range inside RAM
 * without 2 MiB alignment -- a framebuffer, for one, which is how this
 * was found.
 */
enum block_kind { BLOCK_RAM, BLOCK_DEVICE, BLOCK_MIXED };

static enum block_kind block_kind(uint64_t base)
{
    /* The RAM descriptors' overlap with the block, not a page-by-page
     * is_ram: a direct map tens of GiB wide has that many thousand
     * blocks. UEFI descriptors do not overlap, so the sum is exact. */
    uint64_t ram = 0;
    for (UINTN off = 0; off + g_desc_size <= g_mmap_size; off += g_desc_size) {
        const EFI_MEMORY_DESCRIPTOR *d = (const EFI_MEMORY_DESCRIPTOR *)(g_mmap + off);
        uint64_t lo = d->PhysicalStart, hi = lo + d->NumberOfPages * PAGE_SIZE;
        if (!type_is_ram(d->Type) || hi <= base || lo >= base + PAGE_2M)
            continue;
        ram += (hi < base + PAGE_2M ? hi : base + PAGE_2M) - (lo > base ? lo : base);
    }
    return ram == PAGE_2M ? BLOCK_RAM : ram == 0 ? BLOCK_DEVICE : BLOCK_MIXED;
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

uint64_t paging_pa_limit(void)
{
    static const uint8_t bits[] = { 32, 36, 40, 42, 44, 48 };
    uint64_t mmfr0;
    __asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
    uint64_t parange = mmfr0 & 0xF;
    /* 52 bits (6) needs FEAT_LPA's descriptor format; these tables use
     * the 48-bit one. Reserved encodings read as the largest known. */
    if (parange >= sizeof(bits))
        parange = sizeof(bits) - 1;
    return 1ull << bits[parange];
}

UINTN paging_pool_size(const struct elf_image *img, UINTN mem_descriptors, uint64_t hhdm_size)
{
    UINTN gib = (UINTN)(hhdm_size >> 30);
    UINTN l1s = (UINTN)((hhdm_size + (512ull << 30) - 1) >> 39);
    UINTN kernel_span_2m = (img->virt_end - img->virt_base + PAGE_2M - 1) / PAGE_2M;
    /* TTBR1: L0 + one L1 per 512 GiB + one L2 per GiB of direct map;
     * kernel: L1 + L2 + one L3 per 2 MiB. TTBR0: the same direct-map
     * shape, identity. Plus slack.
     *
     * A 2 MiB block that mixes RAM and device memory is mapped as pages
     * instead, which costs one L3 table in each root. A mixed block sits
     * on a boundary between the two kinds of memory, a boundary needs a
     * descriptor end, and a descriptor has two: up to two mixed blocks
     * per descriptor, each an L3 table in both roots. */
    return 2 * (1 + l1s + gib) + (2 + kernel_span_2m) + 4 + 4 * mem_descriptors;
}

EFI_STATUS paging_build(struct paging_ctx *ctx, const struct elf_image *img, uint64_t loader_base,
                        uint64_t loader_size, const uint8_t *mmap, UINTN mmap_size, UINTN desc_size,
                        uint64_t hhdm_size)
{
    (void)loader_base;
    (void)loader_size;
    g_mmap = mmap;
    g_mmap_size = mmap_size;
    g_desc_size = desc_size;
    ctx->root = pool_take(ctx);
    ctx->root_user = pool_take(ctx);
    UINTN mixed = 0;
    for (uint64_t p = 0; p < hhdm_size; p += PAGE_2M) {
        enum block_kind kind = block_kind(p);
        if (kind != BLOCK_MIXED) {
            bool ram = kind == BLOCK_RAM;
            map_2m(ctx, ctx->root, BOOT_HHDM_BASE + p, p, block_attrs(ram, false));
            map_2m(ctx, ctx->root_user, p, p, block_attrs(ram, true));
            continue;
        }
        mixed++;
        for (uint64_t off = 0; off < PAGE_2M; off += PAGE_SIZE) {
            bool ram = is_ram(p + off);
            map_4k(ctx, ctx->root, BOOT_HHDM_BASE + p + off, p + off, block_attrs(ram, false));
            map_4k(ctx, ctx->root_user, p + off, p + off, block_attrs(ram, true));
        }
    }
    if (mixed > 0)
        lprintf("paging: %u block(s) of 2 MiB mix RAM and device memory; mapped as 4 KiB pages\n",
                (unsigned)mixed);
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
