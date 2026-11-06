/*
 * test_cfscrypt.c - cosmofs's keys (kernel-services/filesystem/cosmofs/cosmofs_crypt.cee) on the host: roadmap 2.0
 * SA7. A digest recorded from the C over seeded rounds, with the real ChaCha20-Poly1305, SHA-512 and BLAKE2b
 * (kernel/security) and a pool, buffers, a transaction and a random source the test owns: file keys and nonces, the
 * seeding wait, version-1 key blocks written, unwrapped with right and wrong keys, loaded and rotated, the MAC key,
 * version-2 key blocks, the key table (installed, taken, held, released, unloaded, full), and every /dev/fsctl key
 * operation on a device and on a mount, with every refusal and failure. Every return, every block written, every key
 * handed out, every record filled and every logged line are in the digest; nothing is left allocated.
 */

#include "harness.h"

#include <kernel/blake2b.h>
#include <kernel/blk.h>
#include <kernel/chacha20.h>
#include <kernel/cosmofs.h>
#include <kernel/crypto.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/mutex.h>
#include <kernel/random.h>
#include <kernel/storage.h>
#include <kernel/vfs.h>

#include <uapi/cosmo/fsctl.h>

#include "cosmofs_internal.h"   /* before the C library's headers, as test_cfsparse.c: its signal.h is the kernel's */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The values the CeeSafe port writes as its own constants, and the offsets and stack slots it relies on. */
_Static_assert(ENOENT == 2 && EIO == 5 && ENOMEM == 12 && EINTR == 4 && EAGAIN == 11 && EBUSY == 16 && EXDEV == 18 &&
                   EINVAL == 22 && ENOSPC == 28 && EROFS == 30 && ENOKEY == 126 && EKEYREJECTED == 129,
               "cosmofs_crypt's errno.h values");
_Static_assert(KMEM_ZERO == 1 && RANDOM_KEYGEN_WAIT_NS == 5000000000ull && MOUNT_RDONLY == 1 && COSMOFS_KEY_INFO == 0 &&
                   COSMOFS_KEY_LOAD == 1 && COSMOFS_KEY_UNLOAD == 2 && COSMOFS_KEY_SLOT == 3 &&
                   COSMO_FSCTL_KEY_F_ENCRYPTED == 1 && COSMO_FSCTL_KEY_F_UNLOCKED == 2 && COSMO_FSCTL_KEY_F_MAC == 4,
               "cosmofs_crypt's kmalloc.h, random.h, vfs.h, cosmofs.h and fsctl.h values");
_Static_assert(CFS_BLOCK == 4096 && CFS_MHDR_SIZE == 32 && CFS_MHDR_MAGIC == 0x4d534643u && CFS_KIND_KEYS == 12 &&
                   CFS_KDF_SHA512 == 1 && CFS_KEY_SLOTS == 8 && CFS_KEYSLOT_EMPTY == 0 && CFS_DVA_VDEV_SHIFT == 56 &&
                   CHACHA20_KEY_SIZE == 32 && CHACHA20_NONCE_SIZE == 12 && SHA512_DIGEST_SIZE == 64 &&
                   CFS_LABEL_MAX == 16 && sizeof(((struct cfs *)0)->pool_name) == 17,
               "cosmofs_crypt's cosmofs_format.h and cipher values");
_Static_assert(offsetof(struct cfs_mhdr, magic) == 0 && offsetof(struct cfs_mhdr, kind) == 4 &&
                   offsetof(struct cfs_mhdr, blkno) == 16 && offsetof(struct cfs_keys2, slot) == 40 &&
                   offsetof(struct cosmo_fsctl_keyinfo, slot) == 56 && sizeof(enum cosmofs_key_op) == 4,
               "cosmofs_crypt.cee reads a header, a key block's slots and a record's slots at these offsets");
/* cosmofs_crypt.cee keeps these in stack slots of words: a SHA-512 state 26, a BLAKE2b state 28, a superblock 21, a
 * MAC key 5. */
_Static_assert(sizeof(struct sha512_ctx) <= 26 * 8 && sizeof(struct blake2b) <= 28 * 8 &&
                   sizeof(struct cfs_super) <= 21 * 8 && sizeof(struct cfs_mac) <= 5 * 8,
               "cosmofs_crypt.cee's stack slots");

