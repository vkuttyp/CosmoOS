/*
 * string.c - Freestanding memory and string primitives.
 *
 * Plain C loops. The compiler is told these are the real memcpy/memset
 * (they carry the standard names) so it must not turn their bodies back
 * into calls to themselves; -ffreestanding guarantees that. Word-sized
 * fast paths can come later behind measurements, per coding rule 9.
 */

#include <kernel/string.h>

#include <stdint.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    if (d == s || n == 0)
        return dst;

    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
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

size_t strnlen(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b || *a == '\0')
            return (int)(uint8_t)*a - (int)(uint8_t)*b;
    }
    return 0;
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);
    if (size > 0) {
        size_t copy = len < size - 1 ? len : size - 1;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return len;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == (unsigned char)c)
            return (void *)(uintptr_t)(p + i);
    }
    return NULL;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)(uintptr_t)s;
        if (*s == '\0')
            return NULL;
    }
}

char *strstr(const char *haystack, const char *needle)
{
    size_t n = strlen(needle);
    if (n == 0)
        return (char *)(uintptr_t)haystack;
    for (; *haystack; haystack++) {
        if (strncmp(haystack, needle, n) == 0)
            return (char *)(uintptr_t)haystack;
    }
    return NULL;
}

/* Module ABI v1 exports (docs/kernel/module/api.md). */
#include <kernel/module.h>
EXPORT_SYMBOL(memcpy);
EXPORT_SYMBOL(memmove);
EXPORT_SYMBOL(memset);
EXPORT_SYMBOL(memcmp);
EXPORT_SYMBOL(memchr);
EXPORT_SYMBOL(strlen);
EXPORT_SYMBOL(strnlen);
EXPORT_SYMBOL(strcmp);
EXPORT_SYMBOL(strncmp);
EXPORT_SYMBOL(strchr);
EXPORT_SYMBOL(strstr);
EXPORT_SYMBOL(strlcpy);
