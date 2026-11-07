/*
 * test_cfsdirblk.c - cosmofs's directory blocks, the .snapshots directory and the tree locks
 * (kernel-services/filesystem/cosmofs/cosmofs.cee; its floor holds the tables) on the host: roadmap 2.0 SA7. A
 * digest recorded from the C over directories of zero to several blocks on pools of versions 4 to 15, holding short
 * names, long names with their continuations, free runs and slots that do not check: names looked up (of every
 * length, some there, some not),
 * added (into a free run or a new block, or refused), removed and asked whether the directory is empty; the
 * .snapshots directory made or cached; snapshots looked up by name; and the read-only test and the tree lock for
 * vnodes of the live tree, a snapshot, a clone that exists and one that does not -- with the real entry decoders
 * (cfsparse), while the block reads and writes, the inode, the cache, the snapshot list and kmalloc fail now and
 * then. Every argument, return, block and vnode is in the digest; the lock is balanced and nothing is left allocated.
 */

#include "harness.h"

#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/mutex.h>
#include <kernel/vfs.h>

#include "cosmofs_internal.h"   /* before the C library's headers, as test_cfsparse.c: its signal.h is the kernel's */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The values the CeeSafe port writes as its own constants. */
_Static_assert(ENOENT == 2 && EIO == 5 && ENOMEM == 12 && ENOSPC == 28 && EROFS == 30 && ENAMETOOLONG == 36,
               "cosmofs's errno.h values");
_Static_assert(CFS_BLOCK == 4096 && CFS_ALLOC_META == 0 && CFS_NAME_MAX == 47 && CFS_NAME_MAX_LONG == 255 &&
                   CFS_DIRENTS_PER_BLOCK == 64 && sizeof(struct cfs_dirent) == 64 && CFS_DIRENT_CONT == ~0ull &&
                   CFS_DIRENT_HEAD_NAME == 48 && CFS_DIRENT_CONT_NAME == 56 && CFS_TYPE_DIR == 2 &&
                   CFS_ROOT_INO == 1 && CFS_SNAP_INO_SHIFT == 48 && CFS_SNAPDIR_INO == 0xFFFF000000000000ull &&
                   CFS_SNAP_ID_MAX == 0xFFFE && CFS_SNAP_NAME_MAX == 31 &&
                   CFS_MODE(CFS_TYPE_DIR, 0555) == 0x2000u + 365,
               "cosmofs's cosmofs_format.h values");
_Static_assert(offsetof(struct cfs_dirent, type) == 8 && offsetof(struct cfs_dirent, namelen) == 9 &&
                   offsetof(struct cfs_dirent, extra) == 10 && offsetof(struct cfs_dirent, name) == 16 &&
                   offsetof(struct cfs_dirent_cont, name) == 8,
               "cosmofs.cee's entry offsets");

/* The digest of the C (cosmofs.c, 2026-11-07), which the CeeSafe port gives. */
#define DIRBLK_DIGEST 0x79a7a841980074bbull
#define ROUNDS 8000u
#define SEED 0xD1B1C0C02026D1B1ull

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

void *kzalloc(size_t size)
{
    if (size == 0)
        return NULL;
    if (g_fail_in >= 0 && g_fail_in-- == 0) {
        note(2, size);
        return NULL;
    }
    void *p = calloc(1, size);
    EXPECT(p != NULL);
    g_live++;
    return p;
}

void kfree(void *p)
{
    if (p != NULL)
        g_live--;
    free(p);
}

/* ---- the lock and the clones ---- */

static int g_locked;
static struct cfs_clone g_clone;
static bool g_clone_exists;
static bool g_in_clone;

void mutex_lock(struct mutex *m)
{
    (void)m;
    EXPECT(g_locked == 0);
    g_locked = 1;
    note(3, 0);
}

void mutex_unlock(struct mutex *m)
{
    (void)m;
    EXPECT(g_locked == 1 && !g_in_clone);
    g_locked = 0;
    note(4, 0);
}

struct cfs_clone *cfs_clone_by_id(struct cfs *fs, uint64_t id)
{
    (void)fs;
    EXPECT(g_locked == 1);
    note(5, id);
    return g_clone_exists ? &g_clone : NULL;
}

void cfs_clone_enter(struct cfs *fs, struct cfs_clone *c)
{
    (void)fs;
    EXPECT(c == &g_clone && !g_in_clone);
    note(6, 0);
    g_in_clone = true;
}

