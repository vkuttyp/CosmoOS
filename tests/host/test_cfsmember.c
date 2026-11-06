/*
 * test_cfsmember.c - cosmofs's members (kernel-services/filesystem/cosmofs/cosmofs_member.cee) on the host: roadmap
 * 2.0 SA7. A digest recorded from the C over seeded pools, with the real label and member-table checks (cfsparse, CRC32C)
 * and devices, superblocks, a pool, buffers and an allocator the test owns: labels written to copies and to devices,
 * cleared, read back sealed by CRC or by MAC; member tables loaded (valid, malformed, too big for the device, members
 * that are absent, stale, the wrong size), copies set up from records (ONLINE, missing, resilvering with and without
 * their target, finished before the last stop), lagging, tagged and newer devices found, spares found, the copy-state
 * records prepared, filled and checked, tables stored, labels stamped, holds, the linear addresses, and blocks read
 * with repair and verified on every copy. Every call, return, block written, logged line and the pool's state are in
 * the digest; device references and allocations balance.
 */

#include "harness.h"

#include <kernel/blk.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/list.h>
#include <kernel/log.h>
#include <kernel/storage.h>
#include <kernel/vfs.h>

#include "cosmofs_internal.h"   /* before the C library's headers, as test_cfsparse.c: its signal.h is the kernel's */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The values the CeeSafe port writes as its own constants, and the offsets and stack slots it relies on. */
_Static_assert(EIO == 5 && ENOMEM == 12 && ENODEV == 19 && EINVAL == 22 && ENOENT == 2 && ENOSPC == 28 && KMEM_ZERO == 1,
               "cosmofs_member's errno.h and kmalloc.h values");
_Static_assert(POOL_COPY_ONLINE == 0 && POOL_COPY_MISSING == 1 && POOL_COPY_RESILVERING == 4 && POOL_COPY_NSTATES == 5 &&
                   POOL_MAX_COPIES == 4 && MOUNT_HELD_MAX == 12 && CFS_BLOCK == 4096 && CFS_MHDR_SIZE == 32 &&
                   CFS_DVA_VDEV_SHIFT == 56 && CFS_DVA_NONE == 0xFFFFFFFFFFFFFFFFull && CFS_BITS_PER_BITMAP == 32512 &&
                   CFS_FORMAT_VERSION == 11 && CFS_KIND_MEMBERS == 11 && CFS_KIND_COPYSTATE == 17 &&
                   CFS_ALLOC_META == 0 && CFS_LABEL_RESILVER == 1 && CFS_LABEL_SPARE == 2 && CFS_COPY_WHOLE == 1 &&
                   CFS_COPY_DETACH == 2 && CFS_COPY_DIRTY_BYTES == 944,
               "cosmofs_member's storage.h, vfs.h and cosmofs_format.h values");
_Static_assert(offsetof(struct cfs_member_block, m) == 8 && offsetof(struct cfs_copystate, copy) == 16 &&
                   offsetof(struct cfs_label, crc) == 72 && sizeof(enum cfs_alloc_class) == 4 &&
                   sizeof(((struct cfs *)0)->pool_name) == 17,
               "cosmofs_member.cee reads a table's entries, a record set's records and a label's crc at these offsets");
/* cosmofs_member.cee keeps these in stack slots of words: a label 10, a superblock 21, a member table entry 8. */
_Static_assert(sizeof(struct cfs_label) <= 10 * 8 && sizeof(struct cfs_super) <= 21 * 8 &&
                   sizeof(struct cfs_member) <= 8 * 8,
               "cosmofs_member.cee's stack slots");

/* The digest of the C (cosmofs_member.c, 2026-11-06), which the CeeSafe port gives. */
#define MEMBER_DIGEST 0xf295f4d39f4bf4fdull
#define ROUNDS 4000u
#define SEED 0x3E3BE23E2026B01Aull

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

void *kzalloc(size_t size) { return kmalloc(size, KMEM_ZERO); }

void kfree(void *p)
{
    if (p != NULL)
        g_live--;
    free(p);
}

static unsigned g_fail_pct;   /* of the pool's, the buffers' and the devices' operations that fail */

static int draw_rc(void)
{
    unsigned r = pick(100);
    if (r >= g_fail_pct)
        return 0;
    return pick(2) == 0 ? -EIO : -ENOMEM;
}

/* ---- the devices: two blocks each (a label or superblock, and a second slot), referenced as kobjects ---- */

#define NDEV 10u

static struct blkdev g_dev[NDEV];
static int g_refs[NDEV];
static uint8_t g_img[NDEV][2][CFS_BLOCK];
static unsigned g_ndev;   /* blk_nth's: the registered ones */

static int dev_index(const struct blkdev *bd)
{
    EXPECT(bd >= g_dev && bd < g_dev + NDEV);
    return (int)(bd - g_dev);
}

void kobject_get(struct kobject *o)
{
    g_refs[dev_index((struct blkdev *)((uint8_t *)o - offsetof(struct blkdev, obj)))]++;
}

void kobject_put(struct kobject *o)
{
    int i = dev_index((struct blkdev *)((uint8_t *)o - offsetof(struct blkdev, obj)));
    EXPECT(g_refs[i] > 0);
    g_refs[i]--;
}

static struct blkdev *ref(struct blkdev *bd)
{
    blkdev_get(bd);
    return bd;
}

