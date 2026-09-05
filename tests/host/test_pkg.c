/*
 * test_pkg.c - Host test of the package manager's parsers
 * (docs/pkg/testing.md): versions and constraints, manifests, the index,
 * path confinement, the ustar reader. Built with ASan and UBSan.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pkg.h"

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

    if (g_failures) {
        printf("pkg                           FAIL (%d)\n", g_failures);
        return 1;
    }
    printf("pkg                           ok\n");
    return 0;
}
