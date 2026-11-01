/*
 * manifest.c - Manifests and the index (docs/pkg/design.md "Formats"): the
 * allocation and the messages around the decoders in pkgparse.h, the text
 * a manifest is written as, times, the index's freshness and repos.conf.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pkgparse.h"

void hex_encode(const uint8_t *in, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 15];
    }
    out[2 * n] = '\0';
}

static bool set_str(char *dst, size_t cap, const char *src)
{
    if (strlen(src) >= cap)
        return false;
    strcpy(dst, src);
    return true;
}

const char *op_text(enum cmp_op op)
{
    switch (op) {
    case OP_EQ: return "=";
    case OP_GE: return ">=";
    case OP_LE: return "<=";
    case OP_LT: return "<";
    case OP_GT: return ">";
    default: return "";
    }
}

/* A decoder's refusal (pkgparse.h) in words; `what` is "manifest" or "index". */
static void perr_format(const char *what, const char *text, const struct pkg_perr *pe, char *err, size_t errlen)
{
    int kl = (int)pe->key_len;
    const char *key = text + pe->key_at;
    switch (pe->why) {
    case PKG_PE_LINE:
        snprintf(err, errlen, "%s line %d: not 'key: value'", what, pe->line);
        break;
    case PKG_PE_ENTRY:
        snprintf(err, errlen, "%s line %d: bad '%.*s' entry", what, pe->line, kl, key);
        break;
    case PKG_PE_NAME_VERSION:
        snprintf(err, errlen, "manifest lacks name or version");
        break;
    case PKG_PE_HEADER_KEY:
        snprintf(err, errlen, "index line %d: '%.*s' in the header", pe->line, kl, key);
        break;
    case PKG_PE_HEADER_BAD:
        snprintf(err, errlen, "index line %d: bad '%.*s' header", pe->line, kl, key);
        break;
    case PKG_PE_LACKS_SERIAL:
        snprintf(err, errlen, "the index header lacks serial");
        break;
    case PKG_PE_LACKS_PUBLISHED:
        snprintf(err, errlen, "the index header lacks published");
        break;
    case PKG_PE_LACKS_EXPIRES:
        snprintf(err, errlen, "the index header lacks expires");
        break;
    case PKG_PE_TOO_MANY:
        snprintf(err, errlen, "index has more than %d entries", PKG_MAX_INDEX);
        break;
    case PKG_PE_INCOMPLETE:
        snprintf(err, errlen, "index entry %d is incomplete", pe->line);
        break;
    default:
        snprintf(err, errlen, "%s refused", what);
        break;
    }
}

int manifest_parse(const char *text, size_t len, struct manifest *m, char *err, size_t errlen)
{
    memset(m, 0, sizeof(*m));
    /* "dir:" lines come only in installed records: the list is dropped again when there are none. */
    m->files = calloc(PKG_MAX_FILES, sizeof(*m->files));
    m->dirs = calloc(1, sizeof(*m->dirs));
    if (m->files == NULL || m->dirs == NULL) {
        snprintf(err, errlen, "out of memory");
        manifest_free(m);
        return -1;
    }
    struct pkg_perr pe;
    if (manifest_decode(text, len, m, m->provides[0], m->requires[0], m->dirs->paths[0], &pe) < 0) {
        perr_format("manifest", text, &pe, err, errlen);
        manifest_free(m);
        return -1;
    }
    if (m->dirs->n == 0) {
        free(m->dirs);
        m->dirs = NULL;
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
    for (int i = 0; i < m->nprovides; i++)
        PUT("provides: %s\n", m->provides[i]);
    for (int i = 0; i < m->nrequires; i++)
        PUT("requires: %s\n", m->requires[i]);
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

int index_header_parse(const char *text, size_t len, struct index_header *h, char *err, size_t errlen)
{
    struct pkg_perr pe;
    if (index_header_decode(text, len, h, &pe) < 0) {
        perr_format("index", text, &pe, err, errlen);
        return -1;
    }
    return 0;
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
        return -2;
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
    struct pkg_perr pe;
    if (index_decode(text, len, ix, &pe) < 0) {
        perr_format("index", text, &pe, err, errlen);
        index_free(ix);
        return -1;
    }
    return 0;
}

void index_free(struct index *ix)
{
    free(ix->entries);
    ix->entries = NULL;
    ix->n = 0;
}
