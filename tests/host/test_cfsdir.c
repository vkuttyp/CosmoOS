/*
 * test_cfsdir.c - cosmofs's directory operations (kernel-services/filesystem/cosmofs/cosmofs_dir.cee) on the host:
 * roadmap 2.0 SA7. A digest recorded from the C, with the real directory helpers (cosmofs.cee) and entry decoders
 * (cfsparse), over a tree the test keeps -- the root, a directory, files, links, entries with short and long names --
 * in the live tree, inside a clone (one that exists and one gone) and inside snapshots, and through .snapshots:
 * lookups of names, "..", .snapshots and snapshots and clones by name; creation of files, directories and links
 * (some refused, some failing halfway and given back); readlink; hard links; unlink and rmdir of files, directories
 * empty or not and snapshots busy or not; renames within and between directories, over a file or a directory or
 * nothing, and their rollback; readdir from every position with a reader that stops -- while the block calls, the
 * inode calls, the vnodes, the orphan record, the snapshots and kmalloc fail now and then. Every argument, return,
 * entry, block and vnode is in the digest; the lock is let go and nothing is left allocated.
 */

#include "harness.h"

#include <kernel/cosmofs.h>
#include <kernel/cred.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/mutex.h>
#include <kernel/object.h>
#include <kernel/vfs.h>

#include "cosmofs_internal.h"   /* before the C library's headers, as test_cfsparse.c: its signal.h is the kernel's */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The values the CeeSafe port writes as its own constants. */
_Static_assert(EPERM == 1 && ENOENT == 2 && EIO == 5 && ENOMEM == 12 && EBUSY == 16 && EEXIST == 17 && EXDEV == 18 &&
                   EROFS == 30 && ENAMETOOLONG == 36 && ENOTEMPTY == 39 && EOPNOTSUPP == 95,
               "cosmofs_dir's errno.h values");
_Static_assert(MOUNT_RDONLY == 1 && VNODE_REG == 1 && VNODE_DIR == 2 && VNODE_LNK == 6 &&
                   offsetof(struct vnode, type) == 56 && sizeof(((struct vnode *)0)->type) == 4,
               "cosmofs_dir's vfs.h values");
_Static_assert(CFS_BLOCK == 4096 && CFS_ROOT_INO == 1 && CFS_TYPE_REG == 1 && CFS_TYPE_DIR == 2 && CFS_TYPE_LNK == 3 &&
                   CFS_CSUM_CRC32C == 1 && CFS_CSUM_POLY1305 == 2 && CFS_COMPRESS_NONE == 0 &&
                   CFS_COMPRESS_LZ4 == 1 && CFS_ALLOC_META == 0 && CFS_NAME_MAX_LONG == 255 &&
                   CFS_SNAP_NAME_MAX == 31 && CFS_SNAP_F_CLONE == 1 && CFS_SNAP_INO_SHIFT == 48 &&
                   CFS_DIRENTS_PER_BLOCK == 64 && sizeof(struct cfs_dirent) == 64 && CFS_DIRENT_CONT == ~0ull &&
                   offsetof(struct cfs_dirent, type) == 8 && CFS_EXT_COMPRESSED == 0x80000000u &&
                   CFS_EXT_COUNT_MASK == 0xFFFF,
               "cosmofs_dir's cosmofs_format.h values");
/* cosmofs_dir.cee keeps an inode in a stack slot of 32 words and a snapshot entry in one of 16. */
_Static_assert(sizeof(struct cfs_inode) <= 32 * 8 && sizeof(struct cfs_snapshot) <= 16 * 8,
               "cosmofs_dir.cee's stack slots");

/* The digest of the C (cosmofs_dir.c, 2026-11-07), which the CeeSafe port gives. */
#define DIR_DIGEST 0x00fc036656d8ef7bull
#define ROUNDS 30000u
#define SEED 0xD1D1D1D12026D1D1ull

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

