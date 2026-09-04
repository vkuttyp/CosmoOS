/*
 * log.c - Kernel logging front end.
 *
 * Formats on the stack and hands complete lines to the console. There is
 * no ring buffer yet; when one exists (for dmesg) it hooks in here as a
 * second destination without touching callers.
 */

#include <kernel/console.h>
#include <kernel/log.h>
#include <kernel/printf.h>
#include <kernel/string.h>

static enum klog_level g_level =
#if CONFIG_DEBUG
    KLOG_DEBUG;
#else
    KLOG_INFO;
#endif

static const char *const level_tag[] = {
    [KLOG_DEBUG] = "[DEBUG] ",
    [KLOG_INFO]  = "[ INFO] ",
    [KLOG_WARN]  = "[ WARN] ",
    [KLOG_ERROR] = "[ERROR] ",
    [KLOG_PANIC] = "[PANIC] ",
};

void klog_set_level(enum klog_level level)
{
    g_level = level;
}

enum klog_level klog_get_level(void)
{
    return g_level;
}

void kvlog(enum klog_level level, const char *fmt, va_list ap)
{
    char line[KLOG_LINE_MAX];

    if (level < g_level)
        return;
    if (level > KLOG_PANIC)
        level = KLOG_PANIC;

    size_t n = strlcpy(line, level_tag[level], sizeof(line));
    int m = kvsnprintf(line + n, sizeof(line) - n, fmt, ap);
    if (m < 0)
        return;
    n += (size_t)m;
    if (n >= sizeof(line) - 1) {
        /* Truncated: keep room for the newline. */
        n = sizeof(line) - 2;
    }
    line[n++] = '\n';
    line[n] = '\0';

    console_write(line, n);
}

void klog(enum klog_level level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kvlog(level, fmt, ap);
    va_end(ap);
}

void kvprintf(const char *fmt, va_list ap)
{
    char line[KLOG_LINE_MAX];
    int m = kvsnprintf(line, sizeof(line), fmt, ap);
    if (m < 0)
        return;
    size_t n = (size_t)m;
    if (n >= sizeof(line))
        n = sizeof(line) - 1;
    console_write(line, n);
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}

/* Module ABI v1 exports (docs/kernel/module/api.md). */
#include <kernel/module.h>
EXPORT_SYMBOL(klog);
EXPORT_SYMBOL(kvlog);
EXPORT_SYMBOL(kprintf);
EXPORT_SYMBOL(kvprintf);
