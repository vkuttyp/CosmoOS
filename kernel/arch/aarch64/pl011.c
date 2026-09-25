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
#include <kernel/selftest.h>
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
#define UART_RIS   0x03C
#define UART_MIS   0x040
#define UART_ICR   0x044

#define FR_TXFF (1u << 5)
#define FR_RXFE (1u << 4)
#define FR_BUSY (1u << 3)
#define CR_UARTEN (1u << 0)
#define CR_LBE    (1u << 7)
#define CR_TXE    (1u << 8)
#define CR_RXE    (1u << 9)
#define LCR_WLEN8 (3u << 5)
#define LCR_FEN   (1u << 4)
#define IMSC_RXIM (1u << 4)
#define IMSC_RTIM (1u << 6)

static volatile uint32_t *g_regs;
static bool g_rx_ready;   /* the receive interrupt is requested and enabled */
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

/*
 * Clear, then drain (docs/audit/next-subsystem-console-rx.md). The other
 * order -- drain until empty, then clear -- lost the console for good: a
 * character arriving between the last "empty" read and the clear raised
 * the receive interrupt and had it cleared at once, stayed in the FIFO,
 * and QEMU's PL011 raises the interrupt only when the FIFO count reaches
 * its trigger level (one), so nothing after it raised anything either.
 * Cleared first, a character arriving during the drain is read by it, and
 * one arriving after the drain's last read raises an interrupt nothing
 * clears. `after_drain` is the test's hook at exactly that moment. A
 * NULL `t` reads and discards: the test runs this with every console
 * writer held off, and a byte handed to the tty there would be echoed
 * through console_write, which would spin on the lock the test holds.
 */
static void rx_service(struct tty *t, void (*after_drain)(void))
{
    wr(UART_ICR, IMSC_RXIM | IMSC_RTIM);
    while ((rd(UART_FR) & FR_RXFE) == 0) {
        uint8_t c = (uint8_t)rd(UART_DR);
        if (t != NULL)
            tty_input(t, &c, 1);
    }
    if (after_drain != NULL)
        after_drain();
}

static void rx_irq(unsigned vector, struct arch_trap_frame *frame, void *arg)
{
    (void)vector;
    (void)frame;
    rx_service(arg, NULL);
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
    g_rx_ready = true;
    kinfo("serial: console input on IRQ %u", g_intid);
}

/* --- console-rx-clear: the race, made to happen ---------------------------
 *
 * A byte put into the receive FIFO after the service's drain -- the
 * moment the old order cleared its interrupt -- must leave the receive
 * interrupt pending. The PL011's loopback (CR.LBE) routes a transmitted
 * byte into its own receive FIFO, so the test's hook at that moment
 * transmits one. The GIC line is disabled for the test and every console
 * writer is held off (console_hold), so the UART carries nothing but that
 * byte; the raw interrupt status, not the handler, is the evidence.
 */
#define RIS_RXRIS (1u << 4)
#define RX_TEST_SPINS 100000u   /* register reads; a guard, not a timing */

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            *reason = "check failed: " #cond;                                \
            return false;                                                    \
        }                                                                    \
    } while (0)

static void rx_test_send(void)
{
    /* In loopback: into this UART's own receive FIFO. QEMU also sends it
     * down the line, so a carriage return -- invisible in a log. */
    wr(UART_DR, (uint32_t)'\r');
}

bool selftest_console_rx_clear(const char **reason)
{
    if (!g_rx_ready) {
        kinfo("selftest: console-rx-clear: no console receive interrupt; skipping");
        return true;
    }
    CHECK(irq_disable(g_intid) == 0);
    arch_irq_state_t st = console_hold();   /* nothing may log until console_release */
    /* The last line out, before loopback. Bounded: IRQs are off here, so
     * a transmitter that never idles must not take the boot with it. */
    bool idle = false;
    for (unsigned i = 0; i < RX_TEST_SPINS && !idle; i++)
        idle = (rd(UART_FR) & FR_BUSY) == 0;
    if (!idle) {
        console_release(st);
        CHECK(irq_enable(g_intid) == 0);
        *reason = "the transmitter never went idle";
        return false;
    }
    uint32_t cr = rd(UART_CR);
    while ((rd(UART_FR) & FR_RXFE) == 0)
        (void)rd(UART_DR);
    wr(UART_ICR, IMSC_RXIM | IMSC_RTIM);
    wr(UART_CR, cr | CR_LBE);

    rx_service(NULL, rx_test_send);   /* drains nothing; the hook's byte arrives after */

    bool arrived = false;
    for (unsigned i = 0; i < RX_TEST_SPINS && !arrived; i++)
        arrived = (rd(UART_FR) & FR_RXFE) == 0;
    uint32_t ris = rd(UART_RIS);

    while ((rd(UART_FR) & FR_RXFE) == 0)
        (void)rd(UART_DR);                  /* the test's byte is not input */
    wr(UART_ICR, IMSC_RXIM | IMSC_RTIM);
    wr(UART_CR, cr);
    console_release(st);
    CHECK(irq_enable(g_intid) == 0);

    if (!arrived) {
        /* Said, not passed: without loopback nothing here was tested. */
        kinfo("selftest: console-rx-clear: this PL011 has no loopback (the byte never arrived); skipping");
        return true;
    }
    CHECK(ris & RIS_RXRIS);   /* the byte that arrived after the drain still has its interrupt */
    kinfo("selftest: console-rx-clear: a byte arriving after the drain left its receive interrupt pending");
    return true;
}
