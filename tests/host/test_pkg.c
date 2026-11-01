/*
 * test_pkg.c - Host test of the package manager's parsers
 * (docs/pkg/testing.md): versions and constraints, manifests, the index,
 * path confinement, the ustar reader. Built with ASan and UBSan. The
 * decoders (pkgparse.h) are held to a recorded digest; PKG_BENCH=1 times
 * them.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pkgparse.h"

static int g_failures;
#define CHECK(c)                                                                          \
    do {                                                                                  \
        if (!(c)) {                                                                       \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                         \
            g_failures++;                                                                 \
        }                                                                                 \
    } while (0)

static void tar_header(unsigned char *h, const char *name, size_t size, unsigned mode)
{
    memset(h, 0, 512);
    memcpy(h, name, strlen(name));
    snprintf((char *)h + 100, 8, "%07o", mode);
    snprintf((char *)h + 108, 8, "%07o", 0);
    snprintf((char *)h + 116, 8, "%07o", 0);
    snprintf((char *)h + 124, 12, "%011zo", size);
    snprintf((char *)h + 136, 12, "%011o", 0);
    memset(h + 148, ' ', 8);
    h[156] = '0';
    memcpy(h + 257, "ustar\0" "00", 8);
    unsigned sum = 0;
    for (int i = 0; i < 512; i++)
        sum += h[i];
    snprintf((char *)h + 148, 8, "%06o", sum);
    h[154] = '\0';
    h[155] = ' ';
}

/* --- the decoders' digest (roadmap 2.0 SA5, the pkg family) ------------------------------- */

/* The digest of the C decoders (manifest.c, version.c and tar.c before the
 * split, and pkgparse.c after it; 2026-11-01), which the CeeSafe port
 * (pkgparse.cee) gives too: every parser over seeded
 * manifests, indexes, archives and strings, made near valid and then
 * damaged. What each one returned, the message and everything it decoded
 * go into an FNV-1a digest. */
#define PKG_DIGEST 0xced1250ae40a6b1cull
#define ROUNDS 20000u

static uint64_t g_rng, g_digest;

static uint64_t rnd(void)
{
    g_rng ^= g_rng >> 12;
    g_rng ^= g_rng << 25;
    g_rng ^= g_rng >> 27;
    return g_rng * 0x2545f4914f6cdd1dull;
}

static unsigned pick(unsigned n)
{
    return (unsigned)(rnd() % n);
}

static void h_bytes(const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        g_digest ^= b[i];
        g_digest *= 0x100000001b3ull;
    }
}

static void h_u64(uint64_t v)
{
    h_bytes(&v, sizeof(v));
}

static void h_str(const char *s, size_t cap)
{
    size_t n = 0;
    while (n < cap && s[n])
        n++;
    h_u64(n);
    h_bytes(s, n);
}

/* A growing text buffer. */
struct text {
    char b[32768];
    size_t n;
};

static void put(struct text *t, const char *s)
{
    size_t l = strlen(s);
    if (t->n + l <= sizeof(t->b)) {
        memcpy(t->b + t->n, s, l);
        t->n += l;
    }
}

static void put_hex(struct text *t, size_t digits)
{
    static const char hx[] = "0123456789abcdefABCDEF";
    char s[160];
    for (size_t i = 0; i < digits && i < sizeof(s) - 1; i++)
        s[i] = hx[pick(pick(8) == 0 ? 22 : 16)];
    s[digits < sizeof(s) - 1 ? digits : sizeof(s) - 1] = '\0';
    if (pick(16) == 0 && digits)
        s[pick((unsigned)digits)] = "gG -\0"[pick(5)];
    put(t, s);
}

