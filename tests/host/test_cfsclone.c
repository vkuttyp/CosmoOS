/*
 * test_cfsclone.c - cosmofs's writable clones (kernel-services/filesystem/cosmofs/cosmofs_clone.c) on the host:
 * roadmap 2.0 SA7. A digest recorded from the C over seeded rounds against a snapshot list, trees and a transaction
 * the test owns: clones created (of snapshots, of clones, of nothing, under names taken, on version-11 roots, with the
 * commits before and after failing), loaded from the list, entered and left with their roots moving, blocks freed
 * inside them (the origin's, the clone's own, ones whose origin cannot be walked), published through the list's
 * write walk, made ready and deleted, and the lookups. Every return, every block freed, every commit, every entry
 * written back, every logged line and the clones' state are in the digest; nothing is left allocated.
 */

#include "harness.h"

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>

#include "cosmofs_internal.h"   /* before the C library's headers, as test_cfsparse.c: its signal.h is the kernel's */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The values the CeeSafe port writes as its own constants. */
_Static_assert(EIO == 5 && ENOENT == 2 && EINVAL == 22 && EEXIST == 17 && ENOMEM == 12 && EOPNOTSUPP == 95 &&
                   CFS_SNAP_F_CLONE == 1 && CFS_DVA_NONE == 0xFFFFFFFFFFFFFFFFull,
               "cosmofs_clone's errno.h and cosmofs_format.h values");

/* The digest of the C (cosmofs_clone.c, 2026-11-06), which the CeeSafe port gives. */
#define CLONE_DIGEST 0xf455c79b8ec77804ull
#define ROUNDS 80000u
#define SEED 0xC05F5FA2026B0016ull

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

static bool fail_now(size_t size)
{
    if (g_fail_in >= 0 && g_fail_in-- == 0) {
        note(2, size);
        return true;
    }
    return false;
}

void *kmalloc(size_t size, unsigned flags)
{
    if (fail_now(size))
        return NULL;
    void *p = (flags & KMEM_ZERO) ? calloc(1, size) : malloc(size);
    if (p != NULL)
        g_live++;
    return p;
}

void *kzalloc(size_t size) { return kmalloc(size, KMEM_ZERO); }

void *krealloc(void *ptr, size_t new_size, unsigned flags)
{
    (void)flags;
    if (fail_now(new_size))
        return NULL;
    void *p = realloc(ptr, new_size);
    if (p != NULL && ptr == NULL)
        g_live++;
    return p;
}

void kfree(void *p)
{
    if (p != NULL)
        g_live--;
    free(p);
}

/* ---- the pool's addresses: member 0's blocks, every 50th a padding index with no address ---- */

#define NB 300u

uint64_t cfs_dva_lin(const struct cfs *fs, uint64_t dva)
{
    (void)fs;
    return CFS_DVA_VDEV(dva) == 0 && CFS_DVA_BLK(dva) < NB ? CFS_DVA_BLK(dva) : CFS_DVA_NONE;
}

uint64_t cfs_lin_dva(const struct cfs *fs, uint64_t lin)
{
    (void)fs;
    return lin < NB && lin % 50 != 49 ? CFS_DVA(0, lin) : CFS_DVA_NONE;
}

/* ---- the trees: what a root reaches follows from the root; one root in seven cannot be walked whole ---- */

static uint64_t mixhash(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    return x;
}

int cfs_tree_walk(struct cfs *fs, uint64_t imap_root, uint64_t next_ino, void (*mark)(void *arg, uint64_t lin),
                  void *arg)
{
    (void)fs;
    note(10, imap_root);
    note(11, next_ino);
    if (imap_root == 0)
        return 0;
    unsigned n = (unsigned)(mixhash(imap_root) % 40);
    for (unsigned k = 0; k < n; k++)
        mark(arg, mixhash(imap_root * 131 + k) % NB);
    return imap_root % 7 == 0 ? -EIO : 0;
}

/* ---- the snapshot list and the transaction ---- */

#define MAXS 24

static struct cfs_snapshot g_snaps[MAXS];
static unsigned g_nsnaps;
static uint64_t g_max_id;
static unsigned g_fail_pct;   /* of the list's and the transaction's operations that fail */

static int draw_rc(void)
{
    unsigned r = pick(100);
    if (r >= g_fail_pct)
        return 0;
    return pick(2) == 0 ? -EIO : -ENOMEM;
}

int cfs_snapshot_list(struct cfs *fs, struct cfs_snapshot *out, unsigned max, unsigned *count)
{
    (void)fs;
    int rc = draw_rc();
    note(20, (uint64_t)(int64_t)rc);
    if (rc)
        return rc;
    if (out == NULL) {
        *count = g_nsnaps;
        return 0;
    }
    unsigned n = g_nsnaps < max ? g_nsnaps : max;
    memcpy(out, g_snaps, n * sizeof(*out));
    *count = n;
    return 0;
}

