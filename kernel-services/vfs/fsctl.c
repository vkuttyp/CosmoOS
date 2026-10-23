/*
 * fsctl.c - /dev/fsctl: the operator's channel to a filesystem's
 * maintenance passes (docs/audit/next-subsystem-fsctl.md).
 *
 * cosmofs has two passes over a mounted filesystem, and until this
 * existed every caller of either was a self-test. The problem was never
 * the passes: it was that a mount had no name an operator could use, and
 * nothing kept one alive while a pass walked it.
 *
 * A caller writes one fixed-layout command whole and reads the result
 * back from the same open file. The result belongs to that file, so two
 * operators do not share a "last answer" to race over.
 *
 * This file knows what a pass *is* only through struct fs_type: a
 * filesystem that has one fills in the entry, one that has none leaves
 * it null and a command against it is refused before anything is locked.
 * That is what keeps the VFS from depending on one of its filesystems.
 */

#include <kernel/blk.h>
#include <kernel/cred.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/mutex.h>
#include <kernel/printf.h>
#include <kernel/log.h>
#include <kernel/mountns.h>
#include <kernel/string.h>
#include <kernel/vfs.h>

#include <kernel/cosmofs.h>   /* the pass result structs the fs_type entries name */

#include <uapi/cosmo/fsctl.h>

#include "vfs_internal.h"

/* The result of this file's last command, read back until the next one. */
struct fsctl_open {
    struct mutex lock;   /* the result, replaced by a write and copied by a read (the file lock no longer serialises a stream's callers) */
    void *result;
    size_t len;
};

static int fsctl_open_file(struct vnode *vn, struct file *f)
{
    (void)vn;
    /*
     * Privileged, and not only by the node's mode. 0600 is
     * discretionary: an operator can change it, and the check that
     * cannot be changed is the one that matters. Both, on purpose.
     */
    if (!cred_has(cred_current(), PRIV_SYS_ADMIN))
        return -EPERM;
    struct fsctl_open *o = kmalloc(sizeof(*o), KMEM_ZERO);
    if (o == NULL)
        return -ENOMEM;
    mutex_init(&o->lock, "fsctl-open");
    f->priv = o;
    return 0;
}

static void fsctl_release(struct vnode *vn, struct file *f)
{
    (void)vn;
    struct fsctl_open *o = f->priv;
    if (o == NULL)
        return;
    kfree(o->result);
    kfree(o);
    f->priv = NULL;
}

/* Replace this file's result. Takes ownership of `buf`. */
static void fsctl_set_result(struct fsctl_open *o, void *buf, size_t len)
{
    mutex_lock(&o->lock);
    void *old = o->result;
    o->result = buf;
    o->len = len;
    mutex_unlock(&o->lock);
    kfree(old);
}

/* What passes a filesystem offers. */
static uint32_t fsctl_caps(const struct mount *mnt)
{
    uint32_t caps = 0;
    if (mnt->fs->check)
        caps |= COSMO_FSCTL_CAP_CHECK;
    if (mnt->fs->scrub)
        caps |= COSMO_FSCTL_CAP_SCRUB;
    if (mnt->fs->snapctl)
        caps |= COSMO_FSCTL_CAP_SNAP;
    return caps;
}

static void fsctl_fill_mount(struct cosmo_fsctl_mount *m, const struct mount *mnt,
                             uint64_t ns_id, const char *path)
{
    memset(m, 0, sizeof(*m));
    m->id = mnt->id;
    m->ns_id = ns_id;
    m->flags = mnt->flags;
    m->caps = fsctl_caps(mnt);
    strlcpy(m->fstype, mnt->fs->name, sizeof(m->fstype));
    strlcpy(m->path, path, sizeof(m->path));
}