static const char *const g_names[] = { "fortunes", "libz", "a", "0x", "Bad", "x+y.z_-1", "", "-x", "a b", "na\tme",
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklm" };
static const char *const g_versions[] = { "1.0", "1.2.3-4", "0", "", "1.", "1..2", "1.2.3.4.5.6.7.8", "1.2.3.4.5.6.7.8.9",
    "100000000", "1000000000", "4294967295", "1-", "1-2-3", "2.0-00", "1.0a", "01.002", "9.9.9-999999999",
    "1.2.3.4.5.6.7.8.9.10.11.12.13.14", "1-4294967295", "1.0-1", "1.0-2", "1-0" };
static const char *const g_ops[] = { "", " >= ", ">=", " <= ", "<", " > ", "=", " = ", " ~ ", " >", "=>" };
static const char *const g_sonames[] = { "libc.so", "libz.so.1", ".so", "libx", "lib@.so", "LIB+x-1_2.so.3", "",
    "libverylongnamelibverylongnamelibverylongnamelibverylongname.so" };
static const char *const g_paths[] = { "usr/bin/x", "boot/x", "/abs", "a/../b", "usr/", "a//b", "usr/lib/libz.so.1",
    "tmpx/y", "dev/null", "mnt", "a/./b", ".", "..", "etc/\x7f", "usr/share/doc/README", "x" };
static const char *const g_modes[] = { "0755", "644", "7777", "10000", "-0", "+7", "\t7", "08", "0", "0000000",
    "-1", "00007777", "777" };
static const char *const g_sizes[] = { "0", "5", "67108864", "67108865", "-0", "99999999999999999999999", "\v12", "12x",
    "+3", "-5", "18446744073709551615", " 9", "1e3" };
static const char *const g_serials[] = { "1", "0", "007", "18446744073709551615", "18446744073709551616", "5x", "+5",
    "42", "99999999999999999999" };
static const char *const g_keys[] = { "name", "version", "summary", "depends", "provides", "requires", "file", "dir",
    "size", "sha512", "serial", "published", "expires", "bogus", "Name", "abcdefghijklmnopqrstuvwxyzabcdef" };

static void put_version(struct text *t)
{
    if (pick(3) == 0) {
        put(t, g_versions[pick(sizeof(g_versions) / sizeof(g_versions[0]))]);
        return;
    }
    char s[64];
    int n = snprintf(s, sizeof(s), "%u", pick(4) ? pick(30) : (unsigned)rnd());
    for (unsigned c = pick(4); c > 0 && n < 40; c--)
        n += snprintf(s + n, sizeof(s) - (size_t)n, ".%u", pick(20));
    if (pick(3) == 0)
        snprintf(s + n, sizeof(s) - (size_t)n, "-%u", pick(9));
    put(t, s);
}

static void put_time(struct text *t)
{
    static const char *const odd[] = { "2026-02-29T00:00:00Z", "2024-02-29T23:59:59Z", "1969-12-31T23:59:59Z",
        "2026-13-01T00:00:00Z", "2026-04-31T00:00:00Z", "2026-01-01 00:00:00Z", "2026-01-01T24:00:00Z",
        "2026-01-01T00:00:00", "2100-02-29T00:00:00Z", "2000-02-29T12:00:00Z", "1970-01-01T00:00:00Z" };
    if (pick(3) == 0) {
        put(t, odd[pick(sizeof(odd) / sizeof(odd[0]))]);
        return;
    }
    char s[32];
    snprintf(s, sizeof(s), "%04u-%02u-%02uT%02u:%02u:%02uZ", 1960 + pick(200), pick(14), pick(33), pick(25),
             pick(61), pick(61));
    put(t, s);
}

static void put_value(struct text *t, const char *key)
{
    if (strcmp(key, "name") == 0) {
        put(t, g_names[pick(sizeof(g_names) / sizeof(g_names[0]))]);
    } else if (strcmp(key, "version") == 0) {
        put_version(t);
    } else if (strcmp(key, "summary") == 0) {
        static const char *const s[] = { "prints a line", "", "x: y",
            ("a summary of more than one hundred and twenty-eight characters, which is the most a summary may have "
             "in a manifest or an index entry, or so") };
        put(t, s[pick(4)]);
    } else if (strcmp(key, "depends") == 0) {
        put(t, g_names[pick(6)]);
        if (pick(3)) {
            put(t, g_ops[pick(sizeof(g_ops) / sizeof(g_ops[0]))]);
            put_version(t);
        }
        if (pick(10) == 0)
            put(t, " trailing");
    } else if (strcmp(key, "provides") == 0 || strcmp(key, "requires") == 0) {
        put(t, g_sonames[pick(sizeof(g_sonames) / sizeof(g_sonames[0]))]);
    } else if (strcmp(key, "file") == 0) {
        put(t, g_paths[pick(sizeof(g_paths) / sizeof(g_paths[0]))]);
        put(t, pick(8) ? " " : "  ");
        put(t, g_modes[pick(sizeof(g_modes) / sizeof(g_modes[0]))]);
        put(t, " ");
        put(t, g_sizes[pick(sizeof(g_sizes) / sizeof(g_sizes[0]))]);
        if (pick(12)) {
            put(t, " ");
            put_hex(t, pick(10) ? 128 : 126 + pick(5));
        }
        if (pick(12) == 0)
            put(t, " extra");
    } else if (strcmp(key, "dir") == 0) {
        static const char *const s[] = { "/usr/share/x", "rel", "/", "/a b" };
        if (pick(20) == 0)
            for (unsigned i = 0, n = PKG_PATH_MAX - 1 + pick(2); i < n; i++)
                put(t, i == 0 ? "/" : "d");
        else
            put(t, s[pick(4)]);
    } else if (strcmp(key, "size") == 0) {
        put(t, g_sizes[pick(sizeof(g_sizes) / sizeof(g_sizes[0]))]);
    } else if (strcmp(key, "sha512") == 0) {
        put_hex(t, pick(10) ? 128 : 126 + pick(5));
    } else if (strcmp(key, "serial") == 0) {
        put(t, g_serials[pick(sizeof(g_serials) / sizeof(g_serials[0]))]);
    } else if (strcmp(key, "published") == 0 || strcmp(key, "expires") == 0) {
        put_time(t);
    } else {
        put(t, "value");
    }
}

static void put_line(struct text *t, const char *key)
{
    put(t, key);
    put(t, pick(10) ? ": " : pick(2) ? ":" : ":   ");
    put_value(t, key);
    put(t, "\n");
}

/* A manifest or an index, near valid; then damaged. */
static void gen_doc(struct text *t)
{
    t->n = 0;
    if (pick(60) == 0) {   /* at and past a limit: depends, sonames, directories, index entries */
        static const char *const lines[] = { "depends: a\n", "provides: libc.so\n", "requires: libc.so\n",
            "dir: /a\n", "name: a\n\n" };
        static const unsigned limits[] = { PKG_MAX_DEPENDS, PKG_MAX_LIBS, PKG_MAX_LIBS, PKG_MAX_DIRS, PKG_MAX_INDEX };
        unsigned k = pick(5);
        if (k < 4)
            put(t, "name: a\nversion: 1\n");
        for (unsigned i = 0, n = limits[k] + pick(2); i < n; i++)
            put(t, lines[k]);
        return;
    }
    if (pick(3) == 0) {   /* an index's header */
        const char *order[3] = { "serial", "published", "expires" };
        unsigned n = 3 - (pick(6) == 0);
        for (unsigned i = 0; i < n; i++)
            put_line(t, order[(i + (pick(5) == 0)) % 3]);
        if (pick(8) == 0)
            put_line(t, "name");
        if (pick(10) == 0)
            return;   /* the header alone */
        put(t, "\n");
    }
    unsigned stanzas = 1 + pick(3);
    for (unsigned s = 0; s < stanzas; s++) {
        static const char *const base[] = { "name", "version", "summary", "file", "size", "sha512" };
        unsigned lines = 2 + pick(9);
        for (unsigned i = 0; i < lines; i++) {
            const char *key = i < 6 && pick(4) ? base[i] : g_keys[pick(sizeof(g_keys) / sizeof(g_keys[0]))];
            put_line(t, key);
        }
        if (s + 1 < stanzas)
            put(t, pick(10) ? "\n" : "\n\n");
    }
    if (pick(4) == 0 && t->n > 0)
        t->n--;   /* no final newline */
    for (unsigned d = pick(4) == 0 ? 1 + pick(3) : 0; d > 0 && t->n > 0; d--) {
        static const char bytes[] = { ' ', '\0', ':', '\n', '\t', '/', '.', '-', 'x', (char)0xff };
        t->b[pick((unsigned)t->n)] = bytes[pick(sizeof(bytes))];
    }
    if (pick(40) == 0) {   /* a line too long for the value buffer, or without a colon */
        put(t, "summary: ");
        for (unsigned i = 0; i < 1030 && t->n < sizeof(t->b); i++)
            t->b[t->n++] = 'v';
        put(t, "\n");
    }
    if (pick(40) == 0)
        put(t, "no colon here\n");
}

static void hash_depend(const struct depend *d)
{
    h_str(d->name, sizeof(d->name));
    h_u64((uint64_t)d->op);
    h_str(d->version, sizeof(d->version));
}

static void digest_doc(const struct text *t)
{
    char err[160];
    struct manifest m;
    memset(err, 0, sizeof(err));
    int rc = manifest_parse(t->b, t->n, &m, err, sizeof(err));
    h_u64((uint64_t)(int64_t)rc);
    if (rc < 0) {
        h_str(err, sizeof(err));
    } else {
        h_str(m.name, sizeof(m.name));
        h_str(m.version, sizeof(m.version));
        h_str(m.summary, sizeof(m.summary));
        h_u64((uint64_t)m.ndepends);
        for (int i = 0; i < m.ndepends; i++)
            hash_depend(&m.depends[i]);
        h_u64((uint64_t)m.nprovides);
        for (int i = 0; i < m.nprovides; i++)
            h_str(m.provides[i], sizeof(m.provides[i]));
        h_u64((uint64_t)m.nrequires);
        for (int i = 0; i < m.nrequires; i++)
            h_str(m.requires[i], sizeof(m.requires[i]));
        h_u64((uint64_t)m.nfiles);
        for (int i = 0; i < m.nfiles; i++) {
            h_str(m.files[i].path, sizeof(m.files[i].path));
            h_u64(m.files[i].mode);
            h_u64(m.files[i].size);
            h_bytes(m.files[i].sha, SHA512_LEN);
        }
        h_u64(m.dirs != NULL);
        if (m.dirs) {
            h_u64((uint64_t)m.dirs->n);
            for (int i = 0; i < m.dirs->n; i++)
                h_str(m.dirs->paths[i], sizeof(m.dirs->paths[i]));
        }
        manifest_free(&m);
    }

    struct index ix;
    memset(err, 0, sizeof(err));
    rc = index_parse(t->b, t->n, &ix, err, sizeof(err));
    h_u64((uint64_t)(int64_t)rc);
    if (rc < 0) {
        h_str(err, sizeof(err));
    } else {
        h_u64(ix.header.present);
        h_u64(ix.header.serial);
        h_u64((uint64_t)ix.header.published);
        h_u64((uint64_t)ix.header.expires);
        h_u64((uint64_t)ix.n);
        for (int i = 0; i < ix.n; i++) {
            const struct index_entry *e = &ix.entries[i];
            h_str(e->name, sizeof(e->name));
            h_str(e->version, sizeof(e->version));
            h_str(e->summary, sizeof(e->summary));
            h_str(e->file, sizeof(e->file));
            h_u64((uint64_t)e->ndepends);
            for (int j = 0; j < e->ndepends; j++)
                hash_depend(&e->depends[j]);
            h_u64(e->size);
            h_bytes(e->sha, SHA512_LEN);
            h_u64((uint64_t)e->repo);
        }
        index_free(&ix);
    }

    struct index_header h;
    memset(err, 0, sizeof(err));
    rc = index_header_parse(t->b, t->n, &h, err, sizeof(err));
    h_u64((uint64_t)(int64_t)rc);
    if (rc < 0)
        h_str(err, sizeof(err));
    else
        h_u64(h.present ? h.serial ^ (uint64_t)h.published * 3 ^ (uint64_t)h.expires * 7 : 1);
}

/* An archive of up to three members, then damaged. */
static size_t gen_tar(uint8_t *buf, size_t cap)
{
    size_t n = 0, heads[4];
    unsigned members = pick(4), nheads = 0;
    for (unsigned i = 0; i < members; i++) {
        size_t size = pick(3) == 0 ? 0 : pick(1200);
        if (n + 512 + (size + 511) / 512 * 512 > cap)
            break;
        char name[120];
        unsigned nl = pick(20) == 0 ? 100 + pick(3) : 1 + pick(30);
        for (unsigned j = 0; j < nl && j < sizeof(name) - 1; j++)
            name[j] = "abcdefghijklmnopqrstuvwxyz/._"[pick(29)];
        name[nl < sizeof(name) - 1 ? nl : sizeof(name) - 1] = '\0';
        if (nl > 100)
            name[100] = '\0';
        heads[nheads++] = n;
        tar_header(buf + n, name, size, pick(4) ? 0644 : (unsigned)(rnd() & 077777));
        if (pick(8) == 0) {   /* fields padded with spaces before the digits, or only spaces */
            uint8_t *f = buf + n + (pick(2) ? 100 : 124);
            size_t w = f == buf + n + 100 ? 7 : 11, sp = pick(2) ? 1 + pick(3) : w;
            memmove(f + sp, f, w - (sp < w ? sp : w));
            memset(f, ' ', sp < w ? sp : w);
            uint8_t *h = buf + n;
            memset(h + 148, ' ', 8);
            unsigned sum = 0;
            for (int j = 0; j < 512; j++)
                sum += h[j];
            snprintf((char *)h + 148, 8, "%06o", sum);
            h[155] = ' ';
        }
        for (size_t j = 0; j < (size + 511) / 512 * 512; j++)
            buf[n + 512 + j] = j < size ? (uint8_t)rnd() : 0;
        n += 512 + (size + 511) / 512 * 512;
    }
    if (pick(3) == 0 && n + 1024 <= cap) {   /* end-of-archive blocks */
        memset(buf + n, 0, 1024);
        n += 1024;
    }
    for (unsigned d = pick(3) == 0 ? 1 + pick(3) : 0; d > 0 && n > 0; d--) {
        static const unsigned spots[] = { 0, 99, 100, 101, 107, 124, 125, 135, 148, 150, 154, 155, 156, 257, 261, 345 };
        size_t base = nheads && pick(4) ? heads[pick(nheads)] : pick((unsigned)(n / 512 + 1)) * 512;
        size_t at = base + (pick(2) ? spots[pick(sizeof(spots) / sizeof(spots[0]))] : pick(512));
        if (at < n)
            buf[at] = (uint8_t)"\0 07x/"[pick(6)] ^ (pick(4) == 0 ? (uint8_t)rnd() : 0);
        if (base + 512 <= n && pick(2) && (at < base + 148 || at >= base + 156)) {   /* a checksum that agrees */
            uint8_t *h = buf + base;
            memset(h + 148, ' ', 8);
            unsigned sum = 0;
            for (int i = 0; i < 512; i++)
                sum += h[i];
            snprintf((char *)h + 148, 8, "%06o", sum);
            h[155] = ' ';
        }
    }
    if (pick(5) == 0 && n > 0)
        n -= pick((unsigned)n);   /* cut */
    return n;
}

static void digest_tar(const uint8_t *buf, size_t n)
{
    char err[160];
    struct tar_reader r;
    struct tar_member mem;
    tar_open(&r, buf, n);
    for (unsigned i = 0; i < 8; i++) {
        memset(err, 0, sizeof(err));
        int rc = tar_next(&r, &mem, err, sizeof(err));
        h_u64((uint64_t)(int64_t)rc);
        if (rc < 0)
            h_str(err, sizeof(err));
        if (rc <= 0)
            break;
        h_str(mem.name, 101);
        h_u64(mem.mode);
        h_u64(mem.size);
        h_u64((uint64_t)(mem.data - buf));
    }
}

/* A string for the small parsers: from the documents' vocabulary, or bytes. */
static void gen_string(char *s, size_t cap)
{
    struct text *t = malloc(sizeof(*t));
    t->n = 0;
    switch (pick(8)) {
    case 0: put_version(t); break;
    case 1: put_value(t, "depends"); break;
    case 2: put(t, g_paths[pick(sizeof(g_paths) / sizeof(g_paths[0]))]); break;
    case 3: put(t, g_sonames[pick(sizeof(g_sonames) / sizeof(g_sonames[0]))]); break;
    case 4: put_hex(t, pick(3) ? 128 : pick(140)); break;
    case 5: put_time(t); break;
    case 6:   /* a path at and past PKG_PATH_MAX */
        for (unsigned i = 0, n = PKG_PATH_MAX - 2 + pick(4); i < n; i++)
            t->b[t->n++] = i % 8 == 7 ? '/' : 'p';
        break;
    default:
        for (unsigned i = pick(40); i > 0; i--)
            t->b[t->n++] = (char)(pick(3) ? "0123456789.-abz/: <>=+_\t"[pick(24)] : (char)rnd());
        break;
    }
    if (pick(6) == 0 && t->n > 0)
        t->b[pick((unsigned)t->n)] = (char)"\0 .-/x9"[pick(7)];
    size_t n = t->n < cap - 1 ? t->n : cap - 1;
    memcpy(s, t->b, n);
    s[n] = '\0';
    free(t);
}

static void digest_strings(void)
{
    char a[320], b[320];
    gen_string(a, sizeof(a));
    gen_string(b, sizeof(b));
    if (pick(4) == 0) {   /* b a near copy of a: a revision, a component, or the same */
        memcpy(b, a, sizeof(b));
        size_t l = strlen(b);
        if (l > 0 && l + 3 < sizeof(b) && pick(2))
            memcpy(b + l, pick(2) ? "-1" : ".0", 3);
        else if (l > 0)
            b[l - 1] = (char)('0' + pick(10));
    }
    struct version v;
    memset(&v, 0, sizeof(v));
    bool ok = version_parse(a, &v);
    h_u64(ok);
    if (ok) {
        h_u64((uint64_t)v.ncomp);
        h_bytes(v.comp, sizeof(v.comp));
        h_u64(v.rev);
    }
    h_u64((uint64_t)(int64_t)version_cmp(a, b));
    struct depend d;
    ok = depend_parse(a, &d);
    h_u64(ok);
    if (ok) {
        hash_depend(&d);
        h_u64(depend_satisfied(&d, b));
    }
    h_u64(name_valid(a));
    h_u64(path_allowed(a));
    h_u64(soname_valid(a));
    uint8_t out[64];
    memset(out, 0, sizeof(out));
    size_t want = pick(4) == 0 ? pick(3) : 64;
    ok = hex_decode(a, out, want);
    h_u64(ok);
    h_bytes(out, sizeof(out));
    int64_t t = 0;
    ok = time_parse(a, &t);
    h_u64(ok);
    h_u64((uint64_t)t);
}

static uint64_t pkg_digest(unsigned rounds)
{
    struct text *t = malloc(sizeof(*t));
    uint8_t *tar = malloc(8192);
    g_rng = 0x9e3779b97f4a7c15ull;
    g_digest = 0xcbf29ce484222325ull;
    for (unsigned i = 0; i < rounds; i++) {
        gen_doc(t);
        digest_doc(t);
        digest_tar(tar, gen_tar(tar, 8192));
        digest_strings();
    }
    free(tar);
    free(t);
    return g_digest;
}

/* PKG_BENCH=1: the decoders alone (pkgparse.h), best of 15, on a manifest of
 * 200 files, an index of 300 entries with its header, and an archive of 40
 * members; each in its own buffers, made beforehand. */
static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static void pkg_bench(void)
{
    static char big[1 << 20];
    size_t mlen = 0, ilen = 0;
    mlen += (size_t)snprintf(big, sizeof(big), "name: fortune\nversion: 1.2.3-4\nsummary: prints a line\n"
                             "depends: fortunes >= 1.0\ndepends: libz\nprovides: libfortune.so.1\nrequires: libc.so\n");
    for (int i = 0; i < 200; i++) {
        mlen += (size_t)snprintf(big + mlen, sizeof(big) - mlen, "file: usr/share/fortune/part%03d/data.txt 0644 %d ", i,
                                 1000 + i * 37);
        for (int j = 0; j < 128; j++)
            big[mlen++] = "0123456789abcdef"[(i * 7 + j * 13) & 15];
        big[mlen++] = '\n';
    }
    char *itext = big + mlen;
    ilen += (size_t)snprintf(itext, sizeof(big) - mlen, "serial: 42\npublished: 2026-10-01T00:00:00Z\n"
                             "expires: 2026-12-01T00:00:00Z\n\n");
    for (int i = 0; i < 300; i++) {
        ilen += (size_t)snprintf(itext + ilen, sizeof(big) - mlen - ilen,
                                 "name: port%d\nversion: %d.%d.%d\nsummary: the port number %d\ndepends: libc%d >= 1.0\n"
                                 "file: port%d-%d.0.cpk\nsize: %d\nsha512: ",
                                 i, i % 9, i % 17, i % 5, i, i % 3, i, i % 9, 4096 + i * 11);
        for (int j = 0; j < 128; j++)
            itext[ilen++] = "0123456789abcdef"[(i * 5 + j * 11) & 15];
        itext[ilen++] = '\n';
        itext[ilen++] = '\n';
    }
    size_t tcap = 40 * (512 + 1024);
    uint8_t *tar = malloc(tcap);
    size_t tlen = 0;
    for (int i = 0; i < 40; i++) {
        char name[64];
        snprintf(name, sizeof(name), "usr/share/fortune/part%03d/data.txt", i);
        size_t size = 100 + (size_t)i * 23;
        tar_header(tar + tlen, name, size, 0644);
        memset(tar + tlen + 512, 'x', 1024);
        tlen += 512 + (size + 511) / 512 * 512;
    }

    struct manifest *m = malloc(sizeof(*m));
    struct file_entry *files = calloc(PKG_MAX_FILES, sizeof(*files));
    struct dirlist *dirs = calloc(1, sizeof(*dirs));
    struct index *ix = malloc(sizeof(*ix));
    struct index_entry *entries = calloc(PKG_MAX_INDEX, sizeof(*entries));
    struct pkg_perr pe;
    const int reps = 200;
    double best[4] = { 1e30, 1e30, 1e30, 1e30 };
    unsigned sink = 0;
    for (int round = 0; round < 15; round++) {
        double t0 = now_ns();
        for (int k = 0; k < reps; k++) {
            memset(m, 0, sizeof(*m));
            m->files = files;
            m->dirs = dirs;
            sink += (unsigned)manifest_decode(big, mlen, m, m->provides[0], m->requires[0], dirs->paths[0], &pe) +
                    (unsigned)m->nfiles;
        }
        double t1 = now_ns();
        for (int k = 0; k < reps; k++) {
            memset(ix, 0, sizeof(*ix));
            memset(entries, 0, 300 * sizeof(*entries));
            ix->entries = entries;
            sink += (unsigned)index_decode(itext, ilen, ix, &pe) + (unsigned)ix->n;
        }
        double t2 = now_ns();
        for (int k = 0; k < reps; k++) {
            struct tar_reader r;
            struct tar_decoded d;
            tar_open(&r, tar, tlen);
            while (tar_decode(&r, &d) == TAR_DEC_MEMBER) {
                r.pos = d.next;
                sink += d.mode;
            }
        }
        double t3 = now_ns();
        for (int k = 0; k < reps; k++)
            for (int i = 0; i < 300; i++)
                sink += (unsigned)version_cmp(ix->entries[i].version, ix->entries[(i * 7) % 300].version);
        double t4 = now_ns();
        double t[4] = { t1 - t0, t2 - t1, t3 - t2, t4 - t3 };
        for (int i = 0; i < 4; i++)
            if (t[i] / reps < best[i])
                best[i] = t[i] / reps;
    }
    printf("bench manifest %.0f ns, index %.0f ns, tar %.0f ns, version_cmp %.0f ns (sink %u)\n", best[0], best[1],
           best[2], best[3], sink);
    free(entries);
    free(ix);
    free(dirs);
    free(files);
    free(m);
    free(tar);
}

int main(void)
{
    /* versions */
    struct version v;
    CHECK(version_parse("1.2.3-4", &v) && v.ncomp == 3 && v.comp[2] == 3 && v.rev == 4);
    CHECK(version_parse("0", &v) && v.ncomp == 1);
    CHECK(!version_parse("", &v) && !version_parse("1.", &v) && !version_parse("a.b", &v) && !version_parse("1-", &v));
    CHECK(!version_parse("1.2.3.4.5.6.7.8.9", &v));
    CHECK(version_cmp("1.0", "1.0.0") == 0 && version_cmp("1.0", "1.1") < 0 && version_cmp("1.10", "1.9") > 0);
    CHECK(version_cmp("1.0-2", "1.0-1") > 0 && version_cmp("2", "1.99.99") > 0);

    /* constraints */
    struct depend d;
    CHECK(depend_parse("fortunes", &d) && d.op == OP_NONE && strcmp(d.name, "fortunes") == 0);
    CHECK(depend_parse("fortunes >= 1.0", &d) && d.op == OP_GE && strcmp(d.version, "1.0") == 0);
    CHECK(depend_parse("x=2", &d) && d.op == OP_EQ);
    CHECK(depend_parse("x<=2", &d) && d.op == OP_LE && depend_parse("x < 2", &d) && d.op == OP_LT);
    CHECK(depend_parse("x>2.0", &d) && d.op == OP_GT);
    CHECK(!depend_parse("", &d) && !depend_parse("Bad", &d) && !depend_parse("x ~ 1", &d) && !depend_parse("x >= ", &d));
    CHECK(!depend_parse("x >= 1 trailing", &d));
    CHECK(depend_parse("x >= 1.0", &d) && depend_satisfied(&d, "1.0") && depend_satisfied(&d, "2") &&
          !depend_satisfied(&d, "0.9"));
    CHECK(depend_parse("x = 1.0", &d) && depend_satisfied(&d, "1.0.0") && !depend_satisfied(&d, "1.0.1"));
    CHECK(depend_parse("x < 1.0", &d) && depend_satisfied(&d, "0.9.9") && !depend_satisfied(&d, "1.0"));
    CHECK(name_valid("a") && name_valid("hello-world+1.x") && !name_valid("-x") && !name_valid("Ab") &&
          !name_valid("a b"));

    /* paths */
    CHECK(path_allowed("usr/bin/hello") && path_allowed("etc/x"));
    CHECK(!path_allowed("/usr/bin") && !path_allowed("usr/../etc") && !path_allowed("boot/x") &&
          !path_allowed("tmp/x") && !path_allowed("dev/null") && !path_allowed("mnt/a") && !path_allowed("a//b") &&
          !path_allowed("a/") && !path_allowed("") && !path_allowed("a/./b") && !path_allowed("a b"));

    /* manifest */
    const char *mtext =
        "name: fortune\nversion: 1.0\nsummary: prints a line\ndepends: fortunes >= 1.0\n"
        "file: usr/bin/fortune 0755 3 "
        "3bafbf08882a2d10133093a1b8433f50563b93c14acd05b79028eb1d12799027241450980651994501423a66c276ae26c43b739bc65c4e16b10c3af6c202aebb\n";
    struct manifest m;
    char err[PKG_ERR_MAX];
    CHECK(manifest_parse(mtext, strlen(mtext), &m, err, sizeof(err)) == 0);
    CHECK(strcmp(m.name, "fortune") == 0 && m.ndepends == 1 && m.nfiles == 1 && m.files[0].mode == 0755 &&
          m.files[0].size == 3 && m.files[0].sha[0] == 0x3b && m.files[0].sha[63] == 0xbb);
    manifest_free(&m);
    const char *bad1 = "name: fortune\nversion: 1.0\nfile: ../x 0755 1 00\n";
    CHECK(manifest_parse(bad1, strlen(bad1), &m, err, sizeof(err)) < 0 && strstr(err, "line 3"));
    const char *bad2 = "name: fortune\nfile: usr/x 0755 1 00\n";   /* no version */
    CHECK(manifest_parse(bad2, strlen(bad2), &m, err, sizeof(err)) < 0);
    const char *bad3 = "name: fortune\nversion: 1.0\nfile: usr/x 0755 1 zz\n";
    CHECK(manifest_parse(bad3, strlen(bad3), &m, err, sizeof(err)) < 0);
    const char *bad4 = "name: fortune\nversion: 1.0\nmystery: 1\n";
    CHECK(manifest_parse(bad4, strlen(bad4), &m, err, sizeof(err)) < 0);
    const char *bad5 = "name: fortune\nversion: 1.0\nfile: usr/x 0755 1\n";   /* missing checksum */
    CHECK(manifest_parse(bad5, strlen(bad5), &m, err, sizeof(err)) < 0);
    const char *bad6 = "name: fortune\nversion: 1.0\nnocolon\n";
    CHECK(manifest_parse(bad6, strlen(bad6), &m, err, sizeof(err)) < 0 && strstr(err, "line 3"));
    /* Installed records carry dir: lines; formatting round-trips the record. */
    char rec[1024];
    snprintf(rec, sizeof(rec), "%sdir: /usr/share/x\ndir: /usr/share\n", mtext);
    CHECK(manifest_parse(rec, strlen(rec), &m, err, sizeof(err)) == 0 && m.dirs && m.dirs->n == 2 &&
          strcmp(m.dirs->paths[1], "/usr/share") == 0);
    char out2[2048];
    int flen = manifest_format(&m, m.dirs, out2, sizeof(out2));
    CHECK(flen == (int)strlen(rec) && strcmp(out2, rec) == 0);
    CHECK(manifest_format(&m, m.dirs, out2, 64) < 0);
    manifest_free(&m);
    /* Shared libraries (roadmap 1.1 P4): provides and requires round-trip
     * (written after depends); a value that is not a soname is refused. */
    const char *libs = "name: busybox-dyn\nversion: 1.37.0\nsummary: s\nprovides: libx.so.1\nrequires: libc.so\n";
    CHECK(manifest_parse(libs, strlen(libs), &m, err, sizeof(err)) == 0 && m.nprovides == 1 && m.nrequires == 1 &&
          strcmp(m.provides[0], "libx.so.1") == 0 && strcmp(m.requires[0], "libc.so") == 0);
    CHECK(manifest_format(&m, NULL, out2, sizeof(out2)) == (int)strlen(libs) && strcmp(out2, libs) == 0);
    manifest_free(&m);
    const char *badlib[] = { "name: x\nversion: 1.0\nrequires: libc\n", "name: x\nversion: 1.0\nprovides: .so\n",
                             "name: x\nversion: 1.0\nrequires: lib c.so\n", "name: x\nversion: 1.0\nprovides: a/b.so\n" };
    for (size_t i = 0; i < sizeof(badlib) / sizeof(badlib[0]); i++)
        CHECK(manifest_parse(badlib[i], strlen(badlib[i]), &m, err, sizeof(err)) < 0);
    /* The most library lines a manifest may hold -- 32 of each, every name
     * 63 bytes -- fit the room pkg's record_stage gives them
     * (PKG_SONAME_MAX + 12 per line beyond its 4096; #416's review). */
    {
        static char big[8192], fmt[8192];
        size_t n = (size_t)snprintf(big, sizeof(big), "name: x\nversion: 1.0\nsummary: s\n");
        for (int i = 0; i < 2 * PKG_MAX_LIBS; i++)
            n += (size_t)snprintf(big + n, sizeof(big) - n, "%s: %0*d.so\n", i < PKG_MAX_LIBS ? "provides" : "requires",
                                  PKG_SONAME_MAX - 4, i);
        CHECK(manifest_parse(big, n, &m, err, sizeof(err)) == 0 && m.nprovides == PKG_MAX_LIBS &&
              m.nrequires == PKG_MAX_LIBS && strlen(m.provides[0]) == PKG_SONAME_MAX - 1);
        size_t room = 4096 + (size_t)(m.nprovides + m.nrequires) * (PKG_SONAME_MAX + 12);
        int flen2 = manifest_format(&m, NULL, fmt, room);
        CHECK(flen2 > 4096 && (size_t)flen2 < room && strcmp(fmt, big) == 0);
        manifest_free(&m);
    }
    const char *bad7 = "name: fortune\nversion: 1.0\ndir: relative\n";
    CHECK(manifest_parse(bad7, strlen(bad7), &m, err, sizeof(err)) < 0);

    /* index */
    const char *itext =
        "name: fortune\nversion: 1.0\nsummary: s\ndepends: fortunes\nfile: fortune-1.0.cpk\nsize: 10\nsha512: "
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000\n"
        "\n"
        "name: fortunes\nversion: 1.0\nsummary: t\nfile: fortunes-1.0.cpk\nsize: 20\nsha512: "
        "1111111111111111111111111111111111111111111111111111111111111111"
        "1111111111111111111111111111111111111111111111111111111111111111\n";
    struct index ix;
    CHECK(index_parse(itext, strlen(itext), &ix, err, sizeof(err)) == 0 && ix.n == 2);
    CHECK(strcmp(ix.entries[1].name, "fortunes") == 0 && ix.entries[1].size == 20 && ix.entries[1].sha[0] == 0x11);
    index_free(&ix);
    const char *ibad = "name: fortune\nversion: 1.0\nfile: ../x\nsize: 1\n";
    CHECK(index_parse(ibad, strlen(ibad), &ix, err, sizeof(err)) < 0);
    const char *ibad2 = "name: fortune\nversion: 1.0\nsize: 1\n";   /* no file */
    CHECK(index_parse(ibad2, strlen(ibad2), &ix, err, sizeof(err)) < 0);
    CHECK(index_parse(itext, strlen(itext), &ix, err, sizeof(err)) == 0 && !ix.header.present);
    index_free(&ix);

    /* time */
    int64_t t;
    char ts[21];
    CHECK(time_parse("1970-01-01T00:00:00Z", &t) && t == 0);
    CHECK(time_parse("2026-10-18T12:34:56Z", &t) && t == 1792326896);
    time_format(t, ts);
    CHECK(strcmp(ts, "2026-10-18T12:34:56Z") == 0);
    CHECK(time_parse("2024-02-29T23:59:59Z", &t) && t == 1709251199);
    time_format(t, ts);
    CHECK(strcmp(ts, "2024-02-29T23:59:59Z") == 0);
    CHECK(!time_parse("2026-02-29T00:00:00Z", &t) && !time_parse("2026-10-18 12:34:56Z", &t) &&
          !time_parse("2026-10-18T12:34:56", &t) && !time_parse("2026-13-01T00:00:00Z", &t) &&
          !time_parse("2026-10-18T24:00:00Z", &t) && !time_parse("1969-12-31T23:59:59Z", &t) &&
          !time_parse("2026-10-18T12:34:56+00:00", &t));

    /* the index header and PK10 */
    const char *hdr = "serial: 7\npublished: 2026-10-01T00:00:00Z\nexpires: 2026-10-31T00:00:00Z\n\n";
    char htext[1024];
    snprintf(htext, sizeof(htext), "%s%s", hdr, itext);
    CHECK(index_parse(htext, strlen(htext), &ix, err, sizeof(err)) == 0 && ix.n == 2 && ix.header.present &&
          ix.header.serial == 7 && ix.header.expires - ix.header.published == 30 * 86400);
    CHECK(strcmp(ix.entries[0].name, "fortune") == 0);
    struct index_header h = ix.header;
    index_free(&ix);
    struct index_header h2;
    CHECK(index_header_parse(htext, strlen(htext), &h2, err, sizeof(err)) == 0 && h2.serial == 7);
    /* only the header is read: a broken package line after it does not matter */
    char hbroken[256];
    snprintf(hbroken, sizeof(hbroken), "%sname fortune without a colon\n", hdr);
    CHECK(index_header_parse(hbroken, strlen(hbroken), &h2, err, sizeof(err)) == 0 && h2.serial == 7);
    CHECK(index_parse(hbroken, strlen(hbroken), &ix, err, sizeof(err)) < 0 && strstr(err, "line 5"));
    const char *hbad[] = {
        "serial: 0\npublished: 2026-10-01T00:00:00Z\nexpires: 2026-10-31T00:00:00Z\n\n",
        "serial: 07\npublished: 2026-10-01T00:00:00Z\nexpires: 2026-10-31T00:00:00Z\n\n",
        "serial: 7\nexpires: 2026-10-31T00:00:00Z\n\n",
        "serial: 7\npublished: 2026-10-01T00:00:00Z\nexpires: soon\n\n",
        "serial: 7\nserial: 8\npublished: 2026-10-01T00:00:00Z\nexpires: 2026-10-31T00:00:00Z\n\n",
        "serial: 99999999999999999999999\npublished: 2026-10-01T00:00:00Z\nexpires: 2026-10-31T00:00:00Z\n\n",
        "serial: 7\npublished: 2026-10-01T00:00:00Z\nexpires: 2026-10-31T00:00:00Z\nname: x\n\n",
    };
    for (size_t i = 0; i < sizeof(hbad) / sizeof(hbad[0]); i++)
        CHECK(index_parse(hbad[i], strlen(hbad[i]), &ix, err, sizeof(err)) < 0);
    char hlate[1024];   /* a header key in a package stanza */
    snprintf(hlate, sizeof(hlate), "%s\nserial: 9\n", itext);
    CHECK(index_parse(hlate, strlen(hlate), &ix, err, sizeof(err)) < 0);

    int64_t pub = h.published, day = 86400;
    CHECK(index_fresh(&h, 0, pub + day, err, sizeof(err)) == 0);
    CHECK(index_fresh(&h, 7, pub + day, err, sizeof(err)) == 0);   /* the same index again */
    CHECK(index_fresh(&h, 6, pub + day, err, sizeof(err)) == 0);
    CHECK(index_fresh(&h, 8, pub + day, err, sizeof(err)) < 0 && strstr(err, "older than 8"));
    CHECK(index_fresh(&h, 0, pub + 30 * day, err, sizeof(err)) == -2 && strstr(err, "expired at 2026-10-31"));
    CHECK(index_fresh(&h, 0, pub - 1, err, sizeof(err)) < 0 && strstr(err, "before the index was published"));
    struct index_header hl = h;
    hl.expires = h.published + 90 * day;   /* the longest validity (INDEX.90) */
    CHECK(index_fresh(&hl, 0, pub + 89 * day, err, sizeof(err)) == 0);
    hl.expires = h.published + 91 * day;
    CHECK(index_fresh(&hl, 0, pub + day, err, sizeof(err)) == -1 && strstr(err, "at most 90 days"));
    hl.expires = h.published;
    CHECK(index_fresh(&hl, 0, pub, err, sizeof(err)) < 0);
    struct index_header none = { .present = false };
    CHECK(index_fresh(&none, 0, pub, err, sizeof(err)) < 0 && strstr(err, "no serial"));

    /* repos.conf */
    struct repo rp;
    CHECK(repo_parse("/var/lib/pkg/repo", 0, &rp, err, sizeof(err)) == 0 && rp.local &&
          strcmp(rp.name, "local") == 0 && strcmp(rp.dir, "/var/lib/pkg/repo") == 0 &&
          strcmp(rp.url, "file:///var/lib/pkg/repo") == 0);
    CHECK(repo_parse("/boot/repo/", 1, &rp, err, sizeof(err)) == 0 && strcmp(rp.name, "local2") == 0 &&
          strcmp(rp.dir, "/boot/repo") == 0);
    CHECK(repo_parse("cosmoos https://example.org/repo/x86_64/", 0, &rp, err, sizeof(err)) == 0 && !rp.local &&
          strcmp(rp.url, "https://example.org/repo/x86_64") == 0 && !rp.pin[0] && !rp.insecure);
    CHECK(repo_parse("own\thttps://h/r  pin sha256:"
                     "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff",
                     0, &rp, err, sizeof(err)) == 0 && strncmp(rp.pin, "sha256:0011", 11) == 0);
    CHECK(repo_parse("test http://10.0.2.2:8000/repo insecure", 0, &rp, err, sizeof(err)) == 0 && rp.insecure);
    CHECK(repo_parse("disk file:///mnt/repo", 0, &rp, err, sizeof(err)) == 0 && rp.local &&
          strcmp(rp.dir, "/mnt/repo") == 0);
    const char *rbad[] = {
        "test http://10.0.2.2/repo", "Bad https://h/r", "x ftp://h/r", "x https://h/r pin sha256:00",
        "x https://h/r pin md5:00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff",
        "x https://h/r verify", "x file:///r insecure", "x", "relative/dir",
        "x http://h/r insecure pin sha256:00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff",
        ("x https://h/r pin sha256:00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff "
         "pin sha256:ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100"),
        "x https://h/r insecure insecure insecure insecure insecure insecure insecure",
    };
    for (size_t i = 0; i < sizeof(rbad) / sizeof(rbad[0]); i++)
        CHECK(repo_parse(rbad[i], 0, &rp, err, sizeof(err)) < 0);

    /* tar */
    unsigned char *tar = calloc(1, 512 * 5);
    tar_header(tar, "+MANIFEST", 5, 0644);
    memcpy(tar + 512, "abcde", 5);
    tar_header(tar + 1024, "usr/bin/x", 600, 0755);
    memset(tar + 1536, 'x', 600);
    struct tar_reader r;
    struct tar_member mem;
    tar_open(&r, tar, 512 * 5);
    CHECK(tar_next(&r, &mem, err, sizeof(err)) == 1 && strcmp(mem.name, "+MANIFEST") == 0 && mem.size == 5 &&
          memcmp(mem.data, "abcde", 5) == 0 && mem.mode == 0644);
    CHECK(tar_next(&r, &mem, err, sizeof(err)) == 1 && strcmp(mem.name, "usr/bin/x") == 0 && mem.size == 600 &&
          mem.mode == 0755 && mem.data[599] == 'x');
    CHECK(tar_next(&r, &mem, err, sizeof(err)) == 0);
    /* corruption: checksum, truncation, size past the end */
    tar[10] ^= 1;
    tar_open(&r, tar, 512 * 5);
    CHECK(tar_next(&r, &mem, err, sizeof(err)) < 0 && strstr(err, "checksum"));
    tar[10] ^= 1;
    tar_open(&r, tar, 1024 + 512);   /* the second member's data is cut */
    CHECK(tar_next(&r, &mem, err, sizeof(err)) == 1 && tar_next(&r, &mem, err, sizeof(err)) < 0);
    tar_open(&r, tar, 100);
    CHECK(tar_next(&r, &mem, err, sizeof(err)) < 0 && strstr(err, "truncated"));
    free(tar);

    /* hex */
    uint8_t out[2];
    char hex[5];
    CHECK(hex_decode("0aFf", out, 2) && out[0] == 0x0a && out[1] == 0xff);
    CHECK(!hex_decode("0g", out, 1) && !hex_decode("000", out, 1));
    CHECK(hex_decode("0aff", out, 2));
    hex_encode(out, 2, hex);
    CHECK(strcmp(hex, "0aff") == 0);

    /* the decoders' digest */
    uint64_t dg = pkg_digest(ROUNDS);
    printf("  pkg decoders digest 0x%016llx over %u rounds\n", (unsigned long long)dg, ROUNDS);
    CHECK(dg == PKG_DIGEST);
    if (getenv("PKG_BENCH") != NULL)
        pkg_bench();

    if (g_failures) {
        printf("pkg                           FAIL (%d)\n", g_failures);
        return 1;
    }
    printf("pkg                           ok\n");
    return 0;
}
