/*
 * test_cfsdata.c - a cosmofs file's blocks (kernel-services/filesystem/cosmofs/cosmofs_data.cee) on the host: roadmap
 * 2.0 SA7. A digest recorded from the C over inodes whose maps the test builds -- empty, a few runs in the direct
 * slots, chains of extent blocks of this generation or an older one, compressed records among the runs, chains of
 * thousands of runs up to and past the bound, and maps that are damaged (a cycle, a wild pointer, runs out of order)
 * -- through lookups, counts, runs and records set over what was there, truncations, block and record writes with no
 * checksums, CRC32C or seals, compressed or not, and the by-inode directory block helpers, with the real DVA and run
 * checks (cfsparse) and CRC32C, while the allocator, the reservations, the block writes, the checksum entries, the
 * codec, the reads and kmalloc fail now and then. Every argument, return, written block, freed block, entry, the
 * inode and its chain after are in the digest; every buffer is put and nothing is left allocated.
 */

#include "harness.h"

#include <kernel/chacha20.h>
#include <kernel/cosmofs.h>
#include <kernel/crc32c.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/lz4.h>

#include "cosmofs_internal.h"   /* before the C library's headers, as test_cfsparse.c: its signal.h is the kernel's */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The values the CeeSafe port writes as its own constants. */
_Static_assert(EIO == 5 && ENOMEM == 12 && EFBIG == 27 && ENOSPC == 28 && ENOKEY == 126 && KMEM_ZERO == 1 &&
                   LZ4_WORK_BYTES == 16384,
               "cosmofs_data's errno.h, kmalloc.h and lz4.h values");
_Static_assert(CFS_BLOCK == 4096 && CFS_MHDR_SIZE == 32 && CFS_DIRECT == 10 && CFS_EXTENTS_PER_BLOCK == 253 &&
                   CFS_MAX_EXTENTS == 4096 && CFS_PREP_MAX == 18 && CFS_KIND_EXTENTS == 6 && CFS_RECORD_BLOCKS == 8 &&
                   CFS_COMPRESS_LZ4 == 1 && CFS_CSUM_POLY1305 == 2 && CFS_ALLOC_DATA == 1 && CFS_ALLOC_META == 0 &&
                   CFS_EXT_COMPRESSED == 0x80000000u && CFS_EXT_COUNT_MASK == 0xFFFF && CFS_EXT_PSIZE_SHIFT == 16 &&
                   CFS_EXT_PSIZE_MASK == 0xFF && CFS_EXT_ALGO_SHIFT == 24 && CFS_EXT_ALGO_MASK == 0x7F &&
                   CHACHA20_KEY_SIZE == 32,
               "cosmofs_data's cosmofs_format.h and cosmofs_internal.h values");
_Static_assert(sizeof(struct cfs_extent) == 16 && sizeof(struct cfs_csum_aead) == 32 &&
                   sizeof(struct cfs_prep) == 296 && offsetof(struct cfs_extent_block, next) == 0 &&
                   offsetof(struct cfs_extent_block, ext) == 8 && sizeof(struct cfs_block_aad) == 16,
               "cosmofs_data.cee's sizes and offsets");
/* cosmofs_data.cee keeps a run in a stack slot of 2 words, an authenticated entry in one of 4 (a saved entry, 6 words:
 * a flag, the CRC with the kind, the entry) and an inode in one of 32. */
_Static_assert(sizeof(struct cfs_extent) <= 2 * 8 && sizeof(struct cfs_csum_aead) <= 4 * 8 &&
                   sizeof(struct cfs_inode) <= 32 * 8,
               "cosmofs_data.cee's stack slots");

/* The digest of the C (cosmofs_data.c, 2026-11-07), which the CeeSafe port gives. */
#define DATA_DIGEST 0xd968a547580d35fdull
#define ROUNDS 2500u
#define SEED 0xDA7ADA7A2026DA7Aull

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

static bool fail_now(size_t size)
{
    if (g_fail_in >= 0 && g_fail_in-- == 0) {
        note(2, size);
        return true;
    }
    return false;
}