struct blkdev *blk_nth(unsigned i)
{
    return i < g_ndev ? ref(&g_dev[i]) : NULL;
}

/* Member 0's devices: their two superblock slots. */
struct super_slot {
    int rc;   /* 0, or what reading it says */
    unsigned uuid;   /* which of g_uuid */
    uint64_t generation;
};
static struct super_slot g_super[NDEV][2];
static int g_slots_rc[NDEV];
static uint8_t g_uuid[3][16];

int cfs_super_slots(struct blkdev *bd, struct cfs_super *a, int *ra, struct cfs_super *b, int *rb)
{
    int i = dev_index(bd);
    note(3, (uint64_t)i);
    if (g_slots_rc[i])
        return g_slots_rc[i];
    memset(a, 0, sizeof(*a));
    memset(b, 0, sizeof(*b));
    memcpy(a->uuid, g_uuid[g_super[i][0].uuid], 16);
    a->generation = g_super[i][0].generation;
    memcpy(b->uuid, g_uuid[g_super[i][1].uuid], 16);
    b->generation = g_super[i][1].generation;
    *ra = g_super[i][0].rc;
    *rb = g_super[i][1].rc;
    return 0;
}

/* ---- the pool: struct spool itself, the mount's; a device opened alone is a handle on its two blocks ---- */

#define MAXMEM 4u

static struct spool g_pool;
static uint8_t g_dirty[MAXMEM][POOL_MAX_COPIES][POOL_DIRTY_BYTES];
static bool g_unreadable[MAXMEM][POOL_MAX_COPIES];   /* not to be read for the data block */
struct handle {
    int dev;
};
static long g_handles;

/* The mount pool's metadata blocks: the member table and the copy-state records. */
#define NMETA 16u
static struct {
    uint64_t dva;
    uint8_t data[CFS_BLOCK];
} g_meta[NMETA];
static unsigned g_nmeta;

static uint8_t *meta_at(uint64_t dva, bool make)
{
    for (unsigned i = 0; i < g_nmeta; i++)
        if (g_meta[i].dva == dva)
            return g_meta[i].data;
    if (!make || g_nmeta == NMETA)
        return NULL;
    g_meta[g_nmeta].dva = dva;
    memset(g_meta[g_nmeta].data, 0, CFS_BLOCK);
    return g_meta[g_nmeta++].data;
}

/* The data block read with repair: each copy's first byte, and whether reading it fails. */
static uint8_t g_rr[POOL_MAX_COPIES];
static int g_rr_rc[POOL_MAX_COPIES];

int pool_open(struct blkdev *bd, struct spool **out)
{
    note(4, (uint64_t)dev_index(bd));
    int rc = draw_rc();
    if (rc)
        return rc;
    struct handle *h = calloc(1, sizeof(*h));
    h->dev = dev_index(bd);
    g_handles++;
    *out = (struct spool *)h;
    return 0;
}

void pool_close(struct spool *p)
{
    EXPECT(p != &g_pool);
    g_handles--;
    free(p);
}

static uint8_t *dev_block(struct spool *p, uint64_t dva)
{
    EXPECT(p != &g_pool && CFS_DVA_VDEV(dva) == 0 && CFS_DVA_BLK(dva) < 2);
    return g_img[((struct handle *)p)->dev][CFS_DVA_BLK(dva)];
}

int pool_read(struct spool *p, uint64_t dva, void *buf)
{
    note(5, dva);
    int rc = draw_rc();
    if (rc)
        return rc;
    if (p == &g_pool) {
        uint8_t *m = meta_at(dva, false);
        if (m == NULL)
            return -EIO;
        memcpy(buf, m, CFS_BLOCK);
        return 0;
    }
    memcpy(buf, dev_block(p, dva), CFS_BLOCK);
    return 0;
}

int pool_write(struct spool *p, uint64_t dva, const void *buf)
{
    note(6, dva);
    mix(buf, CFS_BLOCK);
    int rc = draw_rc();
    if (rc == 0)
        memcpy(dev_block(p, dva), buf, CFS_BLOCK);
    return rc;
}

int pool_flush(struct spool *p)
{
    EXPECT(p != &g_pool);
    return draw_rc();
}

unsigned pool_copies(const struct spool *p, uint64_t dva)
{
    unsigned v = CFS_DVA_VDEV(dva);
    return v < p->nmembers ? p->m[v].ncopies : 0;
}

bool pool_copy_online(const struct spool *p, unsigned v, unsigned c)
{
    return p->m[v].copy[c].state == POOL_COPY_ONLINE && p->m[v].copy[c].dev != NULL;
}

bool pool_copy_readable(const struct spool *p, uint64_t dva, unsigned c)
{
    unsigned v = CFS_DVA_VDEV(dva);
    return pool_copy_online(p, v, c) && !g_unreadable[v][c];
}

bool pool_member_degraded(const struct spool *p, unsigned v)
{
    for (unsigned c = 0; c < p->m[v].ncopies; c++)
        if (!pool_copy_online(p, v, c))
            return true;
    return false;
}

int pool_read_copy(struct spool *p, uint64_t dva, unsigned c, void *buf)
{
    EXPECT(p == &g_pool);
    note(7, dva);
    note(8, c);
    if (g_rr_rc[c])
        return g_rr_rc[c];
    memset(buf, 0, CFS_BLOCK);
    *(uint8_t *)buf = g_rr[c];
    return 0;
}

