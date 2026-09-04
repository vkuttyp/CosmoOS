/*
 * x86/serial.h - 16550 UART on COM1. Private to x86-64.
 *
 * Polled transmit only. There is no receive path yet; when one exists it
 * will use the interrupt subsystem rather than polling.
 */

#ifndef X86_SERIAL_H
#define X86_SERIAL_H

#include <stdbool.h>
#include <stddef.h>

/* Probe and configure COM1 at 115200 8N1. Safe to call before anything
 * else in the kernel. Returns false if no UART responded; output is then
 * silently dropped. */
bool serial_init(void);
bool serial_present(void);
void serial_putc(char c);
void serial_write(const char *s, size_t len);

#endif /* X86_SERIAL_H */