void *kmalloc(size_t size, unsigned flags)
{
    if (size == 0 || fail_now(size))
        return NULL;
    void *p = malloc(size);
    EXPECT(p != NULL);
    memset(p, (flags & KMEM_ZERO) ? 0 : 0xA5, size);
    g_live++;
    return p;
}

void *kzalloc(size_t size) { return kmalloc(size, KMEM_ZERO); }

void *krealloc(void *ptr, size_t new_size, unsigned flags)
{
    EXPECT(ptr != NULL && flags == 0);
    note(3, new_size);
    if (fail_now(new_size))
        return NULL;
    void *p = realloc(ptr, new_size);
    EXPECT(p != NULL);
    return p;
}

void kfree(void *p)
{
    if (p != NULL)
        g_live--;
    free(p);
}

/* ---- the filesystem, its extent blocks and the buffer cache ---- */

#define NBLK 160u

struct mblock {
    uint64_t no;
    unsigned refs;
    _Alignas(16) uint8_t data[CFS_BLOCK];
};

static struct mblock g_blk[NBLK];
static unsigned g_nblk;
static uint64_t g_next_meta;
static int g_bufs;
static struct cfs g_fs;
static struct cfs_memstate g_mem[1];

static struct mblock *find(uint64_t no)
{
    for (unsigned i = 0; i < g_nblk; i++)
        if (g_blk[i].no == no)
            return &g_blk[i];
    return NULL;
}

static struct mblock *make(uint64_t no, uint64_t gen)
{
    EXPECT(g_nblk < NBLK);
    struct mblock *m = &g_blk[g_nblk++];
    m->no = no;
    m->refs = 0;
    memset(m->data, 0, CFS_BLOCK);
    struct cfs_mhdr *h = (struct cfs_mhdr *)m->data;
    h->kind = CFS_KIND_EXTENTS;
    h->generation = gen;
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
    EXPECT(fs == &g_fs && kind == CFS_KIND_EXTENTS);
    note(10, blkno);
    if (chance(50)) {
        note(11, 0);
        return -EIO;
    }
    struct mblock *m = find(blkno);
    if (m == NULL)
        return -EIO;
    *out = hold(m);
    return 0;
}

void cfs_buf_put(struct cfs *fs, struct cfs_buf *b)
{
    EXPECT(fs == &g_fs && g_bufs > 0);
    struct mblock *m = find(b->blkno);
    EXPECT(m != NULL && m->refs > 0);
    m->refs--;
    g_bufs--;
    free(b);
}

int cfs_prep_meta(struct cfs *fs, struct cfs_prep *p, unsigned n)
{
    EXPECT(fs == &g_fs && n <= CFS_PREP_MAX);
    note(12, n);
    if (chance(20)) {
        note(13, 0);
        return -ENOSPC;
    }
    p->n = n;
    p->used = 0;
    for (unsigned i = 0; i < n; i++)
        p->blk[i] = g_next_meta++;
    return 0;
}

static uint64_t prep_take(struct cfs_prep *p)
{
    EXPECT(p->used < p->n);
    return p->blk[p->used++];
}

void cfs_buf_cow_prep(struct cfs *fs, struct cfs_buf **bp, uint64_t *parent_slot, struct cfs_prep *p)
{
    EXPECT(fs == &g_fs);
    struct cfs_buf *b = *bp;
    EXPECT(*parent_slot == b->blkno);
    note(14, b->blkno);
    if (cfs_buf_hdr(b)->generation == fs->gen)
        return;   /* this transaction's already */
    struct mblock *m = make(prep_take(p), fs->gen);
    memcpy(m->data, b->data, CFS_BLOCK);
    ((struct cfs_mhdr *)m->data)->generation = fs->gen;
    note(15, b->blkno);   /* the old block, freed with the commit */
    cfs_buf_put(fs, b);
    *bp = hold(m);
    *parent_slot = m->no;
}

struct cfs_buf *cfs_buf_new_prep(struct cfs *fs, uint32_t kind, struct cfs_prep *p)
{
    EXPECT(fs == &g_fs && kind == CFS_KIND_EXTENTS);
    struct mblock *m = make(prep_take(p), fs->gen);
    note(16, m->no);
    return hold(m);
}