bool cfs_snapshot_find(struct cfs *fs, const char *name, struct cfs_snapshot *out)
{
    (void)fs;
    for (unsigned i = 0; i < g_nsnaps; i++)
        if (strcmp(g_snaps[i].name, name) == 0) {
            if (out)
                *out = g_snaps[i];
            return true;
        }
    return false;
}

static void add_entry(const char *name, uint32_t flags, uint64_t origin_id, uint64_t imap_root)
{
    struct cfs_snapshot *s = &g_snaps[g_nsnaps++];
    memset(s, 0, sizeof(*s));
    s->id = ++g_max_id;
    s->flags = flags;
    s->origin_id = origin_id;
    s->imap_root = imap_root;
    s->next_ino = 1 + pick(500);
    s->inode_count = pick(400);
    s->generation = pick(1000);
    snprintf(s->name, sizeof(s->name), "%s", name);
}

int cfs_snapshot_add_clone(struct cfs *fs, const struct cfs_snapshot *origin, const char *name,
                           struct cfs_snapshot *out)
{
    (void)fs;
    int rc = draw_rc();
    note(21, (uint64_t)(int64_t)rc);
    if (rc)
        return rc;
    if (g_nsnaps == MAXS)
        return -ENOSPC;
    add_entry(name, CFS_SNAP_F_CLONE, origin->id, origin->imap_root);
    g_snaps[g_nsnaps - 1].next_ino = origin->next_ino;
    g_snaps[g_nsnaps - 1].inode_count = origin->inode_count;
    *out = g_snaps[g_nsnaps - 1];
    return 0;
}

int cfs_snapshot_list_cow(struct cfs *fs)
{
    (void)fs;
    int rc = draw_rc();
    note(22, (uint64_t)(int64_t)rc);
    return rc;
}

int cfs_snapshot_walk_write(struct cfs *fs, bool (*fn)(struct cfs_snapshot *s, void *arg), void *arg)
{
    (void)fs;
    int rc = draw_rc();
    note(23, (uint64_t)(int64_t)rc);
    if (rc)
        return rc;
    for (unsigned i = 0; i < g_nsnaps; i++)
        if (!fn(&g_snaps[i], arg)) {
            note(24, g_snaps[i].id);
            mix(&g_snaps[i], sizeof(g_snaps[i]));
            return 0;
        }
    return 0;
}

int cfs_commit(struct cfs *fs)
{
    int rc = draw_rc();
    fs->gen++;
    note(25, (uint64_t)(int64_t)rc);
    return rc;
}

void cfs_free_block_exempt(struct cfs *fs, uint64_t blk)
{
    (void)fs;
    note(26, blk);
}

/* ---- the rounds ---- */

static void dump(struct cfs *fs)
{
    note(30, fs->nclones);
    note(31, fs->cur_clone == NULL ? 0xff : (uint64_t)(fs->cur_clone - fs->clones));
    note(32, fs->sb.imap_root);
    note(33, fs->sb.next_ino);
    note(34, fs->sb.inode_count);
    note(35, fs->live_imap_root);
    note(36, fs->live_next_ino);
    note(37, fs->live_inode_count);
    note(38, (uint64_t)(int64_t)fs->failed);
    for (unsigned i = 0; i < fs->nclones; i++) {
        const struct cfs_clone *c = &fs->clones[i];
        note(39, c->id);
        note(40, c->origin_id);
        note(41, c->imap_root);
        note(42, c->next_ino);
        note(43, c->inode_count);
        note(44, c->dirty);
        note(45, c->origin_reach != NULL);
        if (c->origin_reach != NULL)
            mix(c->origin_reach, (NB + 7) / 8);
    }
}

static void name_of(char *out, size_t n)
{
    unsigned k = pick(3);
    if (k == 0 && g_nsnaps > 0)
        snprintf(out, n, "%s", g_snaps[pick(g_nsnaps)].name);
    else
        snprintf(out, n, "n%u", pick(60));
}

static void create(struct cfs *fs)
{
    char origin[32], name[32];
    name_of(origin, sizeof(origin));
    name_of(name, sizeof(name));
    if (pick(15) == 0)
        g_fail_in = (int)pick(2);
    struct cfs_snapshot out;
    memset(&out, 0, sizeof(out));
    struct cfs_snapshot *op = pick(4) == 0 ? NULL : &out;
    int rc = cfs_clone_create(fs, origin, name, op);
    g_fail_in = -1;
    note(50, (uint64_t)(int64_t)rc);
    if (rc == 0 && op != NULL)
        note(51, out.id);
}

