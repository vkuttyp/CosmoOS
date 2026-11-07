/*
 * test_cfscsum.c - cosmofs's file checksums (kernel-services/filesystem/cosmofs/cosmofs_csum.cee) on the host: roadmap
 * 2.0 SA7. A digest recorded from the C over files whose checksums are none, CRC32C or authenticated, on pools of
 * every version from 9 to 15, in the clear or with a version-1 key: entries put and got near the start of a file,
 * past the one-level index (the raise to 14 and the two-level index) and past the two-level one; verified reads,
 * scrubs and checks of blocks whose copies are good, rotten or all rotten, with tags that open and ones that do not,
 * with the key and without; compressed records of every shape; the whole index freed; names of every length (the
 * raise to 15) -- while the buffer cache, its copies on write, the raises and kmalloc fail now and then, with the real
 * CRC32C. Every argument, return, entry, freed block, logged line and the metadata blocks after are in the digest;
 * every buffer is put and nothing is left allocated.
 */

#include "harness.h"

#include <kernel/cosmofs.h>
#include <kernel/crc32c.h>
#include <kernel/errno.h>
#include <kernel/faultinject.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/lz4.h>

#include "cosmofs_internal.h"   /* before the C library's headers, as test_cfsparse.c: its signal.h is the kernel's */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The values the CeeSafe port writes as its own constants. */
_Static_assert(ENOENT == 2 && EIO == 5 && ENOMEM == 12 && EFBIG == 27 && ENAMETOOLONG == 36 && ENOKEY == 126 &&
                   EKEYREJECTED == 129,
               "cosmofs_csum's errno.h values");
_Static_assert(CFS_BLOCK == 4096 && CFS_MHDR_SIZE == 32 && CFS_PTRS_PER_BLOCK == 508 && CFS_CSUMIDX_SPAN == 508 &&
                   CFS_CSUMS_PER_BLOCK == 1016 && CFS_AEAD_PER_BLOCK == 127 &&
                   sizeof(struct cfs_csum_aead) == 32 && CFS_KIND_CSUM == 8 && CFS_KIND_CSUMIDX == 7 &&
                   CFS_KIND_CSUMIDX2 == 18 && CFS_CSUM_NONE == 0 && CFS_CSUM_POLY1305 == 2 && CFS_NAME_MAX == 47 &&
                   CFS_NAME_MAX_LONG == 255 && CFS_RECORD_BLOCKS == 8 && CFS_COMPRESS_LZ4 == 1 &&
                   CFS_EXT_COMPRESSED == 0x80000000u && CFS_EXT_COUNT_MASK == 0xFFFF && CFS_EXT_PSIZE_SHIFT == 16 &&
                   CFS_EXT_PSIZE_MASK == 0xFF && CFS_EXT_ALGO_SHIFT == 24 && CFS_EXT_ALGO_MASK == 0x7F &&
                   CHACHA20_KEY_SIZE == 32 && sizeof(struct cfs_block_aad) == 16 &&
                   offsetof(struct cfs_block_aad, lblk) == 8,
               "cosmofs_csum's cosmofs_format.h and cosmofs_internal.h values");
/* cosmofs_csum.cee keeps an authenticated entry in a stack slot of 4 words. */
_Static_assert(sizeof(struct cfs_csum_aead) <= 4 * 8, "cosmofs_csum.cee's stack slot");

/* The digest of the C (cosmofs_csum.c, 2026-11-07), which the CeeSafe port gives. */
#define CSUM_DIGEST 0x807c60c6070274a7ull
#define ROUNDS 2500u
#define SEED 0xC5C5A7E52026C5C5ull

static uint64_t g_rng, g_digest;

