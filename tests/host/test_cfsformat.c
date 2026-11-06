/*
 * test_cfsformat.c - cosmofs's format (kernel-services/filesystem/cosmofs/cosmofs_format.cee) on the host: roadmap 2.0
 * SA7. A digest recorded from the C over formats the test asks for, with the real key-slot check and CRC32C
 * (cfsparse): one device or several members, mirrored or not, devices too small, too large or too many, every format
 * version, labels absent, empty, full and too long, pools in the clear, encrypted with a wrapped key and with the
 * slots user space sealed (valid, empty, malformed, no uuid); and the test hooks that write an older format or poison
 * the free record and the free_root word of a pool the test keeps -- while opening the pool, adding its devices,
 * every write, the flush, kmalloc and the key and label writers fail now and then. Every block the format writes,
 * every call, every argument, every return and every logged line are in the digest; nothing is left allocated and no
 * pool is left open.
 */

#include "harness.h"

#include <kernel/blk.h>
#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/storage.h>
#include <uapi/cosmo/keyslot.h>

#include "cosmofs_internal.h"   /* before the C library's headers, as test_cfsparse.c: its signal.h is the kernel's */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The values the CeeSafe port writes as its own constants. */
_Static_assert(ENOENT == 2 && ENOMEM == 12 && EINVAL == 22 && KMEM_ZERO == 1 && BIO_PREFLUSH == 1 && BIO_FUA == 2,
               "cosmofs_format's errno.h, kmalloc.h and blk.h values");
_Static_assert(CFS_DVA_VDEV_SHIFT == 56 && CFS_DVA_BLK_MASK == 0x00FFFFFFFFFFFFFFull && CFS_BLOCK == 4096 &&
                   CFS_MHDR_SIZE == 32 && CFS_BITS_PER_BITMAP == 32512 && CFS_PTRS_PER_BLOCK == 508 &&
                   CFS_INODE_SIZE == 256 && CFS_MIN_BLOCKS == 64 && CFS_MAX_BLOCKS == 16516096 &&
                   CFS_MAX_MEMBERS == 255 && CFS_MAX_COPIES == 4 && CFS_LABEL_MAX == 16 && CFS_FORMAT_VERSION == 11 &&
                   CFS_VERSION_MIN == 2 && CFS_VERSION == 15 && CFS_SUPER_A == 0 && CFS_SUPER_B == 1 &&
                   CFS_ROOT_INO == 1 && CFS_TYPE_DIR == 2 && CFS_MODE(CFS_TYPE_DIR, 0755) == ((2u << 12) | 493) &&
                   CFS_CSUM_CRC32C == 1 && CFS_CSUM_POLY1305 == 2 && CFS_COMPRESS_NONE == 0 &&
                   CFS_KEY_SLOTS == 8 && CFS_KEYSLOT_EMPTY == 0,
               "cosmofs_format's cosmofs_format.h and cosmofs_internal.h values");
_Static_assert(CFS_KIND_IMAP1 == 1 && CFS_KIND_IMAP0 == 2 && CFS_KIND_INODES == 3 && CFS_KIND_ALLOCIDX == 4 &&
                   CFS_KIND_BITMAP == 5 && CFS_KIND_MEMBERS == 11,
               "cosmofs_format's block kinds");
_Static_assert(offsetof(struct cfs_member_block, count) == 0 && offsetof(struct cfs_member_block, m) == 8 &&
                   sizeof(struct cfs_member) == 64 && sizeof(struct cfs_keyslot) == 96 &&
                   sizeof(struct cosmo_keyslot) == 96 && offsetof(struct cfs_format_keys, slot) == 48 &&
                   offsetof(struct cfs_inode, mode) == 0 && offsetof(struct cfs_inode, nlink) == 4 &&
                   offsetof(struct cfs_inode, generation) == 40 && offsetof(struct cfs_inode, ino) == 48 &&
                   offsetof(struct cfs_inode, parent) == 224 && offsetof(struct cfs_inode, csum_algo) == 232 &&
                   offsetof(struct cfs_inode, compress_algo) == 248 && offsetof(struct cfs_dead_block, count) == 8 &&
                   offsetof(struct cfs_mhdr, crc) == 24 && offsetof(struct cfs_super, crc) == 160,
               "cosmofs_format.cee reads and writes these offsets and sizes");