/*
 * The mounts the calling process's namespace holds.
 *
 * The root mount is emitted first and separately: it is visible in every
 * namespace and deliberately carries no mount_ns_ref (mountns.c, "the
 * root mount answers true without being on any list"), so a listing
 * built from the namespace's list alone would omit the filesystem most
 * worth checking.
 *
 * Sized under the lock, filled under the lock, but allocated with it
 * dropped -- a path is a kilobyte and this is not an allocation to make
 * while holding the mount table. If the namespace gained a mount in
 * between, `count` is short of `total` and the header says so.
 */
static int64_t fsctl_list(struct fsctl_open *o)
{
    unsigned want = vfs_mount_count() + 1;   /* + the root, which is not in a namespace list */
    size_t bytes = sizeof(struct cosmo_fsctl_result) + (size_t)want * sizeof(struct cosmo_fsctl_mount);
    uint8_t *buf = kmalloc(bytes, KMEM_ZERO);
    if (buf == NULL)
        return -ENOMEM;

    struct cosmo_fsctl_result *hdr = (struct cosmo_fsctl_result *)buf;
    struct cosmo_fsctl_mount *rec = (struct cosmo_fsctl_mount *)(buf + sizeof(*hdr));
    unsigned n = 0, total = 0;

    mutex_lock(&g_mounts_lock);
    struct mount_ns *ns = mountns_current();
    if (g_root_mount != NULL) {
        total++;
        if (n < want)
            fsctl_fill_mount(&rec[n++], g_root_mount, ns->id, "/");
    }
    struct mount_ns_ref *r;
    list_for_each_entry(r, &ns->mounts, ns_link) {
        if (r->mnt == g_root_mount)
            continue;                        /* already emitted, and it holds no ref anyway */
        total++;
        if (n < want)
            fsctl_fill_mount(&rec[n++], r->mnt, ns->id, r->path);
    }
    mutex_unlock(&g_mounts_lock);

    hdr->version = COSMO_FSCTL_VERSION;
    hdr->kind = COSMO_FSCTL_LIST;
    hdr->count = n;
    hdr->total = total;
    hdr->bytes = sizeof(struct cosmo_fsctl_mount);
    fsctl_set_result(o, buf, sizeof(*hdr) + (size_t)n * sizeof(struct cosmo_fsctl_mount));
    return 0;
}

/* Text appended to /proc/mounts' buffer: `len` grows while it fits; past
 * the end `over` is set and nothing more is written. */
struct mounts_out {
    char *buf;
    size_t n, len;
    bool over;
};

static void mo_put(struct mounts_out *o, const char *s, size_t k)
{
    if (o->over || o->len + k >= o->n) {   /* one byte stays for the NUL */
        o->over = true;
        return;
    }
    memcpy(o->buf + o->len, s, k);
    o->len += k;
}

static void mo_str(struct mounts_out *o, const char *s)
{
    mo_put(o, s, strlen(s));
}

/* A path, escaped as Linux escapes /proc/mounts: a space, tab, newline or
 * backslash as \ooo, so a reader splitting on blanks gets it back whole.
 * Written straight into the output, so no intermediate buffer can cut a
 * path whose escapes make it longer than VFS_PATH_MAX. */
static void mo_path(struct mounts_out *o, const char *s)
{
    for (; *s != '\0'; s++) {
        if (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\\') {
            char e[5];
            ksnprintf(e, sizeof(e), "\\%03o", (unsigned)(unsigned char)*s);
            mo_put(o, e, 4);
        } else {
            mo_put(o, s, 1);
        }
    }
}

/* One line, whole or not at all: a line that does not fit is taken back,
 * and `over` stays set so no later line is written either. */
static void mounts_line(struct mounts_out *o, const struct mount *mnt, const char *path)
{
    size_t start = o->len;
    if (mnt->bdev != NULL) {
        mo_str(o, "/dev/");
        mo_str(o, mnt->bdev->name);
    } else {
        mo_str(o, mnt->fs->name);
    }
    mo_str(o, " ");
    mo_path(o, path);
    mo_str(o, " ");
    mo_str(o, mnt->fs->name);
    mo_str(o, (mnt->flags & MOUNT_RDONLY) ? " ro" : " rw");
    if (__atomic_load_n(&mnt->state, __ATOMIC_RELAXED) & MOUNT_STATE_DEGRADED)
        mo_str(o, ",degraded");
    mo_str(o, " 0 0\n");
    if (o->over)
        o->len = start;
}