static void *alloc(size_t size, bool zero)
{
    if (size == 0)
        return NULL;
    if (g_fail_in >= 0 && g_fail_in-- == 0) {
        note(2, size);
        return NULL;
    }
    void *p = zero ? calloc(1, size) : malloc(size);
    EXPECT(p != NULL);
    if (!zero)
        memset(p, 0xA5, size);
    g_live++;
    return p;
}

void *kmalloc(size_t size, unsigned flags) { return alloc(size, (flags & KMEM_ZERO) != 0); }

void *kzalloc(size_t size) { return alloc(size, true); }

void kfree(void *p)
{
    if (p != NULL)
        g_live--;
    free(p);
}

/* ---- locks, credentials, time ---- */

static int g_locked;
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

static struct credentials g_cred;

const struct credentials *cred_current(void) { return &g_cred; }

uint64_t vfs_now_ns(void) { return 31337; }

/* ---- the tree: inodes, directory blocks, vnodes ---- */

#define NINO 32u
#define NBLK 3u
#define NVN 24u

struct node {
    bool used;
    unsigned type;          /* CFS_TYPE_* */
    uint32_t nlink;
    uint64_t parent;
    uint64_t size;
    uint8_t blk[NBLK][CFS_BLOCK];
};

static struct cfs g_fs;
static struct mount g_mnt;
static struct node g_node[NINO];
static struct vnode g_vn[NVN];
static struct cfs_vnode *g_cv[NVN];
static bool g_vused[NVN];
static unsigned g_refs[NVN];
static struct cfs_clone g_clone;
static bool g_clone_exists;

static struct node *node_of(uint64_t ino) { return CFS_INO_OF(ino) < NINO ? &g_node[CFS_INO_OF(ino)] : NULL; }

int cfs_dir_read_block_at(struct cfs *fs, const struct cfs_inode *dir, uint64_t lblk, uint8_t *buf)
{
    EXPECT(fs == &g_fs && g_locked == 1);
    note(10, dir->ino);
    note(11, lblk);
    if (chance(40))
        return -EIO;
    struct node *n = node_of(dir->ino);
    if (n == NULL || lblk >= NBLK)
        memset(buf, 0, CFS_BLOCK);
    else
        memcpy(buf, n->blk[lblk], CFS_BLOCK);
    return 0;
}

int cfs_data_write_block(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, const void *buf,
                         enum cfs_alloc_class cls)
{
    EXPECT(fs == &g_fs && g_locked == 1);
    note(12, in->ino);
    note(13, lblk);
    note(14, (uint64_t)cls);
    mix(buf, CFS_BLOCK);
    if (chance(20))
        return -ENOSPC;
    struct node *n = node_of(in->ino);
    if (n != NULL && lblk < NBLK)
        memcpy(n->blk[lblk], buf, CFS_BLOCK);
    in->direct[0].start = 7000 + in->ino;
    in->direct[0].count = 1;
    return 0;
}

int cfs_map(struct cfs *fs, const struct cfs_inode *in, uint64_t lblk, uint64_t *pblk)
{
    EXPECT(fs == &g_fs && g_locked == 1);
    note(15, in->ino);
    if (chance(15))
        return -EIO;
    if (lblk != 0 || pick(8) == 0)
        return 0;
    *pblk = 9000 + in->ino;
    return 1;
}

int cfs_data_read_verified(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, uint64_t dva, void *buf)
{
    EXPECT(fs == &g_fs && g_locked == 1);
    note(16, dva);
    if (chance(15))
        return -EIO;
    struct node *n = node_of(in->ino);
    memcpy(buf, n ? n->blk[lblk] : (const uint8_t *)"", n ? CFS_BLOCK : 1);
    return 0;
}

static unsigned vslot(const struct vnode *vn)
{
    unsigned i = (unsigned)(vn - g_vn);
    EXPECT(i < NVN);
    return i;
}

static struct cfs_vnode *cv_of(const struct vnode *vn) { return vn->fs_priv; }

