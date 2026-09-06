/*
 * test_hv_s2.c - Host test of the AArch64 stage-2 table builder
 * (docs/kernel-services/virtualization/testing.md). The same ground the
 * NPT and EPT tests cover, in LPAE stage-2 descriptors: mappings and
 * lookups, permissions in the leaf, 2 MiB blocks and their refusals,
 * rollback, and every table page returned at destroy. ASan and UBSan.
 */

#include "harness.h"

#include <kernel/errno.h>
#include <kernel/page.h>
#include <kernel/pmm.h>

#include <arch/hv.h>
#include <aarch64/hv_s2.h>

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

/* The leaf for `ipa`, and whether it is a 2 MiB block. 0 when absent. */
static uint64_t leaf_of(paddr_t root, uint64_t ipa, bool *block)
{
    paddr_t table = root;
    for (unsigned level = 3; level > 0; level--) {
        const uint64_t *t = phys_to_virt(table);
        uint64_t e = t[(ipa >> (12 + 9 * level)) & 0x1FF];
        if (!(e & 1))
            return 0;
        if (!(e & 2)) {            /* valid but not a table: a block */
            if (block)
                *block = true;
            return e;
        }
        table = e & S2_ADDR_MASK;
    }
    if (block)
        *block = false;
    const uint64_t *t = phys_to_virt(table);
    return t[(ipa >> 12) & 0x1FF];
}

int main(void)
{
    host_arena_init(64u << 20);

    uint64_t free0 = host_arena_free_pages();
    paddr_t root = hv_s2_create();
    CHECK(root != 0 && (root & 0xFFF) == 0);
    CHECK(hv_s2_table_pages(root) == 1);
    CHECK(!hv_s2_query(root, 0, NULL));

    CHECK(hv_s2_map(root, 0x1000, 0x40000, 0x3000, HV_MAP_RWX) == 0);
    CHECK(hv_s2_table_pages(root) == 4);
    paddr_t pa;
    CHECK(hv_s2_query(root, 0x1000, &pa) && pa == 0x40000);
    CHECK(hv_s2_query(root, 0x2010, &pa) && pa == 0x41010);
    CHECK(hv_s2_query(root, 0x3FFF, &pa) && pa == 0x42FFF);
    CHECK(!hv_s2_query(root, 0x4000, &pa));

    /* A page leaf is valid+page, carries normal write-back memory, the
     * access flag and inner-shareable, and its permissions are S2AP. */
    uint64_t leaf = leaf_of(root, 0x1000, NULL);
    CHECK((leaf & 3) == 3);                       /* valid + page */
    CHECK(((leaf >> 2) & 0xF) == 0xF);            /* MemAttr: normal WB */
    CHECK((leaf & (1ull << 10)) != 0);            /* AF */
    CHECK(((leaf >> 8) & 3) == 3);                /* SH inner */
    CHECK((leaf & (1ull << 6)) && (leaf & (1ull << 7)));   /* read and write */
    CHECK(((leaf >> 53) & 3) == 0);               /* executable */

    CHECK(hv_s2_map(root, 0x10000, 0x50000, 0x1000, HV_MAP_READ) == 0);
    leaf = leaf_of(root, 0x10000, NULL);
    CHECK((leaf & (1ull << 6)) && !(leaf & (1ull << 7)));
    CHECK(((leaf >> 53) & 3) == 2);               /* XN */
    CHECK(hv_s2_map(root, 0x11000, 0x51000, 0x1000, HV_MAP_READ | HV_MAP_EXEC) == 0);
    leaf = leaf_of(root, 0x11000, NULL);
    CHECK(((leaf >> 53) & 3) == 0 && !(leaf & (1ull << 7)));

    /* refusals and rollback */
    CHECK(hv_s2_map(root, 0x2000, 0x60000, 0x1000, HV_MAP_RWX) == -EEXIST);
    CHECK(hv_s2_map(root, 0x2800, 0x60000, 0x1000, HV_MAP_RWX) == -EINVAL);
    CHECK(hv_s2_map(root, 0x20000, 0x60000, 0x1800, HV_MAP_RWX) == -EINVAL);
    CHECK(hv_s2_map(root, 0x20000, 0x60000, 0x1000, 0) == -EINVAL);
    CHECK(hv_s2_map(root, 0x20000, 0x60000, 0x1000, 0x10u) == -EINVAL);
    CHECK(hv_s2_map(root, 0x5000, 0x70000, 0x3000, HV_MAP_RWX) == 0);
    CHECK(hv_s2_map(root, 0x4000, 0x80000, 0x3000, HV_MAP_RWX) == -EEXIST);
    CHECK(!hv_s2_query(root, 0x4000, NULL));
    CHECK(hv_s2_query(root, 0x5000, &pa) && pa == 0x70000);

    /* 2 MiB blocks */
    unsigned before = hv_s2_table_pages(root);
    CHECK(hv_s2_map(root, 0x40000000ull, 0x40000000ull, 0x200000, HV_MAP_RWX) == 0);
    bool block = false;
    leaf = leaf_of(root, 0x40000000ull, &block);
    CHECK(block && (leaf & 1) && !(leaf & 2));    /* valid, not a table */
    CHECK(hv_s2_table_pages(root) == before + 1);
    CHECK(hv_s2_query(root, 0x401FF123ull, &pa) && pa == 0x401FF123ull);
    CHECK(!hv_s2_query(root, 0x40200000ull, &pa));
    CHECK(hv_s2_map(root, 0x40100000ull, 0x90000, 0x1000, HV_MAP_RWX) == -EEXIST);
    CHECK(hv_s2_unmap(root, 0x40100000ull, 0x1000) == -EINVAL);
    CHECK(hv_s2_unmap(root, 0x40000000ull, 0x200000) == 0);
    CHECK(!hv_s2_query(root, 0x40000000ull, &pa));

    /* unmap, destroy */
    CHECK(hv_s2_unmap(root, 0x1000, 0x1000) == 0);
    CHECK(!hv_s2_query(root, 0x1000, NULL) && hv_s2_query(root, 0x2000, NULL));
    CHECK(hv_s2_unmap(root, 0x1234, 0x1000) == -EINVAL);
    CHECK(hv_s2_unmap(root, 0x90000000ull, 0x1000) == 0);
    hv_s2_destroy(root);
    CHECK(host_arena_free_pages() == free0);
    hv_s2_destroy(0);

    /* VTCR_EL2: T0SZ from the output size, four levels, 4 KiB granule. */
    uint64_t vtcr = hv_s2_vtcr(44, 4);
    CHECK((vtcr & 0x3F) == 20);                   /* 64 - 44 */
    CHECK(((vtcr >> 6) & 3) == 2);                /* SL0: start at level 1 */
    CHECK(((vtcr >> 14) & 3) == 0);               /* TG0 4 KiB */
    CHECK(((vtcr >> 16) & 7) == 4);               /* PS as given */
    CHECK((hv_s2_vtcr(48, 5) & 0x3F) == 16);

    host_arena_destroy();
    if (g_failures) {
        printf("hv-s2                         FAIL (%d)\n", g_failures);
        return 1;
    }
    printf("hv-s2                         ok\n");
    return 0;
}