/* cosmofs_format.cee keeps a struct cfs in a stack slot of 128 words (and refuses to format if it outgrew it). */
_Static_assert(sizeof(struct cfs) <= 128 * 8, "cosmofs_format.cee's stack slot");

/* The digest of the C (cosmofs_format.c, 2026-11-07), which the CeeSafe port gives. */
#define FORMAT_DIGEST 0x4d8818e293f81719ull
#define ROUNDS 8000u
#define SEED 0xF0F3A72026B0F0F0ull

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

void random_get_bytes(void *buf, size_t len)
{
    uint8_t *b = buf;
    note(3, len);
    for (size_t i = 0; i < len; i++)
        b[i] = (uint8_t)next();
}

/* ---- the devices and the pool ---- */

#define NDEV 8u

static struct blkdev g_dev[NDEV];
static uint64_t g_devsz[NDEV];
static unsigned g_open_fail, g_write_fail;
static int g_pools;   /* open */

/* What the pool has written: block 0 and 1 of member 0, and the blocks the poison hooks reread. */
#define KEEP 64u
static struct {
    uint64_t dva;
    uint8_t data[CFS_BLOCK];
} g_kept[KEEP];
static unsigned g_nkept;

static unsigned dev_index(const struct blkdev *bd) { return (unsigned)(bd - g_dev); }

int pool_open(struct blkdev *bd, struct spool **out)
{
    note(10, dev_index(bd));
    if (g_open_fail != 0 && pick(g_open_fail) == 0)
        return -EIO;
    struct spool *p = calloc(1, sizeof(*p));
    EXPECT(p != NULL);
    p->m = calloc(NDEV, sizeof(*p->m));
    EXPECT(p->m != NULL);
    p->nmembers = 1;
    p->m[0].ncopies = 1;
    p->m[0].nblocks = g_devsz[dev_index(bd)];
    p->nblocks = p->m[0].nblocks;
    g_pools++;
    *out = p;
    return 0;
}

int pool_add_member(struct spool *p, struct blkdev *bd, unsigned *vdev)
{
    EXPECT(vdev == NULL);
    note(11, dev_index(bd));
    if (g_open_fail != 0 && pick(g_open_fail) == 0)
        return -EBUSY;
    unsigned v = p->nmembers++;
    p->m[v].ncopies = 1;
    p->m[v].nblocks = g_devsz[dev_index(bd)];
    return 0;
}

int pool_add_copy(struct spool *p, unsigned vdev, struct blkdev *bd)
{
    note(12, vdev);
    note(13, dev_index(bd));
    if (g_open_fail != 0 && pick(g_open_fail) == 0)
        return -EINVAL;
    p->m[vdev].ncopies++;
    if (g_devsz[dev_index(bd)] < p->m[vdev].nblocks)
        p->m[vdev].nblocks = g_devsz[dev_index(bd)];
    return 0;
}

void pool_close(struct spool *p)
{
    note(14, p->nmembers);
    free(p->m);
    free(p);
    g_pools--;
}

static void keep(uint64_t dva, const void *buf)
{
    for (unsigned i = 0; i < g_nkept; i++)
        if (g_kept[i].dva == dva) {
            memcpy(g_kept[i].data, buf, CFS_BLOCK);
            return;
        }
    if (g_nkept < KEEP) {
        g_kept[g_nkept].dva = dva;
        memcpy(g_kept[g_nkept++].data, buf, CFS_BLOCK);
    }
}

int pool_write(struct spool *p, uint64_t dva, const void *buf)
{
    (void)p;
    note(15, dva);
    mix(buf, 64);
    mix((const uint8_t *)buf + 64, 1024);
    if (g_write_fail != 0 && pick(g_write_fail) == 0)
        return -EIO;
    keep(dva, buf);
    return 0;
}

