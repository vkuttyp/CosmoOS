/*
 * test_hv.c - Host test of the virtualization backend's pure parts
 * (docs/kernel-services/virtualization/testing.md): the nested page
 * table builder over the harness arena, the IOIO decoder, the VMCB and
 * UAPI layouts. ASan and UBSan.
 */

#include "harness.h"

#include <kernel/errno.h>
#include <kernel/page.h>
#include <kernel/pmm.h>
#include <uapi/cosmo/syscall.h>

#include <arch/hv.h>
#include <x86/hvseg.h>
#include <x86/paging.h>
#include <x86/svm.h>

#include <stdio.h>
#include <string.h>

static int g_failures;
#define CHECK(c)                                                                          \
    do {                                                                                  \
        if (!(c)) {                                                                       \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                         \
            g_failures++;                                                                 \
        }                                                                                 \
    } while (0)

/* The leaf entry for `gpa`, and whether it is a 2 MiB one. 0 when absent. */
static uint64_t leaf_of(paddr_t root, uint64_t gpa, bool *large)
{
    paddr_t table = root;
    for (unsigned level = 3; level > 0; level--) {
        const uint64_t *t = phys_to_virt(table);
        uint64_t e = t[(gpa >> (12 + 9 * level)) & 0x1FF];
        if (!(e & PTE_P))
            return 0;
        if (e & PTE_PS) {
            if (large)
                *large = true;
            return e;
        }
        table = e & PTE_ADDR_MASK;
    }
    if (large)
        *large = false;
    const uint64_t *pt = phys_to_virt(table);
    return pt[(gpa >> 12) & 0x1FF];
}

static bool entry_has_user_bit(paddr_t root, uint64_t gpa)
{
    /* Walk the table by hand: every level must carry P|RW|US. */
    paddr_t table = root;
    for (unsigned level = 3; level > 0; level--) {
        const uint64_t *t = phys_to_virt(table);
        uint64_t e = t[(gpa >> (12 + 9 * level)) & 0x1FF];
        if ((e & (PTE_P | PTE_RW | PTE_US)) != (PTE_P | PTE_RW | PTE_US))
            return false;
        table = e & PTE_ADDR_MASK;
    }
    const uint64_t *pt = phys_to_virt(table);
    uint64_t leaf = pt[(gpa >> 12) & 0x1FF];
    return (leaf & (PTE_P | PTE_RW | PTE_US)) == (PTE_P | PTE_RW | PTE_US);
}