void cfs_prep_release(struct cfs *fs, struct cfs_prep *p)
{
    EXPECT(fs == &g_fs);
    note(17, p->n - p->used);
}

void cfs_free_block_deferred(struct cfs *fs, uint64_t blk)
{
    EXPECT(fs == &g_fs);
    note(18, blk);
}

void cfs_fail(struct cfs *fs, int rc)
{
    EXPECT(fs == &g_fs);
    note(19, (uint64_t)(int64_t)rc);
    if (fs->failed == 0)
        fs->failed = rc;
}

/* ---- data blocks, the allocator and the checksum entries ---- */

static uint64_t g_next_data;

int cfs_alloc_run(struct cfs *fs, enum cfs_alloc_class cls, uint64_t hint, uint32_t want, uint64_t *start,
                  uint64_t *got)
{
    EXPECT(fs == &g_fs);
    note(20, (uint64_t)cls);
    note(21, hint);
    note(22, want);
    if (chance(25)) {
        note(23, 0);
        return -ENOSPC;
    }
    uint64_t n = want;
    if (want > 1 && pick(6) == 0)
        n = pick(want);   /* not contiguous enough */
    if (hint != 0 && hint + want < 200000 && pick(2)) {
        *start = hint;    /* next to the previous block: runs merge */
    } else {
        *start = g_next_data;
        g_next_data += want + pick(3);
    }
    *got = n;
    return 0;
}

int cfs_data_write(struct cfs *fs, uint64_t blk, const void *buf)
{
    EXPECT(fs == &g_fs);
    note(24, blk);
    note(25, crc32c(buf, CFS_BLOCK));
    return chance(40) ? -EIO : 0;
}

#define NENT 4096u

struct entry {
    bool used;
    uint64_t lblk;
    uint32_t crc;
    struct cfs_csum_aead aead;
};

static struct entry g_ent[NENT];

static struct entry *entry_at(uint64_t lblk, bool make_it)
{
    unsigned h = (unsigned)(hash(lblk) % NENT);
    for (unsigned k = 0; k < NENT; k++) {
        struct entry *e = &g_ent[(h + k) % NENT];
        if (e->used && e->lblk == lblk)
            return e;
        if (!e->used) {
            if (!make_it)
                return NULL;
            e->used = true;
            e->lblk = lblk;
            return e;
        }
    }
    EXPECT(false);
    return NULL;
}

int cfs_csum_put(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, uint32_t crc)
{
    EXPECT(fs == &g_fs && in->csum_algo != CFS_CSUM_POLY1305);
    note(26, lblk);
    note(27, crc);
    if (chance(15))
        return -ENOSPC;
    entry_at(lblk, true)->crc = crc;
    return 0;
}

int cfs_csum_get(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, uint32_t *crc)
{
    EXPECT(fs == &g_fs);
    (void)in;
    note(28, lblk);
    if (chance(60))
        return -EIO;
    struct entry *e = entry_at(lblk, false);
    if (e == NULL)
        return -ENOENT;
    *crc = e->crc;
    return 0;
}

int cfs_aead_put(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, const struct cfs_csum_aead *e)
{
    EXPECT(fs == &g_fs && in->csum_algo == CFS_CSUM_POLY1305);
    note(29, lblk);
    mix(e, sizeof(*e));
    if (chance(15))
        return -ENOSPC;
    entry_at(lblk, true)->aead = *e;
    return 0;
}

int cfs_aead_get(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, struct cfs_csum_aead *e)
{
    EXPECT(fs == &g_fs);
    (void)in;
    note(30, lblk);
    if (chance(60))
        return -EIO;
    struct entry *x = entry_at(lblk, false);
    if (x == NULL)
        return -ENOENT;
    *e = x->aead;
    return 0;
}

static void fill(uint64_t seed, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i += 8) {
        uint64_t v = hash(seed * 4099 + i);
        memcpy(out + i, &v, 8);
    }
}

int cfs_record_read(struct cfs *fs, struct cfs_inode *in, const struct cfs_extent *e, uint8_t *out)
{
    EXPECT(fs == &g_fs);
    (void)in;
    note(31, e->start);
    note(32, e->count);
    if (chance(10))
        return -EIO;
    fill(e->start, out, (size_t)cfs_ext_count(e) * CFS_BLOCK);
    return 0;
}