/* The digest of the C (cosmofs_crypt.c, 2026-11-06), which the CeeSafe port gives. */
#define CRYPT_DIGEST 0x98e1087b49e6224cull
#define ROUNDS 4000u
#define SEED 0xC4F7C4F72026B018ull

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

static void fill(void *out, size_t n)
{
    uint8_t *b = out;
    for (size_t i = 0; i < n; i++)
        b[i] = (uint8_t)next();
}

static void on_klog(int level, const char *line)
{
    note(1, (uint64_t)level);
    mix(line, strlen(line));
}

/* ---- the allocator: malloc, counted; an allocation fails when g_fail_in counts down to it ---- */

static int g_fail_in = -1;
static long g_live;

void *kmalloc(size_t size, unsigned flags)
{
    if (g_fail_in >= 0 && g_fail_in-- == 0) {
        note(2, size);
        return NULL;
    }
    void *p = (flags & KMEM_ZERO) ? calloc(1, size) : malloc(size);
    if (p != NULL)
        g_live++;
    return p;
}

void kfree(void *p)
{
    if (p != NULL)
        g_live--;
    free(p);
}

/* ---- the random source and the seeding wait ---- */

void random_get_bytes(void *buf, size_t len)
{
    note(3, len);
    fill(buf, len);
}

static unsigned g_unseeded_pct;

int random_wait_ready(uint64_t timeout_ns)
{
    note(4, timeout_ns);
    unsigned r = pick(100);
    if (r >= g_unseeded_pct)
        return 0;
    unsigned how = pick(3);
    return how == 0 ? -EINTR : how == 1 ? -ETIMEDOUT : -EAGAIN;
}

/* ---- the pool: one, of two members of NB blocks ---- */

#define NB 16u

static uint8_t g_img[2][NB][CFS_BLOCK];
static unsigned g_fail_pct;   /* of the pool's, the buffers' and the transaction's operations that fail */
static int g_open_rc;
static long g_pools;

static int draw_rc(void)
{
    unsigned r = pick(100);
    if (r >= g_fail_pct)
        return 0;
    return pick(2) == 0 ? -EIO : -ENOMEM;
}

static uint8_t *block_at(uint64_t dva)
{
    unsigned v = CFS_DVA_VDEV(dva);
    uint64_t b = CFS_DVA_BLK(dva);
    EXPECT(v < 2 && b < NB);
    return g_img[v][b];
}

int pool_open(struct blkdev *bd, struct spool **out)
{
    note(5, bd->name[0]);
    if (g_open_rc)
        return g_open_rc;
    *out = calloc(1, 8);
    g_pools++;
    return 0;
}

void pool_close(struct spool *p)
{
    note(6, 0);
    g_pools--;
    free(p);
}

int pool_read(struct spool *p, uint64_t dva, void *buf)
{
    (void)p;
    note(7, dva);
    int rc = draw_rc();
    if (rc == 0)
        memcpy(buf, block_at(dva), CFS_BLOCK);
    return rc;
}

int pool_write(struct spool *p, uint64_t dva, const void *buf)
{
    (void)p;
    note(8, dva);
    mix(buf, CFS_BLOCK);
    int rc = draw_rc();
    if (rc == 0)
        memcpy(block_at(dva), buf, CFS_BLOCK);
    return rc;
}

/* ---- the headers, the superblock and the slots: a MAC is the key's first 8 bytes at the block's end ---- */

#define TAG_AT (CFS_BLOCK - 8)
#define KEY_BLK 3u   /* the key block's, past both superblock slots */

void cfs_mhdr_seal_raw(const struct cfs_mac *mac, void *block, uint32_t kind, uint64_t dva, uint64_t generation)
{
    struct cfs_mhdr *h = block;
    note(9, mac != NULL);
    h->magic = CFS_MHDR_MAGIC;
    h->kind = kind;
    h->blkno = dva;
    h->generation = generation;
    if (mac != NULL)
        memcpy((uint8_t *)block + TAG_AT, mac->key, 8);
}