int cfs_vnode_inode_sync(struct cfs *fs, struct vnode *vn)
{
    EXPECT(fs == &g_fs && g_locked == 1);
    note(17, vslot(vn));
    note(18, vn->nlink);
    note(19, vn->size);
    note(20, cv_of(vn)->inode.parent);
    if (chance(25))
        return -EIO;
    struct node *n = node_of(vn->ino);
    if (n != NULL && CFS_SNAP_TAG(vn->ino) == 0) {
        n->nlink = vn->nlink;
        n->size = vn->size;
        n->parent = cv_of(vn)->inode.parent;
    }
    return 0;
}

int cfs_vnode_prepare(struct cfs *fs, struct vnode *vn)
{
    EXPECT(fs == &g_fs && g_locked == 1);
    note(21, vslot(vn));
    return chance(25) ? -ENOMEM : 0;
}

struct vnode *vnode_lookup_cached(struct mount *mnt, uint64_t ino)
{
    EXPECT(mnt == &g_mnt);
    for (unsigned i = 0; i < NVN; i++)
        if (g_vused[i] && g_vn[i].ino == ino) {
            g_refs[i]++;
            return &g_vn[i];
        }
    return NULL;
}

struct vnode *vnode_alloc(struct mount *mnt, uint64_t ino)
{
    EXPECT(mnt == &g_mnt);
    note(22, ino);
    if (chance(30))
        return NULL;
    for (unsigned i = 0; i < NVN; i++)
        if (!g_vused[i]) {
            g_vused[i] = true;
            g_refs[i] = 1;
            memset(&g_vn[i], 0, sizeof(g_vn[i]));
            g_vn[i].ino = ino;
            g_vn[i].mnt = mnt;
            return &g_vn[i];
        }
    return NULL;
}

void vnode_hash_insert(struct vnode *vn) { note(23, vslot(vn)); }

void cfs_fill_vnode(struct vnode *vn, const struct cfs_inode *in)
{
    unsigned t = CFS_MODE_TYPE(in->mode);
    vn->type = t == CFS_TYPE_DIR ? VNODE_DIR : t == CFS_TYPE_LNK ? VNODE_LNK : VNODE_REG;
    vn->nlink = in->nlink;
    vn->size = in->size;
    note(24, in->mode);
}

/* A vnode for `ino` with tag `tag`, made from the node the test keeps. */
static int vnode_for(uint64_t ino, unsigned tag, struct vnode **out)
{
    uint64_t key = CFS_SNAP_INO(tag, CFS_INO_OF(ino));
    struct vnode *vn = vnode_lookup_cached(&g_mnt, key);
    if (vn != NULL) {
        *out = vn;
        return 0;
    }
    struct node *n = node_of(ino);
    if (n == NULL || !n->used)
        return -ENOENT;
    struct cfs_vnode *cv = kzalloc(sizeof(*cv));
    if (cv == NULL)
        return -ENOMEM;
    cv->inode.ino = CFS_INO_OF(ino);
    cv->inode.mode = CFS_MODE(n->type, 0755);
    cv->inode.nlink = n->nlink;
    cv->inode.size = n->size;
    cv->inode.parent = n->parent;
    cv->snap_tag = tag;
    cv->snap_imap_root = 55;
    cv->snap_next_ino = NINO;
    vn = vnode_alloc(&g_mnt, key);
    if (vn == NULL) {
        kfree(cv);
        return -ENOMEM;
    }
    vn->fs_priv = cv;
    g_cv[vslot(vn)] = cv;
    cfs_fill_vnode(vn, &cv->inode);
    *out = vn;
    return 0;
}

int cfs_vnode_get(struct cfs *fs, uint64_t ino, struct vnode **out)
{
    EXPECT(fs == &g_fs && g_locked == 1);
    note(25, ino);
    if (chance(30))
        return -ENOMEM;
    return vnode_for(ino, g_in_clone ? (unsigned)g_clone.id : 0, out);
}

