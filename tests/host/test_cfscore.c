/*
 * test_cfscore.c - cosmofs's mount and unmount (kernel-services/filesystem/cosmofs/cosmofs_core.c) on the host:
 * roadmap 2.0 SA7. A digest recorded from the C over mounts of pools the test makes: two superblock slots of every
 * shape (missing, not a superblock, of a version this kernel does not read, refusing to authenticate, locked, another
 * pool's key), a device behind a newer member 0 that is or is not mounted, pools in the clear, version-1 encrypted
 * with a key from the firmware or none, version-14 with the key table's key and a key block that is or is not the
 * pool's, bitmaps across several members, a rollback switch that fails, a root that does not load and the older slot
 * it falls back to, a root inode that is not a directory -- and then, on what mounted, commits from the VFS, the
 * writeback thread and a pool command, dirty pages written back, statfs, bitmap claims, copy state changes and the
 * unmount. Every stage fails now and then, as does kmalloc. Every call, argument, return, logged line and the figures
 * after are in the digest; nothing is left allocated, no pool is left open and every reference is put.
 */

#include "harness.h"

#include <kernel/blk.h>
#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/lockdep.h>
#include <kernel/log.h>
#include <kernel/mutex.h>
#include <kernel/pagecache.h>
#include <kernel/storage.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/vfs.h>
#include <uapi/cosmo/fsctl.h>

#include "cosmofs_internal.h"   /* before the C library's headers, as test_cfsparse.c: its signal.h is the kernel's */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The digest of the C (cosmofs_core.c, 2026-11-07), which the CeeSafe port gives. */
#define CORE_DIGEST 0x25fe84ab2d7cdbbfull
#define ROUNDS 6000u
#define SEED 0xC05E0F5A2026C0DEull

extern struct fs_type cosmofs_fs_type;

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

