/*
 * test_cfssnapctl.c - cosmofs's snapshots through /dev/fsctl (kernel-services/filesystem/cosmofs/cosmofs_snapctl.c)
 * on the host: roadmap 2.0 SA7. A digest recorded from the C over every operation and some that are not, with names
 * good and bad, on mounts read-only or not, failed or not: listings of none, a few or hundreds of snapshots, clones
 * among them, in any order, paged by a cursor, with and without sizes, with a rollback record pending, held or absent,
 * over trees whose walks reach blocks once, twice and from hundreds of roots -- while the walks, the list, the
 * rollback record and every operation underneath fail now and then, as does kmalloc. Every argument, return, record,
 * logged line and walk is in the digest; the lock is let go and nothing is left allocated.
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

/* The digest of the C (cosmofs_snapctl.c, 2026-11-07), which the CeeSafe port gives. */
#define SNAPCTL_DIGEST 0x82439c542373f871ull
#define ROUNDS 6000u
#define SEED 0x5AAC7C7E2026D00Dull

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

/* ---- the lock ---- */

static bool g_locked;

void mutex_lock(struct mutex *m)
{
    (void)m;
    EXPECT(!g_locked);
    g_locked = true;
    note(3, 0);
}

void mutex_unlock(struct mutex *m)
{
    (void)m;
    EXPECT(g_locked);
    g_locked = false;
    note(4, 0);
}

/* ---- the filesystem's snapshots and what reaches what ---- */

#define MAXSNAP 300u

static struct cfs g_fs;
static struct mount g_mnt;
static struct cfs_memstate g_mem[1];
static struct cfs_snapshot g_snap[MAXSNAP];
static unsigned g_nsnap;
static bool g_short_list;      /* the second listing finds fewer */
static int g_rb;               /* 0: none; 1: pending; 2: held; 3: does not read */
static uint8_t g_rb_target[16], g_rb_safety[16];
static bool g_wide;            /* every walk reaches block 0: hundreds of roots on one block */

static uint64_t hash(uint64_t x)
{
    x ^= x >> 31;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 29;
    return x;
}

int cfs_rollback_read(struct cfs *fs, struct cfs_rollback *out)
{
    EXPECT(fs == &g_fs && g_locked);
    note(10, (uint64_t)g_rb);
    if (g_rb == 0)
        return -ENOENT;
    if (g_rb == 3)
        return -EIO;
    memset(out, 0, sizeof(*out));
    out->state = g_rb == 1 ? CFS_ROLLBACK_PENDING : CFS_ROLLBACK_HELD;
    memcpy(out->target_uuid, g_rb_target, 16);
    memcpy(out->safety_uuid, g_rb_safety, 16);
    return 0;
}

int cfs_snapshot_list(struct cfs *fs, struct cfs_snapshot *out, unsigned max, unsigned *count)
{
    EXPECT(fs == &g_fs && g_locked);
    note(11, max);
    if (chance(20)) {
        note(12, 0);
        return -EIO;
    }
    unsigned n = g_nsnap;
    if (out != NULL && g_short_list && n > 0)
        n--;
    if (out != NULL) {
        EXPECT(max >= n);
        memcpy(out, g_snap, n * sizeof(*out));
    }
    *count = n;
    return 0;
}

/* A walk: blocks derived from the root, some twice, every one inside the pool. */
static int walk(uint64_t root, void (*mark)(void *arg, uint64_t lin), void *arg)
{
    note(13, root);
    if (chance(60)) {
        note(14, 0);
        return -EIO;
    }
    unsigned n = (unsigned)(hash(root) % 40);
    for (unsigned k = 0; k < n; k++) {
        uint64_t lin = hash(root * 131 + k / 2) % g_fs.nblocks;   /* k / 2: each block twice */
        mark(arg, lin);
    }
    if (g_wide)
        mark(arg, 0);
    return 0;
}

int cfs_tree_walk(struct cfs *fs, uint64_t imap_root, uint64_t next_ino, void (*mark)(void *arg, uint64_t lin),
                  void *arg)
{
    EXPECT(fs == &g_fs && g_locked);
    note(15, next_ino);
    return walk(imap_root, mark, arg);
}

int cfs_alloc_walk(struct cfs *fs, uint64_t root, void (*mark)(void *arg, uint64_t lin), void *arg)
{
    EXPECT(fs == &g_fs && g_locked);
    return walk(root ^ 0xA110C, mark, arg);
}

