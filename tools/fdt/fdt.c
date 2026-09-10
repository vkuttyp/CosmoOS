/*
 * fdt.c - The flattened-device-tree writer (tools/fdt/fdt.h).
 *
 * Compiled into vmctl (the owner hands a running guest a blob) and into
 * the host tool mkdtb (the build puts a blob in the boot archive for the
 * kernel's own tests), so one piece of code describes the machine in both
 * places -- and describes it from cosmo/hv_machine.h, which is also what
 * the kernel implements.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <uapi/cosmo/hv_machine.h>
#include "fdt.h"

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static int emit(struct fdt_writer *w, const void *data, size_t len)
{
    if (w->err)
        return w->err;
    if (w->pos + len > w->cap) {
        w->err = -ENOSPC;
        return w->err;
    }
    memcpy(w->buf + w->pos, data, len);
    w->pos += len;
    return 0;
}

static int emit32(struct fdt_writer *w, uint32_t v)
{
    uint8_t b[4];
    put32(b, v);
    return emit(w, b, 4);
}

static int pad4(struct fdt_writer *w)
{
    static const uint8_t z[4] = { 0, 0, 0, 0 };
    size_t rem = (4 - (w->pos & 3)) & 3;
    return rem ? emit(w, z, rem) : 0;
}

/* The strings block deduplicates: a name used by twenty properties is
 * stored once. Linear search; the block is small. */
static int string_off(struct fdt_writer *w, const char *name)
{
    size_t n = strlen(name) + 1;
    for (size_t i = 0; i < w->strings_len;) {
        size_t l = strlen(w->strings + i) + 1;
        if (l == n && memcmp(w->strings + i, name, n) == 0)
            return (int)i;
        i += l;
    }
    if (w->strings_len + n > sizeof(w->strings)) {
        w->err = -ENOSPC;
        return -1;
    }
    memcpy(w->strings + w->strings_len, name, n);
    int off = (int)w->strings_len;
    w->strings_len += n;
    return off;
}

int fdt_begin(struct fdt_writer *w, void *buf, size_t cap)
{
    memset(w, 0, sizeof(*w));
    w->buf = buf;
    w->cap = cap;
    /* Header (40 bytes), then one empty reservation entry (16 bytes) at
     * 40, which is 8-aligned as the reservation block must be; then the
     * structure block at 56, 4-aligned as it must be. */
    if (cap < 56) {
        w->err = -ENOSPC;
        return w->err;
    }
    memset(buf, 0, 56);
    w->struct_off = 56;
    w->pos = 56;
    return 0;
}

int fdt_begin_node(struct fdt_writer *w, const char *name)
{
    if (emit32(w, FDT_BEGIN_NODE))
        return w->err;
    if (emit(w, name, strlen(name) + 1))
        return w->err;
    if (pad4(w))
        return w->err;
    w->depth++;
    return 0;
}

int fdt_prop(struct fdt_writer *w, const char *name, const void *val, uint32_t len)
{
    if (w->depth == 0) {
        w->err = -EINVAL;   /* a property outside any node */
        return w->err;
    }
    int off = string_off(w, name);
    if (off < 0)
        return w->err;
    if (emit32(w, FDT_PROP) || emit32(w, len) || emit32(w, (uint32_t)off))
        return w->err;
    if (len && emit(w, val, len))
        return w->err;
    return pad4(w);
}

int fdt_prop_u32(struct fdt_writer *w, const char *name, uint32_t v)
{
    uint8_t b[4];
    put32(b, v);
    return fdt_prop(w, name, b, 4);
}

int fdt_prop_u64(struct fdt_writer *w, const char *name, uint64_t v)
{
    uint8_t b[8];
    put32(b, (uint32_t)(v >> 32));
    put32(b + 4, (uint32_t)v);
    return fdt_prop(w, name, b, 8);
}

int fdt_prop_cells(struct fdt_writer *w, const char *name, const uint32_t *cells, unsigned n)
{
    uint8_t b[64];
    if (n * 4 > sizeof(b)) {
        w->err = -EINVAL;
        return w->err;
    }
    for (unsigned i = 0; i < n; i++)
        put32(b + i * 4, cells[i]);
    return fdt_prop(w, name, b, n * 4);
}

int fdt_prop_str(struct fdt_writer *w, const char *name, const char *s)
{
    return fdt_prop(w, name, s, (uint32_t)strlen(s) + 1);
}

int fdt_prop_strs(struct fdt_writer *w, const char *name, const char *const *s, unsigned n)
{
    char b[256];
    size_t len = 0;
    for (unsigned i = 0; i < n; i++) {
        size_t l = strlen(s[i]) + 1;
        if (len + l > sizeof(b)) {
            w->err = -EINVAL;
            return w->err;
        }
        memcpy(b + len, s[i], l);
        len += l;
    }
    return fdt_prop(w, name, b, (uint32_t)len);
}

int fdt_prop_empty(struct fdt_writer *w, const char *name)
{
    return fdt_prop(w, name, NULL, 0);
}

