/*
 * upgrade_be.c -- the boot-environment pairing file (upgrade_be.h).
 */
#include "upgrade_be.h"

#include <stdio.h>
#include <string.h>

static bool all_hex(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return false;
    return true;
}

/* A snapshot name as cosmo-snap allows it: no '/', no leading '-', 1..31. */
static bool name_ok(const char *s, size_t n)
{
    if (n == 0 || n > BE_NAME_MAX || s[0] == '-')
        return false;
    for (size_t i = 0; i < n; i++)
        if (s[i] == '/' || (unsigned char)s[i] <= ' ')
            return false;
    return true;
}

int be_parse(const char *text, size_t len, struct be_file *b, char *err, size_t errlen)
{
    memset(b, 0, sizeof(*b));
    int lineno = 0;
    bool header = false;
    for (size_t pos = 0; pos < len;) {
        const char *l = text + pos;
        const char *nl = memchr(l, '\n', len - pos);
        size_t ll = nl ? (size_t)(nl - l) : len - pos;
        pos += ll + (nl ? 1 : 0);
        lineno++;
        if (ll == 0)
            continue;
        char line[256];
        if (ll >= sizeof(line)) {
            snprintf(err, errlen, "line %d: too long", lineno);
            return -1;
        }
        memcpy(line, l, ll);
        line[ll] = '\0';
        char *f[5];
        int nf = 0;
        for (char *s = line; *s && nf < 5;) {
            while (*s == ' ')
                s++;
            if (!*s)
                break;
            f[nf++] = s;
            while (*s && *s != ' ')
                s++;
            if (*s)
                *s++ = '\0';
        }
        if (!header) {
            if (nf != 2 || strcmp(f[0], "cosmo-be") != 0 || strcmp(f[1], "1") != 0) {
                snprintf(err, errlen, "line %d: not a cosmo-be 1 file", lineno);
                return -1;
            }
            header = true;
        } else if (nf == 2 && strcmp(f[0], "intent") == 0 && strlen(f[1]) == BE_SET_HEX &&
                   all_hex(f[1], BE_SET_HEX)) {
            memcpy(b->intent, f[1], BE_SET_HEX + 1);
        } else if (nf == 4 && strcmp(f[0], "env") == 0 && name_ok(f[1], strlen(f[1])) &&
                   strlen(f[2]) == BE_UUID_HEX && all_hex(f[2], BE_UUID_HEX) && strlen(f[3]) == BE_SET_HEX &&
                   all_hex(f[3], BE_SET_HEX)) {
            if (b->n == BE_MAX) {
                snprintf(err, errlen, "line %d: more than %d environments", lineno, BE_MAX);
                return -1;
            }
            struct be_env *e = &b->env[b->n++];
            snprintf(e->name, sizeof(e->name), "%s", f[1]);
            memcpy(e->uuid, f[2], BE_UUID_HEX + 1);
            memcpy(e->set, f[3], BE_SET_HEX + 1);
        } else {
            snprintf(err, errlen, "line %d: not an intent or env line", lineno);
            return -1;
        }
    }
    if (!header) {
        snprintf(err, errlen, "empty");
        return -1;
    }
    return 0;
}

size_t be_format(const struct be_file *b, char *out, size_t cap)
{
    size_t n = 0;
    int w = snprintf(out, cap, "cosmo-be 1\n");
    if (w < 0 || (size_t)w >= cap)
        return 0;
    n += (size_t)w;
    if (b->intent[0]) {
        w = snprintf(out + n, cap - n, "intent %s\n", b->intent);
        if (w < 0 || (size_t)w >= cap - n)
            return 0;
        n += (size_t)w;
    }
    for (int i = 0; i < b->n; i++) {
        w = snprintf(out + n, cap - n, "env %s %s %s\n", b->env[i].name, b->env[i].uuid, b->env[i].set);
        if (w < 0 || (size_t)w >= cap - n)
            return 0;
        n += (size_t)w;
    }
    return n;
}

void be_drop(struct be_file *b, int i)
{
    if (i < 0 || i >= b->n)
        return;
    memmove(&b->env[i], &b->env[i + 1], (size_t)(b->n - i - 1) * sizeof(b->env[0]));
    b->n--;
}

void be_add(struct be_file *b, const char *name, const char *uuid, const char *set)
{
    if (b->n == BE_MAX)
        be_drop(b, 0);
    struct be_env *e = &b->env[b->n++];
    snprintf(e->name, sizeof(e->name), "%s", name);
    snprintf(e->uuid, sizeof(e->uuid), "%s", uuid);
    snprintf(e->set, sizeof(e->set), "%s", set);
}

const struct be_env *be_for_set(const struct be_file *b, const char *set)
{
    for (int i = b->n - 1; i >= 0; i--)
        if (strcmp(b->env[i].set, set) == 0)
            return &b->env[i];
    return NULL;
}

void be_name(const char *tag, bool (*taken)(const char *name, void *arg), void *arg, char out[BE_NAME_MAX + 1])
{
    char base[BE_NAME_MAX + 1];
    int n = snprintf(base, sizeof(base), "be-");
    for (const char *c = tag; *c && n < BE_NAME_MAX - 3; c++)   /* room for a "-NN" suffix */
        base[n++] = (*c == '/' || (unsigned char)*c <= ' ') ? '_' : *c;
    if (n == 3)
        n += snprintf(base + n, sizeof(base) - (size_t)n, "unknown");
    base[n] = '\0';
    snprintf(out, BE_NAME_MAX + 1, "%s", base);
    for (int k = 2; taken != NULL && taken(out, arg) && k < 100; k++)
        snprintf(out, BE_NAME_MAX + 1, "%s-%d", base, k);
}
