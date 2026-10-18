/* manifest.c - Manifest and index parsing (docs/pkg/design.md "Formats"). */

#include <errno.h>
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

/* --- time (YYYY-MM-DDTHH:MM:SSZ, UTC) --------------------------------------------- */

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

void time_format(int64_t t, char out[21])
{
    int64_t days = t >= 0 ? t / 86400 : (t - 86399) / 86400;
    int64_t secs = t - days * 86400;
    /* civil_from_days */
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    unsigned doe = (unsigned)(days - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    snprintf(out, 21, "%04lld-%02u-%02uT%02u:%02u:%02uZ", (long long)y, m, d, (unsigned)(secs / 3600),
             (unsigned)(secs / 60 % 60), (unsigned)(secs % 60));
}

/* --- the index's header (PK10) ------------------------------------------------------ */

static bool header_key(const char *key)
{
    return strcmp(key, "serial") == 0 || strcmp(key, "published") == 0 || strcmp(key, "expires") == 0;
}

/* The header stanza, when the index starts with one; *after is where the
 * package stanzas begin (0 without a header). */
static int header_scan(const char *text, size_t len, struct index_header *h, size_t *after, char *err,
                       size_t errlen)
{
    memset(h, 0, sizeof(*h));
    *after = 0;
    size_t pos = 0;
    char key[32], value[1024];
    bool blank, seen_serial = false, seen_pub = false, seen_exp = false;
    int lineno = 0, got;
    for (;;) {
        got = next_line(text, len, &pos, key, sizeof(key), value, sizeof(value), &blank);
        lineno++;
        if (got == 0 || blank) {
            if (!h->present)
                return 0;
            break;
        }
        if (got < 0) {
            if (!h->present)
                return 0;   /* index_parse reports it */
            snprintf(err, errlen, "index line %d: not 'key: value'", lineno);
            return -1;
        }
        if (!header_key(key)) {
            if (!h->present)
                return 0;   /* no header: the first stanza is a package */
            snprintf(err, errlen, "index line %d: '%s' in the header", lineno, key);
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
        snprintf(err, errlen, "index line %d: bad '%s' header", lineno, key);
        return -1;
    }
    if (!seen_serial || !seen_pub || !seen_exp) {
        snprintf(err, errlen, "the index header lacks %s", !seen_serial ? "serial" : !seen_pub ? "published" : "expires");
        return -1;
    }
    *after = pos;
    return 0;
}

int index_header_parse(const char *text, size_t len, struct index_header *h, char *err, size_t errlen)
{
    size_t after;
    return header_scan(text, len, h, &after, err, errlen);
}

int index_fresh(const struct index_header *h, uint64_t last_serial, int64_t now, char *err, size_t errlen)
{
    char a[21], b[21];
    if (!h->present) {
        snprintf(err, errlen, "no serial and expiry (refused from a network repository)");
        return -1;
    }
    if (h->expires <= h->published || h->expires - h->published > (int64_t)PKG_INDEX_MAX_DAYS * 86400) {
        time_format(h->published, a);
        time_format(h->expires, b);
        snprintf(err, errlen, "valid from %s to %s, not for at most %d days", a, b, PKG_INDEX_MAX_DAYS);
        return -1;
    }
    if (now < h->published) {
        time_format(now, a);
        time_format(h->published, b);
        snprintf(err, errlen, "the clock (%s) is before the index was published (%s): set the clock (date -s) "
                 "and update again", a, b);
        return -1;
    }
    if (now >= h->expires) {
        time_format(h->expires, a);
        snprintf(err, errlen, "expired at %s", a);
        return -1;
    }
    if (h->serial < last_serial) {
        snprintf(err, errlen, "serial %llu is older than %llu, the last accepted from this repository "
                 "(a replayed index)", (unsigned long long)h->serial, (unsigned long long)last_serial);
        return -1;
    }
    return 0;
}

/* --- repos.conf ------------------------------------------------------------------------ */

int repo_parse(const char *line, int nlocal, struct repo *r, char *err, size_t errlen)
{
    char buf[512];
    char *tok[8];
    int n = 0;
    memset(r, 0, sizeof(*r));
    if (strlen(line) >= sizeof(buf)) {
        snprintf(err, errlen, "line too long");
        return -1;
    }
    strcpy(buf, line);
    for (char *p = buf; *p;) {
        while (*p == ' ' || *p == '\t')
            *p++ = '\0';
        if (*p == '\0')
            break;
        if (n == 8) {
            snprintf(err, errlen, "more than 8 words");
            return -1;
        }
        tok[n++] = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
    }
    if (n == 1 && tok[0][0] == '/') {   /* the form before P6: a directory */
        if (nlocal == 0)
            snprintf(r->name, sizeof(r->name), "local");
        else
            snprintf(r->name, sizeof(r->name), "local%d", nlocal + 1);
        if (!set_str(r->dir, sizeof(r->dir), tok[0]) || snprintf(r->url, sizeof(r->url), "file://%s", tok[0]) >=
                                                            (int)sizeof(r->url)) {
            snprintf(err, errlen, "path too long");
            return -1;
        }
        r->local = true;
        goto strip;
    }
    if (n < 2) {
        snprintf(err, errlen, "not 'NAME URL [pin sha256:HEX] [insecure]'");
        return -1;
    }
    if (!name_valid(tok[0]) || !set_str(r->name, sizeof(r->name), tok[0])) {
        snprintf(err, errlen, "'%s' is not a repository name", tok[0]);
        return -1;
    }
    if (!set_str(r->url, sizeof(r->url), tok[1])) {
        snprintf(err, errlen, "URL too long");
        return -1;
    }
    for (int i = 2; i < n; i++) {
        if (strcmp(tok[i], "insecure") == 0) {
            r->insecure = true;
        } else if (strcmp(tok[i], "pin") == 0 && i + 1 < n) {
            const char *h = tok[++i];
            if (r->pin[0]) {
                snprintf(err, errlen, "a second pin");
                return -1;
            }
            if (strncmp(h, "sha256:", 7) != 0 || strlen(h + 7) != 64 || strspn(h + 7, "0123456789abcdef") != 64) {
                snprintf(err, errlen, "pin takes sha256: and 64 lowercase hex digits");
                return -1;
            }
            snprintf(r->pin, sizeof(r->pin), "%s", h);
        } else {
            snprintf(err, errlen, "unknown option '%s'", tok[i]);
            return -1;
        }
    }
    if (strncmp(r->url, "file:///", 8) == 0) {
        r->local = true;
        if (!set_str(r->dir, sizeof(r->dir), r->url + 7)) {
            snprintf(err, errlen, "path too long");
            return -1;
        }
        if (r->pin[0] || r->insecure) {
            snprintf(err, errlen, "pin and insecure are for network repositories");
            return -1;
        }
    } else if (strncmp(r->url, "http://", 7) == 0) {
        if (!r->insecure) {
            snprintf(err, errlen, "http:// needs 'insecure' (use https://)");
            return -1;
        }
        if (r->pin[0]) {
            snprintf(err, errlen, "a pin needs https://");
            return -1;
        }
    } else if (strncmp(r->url, "https://", 8) != 0) {
        snprintf(err, errlen, "'%s' is not a file:///, https:// or http:// URL", r->url);
        return -1;
    }
strip:;
    size_t ul = strlen(r->url), dl = strlen(r->dir);
    while (ul > 8 && r->url[ul - 1] == '/')
        r->url[--ul] = '\0';
    while (dl > 1 && r->dir[dl - 1] == '/')
        r->dir[--dl] = '\0';
    return 0;
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
    if (header_scan(text, len, &ix->header, &pos, err, errlen) < 0) {
        index_free(ix);
        return -1;
    }
    char key[32], value[1024];
    bool blank;
    int lineno = 0;
    for (size_t i = 0; i < pos; i++)
        lineno += text[i] == '\n';
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