int cfs_data_read_verified(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, uint64_t dva, void *buf)
{
    EXPECT(fs == &g_fs);
    note(33, in->ino);
    note(34, lblk);
    note(35, dva);
    if (chance(10))
        return -EIO;
    fill(dva, buf, CFS_BLOCK);
    return 0;
}

int cfs_inode_write(struct cfs *fs, uint64_t ino, const struct cfs_inode *in)
{
    EXPECT(fs == &g_fs);
    note(36, ino);
    mix(in, sizeof(*in));
    return chance(10) ? -EIO : 0;
}

void cfs_csum_free(struct cfs *fs, struct cfs_inode *in)
{
    EXPECT(fs == &g_fs);
    note(37, in->ino);
    in->csum_root = 0;
}

/* ---- the cipher and the codec ---- */

static uint64_t g_nonce;

void cfs_file_key(const struct cfs *fs, uint64_t ino, uint8_t *out)
{
    EXPECT(fs == &g_fs);
    for (unsigned i = 0; i < CHACHA20_KEY_SIZE; i++)
        out[i] = (uint8_t)hash(ino * 64 + i);
}

void cfs_block_nonce(uint8_t *nonce)
{
    g_nonce++;
    for (unsigned i = 0; i < 12; i++)
        nonce[i] = (uint8_t)(g_nonce >> (8 * (i % 8)));
}

void chacha20_seal(const uint8_t *key, const uint8_t *nonce, const void *aad, size_t aad_len, void *data, size_t len,
                   uint8_t *tag)
{
    EXPECT(aad_len == sizeof(struct cfs_block_aad) && len == CFS_BLOCK);
    const struct cfs_block_aad *a = aad;
    uint8_t *d = data;
    for (size_t i = 0; i < len; i++)
        d[i] ^= key[i % CHACHA20_KEY_SIZE] ^ nonce[i % 12];
    uint64_t h = hash(crc32c(d, len) ^ a->ino ^ (a->lblk << 20));
    for (unsigned i = 0; i < 16; i++)
        tag[i] = (uint8_t)hash(h + i);
}

size_t lz4_compress(const void *src, size_t slen, void *dst, size_t dcap, void *work)
{
    EXPECT(work != NULL);
    note(38, slen);
    note(39, dcap);
    unsigned w = pick(4);
    if (w == 0)
        return 0;   /* does not fit */
    size_t clen = w == 1 ? dcap : 1 + pick((unsigned)dcap);
    const uint8_t *s = src;
    uint8_t *d = dst;
    for (size_t i = 0; i < clen; i++)
        d[i] = s[i] ^ 0x5A;
    return clen;
}

/* ---- maps the test builds ---- */

static struct cfs_inode g_in;

static void put_run(struct cfs_extent *runs, unsigned *n, uint64_t start, uint32_t count, uint32_t lblk)
{
    runs[*n].start = start;
    runs[*n].count = count;
    runs[*n].lblk = lblk;
    (*n)++;
}

/* Sorted runs from logical block 0: plain ones of a few blocks, and records at aligned positions. */
static unsigned runs_of(struct cfs_extent *runs, unsigned want)
{
    unsigned n = 0;
    uint32_t lblk = 0;
    while (n < want) {
        if (pick(5) == 0) {
            lblk = (lblk + CFS_RECORD_BLOCKS - 1) / CFS_RECORD_BLOCKS * CFS_RECORD_BLOCKS;
            uint32_t psize = 1 + pick(CFS_RECORD_BLOCKS - 1);
            put_run(runs, &n, 1000 + g_next_data, cfs_ext_pack(CFS_COMPRESS_LZ4, psize, CFS_RECORD_BLOCKS), lblk);
            g_next_data += psize;
            lblk += CFS_RECORD_BLOCKS;
        } else {
            uint32_t count = 1 + pick(4);
            put_run(runs, &n, 1000 + g_next_data, count, lblk);
            g_next_data += count + 1;   /* a gap: no merge across */
            lblk += count + pick(3);
        }
    }
    return n;
}

