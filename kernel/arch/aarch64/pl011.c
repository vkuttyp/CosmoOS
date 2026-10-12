/*
 * pl011.c - The serial console: early output, console sink, receive into
 * the tty (docs/kernel/arch/aarch64/design.md, "Console").
 *
 * The port is the one the loader found in the firmware's ACPI tables
 * (boot protocol v9: the SPCR, else the DBG2) and recorded in the boot
 * info: a PL011/SBSA UART or a memory-mapped 16550, reached through the
 * loader's direct map, where every non-RAM range has device attributes;
 * map_early_devices carries that page into the kernel's own tables. With
 * no record there is no serial console, and kernel_main brings the
 * framebuffer console up instead. No address is assumed: QEMU virt's
 * 0x09000000, which this file wrote to before reading anything until
 * v1.0.1, is a store into whatever another machine has there.
 *
 * Input: on the recorded interrupt, or, when the firmware names none (a
 * DBG2 port has no interrupt), polled every POLL_NS from a kernel timer.
 */

#include <kernel/acpi.h>
#include <kernel/console.h>
#include <kernel/interrupt.h>
#include <kernel/irq.h>
#include <kernel/log.h>
#include <kernel/selftest.h>
#include <kernel/string.h>
#include <kernel/timer.h>
#include <kernel/tty.h>
#include <kernel/vmm.h>
#include <arch/console.h>
#include <aarch64/platform.h>

#include <cosmoboot.h>

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

/* 16550: register index, scaled by g_stride bytes. */
#define U_RBR 0   /* read */
#define U_THR 0   /* write */
#define U_IER 1
#define U_FCR 2
#define U_LCR 3
#define U_LSR 5
#define LSR_DR   (1u << 0)
#define LSR_THRE (1u << 5)
#define IER_RDI  (1u << 0)

#define POLL_NS 10000000ull   /* 10 ms: typing speed, not a throughput */

static uint32_t g_type = COSMOBOOT_CONSOLE_NONE;
static uintptr_t g_va;            /* register block, virtual */
static paddr_t g_base;            /* register block, physical */
static unsigned g_stride = 1;     /* 16550 register spacing in bytes */
static unsigned g_intid;          /* 0: none named, input is polled */
static volatile uint32_t *g_regs; /* PL011 registers (g_va) */
static bool g_rx_ready;           /* receive interrupt requested and enabled, or polling */
static bool g_polled;
static struct timer g_poll;

static inline uint32_t rd(unsigned off) { return g_regs[off / 4]; }
static inline void wr(unsigned off, uint32_t v) { g_regs[off / 4] = v; }

static inline uint32_t u_rd(unsigned reg)
{
    if (g_stride == 4)
        return *(volatile uint32_t *)(g_va + reg * 4);
    return *(volatile uint8_t *)(g_va + reg * g_stride);
}

static inline void u_wr(unsigned reg, uint32_t v)
{
    if (g_stride == 4)
        *(volatile uint32_t *)(g_va + reg * 4) = v;
    else
        *(volatile uint8_t *)(g_va + reg * g_stride) = (uint8_t)v;
}

void pl011_early_putc(char c)
{
    if (g_type == COSMOBOOT_CONSOLE_PL011) {
        while (rd(UART_FR) & FR_TXFF)
            ;
        wr(UART_DR, (uint32_t)(uint8_t)c);
    } else if (g_type == COSMOBOOT_CONSOLE_16550_MMIO) {
        while ((u_rd(U_LSR) & LSR_THRE) == 0)
            ;
        u_wr(U_THR, (uint8_t)c);
    }
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
    .name = "serial",
    .write = sink_write,
};

/* Set by aarch64_start before arch_console_early_init: the boot info as
 * the loader passed it, not yet validated by bootinfo_init. */
const struct cosmoboot_info *aarch64_boot_info;

/* "serial=none" on the command line (docs/boot/design.md, "The command
 * line"): no serial console even when the firmware names one, so the boot
 * is on the framebuffer alone -- the path a machine without a serial port
 * takes, made reachable on QEMU, whose SPCR is always there (make
 * test-platforms). Read here, before bootinfo_init has copied and checked
 * the line, so with the same bounds: inside the direct map, at most
 * COSMOBOOT_CMDLINE_MAX bytes, byte reads. */
static bool cmdline_says_no_serial(const struct cosmoboot_info *info)
{
    static const char key[] = "serial=none";
    const size_t klen = sizeof(key) - 1;
    if (info->cmdline_phys == 0 || info->cmdline_size == 0 || info->cmdline_size > COSMOBOOT_CMDLINE_MAX ||
        info->cmdline_phys + info->cmdline_size > info->hhdm_size)
        return false;
    const volatile char *t = (const volatile char *)(uintptr_t)(aarch64_hhdm_base + info->cmdline_phys);
    size_t n = info->cmdline_size;
    for (size_t i = 0; i + klen <= n; i++) {
        if (i > 0 && t[i - 1] != ' ' && t[i - 1] != '\t' && t[i - 1] != '\n')
            continue;
        size_t j = 0;
        while (j < klen && t[i + j] == key[j])
            j++;
        if (j == klen && (i + klen == n || t[i + klen] == ' ' || t[i + klen] == '\t' || t[i + klen] == '\n'))
            return true;
    }
    return false;
}

