/*
 * test_cfsparse.c - Host test of cosmofs's decoders of on-disk metadata
 * (kernel-services/filesystem/cosmofs/cfsparse; roadmap 2.0 SA5, the
 * cosmofs family): a recorded digest over seeded inputs -- metadata blocks
 * with headers right and wrong, directory blocks of heads, continuations
 * and free slots under formats 14 and 15, pool geometries with addresses,
 * extents, direct maps and run lists in and out of range. Every result is
 * a line; the lines' FNV-1a digest is recorded below. The C gave it
 * (2026-11-01), and the CeeSafe that replaced it (cfsparse.cee) gives the
 * same. CFSPARSE_DUMP=1 prints the lines;
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
/* The digest of the C decoders (2026-11-01), which cfsparse.cee gives too. */
#define CFSPARSE_DIGEST 0x548e0e13fea79b80ull

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

/* One round's inputs, made before any is decoded so that the benchmark times the decoders alone. */
#define SLOTS (CFS_BLOCK / sizeof(struct cfs_dirent))

struct round {
    _Alignas(8) uint8_t mblock[CFS_BLOCK];
    uint64_t dva;
    uint32_t want;
    bool mac_on;
    _Alignas(8) uint8_t dblock[CFS_BLOCK];
    unsigned version;
    unsigned mut[SLOTS];   /* which name byte to change, per slot */
    struct cfs_memstate mem[4];
    unsigned nmem;
    uint64_t dvas[4];
    struct cfs_inode in;
    unsigned used;
};

/* --- metadata headers -------------------------------------------------- */

static void gen_mhdr(struct round *r)
{
    for (size_t i = 0; i < CFS_BLOCK; i += 8) {
        uint64_t x = next();
        memcpy(r->mblock + i, &x, 8);
    }
    struct cfs_mhdr *h = (struct cfs_mhdr *)r->mblock;
    r->dva = CFS_DVA(next() % 3, next() % 4096);
    uint32_t kind = (uint32_t)(1 + next() % 12);
    if (next() % 8)
        h->magic = CFS_MHDR_MAGIC;
    h->blkno = next() % 8 ? r->dva : r->dva ^ (1ull << (next() % 64));
    h->kind = next() % 8 ? kind : (uint32_t)next();
    uint32_t algos[] = { CFS_CSUM_NONE, CFS_CSUM_CRC32C, CFS_CSUM_POLY1305, 7 };
    h->csum_algo = algos[next() % 4];
    h->crc = own_crc(r->mblock);
    if (next() % 6 == 0)
        r->mblock[next() % CFS_BLOCK] ^= (uint8_t)(1u << (next() % 8));
    r->want = next() % 4 ? kind : 0;
    r->mac_on = next() % 6 == 0;
}

/* --- directory entries ------------------------------------------------- */

static void gen_dirents(struct round *r)
{
    struct cfs_dirent *d = (struct cfs_dirent *)r->dblock;
    memset(r->dblock, 0, CFS_BLOCK);
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
    r->version = next() % 2 ? 15 : 14;
    for (unsigned i = 0; i < SLOTS; i++)
        r->mut[i] = (unsigned)next();
}

/* --- addresses and extents --------------------------------------------- */

static uint64_t rand_dva(const struct round *r)
{
    unsigned v = (unsigned)(next() % (r->nmem + 1));
    uint64_t b = next() % 8 ? next() % (r->mem[v < 4 ? v : 0].nblocks + 4) : next();
    return next() % 16 ? CFS_DVA(v, b) : next();
}

