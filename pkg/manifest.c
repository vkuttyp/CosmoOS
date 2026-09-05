/* manifest.c - Manifest and index parsing (docs/pkg/design.md "Formats"). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pkg.h"

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool hex_decode(const char *hex, uint8_t *out, size_t n)
{
    if (strlen(hex) != 2 * n)
        return false;
    for (size_t i = 0; i < n; i++) {
        int hi = hexval(hex[2 * i]), lo = hexval(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out[i] = (uint8_t)(hi * 16 + lo);
    }
    return hex[2 * n] == '\0';
}

void hex_encode(const uint8_t *in, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 15];
    }
    out[2 * n] = '\0';
}

bool path_allowed(const char *path)
{
    size_t n = strlen(path);
    if (n == 0 || n >= PKG_PATH_MAX || path[0] == '/' || path[n - 1] == '/')
        return false;
    const char *p = path;
    while (*p) {
        const char *e = strchr(p, '/');
        size_t cl = e ? (size_t)(e - p) : strlen(p);
        if (cl == 0 || (cl == 1 && p[0] == '.') || (cl == 2 && p[0] == '.' && p[1] == '.'))
            return false;
        for (size_t i = 0; i < cl; i++)
            if ((unsigned char)p[i] < 0x21 || (unsigned char)p[i] > 0x7e)
                return false;
        p = e ? e + 1 : p + cl;
    }
    static const char *const forbidden[] = { "boot/", "dev/", "tmp/", "mnt/" };
    for (size_t i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); i++)
        if (strncmp(path, forbidden[i], strlen(forbidden[i])) == 0)
            return false;
    return true;
}

/* One "key: value" line out of `text`; advances *pos past the newline.
 * Returns 1 (a line, possibly blank), 0 at the end, -1 for a malformed line. */
static int next_line(const char *text, size_t len, size_t *pos, char *key, size_t keylen, char *value,
                     size_t vallen, bool *blank)
{
    memset(key, 0, keylen);
    memset(value, 0, vallen);
    if (*pos >= len)
        return 0;
    size_t end = *pos;
    while (end < len && text[end] != '\n')
        end++;
    size_t n = end - *pos;
    const char *line = text + *pos;
    *pos = end < len ? end + 1 : end;
    *blank = n == 0;
    key[0] = value[0] = '\0';
    if (n == 0)
        return 1;
    size_t colon = 0;
    while (colon < n && line[colon] != ':')
        colon++;
    if (colon == n || colon == 0 || colon >= keylen)
        return -1;
    memcpy(key, line, colon);
    key[colon] = '\0';
    size_t vs = colon + 1;
    while (vs < n && line[vs] == ' ')
        vs++;
    if (n - vs >= vallen)
        return -1;
    memcpy(value, line + vs, n - vs);
    value[n - vs] = '\0';
    return 1;
}

/* Split `value` into exactly `n` space-separated fields. */
static bool split_fields(const char *value, char *fields[], const size_t caps[], int n)
{
    const char *p = value;
    for (int i = 0; i < n; i++) {
        while (*p == ' ')
            p++;
        size_t len = 0;
        while (p[len] && p[len] != ' ')
            len++;
        if (len == 0 || len >= caps[i])
            return false;
        memcpy(fields[i], p, len);
        fields[i][len] = '\0';
        p += len;
    }
    while (*p == ' ')
        p++;
    return *p == '\0';
}

static bool set_str(char *dst, size_t cap, const char *src)
{
    if (strlen(src) >= cap)
        return false;
    strcpy(dst, src);
    return true;
}

int manifest_parse(const char *text, size_t len, struct manifest *m, char *err, size_t errlen)
{
    memset(m, 0, sizeof(*m));
    m->files = calloc(PKG_MAX_FILES, sizeof(*m->files));
    if (m->files == NULL) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    size_t pos = 0;
    char key[32], value[1024];
    bool blank;
    int lineno = 0;
    int got;
    while ((got = next_line(text, len, &pos, key, sizeof(key), value, sizeof(value), &blank)) != 0) {
        lineno++;
        if (got < 0) {
            snprintf(err, errlen, "manifest line %d: not 'key: value'", lineno);
            manifest_free(m);
            return -1;
        }
        if (blank)
            continue;
        if (strcmp(key, "name") == 0) {
            if (!name_valid(value) || !set_str(m->name, sizeof(m->name), value))
                goto bad;
        } else if (strcmp(key, "version") == 0) {
            struct version v;
            if (!version_parse(value, &v) || !set_str(m->version, sizeof(m->version), value))
                goto bad;
        } else if (strcmp(key, "summary") == 0) {
            if (!set_str(m->summary, sizeof(m->summary), value))
                goto bad;
        } else if (strcmp(key, "depends") == 0) {
            if (m->ndepends == PKG_MAX_DEPENDS || !depend_parse(value, &m->depends[m->ndepends]))
                goto bad;
            m->ndepends++;
        } else if (strcmp(key, "file") == 0) {
            if (m->nfiles == PKG_MAX_FILES)
                goto bad;
            struct file_entry *f = &m->files[m->nfiles];
            char path[PKG_PATH_MAX], mode[8], size[24], sha[132];
            char *fields[4] = { path, mode, size, sha };
            size_t caps[4] = { sizeof(path), sizeof(mode), sizeof(size), sizeof(sha) };
            if (!split_fields(value, fields, caps, 4))
                goto bad;
            if (!path_allowed(path))
                goto bad;
            strcpy(f->path, path);
            char *end;
            unsigned long md = strtoul(mode, &end, 8);
            if (*end || md > 07777)
                goto bad;
            f->mode = (unsigned)md;
            unsigned long long sz = strtoull(size, &end, 10);
            if (*end || sz > PKG_PACKAGE_LIMIT)
                goto bad;
            f->size = sz;
            if (!hex_decode(sha, f->sha, SHA512_LEN))
                goto bad;
            m->nfiles++;
        } else if (strcmp(key, "dir") == 0) {
            /* installed records only: a directory this package created */
            if (m->dirs == NULL) {
                m->dirs = calloc(1, sizeof(*m->dirs));
                if (m->dirs == NULL)
                    goto bad;
            }
            if (value[0] != '/' || strlen(value) >= PKG_PATH_MAX || m->dirs->n == PKG_MAX_DIRS)
                goto bad;
            strcpy(m->dirs->paths[m->dirs->n++], value);
        } else {
            goto bad;
        }
        continue;
    bad:
        snprintf(err, errlen, "manifest line %d: bad '%s' entry", lineno, key);
        manifest_free(m);
        return -1;
    }
    if (m->name[0] == '\0' || m->version[0] == '\0') {
        snprintf(err, errlen, "manifest lacks name or version");
        manifest_free(m);
        return -1;
    }
    return 0;
}