int pool_write_copy(struct spool *p, uint64_t dva, unsigned c, const void *buf)
{
    EXPECT(p == &g_pool);
    note(9, dva);
    note(10, c);
    mix(buf, CFS_BLOCK);
    int rc = draw_rc();
    if (rc)
        return rc;
    if (CFS_DVA_BLK(dva) == 0) {
        struct blkdev *bd = p->m[CFS_DVA_VDEV(dva)].copy[c].dev;
        if (bd != NULL)
            memcpy(g_img[dev_index(bd)][0], buf, CFS_BLOCK);   /* a label, to that copy's device */
    } else {
        g_rr[c] = *(const uint8_t *)buf;
    }
    return 0;
}

static void clear_copy(unsigned v, unsigned c)
{
    struct pool_copy *cp = &g_pool.m[v].copy[c];
    if (cp->dev != NULL)
        blkdev_put(cp->dev);
    memset(cp, 0, sizeof(*cp));
    cp->state = POOL_COPY_MISSING;
    cp->dirty = g_dirty[v][c];
    memset(cp->dirty, 0, POOL_DIRTY_BYTES);
}

int pool_set_copies(struct spool *p, unsigned v, unsigned n)
{
    note(11, n);
    int rc = draw_rc();
    if (rc)
        return rc;
    for (unsigned c = p->m[v].ncopies; c < n && c < POOL_MAX_COPIES; c++)
        clear_copy(v, c);
    p->m[v].ncopies = n;
    return 0;
}

int pool_attach_copy(struct spool *p, unsigned v, unsigned c, struct blkdev *bd, enum pool_copy_state state)
{
    note(12, (uint64_t)dev_index(bd));
    note(13, state);
    int rc = draw_rc();
    if (rc)
        return rc;
    p->m[v].copy[c].dev = ref(bd);
    p->m[v].copy[c].state = state;
    return 0;
}

static uint64_t g_dev_blocks[NDEV];

int pool_add_member(struct spool *p, struct blkdev *bd, unsigned *vdev)
{
    note(14, (uint64_t)dev_index(bd));
    int rc = draw_rc();
    if (rc)
        return rc;
    if (p->nmembers == MAXMEM)
        return -ENOSPC;
    unsigned v = p->nmembers++;
    struct spool_member *m = &p->m[v];
    memset(m, 0, sizeof(*m));
    m->ncopies = 1;
    m->copy[0].dev = ref(bd);
    m->copy[0].state = POOL_COPY_ONLINE;
    unsigned nodirty = pick(3);
    m->copy[0].dirty = nodirty == 0 ? NULL : g_dirty[v][0];   /* a member's bitmaps come with its second slot */
    m->nblocks = g_dev_blocks[dev_index(bd)];
    m->region_blocks = 64;
    unsigned off = pick(6);
    *vdev = off == 0 ? v + 1 : v;   /* now and then a number the caller did not expect */
    return 0;
}

void pool_set_member_blocks(struct spool *p, unsigned v, uint64_t nblocks)
{
    note(15, nblocks);
    p->m[v].nblocks = nblocks;
}

bool pool_find_dev(const struct spool *p, const struct blkdev *bd, unsigned *v, unsigned *c)
{
    for (unsigned i = 0; i < p->nmembers; i++)
        for (unsigned k = 0; k < p->m[i].ncopies; k++)
            if (p->m[i].copy[k].dev == bd) {
                *v = i;
                *c = k;
                return true;
            }
    return false;
}

int pool_add_spare(struct spool *p, struct blkdev *bd, uint64_t tag)
{
    note(16, (uint64_t)dev_index(bd));
    note(17, tag);
    if (p->nspares == POOL_MAX_SPARES)
        return -ENOSPC;
    for (unsigned i = 0; i < p->nspares; i++)
        if (p->spare[i] == bd)
            return -EEXIST;
    p->spare[p->nspares] = ref(bd);
    p->spare_tag[p->nspares++] = tag;
    return 0;
}

void pool_mark_missed(struct spool *p, uint64_t dva)
{
    EXPECT(p == &g_pool);
    note(18, dva);
}

void pool_copy_csum_error(struct spool *p, uint64_t dva, unsigned c)
{
    EXPECT(p == &g_pool);
    note(19, dva);
    note(20, c);
}

const char *pool_copy_state_name(enum pool_copy_state s)
{
    static const char *const names[] = { "ONLINE", "MISSING", "OFFLINE", "FAULTED", "RESILVERING" };
    return (unsigned)s < 5 ? names[s] : "?";
}

/* ---- the seals, the buffers, the allocator, the raise, the holds ---- */

#define TAG_AT (CFS_BLOCK - 8)

void cfs_mac_seal(const struct cfs_mac *m, char domain, uint8_t *block, size_t off)
{
    note(21, (uint64_t)domain);
    memcpy(block + off, m->key, 8);
}

bool cfs_mac_ok(const struct cfs_mac *m, char domain, const uint8_t *block, size_t off)
{
    note(22, (uint64_t)domain);
    return memcmp(block + off, m->key, 8) == 0;
}

bool cfs_mhdr_ok(const struct cfs_mac *mac, const void *block, uint64_t dva, uint32_t kind)
{
    const struct cfs_mhdr *h = block;
    note(23, kind);
    if (h->kind != kind || h->blkno != dva)
        return false;
    return !mac->on || memcmp((const uint8_t *)block + TAG_AT, mac->key, 8) == 0;
}

