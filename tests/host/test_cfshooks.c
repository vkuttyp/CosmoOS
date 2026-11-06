/*
 * test_cfshooks.c - cosmofs's stats, key operations and test hooks (kernel-services/filesystem/cosmofs/cosmofs_hooks.c)
 * on the host: roadmap 2.0 SA7. A digest recorded from the C over every hook, on mounts with and without a
 * filesystem behind them, of every version, one member or several with their copies online or not, encrypted or not,
 * with the key or without, read-only or not -- while the orphan reservation, the key loads and rotations, inode reads,
 * block maps and deadlist walks answer what the test says. Every argument, return, figure and field a hook sets is in
 * the digest; the lock is let go every time.
 */

#include "harness.h"

#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/log.h>
#include <kernel/mutex.h>
#include <kernel/storage.h>
#include <kernel/vfs.h>

#include "cosmofs_internal.h"   /* before the C library's headers, as test_cfsparse.c: its signal.h is the kernel's */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The digest of the C (cosmofs_hooks.c, 2026-11-07), which the CeeSafe port gives. */
#define HOOKS_DIGEST 0x677b20b102df3691ull
#define ROUNDS 30000u
#define SEED 0x4F0C5A7E2026B00Cull

static uint64_t g_rng, g_digest;

static uint64_t next(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static unsigned pick(unsigned n) { return (unsigned)(next() % n); }

/* A value of any width: one draw, then its shift (two draws in one expression are not sequenced). */
static uint64_t wide(void)
{
    uint64_t v = next();
    return v >> pick(64);
}

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

/* ---- the filesystem and what it calls ---- */

#define MEMBERS 4u

static struct cfs g_fs;
static struct mount g_mnt;
static struct spool g_pool;
static struct cfs_memstate g_mem[MEMBERS];
static unsigned g_copies[MEMBERS];
static bool g_online[MEMBERS][4];
static int g_answer;        /* what the next call that returns a status returns */
static uint64_t g_value;    /* and what it hands back */

static uint8_t g_key[64];

bool cfs_orphan_reserve(struct cfs *fs)
{
    EXPECT(fs == &g_fs && g_locked);
    note(10, 0);
    return g_answer >= 0;
}

void cfs_orphan_add(struct cfs *fs, uint64_t ino)
{
    EXPECT(fs == &g_fs && g_locked);
    note(11, ino);
}

int cfs_keys_load(struct cfs *fs, const void *user_key, size_t user_len)
{
    EXPECT(fs == &g_fs && g_locked);
    note(12, user_len);
    if (user_key != NULL)
        mix(user_key, user_len);
    if (g_answer == 0)
        fs->have_key = true;
    return g_answer;
}

int cfs_keys_rotate(struct cfs *fs, const void *new_key, size_t new_len)
{
    EXPECT(fs == &g_fs && g_locked);
    note(13, new_len);
    if (new_key != NULL)
        mix(new_key, new_len);
    return g_answer;
}

int cfs_inode_read(struct cfs *fs, uint64_t ino, struct cfs_inode *out)
{
    EXPECT(fs == &g_fs && g_locked);
    note(14, ino);
    if (g_answer < 0)
        return g_answer;
    memset(out, 0, sizeof(*out));
    out->ino = ino;
    out->size = g_value;
    return 0;
}

int cfs_map(struct cfs *fs, const struct cfs_inode *in, uint64_t lblk, uint64_t *pblk)
{
    EXPECT(fs == &g_fs && g_locked);
    note(15, in->ino);
    note(16, lblk);
    unsigned w = pick(4);
    if (w == 0)
        return -EIO;
    if (w == 1)
        return 0;                                   /* a hole */
    *pblk = in->size + lblk;
    return w == 2 ? 1 : -1;                         /* mapped; or a record's block */
}

uint64_t cfs_snapshot_deadlist_len(struct cfs *fs, uint64_t of)
{
    EXPECT(fs == &g_fs && g_locked);
    note(17, of);
    return g_value;
}

uint64_t cfs_snapshot_deadlist_dups(struct cfs *fs, uint64_t *examined, uint64_t *first)
{
    EXPECT(fs == &g_fs && g_locked);
    note(18, (uint64_t)(examined != NULL) | (uint64_t)(first != NULL) << 1);
    if (examined != NULL)
        *examined = g_value * 3;
    if (first != NULL && pick(2))
        *first = g_value + 7;
    return g_value;
}

unsigned pool_copies(const struct spool *p, uint64_t dva)
{
    EXPECT(p == &g_pool);
    unsigned v = CFS_DVA_VDEV(dva);
    EXPECT(v < MEMBERS);
    EXPECT((dva & CFS_DVA_BLK_MASK) == 0);
    note(19, v);
    return g_copies[v];
}

bool pool_copy_online(const struct spool *p, unsigned vdev, unsigned c)
{
    EXPECT(p == &g_pool);
    EXPECT(vdev < MEMBERS && c < 4);
    note(20, (uint64_t)vdev << 8 | c);
    return g_online[vdev][c];
}

unsigned cfs_copies_out(struct cfs *fs)
{
    EXPECT(fs == &g_fs && g_locked);
    note(21, 0);
    return (unsigned)g_value & 7;
}

/* ---- a round ---- */

static void filesystem(void)
{
    memset(&g_fs, 0, sizeof(g_fs));
    memset(&g_mnt, 0, sizeof(g_mnt));
    g_fs.pool = &g_pool;
    g_fs.mem = g_mem;
    g_fs.nmembers = 1 + pick(MEMBERS);
    for (unsigned v = 0; v < MEMBERS; v++) {
        g_mem[v].free_blocks = wide();
        g_copies[v] = 1 + pick(4);
        for (unsigned c = 0; c < 4; c++)
            g_online[v][c] = pick(4) != 0;
    }
    g_fs.sb.version = 2 + pick(14);
    g_fs.sb.generation = next();
    g_fs.sb.total_blocks = next();
    g_fs.sb.inode_count = next();
    g_fs.sb.free_root = next();
    g_fs.sb.orphan_root = next();
    g_fs.sb.snap_root = next();
    g_fs.free_blocks = next();
    g_fs.nr_orphans = (unsigned)next();
    g_fs.nr_dirty = (unsigned)next();
    g_fs.nr_pending = (unsigned)next();
    g_fs.nr_exempt = (unsigned)next();
    g_fs.reserve = next();
    g_fs.commits = next();
    g_fs.wb_commits = next();
    g_fs.csum_failures = next();
    g_fs.repairs = next();
    g_fs.snap_walks = next();
    g_fs.snap_verdicts = next();
    g_fs.mount_dirty_notes = next();
    g_fs.wb_early = next();
    g_fs.encrypted = pick(3) != 0;
    g_fs.have_key = pick(2) != 0;
    g_fs.mac.on = pick(3) == 0;
    g_fs.rdonly = pick(4) == 0;
    g_fs.wb_enabled = pick(2) != 0;
    g_fs.wb_interval_ms = (unsigned)next();
    g_mnt.fs_priv = pick(10) == 0 ? NULL : &g_fs;
}

/* What every hook may have set. */
static void fields(void)
{
    note(60, g_fs.test_fail_freelog);
    note(61, g_fs.test_fail_snapfill);
    note(62, g_fs.test_fail_orphan);
    note(63, g_fs.discard_on_unmount);
    note(64, g_fs.wb_enabled);
    note(65, g_fs.wb_interval_ms);
    note(66, g_fs.have_key);
#if CONFIG_DEBUG
    note(67, g_fs.test_commit_armed);
    note(68, g_fs.test_commits_left);
    note(69, g_fs.test_no_raise);
#endif
#if CONFIG_SELFTEST
    note(70, g_fs.test_max_extents);
#endif
}

static void op(void)
{
    unsigned which = pick(22);
    unsigned a = pick(3);
    g_answer = a == 0 ? -EIO : a == 1 ? -EAGAIN : 0;
    if (pick(2))
        g_answer = 0;
    g_value = wide();
    bool on = pick(2) != 0;
    uint64_t arg = wide();
    note(50, which);
    int rc = 0;
    uint64_t r = 0;
    switch (which) {
    case 0: {
        struct cosmofs_stats st;
        memset(&st, 0xA5, sizeof(st));
        rc = cosmofs_stats(&g_mnt, &st);
        if (rc == 0) {
            note(51, st.generation);
            note(51, st.free_blocks);
            note(51, st.total_blocks);
            note(51, st.inode_count);
            note(51, st.dirty_buffers);
            note(51, st.pending_frees);
            note(51, st.reserve_blocks);
            note(51, st.commits);
            note(51, st.wb_commits);
            note(51, st.csum_failures);
            note(51, st.members);
            note(51, st.devices);
            note(51, st.repairs);
            note(51, st.degraded);
            note(51, st.version);
            note(51, st.free_root);
            note(51, st.orphan_root);
            note(51, st.pending_orphans);
        }
        break;
    }
    case 1:
        cosmofs_test_fail_freelog(&g_mnt, on);
        break;
    case 2:
        cosmofs_test_fail_snapfill(&g_mnt, on);
        break;
    case 3:
        cosmofs_test_orphan_add(&g_mnt, arg);
        break;
    case 4:
        cosmofs_test_fail_orphan(&g_mnt, on);
        break;
    case 5:
        r = cosmofs_test_snap_walks(&g_mnt);
        break;
    case 6:
        r = cosmofs_test_snap_verdicts(&g_mnt);
        break;
#if CONFIG_DEBUG
    case 7:
        cosmofs_test_commit_budget(&g_mnt, on, (unsigned)arg);
        break;
    case 8:
        cosmofs_test_snap_no_raise(&g_mnt, on);
        break;
#endif
    case 9:
        cosmofs_test_discard_on_unmount(&g_mnt, on);
        break;
    case 10:
    case 11: {
        size_t len = pick(sizeof(g_key) + 1);
        const void *key = pick(8) == 0 ? NULL : g_key;
        rc = which == 10 ? cosmofs_test_unlock(&g_mnt, key, len) : cosmofs_rekey(&g_mnt, key, len);
        break;
    }
    case 12: {
        uint64_t dva = 0xDDDD;
        rc = cosmofs_test_block_of(&g_mnt, arg, g_value ^ 0x55, &dva);
        note(52, dva);
        break;
    }
    case 13:
        r = cosmofs_test_deadlist_len(&g_mnt, arg);
        break;
    case 14: {
        uint64_t examined = 0xEEEE, first = 0xFFFF;
        unsigned w = pick(4);
        r = cosmofs_test_deadlist_dups(&g_mnt, (w & 1) ? &examined : NULL, (w & 2) ? &first : NULL);
        note(53, examined);
        note(54, first);
        break;
    }
    case 15:
        r = cosmofs_test_snap_root(&g_mnt);
        break;
    case 16:
        r = cosmofs_test_member_free(&g_mnt, pick(MEMBERS + 2));
        break;
    case 17: {
        uint64_t notes = 0x1111, early = 0x2222;
        unsigned w = pick(4);
        cosmofs_test_mount_writeback(&g_mnt, (w & 1) ? &notes : NULL, (w & 2) ? &early : NULL);
        note(55, notes);
        note(56, early);
        break;
    }
    case 18:
        r = cosmofs_test_pool(&g_mnt) == &g_pool ? 1 : cosmofs_test_pool(&g_mnt) == NULL ? 2 : 3;
        break;
    case 19:
        cosmofs_test_set_writeback(&g_mnt, on);
        break;
    case 20:
        cosmofs_test_set_writeback_interval(&g_mnt, (unsigned)arg);
        break;
    default:
        cosmofs_test_set_max_extents(&g_mnt, (unsigned)arg);
        break;
    }
    note(57, (uint64_t)(int64_t)rc);
    note(58, r);
    fields();
    EXPECT(!g_locked);
}

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    harness_klog_hook = on_klog;
    harness_klog_min = KLOG_ERROR + 1;
    for (unsigned i = 0; i < sizeof(g_key); i++)
        g_key[i] = (uint8_t)(i * 7 + 1);
    for (unsigned round = 0; round < ROUNDS; round++) {
        filesystem();
        op();
    }
    harness_klog_hook = NULL;
    harness_klog_min = KLOG_WARN;
    printf("  hooks digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, ROUNDS);
    EXPECT(g_digest == HOOKS_DIGEST);
}

int main(void)
{
    static const struct host_test tests[] = {
        { "hooks_digest", test_digest },
    };
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
