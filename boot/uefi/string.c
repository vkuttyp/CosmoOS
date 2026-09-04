/*
 * string.c - Freestanding memory and string primitives for the loader.
 *
 * The compiler is free to emit calls to memcpy/memset for struct copies and
 * initialisation, so these must exist even where the loader never calls
 * them directly. They are byte-at-a-time on purpose: boot-time volume is
 * tiny and clarity wins.
 */

#include "loader.h"

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    while (n--)
        *d++ = (uint8_t)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a;
    const uint8_t *y = b;
    for (; n; n--, x++, y++) {
        if (*x != *y)
            return (int)*x - (int)*y;
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}