int fdt_end_node(struct fdt_writer *w)
{
    if (w->depth == 0) {
        w->err = -EINVAL;
        return w->err;
    }
    w->depth--;
    return emit32(w, FDT_END_NODE);
}

int fdt_finish(struct fdt_writer *w, size_t *size)
{
    if (w->err)
        return w->err;
    if (w->depth != 0) {
        w->err = -EINVAL;   /* a node left open */
        return w->err;
    }
    if (emit32(w, FDT_END))
        return w->err;
    size_t struct_size = w->pos - w->struct_off;
    size_t strings_off = w->pos;
    if (emit(w, w->strings, w->strings_len))
        return w->err;
    size_t total = w->pos;
    uint8_t *h = w->buf;
    put32(h + 0, FDT_MAGIC);
    put32(h + 4, (uint32_t)total);
    put32(h + 8, (uint32_t)w->struct_off);
    put32(h + 12, (uint32_t)strings_off);
    put32(h + 16, 40);                        /* off_mem_rsvmap */
    put32(h + 20, FDT_VERSION);
    put32(h + 24, FDT_LAST_COMPAT);
    put32(h + 28, 0);                         /* boot_cpuid_phys */
    put32(h + 32, (uint32_t)w->strings_len);
    put32(h + 36, (uint32_t)struct_size);
    *size = total;
    return 0;
}

/* --- the machine ------------------------------------------------------ */

#define PHANDLE_GIC 1u
#define PHANDLE_CLK 2u

/* A GIC interrupt specifier: <type number flags>, PPIs numbered from 16
 * and SPIs from 32, flags 4 = level-triggered, active high. */
#define IRQ_SPI  0u
#define IRQ_PPI  1u
#define IRQ_LEVEL_HIGH 4u