int cfs_vnode_get_tagged(struct cfs *fs, uint64_t ino, unsigned tag, const struct cfs_snapshot *snap,
                         struct vnode **out)
{
    EXPECT(fs == &g_fs && g_locked == 1);
    note(26, ino);
    note(27, tag);
    note(28, snap->imap_root);
    note(29, snap->next_ino);
    if (chance(30))
        return -EIO;
    return vnode_for(ino, tag, out);
}

/* ---- inodes, the orphan record, the clones and the snapshots ---- */

int cfs_inode_alloc(struct cfs *fs, uint64_t *ino)
{
    EXPECT(fs == &g_fs && g_locked == 1);
    if (chance(20))
        return -ENOSPC;
    for (uint64_t i = 3; i < NINO; i++)
        if (!g_node[i].used) {
            g_node[i].used = true;
            *ino = i;
            note(30, i);
            return 0;
        }
    return -ENOSPC;
}

int cfs_inode_write(struct cfs *fs, uint64_t ino, const struct cfs_inode *in)
{
    EXPECT(fs == &g_fs && g_locked == 1);
    note(31, ino);
    mix(in, sizeof(*in));
    if (chance(20))
        return -EIO;
    struct node *n = node_of(ino);
    if (n != NULL) {
        n->type = CFS_MODE_TYPE(in->mode);
        n->nlink = in->nlink;
        n->parent = in->parent;
        n->size = in->size;
    }
    return 0;
}

void cfs_inode_discard(struct cfs *fs, uint64_t ino)
{
    EXPECT(fs == &g_fs && g_locked == 1);
    note(32, ino);
    struct node *n = node_of(ino);
    if (n != NULL)
        memset(n, 0, sizeof(*n));
}

int cfs_names_allow(struct cfs *fs, size_t len)
{
    EXPECT(fs == &g_fs && g_locked == 1);
    note(33, len);
    if (len > CFS_NAME_MAX && fs->sb.version < 15)
        return -ENAMETOOLONG;
    return chance(30) ? -EIO : 0;
}

void cfs_csum_free(struct cfs *fs, struct cfs_inode *in)
{
    EXPECT(fs == &g_fs && g_locked == 1);
    note(34, in->ino);
}

void cfs_free_block_deferred(struct cfs *fs, uint64_t blk)
{
    EXPECT(fs == &g_fs && g_locked == 1);
    note(35, blk);
}

bool cfs_orphan_reserve(struct cfs *fs)
{
    EXPECT(fs == &g_fs && g_locked == 1);
    return !chance(15);
}

void cfs_orphan_add(struct cfs *fs, uint64_t ino)
{
    EXPECT(fs == &g_fs && g_locked == 1);
    note(36, ino);
}

void cfs_fail(struct cfs *fs, int rc)
{
    EXPECT(fs == &g_fs);
    note(37, (uint64_t)(int64_t)rc);
    if (fs->failed == 0)
        fs->failed = rc;
}

struct cfs_clone *cfs_clone_by_id(struct cfs *fs, uint64_t id)
{
    EXPECT(fs == &g_fs && g_locked == 1);
    note(38, id);
    if (chance(8))
        return NULL;   /* deleted since the caller last looked */
    return g_clone_exists && id == g_clone.id ? &g_clone : NULL;
}

void cfs_clone_enter(struct cfs *fs, struct cfs_clone *c)
{
    EXPECT(fs == &g_fs && c == &g_clone && !g_in_clone);
    g_in_clone = true;
}

void cfs_clone_leave(struct cfs *fs)
{
    EXPECT(fs == &g_fs);
    g_in_clone = false;
}

bool cfs_snapshot_find(struct cfs *fs, const char *name, struct cfs_snapshot *out)
{
    EXPECT(fs == &g_fs && g_locked == 1);
    mix(name, strlen(name));
    if (name[0] != 's')
        return false;
    memset(out, 0, sizeof(*out));
    out->id = name[1] == 'c' ? g_clone.id : 3 + (unsigned)(name[1] % 4);
    out->flags = name[1] == 'c' ? CFS_SNAP_F_CLONE : 0;
    out->imap_root = 66;
    out->next_ino = NINO;
    return true;
}

