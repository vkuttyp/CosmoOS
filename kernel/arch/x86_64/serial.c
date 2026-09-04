/*
 * serial.c - 16550 UART driver for the early console (COM1).
 *
 * Polled output. Presence is detected with the loopback test so a
 * machine without a UART does not hang on the first log line. This is the
 * arch/console.h implementation: it registers itself as a console sink.
 */

#include <kernel/console.h>

#include <arch/console.h>

#include <x86/io.h>
#include <x86/serial.h>

#define COM1 0x3F8

#define UART_DATA    0 /* THR/RBR, DLL with DLAB */
#define UART_IER     1 /* interrupt enable, DLM with DLAB */
#define UART_FCR     2 /* FIFO control */
#define UART_LCR     3 /* line control */
#define UART_MCR     4 /* modem control */
#define UART_LSR     5 /* line status */

#define LCR_8N1      0x03
#define LCR_DLAB     0x80
#define FCR_ENABLE_CLEAR_14 0xC7
#define MCR_DTR_RTS_OUT2 0x0B
#define MCR_LOOPBACK 0x10
#define LSR_THR_EMPTY 0x20

static bool g_present;

bool serial_init(void)
{
    outb(COM1 + UART_IER, 0x00);            /* no interrupts */
    outb(COM1 + UART_LCR, LCR_DLAB);        /* divisor access */
    outb(COM1 + UART_DATA, 0x01);           /* 115200 baud */
    outb(COM1 + UART_IER, 0x00);
    outb(COM1 + UART_LCR, LCR_8N1);
    outb(COM1 + UART_FCR, FCR_ENABLE_CLEAR_14);
    outb(COM1 + UART_MCR, MCR_DTR_RTS_OUT2 | MCR_LOOPBACK);

    outb(COM1 + UART_DATA, 0xA5);
    if (inb(COM1 + UART_DATA) != 0xA5) {
        g_present = false;
        return false;
    }

    outb(COM1 + UART_MCR, MCR_DTR_RTS_OUT2);
    g_present = true;
    return true;
}

bool serial_present(void)
{
    return g_present;
}

void serial_putc(char c)
{
    if (!g_present)
        return;
    while ((inb(COM1 + UART_LSR) & LSR_THR_EMPTY) == 0)
        ;
    outb(COM1 + UART_DATA, (uint8_t)c);
}

void serial_write(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\n')
            serial_putc('\r');
        serial_putc(s[i]);
    }
}

/* --- console sink --- */

static void serial_sink_write(struct console_sink *sink, const char *s, size_t len)
{
    (void)sink;
    serial_write(s, len);
}

static struct console_sink g_serial_sink = {
    .name = "serial0",
    .write = serial_sink_write,
};

void arch_console_early_init(void)
{
    serial_init();
    console_register(&g_serial_sink);
}
