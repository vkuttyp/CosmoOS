/*
 * driver_floor.c - The C floor every CeeSafe driver module links
 * (drivers/include/drivers/ceesafe_driver.h). Compiled into each module
 * with the module flags, beside CeeSafe's runtime; uses only exported
 * kernel functions (klog, panic). It is also the module's ceesafe_panic:
 * a CeeSafe panic in a driver (a bounds or overflow check) is a kernel
 * panic naming the reason and the source line, as kernel/ceesafe/ceesafe_panic.c
 * makes it in the kernel.
 */

#include <kernel/device.h>
#include <kernel/log.h>
#include <kernel/panic.h>

#include <drivers/ceesafe_driver.h>

#include "ceesafe/rt.h"

#define LINE_MAX 192u

static size_t put_str(char *line, size_t at, const char *s)
{
    if (s == NULL)
        s = "(null)";
    while (*s != '\0' && at + 1 < LINE_MAX)
        line[at++] = *s++;
    return at;
}

static size_t put_u64(char *line, size_t at, uint64_t v)
{
    char digits[20];
    unsigned n = 0;
    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0);
    while (n > 0 && at + 1 < LINE_MAX)
        line[at++] = digits[--n];
    return at;
}

void cosmo_drv_klog(unsigned level, const char *fmt, const char *s, uint64_t value)
{
    char line[LINE_MAX];
    size_t at = 0;
    for (const char *p = fmt; *p != '\0' && at + 1 < LINE_MAX; p++) {
        if (p[0] == '%' && p[1] == 's') {
            at = put_str(line, at, s);
            p++;
        } else if (p[0] == '%' && p[1] == 'u') {
            at = put_u64(line, at, value);
            p++;
        } else {
            line[at++] = *p;
        }
    }
    line[at] = '\0';
    klog((enum klog_level)level, "%s", line);
}

bool cosmo_drv_selftest(void)
{
    return CONFIG_SELFTEST != 0;
}

const char *cosmo_drv_device_name(const struct device *dev)
{
    return dev->name;
}

_Noreturn void ceesafe_panic(cee_panic_kind kind, cee_loc loc)
{
    panic("ceesafe panic: %s at %s:%u:%u", ceesafe_panic_reason(kind), loc.file, (unsigned)loc.line,
          (unsigned)loc.column);
}