int pool_read(struct spool *p, uint64_t dva, void *buf)
{
    (void)p;
    note(16, dva);
    for (unsigned i = 0; i < g_nkept; i++)
        if (g_kept[i].dva == dva) {
            memcpy(buf, g_kept[i].data, CFS_BLOCK);
            return 0;
        }
    return -EIO;
}

int pool_flush(struct spool *p)
{
    (void)p;
    note(17, 0);
    return chance(20) ? -EIO : 0;
}

/* ---- what the format calls besides the pool ---- */

static unsigned g_hook_fail;

static int hook_rc(void) { return g_hook_fail != 0 && pick(g_hook_fail) == 0 ? -EIO : 0; }

/* The seal, the test's: the header's fields, and a sum of the block standing in for its CRC or tag. */
void cfs_mhdr_seal(struct cfs *fs, uint8_t *block, uint32_t kind, uint64_t blkno)
{
    struct cfs_mhdr *h = (struct cfs_mhdr *)block;
    h->magic = CFS_MHDR_MAGIC;
    h->kind = kind;
    h->generation = fs->gen;
    h->blkno = blkno;
    h->crc = 0;
    h->crc = cfs_block_crc(block, offsetof(struct cfs_mhdr, crc)) ^ (fs->mac.on ? 0x5A5A5A5Au : 0);
}

int cfs_super_write(struct cfs *fs, unsigned slot, unsigned flags)
{
    note(20, slot);
    note(21, flags);
    mix(&fs->sb, sizeof(fs->sb));
    int rc = hook_rc();
    if (rc == 0) {
        uint8_t block[CFS_BLOCK];
        memset(block, 0, sizeof(block));
        memcpy(block, &fs->sb, sizeof(fs->sb));
        struct cfs_super *sb = (struct cfs_super *)block;
        sb->crc = 0;
        sb->crc = cfs_block_crc(block, offsetof(struct cfs_super, crc));
        keep(slot, block);
    }
    return rc;
}

int cfs_label_write(struct spool *pool, const struct cfs_mac *mac, unsigned vdev, unsigned copy, uint64_t generation,
                    const uint8_t *uuid, uint64_t nblocks)
{
    (void)pool;
    note(22, mac->on);
    note(23, vdev);
    note(24, copy);
    note(25, generation);
    mix(uuid, 16);
    note(26, nblocks);
    return hook_rc();
}

void cfs_mac_derive(const uint8_t *master, struct cfs_mac *out)
{
    mix(master, 32);
    out->on = true;
    for (unsigned i = 0; i < sizeof(out->key); i++)
        out->key[i] = (uint8_t)(master[i % 32] ^ 0x3C);
}

int cfs_keys2_write(struct spool *pool, const struct cfs_mac *mac, uint64_t dva, uint64_t generation,
                    const uint8_t *uuid, const struct cfs_keyslot *slots)
{
    (void)pool;
    note(27, mac->on);
    note(28, dva);
    note(29, generation);
    mix(uuid, 16);
    mix(slots, sizeof(struct cfs_keyslot) * CFS_KEY_SLOTS);
    return hook_rc();
}

int cfs_keys_write(struct spool *pool, uint64_t dva, uint64_t generation, const uint8_t *master, const void *user_key,
                   size_t key_len)
{
    (void)pool;
    note(30, dva);
    note(31, generation);
    mix(master, 32);
    mix(user_key, key_len);
    return hook_rc();
}

int cfs_keytab_install(const uint8_t *uuid, const uint8_t *master)
{
    mix(uuid, 16);
    mix(master, 32);
    return hook_rc();
}

static unsigned g_unseeded;

int cfs_need_seeded(const char *what)
{
    mix(what, strlen(what));
    return g_unseeded != 0 && pick(g_unseeded) == 0 ? -EAGAIN : 0;
}

/* ---- the formats asked for ---- */

