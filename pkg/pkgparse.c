/*
 * pkgparse.c - The package manager's decoders (pkgparse.h): the ustar
 * reader, versions and dependencies, names, paths, sonames, hex, times,
 * manifests and the INDEX (docs/pkg/design.md "Formats").
 */

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "pkgparse.h"

/* --- names, versions, dependencies ------------------------------------------------------ */

bool name_valid(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n >= PKG_NAME_MAX)
        return false;
    if (!(islower((unsigned char)s[0]) || isdigit((unsigned char)s[0])))
        return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!(islower((unsigned char)c) || isdigit((unsigned char)c) || c == '+' || c == '.' || c == '_' || c == '-'))
            return false;
    }
    return true;
}

bool version_parse(const char *s, struct version *v)
{
    memset(v, 0, sizeof(*v));
    if (*s == '\0' || strlen(s) >= PKG_VERSION_MAX)
        return false;
    const char *p = s;
    for (;;) {
        if (!isdigit((unsigned char)*p) || v->ncomp == 8)
            return false;
        unsigned n = 0;
        while (isdigit((unsigned char)*p)) {
            if (n > 100000000u)
                return false;
            n = n * 10 + (unsigned)(*p++ - '0');
        }
        v->comp[v->ncomp++] = n;
        if (*p == '.') {
            p++;
            continue;
        }
        break;
    }
    if (*p == '-') {
        p++;
        if (!isdigit((unsigned char)*p))
            return false;
        unsigned n = 0;
        while (isdigit((unsigned char)*p)) {
            if (n > 100000000u)
                return false;
            n = n * 10 + (unsigned)(*p++ - '0');
        }
        v->rev = n;
    }
    return *p == '\0';
}

int version_cmp(const char *a, const char *b)
{
    struct version va, vb;
    if (!version_parse(a, &va) || !version_parse(b, &vb))
        return strcmp(a, b) < 0 ? -1 : strcmp(a, b) > 0;
    int n = va.ncomp > vb.ncomp ? va.ncomp : vb.ncomp;
    for (int i = 0; i < n; i++) {
        unsigned x = i < va.ncomp ? va.comp[i] : 0;
        unsigned y = i < vb.ncomp ? vb.comp[i] : 0;
        if (x != y)
            return x < y ? -1 : 1;
    }
    if (va.rev != vb.rev)
        return va.rev < vb.rev ? -1 : 1;
    return 0;
}

bool depend_parse(const char *text, struct depend *d)
{
    memset(d, 0, sizeof(*d));
    const char *p = text;
    while (*p == ' ')
        p++;
    size_t n = 0;
    while (*p && *p != ' ' && *p != '=' && *p != '<' && *p != '>' && n < PKG_NAME_MAX - 1)
        d->name[n++] = *p++;
    d->name[n] = '\0';
    if (!name_valid(d->name))
        return false;
    while (*p == ' ')
        p++;
    if (*p == '\0')
        return true;
    if (p[0] == '>' && p[1] == '=') {
        d->op = OP_GE;
        p += 2;
    } else if (p[0] == '<' && p[1] == '=') {
        d->op = OP_LE;
        p += 2;
    } else if (*p == '=') {
        d->op = OP_EQ;
        p++;
    } else if (*p == '<') {
        d->op = OP_LT;
        p++;
    } else if (*p == '>') {
        d->op = OP_GT;
        p++;
    } else {
        return false;
    }
    while (*p == ' ')
        p++;
    n = 0;
    while (*p && *p != ' ' && n < PKG_VERSION_MAX - 1)
        d->version[n++] = *p++;
    d->version[n] = '\0';
    while (*p == ' ')
        p++;
    struct version v;
    return *p == '\0' && version_parse(d->version, &v);
}

bool depend_satisfied(const struct depend *d, const char *version)
{
    int c = version_cmp(version, d->version);
    switch (d->op) {
    case OP_NONE: return true;
    case OP_EQ: return c == 0;
    case OP_GE: return c >= 0;
    case OP_LE: return c <= 0;
    case OP_LT: return c < 0;
    case OP_GT: return c > 0;
    }
    return false;
}

/* --- hex, paths, sonames ------------------------------------------------------------------ */

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

/* A library's soname: name characters, and ".so" in it (libc.so,
 * libz.so.1). */
bool soname_valid(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n >= PKG_SONAME_MAX || strstr(s, ".so") == NULL || s[0] == '.')
        return false;
    for (const char *p = s; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
              c == '-' || c == '+'))
            return false;
    }
    return true;
}

/* --- time (YYYY-MM-DDTHH:MM:SSZ, UTC) ------------------------------------------------------ */

/* Days since 1970-01-01 of a proleptic Gregorian date (Howard Hinnant's
 * days_from_civil). */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static bool digits(const char *s, int n, unsigned *out)
{
    unsigned v = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return false;
        v = v * 10 + (unsigned)(s[i] - '0');
    }
    *out = v;
    return true;
}

