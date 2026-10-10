/*
 * test_gpt.c - The GPT parser and builder (kernel/block/gpt.c) and the
 * command-line parser (kernel/core/cmdline.c), on the host.
 */

#include "harness.h"

#include <kernel/cmdline.h>
#include <kernel/gpt.h>

#include <stdlib.h>
#include <string.h>

struct disk {
    uint32_t ss;
    uint64_t n;
    uint8_t *data;
    unsigned reads;
    int fail_at;    /* fail the read that would be number fail_at (from 1); 0 never */
};

static int disk_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    struct disk *d = ctx;
    d->reads++;
    if (d->fail_at && (int)d->reads == d->fail_at)
        return -1;
    if (lba >= d->n || d->n - lba < count)
        return -1;
    memcpy(buf, d->data + lba * d->ss, (size_t)count * d->ss);
    return 0;
}

static struct disk *disk_new(uint32_t ss, uint64_t n)
{
    struct disk *d = calloc(1, sizeof(*d));
    d->ss = ss;
    d->n = n;
    d->data = calloc(n, ss);
    return d;
}

static void disk_free(struct disk *d)
{
    free(d->data);
    free(d);
}

static const uint8_t disk_guid[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };

static void two_parts(struct gpt_build_part p[2], uint64_t first_usable, uint64_t last_usable)
{
    memset(p, 0, 2 * sizeof(*p));
    p[0].index = 1;
    p[0].first_lba = first_usable;
    p[0].last_lba = first_usable + 99;
    memcpy(p[0].type, gpt_type_esp, 16);
    memset(p[0].uuid, 0xAA, 16);
    p[1].index = 5;
    p[1].first_lba = first_usable + 100;
    p[1].last_lba = last_usable;
    memcpy(p[1].type, gpt_type_cosmo_root, 16);
    memset(p[1].uuid, 0xBB, 16);
}

static void write_table(struct disk *d, const struct gpt_build_part *p, unsigned n)
{
    uint8_t *head = calloc(GPT_BUILD_HEAD_SECTORS(d->ss), d->ss);
    uint8_t *tail = calloc(GPT_BUILD_TAIL_SECTORS(d->ss), d->ss);
    EXPECT(gpt_build(d->ss, d->n, disk_guid, p, n, head, tail));
    memcpy(d->data, head, (size_t)GPT_BUILD_HEAD_SECTORS(d->ss) * d->ss);
    memcpy(d->data + (d->n - GPT_BUILD_TAIL_SECTORS(d->ss)) * d->ss, tail,
           (size_t)GPT_BUILD_TAIL_SECTORS(d->ss) * d->ss);
    free(head);
    free(tail);
}

static enum gpt_result parse(struct disk *d, struct gpt_table *t, const char **why)
{
    static uint8_t scratch[GPT_SCRATCH_BYTES];
    d->reads = 0;
    return gpt_parse(d->ss, d->n, disk_read, d, scratch, t, why);
}

static void test_crc_and_guid(void)
{
    EXPECT(gpt_crc32("123456789", 9) == 0xCBF43926u);
    EXPECT(gpt_crc32_update(gpt_crc32("1234", 4), "56789", 5) == 0xCBF43926u);
    char text[GPT_GUID_TEXT];
    gpt_guid_format(gpt_type_esp, text);
    EXPECT(strcmp(text, "c12a7328-f81f-11d2-ba4b-00a0c93ec93b") == 0);
    gpt_guid_format(gpt_type_cosmo_root, text);
    EXPECT(strcmp(text, "c9b09224-e00d-418e-846a-9f1d9a61bfd5") == 0);
    uint8_t g[16];
    EXPECT(gpt_guid_parse("C12A7328-F81F-11D2-BA4B-00A0C93EC93B", g) && memcmp(g, gpt_type_esp, 16) == 0);
    EXPECT(!gpt_guid_parse("c12a7328xf81f-11d2-ba4b-00a0c93ec93b", g));
    EXPECT(!gpt_guid_parse("c12a7328-f81f-11d2-ba4b-00a0c93ec93g", g));
    EXPECT(!gpt_guid_parse("c12a7328-f81f-11d2-ba4b-00a0c93ec93", g));   /* 35 + NUL: the NUL is not a digit */
}

