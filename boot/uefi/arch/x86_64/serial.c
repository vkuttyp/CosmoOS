/* arch/x86_64/serial.c - COM1 (16550) output for the loader, via port I/O. */

#include "loader.h"

#define COM1 0x3F8

static bool g_present;

static inline void outb(uint16_t port, uint8_t v)
{
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

void arch_serial_init(void)
{
    outb(COM1 + 1, 0x00); /* no interrupts */
    outb(COM1 + 3, 0x80); /* DLAB on */
    outb(COM1 + 0, 0x01); /* divisor 1 = 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03); /* 8N1, DLAB off */
    outb(COM1 + 2, 0xC7); /* FIFO on, clear, 14-byte threshold */
    outb(COM1 + 4, 0x13); /* DTR, RTS, loopback for the presence test */
    outb(COM1 + 0, 0xA5);
    if (inb(COM1 + 0) != 0xA5) {
        g_present = false;
        return;
    }
    outb(COM1 + 4, 0x0B); /* DTR, RTS, OUT2, loopback off */
    g_present = true;
}

bool arch_serial_present(void)
{
    return g_present;
}

void arch_serial_putc(char c)
{
    if (!g_present)
        return;
    while ((inb(COM1 + 5) & 0x20) == 0)
        ;
    outb(COM1 + 0, (uint8_t)c);
}
