/*
 * vfs.c - Mount table, path resolution, vnodes, files.
 *
 * Every entry validates before it calls a filesystem; every filesystem
 * callback runs with the vnode locks described in
 * docs/kernel-services/vfs/design.md ("Locking").
 */

#include <kernel/blk.h>
#include <kernel/cred.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/lockdep.h>
#include <kernel/log.h>
#include <kernel/mountns.h>
#include <kernel/pmm.h>
#include <kernel/panic.h>
#include <kernel/printf.h>
#include <kernel/process.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <kernel/part.h>
#include <kernel/vfs.h>
#include <kernel/wait.h>

#include "vfs_internal.h"

/* --- directory mutations ---------------------------------------------------- */

/* The sticky bit (01000) on a directory: an entry may be removed or
 * renamed only by the owner of the entry, the owner of the directory, or a
 * privileged caller (/tmp is 01777). */
static bool sticky_denies(const struct vnode *dir, const struct vnode *entry)
{
    if (!(dir->mode & 01000))
        return false;
    const struct credentials *c = cred_current();
    return !cred_has(c, PRIV_SYS_ADMIN) && c->euid != entry->uid && c->euid != dir->uid;
}

/* Resolve the parent directory for a mutation; locks it. The last
 * component is copied into `namebuf` (VFS_NAME_MAX + 1) and is never
 * followed: these calls name an entry in a directory, not what it
 * points at. */
static int parent_for_mutation(struct vnode *start, const char *path, char *namebuf, struct vnode **parent,
                               size_t *len)
{
    int rc = vfs_walk_parent(start, path, namebuf, parent, len, NULL);
    if (rc)
        return rc;
    if (*len == 0 || vfs_dot_name(namebuf, *len)) {
        vnode_put(*parent);
        return *len == 0 ? -EEXIST : -EINVAL;
    }
    if ((*parent)->type != VNODE_DIR) {
        vnode_put(*parent);
        return -ENOTDIR;
    }
    if ((*parent)->mnt->flags & MOUNT_RDONLY) {
        vnode_put(*parent);
        return -EROFS;
    }
    /* Adding, removing or renaming an entry writes the directory and
     * searches it for the name. */
    rc = vfs_permission(*parent, VFS_MAY_WRITE | VFS_MAY_EXEC);
    if (rc) {
        vnode_put(*parent);
        return rc;
    }
    return 0;
}

int vfs_mkdir(struct vnode *start, const char *path, uint32_t mode)
{
    struct vnode *parent;
    char last[VFS_NAME_MAX + 1];
    size_t len = 0;
    int rc = parent_for_mutation(start, path, last, &parent, &len);
    /* "." and ".." exist already: mkdir says so, as Linux does (BusyBox
     * tar makes "./" for every archive that names it). */
    if (rc == -EINVAL && len > 0 && vfs_dot_name(last, len))
        return -EEXIST;
    if (rc)
        return rc;
    mutex_lock(&parent->lock);
    struct vnode *existing = NULL;
    if (parent->flags & VNODE_DEAD) {
        rc = -ENOENT;
    } else if (parent->ops->lookup(parent, last, len, &existing) == 0) {
        vnode_put(existing);
        rc = -EEXIST;
    } else if (parent->ops->mkdir == NULL) {
        rc = -ENOTSUP;
    } else {
        struct vnode *vn = NULL;
        rc = parent->ops->mkdir(parent, last, len, mode & 07777, &vn);
        if (rc == 0)
            vnode_put(vn);
    }
    mutex_unlock(&parent->lock);
    vnode_put(parent);
    return rc;
}

