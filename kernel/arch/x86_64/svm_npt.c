/*
 * svm_npt.c - Nested page tables for AMD-V
 * (docs/kernel-services/virtualization/design.md, "Nested page tables").
 *
 * The 4-level long-mode format with 4 KiB leaves. Every present entry
 * carries P, RW and US: a nested walk treats the guest as a user
 * accessor, so a table without the User bit faults on every access. Only
 * the PMM and the direct map are used, so tests/host/test_hv.c runs this
 * file over the harness arena.
 */

#include <kernel/errno.h>
#include <kernel/page.h>
#include <kernel/pmm.h>
#include <kernel/string.h>

#include <x86/paging.h>
#include <x86/svm.h>

#define NPT_ENTRIES 512u
#define NPT_TABLE_FLAGS (PTE_P | PTE_RW | PTE_US)
#define NPT_LEAF_FLAGS  (PTE_P | PTE_RW | PTE_US)

static inline unsigned idx(uint64_t gpa, unsigned level)   /* level 3 = PML4 .. 0 = PT */
{
    return (unsigned)((gpa >> (12 + 9 * level)) & 0x1FF);
}

static paddr_t table_alloc(void)
{
    struct page *pg = pmm_alloc_page(PMM_FLAGS_ZERO);
    return pg ? page_to_phys(pg) : 0;
}

static void table_free(paddr_t pa)
{
    pmm_free_page(phys_to_page(pa));
}

paddr_t npt_create(void)
{
    return table_alloc();
}

static void destroy_level(paddr_t table, unsigned level)
{
    uint64_t *t = phys_to_virt(table);
    if (level > 0) {
        for (unsigned i = 0; i < NPT_ENTRIES; i++)
            if (t[i] & PTE_P)
                destroy_level(t[i] & PTE_ADDR_MASK, level - 1);
    }
    table_free(table);
}

void npt_destroy(paddr_t root)
{
    if (root)
        destroy_level(root, 3);
}

/* The PT entry for gpa, allocating intermediate tables when `create`. */
static uint64_t *walk(paddr_t root, uint64_t gpa, bool create)
{
    paddr_t table = root;
    for (unsigned level = 3; level > 0; level--) {
        uint64_t *t = phys_to_virt(table);
        uint64_t e = t[idx(gpa, level)];
        if (!(e & PTE_P)) {
            if (!create)
                return NULL;
            paddr_t next = table_alloc();
            if (next == 0)
                return NULL;
            e = next | NPT_TABLE_FLAGS;
            t[idx(gpa, level)] = e;
        }
        table = e & PTE_ADDR_MASK;
    }
    uint64_t *pt = phys_to_virt(table);
    return &pt[idx(gpa, 0)];
}

int npt_map(paddr_t root, uint64_t gpa, paddr_t hpa, size_t len)
{
    if ((gpa | hpa | len) & (PAGE_SIZE - 1))
        return -EINVAL;
    for (size_t off = 0; off < len; off += PAGE_SIZE) {
        uint64_t *pte = walk(root, gpa + off, true);
        if (pte == NULL) {
            npt_unmap(root, gpa, off);
            return -ENOMEM;
        }
        if (*pte & PTE_P) {
            npt_unmap(root, gpa, off);
            return -EEXIST;
        }
        *pte = (hpa + off) | NPT_LEAF_FLAGS;
    }
    return 0;
}

int npt_unmap(paddr_t root, uint64_t gpa, size_t len)
{
    if ((gpa | len) & (PAGE_SIZE - 1))
        return -EINVAL;
    for (size_t off = 0; off < len; off += PAGE_SIZE) {
        uint64_t *pte = walk(root, gpa + off, false);
        if (pte)
            *pte = 0;
    }
    /* Intermediate tables are kept: regions are only added while a VM
     * lives, and destroy frees everything. */
    return 0;
}

bool npt_query(paddr_t root, uint64_t gpa, paddr_t *hpa)
{
    uint64_t *pte = walk(root, gpa & ~(uint64_t)(PAGE_SIZE - 1), false);
    if (pte == NULL || !(*pte & PTE_P))
        return false;
    if (hpa)
        *hpa = (*pte & PTE_ADDR_MASK) | (gpa & (PAGE_SIZE - 1));
    return true;
}

static unsigned count_level(paddr_t table, unsigned level)
{
    unsigned n = 1;
    if (level > 0) {
        const uint64_t *t = phys_to_virt(table);
        for (unsigned i = 0; i < NPT_ENTRIES; i++)
            if (t[i] & PTE_P)
                n += count_level(t[i] & PTE_ADDR_MASK, level - 1);
    }
    return n;
}

unsigned npt_table_pages(paddr_t root)
{
    return root ? count_level(root, 3) : 0;
}
