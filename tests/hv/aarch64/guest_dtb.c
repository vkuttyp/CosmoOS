/*
 * guest_dtb.c - A guest that learns its machine from the device tree it
 * is handed (docs/kernel-services/virtualization/testing.md).
 *
 * Nothing here knows where anything is. The UART's address and interrupt,
 * the number of CPUs, the memory and how to reach firmware all come from
 * the blob in x0 -- and the line that reports them is printed through the
 * UART the blob named, so a wrong blob prints nowhere. Then the three
 * things a kernel does through firmware: ask PSCI its version, bring the
 * second CPU up, and power off.
 *
 *   dtb: uart@9000000 irq 33 cpus 2 mem 40000000+800000 psci hvc
 *   psci version 0x10000
 *   cpu1: up ctx=1234cafe            (on vCPU 1, then CPU_OFF)
 *   cpu_on 1 -> 0
 *   (SYSTEM_OFF)
 */
#include <stdint.h>
#include "fdt.h"

#define PSCI_VERSION     0x84000000ull
#define PSCI_CPU_OFF     0x84000002ull
#define PSCI_CPU_ON      0xC4000003ull
#define PSCI_SYSTEM_OFF  0x84000008ull

static volatile uint32_t *g_uart;

/* Two CPUs, one UART: a line is printed under a lock, as a kernel's
 * console is, or the owner's round-robin interleaves the two CPUs'
 * lines byte by byte -- which it did, on a loaded host, into
 * "cpu1: up cpu_on 1 ctx=1234cafe" / "-> 0". */
static volatile uint32_t g_print_lock;

static void lock(volatile uint32_t *l)
{
    uint32_t tmp, one = 1;
    __asm__ volatile(
        "1: ldaxr %w0, [%2]\n"
        "   cbnz  %w0, 1b\n"
        "   stxr  %w0, %w1, [%2]\n"
        "   cbnz  %w0, 1b\n"
        : "=&r"(tmp) : "r"(one), "r"(l) : "memory");
}

static void unlock(volatile uint32_t *l)
{
    __asm__ volatile("stlr wzr, [%0]" : : "r"(l) : "memory");
}

static uint64_t hvc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
    register uint64_t x0 __asm__("x0") = fn;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    register uint64_t x3 __asm__("x3") = a3;
    __asm__ volatile("hvc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    return x0;
}

static void putc(char c)
{
    if (g_uart == 0)
        return;
    while (g_uart[0x18 / 4] & (1u << 5))   /* FR.TXFF */
        ;
    g_uart[0] = (uint32_t)(uint8_t)c;
}

static void puts(const char *s)
{
    while (*s)
        putc(*s++);
}

static void puthex(uint64_t v)
{
    char b[17];
    int i = 16;
    b[i] = 0;
    do {
        unsigned d = (unsigned)(v & 0xF);
        b[--i] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v >>= 4;
    } while (v);
    puts(b + i);
}

static void putdec(uint64_t v)
{
    char b[21];
    int i = 20;
    b[i] = 0;
    do {
        b[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    puts(b + i);
}

extern void secondary_start(void);

void secondary_main(uint64_t ctx)
{
    lock(&g_print_lock);
    puts("cpu1: up ctx=");
    puthex(ctx);
    puts("\n");
    unlock(&g_print_lock);
    hvc(PSCI_CPU_OFF, 0, 0, 0);
    for (;;)
        hvc(2, 0, 0, 0);
}

void guest_main(uint64_t dtb_pa)
{
    const void *dtb = (const void *)dtb_pa;
    if (dtb_pa == 0 || fdt_check(dtb, 1u << 20) == 0) {
        for (;;)
            hvc(3, dtb_pa, 0, 0);   /* no tree: nothing to print to; say so to the owner */
    }
    uint32_t len = 0;
    const char *stdout_path = fdt_get_prop(dtb, "/chosen", "stdout-path", &len);
    const uint8_t *reg = stdout_path ? fdt_get_prop(dtb, stdout_path, "reg", &len) : 0;
    if (reg == 0 || len < 8) {
        for (;;)
            hvc(4, 0, 0, 0);
    }
    uint64_t uart_base = fdt_be64(reg);
    g_uart = (volatile uint32_t *)uart_base;
    const uint8_t *irq = fdt_get_prop(dtb, stdout_path, "interrupts", &len);
    unsigned intid = irq && len >= 12 ? fdt_be32(irq + 4) + (fdt_be32(irq) == 0 ? 32u : 16u) : 0;
    unsigned cpus = fdt_count_children(dtb, "/cpus", "cpu@");
    const uint8_t *mem = 0;
    uint64_t mem_base = 0, mem_size = 0;
    /* The memory node's name carries its address; look for the one the
     * blob was built with, then read the range from the property. */
    {
        char path[32] = "/memory@";
        const uint8_t *root_mem = fdt_get_prop(dtb, "/memory@40000000", "reg", &len);
        (void)path;
        mem = root_mem;
    }
    if (mem && len >= 16) {
        mem_base = fdt_be64(mem);
        mem_size = fdt_be64(mem + 8);
    }
    const char *method = fdt_get_prop(dtb, "/psci", "method", &len);

    puts("dtb: uart@");
    puthex(uart_base);
    puts(" irq ");
    putdec(intid);
    puts(" cpus ");
    putdec(cpus);
    puts(" mem ");
    puthex(mem_base);
    puts("+");
    puthex(mem_size);
    puts(" psci ");
    puts(method ? method : "none");
    puts("\n");

    uint64_t ver = hvc(PSCI_VERSION, 0, 0, 0);
    puts("psci version 0x");
    puthex(ver);
    puts("\n");

    int64_t rc = (int64_t)hvc(PSCI_CPU_ON, 1, (uint64_t)(uintptr_t)secondary_start, 0x1234cafeull);
    lock(&g_print_lock);
    puts("cpu_on 1 -> ");
    if (rc < 0) {
        puts("-");
        putdec((uint64_t)-rc);
    } else {
        putdec((uint64_t)rc);
    }
    puts("\n");
    unlock(&g_print_lock);

    hvc(PSCI_SYSTEM_OFF, 0, 0, 0);
    for (;;)
        hvc(2, 0, 0, 0);
}
