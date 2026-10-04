/*
 * console.h - Kernel console output sinks.
 *
 * The console is a fan-out of registered serial, framebuffer and device
 * sinks. It is the path by which diagnostic text leaves the kernel.
 *
 * Concurrency: normal writes and unregister use the console spinlock.
 * Panic output bypasses that lock because its owner may be stopped.
 *
 * Context: console_write is callable from interrupt and panic context. A
 * sink's write callback must therefore never sleep, allocate, or take a
 * sleeping lock. A sink needing tracked locks must return without touching
 * its transport when console_in_panic_mode() is true.
 */

#ifndef KERNEL_CONSOLE_H
#define KERNEL_CONSOLE_H

#include <stdbool.h>
#include <arch/irq.h>   /* arch_irq_state_t, for console_hold */
#include <stddef.h>

struct console_sink {
    const char *name;
    /* Emit `len` bytes. Must be non-blocking beyond polling the device. */
    void (*write)(struct console_sink *sink, const char *s, size_t len);
    struct console_sink *next; /* owned by the console; do not touch */
};

/* Register a sink. The sink object must stay valid until
 * console_unregister (module unload) or forever (static). */
void console_register(struct console_sink *sink);
void console_unregister(struct console_sink *sink);
bool console_has_sink(const char *name);

void console_write(const char *s, size_t len);
void console_puts(const char *s);

/* Tests: hold every console writer off -- another CPU's console_write
 * spins -- for a window in which the UART must carry nothing but the
 * test's own bytes (the PL011 loopback test). Nothing may log while it is
 * held: a log line would wait on this very lock. */
arch_irq_state_t console_hold(void);
void console_release(arch_irq_state_t st);

/* Panic mode: bypass console and log-ring locks so a report can be printed
 * even if an interrupted or halted CPU holds them. Irreversible. */
void console_set_panic_mode(void);
bool console_in_panic_mode(void);

#endif /* KERNEL_CONSOLE_H */
