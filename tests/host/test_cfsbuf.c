/*
 * test_cfsbuf.c - cosmofs's checksums, headers and buffer cache (kernel-services/filesystem/cosmofs/cosmofs_buf.c)
 * on the host: roadmap 2.0 SA7. A digest recorded from the C over seeded filesystems the test builds and owns, with the
 * real BLAKE2b, CRC32C and header and superblock decoders (cfsparse): metadata blocks sealed by CRC and by MAC, damaged
 * in each field a header has, read through a repair that has a good copy or not; superblocks of every version, CRC'd
 * and MAC'd, with and without their key; a cache filled past its size, so that clean buffers are evicted, with
 * buffers held, dirtied, cleaned, copied on write (deferred and exempt, from the commit's reservation or not) and set
 * aside ahead of a change; while reads, the allocator and kmalloc fail now and then. Every call the cache makes, every
 * argument, every return, every logged line, the cache's state and every byte it hands back are in the digest;
 * nothing is left allocated and no buffer is left held.
 */

#include "harness.h"

#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/list.h>
#include <kernel/log.h>
#include <kernel/storage.h>

#include "cosmofs_internal.h"   /* before the C library's headers, as test_cfsparse.c: its signal.h is the kernel's */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The digest of the C (cosmofs_buf.c, 2026-11-07), which the CeeSafe port gives. */
#define BUF_DIGEST 0x42be94733b86cd16ull
#define ROUNDS 2000u
#define SEED 0xB0F5CACE2026B0F5ull

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

/* ---- the allocator: malloc, counted; an allocation fails when g_fail_in counts down to it ---- */

static int g_fail_in = -1;
static long g_live;

