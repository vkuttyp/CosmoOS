/*
 * cosmofscrash.c - The cosmofs crash-consistency harness, self-test
 * `cosmofs-replay` (docs/verification/design.md, "Crash consistency").
 *
 * Record the write stream of a workload on a RAM device, then replay every
 * prefix of it (and torn last writes) onto the pre-workload image and
 * require each result to mount, to contain every file the workload had
 * committed by that point with its exact contents, and to walk and read
 * without error.
 */

#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/printf.h>
#include <kernel/ramblk.h>
#include <kernel/selftest.h>
#include <kernel/string.h>
#include <kernel/timer.h>
#include <kernel/vfs.h>

#include <uapi/cosmo/syscall.h>

#define STR_(x) #x
#define STR(x)  STR_(x)
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            *reason = "check failed: " #cond " at line " STR(__LINE__);        \
            return false;                                                      \
        }                                                                      \
    } while (0)

#if CONFIG_DEBUG

#define MNT        "/mnt/crash"
#define NBLOCKS    512u          /* 2 MiB device */
#define MAX_LOG    4096u
#define MAX_FILES  32u
#define MAX_SYNCS  16u

/* What the workload had committed at each sync point. */
struct expect {
    char path[32];
    uint32_t len;
    uint8_t seed;        /* contents: byte i = seed + i */
    bool present;
};

struct sync_point {
    unsigned log_len;    /* writes recorded when the sync returned */
    unsigned commit;     /* entries up to and including the last write that changes the device (trailing flushes excluded) */
    struct expect files[MAX_FILES];
    unsigned nr_files;
};

static struct sync_point g_syncs[MAX_SYNCS];
static unsigned g_nr_syncs;
static struct expect g_state[MAX_FILES];
static unsigned g_nr_state;

static bool write_file(const char *path, uint8_t seed, uint32_t len)
{
    struct file *f;
    if (vfs_open(NULL, path, COSMO_O_WRONLY | COSMO_O_CREAT | COSMO_O_TRUNC, 0644, &f) != 0)
        return false;
    uint8_t *buf = kmalloc(len ? len : 1, 0);
    if (buf == NULL) {
        file_put(f);
        return false;
    }
    for (uint32_t i = 0; i < len; i++)
        buf[i] = (uint8_t)(seed + i);
    bool ok = file_write(f, buf, len) == (int64_t)len;
    kfree(buf);
    file_put(f);
    return ok;
}

static bool read_matches(const char *path, uint8_t seed, uint32_t len)
{
    struct file *f;
    if (vfs_open(NULL, path, COSMO_O_RDONLY, 0, &f) != 0)
        return false;
    uint8_t *buf = kmalloc(len + 1, 0);
    if (buf == NULL) {
        file_put(f);
        return false;
    }
    int64_t n = file_read(f, buf, len + 1);
    bool ok = n == (int64_t)len;
    for (uint32_t i = 0; ok && i < len; i++)
        ok = buf[i] == (uint8_t)(seed + i);
    kfree(buf);
    file_put(f);
    return ok;
}

static struct expect *state_of(const char *path)
{
    for (unsigned i = 0; i < g_nr_state; i++)
        if (strcmp(g_state[i].path, path) == 0)
            return &g_state[i];
    if (g_nr_state == MAX_FILES)
        return NULL;
    struct expect *e = &g_state[g_nr_state++];
    memset(e, 0, sizeof(*e));
    strlcpy(e->path, path, sizeof(e->path));
    return e;
}

static bool wl_write(const char *path, uint8_t seed, uint32_t len)
{
    if (!write_file(path, seed, len))
        return false;
    struct expect *e = state_of(path);
    if (e == NULL)
        return false;
    e->seed = seed;
    e->len = len;
    e->present = true;
    return true;
}

static bool wl_unlink(const char *path)
{
    if (vfs_unlink(NULL, path) != 0)
        return false;
    struct expect *e = state_of(path);
    if (e)
        e->present = false;
    return true;
}

static bool wl_rename(const char *from, const char *to)
{
    if (vfs_rename(NULL, from, to) != 0)
        return false;
    struct expect *src = state_of(from), *dst = state_of(to);
    if (src == NULL || dst == NULL)
        return false;
    dst->seed = src->seed;
    dst->len = src->len;
    dst->present = true;
    src->present = false;
    return true;
}

static bool wl_sync(struct blkdev *bd)
{
    if (vfs_sync() != 0 || g_nr_syncs == MAX_SYNCS)
        return false;
    struct sync_point *sp = &g_syncs[g_nr_syncs++];
    sp->log_len = ramblk_record_count(bd);
    memcpy(sp->files, g_state, sizeof(g_state));
    sp->nr_files = g_nr_state;
    return true;
}