int cfs_snapshot_walk_held(struct cfs *fs, const struct cfs_snapshot *s, void (*mark)(void *arg, uint64_t lin),
                           void *arg)
{
    EXPECT(fs == &g_fs && g_locked);
    return walk(s->deadlist ^ 0xDEAD, mark, arg);
}

void cfs_tree_roots(struct cfs *fs, const struct cfs_snapshot *s, uint64_t *imap_root, uint64_t *next_ino)
{
    EXPECT(fs == &g_fs && g_locked);
    *imap_root = s->imap_root ^ ((s->flags & CFS_SNAP_F_CLONE) ? 0xC10E : 0);
    *next_ino = s->next_ino;
}

int cfs_sync_vnodes(struct cfs *fs)
{
    EXPECT(fs == &g_fs && !g_locked);
    note(16, 0);
    return chance(10) ? -EIO : 0;
}

static void made(struct cfs_snapshot *out, const char *name)
{
    memset(out, 0, sizeof(*out));
    out->id = 1 + pick(1000);
    out->generation = next();
    out->created_ns = next();
    out->origin_id = pick(3);
    for (unsigned i = 0; i < 16; i++)
        out->uuid[i] = (uint8_t)next();
    out->flags = pick(2) ? CFS_SNAP_F_CLONE : 0;
    snprintf(out->name, sizeof(out->name), "%s", name);
}

static int answer(void)
{
    unsigned w = pick(8);
    return w == 0 ? -EIO : w == 1 ? -ENOENT : w == 2 ? -EEXIST : 0;
}

int cfs_snapshot_create(struct cfs *fs, const char *name, bool identity, struct cfs_snapshot *out)
{
    EXPECT(fs == &g_fs && g_locked);
    mix(name, strlen(name));
    note(17, identity);
    int rc = answer();
    if (rc == 0)
        made(out, name);
    return rc;
}

int cfs_snap_by_name(struct cfs *fs, const char *name, size_t len, struct cfs_snapshot *out, unsigned *tag)
{
    EXPECT(fs == &g_fs && g_locked);
    mix(name, len);
    int rc = answer();
    if (rc == 0) {
        made(out, "found");
        *tag = 1 + pick(100);
    }
    return rc;
}

bool cfs_snap_in_use(struct mount *mnt, unsigned tag, const struct vnode *victim)
{
    EXPECT(mnt == &g_mnt && g_locked && victim == NULL);
    note(18, tag);
    return pick(4) == 0;
}

int cfs_snapshot_delete(struct cfs *fs, const char *name)
{
    EXPECT(fs == &g_fs && g_locked);
    mix(name, strlen(name));
    return answer();
}

int cfs_rollback_request(struct cfs *fs, const char *target, const char *safety_name, struct cfs_snapshot *safety)
{
    EXPECT(fs == &g_fs && g_locked);
    mix(target, strlen(target));
    note(19, safety_name != NULL);
    int rc = answer();
    if (rc == 0)
        made(safety, safety_name != NULL ? safety_name : "safety");
    return rc;
}

int cfs_clone_create(struct cfs *fs, const char *origin, const char *name, struct cfs_snapshot *out)
{
    EXPECT(fs == &g_fs && g_locked);
    mix(origin, strlen(origin));
    mix(name, strlen(name));
    int rc = answer();
    if (rc == 0)
        made(out, name);
    return rc;
}

int cfs_rollback_cancel(struct cfs *fs)
{
    EXPECT(fs == &g_fs && g_locked);
    note(20, 0);
    return answer();
}

int cfs_rollback_boot_ok(struct cfs *fs, bool *released)
{
    EXPECT(fs == &g_fs && g_locked);
    note(21, 0);
    *released = pick(2);
    return answer();
}

/* ---- a round ---- */

static const char *const k_names[] = {
    NULL, "", "a", "nightly", "x/y", ".", "..", "...", "/", "0123456789012345678901234567890",
    "01234567890123456789012345678901", "with space", "snap-2026-11-07",
};

static const char *some_name(void) { return k_names[pick(sizeof(k_names) / sizeof(k_names[0]))]; }

