/*
 * console.c - Fan-out of kernel text output to registered sinks.
 *
 * See kernel/console.h for the concurrency and context rules.
 */

#include <kernel/console.h>
#include <kernel/string.h>

static struct console_sink *g_sinks;

void console_register(struct console_sink *sink)
{
    if (sink == NULL || sink->write == NULL)
        return;
    /* Prepend: registration order is boot order; the newest sink is
     * usually the more capable one and should see output first. */
    sink->next = g_sinks;
    g_sinks = sink;
}

void console_write(const char *s, size_t len)
{
    for (struct console_sink *k = g_sinks; k != NULL; k = k->next)
        k->write(k, s, len);
}

void console_puts(const char *s)
{
    console_write(s, strlen(s));
}