void cfs_clone_leave(struct cfs *fs)
{
    (void)fs;
    EXPECT(g_locked == 1);
    note(7, g_in_clone);
    g_in_clone = false;
}

/* ---- the directory's blocks, the inode and the vnodes ---- */

#define NB 4u

static struct cfs g_fs;
static struct mount g_mnt;
static struct vnode g_dir;
static struct cfs_vnode g_dircv;
static uint8_t g_blk[NB + 1][CFS_BLOCK];

int cfs_dir_read_block_at(struct cfs *fs, const struct cfs_inode *dir, uint64_t lblk, uint8_t *buf)
{
    EXPECT(fs == &g_fs && dir == &g_dircv.inode);
    note(10, lblk);
    if (chance(25))
        return -EIO;
    if (lblk > NB)
        memset(buf, 0, CFS_BLOCK);
    else
        memcpy(buf, g_blk[lblk], CFS_BLOCK);
    return 0;
}

int cfs_data_write_block(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, const void *buf,
                         enum cfs_alloc_class cls)
{
    EXPECT(fs == &g_fs && in == &g_dircv.inode);
    note(11, lblk);
    note(12, (uint64_t)cls);
    mix(buf, CFS_BLOCK);
    if (chance(15))
        return -ENOSPC;
    if (lblk <= NB)
        memcpy(g_blk[lblk], buf, CFS_BLOCK);
    return 0;
}

uint64_t vfs_now_ns(void) { return 4242; }

int cfs_vnode_inode_sync(struct cfs *fs, struct vnode *vn)
{
    EXPECT(fs == &g_fs);
    note(13, vn->size);
    note(14, vn->mtime_ns);
    return chance(15) ? -EIO : 0;
}

#define NVN 4u

static struct vnode g_vn[NVN];
static bool g_used[NVN], g_hashed[NVN];

struct vnode *vnode_lookup_cached(struct mount *mnt, uint64_t ino)
{
    EXPECT(mnt == &g_mnt);
    note(15, ino);
    for (unsigned i = 0; i < NVN; i++)
        if (g_hashed[i] && g_vn[i].ino == ino)
            return &g_vn[i];
    return NULL;
}

struct vnode *vnode_alloc(struct mount *mnt, uint64_t ino)
{
    EXPECT(mnt == &g_mnt);
    note(16, ino);
    if (chance(10))
        return NULL;
    for (unsigned i = 0; i < NVN; i++)
        if (!g_used[i]) {
            g_used[i] = true;
            memset(&g_vn[i], 0, sizeof(g_vn[i]));
            g_vn[i].ino = ino;
            g_vn[i].mnt = mnt;
            return &g_vn[i];
        }
    return NULL;
}

void vnode_hash_insert(struct vnode *vn)
{
    unsigned i = (unsigned)(vn - g_vn);
    EXPECT(i < NVN && g_used[i]);
    note(17, i);
    g_hashed[i] = true;
}

void cfs_fill_vnode(struct vnode *vn, const struct cfs_inode *in)
{
    note(18, in->mode);
    note(19, in->ino);
    note(20, in->parent);
    vn->mode = in->mode & 07777;
    vn->nlink = in->nlink;
}

/* ---- the snapshots ---- */

bool cfs_snapshot_find(struct cfs *fs, const char *name, struct cfs_snapshot *out)
{
    EXPECT(fs == &g_fs);
    mix(name, strlen(name) + 1);
    unsigned w = pick(5);
    if (w == 0)
        return false;
    memset(out, 0, sizeof(*out));
    out->id = w == 1 ? 0 : w == 2 ? CFS_SNAP_ID_MAX + 1 + pick(3) : 1 + pick(CFS_SNAP_ID_MAX);
    snprintf(out->name, sizeof(out->name), "%s", name);
    return true;
}

/* ---- the operations the tables name: never reached here ---- */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#define NEVER()                                                                                                     \
    {                                                                                                               \
        EXPECT(false);                                                                                              \
        return -EIO;                                                                                                \
    }