static uint64_t next(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static unsigned pick(unsigned n) { return (unsigned)(next() % n); }

static void mix(const void *bytes, size_t n)
{
    const uint8_t *b = bytes;
    for (size_t i = 0; i < n; i++) {
        g_digest ^= b[i];
        g_digest *= 0x100000001b3ull;
    }
}

static void note(uint32_t which, uint64_t value)
{
    mix(&which, sizeof(which));
    mix(&value, sizeof(value));
}

static void on_klog(int level, const char *line)
{
    note(1, (uint64_t)level);
    mix(line, strlen(line));
}

static bool g_faults;

static bool chance(unsigned n) { return g_faults && pick(n) == 0; }

static uint64_t hash(uint64_t x)
{
    x ^= x >> 31;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 29;
    x *= 0x94D049BB133111EBull;
    return x ^ (x >> 32);
}

/* ---- the allocator: malloc, counted; an allocation fails when g_fail_in counts down to it ---- */

static int g_fail_in = -1;
static long g_live;

void *kmalloc(size_t size, unsigned flags)
{
    if (size == 0)
        return NULL;
    if (g_fail_in >= 0 && g_fail_in-- == 0) {
        note(2, size);
        return NULL;
    }
    void *p = malloc(size);
    EXPECT(p != NULL);
    memset(p, (flags & KMEM_ZERO) ? 0 : 0xA5, size);
    g_live++;
    return p;
}

void kfree(void *p)
{
    if (p != NULL)
        g_live--;
    free(p);
}

/* ---- the metadata blocks and the buffer cache ---- */

#define NBLK 400u

struct mblock {
    uint64_t no;
    unsigned refs;
    _Alignas(16) uint8_t data[CFS_BLOCK];   /* as a buffer's: a page */
};

static struct mblock g_blk[NBLK];
static unsigned g_nblk;
static uint64_t g_next_no;
static int g_bufs;
static struct cfs g_fs;

static struct mblock *find(uint64_t no)
{
    for (unsigned i = 0; i < g_nblk; i++)
        if (g_blk[i].no == no)
            return &g_blk[i];
    return NULL;
}

static struct mblock *make(uint32_t kind)
{
    EXPECT(g_nblk < NBLK);
    struct mblock *m = &g_blk[g_nblk++];
    m->no = g_next_no++;
    m->refs = 0;
    memset(m->data, 0, CFS_BLOCK);
    ((struct cfs_mhdr *)m->data)->kind = kind;
    return m;
}

static struct cfs_buf *hold(struct mblock *m)
{
    struct cfs_buf *b = calloc(1, sizeof(*b));
    EXPECT(b != NULL);
    b->blkno = m->no;
    b->data = m->data;
    b->refs = 1;
    m->refs++;
    g_bufs++;
    return b;
}

struct cfs_mhdr *cfs_buf_hdr(struct cfs_buf *b) { return (struct cfs_mhdr *)b->data; }

int cfs_buf_get(struct cfs *fs, uint64_t blkno, uint32_t kind, struct cfs_buf **out)
{
    EXPECT(fs == &g_fs);
    note(10, blkno);
    note(11, kind);
    if (chance(40)) {
        note(12, 0);
        return -EIO;
    }
    struct mblock *m = find(blkno);
    if (m == NULL || (kind != 0 && ((struct cfs_mhdr *)m->data)->kind != kind))
        return -EIO;
    *out = hold(m);
    return 0;
}

int cfs_buf_new(struct cfs *fs, uint32_t kind, struct cfs_buf **out)
{
    EXPECT(fs == &g_fs);
    note(13, kind);
    if (chance(30) || g_nblk == NBLK) {
        note(14, 0);
        return -ENOSPC;
    }
    *out = hold(make(kind));
    return 0;
}

void cfs_buf_put(struct cfs *fs, struct cfs_buf *b)
{
    EXPECT(fs == &g_fs && g_bufs > 0);
    struct mblock *m = find(b->blkno);
    EXPECT(m != NULL && m->refs > 0);
    m->refs--;
    g_bufs--;
    note(15, b->blkno);
    free(b);
}

int cfs_buf_cow(struct cfs *fs, struct cfs_buf **bp, uint64_t *parent_slot)
{
    EXPECT(fs == &g_fs);
    struct cfs_buf *b = *bp;
    note(16, b->blkno);
    EXPECT(*parent_slot == b->blkno);
    if (chance(30) || g_nblk == NBLK) {
        note(17, 0);
        return -ENOSPC;
    }
    if (b->blkno >= 1000000)
        return 0;   /* written in this transaction already */
    struct mblock *m = make(((struct cfs_mhdr *)b->data)->kind);
    m->no += 1000000;   /* this transaction's */
    memcpy(m->data, b->data, CFS_BLOCK);
    cfs_buf_put(fs, b);
    *bp = hold(m);
    *parent_slot = m->no;
    return 0;
}

void cfs_free_block_deferred(struct cfs *fs, uint64_t blk)
{
    EXPECT(fs == &g_fs);
    note(18, blk);
}

int cfs_snap_raise(struct cfs *fs)
{
    EXPECT(fs == &g_fs);
    note(19, fs->sb.version);
    if (chance(5))
        return -EIO;
    if (fs->sb.version < 12)
        fs->sb.version = 12;
    return 0;
}

/* ---- the data blocks: each copy's bytes from its address; copy 0 rotten when the round says so ---- */

static unsigned g_copies;
static unsigned g_rot;        /* copies, from the first, that do not hold what was written */
static bool g_unreadable;     /* no copy can be read at all */

static void content(uint64_t dva, unsigned copy, uint8_t *out)
{
    for (unsigned i = 0; i < CFS_BLOCK; i += 8) {
        uint64_t v = hash(dva * 977 + i);
        memcpy(out + i, &v, 8);
    }
    if (copy < g_rot)
        out[copy * 13] ^= 0x40;
}

bool cfs_dva_valid(const struct cfs *fs, uint64_t dva)
{
    EXPECT(fs == &g_fs);
    note(20, dva);
    return dva < (1ull << 20);
}

int cfs_read_repair(struct cfs *fs, uint64_t dva, void *buf, bool (*verify)(const void *blk, void *arg), void *arg,
                    bool *repaired, bool *read_ok)
{
    EXPECT(fs == &g_fs && repaired == NULL && read_ok == NULL);
    note(21, dva);
    if (g_unreadable)
        return -EIO;
    for (unsigned c = 0; c < g_copies; c++) {
        content(dva, c, buf);
        if (verify(buf, arg)) {
            note(22, c);
            return 0;
        }
    }
    return -EIO;
}

int cfs_verify_all(struct cfs *fs, uint64_t dva, void *buf, bool (*verify)(const void *blk, void *arg), void *arg,
                   bool repair, unsigned *bad, unsigned *repaired)
{
    EXPECT(fs == &g_fs);
    note(23, dva);
    note(24, repair);
    if (g_unreadable)
        return -EIO;
    unsigned nbad = 0;
    int good = -1;
    uint8_t copy[CFS_BLOCK];
    for (unsigned c = 0; c < g_copies; c++) {
        content(dva, c, copy);
        if (verify(copy, arg)) {
            if (good < 0) {
                good = (int)c;
                memcpy(buf, copy, CFS_BLOCK);
            }
        } else {
            nbad++;
        }
    }
    *bad = nbad;
    *repaired = repair && good >= 0 ? nbad : 0;
    return good >= 0 ? 0 : -EIO;
}

/* ---- the cipher: a tag that opens only its own block, and a "decryption" that changes it ---- */

void cfs_file_key(const struct cfs *fs, uint64_t ino, uint8_t *out)
{
    EXPECT(fs == &g_fs);
    for (unsigned i = 0; i < CHACHA20_KEY_SIZE; i++)
        out[i] = (uint8_t)hash(ino * 64 + i);
}

static void tag_of(const uint8_t *key, const uint8_t *nonce, const struct cfs_block_aad *aad, uint32_t crc,
                   uint8_t *tag)
{
    uint64_t h = hash(key[0] ^ ((uint64_t)nonce[0] << 8) ^ (aad->ino << 16) ^ (aad->lblk << 40) ^ crc);
    for (unsigned i = 0; i < 16; i++)
        tag[i] = (uint8_t)hash(h + i);
}

bool chacha20_open(const uint8_t *key, const uint8_t *nonce, const void *aad, size_t aad_len, void *data, size_t len,
                   const uint8_t *tag)
{
    EXPECT(aad_len == sizeof(struct cfs_block_aad) && len == CFS_BLOCK);
    uint8_t want[16];
    tag_of(key, nonce, aad, crc32c(data, len), want);
    bool ok = memcmp(want, tag, 16) == 0;
    note(25, ok);
    if (ok)
        for (size_t i = 0; i < len; i++)
            ((uint8_t *)data)[i] ^= key[i % CHACHA20_KEY_SIZE];
    return ok;
}

size_t lz4_decompress(const void *src, size_t slen, void *dst, size_t dcap)
{
    const uint8_t *s = src;
    note(26, slen);
    note(27, dcap);
    uint8_t *d = dst;
    for (size_t i = 0; i < dcap; i++)
        d[i] = s[i % slen] ^ (uint8_t)i;
    return s[0] < 0x20 ? dcap - 1 - s[0] : dcap;   /* now and then a record a block short */
}

/* ---- a round ---- */

#define NINO 3u

static struct cfs_inode g_in[NINO];

static uint64_t some_lblk(void)
{
    unsigned w = pick(10);
    if (w < 6)
        return pick(3000);
    if (w < 8)
        return (uint64_t)CFS_CSUMIDX_SPAN * CFS_CSUMS_PER_BLOCK + pick(3 * CFS_CSUMS_PER_BLOCK);
    if (w < 9)
        return (uint64_t)CFS_CSUMIDX_SPAN * CFS_AEAD_PER_BLOCK + pick(3 * CFS_AEAD_PER_BLOCK);
    uint64_t span = (uint64_t)CFS_CSUMIDX_SPAN * CFS_CSUMIDX_SPAN;
    return span * CFS_AEAD_PER_BLOCK + pick(4);   /* past even the two-level index */
}

static uint64_t some_dva(void) { return pick(20) == 0 ? (1ull << 20) + pick(5) : pick(100000); }

/* Write what a read of `lblk` at `dva` should find: its entry, right or wrong. */
static void written(struct cfs_inode *in, uint64_t lblk, uint64_t dva)
{
    uint8_t block[CFS_BLOCK];
    content(dva, NINO, block);   /* a copy past every rotten one: what was written */
    uint32_t crc = crc32c(block, CFS_BLOCK);
    if (pick(6) == 0)
        crc ^= 1;
    int rc;
    if (in->csum_algo == CFS_CSUM_POLY1305) {
        struct cfs_csum_aead e;
        memset(&e, 0, sizeof(e));
        for (unsigned i = 0; i < 12; i++)
            e.nonce[i] = (uint8_t)next();
        e.crc = crc;
        uint8_t key[CHACHA20_KEY_SIZE];
        cfs_file_key(&g_fs, in->ino, key);
        struct cfs_block_aad aad = { .ino = in->ino, .lblk = lblk };
        tag_of(key, e.nonce, &aad, crc32c(block, CFS_BLOCK), e.tag);
        if (pick(6) == 0)
            e.tag[3] ^= 1;
        rc = cfs_aead_put(&g_fs, in, lblk, &e);
    } else {
        rc = cfs_csum_put(&g_fs, in, lblk, crc);
    }
    note(30, (uint64_t)(int64_t)rc);
}

static void op(void)
{
    struct cfs_inode *in = &g_in[pick(NINO)];
    unsigned w = pick(13);
    note(40, w);
    note(41, in->ino);
    uint64_t lblk = some_lblk();
    uint64_t dva = some_dva();
    unsigned kf = pick(10);
    g_fail_in = g_faults && kf == 0 ? (int)pick(2) : -1;
    int rc = 0;
    uint8_t *block = malloc(CFS_BLOCK * CFS_RECORD_BLOCKS);
    EXPECT(block != NULL);
    memset(block, 0, CFS_BLOCK * CFS_RECORD_BLOCKS);
    switch (w) {
    case 0: {
        uint32_t crc = (uint32_t)next();
        rc = cfs_csum_put(&g_fs, in, lblk, crc);
        break;
    }
    case 1: {
        uint32_t crc = 0xDEAD;
        rc = cfs_csum_get(&g_fs, in, lblk, &crc);
        note(42, crc);
        break;
    }
    case 2: {
        struct cfs_csum_aead e;
        for (unsigned i = 0; i < sizeof(e); i++)
            ((uint8_t *)&e)[i] = (uint8_t)next();
        rc = cfs_aead_put(&g_fs, in, lblk, &e);
        break;
    }
    case 3: {
        struct cfs_csum_aead e;
        memset(&e, 0xEE, sizeof(e));
        rc = cfs_aead_get(&g_fs, in, lblk, &e);
        mix(&e, sizeof(e));
        break;
    }
    case 4:
    case 5:
        if (pick(3) != 0)
            written(in, lblk, dva);
        rc = cfs_data_read_verified(&g_fs, in, lblk, dva, block);
        if (rc == 0)
            mix(block, CFS_BLOCK);
        break;
    case 6: {
        if (pick(3) != 0)
            written(in, lblk, dva);
        unsigned bad = 77, repaired = 77;
        bool repair = pick(2);
        rc = cfs_data_scrub_block(&g_fs, in, lblk, dva, block, repair, &bad, &repaired);
        note(43, bad);
        note(44, repaired);
        break;
    }
    case 7: {
        if (pick(3) != 0)
            written(in, lblk, dva);
        content(dva, pick(2) ? NINO : 0, block);
        rc = cfs_csum_verify(&g_fs, in, lblk, block);
        break;
    }
    case 8:
    case 9: {
        struct cfs_extent e;
        memset(&e, 0, sizeof(e));
        e.start = some_dva();
        e.lblk = (uint32_t)pick(100);
        uint32_t count = 1 + pick(CFS_RECORD_BLOCKS + 1);
        uint32_t psize = 1 + pick(count + 1);
        unsigned algo = pick(5) == 0 ? 2 : CFS_COMPRESS_LZ4;
        unsigned shape = pick(8);
        e.count = shape == 0 ? count : cfs_ext_pack(algo, psize, count);
        if (shape == 1)
            e.count = cfs_ext_pack(algo, psize, 0);
        for (uint32_t i = 0; i < psize && i < CFS_RECORD_BLOCKS && pick(2); i++)
            written(in, e.lblk + i, e.start + i);
        rc = cfs_record_read(&g_fs, in, &e, block);
        if (rc == 0)
            mix(block, (size_t)cfs_ext_count(&e) * CFS_BLOCK);
        break;
    }
    case 10:
        cfs_csum_free(&g_fs, in);
        break;
    case 11: {
        unsigned lw = pick(4);
        size_t len = lw == 0 ? pick(CFS_NAME_MAX + 1) : lw == 1 ? CFS_NAME_MAX + 1 + pick(220)
                                                    : lw == 2 ? CFS_NAME_MAX_LONG + pick(3) : CFS_NAME_MAX;
        rc = cfs_names_allow(&g_fs, len);
        break;
    }
    default:
        for (unsigned k = 0; k < 4; k++) {
            uint64_t at = some_lblk();
            written(in, at, some_dva());
        }
        break;
    }
    free(block);
    g_fail_in = -1;
    note(45, (uint64_t)(int64_t)rc);
    note(46, in->csum_root);
    note(47, g_fs.sb.version);
    note(48, g_fs.csum_failures);
    EXPECT(g_bufs == 0);
    EXPECT(g_live == 0);
}

static void round_(void)
{
    memset(&g_fs, 0, sizeof(g_fs));
    g_nblk = 0;
    g_next_no = 100;
    unsigned v = pick(8);
    g_fs.sb.version = v == 0 ? 9 : 10 + pick(6);
    g_fs.sb.key_root = pick(4) == 0 ? CFS_DVA(0, 9) : 0;
    g_fs.have_key = pick(5) != 0;
    g_copies = 1 + pick(3);
    g_rot = pick(4) == 0 ? g_copies : pick(2);
    g_unreadable = pick(12) == 0;
    for (unsigned i = 0; i < NINO; i++) {
        memset(&g_in[i], 0, sizeof(g_in[i]));
        g_in[i].ino = 2 + i;
        unsigned a = pick(4);
        g_in[i].csum_algo = a == 0 ? CFS_CSUM_NONE : a == 1 ? CFS_CSUM_POLY1305 : CFS_CSUM_CRC32C;
        unsigned d = pick(15);
        if (d == 0) {
            /* A damaged inode: its root names a block of another kind, or a two-level index on a pool too old to
             * have one. */
            struct mblock *m = make(pick(2) ? CFS_KIND_BITMAP : CFS_KIND_CSUMIDX2);
            g_in[i].csum_root = m->no;
        }
    }
    unsigned ops = 1 + pick(12);
    for (unsigned k = 0; k < ops; k++)
        op();
    for (unsigned i = 0; i < g_nblk; i++) {
        note(60, g_blk[i].no);
        note(61, crc32c(g_blk[i].data, CFS_BLOCK));
    }
}

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    harness_klog_hook = on_klog;
    harness_klog_min = KLOG_ERROR + 1;
    for (unsigned round = 0; round < ROUNDS; round++) {
        unsigned f = pick(3);
        g_faults = f != 0;
        round_();
    }
    harness_klog_hook = NULL;
    harness_klog_min = KLOG_WARN;
    printf("  csum digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, ROUNDS);
    EXPECT(g_digest == CSUM_DIGEST);
}

int main(void)
{
    static const struct host_test tests[] = {
        { "csum_digest", test_digest },
    };
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