bool time_parse(const char *s, int64_t *t)
{
    static const unsigned mdays[] = { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    unsigned y, mo, d, h, mi, se;
    if (strlen(s) != 20 || s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':' ||
        s[19] != 'Z')
        return false;
    if (!digits(s, 4, &y) || !digits(s + 5, 2, &mo) || !digits(s + 8, 2, &d) || !digits(s + 11, 2, &h) ||
        !digits(s + 14, 2, &mi) || !digits(s + 17, 2, &se))
        return false;
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > mdays[mo - 1] || h > 23 || mi > 59 || se > 59)
        return false;
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    if (mo == 2 && d == 29 && !leap)
        return false;
    *t = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se;
    return true;
}

/* --- the ustar reader ---------------------------------------------------------------------- */

#define BLOCK 512u

static bool octal_field(const uint8_t *f, size_t n, uint64_t *out)
{
    uint64_t v = 0;
    size_t i = 0;
    while (i < n && f[i] == ' ')
        i++;
    if (i == n || f[i] < '0' || f[i] > '7')
        return false;
    for (; i < n && f[i] >= '0' && f[i] <= '7'; i++) {
        if (v > (UINT64_MAX >> 3))
            return false;
        v = v * 8 + (uint64_t)(f[i] - '0');
    }
    for (; i < n; i++)
        if (f[i] != '\0' && f[i] != ' ')
            return false;
    *out = v;
    return true;
}

int tar_decode(struct tar_reader *r, struct tar_decoded *d)
{
    if (r->pos == r->len)
        return TAR_DEC_END;   /* no end-of-archive blocks: the buffer simply ends */
    if (r->pos + BLOCK > r->len)
        return TAR_DEC_TRUNCATED;
    const uint8_t *h = r->buf + r->pos;
    bool zero = true;
    for (size_t i = 0; i < BLOCK && zero; i++)
        zero = h[i] == 0;
    if (zero)
        return TAR_DEC_END;   /* end of archive */
    if (memcmp(h + 257, "ustar", 5) != 0)
        return TAR_DEC_NOT_USTAR;
    /* Header checksum: the sum of all bytes with the checksum field as spaces. */
    uint64_t want;
    if (!octal_field(h + 148, 8, &want))
        return TAR_DEC_CHECKSUM_FIELD;
    uint64_t sum = 0;
    for (size_t i = 0; i < BLOCK; i++)
        sum += (i >= 148 && i < 156) ? ' ' : h[i];
    if (sum != want)
        return TAR_DEC_CHECKSUM;
    if (h[156] != '0' && h[156] != '\0')
        return TAR_DEC_NOT_REGULAR;
    uint64_t mode, size;
    if (!octal_field(h + 100, 8, &mode) || !octal_field(h + 124, 12, &size))
        return TAR_DEC_MODE_SIZE;
    size_t nlen = 0;
    while (nlen < 100 && h[nlen])
        nlen++;
    if (nlen == 0 || h[345] != '\0')   /* no prefix field: names are 100 bytes at most */
        return TAR_DEC_NAME;
    memcpy(r->name, h, nlen);
    r->name[nlen] = '\0';
    size_t data = r->pos + BLOCK;
    size_t padded = (size_t)((size + BLOCK - 1) / BLOCK * BLOCK);
    if (size > r->len || data + padded > r->len)
        return TAR_DEC_PAST_END;
    d->mode = (unsigned)(mode & 07777);
    d->size = size;
    d->data = data;
    d->next = data + padded;
    return TAR_DEC_MEMBER;
}

/* --- manifests and the index --------------------------------------------------------------- */

/* One "key: value" line out of `text`; advances *pos past the newline.
 * Returns 1 (a line, possibly blank), 0 at the end, -1 for a malformed line.
 * *key_at is where the key starts in `text`. */
static int next_line(const char *text, size_t len, size_t *pos, char *key, size_t keylen, char *value,
                     size_t vallen, bool *blank, size_t *key_at)
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
    *key_at = *pos;
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