static int remove_entry(struct vnode *start, const char *path, bool dir)
{
    struct vnode *parent;
    char last[VFS_NAME_MAX + 1];
    size_t len;
    int rc = parent_for_mutation(start, path, last, &parent, &len);
    if (rc)
        return rc;
    mutex_lock(&parent->lock);
    struct vnode *victim = NULL;
    if (parent->flags & VNODE_DEAD)
        rc = -ENOENT;
    else
        rc = parent->ops->lookup(parent, last, len, &victim);
    if (rc == 0) {
        mutex_lock_nested(&victim->lock, VNODE_NESTED_CHILD);   /* child under its parent (V7) */
        if (dir && victim->type != VNODE_DIR)
            rc = -ENOTDIR;
        else if (!dir && victim->type == VNODE_DIR)
            rc = -EISDIR;
        else if (vfs_covering_mount(victim) != NULL || victim->mnt != parent->mnt)
            rc = -EBUSY;   /* a mountpoint or a mount root; victim->lock is held */
        else if (sticky_denies(parent, victim))
            rc = -EACCES;  /* a sticky directory: only the owner of the entry, the directory or root */
        else if (dir)
            rc = parent->ops->rmdir ? parent->ops->rmdir(parent, last, len, victim) : -ENOTSUP;
        else
            rc = parent->ops->unlink ? parent->ops->unlink(parent, last, len, victim) : -ENOTSUP;
        /* Dead once its last name is gone: a file with another name
         * (VFS-4) is still reached through that one. */
        if (rc == 0 && (dir || victim->nlink == 0))
            victim->flags |= VNODE_DEAD;
        mutex_unlock(&victim->lock);
        vnode_put(victim);
    }
    mutex_unlock(&parent->lock);
    vnode_put(parent);
    return rc;
}

int vfs_unlink(struct vnode *start, const char *path)
{
    return remove_entry(start, path, false);
}

int vfs_mknod(struct vnode *start, const char *path, uint32_t mode, enum vnode_type type, struct vnode **out)
{
    if (type != VNODE_SOCK && type != VNODE_FIFO)
        return -EINVAL;
    struct vnode *parent;
    char last[VFS_NAME_MAX + 1];
    size_t len;
    int rc = parent_for_mutation(start, path, last, &parent, &len);
    if (rc)
        return rc;
    mutex_lock(&parent->lock);
    struct vnode *vn = NULL;
    if (parent->flags & VNODE_DEAD) {
        rc = -ENOENT;
    } else if (parent->ops->mknod == NULL) {
        rc = -EOPNOTSUPP;
    } else {
        rc = parent->ops->lookup(parent, last, len, &vn);
        if (rc == 0) {
            vnode_put(vn);
            vn = NULL;
            rc = -EEXIST;
        } else if (rc == -ENOENT) {
            rc = parent->ops->mknod(parent, last, len, mode & 07777, type, &vn);
        }
    }
    mutex_unlock(&parent->lock);
    vnode_put(parent);
    if (rc == 0)
        *out = vn;
    return rc;
}

int vfs_rmdir(struct vnode *start, const char *path)
{
    return remove_entry(start, path, true);
}

/*
 * True if `anc` is `vn` or one of its ancestors. The caller holds the
 * mount's rename_lock and no vnode lock: only rename changes a directory's
 * parent, so the ancestry is stable, and each ".." lookup takes that
 * directory's lock alone (a concurrent mkdir or unlink in it rewrites the
 * same directory data).
 */
static bool is_ancestor(struct vnode *anc, struct vnode *vn)
{
    struct vnode *p = vn;
    vnode_get(p);
    bool yes = false;
    for (unsigned d = 0; d < VFS_MAX_COMPONENTS; d++) {
        if (p == anc) {
            yes = true;
            break;
        }
        if (p->mnt->root == p)
            break;
        struct vnode *up;
        mutex_lock(&p->lock);
        int r = p->ops->lookup(p, "..", 2, &up);
        mutex_unlock(&p->lock);
        vnode_put(p);
        if (r)
            return false;
        p = up;
    }
    vnode_put(p);
    return yes;
}

/*
 * Rename, with the lock order the rest of the VFS uses: parent before
 * child, and for two parents the ancestor first (docs/kernel/lockdep/
 * design.md, "VFS: rename"). Under the mount's rename_lock the ancestry
 * cannot change, so it is decided, and the "directory under itself" rule
 * checked, before any parent is locked. Two unrelated directories are
 * locked in address order: no other path locks two directories without
 * an ancestry between them, and other renames are excluded by the mutex.
 */
