/*
 * fuzz_pkg.c - The package system's parsers (pkg/manifest.c, pkg/tar.c,
 * pkg/version.c) under fuzzing. The input is fed to every parser; a tar
 * member the reader returns must lie inside the buffer.
 */

#include "fuzz.h"

#include "pkg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char err[128];

    /* Manifest and index: text parsers over the whole input. */
    struct manifest m;
    memset(&m, 0, sizeof(m));
    if (manifest_parse((const char *)data, size, &m, err, sizeof(err)) == 0)
        manifest_free(&m);
    struct index ix;
    memset(&ix, 0, sizeof(ix));
    if (index_parse((const char *)data, size, &ix, err, sizeof(err)) == 0)
        index_free(&ix);

    /* Version, dependency, path and hex parsers over a NUL-terminated copy. */
    char *s = malloc(size + 1);
    memcpy(s, data, size);
    s[size] = '\0';
    struct version v;
    version_parse(s, &v);
    struct depend d;
    depend_parse(s, &d);
    path_allowed(s);
    name_valid(s);
    uint8_t hex[32];
    hex_decode(s, hex, sizeof(hex));
    if (size >= 8)
        version_cmp(s, s + size / 2 < s + size ? s + size / 2 : s);
    free(s);

    /* Tar: walk every member the reader yields. */
    struct tar_reader r;
    struct tar_member mem;
    tar_open(&r, data, size);
    for (unsigned n = 0; n < 4096; n++) {
        int rc = tar_next(&r, &mem, err, sizeof(err));
        if (rc <= 0)
            break;
        FUZZ_ASSERT(mem.data >= data && mem.data <= data + size);
        FUZZ_ASSERT(mem.size <= (size_t)((data + size) - mem.data));
    }
    return 0;
}

static void tar_header(unsigned char *h, const char *name, size_t size, unsigned mode)
{
    memset(h, 0, 512);
    strncpy((char *)h, name, 99);
    snprintf((char *)h + 100, 8, "%07o", mode);
    snprintf((char *)h + 108, 8, "%07o", 0);
    snprintf((char *)h + 116, 8, "%07o", 0);
    snprintf((char *)h + 124, 12, "%011lo", (unsigned long)size);
    snprintf((char *)h + 136, 12, "%011lo", 0ul);
    memset(h + 148, ' ', 8);
    h[156] = '0';
    memcpy(h + 257, "ustar", 5);
    memcpy(h + 263, "00", 2);
    unsigned sum = 0;
    for (int i = 0; i < 512; i++)
        sum += h[i];
    snprintf((char *)h + 148, 8, "%06o", sum);
}

size_t fuzz_seed(unsigned i, uint8_t *buf, size_t cap)
{
    static const char manifest[] = "name: hello\nversion: 1.1\ndescription: prints a greeting\n"
                                   "depends: libc >= 1.0\nfile: usr/bin/hello 0755 "
                                   "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\n";
    static const char index[] = "name: hello\nversion: 1.1\ndescription: prints a greeting\n"
                                "sha256: e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\n\n";
    switch (i) {
    case 0:
        if (sizeof(manifest) > cap)
            return 0;
        memcpy(buf, manifest, sizeof(manifest) - 1);
        return sizeof(manifest) - 1;
    case 1:
        if (sizeof(index) > cap)
            return 0;
        memcpy(buf, index, sizeof(index) - 1);
        return sizeof(index) - 1;
    case 2: {
        if (cap < 512 * 5)
            return 0;
        memset(buf, 0, 512 * 5);
        tar_header(buf, "+MANIFEST", 5, 0644);
        memcpy(buf + 512, "name:", 5);
        tar_header(buf + 1024, "usr/bin/x", 600, 0755);
        return 512 * 5;
    }
    case 3:
        memcpy(buf, "1.2.3-beta", 10);
        return 10;
    default:
        return 0;
    }
}