static void delete_one(struct cfs *fs)
{
    if (g_nsnaps == 0)
        return;
    unsigned i = pick(g_nsnaps);
    struct cfs_snapshot doomed = g_snaps[i];
    /* Ready first, as the snapshot code does -- or not, now and then: the delete refuses by itself. */
    unsigned skip = pick(4);
    int rc = skip == 0 ? 0 : cfs_clone_ready(fs, doomed.id);
    note(52, (uint64_t)(int64_t)rc);
    if (rc)
        return;
    uint64_t freed = 0xdead;
    rc = cfs_clone_delete(fs, &doomed, &freed);
    note(53, (uint64_t)(int64_t)rc);
    note(54, freed);
    if (rc == 0) {
        memmove(&g_snaps[i], &g_snaps[i + 1], (g_nsnaps - i - 1) * sizeof(g_snaps[0]));
        g_nsnaps--;
    }
}

static void inside(struct cfs *fs)
{
    if (fs->nclones == 0)
        return;
    struct cfs_clone *c = &fs->clones[pick(fs->nclones)];
    cfs_clone_enter(fs, c);
    unsigned ops = pick(6);
    for (unsigned k = 0; k < ops; k++) {
        unsigned what = pick(4);
        if (what == 0) {
            fs->sb.imap_root = 1 + pick(400);
            fs->sb.next_ino += pick(3);
            fs->sb.inode_count += pick(2);
        } else {
            unsigned lin = pick(NB + 5);
            uint64_t dva = pick(20) == 0 ? CFS_DVA(1, lin) : CFS_DVA(0, lin);
            if (pick(30) == 0)
                g_fail_in = (int)pick(2);
            cfs_clone_free(fs, dva);
            g_fail_in = -1;
        }
    }
    if (pick(8) == 0)
        cfs_clone_enter(fs, &fs->clones[pick(fs->nclones)]);   /* one at a time: ignored */
    cfs_clone_leave(fs);
    if (pick(8) == 0) {
        cfs_clone_leave(fs);   /* outside every clone: nothing */
        cfs_clone_free(fs, CFS_DVA(0, pick(NB)));
    }
}

static void test_digest(void)
{
    g_rng = SEED;
    g_digest = 0xcbf29ce484222325ull;
    harness_klog_hook = on_klog;
    harness_klog_min = KLOG_ERROR + 1;   /* the commits' failures are expected, by the hundred */
    struct cfs *fs = calloc(1, sizeof(*fs));
    EXPECT(fs != NULL);
    fs->nblocks = NB;
    for (unsigned round = 0; round < ROUNDS; round++) {
        if (round % 4000 == 0) {
            /* A new filesystem: its list, its version, its failure rate. */
            cfs_clones_free(fs);
            g_nsnaps = 0;
            g_max_id = 0;
            fs->failed = 0;
            fs->sb.version = round / 4000 % 4 == 1 ? 11 : 12 + pick(4);   /* every fourth a version-11 root */
            unsigned f = pick(3);
            g_fail_pct = f == 0 ? 0 : f == 1 ? 4 : 20;
            fs->sb.imap_root = 1 + pick(400);
            fs->sb.next_ino = 1 + pick(500);
            fs->sb.inode_count = pick(400);
        }
        unsigned what = pick(100);
        if (what < 6) {
            if (g_nsnaps < MAXS) {
                char name[32];
                snprintf(name, sizeof(name), "n%u", pick(60));
                add_entry(name, 0, 0, 1 + pick(400));
            }
        } else if (what < 18) {
            create(fs);
        } else if (what < 22) {
            if (pick(10) == 0)
                g_fail_in = (int)pick(2);
            note(55, (uint64_t)(int64_t)cfs_clones_load(fs));
            g_fail_in = -1;
        } else if (what < 52) {
            inside(fs);
        } else if (what < 62) {
            note(56, (uint64_t)(int64_t)cfs_clones_publish(fs));
        } else if (what < 70) {
            delete_one(fs);
        } else if (what < 85) {
            uint64_t id = pick(4) == 0 ? 1000 : 1 + pick((unsigned)g_max_id + 1);
            note(57, cfs_snapshot_has_clones(fs, id));
            struct cfs_clone *c = cfs_clone_by_id(fs, id);
            note(58, c == NULL ? 0xff : (uint64_t)(c - fs->clones));
            if (g_nsnaps > 0) {
                uint64_t root = 0, ino = 0;
                cfs_tree_roots(fs, &g_snaps[pick(g_nsnaps)], &root, &ino);
                note(59, root);
                note(60, ino);
            }
        } else if (what < 87) {
            cfs_clones_free(fs);
        } else {
            dump(fs);
        }
    }
    dump(fs);
    cfs_clones_free(fs);
    free(fs);
    EXPECT(g_live == 0);
    harness_klog_hook = NULL;
    harness_klog_min = KLOG_WARN;
    printf("  clone digest 0x%016llx over %u rounds\n", (unsigned long long)g_digest, ROUNDS);
    EXPECT(g_digest == CLONE_DIGEST);
}

int main(void)
{
    static const struct host_test tests[] = {
        { "clone_digest", test_digest },
    };
    return harness_run(tests, sizeof(tests) / sizeof(tests[0]));
}