static void build_map(void)
{
    memset(&g_in, 0, sizeof(g_in));
    g_in.ino = 5;
    unsigned a = pick(3);
    g_in.csum_algo = a == 0 ? CFS_CSUM_NONE : a == 1 ? CFS_CSUM_POLY1305 : CFS_CSUM_CRC32C;
    g_in.compress_algo = pick(3) ? CFS_COMPRESS_LZ4 : CFS_COMPRESS_NONE;
    unsigned shape = pick(20);
    unsigned want = shape == 0 ? 4096 + pick(5) : shape == 1 ? 600 + pick(400) : shape < 8 ? 11 + pick(500) : pick(11);
    static struct cfs_extent runs[4200];
    unsigned n = runs_of(runs, want);
    for (unsigned i = 0; i < n && i < CFS_DIRECT; i++)
        g_in.direct[i] = runs[i];
    unsigned left = n > CFS_DIRECT ? n - CFS_DIRECT : 0;
    uint64_t *slot = &g_in.indirect;
    const struct cfs_extent *rest = runs + CFS_DIRECT;
    uint64_t gen = pick(2) ? g_fs.gen : g_fs.gen - 1;
    struct mblock *first = NULL, *last = NULL;
    while (left > 0) {
        struct mblock *m = make(g_next_meta++, gen);
        struct cfs_extent_block *eb = (struct cfs_extent_block *)(m->data + CFS_MHDR_SIZE);
        unsigned take = left < CFS_EXTENTS_PER_BLOCK ? left : CFS_EXTENTS_PER_BLOCK;
        memcpy(eb->ext, rest, take * sizeof(*rest));
        rest += take;
        left -= take;
        *slot = m->no;
        slot = &eb->next;
        if (first == NULL)
            first = m;
        last = m;
    }
    unsigned damage = pick(25);
    if (damage == 0 && last != NULL)
        ((struct cfs_extent_block *)(last->data + CFS_MHDR_SIZE))->next = first->no;   /* a cycle */
    else if (damage == 1 && last != NULL)
        ((struct cfs_extent_block *)(last->data + CFS_MHDR_SIZE))->next = CFS_DVA(7, 9);   /* a wild pointer */
    else if (damage == 2 && n > 2)
        g_in.direct[1].lblk = g_in.direct[0].lblk;   /* out of order */
    g_in.size = (uint64_t)(n ? runs[n - 1].lblk + 8 : 0) * CFS_BLOCK;
}

/* ---- a round ---- */

static uint64_t some_lblk(void)
{
    unsigned w = pick(12);
    if (w == 0)
        return 0xFFFFFFFFull - pick(4);   /* at the end of the 32-bit logical space */
    if (w == 1)
        return pick(12000);
    return pick(200);
}

static void after(int rc)
{
    note(50, (uint64_t)(int64_t)rc);
    mix(&g_in, sizeof(g_in));
    note(51, (uint64_t)(int64_t)g_fs.failed);
    note(52, g_fs.map_stores);
    note(53, g_fs.last_store_n);
    uint64_t at = g_in.indirect;
    for (unsigned k = 0; at != 0 && k < 20; k++) {
        struct mblock *m = find(at);
        if (m == NULL)
            break;
        note(54, crc32c(m->data + CFS_MHDR_SIZE, CFS_BLOCK - CFS_MHDR_SIZE));
        at = ((struct cfs_extent_block *)(m->data + CFS_MHDR_SIZE))->next;
    }
    EXPECT(g_bufs == 0);
    EXPECT(g_live == 0);
}

