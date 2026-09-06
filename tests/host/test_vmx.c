/*
 * test_vmx.c - Host test of the VMX backend's pure parts
 * (docs/kernel-services/virtualization/testing.md): control fixing
 * against capability MSR values, the I/O exit qualification decoder, the
 * EPT pointer and entry encodings, and the EPT builder over the harness
 * arena. ASan and UBSan.
 *
 * These are the only parts of the backend any test in this repository
 * can execute: QEMU's TCG has no VMX and the development host is not
 * Intel, so VMXON, the VMCS writes and VMLAUNCH are compiled and
 * reviewed but never run here.
 */

#include "harness.h"

#include <kernel/errno.h>
#include <kernel/page.h>
#include <kernel/pmm.h>

#include <arch/hv.h>
#include <x86/vmx.h>

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

/* A capability MSR: allowed-0 in the low half (bits that must be 1),
 * allowed-1 in the high half (bits that may be 1). */
static uint64_t cap(uint32_t must_be_one, uint32_t may_be_one)
{
    return (uint64_t)must_be_one | ((uint64_t)(may_be_one | must_be_one) << 32);
}

/* The leaf entry for `gpa`, and whether it is a 2 MiB one. */
static uint64_t leaf_of(paddr_t root, uint64_t gpa, bool *large)
{
    paddr_t table = root;
    for (unsigned level = 3; level > 0; level--) {
        const uint64_t *t = phys_to_virt(table);
        uint64_t e = t[(gpa >> (12 + 9 * level)) & 0x1FF];
        if (!(e & (EPT_READ | EPT_WRITE | EPT_EXEC)))
            return 0;
        if (e & EPT_LARGE) {
            if (large)
                *large = true;
            return e;
        }
        table = e & EPT_ADDR_MASK;
    }
    if (large)
        *large = false;
    const uint64_t *pt = phys_to_virt(table);
    return pt[(gpa >> 12) & 0x1FF];
}

