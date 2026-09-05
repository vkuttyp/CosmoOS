/*
 * pl011.c - The PL011 UART: early console, console sink, receive interrupt
 * into the tty (docs/kernel/arch/aarch64/design.md, "Console").
 *
 * Before ACPI is parsed the `virt` default address is used through the
 * direct map (the loader maps non-RAM ranges as device memory there);
 * arch_console_input_init consults the SPCR table for the base and the
 * interrupt, warning when they differ from the defaults.
 */

#include <kernel/acpi.h>
#include <kernel/console.h>
#include <kernel/interrupt.h>
#include <kernel/irq.h>
#include <kernel/log.h>
#include <kernel/string.h>
#include <kernel/tty.h>
#include <kernel/vmm.h>
#include <arch/console.h>
#include <aarch64/platform.h>

extern uint64_t aarch64_hhdm_base;

#define UART_DR    0x000
#define UART_FR    0x018
#define UART_IBRD  0x024
#define UART_FBRD  0x028
#define UART_LCR_H 0x02C
#define UART_CR    0x030
#define UART_IMSC  0x038
#define UART_MIS   0x040
#define UART_ICR   0x044

#define FR_TXFF (1u << 5)
#define FR_RXFE (1u << 4)
#define CR_UARTEN (1u << 0)
#define CR_TXE    (1u << 8)
#define CR_RXE    (1u << 9)
#define LCR_WLEN8 (3u << 5)
#define LCR_FEN   (1u << 4)
#define IMSC_RXIM (1u << 4)
#define IMSC_RTIM (1u << 6)

static volatile uint32_t *g_regs;
static paddr_t g_base = VIRT_PL011_BASE;
static unsigned g_intid = VIRT_PL011_INTID;

static inline uint32_t rd(unsigned off) { return g_regs[off / 4]; }
static inline void wr(unsigned off, uint32_t v) { g_regs[off / 4] = v; }

void pl011_early_putc(char c)
{
    if (g_regs == NULL)
        return;
    while (rd(UART_FR) & FR_TXFF)
        ;
    wr(UART_DR, (uint32_t)(uint8_t)c);
}

static void pl011_write(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\n')
            pl011_early_putc('\r');
        pl011_early_putc(s[i]);
    }
}

static void sink_write(struct console_sink *sink, const char *s, size_t len)
{
    (void)sink;
    pl011_write(s, len);
}

static struct console_sink g_sink = {
    .name = "pl011",
    .write = sink_write,
};

void arch_console_early_init(void)
{
    g_regs = (volatile uint32_t *)(uintptr_t)(aarch64_hhdm_base + g_base);
    /* The firmware left the UART configured; make the state explicit. */
    wr(UART_IMSC, 0);
    wr(UART_ICR, 0x7FF);
    wr(UART_LCR_H, LCR_WLEN8 | LCR_FEN);
    wr(UART_CR, CR_UARTEN | CR_TXE | CR_RXE);
    console_register(&g_sink);
}

/* SPCR: interface type at 36, base address GAS at 40 (address at 44), GSIV at 54. */
static void read_spcr(void)
{
    const struct acpi_sdt_header *spcr = acpi_find_table("SPCR");
    if (spcr == NULL || spcr->length < 60) {
        kwarn("pl011: no SPCR table; using the virt defaults (0x%llx, INTID %u)", (unsigned long long)g_base,
              g_intid);
        return;
    }
    const uint8_t *b = (const uint8_t *)spcr;
    uint8_t type = b[36];
    uint64_t addr;
    uint32_t gsiv;
    memcpy(&addr, b + 44, 8);
    memcpy(&gsiv, b + 54, 4);
    if (type != 3 && type != 0x0E) {   /* PL011 (3) or ARM SBSA generic UART (0x0E) */
        kwarn("pl011: SPCR describes interface type %u, not a PL011; keeping the defaults", type);
        return;
    }
    if (addr != 0 && addr != g_base) {
        vaddr_t va = vm_map_phys(addr, 0x1000, VM_PROT_RW, VM_CACHE_UC);
        if (va == 0) {
            kwarn("pl011: cannot map SPCR base 0x%llx; keeping 0x%llx", (unsigned long long)addr,
                  (unsigned long long)g_base);
        } else {
            g_base = addr;
            g_regs = (volatile uint32_t *)va;
            kinfo("pl011: SPCR moves the console to 0x%llx", (unsigned long long)addr);
        }
    }
    if (gsiv != 0 && gsiv < GIC_INTID_COUNT)
        g_intid = gsiv;
}

static void rx_irq(unsigned vector, struct arch_trap_frame *frame, void *arg)
{
    (void)vector;
    (void)frame;
    struct tty *t = arg;
    while ((rd(UART_FR) & FR_RXFE) == 0) {
        uint8_t c = (uint8_t)rd(UART_DR);
        tty_input(t, &c, 1);
    }
    wr(UART_ICR, IMSC_RXIM | IMSC_RTIM);
}

void arch_console_input_init(void)
{
    read_spcr();
    int rc = irq_request(g_intid, rx_irq, tty_console(), "pl011-rx", IRQ_TRIGGER_LEVEL, 0);
    if (rc) {
        kwarn("pl011: cannot request INTID %u for receive (%d); console input disabled", g_intid, rc);
        return;
    }
    while ((rd(UART_FR) & FR_RXFE) == 0)
        (void)rd(UART_DR);
    wr(UART_IMSC, IMSC_RXIM | IMSC_RTIM);
    rc = irq_enable(g_intid);
    if (rc) {
        kwarn("pl011: cannot enable INTID %u (%d); console input disabled", g_intid, rc);
        return;
    }
    kinfo("serial: console input on IRQ %u", g_intid);
}
