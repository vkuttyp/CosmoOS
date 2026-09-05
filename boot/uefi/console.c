/*
 * console.c - Loader output to the firmware console and COM1.
 *
 * The serial path uses direct port I/O so that it keeps working after
 * ExitBootServices, when the firmware console must no longer be touched.
 * QEMU's -serial option captures exactly this stream.
 */

#include <stdarg.h>

#include "loader.h"

static bool g_firmware_console = false;

void console_init(void)
{
    arch_serial_init();
    g_firmware_console = (g_st != NULL && g_st->ConOut != NULL);
}

void console_firmware_gone(void)
{
    g_firmware_console = false;
}

/*
 * While boot services are up, output goes through the firmware console
 * only: firmware such as OVMF already mirrors ConOut to the serial port,
 * and writing the UART directly as well would double every character.
 * After ExitBootServices the UART is the only remaining path.
 */
static void console_putc(char c)
{
    if (g_firmware_console) {
        CHAR16 buf[3];
        size_t n = 0;
        if (c == '\n')
            buf[n++] = L'\r';
        buf[n++] = (CHAR16)(uint8_t)c;
        buf[n] = 0;
        g_st->ConOut->OutputString(g_st->ConOut, buf);
        return;
    }

    if (c == '\n')
        arch_serial_putc('\r');
    arch_serial_putc(c);
}

void lputs(const char *s)
{
    while (*s)
        console_putc(*s++);
}

static void put_unsigned(uint64_t v, unsigned base, unsigned width, char pad, bool upper)
{
    char tmp[24];
    size_t n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v != 0 && n < sizeof(tmp));

    while (n < width && width <= sizeof(tmp))
        tmp[n++] = pad;

    while (n > 0)
        console_putc(tmp[--n]);
}

/*
 * Minimal formatter: %s %c %d %u %x %X %p %%, optional '0' flag and width,
 * optional 'l'/'ll'/'z' length modifiers (all treated as 64-bit).
 */
void lprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            console_putc(*fmt);
            continue;
        }
        fmt++;

        char pad = ' ';
        unsigned width = 0;
        /* 0 = int, 1 = long, 2 = long long. The loader target is LLP64, so
         * `long` is 32-bit; read arguments with their real C type. */
        unsigned longs = 0;

        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (unsigned)(*fmt - '0');
            fmt++;
        }
        while (*fmt == 'l' || *fmt == 'z') {
            if (*fmt == 'z')
                longs = (sizeof(size_t) == sizeof(long long)) ? 2 : 1;
            else if (longs < 2)
                longs++;
            fmt++;
        }

#define ARG_SIGNED()   (longs == 2 ? va_arg(ap, long long) : longs == 1 ? (long long)va_arg(ap, long) : (long long)va_arg(ap, int))
#define ARG_UNSIGNED() (longs == 2 ? va_arg(ap, unsigned long long) : longs == 1 ? (unsigned long long)va_arg(ap, unsigned long) : (unsigned long long)va_arg(ap, unsigned))

        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            lputs(s ? s : "(null)");
            break;
        }
        case 'c':
            console_putc((char)va_arg(ap, int));
            break;
        case 'd': {
            long long v = ARG_SIGNED();
            if (v < 0) {
                console_putc('-');
                v = -v;
            }
            put_unsigned((uint64_t)v, 10, width, pad, false);
            break;
        }
        case 'u':
            put_unsigned(ARG_UNSIGNED(), 10, width, pad, false);
            break;
        case 'x':
        case 'X':
            put_unsigned(ARG_UNSIGNED(), 16, width, pad, *fmt == 'X');
            break;
#undef ARG_SIGNED
#undef ARG_UNSIGNED
        case 'p':
            lputs("0x");
            put_unsigned((uint64_t)(uintptr_t)va_arg(ap, void *), 16, 16, '0', false);
            break;
        case '%':
            console_putc('%');
            break;
        case '\0':
            va_end(ap);
            return;
        default:
            console_putc('%');
            console_putc(*fmt);
            break;
        }
    }

    va_end(ap);
}

void die(const char *what, EFI_STATUS status)
{
    lprintf("cosmoboot: FATAL: %s (status 0x%llx)\n", what, (unsigned long long)status);

    if (g_firmware_console && g_bs != NULL) {
        /* Give a human a moment to read the screen, then return to firmware. */
        g_bs->Stall(3 * 1000 * 1000);
        g_bs->Exit(g_image, status, 0, NULL);
    }

    cpu_halt();
}