static void filesystem(void)
{
    memset(&g_fs, 0, sizeof(g_fs));
    memset(&g_mnt, 0, sizeof(g_mnt));
    memset(g_mem, 0, sizeof(g_mem));
    g_fs.mem = g_mem;
    g_fs.nmembers = 1;
    g_mnt.fs_priv = pick(30) == 0 ? NULL : &g_fs;
    g_mnt.flags = pick(6) == 0 ? MOUNT_RDONLY : 0;
    g_fs.failed = pick(20) == 0 ? -EIO : 0;
    g_fs.sb.version = pick(4) == 0 ? 3 : 4 + pick(12);
    g_fs.sb.imap_root = next();
    g_fs.sb.next_ino = pick(1000);
    g_fs.sb.members = next();
    g_mem[0].alloc_root = next();
    unsigned big = pick(10);
    if (big == 0) {
        g_fs.nblocks = 1 + pick(64);
    } else if (big == 1) {
        uint64_t chunks = 1 + pick(3);
        g_fs.nblocks = 4096 * chunks + pick(4096);
    } else {
        g_fs.nblocks = 64 + pick(9000);
    }
    g_fs.free_blocks = pick(10000);
    g_fs.reserve = pick(200);
    g_fs.rollback_error = pick(3) == 0 ? -EIO : 0;
    unsigned many = pick(30);
    g_nsnap = many == 0 ? 256 + pick(MAXSNAP - 256) : pick(8);
    g_wide = many == 0 || pick(10) == 0;
    for (unsigned i = 0; i < g_nsnap; i++) {
        struct cfs_snapshot *s = &g_snap[i];
        memset(s, 0, sizeof(*s));
        s->id = 1 + pick(500);
        s->generation = next();
        s->imap_root = pick(6) == 0 && i > 0 ? g_snap[i - 1].imap_root : next();
        s->next_ino = pick(1000);
        s->deadlist = pick(3) == 0 ? 0 : next();
        s->created_ns = next();
        s->origin_id = pick(4) == 0 ? 1 + pick(10) : 0;
        for (unsigned k = 0; k < 16; k++)
            s->uuid[k] = (uint8_t)next();
        s->flags = pick(4) == 0 ? CFS_SNAP_F_CLONE : 0;
        snprintf(s->name, sizeof(s->name), "s%u", i);
    }
    g_short_list = pick(25) == 0;
    g_rb = (int)pick(4);
    for (unsigned k = 0; k < 16; k++) {
        g_rb_target[k] = (uint8_t)next();
        g_rb_safety[k] = (uint8_t)next();
    }
    if (g_nsnap > 0 && pick(2))
        memcpy(g_rb_target, g_snap[pick(g_nsnap)].uuid, 16);
    if (g_nsnap > 0 && pick(2))
        memcpy(g_rb_safety, g_snap[pick(g_nsnap)].uuid, 16);
}

static void op(void)
{
    struct cosmofs_snap_req req;
    memset(&req, 0, sizeof(req));
    unsigned o = pick(9);
    req.op = (enum cosmofs_snap_op)o;   /* 0 and 8 are no operation */
    if (pick(3) == 0)
        req.op = COSMOFS_SNAP_LIST;
    req.name = some_name();
    req.name2 = some_name();
    static struct cosmofs_snap_info out[MAXSNAP];
    memset(out, 0, sizeof(out));
    req.out = pick(8) == 0 ? NULL : out;
    unsigned mx = pick(4);
    req.max = mx == 0 ? 0 : mx == 1 ? 1 + pick(3) : MAXSNAP;
    if (req.out == NULL && req.op == COSMOFS_SNAP_LIST)
        req.max = 0;   /* a listing's records go where `out` says: fsctl always gives one with room */
    req.no_sizes = pick(3) == 0;
    req.after = pick(3) == 0 ? pick(500) : 0;
    unsigned kf = pick(6);
    g_fail_in = g_faults && kf == 0 ? (int)pick(8) : -1;
    note(50, req.op);
    int rc = cosmofs_snapctl(&g_mnt, &req);
    g_fail_in = -1;
    note(51, (uint64_t)(int64_t)rc);
    note(52, req.count);
    note(53, req.total);
    note(54, req.format);
    note(55, req.pool_flags);
    note(56, req.free_bytes);
    note(57, req.total_bytes);
    note(58, (uint64_t)(int64_t)req.rollback_error);
    if (req.out != NULL) {
        unsigned n = req.count < MAXSNAP ? req.count : MAXSNAP;
        mix(out, n * sizeof(out[0]));
    }
    EXPECT(!g_locked);
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
        filesystem();
        op();
    }
    harness_klog_hook = NULL;
    harness_klog_min = KLOG_WARN;
    printf("  snapctl digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, ROUNDS);
    EXPECT(g_digest == SNAPCTL_DIGEST);
}

int main(void)
{
    static const struct host_test tests[] = {
        { "snapctl_digest", test_digest },
    };
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
