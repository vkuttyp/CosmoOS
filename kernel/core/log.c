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
#include <kernel/spinlock.h>
#include <kernel/string.h>

/* The ring: every emitted line (all builds), oldest overwritten first.
 * `head` is the total number of bytes ever written; the ring holds the
 * last KLOG_RING_SIZE of them. */
static char g_ring[KLOG_RING_SIZE];
static uint64_t g_ring_head;
static spinlock_t g_ring_lock = SPINLOCK_INIT("klog-ring");

static void ring_put(const char *s, size_t n)
{
    arch_irq_state_t st = spin_lock_irqsave(&g_ring_lock);
    for (size_t i = 0; i < n; i++)
        g_ring[(g_ring_head + i) % KLOG_RING_SIZE] = s[i];
    g_ring_head += n;
    spin_unlock_irqrestore(&g_ring_lock, st);
}

size_t klog_copy(char *buf, size_t len)
{
    arch_irq_state_t st = spin_lock_irqsave(&g_ring_lock);
    uint64_t avail = g_ring_head < KLOG_RING_SIZE ? g_ring_head : KLOG_RING_SIZE;
    uint64_t start = g_ring_head - avail;            /* oldest byte still in the ring */
    if (avail > len)
        start = g_ring_head - len;                    /* newest `len` bytes */
    /* Begin at a line boundary: skip to just after the first newline
     * unless we hold the whole history. */
    if (start != g_ring_head - avail) {
        while (start < g_ring_head && g_ring[start % KLOG_RING_SIZE] != '\n')
            start++;
        if (start < g_ring_head)
            start++;
    }
    size_t n = 0;
    for (uint64_t i = start; i < g_ring_head && n < len; i++)
        buf[n++] = g_ring[i % KLOG_RING_SIZE];
    spin_unlock_irqrestore(&g_ring_lock, st);
    return n;
}

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
    ring_put(line, n);
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
    ring_put(line, n);
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
