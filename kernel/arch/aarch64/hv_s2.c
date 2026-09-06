/*
 * hv_s2.c - Stage-2 translation tables for an EL2 guest
 * (docs/kernel-services/virtualization/design.md, "The AArch64 EL2
 * backend"). The third builder of the same shape as svm_npt.c and
 * vmx_ept.c, with LPAE stage-2 descriptors; pure enough for the host
 * test, which is where its evidence comes from.
 */

#include <kernel/errno.h>
#include <kernel/page.h>
#include <kernel/pmm.h>
#include <kernel/string.h>

#include <arch/hv.h>
#include <aarch64/hv_s2.h>

#define S2_ENTRIES 512u
#define S2_LARGE_SIZE (2u * 1024 * 1024)

/* A table descriptor is valid + table; a leaf is valid + page (level 3)
 * or valid + block (levels 1 and 2). */
#define S2_VALID  (1ull << 0)
#define S2_TABLE  (1ull << 1)
#define S2_PAGE   (1ull << 1)   /* only at the last level */

static uint64_t leaf_flags(unsigned prot, bool large)
{
    /* MemAttr 0xF: normal, inner and outer write-back cacheable.
     * S2AP: bit 6 read, bit 7 write. AF set so no access flag faults.
     * SH inner-shareable. XN (bits 53-54) 0b10 when execution is not
     * allowed at EL1 or EL0. */
    uint64_t f = S2_VALID | (0xFull << 2) | (3ull << 8) | (1ull << 10);
    if (!large)
        f |= S2_PAGE;
    if (prot & HV_MAP_READ)
        f |= 1ull << 6;
    if (prot & HV_MAP_WRITE)
        f |= 1ull << 7;
    if (!(prot & HV_MAP_EXEC))
        f |= 2ull << 53;
    return f;
}

static inline bool present(uint64_t e)
{
    return (e & S2_VALID) != 0;
}

static inline unsigned idx(uint64_t ipa, unsigned level)   /* level 3 = top .. 0 = last */
{
    return (unsigned)((ipa >> (12 + 9 * level)) & 0x1FF);
}

static paddr_t table_alloc(void)
{
    struct page *pg = pmm_alloc_page(PMM_FLAGS_ZERO);
    return pg ? page_to_phys(pg) : 0;
}

paddr_t hv_s2_create(void)
{
    return table_alloc();
}

static void destroy_level(paddr_t table, unsigned level)
{
    uint64_t *t = phys_to_virt(table);
    if (level > 0) {
        for (unsigned i = 0; i < S2_ENTRIES; i++)
            if (present(t[i]) && (t[i] & S2_TABLE))   /* a block leaf has bit 1 clear */
                destroy_level(t[i] & S2_ADDR_MASK, level - 1);
    }
    pmm_free_page(phys_to_page(table));
}

void hv_s2_destroy(paddr_t root)
{
    if (root)
        destroy_level(root, 3);
}

/* The entry for `ipa` at `stop` (0 = 4 KiB page, 1 = 2 MiB block), NULL
 * when a level is absent or a block already covers the address. */
static uint64_t *walk_to(paddr_t root, uint64_t ipa, bool create, unsigned stop)
{
    paddr_t table = root;
    for (unsigned level = 3; level > stop; level--) {
        uint64_t *t = phys_to_virt(table);
        uint64_t e = t[idx(ipa, level)];
        if (present(e) && !(e & S2_TABLE))
            return NULL;   /* a block leaf covers this address */
        if (!present(e)) {
            if (!create)
                return NULL;
            paddr_t next = table_alloc();
            if (next == 0)
                return NULL;
            e = next | S2_VALID | S2_TABLE;
            t[idx(ipa, level)] = e;
        }
        table = e & S2_ADDR_MASK;
    }
    uint64_t *t = phys_to_virt(table);
    return &t[idx(ipa, stop)];
}

