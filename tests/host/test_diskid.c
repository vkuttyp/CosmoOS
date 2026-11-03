/*
 * test_diskid.c - What a disk's device reports about itself, decoded
 * (drivers/storage/diskid.c, kernel/include/kernel/diskid.h) on the host:
 * roadmap 2.0 SA6's repayment of the test-disk change's driver C. A digest
 * over seeded answers, recorded from the C: ATA IDENTIFY DEVICE (strings with
 * spaces before, inside and after, LBA28 and LBA48 capacities, every sector
 * size word, NCQ and write-cache bits), NVMe Identify Controller (strings,
 * every MDTS, namespace counts) and Namespace (every FLBAS, metadata, block
 * size), SCSI INQUIRY (strings, peripheral types) and READ CAPACITY (10)
 * (block sizes and the "too big" marker). Answers are mostly random, so the
 * decoders see what a hostile device could send as well as what a disk does.
 *
 * DISKID_BENCH=1 times each decoder over one answer.
 */

#include "harness.h"

#include <kernel/diskid.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The digest of the C (diskid.c, 2026-11-03), which the CeeSafe port gives. */
#define DISKID_DIGEST 0x4949c54aa80780f2ull
#define ROUNDS 20000u
#define SEED 0xC05F5FA2026B0010ull

static uint64_t g_rng, g_digest;

