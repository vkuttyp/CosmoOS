/*
 * console.h - Kernel console output sinks.
 *
 * The console is a fan-out of registered sinks (serial now, framebuffer
 * later). It is the only path by which diagnostic text leaves the kernel.
 *
 * Concurrency: sinks are registered at boot before other CPUs exist and
 * the list is never modified afterwards. console_write itself takes no
 * lock; until the SMP work in Phase 3 adds one, output from concurrent
 * contexts may interleave but cannot corrupt state.
 *
 * Context: console_write is callable from interrupt and panic context. A
 * sink's write callback must therefore never sleep, allocate, or take a
 * sleeping lock.
 */

#ifndef KERNEL_CONSOLE_H
#define KERNEL_CONSOLE_H

#include <stddef.h>

struct console_sink {
    const char *name;
    /* Emit `len` bytes. Must be non-blocking beyond polling the device. */
    void (*write)(struct console_sink *sink, const char *s, size_t len);
    struct console_sink *next; /* owned by the console; do not touch */
};

/* Register a sink. The sink object must outlive the kernel (static). */
void console_register(struct console_sink *sink);

void console_write(const char *s, size_t len);
void console_puts(const char *s);

#endif /* KERNEL_CONSOLE_H */