int hv_s2_map(paddr_t root, uint64_t ipa, paddr_t pa, size_t len, unsigned prot)
{
    if ((ipa | pa | len) & (PAGE_SIZE - 1))
        return -EINVAL;
    if (prot == 0 || (prot & ~(unsigned)HV_MAP_RWX))
        return -EINVAL;
    for (size_t off = 0; off < len;) {
        bool large = ((ipa + off) % S2_LARGE_SIZE) == 0 && ((pa + off) % S2_LARGE_SIZE) == 0 &&
                     len - off >= S2_LARGE_SIZE;
        if (!large) {
            uint64_t *blk = walk_to(root, ipa + off, false, 1u);
            if (blk != NULL && present(*blk) && !(*blk & S2_TABLE)) {
                hv_s2_unmap(root, ipa, off);
                return -EEXIST;
            }
        }
        uint64_t *e = walk_to(root, ipa + off, true, large ? 1u : 0u);
        if (e == NULL) {
            hv_s2_unmap(root, ipa, off);
            return -ENOMEM;
        }
        if (present(*e)) {
            hv_s2_unmap(root, ipa, off);
            return -EEXIST;
        }
        *e = (pa + off) | leaf_flags(prot, large);
        off += large ? S2_LARGE_SIZE : PAGE_SIZE;
    }
    return 0;
}

int hv_s2_unmap(paddr_t root, uint64_t ipa, size_t len)
{
    if ((ipa | len) & (PAGE_SIZE - 1))
        return -EINVAL;
    for (size_t off = 0; off < len;) {
        uint64_t *blk = walk_to(root, ipa + off, false, 1u);
        if (blk != NULL && present(*blk) && !(*blk & S2_TABLE)) {
            if (((ipa + off) % S2_LARGE_SIZE) != 0 || len - off < S2_LARGE_SIZE)
                return -EINVAL;   /* a block goes as a whole or not at all */
            *blk = 0;
            off += S2_LARGE_SIZE;
            continue;
        }
        uint64_t *e = walk_to(root, ipa + off, false, 0u);
        if (e)
            *e = 0;
        off += PAGE_SIZE;
    }
    /* Intermediate tables are kept until hv_s2_destroy, as NPT and EPT
     * keep theirs. The caller invalidates. */
    return 0;
}

bool hv_s2_query(paddr_t root, uint64_t ipa, paddr_t *pa)
{
    uint64_t *blk = walk_to(root, ipa & ~(uint64_t)(S2_LARGE_SIZE - 1), false, 1u);
    if (blk != NULL && present(*blk) && !(*blk & S2_TABLE)) {
        if (pa)
            *pa = (*blk & S2_ADDR_MASK & ~(uint64_t)(S2_LARGE_SIZE - 1)) | (ipa & (S2_LARGE_SIZE - 1));
        return true;
    }
    uint64_t *e = walk_to(root, ipa & ~(uint64_t)(PAGE_SIZE - 1), false, 0u);
    if (e == NULL || !present(*e))
        return false;
    if (pa)
        *pa = (*e & S2_ADDR_MASK) | (ipa & (PAGE_SIZE - 1));
    return true;
}

static unsigned count_level(paddr_t table, unsigned level)
{
    unsigned n = 1;
    if (level > 0) {
        const uint64_t *t = phys_to_virt(table);
        for (unsigned i = 0; i < S2_ENTRIES; i++)
            if (present(t[i]) && (t[i] & S2_TABLE))
                n += count_level(t[i] & S2_ADDR_MASK, level - 1);
    }
    return n;
}

unsigned hv_s2_table_pages(paddr_t root)
{
    return root ? count_level(root, 3) : 0;
}

/* VTCR_EL2 for a four-level 4 KiB walk over `pa_bits` of output, which
 * is what ID_AA64MMFR0_EL1.PARange reports: T0SZ = 64 - pa_bits, SL0 = 2
 * (start at level 1 of four), inner-shareable write-back both ways. The
 * SMMU driver derives its stage-2 configuration the same way, for the
 * same reason: assuming 48 bits is what a 44-bit machine refuses. */
uint64_t hv_s2_vtcr(unsigned pa_bits, unsigned ps_field)
{
    uint64_t t0sz = 64u - pa_bits;
    return t0sz | (2ull << 6) /* SL0 */ | (1ull << 8) /* IRGN0 WBWA */ | (1ull << 10) /* ORGN0 WBWA */ |
           (3ull << 12) /* SH0 inner */ | (0ull << 14) /* TG0 4 KiB */ | ((uint64_t)ps_field << 16);
}