static long g_bufs;
static unsigned g_next_blk;

int cfs_buf_get(struct cfs *fs, uint64_t blkno, uint32_t kind, struct cfs_buf **out)
{
    (void)fs;
    note(24, blkno);
    note(25, kind);
    int rc = draw_rc();
    uint8_t *m = meta_at(blkno, false);
    if (rc == 0 && (m == NULL || ((struct cfs_mhdr *)m)->kind != kind))
        rc = -EIO;
    if (rc)
        return rc;
    struct cfs_buf *b = calloc(1, sizeof(*b));
    b->data = malloc(CFS_BLOCK);
    memcpy(b->data, m, CFS_BLOCK);
    b->blkno = blkno;
    g_bufs++;
    *out = b;
    return 0;
}

static struct cfs_buf *new_buf(uint64_t blkno, uint32_t kind)
{
    struct cfs_buf *b = calloc(1, sizeof(*b));
    b->data = calloc(1, CFS_BLOCK);
    ((struct cfs_mhdr *)b->data)->kind = kind;
    ((struct cfs_mhdr *)b->data)->blkno = blkno;
    b->blkno = blkno;
    g_bufs++;
    return b;
}

int cfs_buf_new_at(struct cfs *fs, uint32_t kind, uint64_t blk, struct cfs_buf **out)
{
    (void)fs;
    note(26, blk);
    int rc = draw_rc();
    if (rc)
        return rc;
    *out = new_buf(blk, kind);
    return 0;
}

int cfs_buf_cow(struct cfs *fs, struct cfs_buf **bp, uint64_t *parent_slot)
{
    (void)fs;
    note(27, (*bp)->blkno);
    int rc = draw_rc();
    if (rc)
        return rc;
    uint64_t moved = CFS_DVA(CFS_DVA_VDEV((*bp)->blkno), 40 + g_next_blk++ % 20);
    (*bp)->blkno = moved;
    ((struct cfs_mhdr *)(*bp)->data)->blkno = moved;
    *parent_slot = moved;
    note(28, moved);
    return 0;
}

void cfs_buf_mark_dirty(struct cfs *fs, struct cfs_buf *b)
{
    (void)fs;
    note(29, b->blkno);
    mix(b->data, CFS_BLOCK);
    uint8_t *m = meta_at(b->blkno, true);
    if (m != NULL)
        memcpy(m, b->data, CFS_BLOCK);
}

void cfs_buf_put(struct cfs *fs, struct cfs_buf *b)
{
    (void)fs;
    note(30, b->blkno);
    g_bufs--;
    free(b->data);
    free(b);
}

int cfs_alloc_run(struct cfs *fs, enum cfs_alloc_class cls, uint64_t hint, uint32_t want, uint64_t *start,
                  uint64_t *got)
{
    (void)fs;
    note(31, cls);
    note(32, hint);
    note(33, want);
    int rc = draw_rc();
    if (rc)
        return rc;
    *start = CFS_DVA(CFS_DVA_VDEV(hint), 20 + g_next_blk++ % 20);
    *got = want;
    return 0;
}

void cfs_free_block_deferred(struct cfs *fs, uint64_t blk)
{
    (void)fs;
    note(34, blk);
}

int cfs_snap_raise(struct cfs *fs)
{
    note(35, fs->sb.version);
    int rc = draw_rc();
    if (rc == 0 && fs->sb.version < 12)
        fs->sb.version = 12;
    return rc;
}

int vfs_mount_set_held(struct mount *mnt, struct blkdev *const *bd, unsigned n)
{
    (void)mnt;
    note(36, n);
    for (unsigned i = 0; i < n; i++)
        note(37, (uint64_t)dev_index(bd[i]));
    unsigned busy = pick(10);
    return busy == 0 ? -EBUSY : 0;
}

/* ---- the worlds ---- */

static struct cfs *g_fs;
static struct mount g_mnt;
static struct cfs_buf g_dirty_bufs[4];   /* on fs->bufs: blocks the open transaction wrote */

static void label_dev(int d, unsigned vdev, unsigned uuid, uint64_t nblocks, uint64_t gen, uint64_t flags,
                      uint64_t tag, bool mac)
{
    uint8_t *b = g_img[d][0];
    memset(b, 0, CFS_BLOCK);
    struct cfs_label *l = (struct cfs_label *)b;
    memcpy(l->magic, CFS_LABEL_MAGIC, 8);
    l->version = 13;
    l->index = vdev;
    memcpy(l->uuid, g_uuid[uuid], 16);
    l->nblocks = nblocks;
    l->generation = gen;
    l->flags = flags;
    l->tag = tag;
    if (mac)
        memcpy(b + offsetof(struct cfs_label, crc), g_fs->mac.key, 8);
    else
        l->crc = cfs_label_crc(b);
}