size_t (strlcpy)(char *dst, const char *src, size_t size)   /* parenthesised: a fortified libc makes strlcpy a macro */
{
    size_t n = strlen(src);
    if (size > 0) {
        size_t k = n < size - 1 ? n : size - 1;
        memcpy(dst, src, k);
        dst[k] = '\0';
    }
    return n;
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

void *kzalloc(size_t size) { return kmalloc(size, KMEM_ZERO); }

void kfree(void *p)
{
    if (p != NULL)
        g_live--;
    free(p);
}

/* ---- time, locks and threads ---- */

static uint64_t g_now = 1;

uint64_t clock_now_ns(void)
{
    g_now += 1000;
    return g_now;
}

uint64_t clock_since_ns(uint64_t stamp) { return g_now > stamp ? g_now - stamp : 0; }

static struct cfs *g_fs;        /* the mounted filesystem, or NULL */
static int g_mutexes;           /* held */

void mutex_init(struct mutex *m, const char *name)
{
    (void)m;
    note(3, strlen(name));
}

void mutex_lock(struct mutex *m)
{
    (void)m;
    g_mutexes++;
    note(4, 0);
}

bool mutex_trylock(struct mutex *m)
{
    (void)m;
    bool ok = pick(5) != 0;
    note(5, ok);
    if (ok)
        g_mutexes++;
    return ok;
}

void mutex_unlock(struct mutex *m)
{
    (void)m;
    EXPECT(g_mutexes > 0);
    g_mutexes--;
    note(6, 0);
}

bool lockdep_is_held(const void *lock, unsigned kind)
{
    (void)lock;
    note(7, kind);
    return g_mutexes > 0;
}

static struct thread *g_thread;           /* the writeback thread, made and not joined */
static void (*g_entry)(void *);
static void *g_arg;
static unsigned g_sleeps;                 /* before the thread is told to stop */
static bool g_thread_fail;

struct thread *thread_create(void (*entry)(void *arg), void *arg, const char *name, int priority)
{
    note(8, (uint64_t)priority);
    mix(name, strlen(name));
    EXPECT(g_thread == NULL);
    if (g_thread_fail && pick(2) == 0) {
        note(9, 0);
        return NULL;
    }
    static char t;
    g_thread = (struct thread *)&t;
    g_entry = entry;
    g_arg = arg;
    return g_thread;
}

int thread_join(struct thread *t)
{
    EXPECT(t == g_thread && g_fs != NULL);
    EXPECT(__atomic_load_n(&g_fs->wb_stop, __ATOMIC_ACQUIRE));
    note(10, 0);
    g_thread = NULL;
    return 0;
}

void thread_sleep_ns(uint64_t ns)
{
    note(11, ns);
    g_now += ns;
    if (g_sleeps == 0 || --g_sleeps == 0)
        __atomic_store_n(&g_fs->wb_stop, true, __ATOMIC_RELEASE);
}

/* ---- devices, kobjects and vnodes ---- */

#define NDEV 4u
#define NVN 12u

static struct blkdev g_dev[NDEV];
static int g_dref[NDEV];
static struct vnode g_vn[NVN];
static int g_vref[NVN];
static bool g_vhashed[NVN];

void kobject_get(struct kobject *o)
{
    for (unsigned i = 0; i < NVN; i++)
        if (o == &g_vn[i].obj) {
            g_vref[i]++;
            note(12, i);
            return;
        }
    EXPECT(false);
}

void kobject_put(struct kobject *o)
{
    for (unsigned i = 0; i < NDEV; i++)
        if (o == &g_dev[i].obj) {
            EXPECT(g_dref[i] > 0);
            g_dref[i]--;
            note(13, i);
            return;
        }
    EXPECT(false);
}

void vnode_put(struct vnode *vn)
{
    unsigned i = (unsigned)(vn - g_vn);
    EXPECT(i < NVN && g_vref[i] > 0);
    g_vref[i]--;
    note(14, i);
}

int pagecache_sync(struct vnode *vn)
{
    unsigned i = (unsigned)(vn - g_vn);
    EXPECT(i < NVN && g_mutexes > 0);
    note(15, i);
    if (chance(6)) {
        note(16, 0);
        return -EIO;
    }
    vn->pc.nr_dirty = 0;
    return 0;
}

static unsigned g_mounted_flags;   /* which devices vfs_bdev_mounted says are */

bool vfs_bdev_mounted(const struct blkdev *bd)
{
    unsigned i = (unsigned)(bd - g_dev);
    EXPECT(i < NDEV);
    note(17, i);
    return (g_mounted_flags >> i) & 1;
}

static struct fs_type *g_registered;
static bool g_register_fails;

int vfs_register_fs(struct fs_type *fs)
{
    mix(fs->name, strlen(fs->name));
    if (g_register_fails)
        return -EBUSY;
    g_registered = fs;
    return 0;
}

static unsigned g_fwkey;   /* 0 none, 1 empty, 2 a key */

bool fwcfg_get_string(const char *key, char *buf, size_t len)
{
    mix(key, strlen(key));
    note(18, len);
    if (g_fwkey == 0)
        return false;
    snprintf(buf, len, "%s", g_fwkey == 1 ? "" : "opensesame");
    return true;
}

/* ---- the pool: superblock images per device ---- */

static uint8_t g_img[NDEV][2][CFS_BLOCK];
static bool g_img_bad[NDEV][2];          /* the read fails */
static uint64_t g_dev0[NDEV];
static int g_pools;
static bool g_degraded;

/* What the seal and shape models read, carried in the superblock's crc (which nothing else here reads). */
#define CTL_SHAPE_BAD  0x1u
#define CTL_EIO        0x2u
#define CTL_ENOKEY     0x4u
#define CTL_MAC        0x8u
#define CTL_NO_PEEK    0x10u

int pool_open(struct blkdev *bd, struct spool **out)
{
    unsigned i = (unsigned)(bd - g_dev);
    EXPECT(i < NDEV);
    note(20, i);
    if (chance(12)) {
        note(21, 0);
        return -EIO;
    }
    struct spool *p = calloc(1, sizeof(*p));
    struct spool_member *m = calloc(1, sizeof(*m));
    EXPECT(p != NULL && m != NULL);
    m->nblocks = g_dev0[i];
    m->copy[0].dev = bd;
    p->m = m;
    p->nmembers = 1;
    p->state_seq = next() & 0xFF;
    g_pools++;
    *out = p;
    return 0;
}

void pool_close(struct spool *p)
{
    EXPECT(g_pools > 0);
    g_pools--;
    note(22, p->state_seq);
    free(p->m);
    free(p);
}

int pool_read(struct spool *p, uint64_t dva, void *buf)
{
    unsigned i = (unsigned)(p->m[0].copy[0].dev - g_dev);
    note(23, dva);
    EXPECT(i < NDEV && dva < 2);
    if (g_img_bad[i][dva])
        return -EIO;
    memcpy(buf, g_img[i][dva], CFS_BLOCK);
    return 0;
}

void pool_commit_done(struct spool *p, uint64_t gen)
{
    (void)p;
    note(24, gen);
}

bool pool_degraded(const struct spool *p)
{
    (void)p;
    note(25, g_degraded);
    return g_degraded;
}

/* ---- the superblock's checks ---- */

static uint32_t ctl(const struct cfs_super *sb) { return sb->crc; }

bool cfs_super_shape_ok(const struct cfs_super *sb, uint64_t dev0)
{
    note(26, dev0);
    return memcmp(sb->magic, CFS_MAGIC, 8) == 0 && sb->version >= CFS_VERSION_MIN && sb->version <= CFS_VERSION &&
           !(ctl(sb) & CTL_SHAPE_BAD);
}

int cfs_super_verify(const void *block, struct cfs_mac *mac)
{
    const struct cfs_super *sb = block;
    note(27, sb->generation);
    memset(mac, 0, sizeof(*mac));
    if (ctl(sb) & CTL_EIO)
        return -EIO;
    if (ctl(sb) & CTL_ENOKEY)
        return -ENOKEY;
    mac->on = (ctl(sb) & CTL_MAC) != 0;
    if (mac->on)
        memset(mac->key, (int)(ctl(sb) >> 8) & 0xFF, sizeof(mac->key));
    return 0;
}

bool cfs_super_peek_ok(const void *block)
{
    const struct cfs_super *sb = block;
    note(28, sb->generation);
    return !(ctl(sb) & CTL_NO_PEEK) && memcmp(sb->magic, CFS_MAGIC, 8) == 0;
}

static unsigned g_newer;   /* NDEV for none */
static uint64_t g_newer_gen;

struct blkdev *cfs_newer_member0(struct blkdev *bd, const uint8_t *uuid, uint64_t gen, uint64_t *newer_gen)
{
    note(29, (uint64_t)(bd - g_dev));
    mix(uuid, 16);
    note(30, gen);
    if (g_newer >= NDEV)
        return NULL;
    g_dref[g_newer]++;
    *newer_gen = g_newer_gen;
    return &g_dev[g_newer];
}

/* ---- the members, the key, the buffers and the replays ---- */

static unsigned g_members;
static bool g_full;   /* every bitmap block all ones, and the superblock agreeing that nothing is free */
static bool g_bad_keyblock;

int cfs_members_load(struct cfs *fs)
{
    note(31, fs->sb.generation);
    if (chance(15)) {
        note(32, 0);
        return -EIO;
    }
    EXPECT(fs->mem == NULL);
    fs->nmembers = g_members;
    fs->mem = kzalloc(g_members * sizeof(*fs->mem));
    if (fs->mem == NULL)
        return -ENOMEM;
    uint64_t base = 0;
    unsigned chunk = 0;
    for (unsigned v = 0; v < g_members; v++) {
        struct cfs_memstate *m = &fs->mem[v];
        m->nblocks = 100 + pick(2 * CFS_BITS_PER_BITMAP);
        m->first_usable = v == 0 ? 2 : 1;
        m->nchunks = (unsigned)((m->nblocks + CFS_BITS_PER_BITMAP - 1) / CFS_BITS_PER_BITMAP);
        m->chunk0 = chunk;
        m->base = base;
        m->alloc_root = CFS_DVA(v, 3);
        chunk += m->nchunks;
        base += (uint64_t)m->nchunks * CFS_BITS_PER_BITMAP;
    }
    fs->nr_chunks = chunk;
    if (g_full)
        fs->sb.free_blocks = 0;
    fs->nblocks = base;   /* whole chunks per member (cfs_members_load) */
    return 0;
}

void cfs_members_free(struct cfs *fs)
{
    note(33, fs->nmembers);
    kfree(fs->mem);
    fs->mem = NULL;
    fs->nmembers = 0;
}

int cfs_keytab_take(const uint8_t *uuid, bool hold, uint8_t *master)
{
    mix(uuid, 16);
    note(34, hold);
    if (chance(6)) {
        note(35, 0);
        return -ENOKEY;
    }
    memset(master, 0x3C, CHACHA20_KEY_SIZE);
    return 0;
}

void cfs_keytab_release(const uint8_t *uuid)
{
    mix(uuid, 16);
    note(36, 0);
}

int cfs_keys_load(struct cfs *fs, const void *user_key, size_t user_len)
{
    note(37, user_len);
    mix(user_key, user_len);
    unsigned w = pick(5);
    if (w == 0)
        return -EKEYREJECTED;
    if (w == 1)
        return -EAGAIN;
    if (w == 2)
        return -EIO;
    fs->have_key = true;
    return 0;
}

static int g_bufs;   /* held */

int cfs_buf_get(struct cfs *fs, uint64_t blkno, uint32_t kind, struct cfs_buf **out)
{
    note(38, blkno);
    note(39, kind);
    if (chance(40)) {
        note(40, 0);
        return -EIO;
    }
    struct cfs_buf *b = kmalloc(sizeof(*b), KMEM_ZERO);
    if (b == NULL)
        return -ENOMEM;
    b->data = kmalloc(CFS_BLOCK, 0);
    if (b->data == NULL) {
        kfree(b);
        return -ENOMEM;
    }
    b->blkno = blkno;
    b->refs = 1;
    if (kind == CFS_KIND_KEYS) {
        memset(b->data, 0, CFS_BLOCK);
        struct cfs_keys2 *k = (struct cfs_keys2 *)(b->data + CFS_MHDR_SIZE);
        k->version = g_bad_keyblock && pick(2) ? 1 : 2;
        memcpy(k->uuid, fs->sb.uuid, 16);
        if (g_bad_keyblock && pick(2))
            k->uuid[3] ^= 1;
    } else if (kind == CFS_KIND_ALLOCIDX) {
        memset(b->data, 0, CFS_BLOCK);
        uint64_t *p = (uint64_t *)(b->data + CFS_MHDR_SIZE);
        for (unsigned c = 0; c < 8; c++)
            p[c] = blkno + 1 + c;
    } else if (kind == CFS_KIND_BITMAP && g_full) {
        memset(b->data, 0xFF, CFS_BLOCK);
    } else {
        for (unsigned i = 0; i < CFS_BLOCK; i += 8) {
            uint64_t v = next();
            memcpy(b->data + i, &v, 8);
        }
    }
    list_push_front(&fs->bufs, &b->link);
    fs->nr_bufs++;
    g_bufs++;
    *out = b;
    return 0;
}

void cfs_buf_put(struct cfs *fs, struct cfs_buf *b)
{
    (void)fs;
    EXPECT(g_bufs > 0 && b->refs > 0);
    b->refs--;
    g_bufs--;
    note(41, b->blkno);
}

int cfs_freelog_replay(struct cfs *fs)
{
    note(42, fs->sb.free_root);
    if (chance(15)) {
        note(43, 0);
        return -EIO;
    }
    if (pick(3) == 0) {
        fs->replay_freed = 1 + pick(5);
        cfs_note_dirty(fs);
    }
    return 0;
}

int cfs_snapshot_list(struct cfs *fs, struct cfs_snapshot *out, unsigned max, unsigned *count)
{
    (void)fs;
    EXPECT(out == NULL && max == 0);
    *count = pick(4);
    unsigned w = pick(10);
    note(44, w);
    return w == 0 ? -EIO : 0;
}

int cfs_clones_load(struct cfs *fs)
{
    note(45, fs->sb.generation);
    if (chance(15)) {
        note(46, 0);
        return -EIO;
    }
    fs->nclones = pick(3);
    return 0;
}

void cfs_clones_free(struct cfs *fs)
{
    note(47, fs->nclones);
    fs->nclones = 0;
}

int cfs_rollback_at_mount(struct cfs *fs, bool *switched)
{
    note(48, fs->sb.rollback_root);
    unsigned w = pick(4);
    if (w == 0) {
        note(49, 0);
        return -EIO;
    }
    *switched = w == 1;
    if (*switched)
        fs->sb.generation += 10;
    return 0;
}

static unsigned g_root_mode;

int cfs_inode_read(struct cfs *fs, uint64_t ino, struct cfs_inode *out)
{
    (void)fs;
    note(50, ino);
    if (chance(10))
        return -EIO;
    memset(out, 0, sizeof(*out));
    out->mode = g_root_mode;
    return 0;
}

int cfs_orphan_replay(struct cfs *fs)
{
    note(51, fs->sb.orphan_root);
    if (chance(15)) {
        note(52, 0);
        return -EIO;
    }
    if (pick(3) == 0) {
        fs->replay_orphans = 1 + pick(5);
        cfs_note_dirty(fs);
    }
    return 0;
}

int cfs_vnode_get(struct cfs *fs, uint64_t ino, struct vnode **out)
{
    (void)fs;
    note(53, ino);
    if (chance(15)) {
        note(54, 0);
        return -ENOMEM;
    }
    *out = &g_vn[0];
    return 0;
}

void cfs_holds_sync(struct cfs *fs) { note(55, fs->sb.generation); }

void cfs_resilver_mount(struct cfs *fs) { note(56, fs->nmembers); }

void cfs_resilver_stop(struct cfs *fs) { note(57, fs->nmembers); }

static bool g_publish;

int cfs_commit_ex(struct cfs *fs, bool *published)
{
    EXPECT(g_mutexes > 0);
    note(58, fs->gen);
    if (chance(6)) {
        note(59, 0);
        return -EIO;
    }
    *published = g_publish;
    if (g_publish) {
        fs->sb.generation = fs->gen++;
        fs->commits++;
        fs->first_dirty_ns = 0;
    }
    return 0;
}

uint64_t cfs_lin_dva(const struct cfs *fs, uint64_t lin)
{
    for (unsigned v = 0; v < fs->nmembers; v++) {
        const struct cfs_memstate *m = &fs->mem[v];
        if (lin >= m->base && lin < m->base + m->nblocks)
            return CFS_DVA(v, lin - m->base);
    }
    return CFS_DVA_NONE;
}

/* ---- the maintenance passes the fs_type names: never reached here ---- */

int cosmofs_check(struct mount *mnt, struct cosmofs_check_report *out, unsigned flags)
{
    (void)mnt, (void)out, (void)flags;
    EXPECT(false);
    return -EIO;
}

int cosmofs_check_dev(struct blkdev *bd, unsigned flags, struct cosmofs_check_report *rep,
                      struct cosmofs_dev_report *dev)
{
    (void)bd, (void)flags, (void)rep, (void)dev;
    EXPECT(false);
    return -EIO;
}

int cosmofs_scrub(struct mount *mnt, struct cosmofs_scrub_stats *out, unsigned flags)
{
    (void)mnt, (void)out, (void)flags;
    EXPECT(false);
    return -EIO;
}

int cosmofs_snapctl(struct mount *mnt, struct cosmofs_snap_req *req)
{
    (void)mnt, (void)req;
    EXPECT(false);
    return -EIO;
}

int cosmofs_poolctl(struct mount *mnt, struct cosmofs_pool_req *req)
{
    (void)mnt, (void)req;
    EXPECT(false);
    return -EIO;
}

int cosmofs_keyctl(struct blkdev *bd, struct mount *mnt, struct cosmofs_key_req *req)
{
    (void)bd, (void)mnt, (void)req;
    EXPECT(false);
    return -EIO;
}

int cosmofs_format_label(struct blkdev *bd, const char *label)
{
    (void)bd, (void)label;
    EXPECT(false);
    return -EIO;
}

int cfs_format_mirror_devs(struct blkdev **bd, unsigned n, const char *label)
{
    (void)bd, (void)n, (void)label;
    EXPECT(false);
    return -EIO;
}

int cosmofs_format_keys(struct blkdev **bd, unsigned n, const char *label, const uint8_t *uuid, const uint8_t *master,
                        const struct cosmo_keyslot *slots)
{
    (void)bd, (void)n, (void)label, (void)uuid, (void)master, (void)slots;
    EXPECT(false);
    return -EIO;
}

/* ---- a round ---- */

static struct mount g_mnt;

static void superblock(unsigned d, unsigned slot, const uint8_t *uuid, uint64_t gen)
{
    uint8_t *img = g_img[d][slot];
    memset(img, 0, CFS_BLOCK);
    struct cfs_super *sb = (struct cfs_super *)img;
    unsigned shape = pick(12);
    if (shape == 0) {
        for (unsigned i = 0; i < CFS_BLOCK; i += 8) {
            uint64_t v = next();
            memcpy(img + i, &v, 8);
        }
        return;
    }
    memcpy(sb->magic, CFS_MAGIC, 8);
    unsigned vs = pick(10);
    sb->version = vs == 0 ? 0 : vs == 1 ? CFS_VERSION + 1 : vs == 2 ? 1 : 2 + pick(14);
    sb->generation = gen;
    sb->total_blocks = 100 + pick(100000);
    sb->free_blocks = pick(1000);
    sb->inode_count = pick(1000);
    memcpy(sb->uuid, uuid, 16);
    sb->key_root = pick(3) ? CFS_DVA(0, 9) : 0;
    sb->rollback_root = pick(4) == 0 ? CFS_DVA(0, 11) : 0;
    sb->free_root = pick(2) ? CFS_DVA(0, 12) : 0;
    sb->orphan_root = pick(2) ? CFS_DVA(0, 13) : 0;
    unsigned lab = pick(3);
    if (lab == 1)
        memcpy(sb->label, "pool-label", 10);
    else if (lab == 2)
        memcpy(sb->label, "sixteen-bytes-ok", 16);
    uint32_t c = 0;
    if (pick(10) == 0)
        c |= CTL_SHAPE_BAD;
    unsigned v = pick(12);
    if (v == 0)
        c |= CTL_EIO;
    else if (v == 1)
        c |= CTL_ENOKEY;
    if (pick(3) == 0)
        c |= CTL_MAC | (uint32_t)(pick(4) == 0 ? 0x77 : 0x55) << 8;
    if (pick(6) == 0)
        c |= CTL_NO_PEEK;
    sb->crc = c;
}

static void devices(void)
{
    uint8_t uuid[16];
    for (unsigned i = 0; i < 16; i++)
        uuid[i] = (uint8_t)next();
    for (unsigned d = 0; d < NDEV; d++) {
        g_dev0[d] = 64 + pick(100000);
        for (unsigned s = 0; s < 2; s++) {
            uint64_t gen = 1 + pick(50);
            superblock(d, s, uuid, gen);
            g_img_bad[d][s] = pick(10) == 0;
        }
    }
}

static void vnodes(void)
{
    for (unsigned b = 0; b < VNODE_HASH; b++)
        list_init(&g_mnt.vnodes[b]);
    for (unsigned i = 0; i < NVN; i++) {
        memset(&g_vn[i], 0, sizeof(g_vn[i]));
        g_vref[i] = 0;
        g_vhashed[i] = pick(3) != 0;
        if (!g_vhashed[i])
            continue;
        g_vn[i].type = pick(3) ? VNODE_REG : VNODE_DIR;
        g_vn[i].pc.nr_dirty = pick(2) ? pick(5) : 0;
        list_push_front(&g_mnt.vnodes[pick(VNODE_HASH)], &g_vn[i].hash_link);
    }
}

static void mount_one(void)
{
    unsigned d = pick(NDEV);
    unsigned fl = pick(6);
    unsigned flags = fl == 0 ? MOUNT_RDONLY : fl == 1 ? MOUNT_DETACHED : 0;
    unsigned nw = pick(4);
    g_newer = nw == 0 ? pick(NDEV) : NDEV;
    g_newer_gen = 60 + pick(10);
    g_mounted_flags = pick(4) == 0 ? (1u << g_newer) : 0;
    g_members = 1 + pick(3);
    g_root_mode = CFS_MODE(pick(12) == 0 ? CFS_TYPE_REG : CFS_TYPE_DIR, 0755);
    g_fwkey = pick(3);
    g_bad_keyblock = pick(6) == 0;
    g_full = pick(5) == 0;
    unsigned kf = pick(8);
    g_fail_in = g_faults && kf == 0 ? (int)pick(12) : -1;
    note(70, d);
    note(71, flags);
    int rc = cosmofs_fs_type.mount(&cosmofs_fs_type, pick(40) == 0 ? NULL : &g_dev[d], flags, &g_mnt);
    g_fail_in = -1;
    note(72, (uint64_t)(int64_t)rc);
    if (rc == 0) {
        g_fs = g_mnt.fs_priv;
        EXPECT(g_fs != NULL && g_mnt.root == &g_vn[0]);
        note(73, g_fs->sb.generation);
        note(74, g_fs->sb_slot);
        note(75, g_fs->free_blocks);
        note(76, g_fs->reserve);
        note(77, g_fs->fell_back);
        note(78, g_fs->rollback_switched);
        note(79, (uint64_t)(int64_t)g_fs->rollback_error);
        note(80, g_fs->have_key);
        note(81, g_fs->encrypted);
        note(82, g_fs->snap_count);
        note(83, g_mnt.state);
        mix(g_fs->pool_name, strlen(g_fs->pool_name));
        mix(g_fs->bitmap, (size_t)((g_fs->nblocks + 7) / 8));
    } else if (pick(3) != 0) {   /* not always asked: a second failure of the device replaces the first */
        EXPECT(g_mnt.fs_priv == NULL);
        unsigned stage = 99;
        int why = 0;
        bool mine = cfs_mount_failure(&g_dev[d], &stage, &why);
        note(84, mine);
        note(85, stage);
        note(86, (uint64_t)(int64_t)why);
    }
}

static void run_thread(void)
{
    if (g_thread == NULL)
        return;
    g_sleeps = 1 + pick(6);
    g_mnt.unmounted = pick(8) == 0;
    g_entry(g_arg);
    g_mnt.unmounted = false;
    EXPECT(__atomic_load_n(&g_fs->wb_stop, __ATOMIC_ACQUIRE));
}

static void live_op(void)
{
    unsigned w = pick(10);
    note(90, w);
    int rc = 0;
    switch (w) {
    case 0:
        rc = cosmofs_fs_type.sync(&g_mnt);
        break;
    case 1:
        g_mnt.unmounted = pick(6) == 0;
        rc = cosmofs_commit_mount(&g_mnt);
        g_mnt.unmounted = false;
        break;
    case 2: {
        struct vfs_statfs st;
        memset(&st, 0, sizeof(st));
        rc = cosmofs_fs_type.statfs(&g_mnt, &st);
        mix(&st, sizeof(st));
        break;
    }
    case 3: {
        uint64_t lin = pick(4) == 0 ? g_fs->nblocks + pick(3) : pick((unsigned)g_fs->nblocks + 1);
        if (pick(3) == 0) {
            g_fs->free_blocks = 0;
            for (unsigned v = 0; v < g_fs->nmembers; v++)
                g_fs->mem[v].free_blocks = 0;
        }
        cfs_bitmap_claim(g_fs, lin);
        break;
    }
    case 4:
        cfs_note_dirty(g_fs);
        break;
    case 5:
        rc = cfs_sync_vnodes(g_fs);
        break;
    case 6:
        g_degraded = pick(2);
        g_fs->pool->notify(g_fs->pool->notify_arg);
        break;
    case 7:
        g_fs->first_dirty_ns = pick(2) ? 0 : g_now - pick(10) * 1000000000ull;
        g_fs->nr_dirty = pick(2) ? CFS_WB_DIRTY_BUFS : 0;
        g_fs->nr_pending = pick(2) ? CFS_WB_PENDING : 0;
        g_mnt.cache_dirty = pick(2) ? CFS_WB_DIRTY_PAGES : 0;
        g_fs->failed = pick(6) == 0 ? -EIO : 0;
        g_fs->wb_enabled = pick(4) != 0;
        run_thread();
        break;
    case 8:
        g_fs->discard_on_unmount = pick(4) == 0;
        break;
    default:
        g_fs->failed = pick(4) == 0 ? -EIO : 0;
        break;
    }
    note(91, (uint64_t)(int64_t)rc);
    note(92, g_fs->first_dirty_ns != 0);
    note(93, g_fs->free_blocks);
    note(94, g_fs->wb_commits);
    note(95, g_fs->mount_dirty_notes);
    note(96, g_fs->wb_early);
    note(97, g_thread != NULL);
    EXPECT(g_mutexes == 0 && g_bufs == 0);
}

static void side_op(void)
{
    unsigned w = pick(4);
    note(100, w);
    unsigned d = pick(NDEV);
    if (w == 0) {
        struct cfs_super a, b;
        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        int ra = 7, rb = 7;
        unsigned kf = pick(6);
        g_fail_in = g_faults && kf == 0 ? (int)pick(3) : -1;
        int rc = cfs_super_slots(&g_dev[d], &a, &ra, &b, &rb);
        g_fail_in = -1;
        note(101, (uint64_t)(int64_t)rc);
        note(102, (uint64_t)(int64_t)ra);
        note(103, (uint64_t)(int64_t)rb);
        mix(&a, sizeof(a));
        mix(&b, sizeof(b));
    } else if (w == 1) {
        struct spool *p = NULL;
        if (pool_open(&g_dev[d], &p) != 0)
            return;
        struct cfs_super out;
        memset(&out, 0, sizeof(out));
        unsigned kf = pick(6);
        g_fail_in = g_faults && kf == 0 ? 0 : -1;
        int rc = cfs_super_peek(p, &out);
        g_fail_in = -1;
        note(104, (uint64_t)(int64_t)rc);
        mix(&out, sizeof(out));
        pool_close(p);
    } else if (w == 2) {
        note(108, (uint64_t)(int64_t)cosmofs_fs_type.sync(&g_mnt));   /* nothing mounted */
    } else {
        unsigned stage = 99;
        int why = 0;
        bool mine = cfs_mount_failure(&g_dev[d], &stage, &why);
        note(105, mine);
        note(106, stage);
        note(107, (uint64_t)(int64_t)why);
    }
}

static void unmount(void)
{
    if (g_fs == NULL)
        return;
    int rc = cosmofs_fs_type.sync(&g_mnt);
    note(110, (uint64_t)(int64_t)rc);
    g_mnt.unmounted = true;
    rc = cosmofs_fs_type.unmount(&g_mnt);
    note(111, (uint64_t)(int64_t)rc);
    EXPECT(g_mnt.fs_priv == NULL);
    EXPECT(g_thread == NULL);
    g_fs = NULL;
    g_mnt.unmounted = false;
}

static void round_(void)
{
    memset(&g_mnt, 0, sizeof(g_mnt));
    vnodes();
    devices();
    g_thread_fail = g_faults && pick(4) == 0;
    g_publish = pick(4) != 0;
    g_degraded = pick(4) == 0;
    unsigned sides = pick(3);
    for (unsigned i = 0; i < sides; i++)
        side_op();
    mount_one();
    if (g_fs != NULL) {
        unsigned ops = pick(8);
        for (unsigned i = 0; i < ops; i++)
            live_op();
        unmount();
    }
    for (unsigned i = 0; i < NVN; i++)
        EXPECT(g_vref[i] == 0);
    for (unsigned i = 0; i < NDEV; i++)
        EXPECT(g_dref[i] == 0);
    EXPECT(g_pools == 0);
    EXPECT(g_live == 0);
    EXPECT(g_mutexes == 0 && g_bufs == 0);
}

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    harness_klog_hook = on_klog;
    harness_klog_min = KLOG_ERROR + 1;
    for (unsigned i = 0; i < NDEV; i++)
        snprintf(g_dev[i].name, sizeof(g_dev[i].name), "vd%c", 'a' + i);
    g_register_fails = true;
    EXPECT_PANIC(cosmofs_init());
    g_register_fails = false;
    cosmofs_init();
    EXPECT(g_registered == &cosmofs_fs_type);
    for (unsigned round = 0; round < ROUNDS; round++) {
        unsigned f = pick(3);
        g_faults = f != 0;
        round_();
    }
    harness_klog_hook = NULL;
    harness_klog_min = KLOG_WARN;
    printf("  core digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, ROUNDS);
    EXPECT(g_digest == CORE_DIGEST);
}

int main(void)
{
    static const struct host_test tests[] = {
        { "core_digest", test_digest },
    };
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
