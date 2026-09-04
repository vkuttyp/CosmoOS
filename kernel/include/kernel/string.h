/*
 * string.h - Freestanding memory and string primitives.
 *
 * memcpy/memmove/memset/memcmp must exist with these exact names because
 * the compiler emits calls to them for aggregate copies and initialisers.
 *
 * All functions: no allocation, no locking, no sleeping, safe in any
 * context including interrupt handlers. Callers own bounds checking;
 * these do exactly what the length says.
 */

#ifndef KERNEL_STRING_H
#define KERNEL_STRING_H

#include <stddef.h>

void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
void  *memchr(const void *s, int c, size_t n);

size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strchr(const char *s, int c);
char  *strstr(const char *haystack, const char *needle);

/* Copy up to size-1 bytes and always NUL-terminate when size > 0.
 * Returns strlen(src) so callers can detect truncation. */
size_t strlcpy(char *dst, const char *src, size_t size);

#endif /* KERNEL_STRING_H */
