/*
 * printf.c - The formatting engine and the printf family.
 *
 * vsnprintf is the single implementation; the others feed it a sink.
 * Supported: %[flags][width][.prec][l|ll|z|h|hh][diuxXocsp%], flags
 * '-' '0' '+' ' ' '#', '*' for width and precision, and the three
 * floating conversions %f, %e and %g over `double`.
 *
 * The float conversion is deliberately small: `double` only (no `long
 * double`, no %a), fixed precision, and no libm. It converts by scaling
 * and integer division, which is exact enough for diagnostics -- the
 * consumer this exists for -- and does not pretend to be exactly rounded
 * in the last digit for every input. Values too large for a 64-bit
 * integer part are printed in exponent form whatever the conversion
 * asked for, since the alternative is a wrong answer.
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

/* --- floating point (double only; see the file comment) --- */

#define DBL_DIG_MAX 17

union dbits {
    double d;
    uint64_t u;
};

static int dbl_is_nan(double v)
{
    union dbits b = { .d = v };
    return ((b.u >> 52) & 0x7ff) == 0x7ff && (b.u & 0xfffffffffffffull) != 0;
}

static int dbl_is_inf(double v)
{
    union dbits b = { .d = v };
    return ((b.u >> 52) & 0x7ff) == 0x7ff && (b.u & 0xfffffffffffffull) == 0;
}

static int dbl_is_neg(double v)
{
    union dbits b = { .d = v };
    return (int)(b.u >> 63);
}

static double dbl_pow10(int e)
{
    double r = 1.0;
    double base = e < 0 ? 0.1 : 10.0;
    if (e < 0)
        e = -e;
    while (e-- > 0)
        r *= base;
    return r;
}

/* The decimal exponent of |v|, for %e and %g. */
static int dbl_exp10(double v)
{
    int e = 0;
    if (v == 0.0)
        return 0;
    while (v >= 10.0) {
        v /= 10.0;
        e++;
    }
    while (v < 1.0) {
        v *= 10.0;
        e--;
    }
    return e;
}

static void pad(struct out *o, char c, int n)
{
    while (n-- > 0)
        o->put(o, &c, 1);
}

/*
 * One floating conversion. `conv` is 'f', 'e' or 'g'; the result is
 * built into `buf` and returned, so the caller applies width and the
 * padding flags exactly as it does for an integer.
 */
static size_t format_double(char *buf, size_t cap, double v, char conv, int prec, int plus, int space, int upper)
{
    size_t n = 0;
    int neg = dbl_is_neg(v);

    if (dbl_is_nan(v) || dbl_is_inf(v)) {
        const char *word = dbl_is_nan(v) ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf");
        if (dbl_is_nan(v))
            neg = 0;   /* a NaN has no useful sign */
        if (neg)
            buf[n++] = '-';
        else if (plus)
            buf[n++] = '+';
        else if (space)
            buf[n++] = ' ';
        for (const char *w = word; *w && n < cap; w++)
            buf[n++] = *w;
        return n;
    }

    if (neg)
        v = -v;
    if (prec < 0)
        prec = 6;
    if (prec > 17)
        prec = 17;   /* past the digits a double carries */

    /* %g: the exponent decides the form, and trailing zeros go. */
    int trim = 0;
    if (conv == 'g') {
        int e = dbl_exp10(v);
        int sig = prec == 0 ? 1 : prec;
        if (e < -4 || e >= sig) {
            conv = 'e';
            prec = sig - 1;
        } else {
            conv = 'f';
            prec = sig - 1 - e;
            if (prec < 0)
                prec = 0;
        }
        trim = 1;
    }

    int exp = 0;
    if (conv == 'e') {
        exp = dbl_exp10(v);
        v /= dbl_pow10(exp);
        if (v >= 10.0) {   /* the division rounded up into two digits */
            v /= 10.0;
            exp++;
        }
    }

    /* Round at the last printed digit, then split. */
    v += 0.5 * dbl_pow10(-prec);
    if (conv == 'e' && v >= 10.0) {
        v /= 10.0;
        exp++;
    }

    /* An integer part beyond 2^64 cannot be split this way; say so in
     * exponent form rather than print something wrong. */
    if (conv == 'f' && v >= 18446744073709549568.0) {
        exp = dbl_exp10(v);
        v /= dbl_pow10(exp);
        conv = 'e';
    }

    unsigned long long ip = (unsigned long long)v;
    double frac = v - (double)ip;

    char digits[DBL_DIG_MAX + 8];
    size_t dn = 0;
    if (ip == 0) {
        digits[dn++] = '0';
    } else {
        char rev[24];
        size_t rn = 0;
        while (ip > 0 && rn < sizeof(rev)) {
            rev[rn++] = (char)('0' + (int)(ip % 10));
            ip /= 10;
        }
        while (rn > 0)
            digits[dn++] = rev[--rn];
    }

    if (neg)
        buf[n++] = '-';
    else if (plus)
        buf[n++] = '+';
    else if (space)
        buf[n++] = ' ';
    for (size_t i = 0; i < dn && n < cap; i++)
        buf[n++] = digits[i];

    size_t point = n;
    if (prec > 0 && n < cap) {
        buf[n++] = '.';
        for (int i = 0; i < prec && n < cap; i++) {
            frac *= 10.0;
            int d = (int)frac;
            if (d < 0)
                d = 0;
            if (d > 9)
                d = 9;
            buf[n++] = (char)('0' + d);
            frac -= (double)d;
        }
    }

    if (trim && prec > 0) {
        while (n > point + 1 && buf[n - 1] == '0')
            n--;
        if (n > point && buf[n - 1] == '.')
            n--;
    }

    if (conv == 'e' && n + 4 <= cap) {
        buf[n++] = upper ? 'E' : 'e';
        buf[n++] = exp < 0 ? '-' : '+';
        int ae = exp < 0 ? -exp : exp;
        if (ae >= 100) {
            buf[n++] = (char)('0' + ae / 100);
            ae %= 100;
        }
        buf[n++] = (char)('0' + ae / 10);
        buf[n++] = (char)('0' + ae % 10);
    }
    return n;
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
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G': {
            char fb[64];
            char conv = (char)(*p | 0x20);
            int upper = (*p >= 'A' && *p <= 'Z');
            size_t n = format_double(fb, sizeof(fb), va_arg(ap, double), conv, prec, plus, space, upper);
            size_t sign = (n > 0 && (fb[0] == '-' || fb[0] == '+' || fb[0] == ' ')) ? 1u : 0u;
            if (!left && zero) {
                /* Zero padding goes between the sign and the digits, as
                 * it does for an integer: "-0001.50", never "000-1.50". */
                o->put(o, fb, sign);
                pad(o, '0', width - (int)n);
                o->put(o, fb + sign, n - sign);
                if (left)
                    pad(o, ' ', width - (int)n);
                break;
            }
            if (!left)
                pad(o, ' ', width - (int)n);
            o->put(o, fb, n);
            if (left)
                pad(o, ' ', width - (int)n);
            break;
        }
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