int cfs_snapshot_create(struct cfs *fs, const char *name, bool identity, struct cfs_snapshot *out)
{
    EXPECT(fs == &g_fs && g_locked == 1 && out == NULL);
    mix(name, strlen(name));
    note(39, identity);
    return chance(5) ? -EEXIST : 0;
}

int cfs_snapshot_delete(struct cfs *fs, const char *name)
{
    EXPECT(fs == &g_fs && g_locked == 1);
    mix(name, strlen(name));
    return chance(5) ? -EIO : 0;
}

bool vnode_cache_any(struct mount *mnt, bool (*pred)(const struct vnode *vn, void *arg), void *arg)
{
    EXPECT(mnt == &g_mnt);
    for (unsigned i = 0; i < NVN; i++)
        if (g_vused[i] && pred(&g_vn[i], arg))
            return true;
    return false;
}

uint32_t kobject_refcount(const struct kobject *obj)
{
    for (unsigned i = 0; i < NVN; i++)
        if (obj == &g_vn[i].obj)
            return g_refs[i];
    EXPECT(false);
    return 0;
}

/* ---- the file operations the tables name: never reached here ---- */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#define NEVER()                                                                                                     \
    {                                                                                                               \
        EXPECT(false);                                                                                              \
        return -EIO;                                                                                                \
    }
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

static const char *const k_names[] = {
    "a", "b", "dir", "lnk", "x", "..", ".snapshots", "s1", "s2", "sc", "zz",
    "a-name-of-sixty-bytes-that-needs-a-continuation-slot-or-two-0123",
};

static char g_toolong[CFS_NAME_MAX_LONG + 10];

static const char *some_name(size_t *len)
{
    if (pick(25) == 0) {
        memset(g_toolong, 'L', sizeof(g_toolong));
        *len = CFS_NAME_MAX_LONG + 1 + pick(5);
        return g_toolong;
    }
    const char *n = k_names[pick(sizeof(k_names) / sizeof(k_names[0]))];
    *len = strlen(n);
    return n;
}

/* A live short entry of `dir` (its first block, as the test wrote it): its name and inode. */
static bool entry_in(const struct vnode *dir, char *name, size_t *len, uint64_t *ino)
{
    struct node *n = node_of(dir->ino);
    if (n == NULL || !n->used)
        return false;
    const struct cfs_dirent *d = (const struct cfs_dirent *)n->blk[0];
    unsigned start = pick(CFS_DIRENTS_PER_BLOCK);
    for (unsigned k = 0; k < CFS_DIRENTS_PER_BLOCK; k++) {
        unsigned s = (start + k) % CFS_DIRENTS_PER_BLOCK;
        if (d[s].ino == 0 || d[s].ino == CFS_DIRENT_CONT || d[s].namelen == 0 || d[s].namelen > CFS_NAME_MAX ||
            d[s].extra != 0)
            continue;
        memcpy(name, d[s].name, d[s].namelen);
        *len = d[s].namelen;
        *ino = d[s].ino;
        return true;
    }
    return false;
}

static void put_entry(struct node *dir, uint64_t ino, unsigned type, const char *name)
{
    struct cfs_dirent *d = (struct cfs_dirent *)dir->blk[0];
    unsigned len = (unsigned)strlen(name);
    int s = cfs_dirent_free_run(d, 1 + cfs_dirent_extra(len));
    if (s >= 0)
        cfs_dirent_put(d, (unsigned)s, ino, type, name, len);
}