int main(void)
{
    host_arena_init(64u << 20);

    /* --- control fixing --- */
    /* Nothing required, everything permitted: the wanted bits survive. */
    CHECK(vmx_fix_ctls(cap(0, 0xFFFFFFFFu), CPU_HLT_EXITING) == CPU_HLT_EXITING);
    /* A required bit is added even when the caller did not ask. */
    CHECK(vmx_fix_ctls(cap(CPU_INVLPG_EXITING, 0xFFFFFFFFu), CPU_HLT_EXITING) ==
          (CPU_HLT_EXITING | CPU_INVLPG_EXITING));
    /* A forbidden bit is dropped, and the caller can tell. */
    CHECK(vmx_fix_ctls(cap(0, CPU_HLT_EXITING), CPU_HLT_EXITING | CPU_RDTSC_EXITING) == CPU_HLT_EXITING);
    CHECK(!vmx_ctls_ok(cap(0, CPU_HLT_EXITING), CPU_HLT_EXITING | CPU_RDTSC_EXITING));
    CHECK(vmx_ctls_ok(cap(0, CPU_HLT_EXITING | CPU_RDTSC_EXITING), CPU_HLT_EXITING | CPU_RDTSC_EXITING));
    /* The reserved-one bits real CPUs report (bits 1, 4-6, 8, 13-16, 26
     * of the primary controls) are honoured without being asked for. */
    CHECK((vmx_fix_ctls(cap(0x0401E172u, 0xFFFFFFFFu), 0) & 0x0401E172u) == 0x0401E172u);

    /* --- the I/O exit qualification (SDM table 27-5) --- */
    struct vmx_io io = vmx_decode_io((0x3F8ull << 16) | 0u);        /* 1 byte, OUT */
    CHECK(io.port == 0x3F8 && io.size == 1 && !io.in && !io.string && !io.rep);
    io = vmx_decode_io((0x80ull << 16) | 1u | (1u << 3));           /* 2 bytes, IN */
    CHECK(io.port == 0x80 && io.size == 2 && io.in);
    io = vmx_decode_io((0xE9ull << 16) | 3u | (1u << 4) | (1u << 5));   /* 4 bytes, REP OUTS */
    CHECK(io.port == 0xE9 && io.size == 4 && io.string && io.rep && !io.in);

    /* --- the EPT pointer --- */
    uint64_t eptp = vmx_eptp(0x123000, false);
    CHECK((eptp & EPT_ADDR_MASK) == 0x123000);
    CHECK((eptp & 7) == 6);                       /* write-back */
    CHECK(((eptp >> 3) & 7) == 3);                /* four levels */
    CHECK((eptp & (1ull << 6)) == 0);             /* no accessed/dirty bits */
    CHECK((vmx_eptp(0x123000, true) & (1ull << 6)) != 0);

    /* --- the EPT builder --- */
    uint64_t free0 = host_arena_free_pages();
    paddr_t root = ept_create();
    CHECK(root != 0 && (root & 0xFFF) == 0);
    CHECK(ept_table_pages(root) == 1);
    CHECK(!ept_query(root, 0, NULL));
    CHECK(ept_map(root, 0x1000, 0x40000, 0x3000, HV_MAP_RWX) == 0);
    CHECK(ept_table_pages(root) == 4);            /* root + PDPT + PD + PT */
    paddr_t hpa;
    CHECK(ept_query(root, 0x1000, &hpa) && hpa == 0x40000);
    CHECK(ept_query(root, 0x2010, &hpa) && hpa == 0x41010);
    CHECK(ept_query(root, 0x3FFF, &hpa) && hpa == 0x42FFF);
    CHECK(!ept_query(root, 0x4000, &hpa));
    CHECK(!ept_query(root, 0x0, &hpa));

    /* permissions are the leaf's, and every leaf carries a memory type */
    uint64_t leaf = leaf_of(root, 0x1000, NULL);
    CHECK((leaf & (EPT_READ | EPT_WRITE | EPT_EXEC)) == (EPT_READ | EPT_WRITE | EPT_EXEC));
    CHECK((leaf & (7ull << 3)) == EPT_MEMTYPE_WB);
    CHECK(ept_map(root, 0x10000, 0x50000, 0x1000, HV_MAP_READ) == 0);
    leaf = leaf_of(root, 0x10000, NULL);
    CHECK((leaf & EPT_READ) && !(leaf & EPT_WRITE) && !(leaf & EPT_EXEC));
    CHECK(ept_map(root, 0x11000, 0x51000, 0x1000, HV_MAP_READ | HV_MAP_EXEC) == 0);
    leaf = leaf_of(root, 0x11000, NULL);
    CHECK((leaf & EPT_READ) && !(leaf & EPT_WRITE) && (leaf & EPT_EXEC));

    /* refusals */
    CHECK(ept_map(root, 0x2000, 0x60000, 0x1000, HV_MAP_RWX) == -EEXIST);
    CHECK(ept_map(root, 0x2800, 0x60000, 0x1000, HV_MAP_RWX) == -EINVAL);   /* alignment */
    CHECK(ept_map(root, 0x20000, 0x60000, 0x1800, HV_MAP_RWX) == -EINVAL);
    CHECK(ept_map(root, 0x20000, 0x60000, 0x1000, 0) == -EINVAL);           /* no permission */
    CHECK(ept_map(root, 0x20000, 0x60000, 0x1000, 0x10u) == -EINVAL);
    CHECK(ept_query(root, 0x2000, &hpa) && hpa == 0x41000);                 /* untouched */

    /* a partially failing map rolls back */
    CHECK(ept_map(root, 0x5000, 0x70000, 0x3000, HV_MAP_RWX) == 0);
    CHECK(ept_map(root, 0x4000, 0x80000, 0x3000, HV_MAP_RWX) == -EEXIST);
    CHECK(!ept_query(root, 0x4000, NULL));
    CHECK(ept_query(root, 0x5000, &hpa) && hpa == 0x70000);

    /* 2 MiB leaves */
    unsigned before = ept_table_pages(root);
    CHECK(ept_map(root, 0x40000000ull, 0x40000000ull, 0x200000, HV_MAP_RWX) == 0);
    bool large = false;
    leaf = leaf_of(root, 0x40000000ull, &large);
    CHECK(large && (leaf & EPT_LARGE));
    CHECK(ept_table_pages(root) == before + 1);                             /* a PD, and no PT */
    CHECK(ept_query(root, 0x401FF123ull, &hpa) && hpa == 0x401FF123ull);
    CHECK(!ept_query(root, 0x40200000ull, &hpa));
    CHECK(ept_map(root, 0x40100000ull, 0x90000, 0x1000, HV_MAP_RWX) == -EEXIST);
    CHECK(ept_unmap(root, 0x40100000ull, 0x1000) == -EINVAL);               /* not split */
    CHECK(ept_unmap(root, 0x40000000ull, 0x200000) == 0);
    CHECK(!ept_query(root, 0x40000000ull, &hpa));

    /* unmap and destroy */
    CHECK(ept_unmap(root, 0x1000, 0x1000) == 0);
    CHECK(!ept_query(root, 0x1000, NULL) && ept_query(root, 0x2000, NULL));
    CHECK(ept_unmap(root, 0x1234, 0x1000) == -EINVAL);
    CHECK(ept_unmap(root, 0x90000000ull, 0x1000) == 0);                     /* never mapped */
    ept_destroy(root);
    CHECK(host_arena_free_pages() == free0);
    ept_destroy(0);

    host_arena_destroy();
    if (g_failures) {
        printf("vmx                           FAIL (%d)\n", g_failures);
        return 1;
    }
    printf("vmx                           ok\n");
    return 0;
}