static void refuse(struct pkg_perr *pe, int why, int line, size_t key_at, const char *key)
{
    pe->why = why;
    pe->line = line;
    pe->key_at = key_at;
    pe->key_len = key ? strlen(key) : 0;
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

int manifest_decode(const char *text, size_t len, struct manifest *m, char *provides, char *requires, char *dirs,
                    struct pkg_perr *pe)
{
    size_t pos = 0, key_at = 0;
    char key[32], value[1024];
    bool blank;
    int lineno = 0;
    int got;
    while ((got = next_line(text, len, &pos, key, sizeof(key), value, sizeof(value), &blank, &key_at)) != 0) {
        lineno++;
        if (got < 0) {
            refuse(pe, PKG_PE_LINE, lineno, key_at, NULL);
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
        } else if (strcmp(key, "provides") == 0) {
            if (m->nprovides == PKG_MAX_LIBS || !soname_valid(value))
                goto bad;
            strcpy(provides + PKG_SONAME_MAX * m->nprovides++, value);
        } else if (strcmp(key, "requires") == 0) {
            if (m->nrequires == PKG_MAX_LIBS || !soname_valid(value))
                goto bad;
            strcpy(requires + PKG_SONAME_MAX * m->nrequires++, value);
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
            if (value[0] != '/' || strlen(value) >= PKG_PATH_MAX || m->dirs->n == PKG_MAX_DIRS)
                goto bad;
            strcpy(dirs + PKG_PATH_MAX * m->dirs->n++, value);
        } else {
            goto bad;
        }
        continue;
    bad:
        refuse(pe, PKG_PE_ENTRY, lineno, key_at, key);
        return -1;
    }
    if (m->name[0] == '\0' || m->version[0] == '\0') {
        refuse(pe, PKG_PE_NAME_VERSION, lineno, 0, NULL);
        return -1;
    }
    return 0;
}

static bool header_key(const char *key)
{
    return strcmp(key, "serial") == 0 || strcmp(key, "published") == 0 || strcmp(key, "expires") == 0;
}

/* The header stanza, when the index starts with one; *after is where the
 * package stanzas begin (0 without a header). */
static int header_scan(const char *text, size_t len, struct index_header *h, size_t *after, struct pkg_perr *pe)
{
    memset(h, 0, sizeof(*h));
    *after = 0;
    size_t pos = 0, key_at = 0;
    char key[32], value[1024];
    bool blank, seen_serial = false, seen_pub = false, seen_exp = false;
    int lineno = 0, got;
    for (;;) {
        got = next_line(text, len, &pos, key, sizeof(key), value, sizeof(value), &blank, &key_at);
        lineno++;
        if (got == 0 || blank) {
            if (!h->present)
                return 0;
            break;
        }
        if (got < 0) {
            if (!h->present)
                return 0;   /* index_decode reports it */
            refuse(pe, PKG_PE_LINE, lineno, key_at, NULL);
            return -1;
        }
        if (!header_key(key)) {
            if (!h->present)
                return 0;   /* no header: the first stanza is a package */
            refuse(pe, PKG_PE_HEADER_KEY, lineno, key_at, key);
            return -1;
        }
        h->present = true;
        if (strcmp(key, "serial") == 0) {
            char *end;
            errno = 0;
            unsigned long long v = strtoull(value, &end, 10);
            if (seen_serial || value[0] < '1' || value[0] > '9' || *end || v == 0 || errno == ERANGE)
                goto bad;
            h->serial = v;
            seen_serial = true;
        } else if (strcmp(key, "published") == 0) {
            if (seen_pub || !time_parse(value, &h->published))
                goto bad;
            seen_pub = true;
        } else {
            if (seen_exp || !time_parse(value, &h->expires))
                goto bad;
            seen_exp = true;
        }
        continue;
    bad:
        refuse(pe, PKG_PE_HEADER_BAD, lineno, key_at, key);
        return -1;
    }
    if (!seen_serial || !seen_pub || !seen_exp) {
        refuse(pe, !seen_serial ? PKG_PE_LACKS_SERIAL : !seen_pub ? PKG_PE_LACKS_PUBLISHED : PKG_PE_LACKS_EXPIRES,
               lineno, 0, NULL);
        return -1;
    }
    *after = pos;
    return 0;
}

int index_header_decode(const char *text, size_t len, struct index_header *h, struct pkg_perr *pe)
{
    size_t after;
    return header_scan(text, len, h, &after, pe);
}

int index_decode(const char *text, size_t len, struct index *ix, struct pkg_perr *pe)
{
    size_t pos = 0, key_at = 0;
    if (header_scan(text, len, &ix->header, &pos, pe) < 0)
        return -1;
    char key[32], value[1024];
    bool blank;
    int lineno = 0;
    for (size_t i = 0; i < pos; i++)
        lineno += text[i] == '\n';
    bool in_stanza = false;
    struct index_entry *e = NULL;
    int got;
    while ((got = next_line(text, len, &pos, key, sizeof(key), value, sizeof(value), &blank, &key_at)) != 0) {
        lineno++;
        if (got < 0) {
            refuse(pe, PKG_PE_LINE, lineno, key_at, NULL);
            return -1;
        }
        if (blank) {
            in_stanza = false;
            continue;
        }
        if (!in_stanza) {
            if (ix->n == PKG_MAX_INDEX) {
                refuse(pe, PKG_PE_TOO_MANY, lineno, 0, NULL);
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
        refuse(pe, PKG_PE_ENTRY, lineno, key_at, key);
        return -1;
    }
    for (int i = 0; i < ix->n; i++) {
        e = &ix->entries[i];
        if (e->name[0] == '\0' || e->version[0] == '\0' || e->file[0] == '\0' || e->size == 0) {
            refuse(pe, PKG_PE_INCOMPLETE, i + 1, 0, NULL);
            return -1;
        }
    }
    return 0;
}