static void op(void)
{
    unsigned w = pick(12);
    note(40, w);
    uint64_t lblk = some_lblk();
    unsigned kf = pick(12);
    g_fail_in = g_faults && kf <= 2 ? (int)pick(16) : -1;
    static uint8_t data[CFS_RECORD_BLOCKS * CFS_BLOCK];
    int rc = 0;
    switch (w) {
    case 0: {
        uint64_t pblk = 0xAAAA;
        rc = cfs_map(&g_fs, &g_in, lblk, &pblk);
        note(41, pblk);
        break;
    }
    case 1: {
        struct cfs_extent e;
        memset(&e, 0xBB, sizeof(e));
        rc = cfs_map_ext(&g_fs, &g_in, lblk, &e);
        mix(&e, sizeof(e));
        break;
    }
    case 2: {
        unsigned n = 0xCC;
        rc = cfs_extents_count(&g_fs, &g_in, &n);
        note(42, n);
        break;
    }
    case 3: {
        uint64_t old = 0xDD;
        bool want = pick(4) != 0;
        uint64_t pblk = 1000 + g_next_data++;
        rc = cfs_set_block(&g_fs, &g_in, lblk, pblk, want ? &old : NULL);
        note(43, old);
        break;
    }
    case 4: {
        struct cfs_extent ne;
        uint64_t start = 1000 + g_next_data;
        if (pick(3) == 0) {
            uint32_t psize = 1 + pick(CFS_RECORD_BLOCKS - 1);
            ne.count = cfs_ext_pack(CFS_COMPRESS_LZ4, psize, CFS_RECORD_BLOCKS);
            ne.lblk = (uint32_t)(lblk / CFS_RECORD_BLOCKS * CFS_RECORD_BLOCKS);
            g_next_data += psize;
        } else {
            ne.count = 1 + pick(20);
            ne.lblk = (uint32_t)lblk;
            g_next_data += ne.count;
        }
        ne.start = start;
        uint64_t old = 0xEE;
        bool want = pick(2);
        rc = cfs_set_extent(&g_fs, &g_in, &ne, want ? &old : NULL);
        note(44, old);
        break;
    }
    case 5: {
        unsigned kw = pick(4);
        uint64_t keep = kw == 0 ? 0 : kw == 1 ? lblk : pick(60);
        rc = cfs_truncate_blocks(&g_fs, &g_in, keep);
        break;
    }
    case 6:
    case 7: {
        fill(next(), data, CFS_BLOCK);
        rc = cfs_data_write_block(&g_fs, &g_in, lblk, data, pick(2) ? CFS_ALLOC_DATA : CFS_ALLOC_META);
        break;
    }
    case 8:
    case 9: {
        unsigned n = pick(3) ? CFS_RECORD_BLOCKS : 1 + pick(CFS_RECORD_BLOCKS);
        uint64_t at = pick(4) ? lblk / CFS_RECORD_BLOCKS * CFS_RECORD_BLOCKS : lblk;
        if (at > 0xFFFFFF00ull)
            at = 0xFFFFFF00ull;
        fill(next(), data, (size_t)n * CFS_BLOCK);
        bool compress = pick(2);
        rc = cfs_record_write(&g_fs, &g_in, at, data, n, compress);
        break;
    }
    case 10: {
        uint8_t block[CFS_BLOCK];
        memset(block, 0x77, sizeof(block));
        rc = cfs_dir_read_block_at(&g_fs, &g_in, lblk, block);
        if (rc == 0)
            note(45, crc32c(block, CFS_BLOCK));
        break;
    }
    default:
        fill(next(), data, CFS_BLOCK);
        rc = cfs_dir_write_block_at(&g_fs, &g_in, lblk, data);
        break;
    }
    g_fail_in = -1;
    after(rc);
}

static void round_(void)
{
    memset(&g_fs, 0, sizeof(g_fs));
    memset(g_mem, 0, sizeof(g_mem));
    memset(g_ent, 0, sizeof(g_ent));
    g_nblk = 0;
    g_next_meta = 500;
    g_next_data = 0;
    g_mem[0].nblocks = 1u << 20;
    g_mem[0].first_usable = 2;
    g_fs.mem = g_mem;
    g_fs.nmembers = 1;
    g_fs.gen = 10 + pick(10);
    g_fs.sb.version = 4 + pick(12);
    g_fs.have_key = pick(5) != 0;
    build_map();
    unsigned ops = 1 + pick(8);
    for (unsigned k = 0; k < ops; k++)
        op();
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
    printf("  data digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, ROUNDS);
    EXPECT(g_digest == DATA_DIGEST);
}

int main(void)
{
    static const struct host_test tests[] = {
        { "data_digest", test_digest },
    };
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
