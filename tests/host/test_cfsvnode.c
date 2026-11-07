/*
 * test_cfsvnode.c - cosmofs's vnodes (kernel-services/filesystem/cosmofs/cosmofs_vnode.cee) on the host: roadmap 2.0
 * SA7. A digest recorded from the C over vnodes asked for in the live tree, inside a clone and in snapshots, cached or
 * made, of directories, links and files with every mode bit, owner and time, inode numbers bare and tagged -- and
 * written back through their inodes after the vnode changed -- while the cache, the allocator, the inode reads and
 * writes and the map's preparation fail now and then. Every argument, return, vnode and inode is in the digest; every
 * vnode made is the cache's and nothing else is left allocated.
 */

#include "harness.h"

#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/vfs.h>

#include "cosmofs_internal.h"   /* before the C library's headers, as test_cfsparse.c: its signal.h is the kernel's */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The values the CeeSafe port writes as its own constants. */
_Static_assert(ENOMEM == 12 && VNODE_REG == 1 && VNODE_DIR == 2 && VNODE_LNK == 6 &&
                   offsetof(struct vnode, type) == 56 && sizeof(((struct vnode *)0)->type) == 4,
               "cosmofs_vnode's errno.h and vfs.h values");
_Static_assert(CFS_TYPE_REG == 1 && CFS_TYPE_DIR == 2 && CFS_TYPE_LNK == 3 && CFS_SNAP_INO_SHIFT == 48 &&
                   CFS_MODE(CFS_TYPE_LNK, 0777) == ((3u << 12) | 511),
               "cosmofs_vnode's cosmofs_format.h values");

/* The digest of the C (cosmofs_vnode.c, 2026-11-07), which the CeeSafe port gives. */
#define VNODE_DIGEST 0x9c4898b008ad2569ull
#define ROUNDS 20000u
#define SEED 0x7E0DE7E02026AB0Dull

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

/* ---- the operations the vnodes are given: cosmofs.c's, here only told apart ---- */

const struct vnode_ops cfs_dir_ops;
const struct vnode_ops cfs_file_ops;
const struct vnode_ops cfs_lnk_ops;

/* ---- the vnode cache ---- */

#define NVN 16u

static struct cfs g_fs;
static struct mount g_mnt;
static struct vnode g_vn[NVN];
static bool g_used[NVN], g_hashed[NVN];

static unsigned slot_of(const struct vnode *vn)
{
    unsigned i = (unsigned)(vn - g_vn);
    EXPECT(i < NVN);
    return i;
}

struct vnode *vnode_lookup_cached(struct mount *mnt, uint64_t ino)
{
    EXPECT(mnt == &g_mnt);
    note(10, ino);
    for (unsigned i = 0; i < NVN; i++)
        if (g_hashed[i] && g_vn[i].ino == ino) {
            note(11, i);
            return &g_vn[i];
        }
    return NULL;
}

struct vnode *vnode_alloc(struct mount *mnt, uint64_t ino)
{
    EXPECT(mnt == &g_mnt);
    note(12, ino);
    if (chance(15))
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
    unsigned i = slot_of(vn);
    EXPECT(g_used[i] && !g_hashed[i]);
    note(13, i);
    g_hashed[i] = true;
}

/* ---- the inodes ---- */

static struct cfs_inode g_disk[64];

static void inode_of(uint64_t ino, struct cfs_inode *out)
{
    memset(out, 0, sizeof(*out));
    *out = g_disk[ino % 64];
    out->ino = ino;
}

int cfs_inode_read(struct cfs *fs, uint64_t ino, struct cfs_inode *out)
{
    EXPECT(fs == &g_fs);
    note(14, ino);
    if (chance(12))
        return -EIO;
    if (ino >= 1ull << CFS_SNAP_INO_SHIFT)
        return -ENOENT;   /* a tagged number reached the map: the caller forgot to strip it */
    inode_of(ino, out);
    return 0;
}

int cfs_inode_read_at(struct cfs *fs, uint64_t imap_root, uint64_t next_ino, uint64_t ino, struct cfs_inode *out)
{
    EXPECT(fs == &g_fs);
    note(15, imap_root);
    note(16, next_ino);
    note(17, ino);
    if (chance(12))
        return -EIO;
    if (ino >= next_ino)
        return -ENOENT;
    inode_of(ino ^ imap_root, out);
    out->ino = ino;
    return 0;
}

int cfs_inode_write(struct cfs *fs, uint64_t ino, const struct cfs_inode *in)
{
    EXPECT(fs == &g_fs);
    note(18, ino);
    mix(in, sizeof(*in));
    return chance(10) ? -EIO : 0;
}

