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

/* --- batch 2: the records a mount reads first --------------------------- */

#define MOUNT_ROUNDS 50000
#define MOUNT_SEED 0xC05F5FA2026B0002ull
/* The digest of the C decoders (cfsmount.c, 2026-11-01), which cfsparse.cee replaced. */
#define MOUNT_DIGEST 0x5d91f9b738036dfeull

static uint64_t edge(void)
{
    static const uint64_t e[] = { 0, 1, 2, 3, 63, 64, 65, 4095, 4096, 4097, 32512, 32513, 0xff00, 0xffffffffull,
                                  CFS_MAX_BLOCKS - 1, CFS_MAX_BLOCKS, CFS_MAX_BLOCKS + 1,
                                  CFS_MAX_BLOCKS * (uint64_t)CFS_MAX_MEMBERS, 0xffffffffffffffffull };
    return next() % 2 ? e[next() % (sizeof(e) / sizeof(e[0]))] : next() % 100000;
}

/* The test's own CRC of a superblock or label (the field at `off`), as own_crc does a header's. */
static uint32_t own_crc_at(const uint8_t *block, size_t off)
{
    static const uint8_t zero4[4] = { 0 };
    uint32_t c = crc32c(block, off);
    c = crc32c_update(c, zero4, 4);
    return crc32c_update(c, block + off + 4, CFS_BLOCK - off - 4);
}

/* One mount round's inputs, made before any is decoded so that the benchmark times the decoders alone. */
struct mround {
    _Alignas(8) uint8_t sb[CFS_BLOCK];
    _Alignas(8) uint8_t lb[CFS_BLOCK];
    struct cfs_member m;
    struct cfs_inode in;
    uint64_t dev0, count, ino;
    unsigned v, nmem;
};

static void gen_mount(struct mround *r)
{
    /* A superblock. */
    memset(r->sb, 0, CFS_BLOCK);
    struct cfs_super *sb = (struct cfs_super *)r->sb;
    memcpy(sb->magic, next() % 8 ? CFS_MAGIC : "COSMOFS2", 8);
    sb->version = (uint32_t)(next() % 8 ? 1 + next() % 16 : next());
    sb->block_size = next() % 8 ? CFS_BLOCK : (uint32_t)edge();
    sb->total_blocks = edge();
    sb->generation = next() % 8 ? 1 + next() % 100 : 0;
    sb->imap_root = next() % 2 ? CFS_DVA(next() % 300, edge()) : edge();
    sb->alloc_root = edge();
    sb->members = next() % 2 ? CFS_DVA(next() % 300, edge()) : edge();
    sb->key_root = next() % 4 ? 0 : 1 + next() % 100;
    sb->crc = own_crc_at(r->sb, offsetof(struct cfs_super, crc));
    if (next() % 6 == 0)
        r->sb[next() % CFS_BLOCK] ^= (uint8_t)(1u << (next() % 8));
    r->dev0 = next() % 2 ? sb->total_blocks : edge();
    /* A label. */
    memset(r->lb, 0, CFS_BLOCK);
    struct cfs_label *l = (struct cfs_label *)r->lb;
    memcpy(l->magic, next() % 8 ? CFS_LABEL_MAGIC : "COSMOMBX", 8);
    l->index = (uint32_t)(next() % 2 ? next() % 300 : next());
    l->copy = (uint32_t)(next() % 2 ? next() % 6 : next());
    l->nblocks = edge();
    l->generation = next();
    l->crc = own_crc_at(r->lb, offsetof(struct cfs_label, crc));
    if (next() % 6 == 0)
        r->lb[next() % CFS_BLOCK] ^= (uint8_t)(1u << (next() % 8));
    /* A member of the table, its index and the table's count. */
    struct cfs_member *m = &r->m;
    memset(m, 0, sizeof(*m));
    m->nblocks = edge();
    m->first_usable = next() % 2 ? 1 + next() % 3 : edge();
    m->alloc_root = next() % 2 ? CFS_DVA(next() % 6, edge()) : edge();
    m->free_blocks = next() % 2 ? m->nblocks - next() % 4 : edge();
    m->copies = (uint32_t)(next() % 2 ? next() % 6 : next());
    r->v = (unsigned)(next() % 4);
    r->nmem = (unsigned)(1 + next() % 5);
    r->count = edge();
    /* An inode slot. */
    memset(&r->in, 0, sizeof(r->in));
    r->ino = 1 + next() % 100;
    r->in.ino = next() % 4 ? r->ino : edge();
    r->in.nlink = (uint32_t)(next() % 4 ? 1 + next() % 3 : 0);
}