static void test_roundtrip(uint32_t ss, uint64_t n)
{
    struct disk *d = disk_new(ss, n);
    struct gpt_table t;
    const char *why = NULL;
    EXPECT(parse(d, &t, &why) == GPT_NONE);   /* blank */
    struct gpt_build_part p[2];
    two_parts(p, gpt_build_first_usable(ss), gpt_build_last_usable(ss, n));
    write_table(d, p, 2);
    EXPECT(parse(d, &t, &why) == GPT_OK);
    EXPECT(t.nparts == 2 && t.parts[0].index == 1 && t.parts[1].index == 5);
    EXPECT(t.parts[0].first_lba == p[0].first_lba && t.parts[1].last_lba == p[1].last_lba);
    EXPECT(memcmp(t.parts[1].uuid, p[1].uuid, 16) == 0 && memcmp(t.parts[0].type, gpt_type_esp, 16) == 0);
    EXPECT(memcmp(t.disk_guid, disk_guid, 16) == 0);
    EXPECT(t.first_usable == gpt_build_first_usable(ss) && t.last_usable == gpt_build_last_usable(ss, n));

    /* A failing read is GPT_IO, whichever read it is. */
    for (int k = 1; k <= 5; k++) {   /* MBR, both headers, both arrays */
        d->fail_at = k;
        EXPECT(parse(d, &t, &why) == GPT_IO);
    }
    d->fail_at = 0;

    /* Each copy damaged in turn: refused, and the reason names it. */
    uint32_t arr = GPT_BUILD_ARRAY_SECTORS(ss);
    struct { uint64_t lba; unsigned off; const char *why; } dmg[] = {
        { 1, 0, "header signature" },
        { 1, 40, "header CRC" },
        { 2, 3, "entry array CRC" },
        { n - 1, 40, "backup header" },
        { n - 1 - arr, 3, "backup entry array CRC" },
    };
    for (unsigned i = 0; i < sizeof(dmg) / sizeof(dmg[0]); i++) {
        write_table(d, p, 2);
        d->data[dmg[i].lba * ss + dmg[i].off] ^= 0x40;
        EXPECT(parse(d, &t, &why) == GPT_BAD);
        EXPECT(strcmp(why, dmg[i].why) == 0);
    }

    /* Valid checksums over bad contents. */
    struct gpt_build_part q[2];
    memcpy(q, p, sizeof(q));
    q[0].last_lba = q[1].first_lba;   /* overlap by one sector */
    write_table(d, q, 2);
    EXPECT(parse(d, &t, &why) == GPT_BAD && strcmp(why, "partitions overlap") == 0);
    memcpy(q, p, sizeof(q));
    q[1].last_lba = gpt_build_last_usable(ss, n) + 1;
    write_table(d, q, 2);
    EXPECT(parse(d, &t, &why) == GPT_BAD && strcmp(why, "partition outside the usable range") == 0);
    memcpy(q, p, sizeof(q));
    q[0].first_lba = q[0].last_lba + 1;
    write_table(d, q, 2);
    EXPECT(parse(d, &t, &why) == GPT_BAD && strcmp(why, "partition ends before it starts") == 0);
    memcpy(q, p, sizeof(q));
    memcpy(q[1].uuid, q[0].uuid, 16);
    write_table(d, q, 2);
    EXPECT(parse(d, &t, &why) == GPT_BAD && strcmp(why, "two partitions share a unique GUID") == 0);
    memcpy(q, p, sizeof(q));
    memset(q[0].uuid, 0, 16);
    write_table(d, q, 2);
    EXPECT(parse(d, &t, &why) == GPT_BAD && strcmp(why, "partition without a unique GUID") == 0);

    /* A disk that grew (the backup is no longer on the last sector). */
    write_table(d, p, 2);
    struct disk grown = *d;
    grown.n = n - 1;   /* as if the table were written for a disk one sector longer */
    EXPECT(gpt_parse(ss, grown.n, disk_read, &grown, (uint8_t[GPT_SCRATCH_BYTES]){ 0 }, &t, &why) == GPT_BAD);

    /* No protective MBR: not a GPT disk, even with a good header. */
    write_table(d, p, 2);
    d->data[446 + 4] = 0x83;
    EXPECT(parse(d, &t, &why) == GPT_NONE);
    disk_free(d);
}