static void build(void)
{
    struct cfs *fs = g_fs;
    memset(g_img, 0, sizeof(g_img));
    memset(g_super, 0, sizeof(g_super));
    memset(g_slots_rc, 0, sizeof(g_slots_rc));
    g_nmeta = 0;
    g_next_blk = 0;
    memset(fs, 0, sizeof(*fs));
    list_init(&fs->bufs);
    fs->mnt = &g_mnt;
    fs->pool = &g_pool;
    unsigned ver = pick(6);
    fs->sb.version = ver == 0 ? 3 : ver == 1 ? 4 + pick(7) : ver == 2 ? 11 + pick(2) : 13 + pick(2);
    fs->sb.generation = 10 + pick(20);
    fs->gen = fs->sb.generation + 1;
    fs->sb.total_blocks = 200 + pick(800);
    fs->sb.alloc_root = CFS_DVA(0, 3);
    fs->sb.members = CFS_DVA(0, 5);
    memcpy(fs->sb.uuid, g_uuid[0], 16);
    unsigned zero = pick(20);
    if (zero == 0)
        memset(fs->sb.uuid, 0, 16);
    unsigned mac = pick(4);
    fs->mac.on = mac == 0;
    for (unsigned i = 0; i < 32; i++)
        fs->mac.key[i] = (uint8_t)next();
    unsigned ro = pick(10);
    fs->rdonly = ro == 0;
    snprintf(fs->pool_name, sizeof(fs->pool_name), "pool%u", pick(9));
    /* The mount pool: member 0 on d0, as a mount assembles it. */
    memset(&g_pool, 0, sizeof(g_pool));
    g_pool.m = calloc(MAXMEM, sizeof(*g_pool.m));
    g_pool.nmembers = 1;
    g_pool.m[0].ncopies = 1;
    g_pool.m[0].copy[0].dev = ref(&g_dev[0]);
    g_pool.m[0].copy[0].state = POOL_COPY_ONLINE;
    unsigned nodirty = pick(3);
    g_pool.m[0].copy[0].dirty = nodirty == 0 ? NULL : g_dirty[0][0];
    g_pool.m[0].region_blocks = 64;
    g_mnt.bdev = &g_dev[0];
    /* The table. */
    unsigned nm = 1 + pick(3);
    uint64_t nblocks[MAXMEM];
    unsigned copies[MAXMEM];
    for (unsigned v = 0; v < MAXMEM; v++) {
        nblocks[v] = 100 + pick(900);
        unsigned c = pick(4);
        copies[v] = c == 0 ? 1 : 1 + pick(3);
    }
    g_pool.m[0].nblocks = nblocks[0] + pick(3);
    uint8_t *tb = meta_at(fs->sb.members, true);
    struct cfs_mhdr *th = (struct cfs_mhdr *)tb;
    th->kind = CFS_KIND_MEMBERS;
    unsigned badhdr = pick(30);
    th->blkno = badhdr == 0 ? 7 : fs->sb.members;
    if (fs->mac.on) {
        unsigned badtag = pick(20);
        memcpy(tb + TAG_AT, fs->mac.key, 8);
        if (badtag == 0)
            tb[TAG_AT] ^= 1;
    }
    struct cfs_member_block *mb = (struct cfs_member_block *)(tb + CFS_MHDR_SIZE);
    unsigned badcount = pick(30);
    mb->count = badcount == 0 ? 0 : nm;
    for (unsigned v = 0; v < nm; v++) {
        struct cfs_member *e = &mb->m[v];
        memcpy(e->uuid, g_uuid[pick(3)], 16);
        e->nblocks = nblocks[v];
        e->first_usable = v == 0 ? 2 : 1;
        e->alloc_root = CFS_DVA(v, 2 + pick(3));
        e->free_blocks = pick(50);
        unsigned v4 = pick(6);
        e->copies = v4 == 0 ? 0 : copies[v];
        unsigned insane = pick(40);
        if (insane == 0)
            e->first_usable = 0;
        unsigned rec = pick(3);
        e->state_root = copies[v] > 1 && rec != 0 ? CFS_DVA(v, 10 + v) : 0;
        if (e->state_root != 0) {
            /* The member's copy-state record. */
            uint8_t *sb = meta_at(e->state_root, true);
            struct cfs_mhdr *sh = (struct cfs_mhdr *)sb;
            sh->kind = CFS_KIND_COPYSTATE;
            sh->blkno = e->state_root;
            struct cfs_copystate *cs = (struct cfs_copystate *)(sb + CFS_MHDR_SIZE);
            unsigned odd = pick(15);
            cs->ncopies = odd == 0 ? copies[v] + 1 : copies[v];
            cs->region_blocks = odd == 1 ? 32 : 64;
            for (unsigned c = 0; c < copies[v]; c++) {
                struct cfs_copy_rec *r = &cs->copy[c];
                static const uint32_t states[] = { CFS_COPY_ONLINE, CFS_COPY_ONLINE, CFS_COPY_ONLINE, CFS_COPY_ONLINE,
                                                   CFS_COPY_MISSING, CFS_COPY_OFFLINE, CFS_COPY_FAULTED,
                                                   CFS_COPY_RESILVERING, CFS_COPY_RESILVERING, 7 };
                r->state = states[pick(10)];
                r->flags = pick(4);
                r->since = fs->sb.generation - pick(4);
                r->cursor = pick(100);
                r->read_errors = pick(3);
                r->tag = pick(2) == 0 ? 0 : 1 + pick(2);
                r->peer = pick(2);
                unsigned at = pick(CFS_COPY_DIRTY_BYTES);
                r->dirty[at] = (uint8_t)next();
            }
        }
    }
    /* The devices: d0 the mount's; the others copies of members, current or behind, targets, spares, strangers. */
    unsigned nd = 4 + pick(NDEV - 3);
    g_ndev = nd;
    for (unsigned d = 0; d < NDEV; d++) {
        g_dev[d].gone = 0;
        unsigned dg = pick(15);
        if (d > 0 && dg == 0)
            g_dev[d].gone = 1;
        g_dev_blocks[d] = 1000;
    }
    for (unsigned d = 1; d < nd; d++) {
        unsigned kind = pick(10);
        unsigned v = pick(nm);
        unsigned uuid = pick(5) == 0 ? 1 : 0;
        uint64_t nb = nblocks[v];
        unsigned sz = pick(15);
        if (sz == 0)
            nb++;   /* the wrong disk */
        unsigned behind = pick(4);
        uint64_t gen = behind == 0 ? fs->sb.generation - 1 - pick(3) : fs->sb.generation + pick(2);
        unsigned small = pick(10);
        g_dev_blocks[d] = small == 0 ? nblocks[v] - 1 : nblocks[v] + pick(20);
        bool sealmac = fs->mac.on && pick(8) != 0;
        if (kind < 5) {
            label_dev((int)d, v, uuid, nb, gen, 0, 0, sealmac);
        } else if (kind == 5 || kind == 9) {
            label_dev((int)d, v, uuid, nb, gen, CFS_LABEL_RESILVER, 1 + pick(2), sealmac);
        } else if (kind == 6) {
            label_dev((int)d, 0, uuid, 1000, gen, CFS_LABEL_SPARE, 1 + pick(4), sealmac);
        } else if (kind == 7) {
            for (unsigned i = 0; i < 64; i++)
            {
                unsigned at = pick(CFS_BLOCK);
                g_img[d][0][at] = (uint8_t)next();   /* not a label */
            }
        } else {
            label_dev((int)d, v, uuid, nb, gen, 0, 0, sealmac);
            unsigned at = pick(64);
            g_img[d][0][at] ^= 0x10;   /* torn: its seal fails */
        }
        /* As a member-0 device: two superblock slots. */
        for (unsigned s = 0; s < 2; s++) {
            unsigned st = pick(5);
            g_super[d][s].rc = st == 0 ? -EIO : 0;
            g_super[d][s].uuid = pick(4) == 0 ? 1 : 0;
            g_super[d][s].generation = fs->sb.generation - 2 + pick(5);
        }
        unsigned src = pick(15);
        g_slots_rc[d] = src == 0 ? -EIO : 0;
    }
    /* The open transaction's dirty blocks. */
    for (unsigned i = 0; i < 4; i++) {
        memset(&g_dirty_bufs[i], 0, sizeof(g_dirty_bufs[i]));
        g_dirty_bufs[i].blkno = CFS_DVA(pick(nm), 30 + pick(30));
        unsigned dirty = pick(2);
        g_dirty_bufs[i].dirty = dirty != 0;
        list_push_back(&fs->bufs, &g_dirty_bufs[i].link);
    }
}