int main(void)
{
    host_arena_init(64u << 20);

    /* layouts */
    CHECK(sizeof(struct cosmo_vcpu_regs) == 448);
    CHECK(sizeof(struct cosmo_vm_exit) == 64);
    CHECK(sizeof(struct cosmo_vcpu_seg) == 16);
    CHECK(sizeof(struct vmcb) == 4096);
    CHECK(offsetof(struct vmcb, control.exitcode) == 0x70);
    CHECK(offsetof(struct vmcb, save.rip) == 0x578);

    /* IOIO decoder */
    struct svm_ioio io = svm_decode_ioio((0x80ull << 16) | SVM_IOIO_SZ16);
    CHECK(io.port == 0x80 && io.size == 2 && !io.in && !io.string && !io.rep);
    io = svm_decode_ioio((0xE9ull << 16) | SVM_IOIO_SZ8 | SVM_IOIO_IN);
    CHECK(io.port == 0xE9 && io.size == 1 && io.in);
    io = svm_decode_ioio((0x3F8ull << 16) | SVM_IOIO_SZ32 | SVM_IOIO_STR | SVM_IOIO_REP);
    CHECK(io.port == 0x3F8 && io.size == 4 && io.string && io.rep && !io.in);

    /* nested page tables */
    uint64_t free0 = host_arena_free_pages();
    paddr_t root = npt_create();
    CHECK(root != 0 && (root & 0xFFF) == 0);
    CHECK(npt_table_pages(root) == 1);
    CHECK(!npt_query(root, 0, NULL));
    CHECK(npt_map(root, 0x1000, 0x40000, 0x3000, HV_MAP_RWX) == 0);      /* three pages */
    CHECK(npt_table_pages(root) == 4);                        /* root + PDPT + PD + PT */
    paddr_t hpa;
    CHECK(npt_query(root, 0x1000, &hpa) && hpa == 0x40000);
    CHECK(npt_query(root, 0x2010, &hpa) && hpa == 0x41010);
    CHECK(npt_query(root, 0x3FFF, &hpa) && hpa == 0x42FFF);
    CHECK(!npt_query(root, 0x4000, &hpa));
    CHECK(!npt_query(root, 0x0, &hpa));
    CHECK(entry_has_user_bit(root, 0x1000));
    CHECK(entry_has_user_bit(root, 0x3000));
    /* a distant mapping in the same PML4 slot needs a new PD and PT */
    CHECK(npt_map(root, 0x80000000ull, 0x100000, 0x1000, HV_MAP_RWX) == 0);
    CHECK(npt_table_pages(root) == 6);                        /* shares the PDPT: + PD + PT */
    CHECK(npt_query(root, 0x80000000ull, &hpa) && hpa == 0x100000);
    CHECK(entry_has_user_bit(root, 0x80000000ull));
    /* refusals */
    CHECK(npt_map(root, 0x2000, 0x50000, 0x1000, HV_MAP_RWX) == -EEXIST);   /* already mapped */
    CHECK(npt_map(root, 0x2800, 0x50000, 0x1000, HV_MAP_RWX) == -EINVAL);   /* alignment */
    CHECK(npt_map(root, 0x5000, 0x50000, 0x1800, HV_MAP_RWX) == -EINVAL);
    CHECK(npt_map(root, 0x5000, 0x50000, 0x1000, 0) == -EINVAL);            /* no permission */
    CHECK(npt_map(root, 0x5000, 0x50000, 0x1000, 0x10u) == -EINVAL);        /* not a permission */
    CHECK(npt_query(root, 0x2000, &hpa) && hpa == 0x41000);     /* untouched by the failed map */
    /* a partially failing map rolls back */
    CHECK(npt_map(root, 0x5000, 0x60000, 0x3000, HV_MAP_RWX) == 0);         /* 0x5000..0x7FFF */
    CHECK(npt_map(root, 0x4000, 0x70000, 0x3000, HV_MAP_RWX) == -EEXIST);   /* 0x4000 ok, 0x5000 collides */
    CHECK(!npt_query(root, 0x4000, NULL));                      /* rolled back */
    CHECK(npt_query(root, 0x5000, &hpa) && hpa == 0x60000);
    /* unmap */
    CHECK(npt_unmap(root, 0x1000, 0x1000) == 0);
    CHECK(!npt_query(root, 0x1000, NULL) && npt_query(root, 0x2000, NULL));
    CHECK(npt_unmap(root, 0x1234, 0x1000) == -EINVAL);
    CHECK(npt_unmap(root, 0x90000000ull, 0x1000) == 0);         /* never mapped: no-op */
    /* permissions reach the leaf, and only the leaf */
    CHECK(npt_map(root, 0x20000, 0x200000, 0x1000, HV_MAP_READ) == 0);
    CHECK(leaf_of(root, 0x20000, NULL) != 0);
    uint64_t leaf = leaf_of(root, 0x20000, NULL);
    CHECK((leaf & PTE_P) && (leaf & PTE_US) && !(leaf & PTE_RW) && (leaf & PTE_NX));
    CHECK(npt_map(root, 0x21000, 0x201000, 0x1000, HV_MAP_READ | HV_MAP_EXEC) == 0);
    leaf = leaf_of(root, 0x21000, NULL);
    CHECK(!(leaf & PTE_RW) && !(leaf & PTE_NX));
    CHECK(npt_map(root, 0x22000, 0x202000, 0x1000, HV_MAP_READ | HV_MAP_WRITE) == 0);
    leaf = leaf_of(root, 0x22000, NULL);
    CHECK((leaf & PTE_RW) && (leaf & PTE_NX));
    CHECK(entry_has_user_bit(root, 0x22000));   /* the levels above stay permissive */

    /* 2 MiB leaves where everything is aligned: one entry, no PT */
    unsigned before = npt_table_pages(root);
    CHECK(npt_map(root, 0x40000000ull, 0x40000000ull, 0x200000, HV_MAP_RWX) == 0);
    bool large = false;
    leaf = leaf_of(root, 0x40000000ull, &large);
    CHECK(large && (leaf & PTE_PS) && (leaf & PTE_P));
    CHECK(npt_table_pages(root) == before + 1);                  /* one new PD, and no PT at all */
    CHECK(npt_query(root, 0x40000000ull, &hpa) && hpa == 0x40000000ull);
    CHECK(npt_query(root, 0x401FF123ull, &hpa) && hpa == 0x401FF123ull);   /* inside the leaf */
    CHECK(!npt_query(root, 0x40200000ull, &hpa));                /* just past it */
    CHECK(npt_map(root, 0x40100000ull, 0x300000, 0x1000, HV_MAP_RWX) == -EEXIST);   /* covered */
    CHECK(npt_unmap(root, 0x40100000ull, 0x1000) == -EINVAL);    /* a large leaf is not split */
    CHECK(npt_unmap(root, 0x40000000ull, 0x200000) == 0);
    CHECK(!npt_query(root, 0x40000000ull, &hpa));
    /* an unaligned length falls back to 4 KiB pages */
    CHECK(npt_map(root, 0x40000000ull, 0x40000000ull, 0x1000, HV_MAP_RWX) == 0);
    leaf = leaf_of(root, 0x40000000ull, &large);
    CHECK(!large && !(leaf & PTE_PS));
    CHECK(npt_unmap(root, 0x40000000ull, 0x1000) == 0);

    /* destroy returns every table page */
    npt_destroy(root);
    CHECK(host_arena_free_pages() == free0);
    npt_destroy(0);                                              /* tolerated */

    /* segment attributes: neutral in, each backend's packing out */
    uint16_t code64 = COSMO_SEG_P | COSMO_SEG_S | 0xB | COSMO_SEG_L | COSMO_SEG_G;
    CHECK(hv_seg_to_svm(code64) == 0x0A9B);          /* AVL/L/DB/G move to bits 8-11 */
    CHECK(hv_seg_from_svm(hv_seg_to_svm(code64), 0x8) == code64);
    CHECK(hv_seg_to_vmx(code64) == 0xA09B);          /* the same layout, unusable at bit 16 */
    CHECK(hv_seg_from_vmx(hv_seg_to_vmx(code64)) == code64);
    uint16_t data32 = COSMO_SEG_P | COSMO_SEG_S | 0x3 | COSMO_SEG_DB | COSMO_SEG_G;
    CHECK(hv_seg_to_svm(data32) == 0x0C93);
    CHECK(hv_seg_from_svm(0x0C93, 0x10) == data32);
    uint16_t dpl3 = (uint16_t)(COSMO_SEG_P | COSMO_SEG_S | 0x3 | (3u << COSMO_SEG_DPL_SHIFT));
    CHECK(((hv_seg_to_svm(dpl3) & COSMO_SEG_DPL) >> COSMO_SEG_DPL_SHIFT) == 3);
    CHECK(hv_seg_from_vmx(hv_seg_to_vmx(dpl3)) == dpl3);
    /* unusable: SVM says "not present", VMX has its own bit */
    uint16_t unusable = COSMO_SEG_UNUSABLE;
    CHECK((hv_seg_to_svm(unusable) & COSMO_SEG_P) == 0);
    CHECK(hv_seg_from_svm(0, 0) == COSMO_SEG_UNUSABLE);       /* not present, null selector */
    CHECK(hv_seg_from_svm(0, 0x10) == 0);                     /* not present, but loaded */
    CHECK(hv_seg_to_vmx(unusable) == (1u << 16));
    CHECK(hv_seg_from_vmx(1u << 16) == COSMO_SEG_UNUSABLE);
    /* validity */
    CHECK(hv_seg_attrib_valid(code64) && hv_seg_attrib_valid(unusable));
    CHECK(!hv_seg_attrib_valid(0x0200));                      /* a reserved bit */
    CHECK(!hv_seg_attrib_valid(COSMO_SEG_UNUSABLE | COSMO_SEG_P));

    host_arena_destroy();
    if (g_failures) {
        printf("hv                            FAIL (%d)\n", g_failures);
        return 1;
    }
    printf("hv                            ok\n");
    return 0;
}