void arch_console_early_init(void)
{
    const struct cosmoboot_info *info = aarch64_boot_info;
    /* A loader of another protocol version has no console record (or a
     * different one): no serial console, and bootinfo_init then refuses
     * the mismatch on the screen. */
    if (info == NULL || info->version != COSMOBOOT_VERSION || info->size < sizeof(*info))
        return;
    if (info->console_type != COSMOBOOT_CONSOLE_PL011 && info->console_type != COSMOBOOT_CONSOLE_16550_MMIO)
        return;
    if (cmdline_says_no_serial(info))
        return;
    if (info->console_base == 0 || info->console_base >= info->hhdm_size)
        return;
    g_base = (paddr_t)info->console_base;
    g_va = (uintptr_t)(aarch64_hhdm_base + g_base);
    g_intid = info->console_gsiv < GIC_INTID_COUNT ? info->console_gsiv : 0;
    g_type = info->console_type;
    if (g_type == COSMOBOOT_CONSOLE_PL011) {
        g_regs = (volatile uint32_t *)g_va;
        /* The firmware left the UART configured; make the state explicit. */
        wr(UART_IMSC, 0);
        wr(UART_ICR, 0x7FF);
        wr(UART_LCR_H, LCR_WLEN8 | LCR_FEN);
        wr(UART_CR, CR_UARTEN | CR_TXE | CR_RXE);
    } else {
        g_stride = info->console_access == 4 ? 4 : 1;
        u_wr(U_IER, 0);
        u_wr(U_FCR, 0x07);   /* FIFOs on and cleared */
        u_wr(U_LCR, 0x03);   /* 8N1; the firmware's baud rate stays */
    }
    console_register(&g_sink);
}

bool aarch64_console_serial(paddr_t *base, unsigned *intid, const char **kind)
{
    if (g_type == COSMOBOOT_CONSOLE_NONE)
        return false;
    *base = g_base;
    *intid = g_intid;
    *kind = g_type == COSMOBOOT_CONSOLE_PL011 ? "PL011" : "16550";
    return true;
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
    if (g_type == COSMOBOOT_CONSOLE_16550_MMIO) {
        while (u_rd(U_LSR) & LSR_DR) {
            uint8_t c = (uint8_t)u_rd(U_RBR);
            if (t != NULL)
                tty_input(t, &c, 1);
        }
        if (after_drain != NULL)
            after_drain();
        return;
    }
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

/* No interrupt named: the receive FIFO is read every POLL_NS. A timer
 * callback runs where an interrupt handler would, so rx_service sees the
 * same context either way. */
static void rx_poll(struct timer *t, void *arg)
{
    rx_service(arg, NULL);
    timer_start(t, POLL_NS);
}

void arch_console_input_init(void)
{
    if (g_type == COSMOBOOT_CONSOLE_NONE)
        return;   /* no serial console: input, if any, is a keyboard's */
    if (g_intid != 0) {
        int rc = irq_request(g_intid, rx_irq, tty_console(), "serial-rx", IRQ_TRIGGER_LEVEL, 0);
        if (rc == 0) {
            if (g_type == COSMOBOOT_CONSOLE_PL011) {
                while ((rd(UART_FR) & FR_RXFE) == 0)
                    (void)rd(UART_DR);
                wr(UART_IMSC, IMSC_RXIM | IMSC_RTIM);
            } else {
                while (u_rd(U_LSR) & LSR_DR)
                    (void)u_rd(U_RBR);
                u_wr(U_IER, IER_RDI);
            }
            rc = irq_enable(g_intid);
        }
        if (rc == 0) {
            g_rx_ready = true;
            kinfo("serial: console input on IRQ %u", g_intid);
            return;
        }
        kwarn("serial: cannot use INTID %u for receive (%d); polling instead", g_intid, rc);
    }
    g_polled = true;
    g_rx_ready = true;
    timer_setup(&g_poll, rx_poll, tty_console());
    timer_start(&g_poll, POLL_NS);
    kinfo("serial: console input polled every %llu ms (the firmware names no interrupt)",
          (unsigned long long)(POLL_NS / 1000000ull));
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
    if (!g_rx_ready || g_polled || g_type != COSMOBOOT_CONSOLE_PL011) {
        kinfo("selftest: console-rx-clear: no PL011 receive interrupt; skipping");
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