int cfs_lookup(struct vnode *d, const char *n, size_t l, struct vnode **o) NEVER()
int cfs_create(struct vnode *d, const char *n, size_t l, uint32_t m, struct vnode **o) NEVER()
int cfs_symlink(struct vnode *d, const char *n, size_t l, const char *t, struct vnode **o) NEVER()
int cfs_readlink(struct vnode *v, char *b, size_t l) NEVER()
int cfs_mkdir(struct vnode *d, const char *n, size_t l, uint32_t m, struct vnode **o) NEVER()
int cfs_link(struct vnode *d, const char *n, size_t l, struct vnode *t) NEVER()
int cfs_unlink(struct vnode *d, const char *n, size_t l, struct vnode *v) NEVER()
int cfs_rmdir(struct vnode *d, const char *n, size_t l, struct vnode *v) NEVER()
int cfs_rename(struct vnode *od, const char *on, size_t ol, struct vnode *v, struct vnode *nd, const char *nn,
               size_t nl, struct vnode *r) NEVER()
int cfs_readdir(struct vnode *d, uint64_t *p, vfs_dirent_cb cb, void *a) NEVER()
int cfs_readpage(struct vnode *v, uint64_t i, void *b) NEVER()
int cfs_dirty_check(struct vnode *v, uint64_t i, unsigned d) NEVER()
int cfs_writepage(struct vnode *v, uint64_t i, const void *b) NEVER()
int cfs_writepages(struct vnode *v, uint64_t i, void *const *p, unsigned n, unsigned *d) NEVER()
int cfs_truncate(struct vnode *v, uint64_t s) NEVER()
int cfs_vnode_setattr(struct vnode *v) NEVER()
int cfs_vnode_sync(struct vnode *v) NEVER()
void cfs_evict(struct vnode *v)
{
    EXPECT(false);
}
#pragma GCC diagnostic pop

/* ---- a round ---- */

static void name_of(char *out, size_t len, unsigned seed)
{
    for (size_t i = 0; i < len; i++)
        out[i] = (char)('a' + (seed * 7 + i * 3) % 26);
}

static void blocks(void)
{
    memset(g_blk, 0, sizeof(g_blk));
    unsigned nb = pick(NB + 1);
    for (unsigned b = 0; b < nb; b++) {
        struct cfs_dirent *d = (struct cfs_dirent *)g_blk[b];
        unsigned style = pick(5);
        unsigned s = 0;
        while (s < CFS_DIRENTS_PER_BLOCK) {
            unsigned w = pick(10);
            if (style == 0 || w < 3) {
                s++;
                continue;
            }
            unsigned len = w == 9 && g_fs.sb.version >= 15 ? 48 + pick(208) : 1 + pick(CFS_NAME_MAX);
            unsigned need = 1 + cfs_dirent_extra(len);
            if (s + need > CFS_DIRENTS_PER_BLOCK)
                break;
            char nm[CFS_NAME_MAX_LONG];
            name_of(nm, len, pick(8));
            uint64_t ino = 2 + pick(100);
            unsigned type = pick(2) ? CFS_TYPE_DIR : CFS_TYPE_REG;
            cfs_dirent_put(d, s, ino, type, nm, len);
            if (w == 8 && pick(3) == 0)
                d[s].namelen = 0;   /* a slot that does not check */
            s += need;
        }
        if (style == 1)
            for (unsigned k = 0; k < CFS_DIRENTS_PER_BLOCK; k++)
                d[k].ino = d[k].ino ? d[k].ino : 9;   /* full */
    }
    g_dir.size = (uint64_t)nb * CFS_BLOCK - (nb && pick(2) ? pick(CFS_BLOCK) : 0);
}