bool cfs_mhdr_ok(const struct cfs_mac *mac, const void *block, uint64_t dva, uint32_t kind)
{
    const struct cfs_mhdr *h = block;
    note(10, mac->on);
    if (h->magic != CFS_MHDR_MAGIC || h->kind != kind || h->blkno != dva)
        return false;
    return !mac->on || memcmp((const uint8_t *)block + TAG_AT, mac->key, 8) == 0;
}

bool cfs_mac_ok(const struct cfs_mac *m, char domain, const uint8_t *block, size_t off)
{
    note(11, (uint64_t)domain);
    note(12, off);
    return m->on && memcmp(block + TAG_AT, m->key, 8) == 0;
}

static int g_peek_rc;

int cfs_super_peek(struct spool *p, struct cfs_super *out)
{
    (void)p;
    note(13, (uint64_t)(int64_t)g_peek_rc);
    if (g_peek_rc)
        return g_peek_rc;
    memcpy(out, g_img[0][0], sizeof(*out));
    return 0;
}

bool cfs_keyslot_valid(unsigned index, const struct cfs_keyslot *s)
{
    note(14, index);
    return s->kind <= 3 && s->lanes != 7;
}

/* ---- the mount's buffers, transaction and lock ---- */

static bool g_locked;
static long g_bufs;
static unsigned g_next_blk;

void mutex_lock(struct mutex *m)
{
    (void)m;
    EXPECT(!g_locked);
    g_locked = true;
}

void mutex_unlock(struct mutex *m)
{
    (void)m;
    EXPECT(g_locked);
    g_locked = false;
}

int cfs_buf_get(struct cfs *fs, uint64_t blkno, uint32_t kind, struct cfs_buf **out)
{
    (void)fs;
    EXPECT(g_locked || kind == CFS_KIND_KEYS);
    note(15, blkno);
    note(16, kind);
    int rc = draw_rc();
    if (rc)
        return rc;
    struct cfs_buf *b = calloc(1, sizeof(*b));
    b->data = malloc(CFS_BLOCK);
    memcpy(b->data, block_at(blkno), CFS_BLOCK);
    b->blkno = blkno;
    g_bufs++;
    *out = b;
    return 0;
}

int cfs_buf_cow(struct cfs *fs, struct cfs_buf **bp, uint64_t *parent_slot)
{
    (void)fs;
    note(17, (*bp)->blkno);
    int rc = draw_rc();
    if (rc)
        return rc;
    uint64_t moved = CFS_DVA(0, KEY_BLK + 1 + g_next_blk++ % (NB - KEY_BLK - 1));
    (*bp)->blkno = moved;
    *parent_slot = moved;
    note(18, moved);
    return 0;
}

void cfs_buf_mark_dirty(struct cfs *fs, struct cfs_buf *b)
{
    (void)fs;
    note(19, b->blkno);
    mix(b->data, CFS_BLOCK);
    memcpy(block_at(b->blkno), b->data, CFS_BLOCK);
}

void cfs_buf_put(struct cfs *fs, struct cfs_buf *b)
{
    (void)fs;
    note(20, b->blkno);
    g_bufs--;
    free(b->data);
    free(b);
}

int cfs_commit(struct cfs *fs)
{
    (void)fs;
    int rc = draw_rc();
    note(21, (uint64_t)(int64_t)rc);
    return rc;
}

/* ---- the worlds ---- */

static uint8_t g_master[32], g_user[40], g_uuid[3][16];
static size_t g_user_len;
static struct cfs_keyslot g_slots[CFS_KEY_SLOTS];

