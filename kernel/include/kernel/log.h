/*
 * log.h - Kernel logging.
 *
 * klog() formats into a fixed stack buffer (KLOG_LINE_MAX) and writes the
 * line to the console with a level prefix. Lines longer than the buffer
 * are truncated, never dropped. kprintf() writes raw text without prefix
 * for banners and test output.
 *
 * Context: safe in interrupt and panic context; never sleeps or
 * allocates. Uses about KLOG_LINE_MAX + 64 bytes of stack.
 */

#ifndef KERNEL_LOG_H
#define KERNEL_LOG_H

#include <stdarg.h>

#include <kernel/compiler.h>

#define KLOG_LINE_MAX 256

enum klog_level {
    KLOG_DEBUG = 0,
    KLOG_INFO  = 1,
    KLOG_WARN  = 2,
    KLOG_ERROR = 3,
    KLOG_PANIC = 4,
};

void klog(enum klog_level level, const char *fmt, ...) __printf(2, 3);
void kvlog(enum klog_level level, const char *fmt, va_list ap) __printf(2, 0);

/* Messages below this level are dropped. Default: DEBUG in debug builds,
 * INFO otherwise. */
void klog_set_level(enum klog_level level);
enum klog_level klog_get_level(void);

/* Raw output, no prefix, no newline added. */
void kprintf(const char *fmt, ...) __printf(1, 2);
void kvprintf(const char *fmt, va_list ap) __printf(1, 0);

#define kdebug(...) klog(KLOG_DEBUG, __VA_ARGS__)
#define kinfo(...)  klog(KLOG_INFO, __VA_ARGS__)
#define kwarn(...)  klog(KLOG_WARN, __VA_ARGS__)
#define kerror(...) klog(KLOG_ERROR, __VA_ARGS__)

#endif /* KERNEL_LOG_H */
