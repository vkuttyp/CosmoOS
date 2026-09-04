/*
 * printf.h - Kernel string formatting.
 *
 * Supported conversions: %c %s %d %i %u %x %X %o %p %%
 * Flags: - 0 + space #    Width and precision: digits or *
 * Length modifiers: hh h l ll z t j
 *
 * Both functions never allocate, never block, and are safe in interrupt
 * context. They return the length the full output would have had (C99
 * snprintf semantics) and always NUL-terminate when size > 0.
 */

#ifndef KERNEL_PRINTF_H
#define KERNEL_PRINTF_H

#include <stdarg.h>
#include <stddef.h>

#include <kernel/compiler.h>

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap) __printf(3, 0);
int ksnprintf(char *buf, size_t size, const char *fmt, ...) __printf(3, 4);

#endif /* KERNEL_PRINTF_H */