void *kmalloc(size_t size, unsigned flags)
{
    if (size == 0)
        return NULL;   /* as kernel/memory/kmalloc.c */
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

void *kzalloc(size_t size) { return kmalloc(size, KMEM_ZERO); }

void kfree(void *p)
{
    if (p != NULL)
        g_live--;
    free(p);
}

/* ---- the pool: up to two members of NB blocks, a good copy of each and, now and then, a bad one ---- */

#define NB 160u
#define NMEM 2u

struct block {
    _Alignas(8) uint8_t data[CFS_BLOCK];
    uint8_t bad;       /* 0: the copy read first is good; else the byte offset + 1 the first copy has wrong */
    bool unreadable;   /* no copy can be read */
    bool lone;         /* a bad first copy has no good one behind it */
    bool made;         /* written yet this round: blocks are made when first read */
};

static struct block g_disk[NMEM * NB];
static unsigned g_nmem;
static unsigned g_read_fail;   /* of reads that fail outright: 0 for none */

static struct cfs *g_fs;
static void make_block(struct cfs *fs, uint64_t lin);

/* The block `dva` names, made now if this round has not read it before. */
static uint64_t disk_lin(uint64_t dva)
{
    unsigned v = CFS_DVA_VDEV(dva);
    uint64_t b = CFS_DVA_BLK(dva);
    EXPECT(v < g_nmem && b < NB);
    uint64_t lin = v * NB + b;
    if (!g_disk[lin].made)
        make_block(g_fs, lin);
    return lin;
}

int pool_read(struct spool *p, uint64_t dva, void *buf)
{
    (void)p;
    note(3, dva);
    if (g_read_fail != 0 && pick(g_read_fail) == 0)
        return -EIO;
    memcpy(buf, g_disk[disk_lin(dva)].data, CFS_BLOCK);
    return 0;
}

int pool_write(struct spool *p, uint64_t dva, const void *buf)
{
    (void)p;
    note(4, dva);
    if (g_read_fail != 0 && pick(g_read_fail) == 0)
        return -EIO;
    memcpy(g_disk[disk_lin(dva)].data, buf, CFS_BLOCK);
    return 0;
}

int cfs_read_repair(struct cfs *fs, uint64_t dva, void *buf, bool (*verify)(const void *blk, void *arg), void *arg,
                    bool *repaired, bool *read_ok)
{
    (void)fs;
    note(5, dva);
    *read_ok = false;
    if (repaired != NULL)
        *repaired = false;
    if (chance(60))
        return -ENOMEM;
    struct block *k = &g_disk[disk_lin(dva)];
    if (k->unreadable || (g_read_fail != 0 && pick(g_read_fail) == 0))
        return -EIO;
    *read_ok = true;
    memcpy(buf, k->data, CFS_BLOCK);
    if (k->bad != 0)
        ((uint8_t *)buf)[k->bad - 1] ^= 0x5A;
    bool ok = verify(buf, arg);
    note(6, ok);
    if (ok)
        return 0;
    if (k->bad != 0 && !k->lone) {
        /* The second copy is the good one. */
        memcpy(buf, k->data, CFS_BLOCK);
        ok = verify(buf, arg);
        note(7, ok);
        if (ok) {
            if (repaired != NULL)
                *repaired = true;
            return 0;
        }
    }
    return -EIO;
}

/* ---- the key table: one pool's key ---- */

static uint8_t g_key_uuid[16];
static uint8_t g_key[32];
static bool g_key_loaded;

int cfs_keytab_mac(const uint8_t *uuid, struct cfs_mac *mac)
{
    mix(uuid, 16);
    if (!g_key_loaded || memcmp(uuid, g_key_uuid, 16) != 0) {
        note(8, 0);
        return -ENOKEY;
    }
    note(8, 1);
    mac->on = true;
    memcpy(mac->key, g_key, sizeof(mac->key));
    return 0;
}

/* ---- the allocator of blocks: the filesystem's own bitmap, first free from a hint ---- */

static unsigned g_alloc_fail;

static int take(struct cfs *fs, uint64_t hint, uint64_t *out)
{
    if (g_alloc_fail != 0 && pick(g_alloc_fail) == 0)
        return -ENOSPC;
    uint64_t start = hint != 0 ? cfs_dva_lin(fs, hint) : (uint64_t)pick((unsigned)fs->nblocks);
    if (start == CFS_DVA_NONE)
        start = 0;
    for (uint64_t k = 0; k < fs->nblocks; k++) {
        uint64_t lin = (start + k) % fs->nblocks;
        unsigned v = (unsigned)(lin / NB);
        uint64_t b = lin % NB;
        if (b < fs->mem[v].first_usable || cfs_bit_test(fs->bitmap, lin))
            continue;
        cfs_bit_set(fs->bitmap, lin);
        fs->bitmap_dirty[lin / CFS_BITS_PER_BITMAP] = 1;
        fs->free_blocks--;
        fs->mem[v].free_blocks--;
        *out = CFS_DVA(v, b);
        return 0;
    }
    return -ENOSPC;
}

int cfs_alloc_run(struct cfs *fs, enum cfs_alloc_class cls, uint64_t hint, uint32_t want, uint64_t *start,
                  uint64_t *got)
{
    note(9, cls);
    note(10, hint);
    note(11, want);
    int rc = take(fs, hint, start);
    *got = rc == 0 ? 1 : 0;
    note(12, rc == 0 ? *start : (uint64_t)(int64_t)rc);
    return rc;
}

int cfs_alloc_block(struct cfs *fs, uint64_t *out)
{
    note(13, 0);
    int rc = take(fs, 0, out);
    note(14, rc == 0 ? *out : (uint64_t)(int64_t)rc);
    return rc;
}

void cfs_free_block_deferred(struct cfs *fs, uint64_t blk)
{
    (void)fs;
    note(15, blk);
}

void cfs_free_block_exempt(struct cfs *fs, uint64_t blk)
{
    (void)fs;
    note(16, blk);
}

void cfs_note_dirty(struct cfs *fs)
{
    note(17, fs->nr_dirty);
}

/* ---- building a filesystem ---- */

static uint8_t g_uuid[16];

static void make_block(struct cfs *fs, uint64_t lin)
{
    struct block *k = &g_disk[lin];
    k->made = true;
    memset(k->data, 0, CFS_BLOCK);
    for (unsigned i = 0; i < 256; i += 8) {
        uint64_t w = next();
        memcpy(k->data + i, &w, 8);
    }
    uint32_t kind = 1 + pick(18);
    uint64_t dva = CFS_DVA(lin / NB, lin % NB);
    cfs_mhdr_seal_raw(&fs->mac, k->data, kind, dva, fs->gen - 1 - pick(3));
    k->bad = 0;
    k->unreadable = false;
    k->lone = false;
    unsigned f = pick(10);
    if (g_faults && f == 0) {
        /* One field wrong in the first copy: the magic, the kind, the generation, the address, the seal, the body. */
        static const uint8_t at[] = { 1, 5, 9, 17, 25, 29, 33, 200 };
        k->bad = at[pick(sizeof(at))];
        k->lone = pick(3) == 0;
    } else if (g_faults && f == 1) {
        k->unreadable = true;
    } else if (g_faults && f == 2) {
        /* Sealed as another block's, or with an algorithm this build does not have, in every copy. */
        unsigned how = pick(3);
        if (how == 0)
            ((struct cfs_mhdr *)k->data)->blkno ^= 1;
        else if (how == 1)
            ((struct cfs_mhdr *)k->data)->csum_algo = 7;
        else
            ((struct cfs_mhdr *)k->data)->magic ^= 0x100;
    }
}

static void build(struct cfs *fs)
{
    unsigned f = pick(4);
    g_faults = f != 0;
    unsigned nm = pick(3);
    g_nmem = nm == 0 ? 2 : 1;
    fs->nmembers = g_nmem;
    fs->nblocks = (uint64_t)g_nmem * NB;
    fs->free_blocks = 0;
    for (unsigned v = 0; v < g_nmem; v++) {
        fs->mem[v].nblocks = NB;
        fs->mem[v].first_usable = v == 0 ? 2 : 1;
        fs->mem[v].base = (uint64_t)v * NB;
        fs->mem[v].free_blocks = 0;
    }
    memset(fs->bitmap, 0, (NMEM * NB + 7) / 8);
    memset(fs->bitmap_dirty, 0, 4);
    for (uint64_t lin = 0; lin < fs->nblocks; lin++) {
        unsigned v = (unsigned)(lin / NB);
        uint64_t b = lin % NB;
        if (b < fs->mem[v].first_usable)
            continue;
        if (pick(3) == 0) {
            cfs_bit_set(fs->bitmap, lin);
        } else {
            fs->free_blocks++;
            fs->mem[v].free_blocks++;
        }
    }
    fs->gen = 40 + pick(10);
    for (unsigned i = 0; i < 16; i++)
        g_uuid[i] = (uint8_t)next();
    unsigned m = pick(3);
    fs->mac.on = m == 0;
    for (unsigned i = 0; i < 32; i++)
        fs->mac.key[i] = (uint8_t)next();
    memcpy(g_key, fs->mac.key, 32);
    memcpy(g_key_uuid, g_uuid, 16);
    unsigned kl = pick(4);
    g_key_loaded = kl != 0;
    unsigned rf = pick(6);
    g_read_fail = g_faults && rf == 0 ? 8 : 0;
    unsigned af = pick(5);
    g_alloc_fail = g_faults && af == 0 ? 4 : 0;
    for (unsigned i = 0; i < NMEM * NB; i++)
        g_disk[i].made = false;
    g_fs = fs;
    list_init(&fs->bufs);
    fs->nr_bufs = 0;
    fs->nr_dirty = 0;
    note(30, g_nmem);
}

/* The cache emptied at the end of a round, every buffer released first. */
static void teardown(struct cfs *fs)
{
    unsigned n = 0;
    while (!list_empty(&fs->bufs)) {
        struct cfs_buf *b = list_entry(fs->bufs.next, struct cfs_buf, link);
        EXPECT(b->refs == 0);
        note(31, b->blkno);
        note(32, b->dirty);
        mix(b->data, CFS_BLOCK);
        list_remove(&b->link);
        kfree(b->data);
        kfree(b);
        n++;
    }
    EXPECT(n == fs->nr_bufs);
    note(33, fs->nr_dirty);
}

/* ---- what the operations are given ---- */

#define HELD 24u
static struct cfs_buf *g_held[HELD];
static unsigned g_nheld;

static void hold(struct cfs_buf *b)
{
    if (g_nheld < HELD)
        g_held[g_nheld++] = b;
    else
        cfs_buf_put(NULL, b);
}

static uint64_t some_dva(void)
{
    unsigned k = pick(20);
    if (k == 0)
        return CFS_DVA(5, 3);       /* no member's */
    if (k == 1)
        return CFS_DVA(0, NB + 2);  /* past a member */
    if (k == 2)
        return CFS_DVA(0, 0);       /* a superblock slot, below first_usable */
    return CFS_DVA(pick(g_nmem), 2 + pick(NB - 2));
}

static void mix_buf(const struct cfs_buf *b)
{
    note(40, b->blkno);
    note(41, b->dirty);
    note(42, b->refs);
    mix(b->data, CFS_MHDR_SIZE);
    mix(b->data + CFS_BLOCK - 64, 64);
}

static void op_super(struct cfs *fs)
{
    uint8_t *block = calloc(1, CFS_BLOCK);
    EXPECT(block != NULL);
    struct cfs_super *sb = (struct cfs_super *)block;
    memcpy(sb->magic, CFS_MAGIC, 8);
    sb->version = 1 + pick(16);
    memcpy(sb->uuid, pick(4) == 0 ? (const uint8_t *)"0123456789abcdef" : g_uuid, 16);
    sb->key_root = pick(2) == 0 ? 0 : CFS_DVA(0, 9);
    sb->generation = pick(1000);
    for (unsigned i = 200; i < CFS_BLOCK; i += 97)
        block[i] = (uint8_t)next();
    struct cfs_mac seal = fs->mac;
    seal.on = true;
    cfs_super_seal(&seal, block);
    unsigned d = pick(6);
    if (d == 0)
        block[300] ^= 1;                 /* damaged after the seal */
    else if (d == 1)
        sb->magic[0] ^= 1;               /* not a cosmofs superblock */
    struct cfs_mac got;
    memset(&got, 0xEE, sizeof(got));
    unsigned withmac = pick(2);
    int rc = cfs_super_verify(block, withmac == 0 ? NULL : &got);
    note(50, (uint64_t)(int64_t)rc);
    if (withmac != 0 && rc == 0) {
        note(51, got.on);
        if (got.on)
            mix(got.key, sizeof(got.key));   /* a CRC'd pool's mac is `on` false and its key unset */
    }
    note(52, cfs_super_ok(block));
    free(block);
}

static void op_header(struct cfs *fs)
{
    uint8_t *block = malloc(CFS_BLOCK);
    EXPECT(block != NULL);
    for (unsigned i = 0; i < CFS_BLOCK; i += 8) {
        uint64_t w = next();
        memcpy(block + i, &w, 8);
    }
    uint32_t kind = 1 + pick(18);
    uint64_t dva = some_dva();
    struct cfs_mac m = fs->mac;
    unsigned on = pick(2);
    m.on = on == 0;
    unsigned nul = pick(8);
    const struct cfs_mac *mp = nul == 0 ? NULL : &m;
    cfs_mhdr_seal_raw(mp, block, kind, dva, pick(99));
    unsigned d = pick(8);
    if (d < 6)
        block[(size_t[]){ 0, 4, 8, 16, 24, 28 }[d]] ^= 0x10;
    uint32_t want = pick(4) == 0 ? 0 : (pick(3) == 0 ? kind + 1 : kind);
    uint64_t wdva = pick(6) == 0 ? dva + 1 : dva;
    note(53, cfs_mhdr_fault_of(mp, block, wdva, want));
    note(54, cfs_mhdr_ok(mp, block, wdva, want));
    uint8_t tag[CFS_MAC_SIZE];
    cfs_mac_tag(&m, CFS_MAC_HDR, block, 24, tag);
    mix(tag, sizeof(tag));
    cfs_mac_seal(&m, CFS_MAC_LABEL, block, 64);
    note(55, cfs_mac_ok(&m, CFS_MAC_LABEL, block, 64));
    block[64 + pick(8)] ^= 1;
    note(56, cfs_mac_ok(&m, CFS_MAC_LABEL, block, 64));
    cfs_mhdr_seal(fs, block, kind, dva);
    mix(block, 64);
    free(block);
}

static void op(struct cfs *fs)
{
    unsigned which = pick(16);
    unsigned burst = pick(5);
    if (which == 14 && burst != 0)
        which = 0;   /* a burst that fills the cache, now and then */
    note(60, which);
    unsigned fi = pick(14);
    g_fail_in = fi == 0 ? (int)pick(3) : -1;
    int rc = 0;
    switch (which) {
    case 0:
    case 1:
    case 2: {
        /* A read, held or put. */
        uint64_t dva = some_dva();
        uint32_t kind = pick(3) == 0 ? 0 : ((struct cfs_mhdr *)g_disk[cfs_dva_valid(fs, dva) ? disk_lin(dva) : 0].data)->kind;
        if (pick(8) == 0)
            kind++;
        struct cfs_buf *b = NULL;
        rc = cfs_buf_get(fs, dva, kind, &b);
        if (rc == 0) {
            mix_buf(b);
            unsigned keep = pick(3);
            if (keep == 0)
                hold(b);
            else
                cfs_buf_put(fs, b);
        }
        break;
    }
    case 3: {
        struct cfs_buf *b = NULL;
        rc = cfs_buf_new(fs, 1 + pick(18), &b);
        if (rc == 0) {
            mix_buf(b);
            hold(b);
        }
        break;
    }
    case 4:
    case 5: {
        /* Copy-on-write of a held buffer, deferred or exempt, from a reservation or not. */
        if (g_nheld == 0)
            break;
        unsigned i = pick(g_nheld);
        uint64_t slot = 0, resv[3];
        struct cfs_res res = { .blk = resv, .n = 0, .used = 0 };
        unsigned nres = pick(4);
        for (unsigned k = 0; k < nres; k++)
            if (take(fs, 0, &resv[res.n]) == 0)
                res.n++;
        unsigned how = pick(3);
        if (how == 0)
            rc = cfs_buf_cow(fs, &g_held[i], &slot);
        else
            rc = cfs_buf_cow_exempt(fs, &g_held[i], &slot, how == 1 ? &res : NULL);
        note(61, slot);
        note(62, res.used);
        mix_buf(g_held[i]);
        break;
    }
    case 6: {
        /* A set-aside, some of it used, the rest given back. */
        struct cfs_prep *p = calloc(1, sizeof(*p));
        EXPECT(p != NULL);
        unsigned n = pick(5) == 0 ? CFS_PREP_MAX + 1 : pick(5);
        rc = cfs_prep_meta(fs, p, n);
        note(63, p->n);
        if (rc == 0) {
            unsigned use = pick(p->n + 1);
            for (unsigned k = 0; k < use; k++) {
                unsigned how = pick(2);
                if (how == 0 || g_nheld == 0) {
                    struct cfs_buf *b = cfs_buf_new_prep(fs, 1 + pick(18), p);
                    mix_buf(b);
                    hold(b);
                } else {
                    uint64_t slot = 0;
                    unsigned i = pick(g_nheld);
                    cfs_buf_cow_prep(fs, &g_held[i], &slot, p);
                    note(64, slot);
                    mix_buf(g_held[i]);
                }
            }
            note(65, p->used);
            cfs_prep_release(fs, p);
        }
        free(p);
        break;
    }
    case 7: {
        uint64_t blk;
        if (take(fs, 0, &blk) != 0)
            break;
        struct cfs_buf *b = NULL;
        rc = cfs_buf_new_at(fs, 1 + pick(18), blk, &b);
        if (rc == 0) {
            mix_buf(b);
            hold(b);
        }
        break;
    }
    case 8:
        if (g_nheld > 0) {
            unsigned i = pick(g_nheld);
            unsigned how = pick(3);
            if (how == 0)
                cfs_buf_mark_dirty(fs, g_held[i]);
            else if (how == 1)
                cfs_buf_mark_clean(fs, g_held[i]);
            note(66, cfs_buf_is_dirty(fs, g_held[i]->blkno));
            note(67, cfs_buf_hdr(g_held[i])->kind);
        }
        note(68, cfs_buf_is_dirty(fs, some_dva()));
        note(69, cfs_buf_find(fs, some_dva()) != NULL);
        break;
    case 9:
        if (g_nheld > 0) {
            /* Put one back. */
            unsigned i = pick(g_nheld);
            cfs_buf_put(fs, g_held[i]);
            g_held[i] = g_held[--g_nheld];
        }
        break;
    case 10: {
        uint8_t *buf = malloc(CFS_BLOCK);
        EXPECT(buf != NULL);
        uint64_t dva = some_dva();
        unsigned w = pick(2);
        if (w == 0) {
            rc = cfs_data_read(fs, dva, buf);
            if (rc == 0)
                mix(buf, 64);
        } else {
            memset(buf, (int)pick(256), CFS_BLOCK);
            rc = cfs_data_write(fs, dva, buf);
        }
        free(buf);
        break;
    }
    case 11:
        op_super(fs);
        break;
    case 12:
        op_header(fs);
        break;
    case 13: {
        uint64_t r[2] = { CFS_DVA(0, 50), CFS_DVA(0, 51) };
        struct cfs_res res = { .blk = r, .n = pick(3), .used = 0 };
        for (unsigned k = 0; k < 4; k++) {
            unsigned how = pick(3);
            if (how == 0)
                cfs_res_untake(&res);
            else
                note(70, cfs_res_take(&res));
        }
        note(71, cfs_res_take(NULL));
        cfs_res_untake(NULL);
        note(72, res.used);
        break;
    }
    case 14: {
        /* A burst of reads: the cache fills past its size and evicts. */
        unsigned n = 60 + pick(30);
        for (unsigned k = 0; k < n; k++) {
            struct cfs_buf *b = NULL;
            uint64_t dva = CFS_DVA(pick(g_nmem), 2 + pick(NB - 2));
            if (cfs_buf_get(fs, dva, 0, &b) == 0)
                cfs_buf_put(fs, b);
        }
        break;
    }
    case 15: {
        struct cfs_buf *b = cfs_buf_alloc(fs, some_dva());
        if (b != NULL) {
            mix_buf(b);
            unsigned keep = pick(2);
            if (keep == 0)
                hold(b);   /* a buffer at any address: its copy has no member to stay on */
            else
                cfs_buf_put(fs, b);
        }
        break;
    }
    }
    g_fail_in = -1;
    note(80, (uint64_t)(int64_t)rc);
    note(81, fs->nr_bufs);
    note(82, fs->nr_dirty);
    note(83, fs->free_blocks);
}

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    harness_klog_hook = on_klog;
    harness_klog_min = KLOG_ERROR + 1;
    struct cfs *fs = calloc(1, sizeof(*fs));
    EXPECT(fs != NULL);
    fs->mem = calloc(NMEM, sizeof(*fs->mem));
    fs->bitmap = calloc(1, (NMEM * NB + 7) / 8);
    fs->bitmap_dirty = calloc(1, 4);
    EXPECT(fs->mem != NULL && fs->bitmap != NULL && fs->bitmap_dirty != NULL);
    for (unsigned round = 0; round < ROUNDS; round++) {
        build(fs);
        g_nheld = 0;
        unsigned ops = 4 + pick(20);
        for (unsigned k = 0; k < ops; k++)
            op(fs);
        while (g_nheld > 0)
            cfs_buf_put(fs, g_held[--g_nheld]);
        teardown(fs);
        mix(fs->bitmap, (NMEM * NB + 7) / 8);
        mix(fs->bitmap_dirty, 4);
        for (unsigned v = 0; v < g_nmem; v++)
            note(90, fs->mem[v].free_blocks);
        EXPECT(g_live == 0);
    }
    free(fs->bitmap_dirty);
    free(fs->bitmap);
    free(fs->mem);
    free(fs);
    harness_klog_hook = NULL;
    harness_klog_min = KLOG_WARN;
    printf("  buf digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, ROUNDS);
    EXPECT(g_digest == BUF_DIGEST);
}

int main(void)
{
    static const struct host_test tests[] = {
        { "buf_digest", test_digest },
    };
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