/* A pool: a superblock in each slot (version 7 to 14), and its key block, written by the code under test. */
static void build(struct cfs *fs)
{
    /* Last round's pools let go of, now and then: their keys are not this round's. */
    unsigned forget = pick(2);
    for (unsigned u = 0; u < 3 && forget == 0; u++) {
        for (unsigned k = 0; k < 64; k++)
            cfs_keytab_release(g_uuid[u]);
        note(38, (uint64_t)(int64_t)cfs_keytab_unload(g_uuid[u]));
    }
    memset(g_img, 0, sizeof(g_img));
    g_next_blk = 0;
    fill(g_master, sizeof(g_master));
    g_user_len = 1 + pick(sizeof(g_user));
    fill(g_user, g_user_len);
    struct cfs_super sb;
    memset(&sb, 0, sizeof(sb));
    memcpy(sb.magic, CFS_MAGIC, 8);
    unsigned v = pick(4);
    unsigned old = pick(8);
    sb.version = old == 0 ? 4 : v == 0 ? 7 : v == 1 ? 12 + pick(2) : 14;
    sb.generation = 1 + pick(100);
    memcpy(sb.uuid, g_uuid[pick(3)], 16);
    snprintf(sb.label, sizeof(sb.label), "pool%u", pick(10));
    unsigned enc = pick(8);
    unsigned other = pick(10);
    sb.key_root = enc == 0 ? 0 : CFS_DVA(other == 0 ? 1 : 0, KEY_BLK);
    struct cfs_mac mac;
    cfs_mac_derive(g_master, &mac);
    mix(&mac, sizeof(mac));
    for (unsigned i = 0; i < CFS_KEY_SLOTS; i++) {
        fill(&g_slots[i], sizeof(g_slots[i]));
        unsigned k = pick(4);
        g_slots[i].kind = k == 0 ? CFS_KEYSLOT_EMPTY : k;
        unsigned lanes = pick(10);
        g_slots[i].lanes = lanes == 0 ? 7 : 1;
    }
    unsigned lone = pick(4);
    if (lone == 0)
        for (unsigned i = 1; i < CFS_KEY_SLOTS; i++)
            g_slots[i].kind = CFS_KEYSLOT_EMPTY;   /* one way in: slot 0's */
    if (sb.key_root != 0) {
        g_fail_pct = 0;
        int rc = sb.version >= 14 ? cfs_keys2_write(NULL, &mac, sb.key_root, sb.generation, sb.uuid, g_slots)
                                  : cfs_keys_write(NULL, sb.key_root, sb.generation, g_master, g_user, g_user_len);
        note(22, (uint64_t)(int64_t)rc);
    }
    /* Both slots: the second older, or another pool's, or not sealed by this key. */
    for (unsigned s = 0; s < 2; s++) {
        struct cfs_super c = sb;
        unsigned what = pick(6);
        if (s == 1 && what == 0)
            c.generation--;
        if (what == 1)
            memcpy(c.uuid, g_uuid[pick(3)], 16);
        if (what == 2)
            c.magic[0] ^= 1;
        if (what == 4)
            c.version = 13;   /* not a MAC pool's superblock */
        memcpy(g_img[0][s], &c, sizeof(c));
        if (what != 3)
            memcpy(g_img[0][s] + TAG_AT, mac.key, 8);
    }
    /* The key block damaged, now and then. */
    unsigned dmg = pick(16);
    if (sb.key_root != 0 && dmg < 6) {
        uint8_t *kb = block_at(sb.key_root);
        if (dmg >= 3)
            kb[dmg == 3 ? 0 : dmg == 4 ? 4 : 16] ^= 1;   /* the header's magic, kind, block number */
        else if (dmg == 0)
            kb[CFS_MHDR_SIZE] ^= 1;   /* the version */
        else if (dmg == 1)
            kb[CFS_MHDR_SIZE + 9] ^= 1;   /* the uuid (v2), the salt (v1) */
        else
            kb[TAG_AT] ^= 1;
    }
    /* The mount over it. */
    memset(&fs->sb, 0, sizeof(fs->sb));
    fs->sb = sb;
    fs->mac = sb.version >= 14 && sb.key_root != 0 ? mac : (struct cfs_mac){ 0 };
    memcpy(fs->master_key, g_master, 32);
    unsigned hk = pick(4);
    fs->have_key = hk != 0;
    unsigned ro = pick(10);
    fs->rdonly = ro == 0;
    snprintf(fs->pool_name, sizeof(fs->pool_name), "%s", sb.label);
}

static void dump_tab(void)
{
    for (unsigned u = 0; u < 3; u++) {
        note(30, cfs_keytab_has(g_uuid[u]));
        struct cfs_mac m;
        memset(&m, 0, sizeof(m));
        note(31, (uint64_t)(int64_t)cfs_keytab_mac(g_uuid[u], &m));
        mix(&m, sizeof(m));
    }
}

static const uint8_t *some_uuid(void)
{
    static const uint8_t zero[16];
    unsigned k = pick(8);
    return k == 0 ? zero : g_uuid[k % 3];
}