static void devices(void)
{
    for (unsigned i = 0; i < NDEV; i++) {
        unsigned k = pick(20);
        g_devsz[i] = k == 0 ? CFS_MIN_BLOCKS - 1 - pick(10)                 /* too small */
                     : k == 1 ? (uint64_t)CFS_PTRS_PER_BLOCK * CFS_BITS_PER_BITMAP + 1 + pick(9)   /* too many chunks */
                     : k == 2 ? CFS_MIN_BLOCKS + pick(40)
                     : k == 3 ? CFS_BITS_PER_BITMAP + pick(9000)          /* two bitmap chunks */
                              : 200 + pick(4000);
    }
}

static void make_label(char *out, size_t n, const char **label)
{
    unsigned k = pick(5);
    if (k == 0) {
        *label = NULL;
        return;
    }
    if (k == 1)
        snprintf(out, n, "%s", "");
    else if (k == 2)
        snprintf(out, n, "%s", "0123456789abcdef");        /* exactly CFS_LABEL_MAX */
    else if (k == 3)
        snprintf(out, n, "%s", "0123456789abcdefg");       /* one too many */
    else
        snprintf(out, n, "pool%u", pick(100));
    *label = out;
}

static void make_keys(struct cfs_format_keys *fk)
{
    memset(fk, 0, sizeof(*fk));
    for (unsigned i = 0; i < 16; i++)
        fk->uuid[i] = (uint8_t)next();
    if (pick(10) == 0)
        memset(fk->uuid, 0, 16);
    for (unsigned i = 0; i < 32; i++)
        fk->master[i] = (uint8_t)next();
    unsigned none = pick(8);
    for (unsigned i = 0; i < CFS_KEY_SLOTS; i++) {
        struct cfs_keyslot *s = &fk->slot[i];
        unsigned k = none == 0 ? 5 : pick(6);   /* now and then every slot empty: no way in */
        if (k == 0 && (i == 0 || pick(8) == 0)) {
            s->kind = CFS_KEYSLOT_KEYFILE;   /* at slot 0 only, or the slots are refused */
        } else if (k == 1) {
            s->kind = CFS_KEYSLOT_PASSPHRASE;
            s->t_cost = 1 + pick(4);
            s->m_cost_kib = 8192 * (1 + pick(4));
            s->lanes = 1 + pick(2);
        } else if (k == 2 && pick(4) == 0) {
            s->kind = 7;   /* no such kind */
        }
        for (unsigned j = 0; j < 16; j++)
            s->salt[j] = (uint8_t)next();
    }
}