/* The walk fsctl_list makes, printed as /proc/mounts (vfs.h). A line that
 * does not fit is left out whole, and so is every line after it. Under
 * g_mount_list_lock alone: procfs calls this from its lookup, holding the
 * directory's vnode lock, and g_mounts_lock is taken before vnode locks
 * (vfs_internal.h). */
int vfs_mounts_render(char *out, size_t n)
{
    struct mounts_out o = { .buf = out, .n = n, .len = 0, .over = n == 0 };
    spin_lock(&g_mount_list_lock);
    struct mount_ns *ns = mountns_current();
    if (g_root_mount != NULL)
        mounts_line(&o, g_root_mount, "/");
    struct mount_ns_ref *r;
    list_for_each_entry(r, &ns->mounts, ns_link) {
        if (o.over)
            break;
        if (r->mnt != g_root_mount)
            mounts_line(&o, r->mnt, r->path);
    }
    spin_unlock(&g_mount_list_lock);
    if (n > 0)
        out[o.len] = '\0';
    return (int)o.len;
}

/*
 * Run one pass against one named mount.
 *
 * The mount is acquired by id, which takes a reference and counts a
 * pass, so it cannot be freed and an unmount waits rather than tearing
 * the filesystem down mid-walk. Everything fallible happens before the
 * acquisition or after the release; there is exactly one path out that
 * holds a mount, and it releases.
 *
 * The result is copied field by field into the published record. A
 * kernel struct is not an ABI: cosmofs may reorder or extend
 * cosmofs_check_report freely, and the only thing that must not move is
 * what userland parses.
 */
static void fsctl_check_record(struct cosmo_fsctl_check *c, const struct cosmofs_check_report *rep);

static int64_t fsctl_run(struct fsctl_open *o, const struct cosmo_fsctl *cmd)
{
    struct mount *mnt = NULL;
    int rc = vfs_mount_acquire(cmd->mount_id, &mnt);
    if (rc)
        return rc;

    bool want_check = cmd->op == COSMO_FSCTL_CHECK;
    if ((want_check && mnt->fs->check == NULL) || (!want_check && mnt->fs->scrub == NULL)) {
        vfs_mount_release(mnt);
        return -EOPNOTSUPP;   /* this filesystem has no such pass */
    }

    size_t bytes = sizeof(struct cosmo_fsctl_result) +
                   (want_check ? sizeof(struct cosmo_fsctl_check) : sizeof(struct cosmo_fsctl_scrub));
    uint8_t *buf = kmalloc(bytes, KMEM_ZERO);
    if (buf == NULL) {
        vfs_mount_release(mnt);
        return -ENOMEM;
    }

    struct cosmo_fsctl_result *hdr = (struct cosmo_fsctl_result *)buf;
    void *rec = buf + sizeof(*hdr);
    int prc;
    if (want_check) {
        struct cosmofs_check_report rep;
        unsigned flags = (cmd->flags & COSMO_FSCTL_F_REPAIR) ? COSMOFS_CHECK_REPAIR : 0;
        prc = mnt->fs->check(mnt, &rep, flags);
        if (prc == 0)
            fsctl_check_record(rec, &rep);
    } else {
        struct cosmofs_scrub_stats st;
        /* Version 1's SCRUB repairs as it always did; SCRUB_EX asks. */
        prc = mnt->fs->scrub(mnt, &st, COSMOFS_SCRUB_REPAIR);
        if (prc == 0) {
            struct cosmo_fsctl_scrub *sc = rec;
            sc->blocks_read = st.blocks_read;
            sc->inodes = st.inodes;
            sc->repaired = st.repaired;
            sc->unrecoverable = st.unrecoverable;
        }
    }
    vfs_mount_release(mnt);

    if (prc) {
        kfree(buf);
        return prc;
    }
    hdr->version = COSMO_FSCTL_VERSION;
    hdr->kind = cmd->op;
    hdr->count = 1;
    hdr->total = 1;
    hdr->bytes = (uint32_t)(bytes - sizeof(*hdr));
    fsctl_set_result(o, buf, bytes);
    return 0;
}