int fdt_cosmo_virt(void *buf, size_t cap, unsigned nr_cpus, uint64_t ram_base, uint64_t ram_bytes,
                   const char *bootargs, size_t *size)
{
    struct fdt_writer w;
    char name[48];
    if (nr_cpus == 0 || nr_cpus > COSMO_HVM_GICR_FRAMES)
        return -EINVAL;
    fdt_begin(&w, buf, cap);

    fdt_begin_node(&w, "");
    fdt_prop_str(&w, "compatible", "cosmo,virt");
    fdt_prop_u32(&w, "#address-cells", 2);
    fdt_prop_u32(&w, "#size-cells", 2);
    fdt_prop_u32(&w, "interrupt-parent", PHANDLE_GIC);

    fdt_begin_node(&w, "chosen");
    fdt_prop_str(&w, "stdout-path", "/pl011@9000000");
    if (bootargs && *bootargs)
        fdt_prop_str(&w, "bootargs", bootargs);
    fdt_end_node(&w);

    fdt_begin_node(&w, "aliases");
    fdt_prop_str(&w, "serial0", "/pl011@9000000");
    fdt_end_node(&w);

    snprintf(name, sizeof(name), "memory@%llx", (unsigned long long)ram_base);
    fdt_begin_node(&w, name);
    fdt_prop_str(&w, "device_type", "memory");
    {
        uint32_t reg[4] = { (uint32_t)(ram_base >> 32), (uint32_t)ram_base, (uint32_t)(ram_bytes >> 32),
                            (uint32_t)ram_bytes };
        fdt_prop_cells(&w, "reg", reg, 4);
    }
    fdt_end_node(&w);

    fdt_begin_node(&w, "cpus");
    fdt_prop_u32(&w, "#address-cells", 1);
    fdt_prop_u32(&w, "#size-cells", 0);
    for (unsigned i = 0; i < nr_cpus; i++) {
        snprintf(name, sizeof(name), "cpu@%u", i);
        fdt_begin_node(&w, name);
        fdt_prop_str(&w, "device_type", "cpu");
        fdt_prop_str(&w, "compatible", "arm,armv8");
        fdt_prop_u32(&w, "reg", i);            /* MPIDR Aff0 = i: what VMPIDR_EL2 says */
        fdt_prop_str(&w, "enable-method", "psci");
        fdt_end_node(&w);
    }
    fdt_end_node(&w);

    fdt_begin_node(&w, "psci");
    {
        static const char *const compat[] = { "arm,psci-1.0", "arm,psci-0.2" };
        fdt_prop_strs(&w, "compatible", compat, 2);
    }
    fdt_prop_str(&w, "method", "hvc");
    fdt_end_node(&w);

    fdt_begin_node(&w, "timer");
    fdt_prop_str(&w, "compatible", "arm,armv8-timer");
    {
        uint32_t irqs[12] = { IRQ_PPI, COSMO_HVM_TIMER_PPI_SEC - 16u,  IRQ_LEVEL_HIGH,
                              IRQ_PPI, COSMO_HVM_TIMER_PPI_PHYS - 16u, IRQ_LEVEL_HIGH,
                              IRQ_PPI, COSMO_HVM_TIMER_PPI_VIRT - 16u, IRQ_LEVEL_HIGH,
                              IRQ_PPI, COSMO_HVM_TIMER_PPI_HYP - 16u,  IRQ_LEVEL_HIGH };
        fdt_prop_cells(&w, "interrupts", irqs, 12);
    }
    fdt_end_node(&w);

    snprintf(name, sizeof(name), "intc@%llx", (unsigned long long)COSMO_HVM_GICD_BASE);
    fdt_begin_node(&w, name);
    fdt_prop_str(&w, "compatible", "arm,gic-v3");
    fdt_prop_u32(&w, "#interrupt-cells", 3);
    fdt_prop_u32(&w, "#address-cells", 2);
    fdt_prop_u32(&w, "#size-cells", 2);
    fdt_prop_empty(&w, "interrupt-controller");
    {
        uint64_t gicr_len = COSMO_HVM_GICR_STRIDE * nr_cpus;
        uint32_t reg[8] = { (uint32_t)(COSMO_HVM_GICD_BASE >> 32), (uint32_t)COSMO_HVM_GICD_BASE,
                            (uint32_t)(COSMO_HVM_GICD_SIZE >> 32), (uint32_t)COSMO_HVM_GICD_SIZE,
                            (uint32_t)(COSMO_HVM_GICR_BASE >> 32), (uint32_t)COSMO_HVM_GICR_BASE,
                            (uint32_t)(gicr_len >> 32),            (uint32_t)gicr_len };
        fdt_prop_cells(&w, "reg", reg, 8);
    }
    fdt_prop_u32(&w, "phandle", PHANDLE_GIC);
    fdt_end_node(&w);

    fdt_begin_node(&w, "apb-pclk");
    fdt_prop_str(&w, "compatible", "fixed-clock");
    fdt_prop_u32(&w, "#clock-cells", 0);
    fdt_prop_u32(&w, "clock-frequency", COSMO_HVM_UART_CLOCK_HZ);
    fdt_prop_str(&w, "clock-output-names", "clk24mhz");
    fdt_prop_u32(&w, "phandle", PHANDLE_CLK);
    fdt_end_node(&w);

    snprintf(name, sizeof(name), "pl011@%llx", (unsigned long long)COSMO_HVM_UART_BASE);
    fdt_begin_node(&w, name);
    {
        static const char *const compat[] = { "arm,pl011", "arm,primecell" };
        fdt_prop_strs(&w, "compatible", compat, 2);
        uint32_t reg[4] = { (uint32_t)(COSMO_HVM_UART_BASE >> 32), (uint32_t)COSMO_HVM_UART_BASE,
                            (uint32_t)(COSMO_HVM_UART_SIZE >> 32), (uint32_t)COSMO_HVM_UART_SIZE };
        fdt_prop_cells(&w, "reg", reg, 4);
        uint32_t irq[3] = { IRQ_SPI, COSMO_HVM_UART_INTID - 32u, IRQ_LEVEL_HIGH };
        fdt_prop_cells(&w, "interrupts", irq, 3);
        uint32_t clocks[2] = { PHANDLE_CLK, PHANDLE_CLK };
        fdt_prop_cells(&w, "clocks", clocks, 2);
        static const char *const clknames[] = { "uartclk", "apb_pclk" };
        fdt_prop_strs(&w, "clock-names", clknames, 2);
        fdt_prop_u32(&w, "current-speed", 115200);
    }
    fdt_end_node(&w);

    snprintf(name, sizeof(name), "virtio_mmio@%llx", (unsigned long long)COSMO_HVM_VIRTIO0_BASE);
    fdt_begin_node(&w, name);
    {
        fdt_prop_str(&w, "compatible", "virtio,mmio");
        uint32_t reg[4] = { (uint32_t)(COSMO_HVM_VIRTIO0_BASE >> 32), (uint32_t)COSMO_HVM_VIRTIO0_BASE,
                            (uint32_t)(COSMO_HVM_VIRTIO0_SIZE >> 32), (uint32_t)COSMO_HVM_VIRTIO0_SIZE };
        fdt_prop_cells(&w, "reg", reg, 4);
        uint32_t irq[3] = { IRQ_SPI, COSMO_HVM_VIRTIO0_INTID - 32u, IRQ_LEVEL_HIGH };
        fdt_prop_cells(&w, "interrupts", irq, 3);
    }
    fdt_end_node(&w);

    snprintf(name, sizeof(name), "virtio_mmio@%llx", (unsigned long long)COSMO_HVM_VIRTIO1_BASE);
    fdt_begin_node(&w, name);
    {
        fdt_prop_str(&w, "compatible", "virtio,mmio");
        uint32_t reg[4] = { (uint32_t)(COSMO_HVM_VIRTIO1_BASE >> 32), (uint32_t)COSMO_HVM_VIRTIO1_BASE,
                            (uint32_t)(COSMO_HVM_VIRTIO1_SIZE >> 32), (uint32_t)COSMO_HVM_VIRTIO1_SIZE };
        fdt_prop_cells(&w, "reg", reg, 4);
        uint32_t irq[3] = { IRQ_SPI, COSMO_HVM_VIRTIO1_INTID - 32u, IRQ_LEVEL_HIGH };
        fdt_prop_cells(&w, "interrupts", irq, 3);
    }
    fdt_end_node(&w);

    fdt_end_node(&w);   /* / */
    return fdt_finish(&w, size);
}
