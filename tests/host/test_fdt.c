/*
 * test_fdt.c - The device-tree writer, read back
 * (docs/kernel-services/virtualization/testing.md).
 *
 * The blob the owner hands a guest has a sound header and is the size it
 * says; every node and property the machine description promises reads
 * back through the reader, and each value is the uapi constant it came
 * from -- not a literal, so this is the header's and the blob's agreement.
 */
#include "harness.h"

#include <cosmo/hv_machine.h>
#include <string.h>
#include <stdio.h>

#include "../../tools/fdt/fdt.h"

static unsigned char g_blob[16384];
static size_t g_size;

static void test_header(void)
{
    g_size = 0;
    EXPECT(fdt_cosmo_virt(g_blob, sizeof(g_blob), 2, COSMO_HVM_RAM_BASE, 8u << 20, "console=ttyAMA0", &g_size) == 0);
    EXPECT(g_size > 200 && g_size < 4096);
    EXPECT(fdt_check(g_blob, sizeof(g_blob)) == g_size);
    EXPECT(fdt_check(g_blob, g_size - 1) == 0);           /* a truncated blob is refused */
    EXPECT(fdt_be32(g_blob + 20) == FDT_VERSION);
    /* A blob that does not fit is an error, not a truncated write. */
    unsigned char small[128];
    size_t s = 0;
    EXPECT(fdt_cosmo_virt(small, sizeof(small), 2, COSMO_HVM_RAM_BASE, 8u << 20, NULL, &s) != 0);
}

static void test_nodes(void)
{
    uint32_t len = 0;
    const void *p;
    p = fdt_get_prop(g_blob, "/", "compatible", &len);
    EXPECT(p != NULL && strcmp(p, "cosmo,virt") == 0);
    p = fdt_get_prop(g_blob, "/", "#address-cells", &len);
    EXPECT(p != NULL && len == 4 && fdt_be32(p) == 2);
    p = fdt_get_prop(g_blob, "/chosen", "stdout-path", &len);
    EXPECT(p != NULL && strcmp(p, "/pl011@9000000") == 0);
    p = fdt_get_prop(g_blob, "/chosen", "bootargs", &len);
    EXPECT(p != NULL && strcmp(p, "console=ttyAMA0") == 0);
    EXPECT(fdt_count_children(g_blob, "/cpus", "cpu@") == 2);
    p = fdt_get_prop(g_blob, "/cpus/cpu@1", "reg", &len);
    EXPECT(p != NULL && fdt_be32(p) == 1);
    p = fdt_get_prop(g_blob, "/cpus/cpu@1", "enable-method", &len);
    EXPECT(p != NULL && strcmp(p, "psci") == 0);
    EXPECT(fdt_get_prop(g_blob, "/cpus/cpu@2", "reg", &len) == NULL);   /* only two */
    p = fdt_get_prop(g_blob, "/psci", "method", &len);
    EXPECT(p != NULL && strcmp(p, "hvc") == 0);
}

