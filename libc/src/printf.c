/*
 * printf.c - The formatting engine and the printf family.
 *
 * vsnprintf is the single implementation; the others feed it a sink.
 * Supported: %[flags][width][.prec][l|ll|z|h|hh][diuxXocsp%], flags
 * '-' '0' '+' ' ' '#', '*' for width and precision. No floating point
 * (programs are built without FPU state: %f prints '?').
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct out {
    void (*put)(struct out *o, const char *s, size_t n);
    char *buf;       /* snprintf */
    size_t cap;
    size_t len;      /* total characters that would have been written */
    int fd;          /* dprintf */
    FILE *file;      /* fprintf */
    char tmp[256];
    size_t tmp_len;
};

static void put_buf(struct out *o, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (o->len + i + 1 < o->cap)
            o->buf[o->len + i] = s[i];
    }
    o->len += n;
}

static void flush_tmp(struct out *o)
{
    if (o->tmp_len) {
        if (o->file)
            fwrite(o->tmp, 1, o->tmp_len, o->file);
        else
            write(o->fd, o->tmp, o->tmp_len);
        o->tmp_len = 0;
    }
}

static void put_stream(struct out *o, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (o->tmp_len == sizeof(o->tmp))
            flush_tmp(o);
        o->tmp[o->tmp_len++] = s[i];
    }
    o->len += n;
}

static void pad(struct out *o, char c, int n)
{
    while (n-- > 0)
        o->put(o, &c, 1);
}

static void format_number(struct out *o, unsigned long long v, int neg, int base, int upper, int width, int prec,
                          int left, int zero, int plus, int space, int alt)
{
    char digits[32];
    int n = 0;
    const char *set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (prec == 0 && v == 0) {
        /* an explicit zero precision prints nothing for 0 */
    } else {
        do {
            digits[n++] = set[v % (unsigned)base];
            v /= (unsigned)base;
        } while (v);
    }
    while (n < prec && n < (int)sizeof(digits))
        digits[n++] = '0';
    char sign = neg ? '-' : plus ? '+' : space ? ' ' : 0;
    const char *prefix = "";
    if (alt && base == 16)
        prefix = upper ? "0X" : "0x";
    else if (alt && base == 8 && (n == 0 || digits[n - 1] != '0'))
        prefix = "0";
    int total = n + (sign ? 1 : 0) + (int)strlen(prefix);
    if (!left && !zero)
        pad(o, ' ', width - total);
    if (sign)
        o->put(o, &sign, 1);
    o->put(o, prefix, strlen(prefix));
    if (!left && zero && prec < 0)
        pad(o, '0', width - total);
    while (n > 0)
        o->put(o, &digits[--n], 1);
    if (left)
        pad(o, ' ', width - total);
}

static void format(struct out *o, const char *fmt, va_list ap)
{
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            const char *start = p;
            while (*p && *p != '%')
                p++;
            o->put(o, start, (size_t)(p - start));
            p--;
            continue;
        }
        p++;
        int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
        for (;; p++) {
            if (*p == '-')
                left = 1;
            else if (*p == '0')
                zero = 1;
            else if (*p == '+')
                plus = 1;
            else if (*p == ' ')
                space = 1;
            else if (*p == '#')
                alt = 1;
            else
                break;
        }
        int width = 0;
        if (*p == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                left = 1;
                width = -width;
            }
            p++;
        } else {
            while (*p >= '0' && *p <= '9')
                width = width * 10 + (*p++ - '0');
        }
        int prec = -1;
        if (*p == '.') {
            p++;
            prec = 0;
            if (*p == '*') {
                prec = va_arg(ap, int);
                p++;
            } else {
                while (*p >= '0' && *p <= '9')
                    prec = prec * 10 + (*p++ - '0');
            }
        }
        int len = 0;   /* 0 int, 1 long, 2 long long, 3 size_t, -1 short, -2 char */
        while (*p == 'l' || *p == 'z' || *p == 'h' || *p == 'j' || *p == 't') {
            if (*p == 'l')
                len = len == 1 ? 2 : 1;
            else if (*p == 'z' || *p == 'j' || *p == 't')
                len = 3;
            else
                len = len == -1 ? -2 : -1;
            p++;
        }
        switch (*p) {
        case 'd':
        case 'i': {
            long long v;
            if (len == 2)
                v = va_arg(ap, long long);
            else if (len == 1)
                v = va_arg(ap, long);
            else if (len == 3)
                v = (long long)va_arg(ap, ssize_t);
            else
                v = va_arg(ap, int);
            if (len == -1)
                v = (short)v;
            if (len == -2)
                v = (signed char)v;
            unsigned long long u = v < 0 ? 0ULL - (unsigned long long)v : (unsigned long long)v;
            format_number(o, u, v < 0, 10, 0, width, prec, left, zero, plus, space, 0);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            unsigned long long v;
            if (len == 2)
                v = va_arg(ap, unsigned long long);
            else if (len == 1)
                v = va_arg(ap, unsigned long);
            else if (len == 3)
                v = va_arg(ap, size_t);
            else
                v = va_arg(ap, unsigned);
            if (len == -1)
                v = (unsigned short)v;
            if (len == -2)
                v = (unsigned char)v;
            int base = *p == 'u' ? 10 : *p == 'o' ? 8 : 16;
            format_number(o, v, 0, base, *p == 'X', width, prec, left, zero, 0, 0, alt);
            break;
        }
        case 'p': {
            uintptr_t v = (uintptr_t)va_arg(ap, void *);
            format_number(o, v, 0, 16, 0, width, -1, left, 0, 0, 0, 1);
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            if (!left)
                pad(o, ' ', width - 1);
            o->put(o, &c, 1);
            if (left)
                pad(o, ' ', width - 1);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (s == NULL)
                s = "(null)";
            size_t n = prec >= 0 ? strnlen(s, (size_t)prec) : strlen(s);
            if (!left)
                pad(o, ' ', width - (int)n);
            o->put(o, s, n);
            if (left)
                pad(o, ' ', width - (int)n);
            break;
        }
        case 'f':
        case 'g':
        case 'e':
            o->put(o, "?", 1);
            break;
        case '%':
            o->put(o, "%", 1);
            break;
        case '\0':
            return;
        default:
            o->put(o, "%", 1);
            o->put(o, p, 1);
            break;
        }
    }
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    struct out o = { .put = put_buf, .buf = buf, .cap = n };
    format(&o, fmt, ap);
    if (n)
        buf[o.len < n ? o.len : n - 1] = '\0';
    return (int)o.len;
}

int snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, (size_t)-1 / 2, fmt, ap);
    va_end(ap);
    return r;
}

int vdprintf(int fd, const char *fmt, va_list ap)
{
    struct out o = { .put = put_stream, .fd = fd };
    format(&o, fmt, ap);
    flush_tmp(&o);
    return (int)o.len;
}

int dprintf(int fd, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vdprintf(fd, fmt, ap);
    va_end(ap);
    return r;
}

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    struct out o = { .put = put_stream, .file = f };
    format(&o, fmt, ap);
    flush_tmp(&o);
    return (int)o.len;
}

int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

int vprintf(const char *fmt, va_list ap)
{
    return vfprintf(stdout, fmt, ap);
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return r;
}