static void tree(void)
{
    memset(g_node, 0, sizeof(g_node));
    struct node *root = &g_node[1], *sub = &g_node[2];
    root->used = sub->used = true;
    root->type = sub->type = CFS_TYPE_DIR;
    root->nlink = 3;
    sub->nlink = 2;
    root->parent = sub->parent = 1;
    root->size = sub->size = CFS_BLOCK;
    put_entry(root, 2, CFS_TYPE_DIR, "dir");
    unsigned files = 1 + pick(5);
    for (unsigned f = 0; f < files; f++) {
        uint64_t ino = 3 + f;
        struct node *n = &g_node[ino];
        n->used = true;
        n->type = pick(4) == 0 ? CFS_TYPE_LNK : CFS_TYPE_REG;
        n->nlink = 1 + pick(2);
        n->parent = pick(2) ? 1 : 2;
        n->size = n->type == CFS_TYPE_LNK ? 1 + pick(40) : pick(9000);
        memset(n->blk[0], 'T', CFS_BLOCK);
        put_entry(&g_node[n->parent], ino, n->type, k_names[pick(sizeof(k_names) / sizeof(k_names[0]))]);
    }
    if (g_fs.sb.version >= 15 && pick(2))
        put_entry(sub, 3, CFS_TYPE_REG, k_names[sizeof(k_names) / sizeof(k_names[0]) - 1]);
    if (pick(6) == 0) {
        struct cfs_dirent *d = (struct cfs_dirent *)sub->blk[0];
        d[pick(CFS_DIRENTS_PER_BLOCK)].ino = CFS_DIRENT_CONT;   /* a continuation with its head gone */
    }
    if (pick(8) == 0)
        ((struct cfs_dirent *)sub->blk[0])[pick(4)].namelen = 0;   /* an entry that does not check */
}

static struct vnode *some_vnode(void)
{
    unsigned w = pick(8);
    if (g_mnt.fs_priv == NULL && w <= 2)
        w = 3;   /* a mount that is gone has only the live tree's vnodes: a tree lookup needs the filesystem */
    struct vnode *vn = NULL;
    int rc;
    g_locked = 1;
    if (w == 0)
        rc = cfs_snapdir_get(&g_fs, &vn);
    else {
        uint64_t ino = 1 + pick(NINO / 2);
        unsigned tag = w == 1 ? 3 + pick(2) : w == 2 ? (unsigned)g_clone.id : 0;
        if (w == 2 && pick(3) == 0)
            ino = CFS_ROOT_INO;   /* a clone's root */
        rc = vnode_for(ino, tag, &vn);
    }
    g_locked = 0;
    return rc == 0 ? vn : NULL;
}

static int count_cb(void *arg, const char *name, size_t len, uint64_t ino, enum vnode_type type)
{
    unsigned *left = arg;
    mix(name, len);
    note(60, ino);
    note(61, (uint64_t)type);
    if (*left == 0)
        return 1;
    (*left)--;
    return 0;
}

