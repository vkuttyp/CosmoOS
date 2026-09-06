/*
 * vmx_ept.c - Extended page tables: the guest-physical to host-physical
 * map of a VMX guest (docs/kernel-services/virtualization/design.md).
 *
 * The same four-level, 4 KiB-granule shape as svm_npt.c, with EPT's own
 * entry encoding: read/write/execute are separate bits, leaves carry a
 * memory type, and there is no user bit. Pure enough for the host test.
 */

#include <kernel/errno.h>
#include <kernel/page.h>
#include <kernel/pmm.h>
#include <kernel/string.h>

#include <arch/hv.h>
#include <x86/vmx.h>

#define EPT_ENTRIES 512u
#define EPT_LARGE_SIZE (2u * 1024 * 1024)
/* Intermediate entries permit everything; the leaf decides. */
#define EPT_TABLE_FLAGS (EPT_READ | EPT_WRITE | EPT_EXEC)

static uint64_t leaf_flags(unsigned prot)
{
    uint64_t f = EPT_MEMTYPE_WB;
    if (prot & HV_MAP_READ)
        f |= EPT_READ;
    if (prot & HV_MAP_WRITE)
        f |= EPT_WRITE;
    if (prot & HV_MAP_EXEC)
        f |= EPT_EXEC;
    return f;
}

/* An entry is present when it grants any access at all. */
static inline bool present(uint64_t e)
{
    return (e & (EPT_READ | EPT_WRITE | EPT_EXEC)) != 0;
}

static inline unsigned idx(uint64_t gpa, unsigned level)   /* level 3 = PML4 .. 0 = PT */
{
    return (unsigned)((gpa >> (12 + 9 * level)) & 0x1FF);
}

static paddr_t table_alloc(void)
{
    struct page *pg = pmm_alloc_page(PMM_FLAGS_ZERO);
    return pg ? page_to_phys(pg) : 0;
}

paddr_t ept_create(void)
{
    return table_alloc();
}

static void destroy_level(paddr_t table, unsigned level)
{
    uint64_t *t = phys_to_virt(table);
    if (level > 0) {
        for (unsigned i = 0; i < EPT_ENTRIES; i++)
            if (present(t[i]) && !(t[i] & EPT_LARGE))   /* a large leaf is guest memory */
                destroy_level(t[i] & EPT_ADDR_MASK, level - 1);
    }
    pmm_free_page(phys_to_page(table));
}

void ept_destroy(paddr_t root)
{
    if (root)
        destroy_level(root, 3);
}

/* The entry for gpa at `stop` (0 = 4 KiB leaf, 1 = 2 MiB leaf), NULL
 * when a level is absent or a large leaf already covers the address. */
static uint64_t *walk_to(paddr_t root, uint64_t gpa, bool create, unsigned stop)
{
    paddr_t table = root;
    for (unsigned level = 3; level > stop; level--) {
        uint64_t *t = phys_to_virt(table);
        uint64_t e = t[idx(gpa, level)];
        if (e & EPT_LARGE)
            return NULL;
        if (!present(e)) {
            if (!create)
                return NULL;
            paddr_t next = table_alloc();
            if (next == 0)
                return NULL;
            e = next | EPT_TABLE_FLAGS;
            t[idx(gpa, level)] = e;
        }
        table = e & EPT_ADDR_MASK;
    }
    uint64_t *t = phys_to_virt(table);
    return &t[idx(gpa, stop)];
}

int ept_map(paddr_t root, uint64_t gpa, paddr_t hpa, size_t len, unsigned prot)
{
    if ((gpa | hpa | len) & (PAGE_SIZE - 1))
        return -EINVAL;
    if (prot == 0 || (prot & ~(unsigned)HV_MAP_RWX))
        return -EINVAL;
    uint64_t flags = leaf_flags(prot);
    for (size_t off = 0; off < len;) {
        bool large = ((gpa + off) % EPT_LARGE_SIZE) == 0 && ((hpa + off) % EPT_LARGE_SIZE) == 0 && len - off >= EPT_LARGE_SIZE;
        if (!large) {
            uint64_t *pde = walk_to(root, gpa + off, false, 1u);
            if (pde != NULL && present(*pde) && (*pde & EPT_LARGE)) {
                ept_unmap(root, gpa, off);
                return -EEXIST;
            }
        }
        uint64_t *e = walk_to(root, gpa + off, true, large ? 1u : 0u);
        if (e == NULL) {
            ept_unmap(root, gpa, off);
            return -ENOMEM;
        }
        if (present(*e)) {
            ept_unmap(root, gpa, off);
            return -EEXIST;
        }
        *e = (hpa + off) | flags | (large ? EPT_LARGE : 0);
        off += large ? EPT_LARGE_SIZE : PAGE_SIZE;
    }
    return 0;
}

int ept_unmap(paddr_t root, uint64_t gpa, size_t len)
{
    if ((gpa | len) & (PAGE_SIZE - 1))
        return -EINVAL;
    for (size_t off = 0; off < len;) {
        uint64_t *pde = walk_to(root, gpa + off, false, 1u);
        if (pde != NULL && present(*pde) && (*pde & EPT_LARGE)) {
            if (((gpa + off) % EPT_LARGE_SIZE) != 0 || len - off < EPT_LARGE_SIZE)
                return -EINVAL;   /* a large leaf goes as a whole or not at all */
            *pde = 0;
            off += EPT_LARGE_SIZE;
            continue;
        }
        uint64_t *e = walk_to(root, gpa + off, false, 0u);
        if (e)
            *e = 0;
        off += PAGE_SIZE;
    }
    /* Intermediate tables are kept until ept_destroy, as NPT does. */
    return 0;
}

bool ept_query(paddr_t root, uint64_t gpa, paddr_t *hpa)
{
    uint64_t *pde = walk_to(root, gpa & ~(uint64_t)(EPT_LARGE_SIZE - 1), false, 1u);
    if (pde != NULL && present(*pde) && (*pde & EPT_LARGE)) {
        if (hpa)
            *hpa = (*pde & EPT_ADDR_MASK & ~(uint64_t)(EPT_LARGE_SIZE - 1)) | (gpa & (EPT_LARGE_SIZE - 1));
        return true;
    }
    uint64_t *e = walk_to(root, gpa & ~(uint64_t)(PAGE_SIZE - 1), false, 0u);
    if (e == NULL || !present(*e))
        return false;
    if (hpa)
        *hpa = (*e & EPT_ADDR_MASK) | (gpa & (PAGE_SIZE - 1));
    return true;
}

static unsigned count_level(paddr_t table, unsigned level)
{
    unsigned n = 1;
    if (level > 0) {
        const uint64_t *t = phys_to_virt(table);
        for (unsigned i = 0; i < EPT_ENTRIES; i++)
            if (present(t[i]) && !(t[i] & EPT_LARGE))
                n += count_level(t[i] & EPT_ADDR_MASK, level - 1);
    }
    return n;
}

unsigned ept_table_pages(paddr_t root)
{
    return root ? count_level(root, 3) : 0;
}
