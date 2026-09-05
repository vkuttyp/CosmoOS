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
    vfs_umount2(MNT, VFS_UMOUNT_FORCE);
    return ok;
}

bool selftest_cosmofs_replay(const char **reason)
{
    g_nr_syncs = 0;
    g_nr_state = 0;
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
    kinfo("selftest: cosmofs-replay: %u writes recorded over %u sync points; %u prefix images mounted and checked",
          writes, g_nr_syncs, checked);
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
