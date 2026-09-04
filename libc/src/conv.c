/* conv.c - Numeric conversions and qsort (pure C; also compiled on the host by tests/host/test_libc.c). */

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

/* --- conversions --- */

static unsigned long long parse_ull(const char *s, char **end, int base, int *neg, int *overflow)
{
    const char *p = s;
    while (isspace((unsigned char)*p))
        p++;
    *neg = 0;
    if (*p == '+' || *p == '-') {
        *neg = *p == '-';
        p++;
    }
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])) {
        base = 16;
        p += 2;
    } else if (base == 0) {
        base = p[0] == '0' ? 8 : 10;
    }
    unsigned long long v = 0;
    const char *start = p;
    *overflow = 0;
    for (;; p++) {
        int d;
        if (isdigit((unsigned char)*p))
            d = *p - '0';
        else if (isalpha((unsigned char)*p))
            d = tolower((unsigned char)*p) - 'a' + 10;
        else
            break;
        if (d >= base)
            break;
        if (v > (~0ULL - (unsigned long long)d) / (unsigned long long)base)
            *overflow = 1;
        v = v * (unsigned long long)base + (unsigned long long)d;
    }
    if (end)
        *end = (char *)(uintptr_t)(p == start ? s : p);
    return v;
}

long long strtoll(const char *s, char **end, int base)
{
    int neg, ov;
    unsigned long long v = parse_ull(s, end, base, &neg, &ov);
    if (ov || v > (neg ? (unsigned long long)LLONG_MAX + 1 : (unsigned long long)LLONG_MAX)) {
        errno = ERANGE;
        return neg ? (-LLONG_MAX - 1) : LLONG_MAX;
    }
    return neg ? -(long long)v : (long long)v;
}

unsigned long long strtoull(const char *s, char **end, int base)
{
    int neg, ov;
    unsigned long long v = parse_ull(s, end, base, &neg, &ov);
    if (ov) {
        errno = ERANGE;
        return ~0ULL;
    }
    return neg ? (unsigned long long)(-(long long)v) : v;
}

long strtol(const char *s, char **end, int base) { return (long)strtoll(s, end, base); }
unsigned long strtoul(const char *s, char **end, int base) { return (unsigned long)strtoull(s, end, base); }
int atoi(const char *s) { return (int)strtol(s, NULL, 10); }
long atol(const char *s) { return strtol(s, NULL, 10); }
int abs(int v) { return v < 0 ? -v : v; }
long labs(long v) { return v < 0 ? -v : v; }

/* --- qsort: insertion sort for small ranges, else quicksort --- */

static void swap_bytes(char *a, char *b, size_t n)
{
    while (n--) {
        char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

static void qsort_impl(char *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    while (n > 8) {
        char *pivot = base + (n / 2) * size;
        swap_bytes(pivot, base + (n - 1) * size, size);
        pivot = base + (n - 1) * size;
        size_t store = 0;
        for (size_t i = 0; i + 1 < n; i++) {
            if (cmp(base + i * size, pivot) < 0) {
                swap_bytes(base + i * size, base + store * size, size);
                store++;
            }
        }
        swap_bytes(base + store * size, pivot, size);
        /* Recurse on the smaller half, loop on the larger. */
        if (store < n - store - 1) {
            qsort_impl(base, store, size, cmp);
            base += (store + 1) * size;
            n -= store + 1;
        } else {
            qsort_impl(base + (store + 1) * size, n - store - 1, size, cmp);
            n = store;
        }
    }
    for (size_t i = 1; i < n; i++)
        for (size_t j = i; j > 0 && cmp(base + (j - 1) * size, base + j * size) > 0; j--)
            swap_bytes(base + (j - 1) * size, base + j * size, size);
}

void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    if (n > 1 && size > 0)
        qsort_impl(base, n, size, cmp);
}