static void op(void)
{
    unsigned which = pick(13);
    note(40, which);
    unsigned fi = pick(10);
    g_fail_in = fi == 0 ? (int)pick(3) : -1;
    struct blkdev *bd[NDEV];
    for (unsigned i = 0; i < NDEV; i++)
        bd[i] = &g_dev[pick(NDEV)];
    char lbuf[32];
    const char *label = NULL;
    int rc = 0;
    switch (which) {
    case 0:
        rc = cosmofs_format(bd[0]);
        break;
    case 1: {
        unsigned k = pick(10);
        /* None, or more than the pool can have: refused before a device is looked at. */
        rc = cosmofs_format_pool(bd, k == 0 ? 0 : k == 1 ? CFS_MAX_MEMBERS + 1 : 1 + pick(4));
        break;
    }
    case 2: {
        unsigned n = 1 + pick(3), kc = pick(6);
        unsigned copies = kc == 0 ? 5 : kc == 1 ? 0 : 1 + pick(2);
        if (n * copies > NDEV)
            n = 1;
        rc = cosmofs_format_mirror(bd, n, copies);
        break;
    }
    case 3:
        make_label(lbuf, sizeof(lbuf), &label);
        rc = cfs_format_mirror_devs(bd, 1 + pick(3), label);
        break;
    case 4:
        make_label(lbuf, sizeof(lbuf), &label);
        rc = cosmofs_format_label(bd[0], label);
        break;
    case 5: {
        uint8_t key[24];
        for (unsigned i = 0; i < sizeof(key); i++)
            key[i] = (uint8_t)next();
        unsigned nk = pick(6);
        rc = cosmofs_format_encrypted(bd[0], nk == 0 ? NULL : key, nk == 1 ? 0 : 1 + pick(sizeof(key)));
        break;
    }
    case 6:
    case 7: {
        struct cfs_format_keys fk;
        make_keys(&fk);
        make_label(lbuf, sizeof(lbuf), &label);
        unsigned n = 1 + pick(2);
        if (which == 6)
            rc = cosmofs_format_keyed(bd, n, label, &fk);
        else
            rc = cosmofs_format_keys(bd, n, label, fk.uuid, fk.master, (const struct cosmo_keyslot *)fk.slot);
        break;
    }
    case 8: {
        struct cfs_format_keys fk;
        make_keys(&fk);
        rc = cosmofs_test_format_keyed_members(bd, 1 + pick(3), fk.uuid, fk.master,
                                               (const struct cosmo_keyslot *)fk.slot);
        break;
    }
    case 9:
        rc = cosmofs_test_format_version(bd[0], pick(18));
        break;
    case 10:
    case 11: {
        /* A pool the test keeps: its superblocks, and a free record the first names. */
        g_nkept = 0;
        for (unsigned slot = 0; slot < 2; slot++) {
            uint8_t block[CFS_BLOCK];
            memset(block, 0, sizeof(block));
            struct cfs_super *sb = (struct cfs_super *)block;
            sb->generation = pick(3) == 0 ? 0 : 1 + pick(9);
            sb->free_root = pick(4) == 0 ? 0 : CFS_DVA(0, 10 + slot);
            sb->crc = cfs_block_crc(block, offsetof(struct cfs_super, crc));
            if (pick(5) == 0)
                sb->crc ^= 1;
            if (pick(6) != 0)
                keep(slot, block);
            uint8_t rec[CFS_BLOCK];
            memset(rec, 0, sizeof(rec));
            if (pick(5) != 0)
                keep(CFS_DVA(0, 10 + slot), rec);
        }
        unsigned saved = g_write_fail;
        unsigned wf = pick(4);
        if (wf == 0)
            g_write_fail = 2;   /* the poisoned block's write fails, now and then */
        if (which == 10)
            rc = cosmofs_test_poison_freelog_count(bd[0], CFS_DEAD_PER_BLOCK + pick(9));
        else
            rc = cosmofs_test_poison_free_root(bd[0], next());
        g_write_fail = saved;
        for (unsigned i = 0; i < g_nkept; i++) {
            note(41, g_kept[i].dva);
            mix(g_kept[i].data, 256);
        }
        break;
    }
    case 12: {
        rc = cosmofs_format(bd[pick(2)]);
        break;
    }
    }
    g_fail_in = -1;
    note(42, (uint64_t)(int64_t)rc);
    EXPECT(g_pools == 0);
    EXPECT(g_live == 0);
}

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    harness_klog_hook = on_klog;
    harness_klog_min = KLOG_ERROR + 1;
    for (unsigned i = 0; i < NDEV; i++)
        snprintf(g_dev[i].name, sizeof(g_dev[i].name), "vd%c", 'a' + i);
    for (unsigned round = 0; round < ROUNDS; round++) {
        unsigned f = pick(4);
        g_faults = f != 0;
        devices();
        unsigned of = pick(6), wf = pick(6), hf = pick(5), us = pick(6);
        g_open_fail = g_faults && of == 0 ? 6 : 0;
        g_write_fail = !g_faults ? 0 : wf == 0 ? 30 : wf == 1 ? 6 : 0;
        g_hook_fail = g_faults && hf == 0 ? 5 : 0;
        g_unseeded = g_faults && us <= 1 ? 2 : 0;
        g_nkept = 0;
        op();
    }
    harness_klog_hook = NULL;
    harness_klog_min = KLOG_WARN;
    printf("  format digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, ROUNDS);
    EXPECT(g_digest == FORMAT_DIGEST);
}

int main(void)
{
    static const struct host_test tests[] = {
        { "format_digest", test_digest },
    };
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
