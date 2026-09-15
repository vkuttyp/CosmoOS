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

#include <kernel/cred.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/mountns.h>
#include <kernel/string.h>
#include <kernel/vfs.h>

#include <kernel/cosmofs.h>   /* the pass result structs the fs_type entries name */

#include <uapi/cosmo/fsctl.h>

#include "vfs_internal.h"

/* The result of this file's last command, read back until the next one. */
struct fsctl_open {
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
    if (!cred_privileged(cred_current()))
        return -EPERM;
    struct fsctl_open *o = kmalloc(sizeof(*o), KMEM_ZERO);
    if (o == NULL)
        return -ENOMEM;
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
    kfree(o->result);
    o->result = buf;
    o->len = len;
}

/* What passes a filesystem offers. */
static uint32_t fsctl_caps(const struct mount *mnt)
{
    uint32_t caps = 0;
    if (mnt->fs->check)
        caps |= COSMO_FSCTL_CAP_CHECK;
    if (mnt->fs->scrub)
        caps |= COSMO_FSCTL_CAP_SCRUB;
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
        if (prc == 0) {
            struct cosmo_fsctl_check *c = rec;
            const struct cosmofs_check_class *src[COSMO_FSCTL_CLASSES] = {
                &rep.alloc_not_seen, &rep.seen_not_alloc, &rep.dup, &rep.nlink_wrong,
                &rep.orphan, &rep.dangling_entry, &rep.dir_bad, &rep.counter_wrong,
                &rep.chain_cycle, &rep.unreadable,
            };
            c->nclasses = COSMO_FSCTL_CLASSES;
            c->flags = (rep.partial ? COSMO_FSCTL_R_PARTIAL : 0u) |
                       (rep.clean ? COSMO_FSCTL_R_CLEAN : 0u) |
                       (rep.repair_refused ? COSMO_FSCTL_R_REPAIR_REFUSED : 0u);
            c->blocks_seen = rep.blocks_seen;
            c->inodes_seen = rep.inodes_seen;
            c->dirs_seen = rep.dirs_seen;
            c->snapshots_seen = rep.snapshots_seen;
            c->counted_free = rep.counted_free;
            c->counted_inodes = rep.counted_inodes;
            c->bytes_allocated = rep.bytes_allocated;
            c->elapsed_ns = rep.elapsed_ns;
            for (unsigned i = 0; i < COSMO_FSCTL_CLASSES; i++) {
                c->class[i].count = src[i]->count;
                c->class[i].repaired = src[i]->repaired;
                c->class[i].named = src[i]->named;
                for (unsigned k = 0; k < COSMO_FSCTL_NAMES && k < CFS_CHECK_NAMES; k++)
                    c->class[i].name[k] = src[i]->name[k];
            }
        }
    } else {
        struct cosmofs_scrub_stats st;
        prc = mnt->fs->scrub(mnt, &st);
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

static int64_t fsctl_write_file(struct vnode *vn, struct file *f, uint64_t off,
                                const void *buf, size_t len)
{
    (void)vn; (void)off;
    struct fsctl_open *o = f->priv;
    if (o == NULL)
        return -EINVAL;
    if (!cred_privileged(cred_current()))
        return -EPERM;   /* the fd may have outlived the privilege that opened it */
    if (len != sizeof(struct cosmo_fsctl))
        return -EINVAL;  /* a command is exactly one struct, applied whole */
    struct cosmo_fsctl cmd;
    memcpy(&cmd, buf, sizeof(cmd));
    if (cmd.version != COSMO_FSCTL_VERSION)
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
    if (o == NULL || o->result == NULL)
        return 0;
    if (len < o->len)
        return -ERANGE;   /* a result is read whole or not at all */
    memcpy(buf, o->result, o->len);
    return (int64_t)o->len;
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
    if (rc)
        kerror("fsctl: /dev/fsctl: %d", rc);
}