void manifest_free(struct manifest *m)
{
    free(m->files);
    free(m->dirs);
    m->files = NULL;
    m->dirs = NULL;
    m->nfiles = 0;
}

int manifest_format(const struct manifest *m, const struct dirlist *dirs, char *out, size_t cap)
{
    size_t used = 0;
#define PUT(...)                                                                          \
    do {                                                                                  \
        int _n = snprintf(out + used, cap > used ? cap - used : 0, __VA_ARGS__);         \
        if (_n < 0 || (size_t)_n >= cap - used)                                           \
            return -1;                                                                    \
        used += (size_t)_n;                                                               \
    } while (0)
    PUT("name: %s\nversion: %s\nsummary: %s\n", m->name, m->version, m->summary);
    for (int i = 0; i < m->ndepends; i++) {
        if (m->depends[i].op == OP_NONE)
            PUT("depends: %s\n", m->depends[i].name);
        else
            PUT("depends: %s %s %s\n", m->depends[i].name, op_text(m->depends[i].op), m->depends[i].version);
    }
    for (int i = 0; i < m->nfiles; i++) {
        char hex[2 * SHA512_LEN + 1];
        hex_encode(m->files[i].sha, SHA512_LEN, hex);
        PUT("file: %s %04o %llu %s\n", m->files[i].path, m->files[i].mode, (unsigned long long)m->files[i].size,
            hex);
    }
    if (dirs)
        for (int i = 0; i < dirs->n; i++)
            PUT("dir: %s\n", dirs->paths[i]);
#undef PUT
    return (int)used;
}

int index_parse(const char *text, size_t len, struct index *ix, char *err, size_t errlen)
{
    memset(ix, 0, sizeof(*ix));
    ix->entries = calloc(PKG_MAX_INDEX, sizeof(*ix->entries));
    if (ix->entries == NULL) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    size_t pos = 0;
    char key[32], value[1024];
    bool blank;
    int lineno = 0;
    bool in_stanza = false;
    struct index_entry *e = NULL;
    int got;
    while ((got = next_line(text, len, &pos, key, sizeof(key), value, sizeof(value), &blank)) != 0) {
        lineno++;
        if (got < 0) {
            snprintf(err, errlen, "index line %d: not 'key: value'", lineno);
            index_free(ix);
            return -1;
        }
        if (blank) {
            in_stanza = false;
            continue;
        }
        if (!in_stanza) {
            if (ix->n == PKG_MAX_INDEX) {
                snprintf(err, errlen, "index has more than %d entries", PKG_MAX_INDEX);
                index_free(ix);
                return -1;
            }
            e = &ix->entries[ix->n++];
            in_stanza = true;
        }
        if (strcmp(key, "name") == 0) {
            if (!name_valid(value) || !set_str(e->name, sizeof(e->name), value))
                goto bad;
        } else if (strcmp(key, "version") == 0) {
            struct version v;
            if (!version_parse(value, &v) || !set_str(e->version, sizeof(e->version), value))
                goto bad;
        } else if (strcmp(key, "summary") == 0) {
            if (!set_str(e->summary, sizeof(e->summary), value))
                goto bad;
        } else if (strcmp(key, "depends") == 0) {
            if (e->ndepends == PKG_MAX_DEPENDS || !depend_parse(value, &e->depends[e->ndepends]))
                goto bad;
            e->ndepends++;
        } else if (strcmp(key, "file") == 0) {
            if (strchr(value, '/') || !set_str(e->file, sizeof(e->file), value) || value[0] == '.')
                goto bad;
        } else if (strcmp(key, "size") == 0) {
            char *end;
            unsigned long long sz = strtoull(value, &end, 10);
            if (*end || sz == 0 || sz > PKG_PACKAGE_LIMIT)
                goto bad;
            e->size = sz;
        } else if (strcmp(key, "sha512") == 0) {
            if (!hex_decode(value, e->sha, SHA512_LEN))
                goto bad;
        } else {
            goto bad;
        }
        continue;
    bad:
        snprintf(err, errlen, "index line %d: bad '%s' entry", lineno, key);
        index_free(ix);
        return -1;
    }
    for (int i = 0; i < ix->n; i++) {
        e = &ix->entries[i];
        if (e->name[0] == '\0' || e->version[0] == '\0' || e->file[0] == '\0' || e->size == 0) {
            snprintf(err, errlen, "index entry %d is incomplete", i + 1);
            index_free(ix);
            return -1;
        }
    }
    return 0;
}

void index_free(struct index *ix)
{
    free(ix->entries);
    ix->entries = NULL;
    ix->n = 0;
}