/* The record a pass fills, copied field by field (fsctl_run's rule). */
static void fsctl_check_record(struct cosmo_fsctl_check *c, const struct cosmofs_check_report *rep)
{
    const struct cosmofs_check_class *src[COSMO_FSCTL_CLASSES] = {
        &rep->alloc_not_seen, &rep->seen_not_alloc, &rep->dup, &rep->nlink_wrong,
        &rep->orphan, &rep->dangling_entry, &rep->dir_bad, &rep->counter_wrong,
        &rep->chain_cycle, &rep->unreadable,
        &rep->extent_order, &rep->extent_overlap, &rep->dir_dup_name,
        &rep->snap_identity, &rep->rollback_record, &rep->hold,
    };
    c->nclasses = COSMO_FSCTL_CLASSES;
    c->flags = (rep->partial ? COSMO_FSCTL_R_PARTIAL : 0u) | (rep->clean ? COSMO_FSCTL_R_CLEAN : 0u) |
               (rep->repair_refused ? COSMO_FSCTL_R_REPAIR_REFUSED : 0u);
    c->blocks_seen = rep->blocks_seen;
    c->inodes_seen = rep->inodes_seen;
    c->dirs_seen = rep->dirs_seen;
    c->snapshots_seen = rep->snapshots_seen;
    c->counted_free = rep->counted_free;
    c->counted_inodes = rep->counted_inodes;
    c->bytes_allocated = rep->bytes_allocated;
    c->elapsed_ns = rep->elapsed_ns;
    for (unsigned i = 0; i < COSMO_FSCTL_CLASSES; i++) {
        c->class[i].count = src[i]->count;
        c->class[i].repaired = src[i]->repaired;
        c->class[i].named = src[i]->named;
        for (unsigned k = 0; k < COSMO_FSCTL_NAMES && k < CFS_CHECK_NAMES; k++)
            c->class[i].name[k] = src[i]->name[k];
    }
}

/*
 * CHECK_DEV (roadmap 1.2 B2): the check on a device nothing has mounted,
 * named by its block-device name. The filesystem mounts it where no
 * namespace sees it and runs the same check the mounted case runs
 * (cosmofs_check_dev), so fsck.cosmofs is the kernel's checker and not a
 * copy of it. A mounted device is -EBUSY: its own mount's CHECK is the
 * way in.
 */