static void decode_mount(const struct mround *r)
{
    char line[512];
    int n;
    const struct cfs_super *sb = (const struct cfs_super *)r->sb;
    n = snprintf(line, sizeof(line), "s%d%d%d%d", cfs_super_ident_ok(sb), cfs_super_shape_ok(sb, r->dev0),
                 cfs_super_crc_ok(r->sb), cfs_super_peek_ok(r->sb));
    emit(line, n);
    n = snprintf(line, sizeof(line), "l%08x %d%d", cfs_label_crc(r->lb), cfs_label_crc_ok(r->lb),
                 cfs_label_shape_ok(r->lb));
    emit(line, n);
    n = snprintf(line, sizeof(line), "m%d c%d%d%d i%d", cfs_member_sane(&r->m, r->v, r->nmem),
                 cfs_members_count_ok(r->count), cfs_freelog_count_ok(r->count), cfs_orphan_count_ok(r->count),
                 cfs_inode_slot_ok(&r->in, r->ino));
    emit(line, n);
}

/* The checks alone, without the block CRCs (the same crc32c in both builds): what the port changed. */
static unsigned check_mount(const struct mround *r)
{
    const struct cfs_super *sb = (const struct cfs_super *)r->sb;
    return (unsigned)cfs_super_ident_ok(sb) + cfs_super_shape_ok(sb, r->dev0) + cfs_label_shape_ok(r->lb) +
           cfs_member_sane(&r->m, r->v, r->nmem) + cfs_members_count_ok(r->count) +
           cfs_freelog_count_ok(r->count) + cfs_orphan_count_ok(r->count) + cfs_inode_slot_ok(&r->in, r->ino);
}

static void run_mount(void)
{
    static struct mround r;
    gen_mount(&r);
    decode_mount(&r);
}

/* CFSPARSE_BENCH=mount: batch 2's checks alone, then with the CRCs, on 2,000 rounds made beforehand (best of 15). */
static int bench_mount(void)
{
    enum { N = 2000 };
    struct mround *rs = malloc(sizeof(*rs) * N);
    if (rs == NULL)
        return 1;
    g_rng = MOUNT_SEED;
    for (unsigned i = 0; i < N; i++)
        gen_mount(&rs[i]);
    for (int crc = 0; crc < 2; crc++) {
        double best = 1e30;
        for (int run = 0; run < 15; run++) {
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            uint64_t acc = 0;
            for (unsigned i = 0; i < N; i++) {
                acc += check_mount(&rs[i]);
                if (crc)
                    acc += cfs_super_crc_ok(rs[i].sb) + cfs_super_peek_ok(rs[i].sb) + cfs_label_crc_ok(rs[i].lb);
            }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            g_sink += acc;
            double ns = (double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec);
            if (ns < best)
                best = ns;
        }
        printf("%s %.2f ns  ", crc ? "with crc" : "checks", best / N);
    }
    printf("per round (best of 15 runs of %u)\n", N);
    free(rs);
    return 0;
}