static void rand_extent(const struct round *r, struct cfs_extent *e)
{
    e->start = rand_dva(r);
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

static void gen_extents(struct round *r)
{
    r->nmem = (unsigned)(1 + next() % 3);
    for (unsigned v = 0; v < 4; v++) {
        r->mem[v].nblocks = 64 + next() % 4096;
        r->mem[v].first_usable = v == 0 ? 2 : 1;
        r->mem[v].base = v == 0 ? 0 : r->mem[v - 1].base + r->mem[v - 1].nblocks;
    }
    for (unsigned k = 0; k < 4; k++)
        r->dvas[k] = rand_dva(r);
    memset(&r->in, 0, sizeof(r->in));
    r->used = (unsigned)(next() % (CFS_DIRECT + 1));
    uint32_t lblk = (uint32_t)(next() % 50);
    for (unsigned i = 0; i < r->used; i++) {
        rand_extent(r, &r->in.direct[i]);
        if (next() % 4) {   /* mostly sorted, sometimes not */
            r->in.direct[i].lblk = lblk;
            lblk += cfs_ext_count(&r->in.direct[i]) + (uint32_t)(next() % 4);
        }
    }
    if (next() % 8 == 0 && r->used < CFS_DIRECT)
        rand_extent(r, &r->in.direct[r->used + (next() % (CFS_DIRECT - r->used))]);   /* behind an empty slot */
}

static void gen_round(struct round *r)
{
    gen_mhdr(r);
    gen_dirents(r);
    gen_extents(r);
}

/* Every decoder on a round's inputs; its results as lines when `emit_lines`, else a checksum of them. */
static uint64_t decode_round(struct round *r, bool emit_lines)
{
    uint64_t acc = 0;
    char line[4096];
    int n = 0;
    uint32_t m = cfs_mhdr_decode(r->mblock, r->dva, r->want, r->mac_on);
    uint32_t c = cfs_block_crc(r->mblock, offsetof(struct cfs_mhdr, crc));
    acc += m + c;
    if (emit_lines) {
        n = snprintf(line, sizeof(line), "m%u c%08x", m, c);
        emit(line, n);
        n = snprintf(line, sizeof(line), "d%u", r->version);
    }
    struct cfs_dirent *d = (struct cfs_dirent *)r->dblock;
    for (unsigned i = 0; i < SLOTS; i++) {
        unsigned slots = cfs_dirent_check(d, i, r->version);
        acc += slots;
        if (emit_lines)
            n += snprintf(line + n, sizeof(line) - (size_t)n, " %u", slots);
        if (slots == 0)
            continue;
        char name[CFS_NAME_MAX_LONG];
        unsigned len = cfs_dirent_name(d, i, name);
        int same = cfs_dirent_name_eq(d, i, name, len);
        if (len > 0)
            name[r->mut[i] % len] ^= 1;
        int changed = cfs_dirent_name_eq(d, i, name, len);
        int shorter = len > 0 ? cfs_dirent_name_eq(d, i, name, len - 1) : 9;
        acc += len + (unsigned)same + (unsigned)changed + (unsigned)shorter;
        if (emit_lines) {
            if (len > 0)
                name[r->mut[i] % len] ^= 1;   /* the name as read, for its hash */
            uint64_t h = 0xcbf29ce484222325ull;
            for (unsigned k = 0; k < len; k++) {
                h ^= (uint8_t)name[k];
                h *= 0x100000001b3ull;
            }
            n += snprintf(line + n, sizeof(line) - (size_t)n, ":%u:%llx:%d%d%d", len, (unsigned long long)h, same,
                          changed, shorter);
        }
    }
    if (emit_lines) {
        emit(line, n);
        n = snprintf(line, sizeof(line), "x%u", r->nmem);
    }
    struct cfs fs;
    memset(&fs, 0, sizeof(fs));
    fs.mem = r->mem;
    fs.nmembers = r->nmem;
    for (unsigned k = 0; k < 4; k++) {
        bool ok = cfs_dva_valid(&fs, r->dvas[k]);
        uint64_t lin = cfs_dva_lin(&fs, r->dvas[k]);
        acc += ok + lin;
        if (emit_lines)
            n += snprintf(line + n, sizeof(line) - (size_t)n, " a%d:%llx", ok, (unsigned long long)lin);
    }
    for (unsigned i = 0; i < CFS_DIRECT; i++) {
        bool ok = cfs_extent_valid(&fs, &r->in.direct[i]);
        acc += ok;
        if (emit_lines)
            n += snprintf(line + n, sizeof(line) - (size_t)n, " e%d", ok);
    }
    bool dv = cfs_direct_valid(&fs, &r->in), lv = cfs_extents_valid(&fs, r->in.direct, r->used);
    acc += dv + lv;
    if (emit_lines) {
        n += snprintf(line + n, sizeof(line) - (size_t)n, " D%d L%d", dv, lv);
        emit(line, n);
    }
    return acc;
}

static void test_recorded_digest(void)
{
    g_dump = getenv("CFSPARSE_DUMP") != NULL;
    g_digest = 0xcbf29ce484222325ull;
    g_rng = SEED;
    struct round *r = malloc(sizeof(*r));
    EXPECT(r != NULL);
    if (r == NULL)
        return;
    for (unsigned i = 0; i < ROUNDS; i++) {
        gen_round(r);
        decode_round(r, true);
    }
    free(r);
    printf("  cfsparse digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, ROUNDS);
    EXPECT(g_digest == CFSPARSE_DIGEST);
}

static volatile uint64_t g_sink;

/* CFSPARSE_BENCH=1: the decoders alone on 2,000 rounds made beforehand, ns per round (best of 15). */
static int bench(void)
{
    enum { N = 2000 };
    struct round *rs = malloc(sizeof(*rs) * N);
    if (rs == NULL)
        return 1;
    g_rng = SEED;
    for (unsigned i = 0; i < N; i++)
        gen_round(&rs[i]);
    double best = 1e30;
    for (int run = 0; run < 15; run++) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        uint64_t acc = 0;
        for (unsigned i = 0; i < N; i++)
            acc += decode_round(&rs[i], false);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        g_sink += acc;
        double ns = (double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec);
        if (ns < best)
            best = ns;
    }
    printf("cfsparse bench: %.2f ns per round (best of 15 runs of %u)\n", best / N, N);
    free(rs);
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