static int64_t fsctl_check_dev(struct fsctl_open *o, const struct cosmo_fsctl *cmd)
{
    struct fs_type *fst = vfs_find_fs(cmd->name2[0] ? cmd->name2 : "cosmofs");
    if (fst == NULL)
        return -ENODEV;
    if (fst->check_dev == NULL)
        return -EOPNOTSUPP;
    struct blkdev *bd = blk_find(cmd->name);
    if (bd == NULL)
        return -ENOENT;
    int64_t rc = 0;
    struct cosmofs_check_report *rep = kzalloc(sizeof(*rep));
    struct cosmofs_dev_report *dev = kzalloc(sizeof(*dev));
    size_t bytes = sizeof(struct cosmo_fsctl_result) + sizeof(struct cosmo_fsctl_devcheck);
    uint8_t *buf = kzalloc(bytes);
    if (rep == NULL || dev == NULL || buf == NULL)
        rc = -ENOMEM;
    else if (vfs_bdev_mounted(bd))
        rc = -EBUSY;
    if (rc == 0) {
        unsigned flags = 0;
        if (cmd->flags & COSMO_FSCTL_F_REPAIR)
            flags |= COSMOFS_CHECK_REPAIR;
        if (cmd->flags & COSMO_FSCTL_F_BOOT)
            flags |= COSMOFS_CHECK_BOOT;
        rc = fst->check_dev(bd, flags, rep, dev);
    }
    blkdev_put(bd);
    if (rc == 0) {
        struct cosmo_fsctl_result *hdr = (struct cosmo_fsctl_result *)buf;
        struct cosmo_fsctl_devcheck *r = (struct cosmo_fsctl_devcheck *)(buf + sizeof(*hdr));
        r->dev.flags = dev->flags;
        r->dev.mount_error = dev->mount_error;
        r->dev.mount_stage = dev->mount_stage;
        r->dev.rollback_error = dev->rollback_error;
        r->dev.format = dev->format;
        r->dev.replayed_orphans = dev->replayed_orphans;
        r->dev.generation = dev->generation;
        r->dev.slot_generation[0] = dev->slot_generation[0];
        r->dev.slot_generation[1] = dev->slot_generation[1];
        r->dev.replayed_frees = dev->replayed_frees;
        strlcpy(r->dev.rollback_target, dev->rollback_target, sizeof(r->dev.rollback_target));
        if (dev->flags & COSMO_FSCTL_DEV_F_CHECKED)
            fsctl_check_record(&r->check, rep);
        hdr->version = COSMO_FSCTL_VERSION;
        hdr->kind = cmd->op;
        hdr->count = 1;
        hdr->total = 1;
        hdr->bytes = sizeof(struct cosmo_fsctl_devcheck);
        fsctl_set_result(o, buf, bytes);
        buf = NULL;
    }
    kfree(buf);
    kfree(rep);
    kfree(dev);
    return rc;
}

/* SCRUB_EX (roadmap 1.2 B2): the scrub, repairing only when asked. */
static int64_t fsctl_scrub_ex(struct fsctl_open *o, const struct cosmo_fsctl *cmd)
{
    struct mount *mnt = NULL;
    int rc = vfs_mount_acquire(cmd->mount_id, &mnt);
    if (rc)
        return rc;
    if (mnt->fs->scrub == NULL) {
        vfs_mount_release(mnt);
        return -EOPNOTSUPP;
    }
    /* The record first: a repairing scrub that then had nowhere to say
     * what it repaired would have changed the pool and told nobody
     * (Codex on #434). */
    size_t bytes = sizeof(struct cosmo_fsctl_result) + sizeof(struct cosmo_fsctl_scrub_ex);
    uint8_t *buf = kzalloc(bytes);
    if (buf == NULL) {
        vfs_mount_release(mnt);
        return -ENOMEM;
    }
    struct cosmofs_scrub_stats st;
    rc = mnt->fs->scrub(mnt, &st, (cmd->flags & COSMO_FSCTL_F_REPAIR) ? COSMOFS_SCRUB_REPAIR : 0u);
    vfs_mount_release(mnt);
    if (rc) {
        kfree(buf);
        return rc;
    }
    struct cosmo_fsctl_result *hdr = (struct cosmo_fsctl_result *)buf;
    struct cosmo_fsctl_scrub_ex *x = (struct cosmo_fsctl_scrub_ex *)(buf + sizeof(*hdr));
    x->blocks_read = st.blocks_read;
    x->inodes = st.inodes;
    x->bad_copies = st.bad_copies;
    x->repaired = st.repaired;
    x->unrecoverable = st.unrecoverable;
    x->skipped = st.skipped;
    x->elapsed_ns = st.elapsed_ns;
    x->rate = st.rate;
    hdr->version = COSMO_FSCTL_VERSION;
    hdr->kind = cmd->op;
    hdr->count = 1;
    hdr->total = 1;
    hdr->bytes = sizeof(*x);
    fsctl_set_result(o, buf, bytes);
    return 0;
}

