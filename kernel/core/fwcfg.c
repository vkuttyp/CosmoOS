/*
 * fwcfg.c - Generic access to firmware configuration strings.
 */

#include <kernel/fwcfg.h>
#include <kernel/printf.h>
#include <kernel/string.h>

#include <arch/fwcfg.h>

bool fwcfg_get_string(const char *key, char *buf, size_t len)
{
    if (len == 0)
        return false;
    char name[64];
    ksnprintf(name, sizeof(name), "opt/cosmo/%s", key);
    int n = arch_fwcfg_read(name, buf, len - 1);
    if (n < 0)
        return false;
    buf[(size_t)n < len - 1 ? (size_t)n : len - 1] = '\0';
    return true;
}
