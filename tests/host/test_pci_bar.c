/*
 * test_pci_bar.c - The size a PCI memory BAR decodes
 * (kernel/include/kernel/pci_bar.h), on the host.
 */

#include "harness.h"

#include <kernel/pci_bar.h>

/* A BAR's sizing mask as decode_bars assembles it: the low half with the
 * flag bits cleared, and the upper half read back (64-bit) or all ones. */
static uint64_t mask64(uint32_t lo, uint32_t hi)
{
    return ((uint64_t)hi << 32) | (lo & ~0xfu);
}

static void test_writable_upper_half(void)
{
    EXPECT(pci_bar_mem_size(mask64(0xfffff000u, 0xffffffffu)) == 0x1000);       /* 32-bit, 4 KiB */
    EXPECT(pci_bar_mem_size(mask64(0xffffc00cu, 0xffffffffu)) == 0x4000);       /* 64-bit, 16 KiB */
    EXPECT(pci_bar_mem_size(mask64(0x00000000u, 0xfffffffeu)) == 0x200000000);  /* 64-bit, 8 GiB */
}

/* Parallels' NVMe controller: BAR0 0x14000004, low mask 0xffffc004, the
 * upper half hard-wired to zero. ~mask + 1 made it about 16 EiB. */
static void test_read_only_upper_half(void)
{
    EXPECT(pci_bar_mem_size(mask64(0xffffc004u, 0x00000000u)) == 0x4000);
    EXPECT(pci_bar_mem_size(mask64(0xfff00004u, 0x00000000u)) == 0x100000);
}

static void test_no_bar(void)
{
    EXPECT(pci_bar_mem_size(0) == 0);
}

static const struct host_test tests[] = {
    { "a BAR with a writable upper half", test_writable_upper_half },
    { "a 64-bit BAR whose upper half reads zero", test_read_only_upper_half },
    { "no BAR", test_no_bar },
};

int main(void)
{
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