static void test_devices(void)
{
    uint32_t len = 0;
    const void *p;
    /* The memory node names the RAM the blob was built for. */
    p = fdt_get_prop(g_blob, "/memory@40000000", "reg", &len);
    EXPECT(p != NULL && len == 16);
    EXPECT(fdt_be64(p) == COSMO_HVM_RAM_BASE && fdt_be64((const uint8_t *)p + 8) == (8u << 20));
    /* The GIC: two ranges, the distributor and two redistributor frames. */
    p = fdt_get_prop(g_blob, "/intc@8000000", "reg", &len);
    EXPECT(p != NULL && len == 32);
    EXPECT(fdt_be64(p) == COSMO_HVM_GICD_BASE && fdt_be64((const uint8_t *)p + 8) == COSMO_HVM_GICD_SIZE);
    EXPECT(fdt_be64((const uint8_t *)p + 16) == COSMO_HVM_GICR_BASE);
    EXPECT(fdt_be64((const uint8_t *)p + 24) == COSMO_HVM_GICR_STRIDE * 2);
    p = fdt_get_prop(g_blob, "/intc@8000000", "interrupt-controller", &len);
    EXPECT(p != NULL && len == 0);
    p = fdt_get_prop(g_blob, "/intc@8000000", "phandle", &len);
    EXPECT(p != NULL);
    uint32_t gic = fdt_be32(p);
    p = fdt_get_prop(g_blob, "/", "interrupt-parent", &len);
    EXPECT(p != NULL && fdt_be32(p) == gic);
    /* The UART: where the kernel implements it, on the SPI it raises. */
    p = fdt_get_prop(g_blob, "/pl011@9000000", "reg", &len);
    EXPECT(p != NULL && len == 16 && fdt_be64(p) == COSMO_HVM_UART_BASE);
    EXPECT(fdt_be64((const uint8_t *)p + 8) == COSMO_HVM_UART_SIZE);
    p = fdt_get_prop(g_blob, "/pl011@9000000", "interrupts", &len);
    EXPECT(p != NULL && len == 12);
    EXPECT(fdt_be32(p) == 0 && fdt_be32((const uint8_t *)p + 4) == COSMO_HVM_UART_INTID - 32 &&
          fdt_be32((const uint8_t *)p + 8) == 4);
    p = fdt_get_prop(g_blob, "/pl011@9000000", "compatible", &len);
    EXPECT(p != NULL && len == strlen("arm,pl011") + 1 + strlen("arm,primecell") + 1);
    EXPECT(strcmp(p, "arm,pl011") == 0 && strcmp((const char *)p + 10, "arm,primecell") == 0);
    p = fdt_get_prop(g_blob, "/pl011@9000000", "clocks", &len);
    EXPECT(p != NULL && len == 8);
    uint32_t clk = fdt_be32(p);
    p = fdt_get_prop(g_blob, "/apb-pclk", "phandle", &len);
    EXPECT(p != NULL && fdt_be32(p) == clk);
    p = fdt_get_prop(g_blob, "/apb-pclk", "clock-frequency", &len);
    EXPECT(p != NULL && fdt_be32(p) == COSMO_HVM_UART_CLOCK_HZ);
    /* The timer: four PPIs, numbered from 16. */
    p = fdt_get_prop(g_blob, "/timer", "interrupts", &len);
    EXPECT(p != NULL && len == 48);
    EXPECT(fdt_be32((const uint8_t *)p + 4) == COSMO_HVM_TIMER_PPI_SEC - 16);
    EXPECT(fdt_be32((const uint8_t *)p + 28) == COSMO_HVM_TIMER_PPI_VIRT - 16);
    /* A node that is not there is not found, and neither is a property. */
    EXPECT(fdt_get_prop(g_blob, "/virtio_mmio@a000000", "reg", &len) == NULL);
    EXPECT(fdt_get_prop(g_blob, "/pl011@9000000", "dma", &len) == NULL);
}

static void test_limits(void)
{
    size_t s = 0;
    EXPECT(fdt_cosmo_virt(g_blob, sizeof(g_blob), 0, COSMO_HVM_RAM_BASE, 8u << 20, NULL, &s) != 0);
    EXPECT(fdt_cosmo_virt(g_blob, sizeof(g_blob), COSMO_HVM_GICR_FRAMES + 1, COSMO_HVM_RAM_BASE, 8u << 20, NULL, &s) != 0);
    EXPECT(fdt_cosmo_virt(g_blob, sizeof(g_blob), COSMO_HVM_GICR_FRAMES, COSMO_HVM_RAM_BASE, 64u << 20, NULL, &s) == 0);
    EXPECT(fdt_count_children(g_blob, "/cpus", "cpu@") == COSMO_HVM_GICR_FRAMES);
    /* A node left open, or a property outside any node, is refused. */
    struct fdt_writer w;
    unsigned char b[512];
    fdt_begin(&w, b, sizeof(b));
    EXPECT(fdt_prop_u32(&w, "x", 1) != 0);
    fdt_begin(&w, b, sizeof(b));
    fdt_begin_node(&w, "");
    EXPECT(fdt_finish(&w, &s) != 0);
}

static const struct host_test tests[] = {
    { "header", test_header },
    { "nodes", test_nodes },
    { "devices", test_devices },
    { "limits", test_limits },
};

int main(void)
{
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
