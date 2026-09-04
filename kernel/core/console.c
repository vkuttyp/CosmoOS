/*
 * console.c - Fan-out of kernel text output to registered sinks.
 *
 * A spinlock serialises writes so lines from different CPUs do not
 * interleave. Panic mode drops the lock for good: the panicking CPU has
 * halted the others, one of which may have held it.
 */

#include <kernel/console.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>

static struct console_sink *g_sinks;
static spinlock_t g_console_lock = SPINLOCK_INIT("console");
static volatile bool g_panic_mode;

void console_register(struct console_sink *sink)
{
    if (sink == NULL || sink->write == NULL)
        return;
    /* Prepend: registration order is boot order; the newest sink is
     * usually the more capable one and should see output first. */
    sink->next = g_sinks;
    g_sinks = sink;
}

static void write_unlocked(const char *s, size_t len)
{
    for (struct console_sink *k = g_sinks; k != NULL; k = k->next)
        k->write(k, s, len);
}

void console_write(const char *s, size_t len)
{
    if (g_panic_mode) {
        write_unlocked(s, len);
        return;
    }
    arch_irq_state_t st = spin_lock_irqsave(&g_console_lock);
    write_unlocked(s, len);
    spin_unlock_irqrestore(&g_console_lock, st);
}

void console_puts(const char *s)
{
    console_write(s, strlen(s));
}

void console_set_panic_mode(void)
{
    g_panic_mode = true;
}
