/*
 * printf.c - kvsnprintf: the one formatter every kernel message goes
 * through.
 *
 * Deliberately complete for integers and strings and deliberately absent
 * for floating point: the kernel is built with -mgeneral-regs-only and
 * has no FP state to save. Output is bounded by the caller's buffer;
 * the return value reports the untruncated length.
 */

#include <kernel/printf.h>
#include <kernel/string.h>

struct out {
    char *buf;
    size_t size;  /* capacity including the NUL */
    size_t pos;   /* characters "written", may exceed size */
};

static void out_char(struct out *o, char c)
{
    if (o->pos + 1 < o->size)
        o->buf[o->pos] = c;
    o->pos++;
}

static void out_pad(struct out *o, char c, int n)
{
    while (n-- > 0)
        out_char(o, c);
}

struct spec {
    bool left;      /* '-' */
    bool zero;      /* '0' */
    bool plus;      /* '+' */
    bool space;     /* ' ' */
    bool alt;       /* '#' */
    int  width;     /* -1 = none */
    int  precision; /* -1 = none */
    enum { LEN_NONE, LEN_HH, LEN_H, LEN_L, LEN_LL, LEN_Z, LEN_T, LEN_J } len;
};

static void emit_number(struct out *o, const struct spec *s, uint64_t mag, bool negative,
                        unsigned base, bool upper)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24]; /* 64-bit octal needs 22 digits */
    int ndigits = 0;

    /* Precision 0 with value 0 prints nothing (C99). */
    if (!(s->precision == 0 && mag == 0)) {
        do {
            tmp[ndigits++] = digits[mag % base];
            mag /= base;
        } while (mag != 0);
    }

    char sign = 0;
    if (negative)
        sign = '-';
    else if (s->plus)
        sign = '+';
    else if (s->space)
        sign = ' ';

    const char *prefix = "";
    if (s->alt) {
        if (base == 16 && ndigits > 0)
            prefix = upper ? "0X" : "0x";
        else if (base == 8 && (ndigits == 0 || tmp[ndigits - 1] != '0'))
            prefix = "0";
    }
    int prefix_len = (int)strlen(prefix);

    int zeros = 0;
    if (s->precision > ndigits)
        zeros = s->precision - ndigits;

    int body = (sign ? 1 : 0) + prefix_len + zeros + ndigits;
    int pad = (s->width > body) ? s->width - body : 0;

    /* '0' flag pads with zeros after the sign/prefix, unless '-' or a
     * precision was given. */
    bool zero_pad = s->zero && !s->left && s->precision < 0;

    if (!s->left && !zero_pad)
        out_pad(o, ' ', pad);
    if (sign)
        out_char(o, sign);
    while (*prefix)
        out_char(o, *prefix++);
    if (zero_pad)
        out_pad(o, '0', pad);
    out_pad(o, '0', zeros);
    while (ndigits > 0)
        out_char(o, tmp[--ndigits]);
    if (s->left)
        out_pad(o, ' ', pad);
}

static void emit_string(struct out *o, const struct spec *s, const char *str)
{
    if (str == NULL)
        str = "(null)";

    size_t len = (s->precision >= 0) ? strnlen(str, (size_t)s->precision) : strlen(str);
    int pad = (s->width > (int)len) ? s->width - (int)len : 0;

    if (!s->left)
        out_pad(o, ' ', pad);
    for (size_t i = 0; i < len; i++)
        out_char(o, str[i]);
    if (s->left)
        out_pad(o, ' ', pad);
}

static int64_t fetch_signed(const struct spec *s, va_list *ap)
{
    switch (s->len) {
    case LEN_HH: return (signed char)va_arg(*ap, int);
    case LEN_H:  return (short)va_arg(*ap, int);
    case LEN_L:  return va_arg(*ap, long);
    case LEN_LL: return va_arg(*ap, long long);
    case LEN_Z:  return (int64_t)va_arg(*ap, size_t);
    case LEN_T:  return va_arg(*ap, ptrdiff_t);
    case LEN_J:  return va_arg(*ap, intmax_t);
    case LEN_NONE:
    default:     return va_arg(*ap, int);
    }
}