int vfs_ftruncate(struct vnode *vn, uint64_t size)
{
    if (vn->type == VNODE_DIR)
        return -EISDIR;
    if (vn->type != VNODE_REG)
        return -EINVAL;
    /* No inode-permission check: ftruncate(2) gates on the fd being open for
     * writing, which the caller has already established; it does not consult
     * the file mode the way truncate(2) (vfs_truncate) does. */
    mutex_lock(&vn->lock);
    /* As in the write and the O_TRUNC open: a running program's pages are not
     * the caller's to remove. */
    int rc = vfs_text_busy_locked(vn) ? -ETXTBSY
           : vn->ops->truncate    ? vn->ops->truncate(vn, size) : -ENOTSUP;
    mutex_unlock(&vn->lock);
    return rc;
}

int vfs_truncate(struct vnode *start, const char *path, uint64_t size)
{
    struct vnode *vn;
    int rc = vfs_lookup(start, path, &vn);
    if (rc)
        return rc;
    if (vn->type == VNODE_DIR) {
        vnode_put(vn);
        return -EISDIR;
    }
    if (vn->type != VNODE_REG) {
        vnode_put(vn);
        return -EINVAL;
    }
    rc = vfs_permission(vn, VFS_MAY_WRITE);
    if (rc == 0) {
        mutex_lock(&vn->lock);
        /* As in the write and the O_TRUNC open: a running program's
         * pages are not the caller's to remove. */
        rc = vfs_text_busy_locked(vn) ? -ETXTBSY
           : vn->ops->truncate    ? vn->ops->truncate(vn, size) : -ENOTSUP;
        mutex_unlock(&vn->lock);
    }
    vnode_put(vn);
    return rc;
}

int vfs_rename(struct vnode *start, const char *oldpath, const char *newpath)
{
    return vfs_rename2(start, oldpath, start, newpath);
}

/* Two starts, for renameat's two descriptors. The checks below are made
 * on the two parents, however they were reached. */
int vfs_rename2(struct vnode *ostart, const char *oldpath, struct vnode *nstart, const char *newpath)
{
    struct vnode *odir, *ndir;
    char oname[VFS_NAME_MAX + 1], nname[VFS_NAME_MAX + 1];
    size_t olen, nlen;
    int rc = parent_for_mutation(ostart, oldpath, oname, &odir, &olen);
    if (rc)
        return rc;
    rc = parent_for_mutation(nstart, newpath, nname, &ndir, &nlen);
    if (rc) {
        vnode_put(odir);
        return rc;
    }
    if (odir->mnt != ndir->mnt) {
        rc = -EXDEV;
        goto out_dirs;
    }
    if (odir->ops->rename == NULL) {
        rc = -ENOTSUP;
        goto out_dirs;
    }
    struct mount *mnt = odir->mnt;
    mutex_lock(&mnt->rename_lock);

    for (unsigned attempt = 0;; attempt++) {
        /* Phase 1, no parent locked: find the victim, check it is not an
         * ancestor of the destination, decide the lock order. */
        struct vnode *victim0 = NULL;
        mutex_lock(&odir->lock);
        rc = (odir->flags & VNODE_DEAD) ? -ENOENT : odir->ops->lookup(odir, oname, olen, &victim0);
        mutex_unlock(&odir->lock);
        if (rc)
            break;
        if (victim0->type == VNODE_DIR && odir != ndir && is_ancestor(victim0, ndir)) {
            vnode_put(victim0);
            rc = -EINVAL;   /* a directory may not be moved under itself */
            break;
        }
        struct vnode *first = odir, *second = NULL;
        if (odir != ndir) {
            if (is_ancestor(odir, ndir)) {
                first = odir;
                second = ndir;
            } else if (is_ancestor(ndir, odir)) {
                first = ndir;
                second = odir;
            } else {
                first = odir < ndir ? odir : ndir;
                second = odir < ndir ? ndir : odir;
            }
        }

        /* Phase 2: the parents, then the entries under them. */
        mutex_lock(&first->lock);
        if (second)
            mutex_lock_nested(&second->lock, VNODE_NESTED_PARENT2);

        struct vnode *victim = NULL, *replaced = NULL;
        if ((odir->flags | ndir->flags) & VNODE_DEAD)
            rc = -ENOENT;
        else
            rc = odir->ops->lookup(odir, oname, olen, &victim);
        bool changed = rc == 0 && victim != victim0;   /* unlinked and re-created meanwhile */
        vnode_put(victim0);
        if (rc == 0 && !changed) {
            if (vfs_entry_is_mountpoint(victim, odir, ndir) || victim->mnt != odir->mnt) {
                rc = -EBUSY;
            } else if (sticky_denies(odir, victim)) {
                rc = -EACCES;
            } else if (victim->type == VNODE_DIR && odir != ndir && vfs_permission(victim, VFS_MAY_WRITE) != 0) {
                rc = -EACCES;   /* moving a directory rewrites its ".." */
            } else if (ndir->ops->lookup(ndir, nname, nlen, &replaced) == 0) {
                if (replaced == victim)
                    rc = 0;   /* same entry: nothing to do */
                else if (replaced->type == VNODE_DIR && victim->type != VNODE_DIR)
                    rc = -EISDIR;
                else if (replaced->type != VNODE_DIR && victim->type == VNODE_DIR)
                    rc = -ENOTDIR;
                else if (vfs_entry_is_mountpoint(replaced, odir, ndir) || replaced->mnt != odir->mnt)
                    rc = -EBUSY;
                else if (sticky_denies(ndir, replaced))
                    rc = -EACCES;
            }
            if (rc == 0 && replaced != victim) {
                rc = odir->ops->rename(odir, oname, olen, victim, ndir, nname, nlen, replaced);
                if (rc == 0 && replaced && (replaced->type == VNODE_DIR || replaced->nlink == 0))
                    replaced->flags |= VNODE_DEAD;   /* as remove_entry: once its last name is gone */
            }
            if (replaced)
                vnode_put(replaced);
        }
        if (victim)
            vnode_put(victim);
        if (second)
            mutex_unlock(&second->lock);
        mutex_unlock(&first->lock);
        if (!changed)
            break;
        if (attempt == 8) {
            rc = -EBUSY;
            break;
        }
        /* The entry was replaced between the phases: the ancestry check
         * was for the old one, so start over. */
    }
    mutex_unlock(&mnt->rename_lock);
out_dirs:
    vnode_put(ndir);
    vnode_put(odir);
    return rc;
}