static void test_mount_digest(void)
{
    g_dump = getenv("CFSPARSE_DUMP") != NULL;
    g_digest = 0xcbf29ce484222325ull;
    g_rng = MOUNT_SEED;
    for (unsigned r = 0; r < MOUNT_ROUNDS; r++)
        run_mount();
    printf("  cfsparse mount digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, MOUNT_ROUNDS);
    EXPECT(g_digest == MOUNT_DIGEST);
}

/* --- batch 3: the records snapshots, rollback and encryption keep ------- */

#define RECS_ROUNDS 50000
#define RECS_SEED 0xC05F5FA2026B0003ull
/* The digest of the C decoders (cfsrecs.c, 2026-11-01), which cfsparse.cee replaced. */
#define RECS_DIGEST 0xb62644e6cd01c30cull

/* One round's inputs: a snapshot list block read under a pool version, a
 * rollback record, a key slot at an index. */
struct rround {
    _Alignas(8) uint8_t list[CFS_BLOCK];
    struct cfs_rollback rb;
    struct cfs_keyslot ks;
    uint32_t version;
    unsigned entry, slot;
};

static uint64_t fnv_bytes(const void *p, size_t n)
{
    const uint8_t *b = p;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static void uuid_fill(uint8_t u[16], uint64_t how)
{
    memset(u, 0, 16);
    if (how % 4 != 0)   /* a quarter of them all zero */
        for (unsigned i = 0; i < 16; i++)
            u[i] = (uint8_t)(i % 3 == 0 ? 0 : next());
}

static void gen_recs(struct rround *r)
{
    /* A snapshot list block, either layout: entries whose names are empty
     * a third of the time, the rest of every byte random. */
    for (unsigned i = 0; i < CFS_BLOCK; i += 8) {
        uint64_t w = next();
        memcpy(r->list + i, &w, 8);
    }
    for (unsigned i = 0; i < CFS_SNAPS_PER_BLOCK2; i++)
        if (next() % 3 == 0)
            r->list[CFS_MHDR_SIZE + 16 + i * sizeof(struct cfs_snapshot) + offsetof(struct cfs_snapshot, name)] = 0;
    for (unsigned i = 0; i < CFS_SNAPS_PER_BLOCK; i++)
        if (next() % 3 == 0)
            r->list[CFS_MHDR_SIZE + 8 + i * sizeof(struct cfs_snapshot_v11) + offsetof(struct cfs_snapshot_v11, name)] =
                0;
    r->version = (uint32_t)(2 + next() % 14);
    r->entry = (unsigned)(next() % 48);
    /* A rollback record near a valid one, sealed by the test's own CRC. */
    struct cfs_rollback *b = &r->rb;
    for (unsigned i = 0; i < sizeof(*b); i += 8) {
        uint64_t w = next();
        memcpy((uint8_t *)b + i, &w, sizeof(*b) - i < 8 ? sizeof(*b) - i : 8);
    }
    b->magic = next() % 8 ? CFS_ROLLBACK_MAGIC : (uint32_t)next();
    b->version = next() % 8 ? 1 : (uint32_t)(next() % 3);
    b->state = (uint32_t)(next() % 8 ? next() % 4 : next());
    static const uint32_t fl[] = { 0, 0, 0, CFS_ROLLBACK_F_FROM_HELD, CFS_ROLLBACK_F_FROM_HELD, 2, 3, 0x80000000u };
    b->flags = fl[next() % (sizeof(fl) / sizeof(fl[0]))];
    uuid_fill(b->safety_uuid, next());
    if (next() % 4 == 0)
        memcpy(b->target_uuid, b->safety_uuid, 16);
    else
        uuid_fill(b->target_uuid, next());
    b->crc = 0;
    b->crc = crc32c(b, sizeof(*b));
    if (next() % 6 == 0)
        ((uint8_t *)b)[next() % sizeof(*b)] ^= (uint8_t)(1u << (next() % 8));
    /* A key slot near every bound of its parameters. */
    struct cfs_keyslot *k = &r->ks;
    memset(k, 0, sizeof(*k));
    k->kind = (uint32_t)(next() % 8 ? next() % 4 : next());
    k->t_cost = (uint32_t)(next() % 4 ? next() % 4 : next());
    static const uint32_t ln[] = { 0, 1, 2, 4, 0xfffffe, 0xffffff, 0x1000000, 0x1fffffff, 0x20000000, 0x40000000,
                                   0x80000000u, 0xffffffffu };
    k->lanes = next() % 2 ? ln[next() % (sizeof(ln) / sizeof(ln[0]))] : (uint32_t)next();
    uint32_t want = 8u * k->lanes;
    static const uint32_t mc[] = { 0, 7, 8, 16, 64, 65536, 0xffffffffu };
    switch (next() % 4) {
    case 0: k->m_cost_kib = mc[next() % (sizeof(mc) / sizeof(mc[0]))]; break;
    case 1: k->m_cost_kib = want; break;
    case 2: k->m_cost_kib = want - 1; break;
    default: k->m_cost_kib = (uint32_t)next(); break;
    }
    r->slot = (unsigned)(next() % 4);
}

static void decode_recs(const struct rround *r)
{
    static struct cfs fs;
    char line[256];
    int n;
    fs.sb.version = r->version;
    struct cfs_buf b;
    memset(&b, 0, sizeof(b));
    b.data = (uint8_t *)r->list;
    struct cfs_snapshot s;
    memset(&s, 0xa5, sizeof(s));   /* an entry not returned leaves it as it was */
    int got = cfs_snaplist_get(&fs, &b, r->entry, &s);
    n = snprintf(line, sizeof(line), "n%d %016llx", got, (unsigned long long)fnv_bytes(&s, sizeof(s)));
    emit(line, n);
    struct cfs_rollback out;
    memset(&out, 0xa5, sizeof(out));
    int rc = cfs_rollback_parse(&r->rb, &out);
    n = snprintf(line, sizeof(line), "r%d %016llx %08x", rc, (unsigned long long)fnv_bytes(&out, sizeof(out)),
                 cfs_rollback_crc(&r->rb));
    emit(line, n);
    n = snprintf(line, sizeof(line), "k%d", cfs_keyslot_valid(r->slot, &r->ks));
    emit(line, n);
}

static void test_recs_digest(void)
{
    static struct rround r;
    g_dump = getenv("CFSPARSE_DUMP") != NULL;
    g_digest = 0xcbf29ce484222325ull;
    g_rng = RECS_SEED;
    for (unsigned i = 0; i < RECS_ROUNDS; i++) {
        gen_recs(&r);
        decode_recs(&r);
    }
    printf("  cfsparse records digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, RECS_ROUNDS);
    EXPECT(g_digest == RECS_DIGEST);
}

/* Argon2id's bound on lanes (RFC 9106; userland/lib/argon2.c): 8 * lanes
 * must not wrap into a memory cost any slot meets. */
static void test_keyslot_lanes(void)
{
    struct cfs_keyslot k;
    memset(&k, 0, sizeof(k));
    k.kind = CFS_KEYSLOT_PASSPHRASE;
    k.t_cost = 3;
    k.lanes = 0x20000000;   /* 8 * lanes wraps to 0 */
    k.m_cost_kib = 0;
    EXPECT(!cfs_keyslot_valid(1, &k));
    k.lanes = 0x1000000;
    k.m_cost_kib = 0x8000000;
    EXPECT(!cfs_keyslot_valid(1, &k));
    k.lanes = 0xffffff;
    k.m_cost_kib = 8u * 0xffffff;
    EXPECT(cfs_keyslot_valid(1, &k));
    k.m_cost_kib -= 1;
    EXPECT(!cfs_keyslot_valid(1, &k));
}

/* CFSPARSE_BENCH=records: batch 3's decoders alone on 2,000 rounds made beforehand (best of 15). */
static int bench_recs(void)
{
    enum { N = 2000 };
    struct rround *rs = malloc(sizeof(*rs) * N);
    if (rs == NULL)
        return 1;
    static struct cfs fs;
    g_rng = RECS_SEED;
    for (unsigned i = 0; i < N; i++)
        gen_recs(&rs[i]);
    const char *what[] = { "snaplist", "rollback", "keyslot" };
    for (int d = 0; d < 3; d++) {
        double best = 1e30;
        for (int run = 0; run < 15; run++) {
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            uint64_t acc = 0;
            for (unsigned i = 0; i < N; i++) {
                const struct rround *r = &rs[i];
                if (d == 0) {
                    fs.sb.version = r->version;
                    struct cfs_buf b = { .data = (uint8_t *)r->list };
                    struct cfs_snapshot s;
                    acc += cfs_snaplist_get(&fs, &b, r->entry, &s) ? s.id : 1;
                } else if (d == 1) {
                    struct cfs_rollback out;
                    acc += (uint64_t)cfs_rollback_parse(&r->rb, &out) + out.generation;
                } else {
                    acc += cfs_keyslot_valid(r->slot, &r->ks);
                }
            }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            g_sink += acc;
            double ns = (double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec);
            if (ns < best)
                best = ns;
        }
        printf("%s %.2f ns  ", what[d], best / N);
    }
    printf("per round (best of 15 runs of %u)\n", N);
    free(rs);
    return 0;
}

static const struct host_test tests[] = {
    { "recorded digest", test_recorded_digest },
    { "mount digest", test_mount_digest },
    { "records digest", test_recs_digest },
    { "keyslot lanes", test_keyslot_lanes },
};

int main(void)
{
    const char *b = getenv("CFSPARSE_BENCH");
    if (b != NULL)
        return strcmp(b, "mount") == 0 ? bench_mount() : strcmp(b, "records") == 0 ? bench_recs() : bench();
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