/* Walk every directory, read every regular file. */
struct walk_ctx {
    char path[64];
    unsigned files, dirs;
    bool ok;
};

struct dirent_list {
    struct {
        char name[32];
        enum vnode_type type;
    } e[64];
    unsigned n;
};

static int collect(void *arg, const char *name, size_t len, uint64_t ino, enum vnode_type type)
{
    struct dirent_list *l = arg;
    (void)ino;
    if (l->n == 64 || len >= sizeof(l->e[0].name))
        return 1;
    if ((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.'))
        return 0;
    memcpy(l->e[l->n].name, name, len);
    l->e[l->n].name[len] = '\0';
    l->e[l->n].type = type;
    l->n++;
    return 0;
}

static void walk(const char *dir, unsigned depth, struct walk_ctx *w)
{
    if (!w->ok || depth > 4)
        return;
    struct vnode *vn;
    if (vfs_lookup(NULL, dir, &vn) != 0) {
        w->ok = false;
        return;
    }
    struct dirent_list *l = kzalloc(sizeof(*l));
    if (l == NULL) {
        vnode_put(vn);
        w->ok = false;
        return;
    }
    uint64_t pos = 0;
    mutex_lock(&vn->lock);
    int rc = vn->ops->readdir ? vn->ops->readdir(vn, &pos, collect, l) : -ENOTSUP;
    mutex_unlock(&vn->lock);
    vnode_put(vn);
    if (rc != 0)
        w->ok = false;
    w->dirs++;
    for (unsigned i = 0; i < l->n && w->ok; i++) {
        char path[96];
        ksnprintf(path, sizeof(path), "%s/%s", dir, l->e[i].name);
        if (l->e[i].type == VNODE_DIR) {
            walk(path, depth + 1, w);
        } else {
            struct file *f;
            if (vfs_open(NULL, path, COSMO_O_RDONLY, 0, &f) != 0) {
                w->ok = false;
                break;
            }
            static uint8_t buf[4096];
            int64_t n;
            do {
                n = file_read(f, buf, sizeof(buf));
            } while (n > 0);
            if (n < 0)
                w->ok = false;
            file_put(f);
            w->files++;
        }
    }
    kfree(l);
}

static const struct sync_point *last_sync_within(unsigned entries)
{
    const struct sync_point *sp = NULL;
    for (unsigned s = 0; s < g_nr_syncs; s++)
        if (g_syncs[s].commit <= entries)
            sp = &g_syncs[s];
    return sp;
}

/* Every file the sync point recorded as present reads back with its
 * contents. On a mismatch, the offending entry and its stat result. */
static bool state_matches(const struct sync_point *sp, const struct expect **bad, int *stat_rc)
{
    for (unsigned i = 0; i < sp->nr_files; i++) {
        const struct expect *e = &sp->files[i];
        if (!e->present)
            continue;
        if (!read_matches(e->path, e->seed, e->len)) {   /* paths are recorded with the mount prefix */
            if (bad) {
                struct cosmo_stat st;
                *bad = e;
                *stat_rc = vfs_stat(NULL, e->path, &st);
            }
            return false;
        }
    }
    return true;
}

/* Mount a prefix image and check it against the last committed state. */
/* What the replays leaked, across every prefix: a crash strands the
 * blocks the last transaction freed, and this is how many. */
static uint64_t g_leak_total;        /* blocks a replayed prefix stranded: must be zero */
static unsigned g_prefixes_checked;  /* images the structural check actually ran on */
/* The snapshot half (docs/audit/next-subsystem-snap-deadlist.md): a
 * block named by two deadlist entries is what an interrupted keeper
 * write-back used to leave, and cosmofs_check cannot see it, so the
 * suite asks per prefix. `g_dead_seen` is how many entries it looked at
 * across every prefix: zero would mean the workload never produced a
 * deadlist and the whole check was vacuous. */
static uint64_t g_dead_dups;
static uint64_t g_dead_seen;

/* The snapshots the workload takes, by name: every one an image still
 * has must read through, because a snapshot's tree is an ordinary tree
 * and a prefix that lost half a snapshot-list change would not walk. */
static const char *const g_snapnames[] = { "s1", "s2" };
/* The mount at MNT, for the structural check. */
static struct mount *mount_of_mnt(void)
{
    struct vnode *vn;
    if (vfs_lookup(NULL, MNT, &vn))
        return NULL;
    struct mount *m = vn->mnt;
    vnode_put(vn);
    return m;
}

static bool check_prefix(struct blkdev *bd, const uint8_t *base, const struct ramblk_log *log, unsigned k,
                         bool torn, const char **why)
{
    ramblk_restore(bd, base);
    ramblk_replay(bd, log, k, torn);
    if (vfs_mount(MNT, "cosmofs", bd, 0) != 0) {
        *why = "prefix image does not mount";
        return false;
    }
    /* The last sync whose device-changing writes are all in this prefix
     * (a sync's trailing flush changes nothing, so a prefix that ends right
     * after the superblock write shows the new root). With a torn last
     * write, entry k-1 is incomplete: if it is a sync's commit write the
     * device may show either root, depending on which half carried the
     * superblock, so both states are accepted. */
    const struct sync_point *sp = last_sync_within(torn && k > 0 ? k - 1 : k);
    const struct sync_point *alt = torn ? last_sync_within(k) : sp;
    bool ok = true;
    if (sp && !state_matches(sp, NULL, NULL) && !(alt != sp && state_matches(alt, NULL, NULL))) {
        const struct expect *e = NULL;
        struct cosmo_stat st;
        int src = -ENOENT;
        state_matches(sp, &e, &src);
        if (e) {
            if (src == 0)
                vfs_stat(NULL, e->path, &st);
            kerror("cosmofs-replay: prefix %u: %s expected %u bytes (seed %u): stat %d size %llu; sync committed at log %u",
                   k, e->path, e->len, e->seed, src, (unsigned long long)(src == 0 ? st.size : 0), sp->commit);
        }
        *why = "a committed file is missing or wrong in a prefix image";
        ok = false;
    }
    if (ok) {
        struct walk_ctx w = { .ok = true };
        walk(MNT, 0, &w);
        if (!w.ok) {
            *why = "a prefix image does not walk or read cleanly";
            ok = false;
        }
    }
    /* Every snapshot this image still has, read back the same way: the
     * live walk never enters `.snapshots`, which is found by name only. */
    for (unsigned i = 0; ok && i < sizeof(g_snapnames) / sizeof(g_snapnames[0]); i++) {
        char path[96];
        ksnprintf(path, sizeof(path), "%s/.snapshots/%s", MNT, g_snapnames[i]);
        struct vnode *vn;
        if (vfs_lookup(NULL, path, &vn) != 0)
            continue;   /* this prefix predates it, or postdates its deletion */
        vnode_put(vn);
        struct walk_ctx w = { .ok = true };
        walk(path, 0, &w);
        if (!w.ok) {
            kerror("cosmofs-replay: prefix %u: snapshot %s does not walk or read", k, g_snapnames[i]);
            *why = "a snapshot in a prefix image does not walk or read cleanly";
            ok = false;
        }
    }
    /*
     * And the shape, not only the bytes: every replayed prefix is
     * **clean**.
     *
     * This assertion is back to its full strength. The fsck unit had to
     * weaken it -- "no finding a crash cannot explain" -- because a
     * block freed during a transaction kept its bitmap bit until the
     * commit *after* the one that made the new root durable, and a crash
     * leaves no next commit. It measured the cost at 162 of 199 prefixes
     * and 1912 blocks (docs/audit/next-subsystem-fsck.md).
     *
     * A root now records what it freed, and a mount finishes the job
     * (docs/audit/next-subsystem-unmount-leak.md), so a crash-consistent
     * image has nothing stranded and the suite can say so plainly. The
     * count is still gathered, because "zero" is a measurement and not
     * an assumption, and it must be zero.
     */
    if (ok) {
        struct cosmofs_check_report rep;
        int crc = cosmofs_check(mount_of_mnt(), &rep, 0);
        if (crc != 0 || !rep.clean) {
            kerror("cosmofs-replay: prefix %u: check rc %d: %llu leaked, %llu free-in-use, %llu cross-linked, "
                   "%llu bad nlink, %llu orphan, %llu dangling, %llu bad entries, %llu counters, %llu cycles, "
                   "%llu unreadable",
                   k, crc, (unsigned long long)rep.alloc_not_seen.count,
                   (unsigned long long)rep.seen_not_alloc.count, (unsigned long long)rep.dup.count,
                   (unsigned long long)rep.nlink_wrong.count, (unsigned long long)rep.orphan.count,
                   (unsigned long long)rep.dangling_entry.count, (unsigned long long)rep.dir_bad.count,
                   (unsigned long long)rep.counter_wrong.count, (unsigned long long)rep.chain_cycle.count,
                   (unsigned long long)rep.unreadable.count);
            kerror("cosmofs-replay: prefix %u: first offenders: leaked %llu, free-in-use %llu, unreadable %llu, "
                   "dir_bad %llu; snapshots seen %llu",
                   k, (unsigned long long)(rep.alloc_not_seen.named ? rep.alloc_not_seen.name[0] : 0),
                   (unsigned long long)(rep.seen_not_alloc.named ? rep.seen_not_alloc.name[0] : 0),
                   (unsigned long long)(rep.unreadable.named ? rep.unreadable.name[0] : 0),
                   (unsigned long long)(rep.dir_bad.named ? rep.dir_bad.name[0] : 0),
                   (unsigned long long)rep.snapshots_seen);
            *why = "a replayed prefix image is not clean";
            ok = false;
        } else {
            g_leak_total += rep.alloc_not_seen.count;   /* must stay zero */
            g_prefixes_checked++;
        }
    }
    /*
     * And the one the structural check cannot make. A deadlist's entries
     * are claimed as non-live, so a block named by two of them is not a
     * duplicate claim and not a finding -- the image mounts, reads and
     * checks perfectly, and the damage arrives two snapshot deletions
     * later under whatever the allocator has since put in the block.
     */
    if (ok) {
        uint64_t seen = 0, first = 0;
        uint64_t dups = cosmofs_test_deadlist_dups(mount_of_mnt(), &seen, &first);
        g_dead_seen += seen;
        g_dead_dups += dups;
        if (dups != 0) {
            kerror("cosmofs-replay: prefix %u: %llu deadlist entr%s duplicated, first block %llu", k,
                   (unsigned long long)dups, dups == 1 ? "y" : "ies", (unsigned long long)first);
            *why = "a replayed prefix has one block on two deadlists";
            ok = false;
        }
    }
    vfs_umount2(MNT, VFS_UMOUNT_FORCE);
    return ok;
}

bool selftest_cosmofs_replay(const char **reason)
{
    g_nr_syncs = 0;
    g_nr_state = 0;
    g_dead_dups = 0;
    g_dead_seen = 0;
    memset(g_state, 0, sizeof(g_state));
    unsigned vnodes0 = vfs_vnode_count();

    struct blkdev *bd = ramblk_create(NBLOCKS);
    CHECK(bd != NULL);
    CHECK(cosmofs_format(bd) == 0);
    uint8_t *base = ramblk_snapshot(bd);
    CHECK(base != NULL);
    int mk = vfs_mkdir(NULL, MNT, 0755);
    CHECK(mk == 0 || mk == -EEXIST);
    CHECK(vfs_mount(MNT, "cosmofs", bd, 0) == 0);
    {
        struct vnode *mv;
        CHECK(vfs_lookup(NULL, MNT, &mv) == 0);
        cosmofs_test_set_writeback(mv->mnt, false);   /* every root write in the log is one this workload asked for */
        vnode_put(mv);
    }
    ramblk_record_start(bd, MAX_LOG);

    /* The workload: creates, rewrites, a directory, renames, unlinks, with
     * a sync after each group. */
    CHECK(wl_write(MNT "/a", 1, 100));
    CHECK(wl_write(MNT "/b", 2, 5000));
    CHECK(wl_sync(bd));
    CHECK(wl_write(MNT "/a", 3, 9000));          /* rewrite, grows */
    CHECK(vfs_mkdir(NULL, MNT "/d", 0755) == 0);
    CHECK(wl_write(MNT "/d/c", 4, 300));
    CHECK(wl_sync(bd));
    CHECK(wl_rename(MNT "/b", MNT "/d/b2"));
    CHECK(wl_write(MNT "/e", 5, 40000));         /* many blocks */
    CHECK(wl_sync(bd));
    CHECK(wl_unlink(MNT "/a"));
    CHECK(wl_write(MNT "/d/c", 6, 100));         /* rewrite, shrinks */
    CHECK(wl_write(MNT "/f", 7, 1));
    CHECK(wl_sync(bd));
    CHECK(wl_rename(MNT "/f", MNT "/e"));        /* replaces e */
    CHECK(wl_sync(bd));
    /*
     * And the snapshot list, which until this unit was the one metadata
     * chain a commit edited where it lay
     * (docs/audit/next-subsystem-snap-deadlist.md). Two groups, because
     * the two hazards are in different writers:
     *
     *  - `s1` taken and then a file it holds unlinked, which is the
     *    commit's release loop appending to a deadlist;
     *  - `s2` taken and `s1` deleted, which is the settle's keeper
     *    write-back and the cleared entry -- the two writes that run
     *    *inside* a transaction and that a crash could show to the root
     *    before it.
     *
     * Without them the suite replayed every prefix of a filesystem that
     * had never taken a snapshot, and reported them all clean.
     */
    CHECK(vfs_mkdir(NULL, MNT "/.snapshots/s1", 0755) == 0);
    CHECK(wl_unlink(MNT "/d/c"));                /* blocks s1 holds leave the live tree */
    CHECK(wl_sync(bd));
    CHECK(vfs_mkdir(NULL, MNT "/.snapshots/s2", 0755) == 0);
    CHECK(vfs_rmdir(NULL, MNT "/.snapshots/s1") == 0);
    CHECK(wl_sync(bd));
    /*
     * And an inode whose last name goes while a handle still holds it,
     * across a sync -- so the prefixes from here on carry a live orphan
     * record, and a crash in the middle of one is the case that used to
     * lose the inode and its blocks for good
     * (docs/audit/next-subsystem-orphan.md).
     */
    CHECK(wl_write(MNT "/tmpfile", 9, 3000));
    CHECK(wl_sync(bd));
    struct file *held = NULL;
    CHECK(vfs_open(NULL, MNT "/tmpfile", COSMO_O_RDONLY, 0, &held) == 0);
    CHECK(wl_unlink(MNT "/tmpfile"));
    CHECK(wl_sync(bd));
    file_put(held);
    CHECK(vfs_umount(MNT) == 0);                 /* the final commit */
    struct ramblk_log *log = ramblk_record_stop(bd);
    CHECK(log != NULL && log->dropped == 0 && log->n > 0);

    for (unsigned s = 0; s < g_nr_syncs; s++) {
        unsigned c = g_syncs[s].log_len;
        while (c > 0 && log->w[c - 1].nsectors == 0)
            c--;
        g_syncs[s].commit = c;
        kdebug("cosmofs-replay: sync %u committed at log entry %u (returned at %u)", s, c, g_syncs[s].log_len);
    }

    /* Every prefix when the log is short; otherwise every entry around each
     * sync point and a stride elsewhere. Both intact and torn last writes. */
    unsigned checked = 0;
    const char *why = NULL;
    bool ok = true;
    unsigned stride = log->n <= 256 ? 1 : (log->n + 127) / 128;
    for (unsigned k = 0; k <= log->n && ok; k++) {
        bool near_sync = false;
        for (unsigned s = 0; s < g_nr_syncs; s++)
            if (k + 4 >= g_syncs[s].log_len && k <= g_syncs[s].log_len + 4)
                near_sync = true;
        if (!(near_sync || k % stride == 0 || k == log->n))
            continue;
        bool torn_variant = false;
        ok = check_prefix(bd, base, log, k, false, &why);
        checked++;
        if (ok && k > 0 && log->w[k - 1].nsectors > 1) {
            torn_variant = true;
            ok = check_prefix(bd, base, log, k, true, &why);
            checked++;
        }
        if (!ok)
            kerror("cosmofs-replay: prefix %u of %u writes (last write %s): %s", k, log->n,
                   torn_variant ? "torn" : "intact", why);
    }
    unsigned writes = log->n;
    ramblk_log_free(log);
    kfree(base);
    CHECK(vfs_rmdir(NULL, MNT) == 0);
    ramblk_destroy(bd);
    CHECK(ok);
    CHECK(vfs_vnode_count() == vnodes0);
    /*
     * The structural check ran on these images, and found nothing.
     * Both halves are asserted: a suite that stopped asking would pass
     * in silence (what the fsck unit's no-crash-check bug-proof showed),
     * and a suite that asked and found something would have a crash
     * stranding blocks again.
     */
    CHECK(g_prefixes_checked == checked);
    CHECK(g_leak_total == 0);
    /* Both halves again: entries were looked at, and none was a
     * duplicate. The first is what stops "no duplicates" being the
     * answer a filesystem with no deadlist gives. */
    CHECK(g_dead_seen > 0);
    CHECK(g_dead_dups == 0);
    kinfo("selftest: cosmofs-replay: %u prefix images checked, %llu blocks stranded -- the 162 of 199 the fsck unit "
          "measured are now recorded by the root that freed them and reclaimed at mount",
          g_prefixes_checked, (unsigned long long)g_leak_total);
    kinfo("selftest: cosmofs-replay: %u writes recorded over %u sync points; %u prefix images mounted and checked",
          writes, g_nr_syncs, checked);
    kinfo("selftest: cosmofs-replay: %llu deadlist entries examined across those images, %llu on two lists",
          (unsigned long long)g_dead_seen, (unsigned long long)g_dead_dups);
    return true;
}

#else

bool selftest_cosmofs_replay(const char **reason)
{
    (void)reason;
    kinfo("selftest: cosmofs-replay: debug builds only");
    return true;
}

#endif