static uint64_t fetch_unsigned(const struct spec *s, va_list *ap)
{
    switch (s->len) {
    case LEN_HH: return (unsigned char)va_arg(*ap, unsigned);
    case LEN_H:  return (unsigned short)va_arg(*ap, unsigned);
    case LEN_L:  return va_arg(*ap, unsigned long);
    case LEN_LL: return va_arg(*ap, unsigned long long);
    case LEN_Z:  return va_arg(*ap, size_t);
    case LEN_T:  return (uint64_t)va_arg(*ap, ptrdiff_t);
    case LEN_J:  return va_arg(*ap, uintmax_t);
    case LEN_NONE:
    default:     return va_arg(*ap, unsigned);
    }
}

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap_in)
{
    struct out o = { .buf = buf, .size = size, .pos = 0 };
    va_list ap;

    va_copy(ap, ap_in);

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            out_char(&o, *fmt);
            continue;
        }
        fmt++;

        struct spec s = { .width = -1, .precision = -1, .len = LEN_NONE };

        for (;; fmt++) {
            if (*fmt == '-')      s.left = true;
            else if (*fmt == '0') s.zero = true;
            else if (*fmt == '+') s.plus = true;
            else if (*fmt == ' ') s.space = true;
            else if (*fmt == '#') s.alt = true;
            else break;
        }

        if (*fmt == '*') {
            s.width = va_arg(ap, int);
            if (s.width < 0) {
                s.left = true;
                s.width = -s.width;
            }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') {
                if (s.width < 0)
                    s.width = 0;
                s.width = s.width * 10 + (*fmt - '0');
                fmt++;
            }
        }

        if (*fmt == '.') {
            fmt++;
            s.precision = 0;
            if (*fmt == '*') {
                s.precision = va_arg(ap, int);
                if (s.precision < 0)
                    s.precision = -1;
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9') {
                    s.precision = s.precision * 10 + (*fmt - '0');
                    fmt++;
                }
            }
        }

        switch (*fmt) {
        case 'h':
            fmt++;
            if (*fmt == 'h') {
                s.len = LEN_HH;
                fmt++;
            } else {
                s.len = LEN_H;
            }
            break;
        case 'l':
            fmt++;
            if (*fmt == 'l') {
                s.len = LEN_LL;
                fmt++;
            } else {
                s.len = LEN_L;
            }
            break;
        case 'z': s.len = LEN_Z; fmt++; break;
        case 't': s.len = LEN_T; fmt++; break;
        case 'j': s.len = LEN_J; fmt++; break;
        default: break;
        }

        switch (*fmt) {
        case 'd':
        case 'i': {
            int64_t v = fetch_signed(&s, &ap);
            bool neg = v < 0;
            uint64_t mag = neg ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
            emit_number(&o, &s, mag, neg, 10, false);
            break;
        }
        case 'u':
            emit_number(&o, &s, fetch_unsigned(&s, &ap), false, 10, false);
            break;
        case 'x':
            emit_number(&o, &s, fetch_unsigned(&s, &ap), false, 16, false);
            break;
        case 'X':
            emit_number(&o, &s, fetch_unsigned(&s, &ap), false, 16, true);
            break;
        case 'o':
            emit_number(&o, &s, fetch_unsigned(&s, &ap), false, 8, false);
            break;
        case 'p': {
            /* Fixed 0x + 16 hex digits: addresses line up in dumps. */
            struct spec ps = { .width = 18, .precision = 16, .alt = true, .left = s.left };
            emit_number(&o, &ps, (uint64_t)(uintptr_t)va_arg(ap, void *), false, 16, false);
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            int pad = (s.width > 1) ? s.width - 1 : 0;
            if (!s.left)
                out_pad(&o, ' ', pad);
            out_char(&o, c);
            if (s.left)
                out_pad(&o, ' ', pad);
            break;
        }
        case 's':
            emit_string(&o, &s, va_arg(ap, const char *));
            break;
        case '%':
            out_char(&o, '%');
            break;
        case '\0':
            /* Dangling '%' at end of format: emit it and stop. */
            out_char(&o, '%');
            goto done;
        default:
            /* Unknown conversion: reproduce it so the bug is visible. */
            out_char(&o, '%');
            out_char(&o, *fmt);
            break;
        }
    }

done:
    va_end(ap);

    if (size > 0)
        buf[(o.pos < size) ? o.pos : size - 1] = '\0';

    return (int)o.pos;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

/* Module ABI v1 exports (docs/kernel/module/api.md). */
#include <kernel/module.h>
EXPORT_SYMBOL(ksnprintf);
EXPORT_SYMBOL(kvsnprintf);