static void keytab_ops(void)
{
    unsigned what = pick(6);
    const uint8_t *u = some_uuid();
    uint8_t key[32];
    if (what == 0) {
        unsigned right = pick(3);
        if (right == 0)
            fill(key, 32);
        else
            memcpy(key, g_master, 32);
        /* Never the zero uuid: no lookup finds it, so its entry would stay for good (the kernel never installs one). */
        if (u[0] == 0 && memcmp(u, u + 1, 15) == 0)
            u = g_uuid[0];
        note(32, (uint64_t)(int64_t)cfs_keytab_install(u, key));
        unsigned again = pick(2);
        if (again == 0)
            note(37, (uint64_t)(int64_t)cfs_keytab_install(u, key));   /* the same key again: kept */
        unsigned hold = pick(2);
        note(39, (uint64_t)(int64_t)cfs_keytab_take(u, hold != 0, key));
    } else if (what == 1) {
        unsigned hold = pick(2);
        memset(key, 0, 32);
        note(33, (uint64_t)(int64_t)cfs_keytab_take(u, hold != 0, key));
        mix(key, 32);
    } else if (what == 2) {
        cfs_keytab_release(u);
    } else if (what == 3) {
        note(34, (uint64_t)(int64_t)cfs_keytab_unload(u));
    } else if (what == 4) {
        /* Fill the table with other pools until it refuses one, then empty it of them again. */
        uint8_t uu[9][16];
        for (unsigned i = 0; i < 9; i++) {
            fill(uu[i], 16);
            fill(key, 32);
            note(35, (uint64_t)(int64_t)cfs_keytab_install(uu[i], key));
        }
        for (unsigned i = 0; i < 9; i++)
            note(36, (uint64_t)(int64_t)cfs_keytab_unload(uu[i]));
    } else {
        dump_tab();
    }
}

static void key_ops(struct cfs *fs)
{
    unsigned what = pick(7);
    uint8_t out[64];
    memset(out, 0, sizeof(out));
    if (what == 0) {
        uint64_t ino = next();
        cfs_file_key(fs, ino, out);
        mix(out, 32);
    } else if (what == 1) {
        cfs_block_nonce(out);
        mix(out, CHACHA20_NONCE_SIZE);
    } else if (what == 2) {
        note(40, (uint64_t)(int64_t)cfs_need_seeded("do the thing"));
    } else if (what == 3) {
        /* Unwrap a version-1 block: the right key, another, a bad version or kdf. */
        struct cfs_keys k;
        memcpy(&k, block_at(CFS_DVA(0, KEY_BLK)) + CFS_MHDR_SIZE, sizeof(k));
        unsigned how = pick(5);
        if (how == 0)
            k.version = 3;
        else if (how == 1)
            k.kdf = 9;
        uint8_t bad[40];
        fill(bad, sizeof(bad));
        unsigned wrong = pick(3);
        int rc = cfs_keys_unwrap(&k, wrong == 0 ? bad : g_user, g_user_len, out);
        note(41, (uint64_t)(int64_t)rc);
        mix(out, 32);
    } else if (what == 4) {
        unsigned wrong = pick(4);
        uint8_t bad[40];
        fill(bad, sizeof(bad));
        unsigned fi = pick(12);
        g_fail_in = fi == 0 ? 0 : -1;
        int rc = cfs_keys_load(fs, wrong == 0 ? bad : g_user, g_user_len);
        g_fail_in = -1;
        note(42, (uint64_t)(int64_t)rc);
        note(43, fs->have_key);
        mix(fs->master_key, 32);
    } else if (what == 5) {
        uint8_t nk[24];
        fill(nk, sizeof(nk));
        g_locked = true;
        int rc = cfs_keys_rotate(fs, nk, 1 + pick(sizeof(nk)));
        g_locked = false;
        note(44, (uint64_t)(int64_t)rc);
        note(45, fs->sb.key_root);
        if (rc == 0) {
            memcpy(g_user, nk, sizeof(nk));
            g_user_len = sizeof(nk) < g_user_len ? sizeof(nk) : g_user_len;
        }
    } else {
        unsigned fi = pick(6);
        g_fail_in = fi == 0 ? 0 : -1;
        uint8_t uu[16];
        fill(uu, 16);
        int rc = fs->sb.version >= 14 ? cfs_keys2_write(NULL, &fs->mac, CFS_DVA(0, KEY_BLK), fs->sb.generation, uu, g_slots)
                                              : cfs_keys_write(NULL, CFS_DVA(0, KEY_BLK), fs->sb.generation, g_master, g_user,
                                                       g_user_len);
        g_fail_in = -1;
        note(46, (uint64_t)(int64_t)rc);
    }
}