/*
 * A snapshot operation against one named mount (version 3). The mount is
 * held across the call exactly as a pass holds it, and the records are
 * copied field by field: struct cosmofs_snap_info is cosmofs's and may
 * change, struct cosmo_fsctl_snap is the ABI.
 */
static int64_t fsctl_snap(struct fsctl_open *o, const struct cosmo_fsctl *cmd)
{
    static const enum cosmofs_snap_op ops[] = {
        [COSMO_FSCTL_SNAP_CREATE] = COSMOFS_SNAP_CREATE,
        [COSMO_FSCTL_SNAP_LIST] = COSMOFS_SNAP_LIST,
        [COSMO_FSCTL_SNAP_DELETE] = COSMOFS_SNAP_DELETE,
        [COSMO_FSCTL_SNAP_ROLLBACK] = COSMOFS_SNAP_ROLLBACK,
        [COSMO_FSCTL_SNAP_CANCEL] = COSMOFS_SNAP_CANCEL,
        [COSMO_FSCTL_SNAP_CLONE] = COSMOFS_SNAP_CLONE,
        [COSMO_FSCTL_BOOT_OK] = COSMOFS_SNAP_BOOT_OK,
    };
    struct mount *mnt = NULL;
    int rc = vfs_mount_acquire(cmd->mount_id, &mnt);
    if (rc)
        return rc;
    if (mnt->fs->snapctl == NULL) {
        vfs_mount_release(mnt);
        return -EOPNOTSUPP;
    }
    /* Room for every snapshot a list can hold (CFS_SNAP_ID_MAX is the
     * id space; a list this long is not one anyone has). */
    unsigned max = cmd->op == COSMO_FSCTL_SNAP_LIST ? COSMO_FSCTL_SNAP_PAGE : 1u;
    struct cosmofs_snap_info *info = kzalloc((size_t)max * sizeof(*info));
    if (info == NULL) {
        vfs_mount_release(mnt);
        return -ENOMEM;
    }
    bool safety_named = (cmd->flags & COSMO_FSCTL_F_SAFETY_NAME) != 0;
    struct cosmofs_snap_req req = {
        .op = ops[cmd->op],
        .name = cmd->name[0] ? cmd->name : NULL,
        .name2 = (cmd->op == COSMO_FSCTL_SNAP_CLONE || safety_named) && cmd->name2[0] ? cmd->name2 : NULL,
        .out = info,
        .max = max,
        .no_sizes = (cmd->flags & COSMO_FSCTL_F_NO_SIZES) != 0,
        .after = cmd->op == COSMO_FSCTL_SNAP_LIST ? cmd->cursor : 0,
    };
    rc = mnt->fs->snapctl(mnt, &req);
    vfs_mount_release(mnt);
    if (rc) {
        kfree(info);
        return rc;
    }
    bool listing = cmd->op == COSMO_FSCTL_SNAP_LIST;
    size_t pool = listing ? sizeof(struct cosmo_fsctl_snap_pool) : 0;
    size_t bytes = sizeof(struct cosmo_fsctl_result) + pool + (size_t)req.count * sizeof(struct cosmo_fsctl_snap);
    uint8_t *buf = kzalloc(bytes);
    if (buf == NULL) {
        kfree(info);
        return -ENOMEM;
    }
    struct cosmo_fsctl_result *hdr = (struct cosmo_fsctl_result *)buf;
    if (listing) {
        struct cosmo_fsctl_snap_pool *p = (struct cosmo_fsctl_snap_pool *)(buf + sizeof(*hdr));
        p->format = req.format;
        p->flags = ((req.pool_flags & COSMOFS_POOL_PENDING) ? COSMO_FSCTL_POOL_F_PENDING : 0u) |
                   ((req.pool_flags & COSMOFS_POOL_HELD) ? COSMO_FSCTL_POOL_F_HELD : 0u);
        p->free_bytes = req.free_bytes;
        p->total_bytes = req.total_bytes;
        p->rollback_error = req.rollback_error;
    }
    struct cosmo_fsctl_snap *rec = (struct cosmo_fsctl_snap *)(buf + sizeof(*hdr) + pool);
    for (unsigned i = 0; i < req.count; i++) {
        rec[i].id = info[i].id;
        rec[i].generation = info[i].generation;
        rec[i].created_ns = info[i].created_ns;
        rec[i].origin_id = info[i].origin_id;
        rec[i].referenced = info[i].referenced;
        rec[i].unique = info[i].unique;
        memcpy(rec[i].uuid, info[i].uuid, sizeof(rec[i].uuid));
        rec[i].flags = ((info[i].flags & COSMOFS_SNAP_I_CLONE) ? COSMO_FSCTL_SNAP_F_CLONE : 0u) |
                       ((info[i].flags & COSMOFS_SNAP_I_TARGET) ? COSMO_FSCTL_SNAP_F_TARGET : 0u) |
                       ((info[i].flags & COSMOFS_SNAP_I_SAFETY) ? COSMO_FSCTL_SNAP_F_SAFETY : 0u) |
                       ((info[i].flags & COSMOFS_SNAP_I_HELD) ? COSMO_FSCTL_SNAP_F_HELD : 0u) |
                       ((info[i].flags & COSMOFS_SNAP_I_LIVE) ? COSMO_FSCTL_SNAP_F_LIVE : 0u);
        rec[i].format = info[i].format;
        strlcpy(rec[i].name, info[i].name, sizeof(rec[i].name));
    }
    kfree(info);
    hdr->version = COSMO_FSCTL_VERSION;
    hdr->kind = cmd->op;
    hdr->count = req.count;
    /* BOOT_OK's total is 1 when it released a hold, 0 when there was none. */
    hdr->total = (cmd->op == COSMO_FSCTL_SNAP_LIST || cmd->op == COSMO_FSCTL_BOOT_OK) ? req.total : req.count;
    hdr->bytes = sizeof(struct cosmo_fsctl_snap);
    fsctl_set_result(o, buf, bytes);
    return 0;
}