static void op(void)
{
    unsigned w = pick(13);
    note(50, w);
    struct vnode *dir = some_vnode();
    struct vnode *other = some_vnode();
    if (dir == NULL || other == NULL)
        return;
    size_t len, len2;
    const char *name = some_name(&len);
    const char *name2 = some_name(&len2);
    static char found[CFS_NAME_MAX + 1];
    uint64_t fino;
    size_t flen;
    if ((w >= 5 && w <= 10) && pick(3) != 0 && entry_in(dir, found, &flen, &fino)) {
        /* An entry that is there, and the vnode it names: what the VFS hands an unlink or a rename. */
        struct vnode *named = NULL;
        g_locked = 1;
        int vrc = vnode_for(fino, cfs_snap_tag_of(dir), &named);
        g_locked = 0;
        if (vrc == 0) {
            other = named;
            name = found;
            len = flen;
        }
    }
    unsigned kf = pick(8);
    g_fail_in = g_faults && kf == 0 ? (int)pick(4) : -1;
    int rc = 0;
    struct vnode *out = NULL;
    switch (w) {
    case 0:
    case 1:
        if (pick(4) == 0) {
            name = "..";
            len = 2;
        }
        rc = cfs_lookup(dir, name, len, &out);
        break;
    case 2:
        rc = cfs_create(dir, name, len, 0644, &out);
        break;
    case 3:
        /* NULL out: a snapshot by mkdir, which is the only mkdir that may say nothing back */
        rc = cfs_mkdir(dir, name, len, 0755, pick(4) || !cfs_is_snapdir(dir) ? &out : NULL);
        break;
    case 4: {
        char target[64];
        unsigned tw = pick(6);
        size_t tl = tw == 0 ? 0 : 1 + pick(60);
        memset(target, 'q', sizeof(target));
        target[tl] = '\0';
        rc = cfs_symlink(dir, name, len, target, &out);
        break;
    }
    case 5: {
        char buf[64];
        memset(buf, 0, sizeof(buf));
        rc = cfs_readlink(other, buf, pick(64));
        mix(buf, sizeof(buf));
        break;
    }
    case 6:
        rc = cfs_link(dir, name, len, other);
        break;
    case 7:
        rc = cfs_unlink(dir, name, len, other);
        break;
    case 8:
        rc = cfs_rmdir(dir, name, len, other);
        break;
    case 9:
    case 10: {
        struct vnode *ndir = pick(2) ? dir : some_vnode();
        if (ndir == NULL)
            ndir = dir;
        struct vnode *replaced = NULL;
        unsigned rw = pick(3);
        static char rname[CFS_NAME_MAX + 1];
        size_t rlen;
        uint64_t rino;
        if (rw == 0) {
            replaced = some_vnode();
        } else if (rw == 1 && entry_in(ndir, rname, &rlen, &rino)) {
            /* The destination is there, and so is the vnode it names. */
            g_locked = 1;
            if (vnode_for(rino, cfs_snap_tag_of(ndir), &replaced) != 0)
                replaced = NULL;
            g_locked = 0;
            name2 = rname;
            len2 = rlen;
        }
        rc = cfs_rename(dir, name, len, other, ndir, name2, len2, replaced);
        break;
    }
    default: {
        uint64_t pos = pick(3) == 0 ? pick(140) : 0;
        unsigned left = pick(4) == 0 ? pick(5) : 1000;
        rc = cfs_readdir(dir, &pos, count_cb, &left);
        note(62, pos);
        break;
    }
    }
    g_fail_in = -1;
    note(51, (uint64_t)(int64_t)rc);
    if (rc == 0 && out != NULL)
        note(52, out->ino);
    note(53, (uint64_t)(int64_t)g_fs.failed);
    for (unsigned i = 1; i < NINO; i++) {
        note(54, g_node[i].used);
        note(55, g_node[i].nlink);
        mix(g_node[i].blk[0], CFS_BLOCK);
    }
    EXPECT(g_locked == 0 && !g_in_clone);
}

static void round_(void)
{
    memset(&g_fs, 0, sizeof(g_fs));
    memset(&g_mnt, 0, sizeof(g_mnt));
    g_fs.mnt = &g_mnt;
    g_fs.sb.version = pick(6) == 0 ? 4 + pick(4) : 8 + pick(8);
    g_fs.encrypted = pick(4) == 0;
    g_mnt.fs_priv = pick(40) == 0 ? NULL : &g_fs;
    g_mnt.flags = pick(10) == 0 ? MOUNT_RDONLY : 0;
    g_fs.failed = pick(40) == 0 ? -EIO : 0;
    g_cred.euid = (uint32_t)pick(1000);
    g_cred.egid = (uint32_t)pick(1000);
    g_clone.id = 9;
    g_clone_exists = pick(3) != 0;
    tree();
    unsigned ops = 1 + pick(8);
    for (unsigned k = 0; k < ops; k++)
        op();
    for (unsigned i = 0; i < NVN; i++) {
        if (g_vused[i])
            kfree(g_vn[i].fs_priv);
        g_vused[i] = false;
        g_cv[i] = NULL;
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
    printf("  dir digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, ROUNDS);
    EXPECT(g_digest == DIR_DIGEST);
}

int main(void)
{
    static const struct host_test tests[] = {
        { "dir_digest", test_digest },
    };
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