static void teardown(void)
{
    for (unsigned v = 0; v < g_pool.nmembers; v++)
        for (unsigned c = 0; c < g_pool.m[v].ncopies && c < POOL_MAX_COPIES; c++)
            if (g_pool.m[v].copy[c].dev != NULL)
                blkdev_put(g_pool.m[v].copy[c].dev);
    for (unsigned i = 0; i < g_pool.nspares; i++)
        blkdev_put(g_pool.spare[i]);
    free(g_pool.m);
    cfs_members_free(g_fs);
    for (unsigned d = 0; d < NDEV; d++)
        EXPECT(g_refs[d] == 0);
    EXPECT(g_handles == 0 && g_bufs == 0);
}

static void dump(void)
{
    struct cfs *fs = g_fs;
    note(50, fs->nmembers);
    note(51, fs->nblocks);
    note(52, fs->nr_chunks);
    note(53, fs->degraded);
    note(54, fs->sb.version);
    note(55, fs->sb.members);
    note(56, fs->sb.alloc_root);
    note(57, fs->repairs);
    for (unsigned v = 0; v < fs->nmembers && fs->mem != NULL; v++) {
        const struct cfs_memstate *m = &fs->mem[v];
        note(58, m->nblocks);
        note(59, m->first_usable);
        note(60, m->base);
        note(61, m->chunk0 * 1000u + m->nchunks);
        note(62, m->alloc_root);
        note(63, m->free_blocks);
        note(64, m->copies);
        note(65, m->state_root);
        mix(m->uuid, 16);
    }
    note(66, g_pool.nmembers);
    note(67, g_pool.nspares);
    for (unsigned v = 0; v < g_pool.nmembers; v++) {
        note(68, g_pool.m[v].ncopies);
        note(69, g_pool.m[v].nblocks);
        for (unsigned c = 0; c < g_pool.m[v].ncopies && c < POOL_MAX_COPIES; c++) {
            const struct pool_copy *cp = &g_pool.m[v].copy[c];
            note(70, cp->dev ? (uint64_t)dev_index(cp->dev) : 99);
            note(71, cp->state);
            note(72, cp->whole);
            note(73, cp->since);
            note(74, cp->cursor);
            note(75, cp->tag);
            note(76, cp->peer);
            note(77, cp->read_errors + cp->write_errors * 100u + cp->csum_errors * 10000u);
            if (cp->dirty)
                mix(cp->dirty, POOL_DIRTY_BYTES);
        }
    }
}