static int64_t fsctl_write_file(struct vnode *vn, struct file *f, uint64_t off,
                                const void *buf, size_t len)
{
    (void)vn; (void)off;
    struct fsctl_open *o = f->priv;
    if (o == NULL)
        return -EINVAL;
    if (!cred_has(cred_current(), PRIV_SYS_ADMIN))
        return -EPERM;   /* the fd may have outlived the privilege that opened it */
    if (len != sizeof(struct cosmo_fsctl))
        return -EINVAL;  /* a command is exactly one struct, applied whole */
    struct cosmo_fsctl cmd;
    memcpy(&cmd, buf, sizeof(cmd));
    if (cmd.version != COSMO_FSCTL_VERSION)
        return -EINVAL;

    /*
     * Every bit of `flags` must mean something to this op. Accepting a
     * bit nobody defined makes a caller's mistake silent now and makes
     * it ambiguous later, when a version gives that bit a meaning and
     * an old writer turns out to have been setting it.
     */
    uint32_t allowed = cmd.op == COSMO_FSCTL_CHECK      ? COSMO_FSCTL_F_REPAIR
                       : cmd.op == COSMO_FSCTL_CHECK_DEV ? COSMO_FSCTL_F_REPAIR | COSMO_FSCTL_F_BOOT
                       : cmd.op == COSMO_FSCTL_SCRUB_EX  ? COSMO_FSCTL_F_REPAIR
                       : cmd.op == COSMO_FSCTL_SNAP_ROLLBACK ? COSMO_FSCTL_F_SAFETY_NAME
                       : cmd.op == COSMO_FSCTL_SNAP_LIST     ? COSMO_FSCTL_F_NO_SIZES
                                                             : 0u;
    if ((cmd.flags & ~allowed) != 0)
        return -EINVAL;
    if (cmd.op == COSMO_FSCTL_CHECK_DEV && (cmd.flags & COSMO_FSCTL_F_BOOT) && !(cmd.flags & COSMO_FSCTL_F_REPAIR))
        return -EINVAL;   /* BOOT narrows a repair; alone it means nothing */
    /* A name is a NUL-terminated string inside its field, or empty. */
    if (memchr(cmd.name, '\0', sizeof(cmd.name)) == NULL || memchr(cmd.name2, '\0', sizeof(cmd.name2)) == NULL)
        return -EINVAL;

    int64_t rc;
    switch (cmd.op) {
    case COSMO_FSCTL_LIST:
        rc = fsctl_list(o);
        break;
    case COSMO_FSCTL_CHECK:
    case COSMO_FSCTL_SCRUB:
        rc = fsctl_run(o, &cmd);
        break;
    case COSMO_FSCTL_CHECK_DEV:
        rc = fsctl_check_dev(o, &cmd);
        break;
    case COSMO_FSCTL_SCRUB_EX:
        rc = fsctl_scrub_ex(o, &cmd);
        break;
    case COSMO_FSCTL_SNAP_CREATE:
    case COSMO_FSCTL_SNAP_LIST:
    case COSMO_FSCTL_SNAP_DELETE:
    case COSMO_FSCTL_SNAP_ROLLBACK:
    case COSMO_FSCTL_SNAP_CANCEL:
    case COSMO_FSCTL_SNAP_CLONE:
    case COSMO_FSCTL_BOOT_OK:
        rc = fsctl_snap(o, &cmd);
        break;
    default:
        rc = -EINVAL;
        break;
    }
    /*
     * A command that failed leaves no result: a reader must not see the
     * previous one and take it for this one's answer. A pass that *found
     * faults* did not fail -- the report says what they are and the
     * write succeeds, because "the filesystem has three leaked blocks"
     * is a successful check.
     */
    if (rc)
        fsctl_set_result(o, NULL, 0);
    return rc ? rc : (int64_t)len;
}