static void test_limits(void)
{
    /* GPT_MAX_PARTS used entries are fine; one more is refused. */
    struct disk *d = disk_new(512, 4096);
    struct gpt_build_part p[GPT_MAX_PARTS + 1];
    memset(p, 0, sizeof(p));
    uint64_t f = gpt_build_first_usable(512);
    for (unsigned i = 0; i <= GPT_MAX_PARTS; i++) {
        p[i].index = i + 1;
        p[i].first_lba = f + 10 * i;
        p[i].last_lba = f + 10 * i + 9;
        memcpy(p[i].type, gpt_type_esp, 16);
        memset(p[i].uuid, (int)(i + 1), 16);
    }
    struct gpt_table t;
    const char *why = NULL;
    write_table(d, p, GPT_MAX_PARTS);
    EXPECT(parse(d, &t, &why) == GPT_OK && t.nparts == GPT_MAX_PARTS);
    write_table(d, p, GPT_MAX_PARTS + 1);
    EXPECT(parse(d, &t, &why) == GPT_BAD && strcmp(why, "too many partitions") == 0);
    /* The builder refuses what the layout cannot hold. */
    uint8_t *head = calloc(GPT_BUILD_HEAD_SECTORS(512), 512);
    uint8_t *tail = calloc(GPT_BUILD_TAIL_SECTORS(512), 512);
    EXPECT(!gpt_build(1024, 4096, disk_guid, p, 1, head, tail));
    EXPECT(!gpt_build(512, 60, disk_guid, p, 1, head, tail));
    p[0].index = 0;
    EXPECT(!gpt_build(512, 4096, disk_guid, p, 1, head, tail));
    free(head);
    free(tail);
    /* Sizes the parser does not take. */
    EXPECT(gpt_parse(1024, 4096, disk_read, d, (uint8_t[GPT_SCRATCH_BYTES]){ 0 }, &t, &why) == GPT_BAD);
    EXPECT(gpt_parse(512, 5, disk_read, d, (uint8_t[GPT_SCRATCH_BYTES]){ 0 }, &t, &why) == GPT_NONE);
    disk_free(d);
}

static void test_cmdline(void)
{
    char v[64];
    EXPECT(cmdline_find("", "root", v, sizeof(v)) == -1);
    EXPECT(cmdline_find("root=vda2", "root", v, sizeof(v)) == 4 && strcmp(v, "vda2") == 0);
    EXPECT(cmdline_find("  quiet\troot=PARTUUID=x\n", "root", v, sizeof(v)) == 10 && strcmp(v, "PARTUUID=x") == 0);
    EXPECT(cmdline_find("quiet root=a", "quiet", v, sizeof(v)) == 0 && strcmp(v, "") == 0);
    EXPECT(cmdline_find("rootfstype=x", "root", v, sizeof(v)) == -1);      /* a longer key is another key */
    EXPECT(cmdline_find("roo=x", "root", v, sizeof(v)) == -1);
    EXPECT(cmdline_find("root=a root=b", "root", v, sizeof(v)) == 1 && strcmp(v, "a") == 0);   /* first wins */
    EXPECT(cmdline_find("#cosmo-cmdline v1\nroot=b\n", "root", v, sizeof(v)) == 1 && strcmp(v, "b") == 0);
    EXPECT(cmdline_find("# root=a\nroot=b", "root", v, sizeof(v)) == 1 && strcmp(v, "b") == 0);
    EXPECT(cmdline_find("root=a#comment", "root", v, sizeof(v)) == 1 && strcmp(v, "a") == 0);
    EXPECT(cmdline_find("x=1 # root=a", "root", v, sizeof(v)) == -1);
    EXPECT(cmdline_find("root=abcdef", "root", v, 4) == -2);               /* never truncated */
    EXPECT(cmdline_find("root=abc", "root", v, 4) == 3 && strcmp(v, "abc") == 0);
}

static void test_roundtrip_512(void) { test_roundtrip(512, 2048); }
static void test_roundtrip_4096(void) { test_roundtrip(4096, 1024); }

static const struct host_test tests[] = {
    { "crc32 and guid text", test_crc_and_guid },
    { "build, parse and refuse, 512-byte sectors", test_roundtrip_512 },
    { "build, parse and refuse, 4096-byte sectors", test_roundtrip_4096 },
    { "partition and size limits", test_limits },
    { "command line tokens", test_cmdline },
};

int main(void)
{
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