static uint64_t next(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static void mix(const void *bytes, size_t n)
{
    const uint8_t *b = bytes;
    for (size_t i = 0; i < n; i++) {
        g_digest ^= b[i];
        g_digest *= 0x100000001b3ull;
    }
}

static void mix32(uint32_t v) { mix(&v, sizeof(v)); }
static void mix64(uint64_t v) { mix(&v, sizeof(v)); }

static void fill(uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        b[i] = (uint8_t)next();
}

/* A string field: printable characters with runs of spaces, or noise. */
static void text_field(uint8_t *b, size_t n)
{
    uint64_t kind = next() % 4;
    for (size_t i = 0; i < n; i++) {
        uint64_t r = next();
        b[i] = kind == 3 ? (uint8_t)r : r % 4 == 0 ? ' ' : (uint8_t)(0x21 + r % 94);
    }
    if (kind == 1)
        memset(b, ' ', n);                               /* all spaces */
    else if (kind == 2 && n > 2)
        memset(b + n - 1 - next() % (n / 2), ' ', 1);    /* a space near the end */
}

static void test_ata(void)
{
    uint16_t id[256];
    fill((uint8_t *)id, sizeof(id));
    uint8_t s[40];
    text_field(s, 40);
    for (unsigned i = 0; i < 20; i++)
        id[27 + i] = (uint16_t)(s[2 * i] << 8 | s[2 * i + 1]);
    text_field(s, 20);
    for (unsigned i = 0; i < 10; i++)
        id[10 + i] = (uint16_t)(s[2 * i] << 8 | s[2 * i + 1]);
    if (next() % 2) {
        id[106] = (uint16_t)(0x4000u | (next() % 2 ? 0x1000u : 0));   /* a valid sector-info word */
        uint64_t w = next() % 6;   /* random, a power of two, small, or inside the range but not a power of two */
        uint32_t words = (uint32_t)(w == 0 ? next() : w < 3 ? 256u << (next() % 5) : w == 3 ? next() % 256 : 256 + next() % 1792);
        id[117] = (uint16_t)words;
        id[118] = (uint16_t)(words >> 16);
    }
    if (next() % 4 == 0) {
        id[100] = id[101] = id[102] = id[103] = 0;   /* no LBA48 capacity */
        id[60] = id[61] = 0;
    }
    char model[41], serial[21];
    memset(model, 0x5a, sizeof(model));
    memset(serial, 0x5a, sizeof(serial));
    struct ata_identity a;
    memset(&a, 0, sizeof(a));
    ata_identity_decode(id, model, serial, &a);
    mix(model, strlen(model) + 1);
    mix(serial, strlen(serial) + 1);
    mix64(a.sectors);
    mix32(a.sector_size);
    mix32(a.qdepth);
    mix32((uint32_t)a.lba48 | (uint32_t)a.wcache << 1 | (uint32_t)a.ncq << 2 | (uint32_t)a.usable << 3);
}

static void test_nvme(void)
{
    static uint8_t id[4096];
    fill(id, sizeof(id));
    text_field(id + 4, 20);
    text_field(id + 24, 40);
    id[77] = (uint8_t)(next() % 3 ? next() % 10 : next());
    char serial[21], model[41];
    memset(serial, 0x5a, sizeof(serial));
    memset(model, 0x5a, sizeof(model));
    struct nvme_ctrl_identity c;
    memset(&c, 0, sizeof(c));
    uint32_t limit = next() % 2 ? 128 : (uint32_t)(1 + next() % 300);
    nvme_ctrl_identity_decode(id, limit, serial, model, &c);
    mix(serial, strlen(serial) + 1);
    mix(model, strlen(model) + 1);
    mix32(c.max_pages);
    mix32(c.nn);

    fill(id, 256);
    if (next() % 2) {
        unsigned f = (unsigned)(next() % 16);
        id[26] = (uint8_t)((next() & 0xf0) | f);
        id[128 + 4 * f] = 0;
        id[128 + 4 * f + 1] = next() % 4 ? 0 : (uint8_t)next();      /* metadata, now and then */
        id[128 + 4 * f + 2] = (uint8_t)(8 + next() % 6);             /* 2^8 to 2^13 */
        if (next() % 8 == 0)
            memset(id, 0, 8);                                        /* no blocks */
    }
    struct nvme_ns_identity n;
    memset(&n, 0, sizeof(n));
    nvme_ns_identity_decode(id, &n);
    mix64(n.nsze);
    mix32(n.ms);
    mix32(n.lbads);
    mix32(n.usable);
}

static void test_scsi(void)
{
    uint8_t inq[36];
    fill(inq, sizeof(inq));
    text_field(inq + 8, 8);
    text_field(inq + 16, 16);
    text_field(inq + 32, 4);
    if (next() % 2) {
        uint8_t qualifier = (uint8_t)(next() & 0xe0);   /* drawn one after the other: operand order is the compiler's */
        inq[0] = (uint8_t)(qualifier | (next() % 2 ? 0x00 : 0x0e));
    }
    char vendor[9], product[17], rev[5];
    memset(vendor, 0x5a, sizeof(vendor));
    memset(product, 0x5a, sizeof(product));
    memset(rev, 0x5a, sizeof(rev));
    uint32_t type = scsi_inquiry_decode(inq, vendor, product, rev);
    mix32(type);
    mix(vendor, strlen(vendor) + 1);
    mix(product, strlen(product) + 1);
    mix(rev, strlen(rev) + 1);

    uint8_t cap[8];
    fill(cap, sizeof(cap));
    uint64_t k = next() % 4;
    if (k == 0) {
        uint32_t bs = next() % 3 ? 256u << (next() % 6) : (uint32_t)(512 + next() % 3584);   /* or not a power of two */
        cap[4] = (uint8_t)(bs >> 24); cap[5] = (uint8_t)(bs >> 16); cap[6] = (uint8_t)(bs >> 8); cap[7] = (uint8_t)bs;
    } else if (k == 1) {
        memset(cap, 0xff, 4);   /* the "too big for READ CAPACITY (10)" marker */
        cap[4] = 0; cap[5] = 0; cap[6] = 2; cap[7] = 0;
    }
    uint32_t last = 0x12345678u, bs = 0x9abcdef0u;
    bool ok = scsi_capacity10_decode(cap, &last, &bs);
    mix32(ok);
    mix32(last);
    mix32(bs);
}

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    for (unsigned r = 0; r < ROUNDS; r++) {
        test_ata();
        test_nvme();
        test_scsi();
    }
    printf("  diskid digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, ROUNDS);
    EXPECT(g_digest == DISKID_DIGEST);
}

/* DISKID_BENCH=1: the median of nine runs, 200,000 calls each. */
static double ns_since(const struct timespec *t0)
{
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (double)(t1.tv_sec - t0->tv_sec) * 1e9 + (double)(t1.tv_nsec - t0->tv_nsec);
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static volatile uint64_t g_sink;

static void diskid_bench(void)
{
    g_rng = SEED;
    uint16_t ata[256];
    static uint8_t nvme[4096];
    uint8_t inq[36], cap[8];
    fill((uint8_t *)ata, sizeof(ata));
    fill(nvme, sizeof(nvme));
    fill(inq, sizeof(inq));
    fill(cap, sizeof(cap));
    double t[4][9];
    for (unsigned k = 0; k < 9; k++) {
        struct timespec t0;
        char model[41], serial[21], v[9], p[17], rv[5];
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 200000; i++) {
            struct ata_identity a;
            ata_identity_decode(ata, model, serial, &a);
            g_sink += a.sectors;
        }
        t[0][k] = ns_since(&t0) / 200000;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 200000; i++) {
            struct nvme_ctrl_identity c;
            nvme_ctrl_identity_decode(nvme, 128, serial, model, &c);
            g_sink += c.nn;
        }
        t[1][k] = ns_since(&t0) / 200000;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 200000; i++) {
            struct nvme_ns_identity n;
            nvme_ns_identity_decode(nvme, &n);
            g_sink += n.nsze;
        }
        t[2][k] = ns_since(&t0) / 200000;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned i = 0; i < 200000; i++) {
            uint32_t last, bs;
            g_sink += scsi_inquiry_decode(inq, v, p, rv) + scsi_capacity10_decode(cap, &last, &bs) + last;
        }
        t[3][k] = ns_since(&t0) / 200000;
    }
    for (unsigned i = 0; i < 4; i++)
        qsort(t[i], 9, sizeof(double), cmp_double);
    printf("  bench ATA IDENTIFY %.1f ns, NVMe controller %.1f ns, NVMe namespace %.1f ns, SCSI INQUIRY + capacity %.1f ns\n",
           t[0][4], t[1][4], t[2][4], t[3][4]);
}

static const struct host_test tests[] = {
    { "diskid digest", test_digest },
};

int main(void)
{
    int rc = harness_run(tests, sizeof(tests) / sizeof(tests[0]));
    if (getenv("DISKID_BENCH") != NULL)
        diskid_bench();
    return rc;
}