int vfs_stat(struct vnode *start, const char *path, struct cosmo_stat *st)
{
    struct vnode *vn;
    int rc = vfs_lookup(start, path, &vn);
    if (rc)
        return rc;
    mutex_lock(&vn->lock);
    vnode_stat(vn, st);
    mutex_unlock(&vn->lock);
    vnode_put(vn);
    return 0;
}

int vnode_statfs(struct vnode *vn, struct vfs_statfs *out)
{
    struct mount *mnt = vn->mnt;
    memset(out, 0, sizeof(*out));
    out->magic = mnt->fs->magic;
    out->bsize = PAGE_SIZE;
    out->namelen = VFS_NAME_MAX;
    out->flags = mnt->flags & MOUNT_RDONLY;
    if (mnt->fs->statfs != NULL)
        return mnt->fs->statfs(mnt, out);
    uint64_t limit = __atomic_load_n(&mnt->cache_limit_pages, __ATOMIC_RELAXED);
    if ((mnt->flags & MOUNT_CACHE_IS_STORE) && limit != 0) {
        uint64_t used = __atomic_load_n(&mnt->cache_pages, __ATOMIC_RELAXED);
        out->blocks = limit;
        out->bfree = out->bavail = used < limit ? limit - used : 0;
    }
    return 0;
}

int vfs_statfs(struct vnode *start, const char *path, struct vfs_statfs *out)
{
    struct vnode *vn;
    int rc = vfs_lookup(start, path, &vn);
    if (rc)
        return rc;
    rc = vnode_statfs(vn, out);
    vnode_put(vn);
    return rc;
}

int vfs_lstat(struct vnode *start, const char *path, struct cosmo_stat *st)
{
    struct vnode *vn;
    int rc = vfs_lookup_nofollow(start, path, &vn);
    if (rc)
        return rc;
    mutex_lock(&vn->lock);
    vnode_stat(vn, st);
    mutex_unlock(&vn->lock);
    vnode_put(vn);
    return 0;
}

int vfs_readlink(struct vnode *start, const char *path, char *buf, size_t len)
{
    if (len == 0)
        return -EINVAL;
    struct vnode *vn;
    int rc = vfs_lookup_nofollow(start, path, &vn);
    if (rc)
        return rc;
    if (vn->type != VNODE_LNK || vn->ops->readlink == NULL) {
        vnode_put(vn);
        return -EINVAL;   /* what POSIX says of a name that is not a link */
    }
    mutex_lock(&vn->lock);
    rc = vn->ops->readlink(vn, buf, len);
    mutex_unlock(&vn->lock);
    vnode_put(vn);
    return rc;
}

