/*
 * console.c - Fan-out of kernel text output to registered sinks.
 *
 * A spinlock serialises writes so lines from different CPUs do not
 * interleave. Panic mode drops the lock for good: the panicking CPU has
 * halted the others, one of which may have held it.
 *
 * The lock is taken with interrupts off, and a sink can take milliseconds
 * a line (fbcon's scroll repaints the screen: 11 ms under QEMU's TCG), so
 * a write is passed to the sinks a line at a time, at most
 * CONSOLE_HOLD_MAX bytes a hold. One hold for a whole write kept
 * interrupts off on the writer's CPU for as long as the write took to
 * draw: `dmesg`'s one 32 KiB write, over a second -- past the second a
 * TLB shootdown waits for an acknowledgement (docs/testing/flakes.md,
 * 2026-10-21).
 */

#include <kernel/console.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>

static struct console_sink *g_sinks;
static spinlock_t g_console_lock = SPINLOCK_INIT("console");
static bool g_panic_mode;
static uint64_t g_calls;   /* console_write calls, under the lock: a sink's skip_call */
#if CONFIG_SELFTEST
static size_t g_hold_max;   /* the most bytes one console_write hold passed to the sinks, under the lock */
#endif

void console_register(struct console_sink *sink)
{
    if (sink == NULL || sink->write == NULL)
        return;
    /* Prepend: registration order is boot order; the newest sink is
     * usually the more capable one and should see output first. */
    sink->next = g_sinks;
    g_sinks = sink;
}

void console_unregister(struct console_sink *sink)
{
    /* Under the console lock so no writer holds a pointer to a sink
     * that is being unlinked (module unload). */
    arch_irq_state_t st = spin_lock_irqsave(&g_console_lock);
    for (struct console_sink **pp = &g_sinks; *pp != NULL; pp = &(*pp)->next) {
        if (*pp == sink) {
            *pp = sink->next;
            sink->next = NULL;
            break;
        }
    }
    spin_unlock_irqrestore(&g_console_lock, st);
}

bool console_any_sink(void)
{
    return g_sinks != NULL;
}

bool console_has_sink(const char *name)
{
    for (struct console_sink *k = g_sinks; k != NULL; k = k->next) {
        if (k->name && strcmp(k->name, name) == 0)
            return true;
    }
    return false;
}

static void write_unlocked(const char *s, size_t len)
{
    for (struct console_sink *k = g_sinks; k != NULL; k = k->next)
        k->write(k, s, len);
}

/* One hold of console_write `call`: a sink that dropped an earlier part
 * of the call is passed none of the rest. Lock held. */
static void write_call(const char *s, size_t len, uint64_t call)
{
    for (struct console_sink *k = g_sinks; k != NULL; k = k->next) {
        if (k->skip_call == call)
            continue;
        k->dropped = false;
        k->write(k, s, len);
        if (k->dropped)
            k->skip_call = call;
    }
}

void console_write(const char *s, size_t len)
{
    if (console_in_panic_mode()) {
        write_unlocked(s, len);
        return;
    }
    uint64_t call = 0;
    while (len > 0) {
        const char *nl = memchr(s, '\n', len < CONSOLE_HOLD_MAX ? len : CONSOLE_HOLD_MAX);
        size_t n = nl ? (size_t)(nl - s) + 1 : (len < CONSOLE_HOLD_MAX ? len : CONSOLE_HOLD_MAX);
        arch_irq_state_t st = spin_lock_irqsave(&g_console_lock);
#if CONFIG_SELFTEST
        if (n > g_hold_max)
            g_hold_max = n;
#endif
        if (call == 0)
            call = ++g_calls;
        write_call(s, n, call);
        spin_unlock_irqrestore(&g_console_lock, st);
        s += n;
        len -= n;
    }
}

size_t console_test_hold_max(bool reset)
{
#if CONFIG_SELFTEST
    arch_irq_state_t st = spin_lock_irqsave(&g_console_lock);
    size_t n = g_hold_max;
    if (reset)
        g_hold_max = 0;
    spin_unlock_irqrestore(&g_console_lock, st);
    return n;
#else
    (void)reset;
    return 0;
#endif
}

arch_irq_state_t console_hold(void)
{
    return spin_lock_irqsave(&g_console_lock);
}

void console_release(arch_irq_state_t st)
{
    spin_unlock_irqrestore(&g_console_lock, st);
}

void console_puts(const char *s)
{
    console_write(s, strlen(s));
}

void console_set_panic_mode(void)
{
    __atomic_store_n(&g_panic_mode, true, __ATOMIC_RELEASE);
}

bool console_in_panic_mode(void)
{
    return __atomic_load_n(&g_panic_mode, __ATOMIC_ACQUIRE);
}

/* Module ABI v1 exports (docs/kernel/module/api.md). */
#include <kernel/module.h>
EXPORT_SYMBOL(console_register);
EXPORT_SYMBOL(console_unregister);
EXPORT_SYMBOL(console_in_panic_mode);
