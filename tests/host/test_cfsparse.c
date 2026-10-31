/*
 * test_cfsparse.c - Host test of cosmofs's decoders of on-disk metadata
 * (kernel-services/filesystem/cosmofs/cfsparse; roadmap 2.0 SA5, the
 * cosmofs family): a recorded digest over seeded inputs -- metadata blocks
 * with headers right and wrong, directory blocks of heads, continuations
 * and free slots under formats 14 and 15, pool geometries with addresses,
 * extents, direct maps and run lists in and out of range. Every result is
 * a line; the lines' FNV-1a digest is recorded below. The C gave it; the
 * CeeSafe must give the same. CFSPARSE_DUMP=1 prints the lines;
 * CFSPARSE_BENCH=1 times the decoders. ASan and UBSan.
 */

#include "harness.h"

#include <kernel/crc32c.h>

#include "cosmofs_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ROUNDS 20000
#define SEED 0xC05F5FA2026B0001ull
/* The digest of the C decoders (cfsparse.c, 2026-11-01). */
#define CFSPARSE_DIGEST 0x8d9223e072453d78ull

static uint64_t g_rng;

static uint64_t next(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static uint64_t g_digest;
static bool g_dump;

static void emit(const char *line, int n)
{
    EXPECT(n > 0 && n < 4096);
    for (int i = 0; i < n; i++) {
        g_digest ^= (uint8_t)line[i];
        g_digest *= 0x100000001b3ull;
    }
    g_digest ^= '\n';
    g_digest *= 0x100000001b3ull;
    if (g_dump)
        printf("%s\n", line);
}

/* The test's own CRC over a header block, independent of the decoders under test. */
static uint32_t own_crc(const uint8_t *block)
{
    static const uint8_t zero4[4] = { 0 };
    size_t off = offsetof(struct cfs_mhdr, crc);
    uint32_t c = crc32c(block, off);
    c = crc32c_update(c, zero4, 4);
    return crc32c_update(c, block + off + 4, CFS_BLOCK - off - 4);
}

static uint8_t g_block[CFS_BLOCK];

/* --- metadata headers -------------------------------------------------- */

static void run_mhdr(void)
{
    for (size_t i = 0; i < CFS_BLOCK; i += 8) {
        uint64_t r = next();
        memcpy(g_block + i, &r, 8);
    }
    struct cfs_mhdr *h = (struct cfs_mhdr *)g_block;
    uint64_t dva = CFS_DVA(next() % 3, next() % 4096);
    uint32_t kind = (uint32_t)(1 + next() % 12);
    if (next() % 8)
        h->magic = CFS_MHDR_MAGIC;
    h->blkno = next() % 8 ? dva : dva ^ (1ull << (next() % 64));
    h->kind = next() % 8 ? kind : (uint32_t)next();
    uint32_t algos[] = { CFS_CSUM_NONE, CFS_CSUM_CRC32C, CFS_CSUM_POLY1305, 7 };
    h->csum_algo = algos[next() % 4];
    h->crc = own_crc(g_block);
    if (next() % 6 == 0)
        g_block[next() % CFS_BLOCK] ^= (uint8_t)(1u << (next() % 8));
    uint32_t want = next() % 4 ? kind : 0;
    bool mac_on = next() % 6 == 0;
    char line[128];
    int n = snprintf(line, sizeof(line), "m%u c%08x", cfs_mhdr_decode(g_block, dva, want, mac_on),
                     cfs_block_crc(g_block, offsetof(struct cfs_mhdr, crc)));
    emit(line, n);
}

/* --- directory entries ------------------------------------------------- */

#define SLOTS (CFS_BLOCK / sizeof(struct cfs_dirent))

static void run_dirents(void)
{
    struct cfs_dirent *d = (struct cfs_dirent *)g_block;
    memset(g_block, 0, CFS_BLOCK);
    unsigned s = 0;
    while (s < SLOTS) {
        unsigned kind = (unsigned)(next() % 6);
        if (kind == 0) {   /* a free slot */
            s++;
            continue;
        }
        unsigned len = next() % 4 ? (unsigned)(1 + next() % 47) : (unsigned)(next() % 300);
        d[s].ino = next() % 16 ? 1 + next() % 1000 : (next() % 2 ? 0 : CFS_DIRENT_CONT);
        d[s].type = (uint8_t)(next() % 8);
        d[s].namelen = (uint8_t)len;
        d[s].extra = (uint8_t)(next() % 8 ? cfs_dirent_extra(len > 255 ? 255 : len) : next() % 6);
        for (unsigned i = 0; i < 48; i++)
            d[s].name[i] = (char)('a' + next() % 26);
        unsigned extra = d[s].extra;
        s++;
        for (unsigned k = 0; k < extra && s < SLOTS; k++, s++) {
            struct cfs_dirent_cont *c = (struct cfs_dirent_cont *)&d[s];
            c->mark = next() % 12 ? CFS_DIRENT_CONT : next();
            for (unsigned i = 0; i < CFS_DIRENT_CONT_NAME; i++)
                c->name[i] = (char)('A' + next() % 26);
        }
    }
    unsigned version = next() % 2 ? 15 : 14;
    char line[4096];
    int n = snprintf(line, sizeof(line), "d%u", version);
    for (unsigned i = 0; i < SLOTS; i++) {
        unsigned slots = cfs_dirent_check(d, i, version);
        n += snprintf(line + n, sizeof(line) - (size_t)n, " %u", slots);
        if (slots == 0)
            continue;
        char name[CFS_NAME_MAX_LONG];
        unsigned len = cfs_dirent_name(d, i, name);
        uint64_t h = 0xcbf29ce484222325ull;
        for (unsigned k = 0; k < len; k++) {
            h ^= (uint8_t)name[k];
            h *= 0x100000001b3ull;
        }
        int same = cfs_dirent_name_eq(d, i, name, len);
        if (len > 0)
            name[next() % len] ^= 1;
        int changed = cfs_dirent_name_eq(d, i, name, len);
        int shorter = len > 0 ? cfs_dirent_name_eq(d, i, name, len - 1) : 9;
        n += snprintf(line + n, sizeof(line) - (size_t)n, ":%u:%llx:%d%d%d", len, (unsigned long long)h, same, changed,
                      shorter);
    }
    emit(line, n);
}

/* --- addresses and extents --------------------------------------------- */

static struct cfs_memstate g_mem[4];

static uint64_t rand_dva(unsigned nmem)
{
    unsigned v = (unsigned)(next() % (nmem + 1));
    uint64_t b = next() % 8 ? next() % (g_mem[v < 4 ? v : 0].nblocks + 4) : next();
    return next() % 16 ? CFS_DVA(v, b) : next();
}

static void rand_extent(struct cfs_extent *e, unsigned nmem)
{
    e->start = rand_dva(nmem);
    e->lblk = (uint32_t)(next() % 2 ? next() % 1000 : next());
    if (next() % 4) {
        e->count = (uint32_t)(1 + next() % 32);
    } else {
        unsigned algo = next() % 4 ? CFS_COMPRESS_LZ4 : (unsigned)(next() % 128);
        e->count = CFS_EXT_COMPRESSED | (algo & CFS_EXT_ALGO_MASK) << CFS_EXT_ALGO_SHIFT |
                   (uint32_t)(next() % 16) << CFS_EXT_PSIZE_SHIFT | (uint32_t)(next() % 16);
    }
    if (next() % 10 == 0)
        e->count = (uint32_t)next();
}

static void run_extents(void)
{
    struct cfs fs;
    memset(&fs, 0, sizeof(fs));
    unsigned nmem = (unsigned)(1 + next() % 3);
    for (unsigned v = 0; v < 4; v++) {
        g_mem[v].nblocks = 64 + next() % 4096;
        g_mem[v].first_usable = v == 0 ? 2 : 1;
        g_mem[v].base = v == 0 ? 0 : g_mem[v - 1].base + g_mem[v - 1].nblocks;
    }
    fs.mem = g_mem;
    fs.nmembers = nmem;
    char line[4096];
    int n = snprintf(line, sizeof(line), "x%u", nmem);
    for (unsigned k = 0; k < 4; k++) {
        uint64_t dva = rand_dva(nmem);
        n += snprintf(line + n, sizeof(line) - (size_t)n, " a%d:%llx", cfs_dva_valid(&fs, dva),
                      (unsigned long long)cfs_dva_lin(&fs, dva));
    }
    struct cfs_inode in;
    memset(&in, 0, sizeof(in));
    unsigned used = (unsigned)(next() % (CFS_DIRECT + 1));
    uint32_t lblk = (uint32_t)(next() % 50);
    for (unsigned i = 0; i < used; i++) {
        rand_extent(&in.direct[i], nmem);
        if (next() % 4) {   /* mostly sorted, sometimes not */
            in.direct[i].lblk = lblk;
            lblk += cfs_ext_count(&in.direct[i]) + (uint32_t)(next() % 4);
        }
    }
    if (next() % 8 == 0 && used < CFS_DIRECT)
        rand_extent(&in.direct[used + (next() % (CFS_DIRECT - used))], nmem);   /* a run behind an empty slot */
    for (unsigned i = 0; i < CFS_DIRECT; i++)
        n += snprintf(line + n, sizeof(line) - (size_t)n, " e%d", cfs_extent_valid(&fs, &in.direct[i]));
    n += snprintf(line + n, sizeof(line) - (size_t)n, " D%d L%d", cfs_direct_valid(&fs, &in),
                  cfs_extents_valid(&fs, in.direct, used));
    emit(line, n);
}

static void test_recorded_digest(void)
{
    g_dump = getenv("CFSPARSE_DUMP") != NULL;
    g_digest = 0xcbf29ce484222325ull;
    g_rng = SEED;
    for (unsigned r = 0; r < ROUNDS; r++) {
        run_mhdr();
        run_dirents();
        run_extents();
    }
    printf("  cfsparse digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, ROUNDS);
    EXPECT(g_digest == CFSPARSE_DIGEST);
}

static volatile uint64_t g_sink;

/* CFSPARSE_BENCH=1: the decoders on the same inputs, ns per round (best of 15). */
static int bench(void)
{
    double best = 1e30;
    for (int run = 0; run < 15; run++) {
        g_rng = SEED;
        g_dump = false;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (unsigned r = 0; r < 2000; r++) {
            run_mhdr();
            run_dirents();
            run_extents();
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        g_sink += g_digest;
        double ns = (double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec);
        if (ns < best)
            best = ns;
    }
    printf("cfsparse bench: %.2f ns per round (best of 15 runs of 2000)\n", best / 2000);
    return 0;
}

static const struct host_test tests[] = {
    { "recorded digest", test_recorded_digest },
};

int main(void)
{
    if (getenv("CFSPARSE_BENCH") != NULL)
        return bench();
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