/*
 * The result of this file's last command. A file that has issued none,
 * or whose last command failed, reads zero bytes -- which is why LIST is
 * a written command and not "a read with no prior command": the empty
 * state needs one meaning.
 */
static int64_t fsctl_read_file(struct vnode *vn, struct file *f, uint64_t off,
                               void *buf, size_t len)
{
    (void)vn; (void)off;
    struct fsctl_open *o = f->priv;
    if (o == NULL)
        return 0;
    mutex_lock(&o->lock);
    int64_t n = 0;
    if (o->result == NULL) {
        n = 0;
    } else if (len < o->len) {
        n = -ERANGE;   /* a result is read whole or not at all */
    } else {
        memcpy(buf, o->result, o->len);
        n = (int64_t)o->len;
    }
    mutex_unlock(&o->lock);
    return n;
}

static const struct chrdev_ops fsctl_ops = {
    .open = fsctl_open_file,
    .release = fsctl_release,
    .read_file = fsctl_read_file,
    .write_file = fsctl_write_file,
};

static struct vnode *g_fsctl_node;

void fsctl_dev_init(void)
{
    int rc = ramfs_mkchr("/dev/fsctl", 0600, &fsctl_ops, NULL, &g_fsctl_node);
    if (rc) {
        kerror("fsctl: /dev/fsctl: %d", rc);
        return;
    }
    /*
     * Said out loud on every boot, release included. No self-test runs
     * in a release build, so this line is the only evidence that the
     * channel an operator needs is actually there -- and a release
     * kernel that shipped the passes with no way to reach them is the
     * thing this unit exists to stop.
     */
    kinfo("fsctl: /dev/fsctl ready (0600): mount listing, check (mounted or not), scrub and snapshots");
}