static void op(void)
{
    unsigned w = pick(10);
    note(40, w);
    unsigned kf = pick(8);
    g_fail_in = g_faults && kf == 0 ? 0 : -1;
    uint8_t block[CFS_BLOCK];
    memset(block, 0xEE, sizeof(block));
    char nm[CFS_NAME_MAX_LONG + 4];
    unsigned lw = pick(5);
    size_t len = lw == 0 ? 48 + pick(210) : lw == 1 ? 0 : 1 + pick(CFS_NAME_MAX);
    name_of(nm, len, pick(8));
    int rc = 0;
    switch (w) {
    case 0:
    case 1: {
        uint64_t lblk = 77;
        unsigned slot = 77;
        rc = cfs_dir_find(&g_fs, &g_dir, nm, len, block, &lblk, &slot);
        note(41, lblk);
        note(42, slot);
        if (rc == 0 && pick(2)) {
            rc = cfs_dir_remove_slot(&g_fs, &g_dir, lblk, slot, block);
            note(43, (uint64_t)(int64_t)rc);
        }
        break;
    }
    case 2:
    case 3:
    {
        unsigned tag = pick(3);
        uint64_t ino = CFS_SNAP_INO(tag, 2 + pick(50));
        unsigned type = pick(2) ? CFS_TYPE_DIR : CFS_TYPE_REG;
        rc = cfs_dir_add(&g_fs, &g_dir, nm, len, ino, type, block);
    }
        break;
    case 4:
        rc = cfs_dir_is_empty(&g_fs, &g_dir, block);
        note(44, cfs_dir_blocks(&g_dir));
        break;
    case 5: {
        struct vnode *vn = NULL;
        rc = cfs_snapdir_get(&g_fs, &vn);
        if (rc == 0) {
            note(45, (uint64_t)(vn - g_vn));
            struct cfs_vnode *cv = vn->fs_priv;
            mix(&cv->inode, sizeof(cv->inode));
            note(46, cv->snap_tag);
            note(47, cfs_is_snapdir(vn));
            note(48, cfs_snap_tag_of(vn));
        }
        break;
    }
    case 6: {
        struct cfs_snapshot s;
        memset(&s, 0, sizeof(s));
        unsigned tag = 99;
        size_t sl = pick(4) == 0 ? CFS_SNAP_NAME_MAX + 1 + pick(3) : 1 + pick(CFS_SNAP_NAME_MAX);
        rc = cfs_snap_by_name(&g_fs, nm, sl <= len ? sl : len, &s, &tag);
        note(49, tag);
        break;
    }
    case 7:
        rc = cfs_tree_readonly(&g_fs, &g_dir);
        break;
    default:
        rc = cfs_lock_tree_w(&g_fs, &g_dir);
        if (rc == 0) {
            EXPECT(g_locked == 1);
            cfs_unlock_tree(&g_fs);
        }
        break;
    }
    g_fail_in = -1;
    note(50, (uint64_t)(int64_t)rc);
    note(51, g_dir.size);
    mix(g_blk, sizeof(g_blk));
    EXPECT(g_locked == 0 && !g_in_clone);
}

static void round_(void)
{
    memset(&g_fs, 0, sizeof(g_fs));
    memset(&g_mnt, 0, sizeof(g_mnt));
    memset(&g_dir, 0, sizeof(g_dir));
    memset(&g_dircv, 0, sizeof(g_dircv));
    g_fs.mnt = &g_mnt;
    g_fs.sb.version = 4 + pick(12);
    g_dir.mnt = &g_mnt;
    g_dir.ino = 1;
    unsigned t = pick(6);
    g_dir.fs_priv = t == 0 ? NULL : &g_dircv;
    g_dircv.snap_tag = t == 1 ? 0xFFFFu : t == 2 ? 1 + pick(10) : 0;
    g_clone_exists = pick(2);
    blocks();
    unsigned ops = 1 + pick(6);
    for (unsigned k = 0; k < ops; k++) {
        if (g_dir.fs_priv == NULL && pick(2))
            g_dir.fs_priv = &g_dircv;   /* the block calls need a directory's state */
        if (g_dir.fs_priv == NULL) {
            /* A vnode with no state of this filesystem's yet: the live tree's, read-only or not. */
            bool ro = cfs_tree_readonly(&g_fs, &g_dir);
            int rc = cfs_lock_tree_w(&g_fs, &g_dir);
            note(52, ro);
            note(53, (uint64_t)(int64_t)rc);
            if (rc == 0)
                cfs_unlock_tree(&g_fs);
            continue;
        }
        op();
    }
    for (unsigned i = 0; i < NVN; i++) {
        if (g_used[i])
            kfree(g_vn[i].fs_priv);
        g_used[i] = g_hashed[i] = false;
    }
    EXPECT(g_live == 0);
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
    printf("  dirblk digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, ROUNDS);
    EXPECT(g_digest == DIRBLK_DIGEST);
    EXPECT(cfs_dir_ops.lookup == cfs_lookup && cfs_file_ops.writepages == cfs_writepages &&
           cfs_lnk_ops.readlink == cfs_readlink);
}

int main(void)
{
    static const struct host_test tests[] = {
        { "dirblk_digest", test_digest },
    };
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
