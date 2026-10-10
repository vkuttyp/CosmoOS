/*
 * cmdline.c - The kernel command line (cmdline.h).
 *
 * cmdline_find is the parser and has no kernel dependencies; the two
 * functions after it are the kernel's view of the loader's text, and are
 * left out of host builds by CMDLINE_HOST_TEST.
 */

#include <kernel/cmdline.h>

#include <stdbool.h>

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

int cmdline_find(const char *text, const char *key, char *out, size_t out_len)
{
    size_t klen = 0;
    while (key[klen])
        klen++;
    const char *p = text;
    for (;;) {
        while (is_space(*p))
            p++;
        if (*p == '\0')
            return -1;
        if (*p == '#') {
            while (*p != '\0' && *p != '\n')
                p++;
            continue;
        }
        const char *tok = p;
        while (*p != '\0' && !is_space(*p) && *p != '#')
            p++;
        size_t tlen = (size_t)(p - tok);
        bool match = tlen >= klen;
        for (size_t i = 0; match && i < klen; i++)
            match = tok[i] == key[i];
        if (!match || (tlen > klen && tok[klen] != '='))
            continue;
        const char *val = tlen > klen ? tok + klen + 1 : tok + tlen;
        size_t vlen = (size_t)(p - val);
        if (out_len == 0 || vlen > out_len - 1)
            return -2;
        for (size_t i = 0; i < vlen; i++)
            out[i] = val[i];
        out[vlen] = '\0';
        return (int)vlen;
    }
}

#ifndef CMDLINE_HOST_TEST
#include <kernel/bootinfo.h>
#include <kernel/log.h>
#include <kernel/string.h>

void cmdline_log(void)
{
    const char *text = bootinfo_cmdline();
    if (text[0] == '\0') {
        kinfo("cmdline: none");
        return;
    }
    /* One line in the log: the file's line breaks become spaces. */
    char line[COSMOBOOT_CMDLINE_MAX + 1];
    size_t n = strlcpy(line, text, sizeof(line));
    for (size_t i = 0; i < n; i++)
        if (is_space(line[i]))
            line[i] = ' ';
    kinfo("cmdline: %s", line);
}

int cmdline_get(const char *key, char *out, size_t out_len)
{
    return cmdline_find(bootinfo_cmdline(), key, out, out_len);
}
#endif
