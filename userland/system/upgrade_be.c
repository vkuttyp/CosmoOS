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
    int w = snprintf(out, cap, "%s", BE_MAGIC);
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

bool be_aware(const unsigned char *bin, size_t len)
{
    size_t m = sizeof(BE_MAGIC) - 1;
    for (size_t i = 0; i + m <= len; i++)
        if (bin[i] == BE_MAGIC[0] && memcmp(bin + i, BE_MAGIC, m) == 0)
            return true;
    return false;
}

/* One line of `text` from `*pos`: its start and length, *pos past it. */
static bool next_line(const char *text, size_t len, size_t *pos, const char **l, size_t *ll)
{
    if (*pos >= len)
        return false;
    *l = text + *pos;
    const char *nl = memchr(*l, '\n', len - *pos);
    *ll = nl ? (size_t)(nl - *l) : len - *pos;
    *pos += *ll + 1;
    return true;
}

int rootset_parse(const char *text, size_t len, struct root_set *r)
{
    memset(r, 0, sizeof(*r));
    if (len < BE_SET_HEX || !all_hex(text, BE_SET_HEX) || (len > BE_SET_HEX && text[BE_SET_HEX] != '\n'))
        return -1;
    memcpy(r->set, text, BE_SET_HEX);
    r->version = 1;
    size_t pos = BE_SET_HEX + 1, m = sizeof(ROOTSET_MAGIC) - 1;
    if (len < pos + m || memcmp(text + pos, ROOTSET_MAGIC, m) != 0)
        return 0;   /* v1.0.8's record, or a later one's first line */
    pos += m;
    r->version = 2;
    const char *l;
    size_t ll;
    while (next_line(text, len, &pos, &l, &ll)) {
        if (ll > 8 && memcmp(l, "release ", 8) == 0 && ll - 8 <= ROOTSET_RELEASE_MAX) {
            memcpy(r->release, l + 8, ll - 8);
            r->release[ll - 8] = '\0';
        } else if (ll == 5 + BE_SET_HEX && memcmp(l, "from ", 5) == 0 && all_hex(l + 5, BE_SET_HEX)) {
            memcpy(r->from, l + 5, BE_SET_HEX);
            r->from[BE_SET_HEX] = '\0';
        }
    }
    if (strcmp(r->release, "-") == 0)
        r->release[0] = '\0';
    return 0;
}

size_t rootset_format(const struct root_set *r, char *out, size_t cap)
{
    int w = snprintf(out, cap, "%s\n%srelease %s\nfrom %s\n", r->set, ROOTSET_MAGIC, r->release[0] ? r->release : "-",
                     r->from[0] ? r->from : "-");
    return w < 0 || (size_t)w >= cap ? 0 : (size_t)w;
}

static bool word_char(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

/* Digits at `*i`, at least one; false otherwise. */
static bool digits(const unsigned char *s, size_t len, size_t *i)
{
    size_t start = *i;
    while (*i < len && s[*i] >= '0' && s[*i] <= '9')
        (*i)++;
    return *i > start;
}

void release_of(const unsigned char *elf, size_t len, char out[ROOTSET_RELEASE_MAX + 1])
{
    out[0] = '\0';
    for (size_t at = 0; at + 6 <= len; at++) {
        if (elf[at] != 'v' || (at > 0 && (word_char(elf[at - 1]) || elf[at - 1] == '.')))
            continue;
        size_t i = at + 1;
        if (!digits(elf, len, &i) || i >= len || elf[i++] != '.' || !digits(elf, len, &i) || i >= len ||
            elf[i++] != '.' || !digits(elf, len, &i))
            continue;
        size_t j = i;
        if (j + 3 < len && memcmp(elf + j, "-rc", 3) == 0 && (j += 3, digits(elf, len, &j)))
            i = j;
        j = i;
        if (j < len && elf[j] == '-' && (j++, digits(elf, len, &j)) && j + 2 < len && memcmp(elf + j, "-g", 2) == 0) {
            size_t h = j + 2;
            while (h < len && ((elf[h] >= '0' && elf[h] <= '9') || (elf[h] >= 'a' && elf[h] <= 'f')))
                h++;
            if (h - (j + 2) >= 7)
                i = h;
        }
        if (i + 6 <= len && memcmp(elf + i, "-dirty", 6) == 0)
            i += 6;
        if (i < len && (word_char(elf[i]) || elf[i] == '.' || elf[i] == '-'))
            continue;
        if (i - at > ROOTSET_RELEASE_MAX)
            continue;
        memcpy(out, elf + at, i - at);
        out[i - at] = '\0';
        return;
    }
}