static bool verify_first(const void *blk, void *arg)
{
    note(80, *(const uint8_t *)blk);
    return *(const uint8_t *)blk == *(const uint8_t *)arg;
}

static void reads(void)
{
    struct cfs *fs = g_fs;
    if (g_pool.nmembers == 0)
        return;
    unsigned v = pick(g_pool.nmembers + 1);   /* now and then a member the pool does not have */
    uint8_t want = 0x5A;
    for (unsigned c = 0; c < POOL_MAX_COPIES; c++) {
        unsigned k = pick(4);
        g_rr[c] = k == 0 ? 0x11 : want;
        unsigned f = pick(6);
        g_rr_rc[c] = f == 0 ? -EIO : 0;
        unsigned u = pick(3);
        g_unreadable[v < MAXMEM ? v : 0][c] = u == 0;
    }
    uint8_t buf[CFS_BLOCK];
    unsigned which = pick(2);
    if (which == 0) {
        bool repaired = true, read_ok = true;
        unsigned nulls = pick(4);
        int rc = cfs_read_repair(fs, CFS_DVA(v, 9), buf, verify_first, &want, nulls == 0 ? NULL : &repaired,
                                 nulls == 0 ? NULL : &read_ok);
        note(81, (uint64_t)(int64_t)rc);
        note(82, repaired * 2u + read_ok);
    } else {
        unsigned bad = 99, fixed = 99;
        unsigned repair = pick(2);
        unsigned nulls = pick(4);
        unsigned fi = pick(12);
        g_fail_in = fi == 0 ? 0 : -1;
        int rc = cfs_verify_all(fs, CFS_DVA(v, 9), buf, verify_first, &want, repair != 0,
                                nulls == 0 ? NULL : &bad, nulls == 0 ? NULL : &fixed);
        g_fail_in = -1;
        note(83, (uint64_t)(int64_t)rc);
        note(84, bad * 100u + fixed);
        if (rc == 0)
            note(85, buf[0]);
    }
}

static void finds(void)
{
    struct cfs *fs = g_fs;
    unsigned what = pick(5);
    if (what == 0 && fs->mem != NULL) {
        unsigned v = pick(fs->nmembers);
        bool exact = false;
        unsigned s = pick(3);
        struct blkdev *bd = cfs_find_lagging(fs, v, s == 0 ? 0 : fs->sb.generation - 1 - pick(3), &exact);
        note(90, bd ? (uint64_t)dev_index(bd) : 99);
        note(91, exact);
        if (bd)
            blkdev_put(bd);
    } else if (what == 1) {
        unsigned f = pick(3);
        unsigned t = pick(5);
        struct blkdev *bd = cfs_find_tagged(fs, f == 0 ? CFS_LABEL_SPARE : CFS_LABEL_RESILVER, t);
        note(92, bd ? (uint64_t)dev_index(bd) : 99);
        if (bd)
            blkdev_put(bd);
    } else if (what == 2) {
        cfs_spares_find(fs);
    } else if (what == 3) {
        uint64_t newer = 0xdead;
        unsigned self = pick(3);
        struct blkdev *bd = cfs_newer_member0(self == 0 ? &g_dev[0] : NULL, fs->sb.uuid, fs->sb.generation - pick(3),
                                              &newer);
        note(93, bd ? (uint64_t)dev_index(bd) : 99);
        note(94, newer);
        if (bd)
            blkdev_put(bd);
    } else {
        unsigned d = pick(g_ndev);
        unsigned fi = pick(8);
        g_fail_in = fi == 0 ? 0 : -1;
        cfs_label_clear_dev(pick(10) == 0 ? NULL : &g_dev[d]);
        g_fail_in = -1;
        mix(g_img[d], sizeof(g_img[d]));
    }
}

static void labels(void)
{
    struct cfs *fs = g_fs;
    unsigned what = pick(3);
    unsigned fi = pick(10);
    g_fail_in = fi == 0 ? 0 : -1;
    if (what == 0 && g_pool.nmembers > 0) {
        unsigned v = pick(g_pool.nmembers);
        unsigned c = pick(g_pool.m[v].ncopies);
        unsigned nomac = pick(3);
        int rc = cfs_label_write(&g_pool, nomac == 0 ? NULL : &fs->mac, v, c, fs->gen, fs->sb.uuid, 500);
        note(95, (uint64_t)(int64_t)rc);
    } else if (what == 1) {
        unsigned d = 1 + pick(NDEV - 1);
        unsigned f = pick(3);
        unsigned v = pick(3);
        unsigned tag = pick(5);
        int rc = cfs_label_write_dev(fs, &g_dev[d], v, f == 0 ? CFS_LABEL_SPARE : f == 1 ? CFS_LABEL_RESILVER : 0, tag,
                                     700);
        note(96, (uint64_t)(int64_t)rc);
        mix(g_img[d], sizeof(g_img[d]));
    } else if (fs->mem != NULL) {
        note(97, (uint64_t)(int64_t)cfs_labels_update(fs));
    }
    g_fail_in = -1;
}