int vfs_symlink(struct vnode *start, const char *path, const char *target)
{
    if (target == NULL)
        return -EINVAL;
    size_t tlen = strnlen(target, VFS_PATH_MAX);
    if (tlen == 0)
        return -ENOENT;          /* a link to nothing names nothing */
    if (tlen >= VFS_PATH_MAX)
        return -ENAMETOOLONG;
    struct vnode *parent;
    char last[VFS_NAME_MAX + 1];
    size_t len;
    int rc = parent_for_mutation(start, path, last, &parent, &len);
    if (rc)
        return rc;
    struct vnode *vn = NULL;
    mutex_lock(&parent->lock);
    if (parent->flags & VNODE_DEAD)
        rc = -ENOENT;
    else if (parent->ops->symlink == NULL)
        rc = -EPERM;             /* a filesystem without links, as Linux answers */
    else {
        rc = parent->ops->lookup(parent, last, len, &vn);
        if (rc == 0) {
            vnode_put(vn);
            rc = -EEXIST;
        } else if (rc == -ENOENT) {
            rc = parent->ops->symlink(parent, last, len, target, &vn);
            if (rc == 0)
                vnode_put(vn);
        }
    }
    mutex_unlock(&parent->lock);
    vnode_put(parent);
    return rc;
}

/*
 * link (VFS-4; docs/kernel-services/vfs/design.md, "Hard links"): the new
 * name's directory as any creation takes it (parent_for_mutation's checks:
 * a writable mount, write and search permission), except that two mounts
 * are -EXDEV before either, as Linux answers; then the target under it, as
 * a child is taken under its parent (V7).
 */
int vfs_link(struct vnode *ostart, const char *oldpath, struct vnode *nstart, const char *newpath, bool follow)
{
    struct vnode *target;
    int rc = follow ? vfs_lookup(ostart, oldpath, &target) : vfs_lookup_nofollow(ostart, oldpath, &target);
    if (rc)
        return rc;
    if (target->type == VNODE_DIR) {
        vnode_put(target);
        return -EPERM;            /* a directory has one name: the tree stays a tree */
    }
    struct vnode *parent;
    char last[VFS_NAME_MAX + 1];
    size_t len;
    rc = vfs_walk_parent(nstart, newpath, last, &parent, &len, NULL);
    if (rc) {
        vnode_put(target);
        return rc;
    }
    if (len == 0 || vfs_dot_name(last, len))
        rc = len == 0 ? -EEXIST : -EINVAL;
    else if (parent->type != VNODE_DIR)
        rc = -ENOTDIR;
    else if (parent->mnt != target->mnt)
        rc = -EXDEV;
    else if (parent->mnt->flags & MOUNT_RDONLY)
        rc = -EROFS;
    else
        rc = vfs_permission(parent, VFS_MAY_WRITE | VFS_MAY_EXEC);
    if (rc) {
        vnode_put(parent);
        vnode_put(target);
        return rc;
    }
    mutex_lock(&parent->lock);
    if (parent->flags & VNODE_DEAD) {
        rc = -ENOENT;
    } else if (parent->ops->link == NULL) {
        rc = -EPERM;              /* a filesystem without links, as Linux answers */
    } else {
        struct vnode *vn = NULL;
        rc = parent->ops->lookup(parent, last, len, &vn);
        if (rc == 0) {
            vnode_put(vn);
            rc = -EEXIST;
        } else if (rc == -ENOENT) {
            mutex_lock_nested(&target->lock, VNODE_NESTED_CHILD);
            if (target->flags & VNODE_DEAD)
                rc = -ENOENT;     /* its last name went: nothing to name again */
            else if (target->nlink >= VFS_LINK_MAX)
                rc = -EMLINK;
            else
                rc = parent->ops->link(parent, last, len, target);
            mutex_unlock(&target->lock);
        }
    }
    mutex_unlock(&parent->lock);
    vnode_put(parent);
    vnode_put(target);
    return rc;
}