int cfs_inode_prepare(struct cfs *fs, uint64_t ino)
{
    EXPECT(fs == &g_fs);
    note(19, ino);
    return chance(10) ? -ENOMEM : 0;
}

void cfs_fail(struct cfs *fs, int rc)
{
    EXPECT(fs == &g_fs);
    note(20, (uint64_t)(int64_t)rc);
    if (fs->failed == 0)
        fs->failed = rc;
}

/* ---- a round ---- */

static struct cfs_clone g_clone;

static void mix_vnode(const struct vnode *vn)
{
    note(30, slot_of(vn));
    note(31, vn->ino);
    note(32, (uint64_t)vn->type);
    note(33, vn->mode);
    note(34, vn->uid);
    note(35, vn->gid);
    note(36, vn->nlink);
    note(37, vn->size);
    note(38, vn->mtime_ns);
    note(39, vn->ctime_ns);
    note(40, vn->ops == &cfs_dir_ops ? 1 : vn->ops == &cfs_lnk_ops ? 2 : vn->ops == &cfs_file_ops ? 3 : 0);
    const struct cfs_vnode *cv = vn->fs_priv;
    if (cv != NULL) {
        mix(&cv->inode, sizeof(cv->inode));
        note(41, cv->snap_tag);
        note(42, cv->snap_imap_root);
        note(43, cv->snap_next_ino);
    }
}

static void round_(void)
{
    memset(&g_fs, 0, sizeof(g_fs));
    memset(&g_mnt, 0, sizeof(g_mnt));
    g_fs.mnt = &g_mnt;
    for (unsigned i = 0; i < 64; i++) {
        struct cfs_inode *in = &g_disk[i];
        memset(in, 0, sizeof(*in));
        unsigned t = pick(5);
        uint32_t type = t == 0 ? CFS_TYPE_DIR : t == 1 ? CFS_TYPE_LNK : t == 2 ? 7 : CFS_TYPE_REG;
        in->mode = CFS_MODE(type, (uint32_t)next());
        in->uid = (uint32_t)next();
        in->gid = (uint32_t)next();
        in->nlink = pick(4);
        uint64_t size = next();
        in->size = size >> pick(64);
        in->mtime_ns = next();
        in->ctime_ns = next();
        in->csum_algo = pick(3);
    }
    unsigned ops = 1 + pick(10);
    for (unsigned k = 0; k < ops; k++) {
        unsigned w = pick(5);
        note(50, w);
        unsigned kf = pick(10);
        g_fail_in = g_faults && kf == 0 ? 0 : -1;
        int rc = 0;
        struct vnode *vn = NULL;
        uint64_t ino = 1 + pick(40);
        if (pick(5) == 0)
            ino = CFS_SNAP_INO(1 + pick(3), ino);   /* a number a tagged vnode carries */
        if (w == 0 || w == 1) {
            unsigned c = pick(3);
            g_clone.id = 1 + pick(5);
            g_fs.cur_clone = c == 0 ? &g_clone : NULL;
            rc = cfs_vnode_get(&g_fs, ino, &vn);
        } else if (w == 2) {
            struct cfs_snapshot snap;
            memset(&snap, 0, sizeof(snap));
            snap.imap_root = pick(64);
            snap.next_ino = pick(50);
            unsigned tag = pick(4) == 0 ? 0 : 1 + pick(5);
            rc = cfs_vnode_get_tagged(&g_fs, CFS_INO_OF(ino), tag, &snap, &vn);
        } else {
            unsigned i = pick(NVN);
            if (!g_hashed[i])
                continue;
            vn = &g_vn[i];
            unsigned t = pick(4);
            vn->type = t == 0 ? VNODE_DIR : t == 1 ? VNODE_LNK : VNODE_REG;
            vn->mode = (uint32_t)next();
            vn->uid = (uint32_t)next();
            vn->nlink = pick(5);
            vn->size = next();
            vn->mtime_ns = next();
            rc = w == 3 ? cfs_vnode_inode_sync(&g_fs, vn) : cfs_vnode_prepare(&g_fs, vn);
        }
        g_fail_in = -1;
        note(51, (uint64_t)(int64_t)rc);
        if (rc == 0 && vn != NULL)
            mix_vnode(vn);
        note(52, (uint64_t)(int64_t)g_fs.failed);
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
    printf("  vnode digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, ROUNDS);
    EXPECT(g_digest == VNODE_DIGEST);
}

int main(void)
{
    static const struct host_test tests[] = {
        { "vnode_digest", test_digest },
    };
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