static void copystate(void)
{
    struct cfs *fs = g_fs;
    if (fs->mem == NULL)
        return;
    unsigned what = pick(5);
    if (what == 0) {
        unsigned saved = g_fail_pct;
        unsigned harsh = pick(3);
        if (harsh == 0)
            g_fail_pct = 50;   /* the allocator, the new block or the copy on write failing */
        note(100, (uint64_t)(int64_t)cfs_copystate_prepare(fs));
        g_fail_pct = saved;
    } else if (what == 1) {
        note(101, cfs_copystate_recorded(fs));
    } else if (what == 2) {
        note(102, (uint64_t)(int64_t)cfs_copystate_fill(fs));
    } else if (what == 3) {
        note(103, (uint64_t)(int64_t)cfs_members_store(fs));
        uint8_t *m = meta_at(fs->sb.members, false);
        if (m)
            mix(m, CFS_BLOCK);
    } else {
        /* A copy fails, or one comes back. */
        unsigned v = pick(g_pool.nmembers);
        unsigned c = pick(g_pool.m[v].ncopies);
        unsigned st = pick(2);
        g_pool.m[v].copy[c].state = st == 0 ? POOL_COPY_FAULTED : POOL_COPY_ONLINE;
    }
}

static void misc(void)
{
    struct cfs *fs = g_fs;
    unsigned what = pick(4);
    unsigned crowd = pick(4);
    if (what < 2 && crowd == 0) {
        /* More devices than a mount may hold: every member mirrored four ways, and spares. */
        for (unsigned v = 0; v < g_pool.nmembers; v++) {
            for (unsigned c = g_pool.m[v].ncopies; c < POOL_MAX_COPIES; c++) {
                clear_copy(v, c);
                g_pool.m[v].copy[c].dev = ref(&g_dev[1 + pick(NDEV - 1)]);
                g_pool.m[v].copy[c].state = POOL_COPY_ONLINE;
            }
            g_pool.m[v].ncopies = POOL_MAX_COPIES;
        }
        while (g_pool.nmembers < MAXMEM) {
            unsigned v = g_pool.nmembers++;
            memset(&g_pool.m[v], 0, sizeof(g_pool.m[v]));
            for (unsigned c = 0; c < POOL_MAX_COPIES; c++) {
                clear_copy(v, c);
                g_pool.m[v].copy[c].dev = ref(&g_dev[1 + pick(NDEV - 1)]);
            }
            g_pool.m[v].ncopies = POOL_MAX_COPIES;
        }
        while (g_pool.nspares < POOL_MAX_SPARES) {
            g_pool.spare[g_pool.nspares] = ref(&g_dev[pick(NDEV)]);
            g_pool.spare_tag[g_pool.nspares++] = 9;
        }
    }
    if (what == 0) {
        cfs_holds_sync(fs);
    } else if (what == 1) {
        unsigned nomnt = pick(6);
        fs->mnt = nomnt == 0 ? NULL : &g_mnt;
        unsigned extra = pick(3);
        note(110, (uint64_t)(int64_t)cfs_holds_sync_with(fs, extra == 0 ? NULL : &g_dev[pick(NDEV)]));
        fs->mnt = &g_mnt;
    } else if (what == 2 && fs->mem != NULL) {
        note(111, cfs_copies_out(fs));
    } else if (fs->mem != NULL) {
        for (unsigned k = 0; k < 6; k++) {
            uint64_t lin = next() % (fs->nblocks + 70000);
            note(112, cfs_lin_dva(fs, lin));
        }
    }
}

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    harness_klog_hook = on_klog;
    harness_klog_min = KLOG_ERROR + 1;   /* the refusals are expected, by the thousand */
    for (unsigned i = 0; i < NDEV; i++)
        snprintf(g_dev[i].name, sizeof(g_dev[i].name), "d%u", i);
    for (unsigned u = 0; u < 3; u++)
        for (unsigned i = 0; i < 16; i++)
            g_uuid[u][i] = (uint8_t)next();
    g_fs = calloc(1, sizeof(*g_fs));
    EXPECT(g_fs != NULL);
    g_mnt.fs_priv = g_fs;
    for (unsigned round = 0; round < ROUNDS; round++) {
        build();
        unsigned f = pick(3);
        g_fail_pct = f == 0 ? 0 : f == 1 ? 3 : 15;
        unsigned fi = pick(10);
        g_fail_in = fi == 0 ? (int)pick(3) : -1;
        int rc = cfs_members_load(g_fs);
        g_fail_in = -1;
        note(120, (uint64_t)(int64_t)rc);
        dump();
        unsigned again = pick(5);
        if (rc == 0 && again == 0) {
            /* The older root's load, after the newer one's table was let go: the members already assembled. */
            cfs_members_free(g_fs);
            note(121, (uint64_t)(int64_t)cfs_members_load(g_fs));
            dump();
        }
        unsigned ops = 1 + pick(8);
        for (unsigned k = 0; k < ops; k++) {
            unsigned what = pick(5);
            if (what == 0)
                reads();
            else if (what == 1)
                finds();
            else if (what == 2)
                labels();
            else if (what == 3)
                copystate();
            else
                misc();
            EXPECT(g_handles == 0 && g_bufs == 0);
        }
        dump();
        teardown();
        EXPECT(g_live == 0);
    }
    free(g_fs);
    EXPECT(g_live == 0);
    harness_klog_hook = NULL;
    harness_klog_min = KLOG_WARN;
    printf("  member digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, ROUNDS);
    EXPECT(g_digest == MEMBER_DIGEST);
}

int main(void)
{
    static const struct host_test tests[] = {
        { "member_digest", test_digest },
    };
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