static void keyctl_ops(struct cfs *fs, struct blkdev *bd, struct mount *mnt)
{
    struct cosmo_fsctl_keyinfo info;
    memset(&info, 0xEE, sizeof(info));
    struct cosmo_keyslot rec;
    fill(&rec, sizeof(rec));
    unsigned rk = pick(4);
    rec.kind = rk == 0 ? CFS_KEYSLOT_EMPTY : rk;
    unsigned rl = pick(8);
    rec.lanes = rl == 0 ? 7 : 1;
    uint8_t key[32];
    unsigned right = pick(3);
    if (right == 0)
        fill(key, 32);
    else
        memcpy(key, g_master, 32);
    struct cosmofs_key_req req;
    memset(&req, 0, sizeof(req));
    req.op = (enum cosmofs_key_op)pick(5);
    unsigned sl = pick(10);
    req.slot = sl < 3 ? 0 : sl;
    req.key = key;
    req.rec = &rec;
    req.info = &info;
    unsigned where = pick(10);
    unsigned po = pick(15);
    g_open_rc = po == 0 ? -ENODEV : 0;
    unsigned pk = pick(15);
    g_peek_rc = pk == 0 ? -ENOENT : pk == 1 ? -EIO : 0;
    unsigned fi = pick(15);
    g_fail_in = fi == 0 ? 0 : -1;
    unsigned mf = pick(8);
    mnt->flags = mf == 0 ? MOUNT_RDONLY : 0;
    unsigned nopriv = pick(20);
    mnt->fs_priv = nopriv == 0 ? NULL : fs;
    int rc;
    if (where == 0)
        rc = cosmofs_keyctl(NULL, NULL, &req);
    else if (where == 1)
        rc = cosmofs_keyctl(bd, mnt, &req);
    else if (where < 6)
        rc = cosmofs_keyctl(bd, NULL, &req);
    else
        rc = cosmofs_keyctl(NULL, mnt, &req);
    g_fail_in = -1;
    EXPECT(!g_locked && g_bufs == 0 && g_pools == 0);
    note(50, (uint64_t)(int64_t)rc);
    mix(&info, sizeof(info));
    note(51, fs->sb.key_root);
}

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    harness_klog_hook = on_klog;
    harness_klog_min = KLOG_ERROR + 1;   /* the refusals are expected, by the hundred */
    struct cfs *fs = calloc(1, sizeof(*fs));
    EXPECT(fs != NULL);
    struct mount mnt;
    memset(&mnt, 0, sizeof(mnt));
    struct blkdev *bd = calloc(1, sizeof(*bd));
    EXPECT(bd != NULL);
    snprintf(bd->name, sizeof(bd->name), "vblk0");
    for (unsigned u = 0; u < 3; u++)
        fill(g_uuid[u], 16);
    for (unsigned round = 0; round < ROUNDS; round++) {
        build(fs);
        unsigned f = pick(3);
        g_fail_pct = f == 0 ? 0 : f == 1 ? 5 : 25;
        unsigned us = pick(4);
        g_unseeded_pct = us == 0 ? 30 : 0;
        unsigned ops = 1 + pick(10);
        for (unsigned k = 0; k < ops; k++) {
            unsigned what = pick(3);
            if (what == 0)
                keytab_ops();
            else if (what == 1)
                key_ops(fs);
            else
                keyctl_ops(fs, bd, &mnt);
            EXPECT(g_live == 0);
        }
    }
    dump_tab();
    free(bd);
    free(fs);
    EXPECT(g_live == 0);
    harness_klog_hook = NULL;
    harness_klog_min = KLOG_WARN;
    printf("  crypt digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, ROUNDS);
    EXPECT(g_digest == CRYPT_DIGEST);
}

int main(void)
{
    static const struct host_test tests[] = {
        { "crypt_digest", test_digest },
    };
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
