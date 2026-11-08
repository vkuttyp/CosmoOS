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

/* --- path walk ------------------------------------------------------------ */

/* Cross into a mount whose root covers *vnp. Consumes the reference on
 * failure. -EBUSY while an unmount of that mount is being decided: the
 * walker must not fall through to the covered directory, since the
 * unmount may yet be refused and the mount restored. */
static int follow_mount(struct vnode **vnp)
{
    struct vnode *dir = *vnp;
    for (unsigned depth = 0; depth < 16; depth++) {
        /* Taken under dir->lock: vfs_umount sets `unmounting` and, when
         * it proceeds, removes its cover entry under the same lock, so a walker
         * either already holds the root (the reference scan sees it) or
         * is turned away. */
        mutex_lock(&dir->lock);
        struct mount *m = vfs_covering_mount_ns(dir, mountns_current());
        if (m && m->unmounting) {
            mutex_unlock(&dir->lock);
            vnode_put(dir);
            return -EBUSY;
        }
        struct vnode *root = m ? m->root : NULL;
        if (root)
            vnode_get(root);
        mutex_unlock(&dir->lock);
        if (root == NULL)
            break;
        vnode_put(dir);
        dir = root;
    }
    *vnp = dir;
    return 0;
}

/* --- permissions ------------------------------------------------------------ */

int vfs_permission(const struct vnode *vn, unsigned mask)
{
    const struct credentials *c = cred_current();
    if (cred_has(c, PRIV_SYS_ADMIN)) {
        /* PRIV_SYS_ADMIN reads, writes and searches everything (uid 0
         * alone does not, roadmap M5); executing a regular file still
         * needs it to be marked executable by someone. */
        if ((mask & VFS_MAY_EXEC) && vn->type == VNODE_REG && (vn->mode & 0111) == 0)
            return -EACCES;
        return 0;
    }
    unsigned bits;
    if (c->euid == vn->uid)
        bits = (vn->mode >> 6) & 7;
    else if (cred_in_group(c, vn->gid))
        bits = (vn->mode >> 3) & 7;
    else
        bits = vn->mode & 7;
    return (bits & mask) == mask ? 0 : -EACCES;
}

int vfs_setattr(struct vnode *vn, unsigned what, uint32_t mode, uint64_t mtime_ns)
{
    const struct credentials *c = cred_current();
    bool owner = cred_has(c, PRIV_SYS_ADMIN) || c->euid == vn->uid;
    if ((what & (VFS_SET_MODE | VFS_SET_MTIME)) && !owner)
        return -EPERM;
    if ((what & VFS_SET_MTIME_NOW) && !owner && vfs_permission(vn, VFS_MAY_WRITE) != 0)
        return -EACCES;
    if (vn->mnt != NULL && (vn->mnt->flags & MOUNT_RDONLY))
        return -EROFS;
    mutex_lock(&vn->lock);
    uint64_t now = vfs_now_ns();
    if (what & VFS_SET_MODE)
        vn->mode = mode & 07777u;
    if (what & VFS_SET_MTIME)
        vn->mtime_ns = mtime_ns;
    if (what & VFS_SET_MTIME_NOW)
        vn->mtime_ns = now;
    vn->ctime_ns = now;
    int rc = vn->ops->setattr != NULL ? vn->ops->setattr(vn) : 0;
    mutex_unlock(&vn->lock);
    return rc;
}

int vfs_chown(struct vnode *vn, uint32_t uid, uint32_t gid)
{
    const struct credentials *c = cred_current();
    bool admin = cred_has(c, PRIV_SYS_ADMIN);
    if (vn->mnt != NULL && (vn->mnt->flags & MOUNT_RDONLY))
        return -EROFS;
    /* The rule reads the owner and group under the lock the change is
     * made under: checked outside it, a group change could be authorised
     * by an owner another chown had already replaced (found in review). */
    mutex_lock(&vn->lock);
    bool owner = c->euid == vn->uid;
    if ((uid != VFS_ID_KEEP && !admin && !(owner && uid == vn->uid)) ||
        (gid != VFS_ID_KEEP && !admin && !(owner && (gid == vn->gid || cred_in_group(c, gid))))) {
        mutex_unlock(&vn->lock);
        return -EPERM;
    }
    const uint32_t old_uid = vn->uid, old_gid = vn->gid, old_mode = vn->mode;
    const uint64_t old_ctime = vn->ctime_ns;
    if (uid != VFS_ID_KEEP)
        vn->uid = uid;
    if (gid != VFS_ID_KEEP)
        vn->gid = gid;
    /* Linux's ATTR_KILL_SUID and ATTR_KILL_SGID, which chown_common sets
     * for every successful chown of a non-directory, -1, -1 included
     * (found in review): a program's set-id bits do not survive a chown. */
    if (vn->type == VNODE_REG) {
        vn->mode &= ~04000u;
        if (vn->mode & 0010u)
            vn->mode &= ~02000u;
    }
    vn->ctime_ns = vfs_now_ns();
    int rc = vn->ops->setattr != NULL ? vn->ops->setattr(vn) : 0;
    if (rc) {
        /* Not stored: the vnode says what the inode says, not what was
         * asked, or a later write-back would store it after all. */
        vn->uid = old_uid;
        vn->gid = old_gid;
        vn->mode = old_mode;
        vn->ctime_ns = old_ctime;
    }
    mutex_unlock(&vn->lock);
    return rc;
}

/* Search permission on a directory being entered; consumes `dir` on failure. */
static int may_search(struct vnode *dir)
{
    int rc = vfs_permission(dir, VFS_MAY_EXEC);
    if (rc)
        vnode_put(dir);
    return rc;
}

struct walk;
static struct vnode *walk_root(struct walk *w);

/* One component from `dir` (referenced, consumed). Returns a referenced
 * child. Entering a directory needs search permission on it. `..` stops
 * at the walk's root (walk_root). */
static int step(struct walk *w, struct vnode *dir, const char *name, size_t len, struct vnode **out)
{
    if (len == 1 && name[0] == '.') {
        int rc = may_search(dir);
        if (rc)
            return rc;
        *out = dir;
        return 0;
    }
    if (len == 2 && name[0] == '.' && name[1] == '.') {
        /*
         * ".." stops at the caller's root as it stops at the global one,
         * and the test comes *first* -- before the step that leaves a
         * mount through its mountpoint. A process rooted at a mounted
         * filesystem stands on that filesystem's root vnode, which the
         * crossing would replace with the covered vnode underneath it:
         * a different vnode, no longer equal to the root, and the walk
         * would climb out of the very mount it was confined to.
         */
        /* The root this walk began with, not the process's root now: a
         * chroot on another thread (P39) must not move the boundary under
         * a walk already past it. */
        bool at_root = dir == walk_root(w);
        if (at_root) {
            int prc = may_search(dir);
            if (prc)
                return prc;
            *out = dir;
            return 0;
        }

        /* At a mount root, ".." leaves through the mountpoint. */
        struct vnode *base = dir;
        if (dir->mnt->root == dir && dir->mnt->mountpoint) {
            base = dir->mnt->mountpoint;
            vnode_get(base);
            vnode_put(dir);
        }
        int rc = may_search(base);
        if (rc)
            return rc;
        if (base->mnt->root == base) {   /* the global root: stays put */
            *out = base;
            return 0;
        }
        mutex_lock(&base->lock);
        rc = base->ops->lookup(base, "..", 2, out);
        mutex_unlock(&base->lock);
        vnode_put(base);
        return rc;
    }
    int prc = may_search(dir);
    if (prc)
        return prc;
    mutex_lock(&dir->lock);
    int rc = (dir->flags & VNODE_DEAD) ? -ENOENT : dir->ops->lookup(dir, name, len, out);
    mutex_unlock(&dir->lock);
    vnode_put(dir);
    if (rc)
        return rc;
    return follow_mount(out);
}

/*
 * Whether `dir` lies under `root`: climbing `..` as step() climbs it -- out
 * of a mount through its mountpoint, then the filesystem's own entry --
 * meets `root` before the top of the tree. Free for an unconfined walk
 * (`root` is the global root, which everything is under); for a confined
 * one it costs a lookup per level, bounded by the depth a path can name.
 */
#define WITHIN_ROOT_MAX_DEPTH (VFS_PATH_MAX / 2)
static bool within_root(struct vnode *dir, struct vnode *root)
{
    if (root == NULL || root == g_root_mount->root)
        return true;
    struct vnode *cur = dir;
    vnode_get(cur);
    for (unsigned depth = 0; depth < WITHIN_ROOT_MAX_DEPTH; depth++) {
        if (cur == root) {
            vnode_put(cur);
            return true;
        }
        if (cur->mnt->root == cur) {
            if (cur->mnt->mountpoint == NULL)
                break;   /* the top of the tree, and the root was not on the way */
            struct vnode *mp = cur->mnt->mountpoint;
            vnode_get(mp);
            vnode_put(cur);
            cur = mp;
            continue;   /* the covered directory, then its own `..` */
        }
        struct vnode *parent = NULL;
        mutex_lock(&cur->lock);
        int rc = (cur->flags & VNODE_DEAD) ? -ENOENT : cur->ops->lookup(cur, "..", 2, &parent);
        mutex_unlock(&cur->lock);
        vnode_put(cur);
        if (rc)
            return false;   /* removed, or a filesystem that cannot say: not shown to be inside */
        cur = parent;
    }
    vnode_put(cur);
    return false;
}

static bool dot_name(const char *name, size_t len)
{
    return (len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.');
}

/*
 * One path resolution's symlink state: how many links it has expanded,
 * and the two buffers an expansion needs -- the path being walked and
 * the one being built -- carved from a single allocation taken on the
 * first expansion. A walk that meets no link allocates nothing, which is
 * every walk in a tree without links.
 *
 * The halves ping-pong: a target is read into the half that is not the
 * current path and the remainder is appended there, so the copy never
 * overlaps the string it reads (the remainder points into the current
 * path).
 */
struct walk {
    /* The traversed name (P32): the absolute name, relative to the
     * caller's root, of the directory the walk is in. NULL when the
     * caller did not ask; `name_long` once it would not fit. */
    char *name;
    size_t name_len, name_cap;
    bool name_long;
    unsigned links;
    char *buf;      /* 2 * VFS_PATH_MAX, or NULL */
    char *cur;      /* the half holding the path being walked */
    char *alt;      /* the other half */
    /* The caller's root, referenced, captured at the walk's first need of
     * it and used for all of it: an absolute start, an absolute link
     * target and every `..` boundary (shell completeness S4, P39 -- the
     * root became mutable with chroot, and a walk that re-read it at each
     * `..` could see a nested chroot move the boundary below where it
     * already stood, and climb past both). */
    struct vnode *root;
};

static struct vnode *walk_root(struct walk *w)
{
    if (w->root == NULL)
        w->root = vfs_current_root();
    return w->root;
}

static void walk_fini(struct walk *w)
{
    kfree(w->buf);
    w->buf = NULL;
    if (w->root != NULL)
        vnode_put(w->root);
    w->root = NULL;
}

/*
 * The traversed name follows the walk's own rules, applied to a string
 * (docs/audit/next-subsystem-cwd-name.md, section 1): an absolute start
 * or an absolute link target is `/`; a component entered as anything
 * but a followed link is appended, `.` adding nothing and `..` removing
 * the last component (and staying at `/`). A link is never appended: a
 * relative target continues from the directory the link was found in,
 * which is the name as it stands. `..` removing a component is right
 * because the name holds no link -- the directory before its last
 * component is the one `..` reaches, a mount root's included, whose
 * mountpoint is the component removed.
 */
static void name_reset(struct walk *w)
{
    if (w->name == NULL)
        return;
    w->name[0] = '/';
    w->name[1] = '\0';
    w->name_len = 1;
    w->name_long = false;   /* what overflowed before the restart is no part of the name (found in review) */
}

static void name_step(struct walk *w, const char *comp, size_t len)
{
    if (w->name == NULL || (len == 1 && comp[0] == '.'))
        return;
    if (len == 2 && comp[0] == '.' && comp[1] == '.') {
        size_t at = w->name_len;
        while (at > 1 && w->name[at - 1] != '/')
            at--;
        w->name_len = at > 1 ? at - 1 : 1;   /* the separator goes too, unless it is the root */
        w->name[w->name_len] = '\0';
        return;
    }
    size_t sep = w->name_len > 1 ? 1 : 0;
    if (w->name_len + sep + len + 1 > w->name_cap) {
        w->name_long = true;   /* reported by the caller, which asked for it */
        return;
    }
    if (sep)
        w->name[w->name_len++] = '/';
    memcpy(w->name + w->name_len, comp, len);
    w->name_len += len;
    w->name[w->name_len] = '\0';
}

/*
 * Replace the path being walked with `link`'s target followed by `rest`.
 * Returns the new path through `out` and whether it is absolute; the
 * caller decides where to continue from. -ELOOP past the budget,
 * -ENAMETOOLONG when the result would not fit a path.
 */
static int walk_expand(struct walk *w, struct vnode *link, const char *rest, const char **out, bool *absolute)
{
    if (w->links >= VFS_MAX_SYMLINKS)
        return -ELOOP;
    if (link->ops->readlink == NULL)
        return -EIO;   /* a link this filesystem cannot read is not a link */
    if (w->buf == NULL) {
        w->buf = kmalloc(2 * VFS_PATH_MAX, 0);
        if (w->buf == NULL)
            return -ENOMEM;
        w->cur = w->buf;
        w->alt = w->buf + VFS_PATH_MAX;
    }
    char *dst = w->alt;
    mutex_lock(&link->lock);
    int n = link->ops->readlink(link, dst, VFS_PATH_MAX - 1);
    mutex_unlock(&link->lock);
    if (n < 0)
        return n;
    if (n == 0)
        return -ENOENT;   /* an empty target names nothing */
    size_t restlen = rest != NULL ? strlen(rest) : 0;
    if ((size_t)n + (restlen != 0 ? restlen + 1 : 0) >= VFS_PATH_MAX)
        return -ENAMETOOLONG;   /* never silently drop what follows the link */
    size_t at = (size_t)n;
    if (restlen != 0) {
        dst[at++] = '/';
        memcpy(dst + at, rest, restlen);
        at += restlen;
    }
    dst[at] = '\0';
    char *swap = w->cur;
    w->cur = w->alt;
    w->alt = swap;
    *absolute = dst[0] == '/';
    *out = dst;
    w->links++;
    return 0;
}

/* Walk every component but the last, expanding any symbolic link met on
 * the way. On success *parent is referenced and the last component is
 * copied into `namebuf` (a VFS_NAME_MAX + 1 buffer; empty for the root),
 * which is the caller's because the path may live in the walk's own
 * scratch and not outlive it. */
static int walk_parent(struct vnode *start, const char *path, struct walk *w, char *namebuf,
                       struct vnode **parent, size_t *last_len, bool *trailing_slash)
{
    if (path == NULL || strnlen(path, VFS_PATH_MAX) >= VFS_PATH_MAX)
        return -ENAMETOOLONG;
    if (trailing_slash)
        *trailing_slash = false;

    struct vnode *cur;
    if (path[0] == '/' || start == NULL) {
        /* Absolute means absolute *to this process*: a process given a
         * root below the global one cannot name its way out with a
         * leading slash. */
        cur = walk_root(w);
        if (cur == NULL)
            return -ENOENT;   /* no root mounted yet: nothing is absolute */
        vnode_get(cur);
        name_reset(w);
    } else {
        /* The one line every relative walk from every caller shares, and
         * the first dereference of the caller's starting directory: the
         * held-walk seam sits in front of it, with the pointer read and
         * nothing yet taken. */
        int hrc = vfs_cwd_hold_walk(start);
        if (hrc)
            return hrc;
        /* A confined walk starts only from inside its root: a directory
         * renamed out from under the root after it was entered or opened
         * would otherwise be a base whose `..` never meets the root (P39;
         * found in review, and older than chroot -- a root given at spawn
         * had it too). */
        if (!within_root(start, walk_root(w)))
            return -EACCES;
        cur = start;
        vnode_get(cur);
    }
    while (*path == '/')
        path++;

    unsigned components = 0;
    for (;;) {
        const char *end = path;
        while (*end && *end != '/')
            end++;
        size_t len = (size_t)(end - path);
        const char *next = end;
        while (*next == '/')
            next++;

        if (len == 0) {              /* the root itself, or a trailing separator */
            *parent = cur;
            namebuf[0] = '\0';
            *last_len = 0;
            return 0;
        }
        if (len > VFS_NAME_MAX) {
            vnode_put(cur);
            return -ENAMETOOLONG;
        }
        if (*next == '\0') {
            *parent = cur;
            memcpy(namebuf, path, len);
            namebuf[len] = '\0';
            *last_len = len;
            if (trailing_slash)
                *trailing_slash = *end == '/';
            return 0;
        }
        if (++components > VFS_MAX_COMPONENTS) {
            vnode_put(cur);
            return -ELOOP;
        }
        if (cur->type != VNODE_DIR) {
            vnode_put(cur);
            return -ENOTDIR;
        }
        /* step() consumes this reference (it puts `dir` before returning
         * the child), and a relative target resolves against the
         * directory the link was found in -- so the walk holds a second
         * one across the step and releases it as soon as the child turns
         * out not to be a link. */
        struct vnode *linkdir = cur;
        vnode_get(linkdir);
        struct vnode *child;
        int rc = step(w, cur, path, len, &child);
        if (rc) {
            vnode_put(linkdir);
            return rc;
        }
        if (child->type == VNODE_LNK) {
            const char *expanded;
            bool absolute = false;
            rc = walk_expand(w, child, next, &expanded, &absolute);
            vnode_put(child);
            if (rc) {
                vnode_put(linkdir);
                return rc;
            }
            if (absolute) {
                vnode_put(linkdir);
                cur = walk_root(w);
                if (cur == NULL)
                    return -ENOENT;
                vnode_get(cur);
                name_reset(w);
            } else {
                cur = linkdir;   /* keeps linkdir's reference; the name stays the directory's */
            }
            path = expanded;
            while (*path == '/')
                path++;
            continue;
        }
        vnode_put(linkdir);
        name_step(w, path, len);
        cur = child;
        path = next;
    }
}

/* The name `name` in the referenced directory `dir`, with the search
 * permission check and the mount crossing the walk applies. `dir` stays
 * the caller's. */
static int lookup_in(struct vnode *dir, const char *name, size_t len, struct vnode **out)
{
    int rc = vfs_permission(dir, VFS_MAY_EXEC);
    if (rc)
        return rc;
    mutex_lock(&dir->lock);
    rc = (dir->flags & VNODE_DEAD) ? -ENOENT : dir->ops->lookup(dir, name, len, out);
    mutex_unlock(&dir->lock);
    if (rc)
        return rc;
    return follow_mount(out);
}

#define RESOLVE_FOLLOW 1u   /* follow a link named by the last component */

/*
 * A whole path to a referenced vnode, expanding links on the way and --
 * with RESOLVE_FOLLOW -- at the end. The walk carries the budget, so a
 * chain that ends in a link cannot outrun it. An absolute target
 * restarts at the caller's root (walk_parent does that for a NULL
 * start), which is what keeps a link inside a process's root.
 */
static int resolve(struct vnode *start, const char *path, struct walk *w, unsigned flags, struct vnode **out)
{
    char name[VFS_NAME_MAX + 1];
    struct vnode *base = start;   /* borrowed from the caller ... */
    bool owned = false;           /* ... until an expansion makes it ours */
    for (;;) {
        struct vnode *parent;
        size_t len;
        bool trailing = false;
        int rc = walk_parent(base, path, w, name, &parent, &len, &trailing);
        if (owned) {
            vnode_put(base);   /* walk_parent took its own reference */
            base = NULL;
            owned = false;
        }
        if (rc)
            return rc;
        if (len == 0) {   /* the root itself: never a link */
            *out = parent;
            return 0;
        }
        if (parent->type != VNODE_DIR) {
            vnode_put(parent);
            return -ENOTDIR;
        }
        struct vnode *vn;
        if (dot_name(name, len)) {
            vnode_get(parent);
            rc = step(w, parent, name, len, &vn);   /* consumes that reference */
        } else {
            rc = lookup_in(parent, name, len, &vn);
        }
        if (rc) {
            vnode_put(parent);
            return rc;
        }
        if (vn->type != VNODE_LNK || !(flags & RESOLVE_FOLLOW)) {
            vnode_put(parent);
            name_step(w, name, len);
            /* `name/` names a directory, whatever the name resolved to. */
            if (trailing && vn->type != VNODE_DIR) {
                vnode_put(vn);
                return -ENOTDIR;
            }
            *out = vn;
            return 0;
        }
        const char *expanded;
        bool absolute = false;
        rc = walk_expand(w, vn, NULL, &expanded, &absolute);
        vnode_put(vn);
        if (rc) {
            vnode_put(parent);
            return rc;
        }
        if (absolute) {
            vnode_put(parent);
            base = NULL;
        } else {
            base = parent;
            owned = true;
        }
        path = expanded;
    }
}

static int lookup_flags(struct vnode *start, const char *path, unsigned flags, struct vnode **out)
{
    struct walk w = { 0 };
    int rc = resolve(start, path, &w, flags, out);
    walk_fini(&w);
    return rc;
}

int vfs_lookup(struct vnode *start, const char *path, struct vnode **out)
{
    return lookup_flags(start, path, RESOLVE_FOLLOW, out);
}

int vfs_lookup_named(struct vnode *start, const char *startname, const char *path, struct vnode **out,
                     char *name, size_t n)
{
    if (name == NULL || n < 2)
        return -EINVAL;
    if (path != NULL && path[0] != '/' && start != NULL) {
        /* A relative walk continues from its start's name, which must be
         * one: a directory that recorded none cannot name what is below it. */
        if (startname == NULL || startname[0] != '/')
            return -ENOENT;
        if (strlcpy(name, startname, n) >= n)
            return -ENAMETOOLONG;
    } else {
        name[0] = '/';   /* walk_parent resets it too; this is the empty-path case */
        name[1] = '\0';
    }
    struct walk w = { .name = name, .name_len = strlen(name), .name_cap = n };
    int rc = resolve(start, path, &w, RESOLVE_FOLLOW, out);
    walk_fini(&w);
    if (rc)
        return rc;
    if (w.name_long) {
        vnode_put(*out);
        *out = NULL;
        return -ENAMETOOLONG;
    }
    return 0;
}

int vfs_lookup_nofollow(struct vnode *start, const char *path, struct vnode **out)
{
    return lookup_flags(start, path, 0, out);
}


/* --- files ---------------------------------------------------------------- */

static void file_release(struct kobject *obj)
{
    struct file *f = container_of(obj, struct file, obj);
    if (f->dev_open && f->vn->ops->release)
        f->vn->ops->release(f->vn, f);   /* once, on the last reference */
    if (f->vn->type == VNODE_REG && f->vn->pc.nr_dirty && f->vn->ops->writepage) {
        mutex_lock(&f->vn->lock);
        pagecache_sync(f->vn);
        mutex_unlock(&f->vn->lock);
    }
    if (f->dir_path)
        kfree(f->dir_path);
    if (f->dir_root)
        vnode_put(f->dir_root);
    vnode_put(f->vn);
    kfree(f);
}

void file_set_dir_path(struct file *f, const char *name)
{
    /* The name is the open's own traversed name (vfs_open_named, P32):
     * the path its walk took to this very vnode. */
    if (f->vn->type != VNODE_DIR || f->dir_path != NULL || name == NULL || name[0] != '/')
        return;
    size_t n = strlen(name) + 1;
    char *p = kmalloc(n, 0);
    if (p == NULL)
        return;
    memcpy(p, name, n);
    f->dir_path = p;
}

static int64_t file_obj_read(struct kobject *obj, void *buf, size_t len)
{
    return file_read(container_of(obj, struct file, obj), buf, len);
}

static int64_t file_obj_write(struct kobject *obj, const void *buf, size_t len)
{
    return file_write(container_of(obj, struct file, obj), buf, len);
}

static int file_obj_flush(struct kobject *obj)
{
    return file_flush(container_of(obj, struct file, obj));
}

/*
 * Readiness through the vnode, when it has an opinion (a FIFO, a device
 * that implements it); otherwise a file is always ready, never changes
 * and cannot be made non-blocking, which is the truth of a regular
 * file and was the file type's only answer before the named-pipes unit.
 */
static unsigned file_obj_ready(struct kobject *obj)
{
    struct file *f = container_of(obj, struct file, obj);
    if (f->vn->ops->ready)
        return f->vn->ops->ready(f->vn, f);
    return COSMO_IO_READABLE | COSMO_IO_WRITABLE;
}

static struct waitqueue *file_obj_poll_wq(struct kobject *obj, unsigned events)
{
    struct file *f = container_of(obj, struct file, obj);
    return f->vn->ops->poll_wq ? f->vn->ops->poll_wq(f->vn, f, events) : NULL;
}

static int file_obj_set_nonblock(struct kobject *obj, int on)
{
    struct file *f = container_of(obj, struct file, obj);
    return f->vn->ops->set_nonblock ? f->vn->ops->set_nonblock(f->vn, f, on) : -EOPNOTSUPP;
}

/* A regular file, or a device whose reads are always whole (/dev/zero):
 * a large read is filled as one read (kobject_io_type.read_filled). A
 * regular file's position lock, f->lock, is held across every piece and
 * its copy to the caller, as Linux holds f_pos_lock, so a second reader
 * or a seek on the same open file waits for the whole read; the order is
 * f->lock before vn->lock (file_pread) and before what a fault in `put`
 * takes, and no fault path takes an open file's lock. The position moves
 * by what `put` accepted: a fault after some pieces returns those bytes,
 * as Linux does, and the next read starts after them. A device has no
 * position and no lock (file_read). */
static int64_t file_obj_read_filled(struct kobject *obj, void *buf, size_t cap, size_t len,
                                    int (*put)(void *ctx, const void *piece, size_t n), void *ctx)
{
    struct file *f = container_of(obj, struct file, obj);
    bool reg = f->vn->type == VNODE_REG;
    if (!reg) {
        const struct chrdev_ops *chr = f->vn->type == VNODE_CHR ? ramfs_chr_ops_of(f->vn) : NULL;
        if (chr == NULL || !chr->fills)
            return -EOPNOTSUPP;
    }
    if (reg)
        mutex_lock(&f->lock);
    size_t done = 0;
    int64_t rc = 0;
    while (done < len) {
        size_t n = len - done < cap ? len - done : cap;
        rc = reg ? file_pread(f, buf, n, f->pos + done) : file_read(f, buf, n);
        KASSERT(rc <= (int64_t)n);
        if (rc > (int64_t)n)
            rc = -EIO;
        if (rc <= 0)
            break;
        if (put(ctx, buf, (size_t)rc)) {
            rc = -EFAULT;
            break;
        }
        done += (size_t)rc;
        if ((size_t)rc < n)
            break;
    }
    if (reg) {
        f->pos += done;
        mutex_unlock(&f->lock);
    }
    return done > 0 ? (int64_t)done : rc;
}

static const struct kobject_io_type file_type = {
    .base = { .name = "file", .release = file_release, .flags = KOBJECT_TYPE_IO },
    .read = file_obj_read,
    .read_filled = file_obj_read_filled,
    .write = file_obj_write,
    .flush = file_obj_flush,
    .ready = file_obj_ready,
    .poll_wq = file_obj_poll_wq,
    .set_nonblock = file_obj_set_nonblock,
};

/* A stream: no position, no seek, the driver's own read and write. */
/*
 * Is this file's text shared with a process that is executing it?
 *
 * Every path that changes a regular file's contents asks this, not just
 * the write: a truncate removes or zeroes the very pages a running
 * program is executing, which is the same harm by another name. Called
 * with `vn->lock` held, and takes the cache lock inside it -- the
 * documented order is vnode -> pagecache
 * (docs/audit/next-subsystem-elf-shared-text.md).
 */
static bool text_busy_locked(struct vnode *vn)
{
    pagecache_lock(vn);
    bool busy = pagecache_text_busy(vn);
    pagecache_unlock(vn);
    return busy;
}

static bool vnode_is_stream(const struct vnode *vn)
{
    return vn->type == VNODE_CHR || vn->type == VNODE_FIFO;
}

bool file_nonblocking(const struct file *f)
{
    return (file_flags(f) & COSMO_O_NONBLOCK) != 0;
}

int file_set_nonblock(struct file *f, int on)
{
    unsigned old = file_flags(f);
    if (on >= 0) {
        if (on)
            old = __atomic_fetch_or(&f->flags, COSMO_O_NONBLOCK, __ATOMIC_RELAXED);
        else
            old = __atomic_fetch_and(&f->flags, ~COSMO_O_NONBLOCK, __ATOMIC_RELAXED);
    }
    return (old & COSMO_O_NONBLOCK) ? 1 : 0;
}

struct file *file_from_kobject(struct kobject *obj)
{
    return obj->type == &file_type.base ? container_of(obj, struct file, obj) : NULL;
}

/* Run the vnode's per-open hook, if any. On refusal the file is dropped
 * (its release hook does not run, since open never succeeded) and the
 * caller's vnode reference goes with it. */
static int file_run_open(struct file *f)
{
    struct vnode *vn = f->vn;
    if (vn->ops->open == NULL)
        return 0;
    int rc = vn->ops->open(vn, f);
    if (rc != 0) {
        file_put(f);
        return rc;
    }
    f->dev_open = true;
    return 0;
}

static struct file *file_alloc(struct vnode *vn, unsigned flags)
{
    struct file *f = kzalloc(sizeof(*f));
    if (f == NULL)
        return NULL;
    kobject_init(&f->obj, &file_type.base);
    f->vn = vn;   /* takes the caller's reference */
    f->flags = flags;
    mutex_init(&f->lock, "file");
    f->wb_seq_seen = pagecache_wb_seq(&vn->pc);   /* a failure before this open is not this file's */
    if (vn->type == VNODE_DIR)
        f->dir_root = vfs_current_root();   /* the opener's: see file_dir_base_ok */
    return f;
}

bool file_dir_base_ok(const struct file *f)
{
    if (f->dir_root == NULL)
        return true;
    struct vnode *root = vfs_current_root();
    bool ok = root == f->dir_root;
    vnode_put(root);
    return ok;
}

/* vfs_open's walk, which keeps the traversed name when `w` asks for one. */
static int open_walk(struct vnode *start, const char *path, unsigned flags, uint32_t mode, struct file **out,
                     struct walk *w)
{
    unsigned acc = flags & COSMO_O_ACCMODE;
    if (acc == COSMO_O_ACCMODE)
        return -EINVAL;

    /*
     * The resolution is a loop so that a symbolic link named by the last
     * component re-enters it with the target path: O_CREAT then creates
     * the target of a dangling link, O_TRUNC truncates the target, and
     * the permission checks are the target's -- which is what every one
     * of those flags means. The walk carries the budget across rounds,
     * so a chain ending in a link cannot outrun it, and every exit runs
     * one cleanup.
     */
    struct vnode *base = start;   /* borrowed from the caller ... */
    bool base_owned = false;      /* ... until an expansion makes it ours */
    struct vnode *vn = NULL;
    bool created = false;
    int rc;

    for (;;) {
        struct vnode *parent;
        char last[VFS_NAME_MAX + 1];
        size_t len;
        bool trailing = false;
        rc = walk_parent(base, path, w, last, &parent, &len, &trailing);
        if (base_owned) {
            vnode_put(base);   /* walk_parent took its own reference */
            base = NULL;
            base_owned = false;
        }
        if (rc)
            goto out;
        if (trailing)
            flags |= COSMO_O_DIRECTORY;

        if (len == 0) {
            vn = parent;   /* the root itself */
            break;
        }
        if (parent->type != VNODE_DIR) {
            vnode_put(parent);
            rc = -ENOTDIR;
            goto out;
        }
        if (dot_name(last, len)) {
            rc = step(w, parent, last, len, &vn);   /* consumes the reference */
            if (rc)
                goto out;
            name_step(w, last, len);
            break;                               /* "." and ".." are directories */
        }

        rc = vfs_permission(parent, VFS_MAY_EXEC);   /* search the last directory */
        if (rc) {
            vnode_put(parent);
            goto out;
        }
        mutex_lock(&parent->lock);
        rc = (parent->flags & VNODE_DEAD) ? -ENOENT : parent->ops->lookup(parent, last, len, &vn);
        if (rc == -ENOENT && (flags & COSMO_O_CREAT)) {
            if (parent->mnt->flags & MOUNT_RDONLY)
                rc = -EROFS;
            else if (parent->ops->create == NULL)
                rc = -ENOTSUP;
            else if (vfs_permission(parent, VFS_MAY_WRITE) != 0)
                rc = -EACCES;   /* creating an entry writes the directory */
            else {
                rc = parent->ops->create(parent, last, len, mode & 07777, &vn);
                created = rc == 0;
            }
        } else if (rc == 0 && (flags & COSMO_O_CREAT) && (flags & COSMO_O_EXCL)) {
            vnode_put(vn);
            rc = -EEXIST;   /* a link counts as existing, as POSIX says */
        }
        mutex_unlock(&parent->lock);
        if (rc == 0)
            rc = follow_mount(&vn);
        if (rc) {
            vnode_put(parent);
            goto out;
        }
        if (vn->type != VNODE_LNK) {
            vnode_put(parent);
            name_step(w, last, len);
            break;
        }
        if (flags & COSMO_O_NOFOLLOW) {
            vnode_put(vn);
            vnode_put(parent);
            vn = NULL;
            rc = -ELOOP;   /* Linux's answer for a link opened with O_NOFOLLOW */
            goto out;
        }
        const char *expanded;
        bool absolute = false;
        rc = walk_expand(w, vn, NULL, &expanded, &absolute);
        vnode_put(vn);
        vn = NULL;
        if (rc) {
            vnode_put(parent);
            goto out;
        }
        if (absolute) {
            vnode_put(parent);   /* an absolute target restarts at the caller's root */
        } else {
            base = parent;       /* a relative one, at the link's own directory */
            base_owned = true;
        }
        path = expanded;
    }
    walk_fini(w);

    if (flags & COSMO_O_DIRECTORY) {
        if (vn->type != VNODE_DIR) {
            vnode_put(vn);
            return -ENOTDIR;
        }
    }
    if (vn->type == VNODE_SOCK) {
        vnode_put(vn);
        return -ENXIO;   /* a socket's name is connected to, not opened (Linux's answer) */
    }
    if (vn->type == VNODE_DIR && acc != COSMO_O_RDONLY) {
        vnode_put(vn);
        return -EISDIR;
    }
    if (acc != COSMO_O_RDONLY && (vn->mnt->flags & MOUNT_RDONLY)) {
        vnode_put(vn);
        return -EROFS;
    }
    /* The file itself: read and/or write per the access mode. A file this
     * call just created is the caller's regardless of the mode it asked
     * for (POSIX), so the check is skipped for it. */
    if (!created) {
        unsigned need = 0;
        if (acc != COSMO_O_WRONLY)
            need |= VFS_MAY_READ;
        if (acc != COSMO_O_RDONLY)
            need |= VFS_MAY_WRITE;
        rc = vfs_permission(vn, need);
        if (rc) {
            vnode_put(vn);
            return rc;
        }
    }
    if ((flags & COSMO_O_TRUNC) && vn->type == VNODE_REG && acc != COSMO_O_RDONLY) {
        mutex_lock(&vn->lock);
        /* Truncating a running program removes the pages it is
         * executing: the same harm the write refuses. */
        rc = text_busy_locked(vn) ? -ETXTBSY
           : vn->ops->truncate    ? vn->ops->truncate(vn, 0) : -ENOTSUP;
        mutex_unlock(&vn->lock);
        if (rc) {
            vnode_put(vn);
            return rc;
        }
    }

    struct file *f = file_alloc(vn, flags);
    if (f == NULL) {
        vnode_put(vn);
        return -ENOMEM;
    }
    int orc = file_run_open(f);
    if (orc != 0)
        return orc;
    *out = f;
    return 0;

out:
    walk_fini(w);
    if (base_owned)
        vnode_put(base);
    return rc;
}

int vfs_open(struct vnode *start, const char *path, unsigned flags, uint32_t mode, struct file **out)
{
    struct walk w = { 0 };
    return open_walk(start, path, flags, mode, out, &w);
}

int vfs_open_named(struct vnode *start, const char *startname, const char *path, unsigned flags, uint32_t mode,
                   struct file **out, char *name, size_t n)
{
    /*
     * The open's own walk names what it opened (P32): one walk, so the
     * name is of the very vnode the file holds -- a second walk to name
     * it could meet a rename in between and name another directory.
     * The open never fails for the name's sake: a relative path from a
     * start with no name, or a name that does not fit, leaves `name`
     * empty.
     */
    struct walk w = { 0 };
    name[0] = '\0';
    if (n >= 2) {
        bool seeded = true;
        if (path != NULL && path[0] != '/' && start != NULL)
            seeded = startname != NULL && startname[0] == '/' && strlcpy(name, startname, n) < n;
        else
            strlcpy(name, "/", n);
        if (seeded) {
            w.name = name;
            w.name_len = strlen(name);
            w.name_cap = n;
        } else {
            name[0] = '\0';
        }
    }
    int rc = open_walk(start, path, flags, mode, out, &w);
    if (rc == 0 && w.name_long)
        name[0] = '\0';
    return rc;
}

int vfs_open_vnode(struct vnode *vn, unsigned flags, struct file **out)
{
    struct file *f = file_alloc(vn, flags);
    if (f == NULL) {
        vnode_put(vn);
        return -ENOMEM;
    }
    int orc = file_run_open(f);
    if (orc != 0)
        return orc;
    *out = f;
    return 0;
}

int64_t file_pread(struct file *f, void *buf, size_t len, uint64_t off)
{
    struct vnode *vn = f->vn;
    if ((file_flags(f) & COSMO_O_ACCMODE) == COSMO_O_WRONLY)
        return -EBADF;
    if (vn->type == VNODE_DIR)
        return -EISDIR;
    if (len == 0)
        return 0;
    if (vnode_is_stream(vn)) {
        /* No vn->lock across a driver (a FIFO's ring is one too).
         * It guards nothing on this path --
         * the arm below touches no vnode field -- and a device may sleep
         * for an unbounded time inside it (tty_read waits for a line),
         * while this is the lock every other opener of the node needs:
         * ramfs_lookup hands out one vnode per device node. The VFS
         * already calls a device's other entry points outside it,
         * ops->open from file_run_open and ops->release from
         * file_release (docs/audit/next-subsystem-chrdev-vnode-lock.md).
         *
         * Nor f->lock, the open file's own, since the device-readiness
         * unit: a stream's read may wait (a FIFO for bytes, the tap for a
         * frame, the terminal for a line), and a reader that held the
         * open file's lock while it waited deadlocked a writer on the
         * same open file -- a child given the parent's handle, blocked in
         * read, and the parent's write blocked behind it for good. The
         * open file's lock is the position's lock, and a stream has no
         * position; a stream's per-open state is the driver's to protect
         * (fsctl keeps its own mutex over its result). file_read and
         * file_write therefore call a stream's driver with no lock held,
         * as the pipe and socket objects are called. */
        lockdep_assert_not_held(&vn->lock, LOCKDEP_KIND_MUTEX);
        return vn->ops->read_file ? vn->ops->read_file(vn, f, off, buf, len)
             : vn->ops->read      ? vn->ops->read(vn, off, buf, len) : -ENOTSUP;
    }
    mutex_lock(&vn->lock);
    int64_t n = pagecache_read(vn, off, buf, len);
    mutex_unlock(&vn->lock);
    return n;
}

/*
 * The dirty bound (MEM-9): a writer to a cached file sleeps here, if it
 * must, before it takes the open file's lock or the vnode's. file_sync
 * cleans pages holding the open file's lock, so a writer asleep under it
 * would wait for what that lock holds up (pagecache_throttle).
 */
void file_throttle(struct file *f)
{
    if (f->vn->type == VNODE_REG && !vnode_is_stream(f->vn))
        pagecache_throttle(f->vn);
}

int64_t file_pwrite(struct file *f, const void *buf, size_t len, uint64_t off)
{
    file_throttle(f);
    return file_pwrite_held(f, buf, len, off);
}

int64_t file_pwrite_held(struct file *f, const void *buf, size_t len, uint64_t off)
{
    struct vnode *vn = f->vn;
    if ((file_flags(f) & COSMO_O_ACCMODE) == COSMO_O_RDONLY)
        return -EBADF;
    if (vn->type == VNODE_DIR)
        return -EISDIR;
    if (vn->mnt->flags & MOUNT_RDONLY)
        return -EROFS;
    if (len == 0)
        return 0;
    if (vnode_is_stream(vn)) {
        /* As in file_pread: the driver runs with no filesystem lock and no file lock. */
        lockdep_assert_not_held(&vn->lock, LOCKDEP_KIND_MUTEX);
        return vn->ops->write_file ? vn->ops->write_file(vn, f, off, buf, len)
             : vn->ops->write      ? vn->ops->write(vn, off, buf, len) : -ENOTSUP;
    }
    mutex_lock(&vn->lock);
    /*
     * A file somebody is executing does not change underneath them.
     * Shared text means the running program's instructions *are* these
     * pages, so a write here would rewrite them in every process at
     * once; POSIX names the refusal and this is where it belongs, under
     * the lock the write itself holds, with the cache lock taken inside
     * it (the order is vnode -> pagecache)
     * (docs/audit/next-subsystem-elf-shared-text.md).
     */
    if (text_busy_locked(vn)) {
        mutex_unlock(&vn->lock);
        return -ETXTBSY;
    }
    int64_t n = pagecache_write(vn, off, buf, len);
    if (n == -ENOSPC && vn->ops->sync) {
        /* The filesystem charges a dirty page for the block it will
         * take (MEM-9); blocks the open transaction has replaced are free
         * only once it commits. Commit, as file_sync does, and ask once
         * more before the writer is told. */
        if (pagecache_sync(vn) == 0 && vn->ops->sync(vn) == 0)
            n = pagecache_write(vn, off, buf, len);
    }
    if (n > 0)
        vn->mtime_ns = vfs_now_ns();   /* vnode state, so it stays inside */
    mutex_unlock(&vn->lock);
    return n;
}

int64_t file_read(struct file *f, void *buf, size_t len)
{
    if (vnode_is_stream(f->vn))
        return file_pread(f, buf, len, 0);   /* no position, and no lock a waiting read could hold */
    mutex_lock(&f->lock);
    int64_t n = file_pread(f, buf, len, f->pos);
    if (n > 0)
        f->pos += (uint64_t)n;
    mutex_unlock(&f->lock);
    return n;
}

int64_t file_write(struct file *f, const void *buf, size_t len)
{
    if (vnode_is_stream(f->vn))
        return file_pwrite(f, buf, len, 0);
    file_throttle(f);
    mutex_lock(&f->lock);
    uint64_t off = f->pos;
    if (file_flags(f) & COSMO_O_APPEND)
        off = f->vn->size;
    int64_t n = file_pwrite_held(f, buf, len, off);
    if (n > 0)
        f->pos = off + (uint64_t)n;
    mutex_unlock(&f->lock);
    return n;
}

int64_t file_seek(struct file *f, int64_t off, int whence)
{
    if (vnode_is_stream(f->vn))
        return -ESPIPE;
    mutex_lock(&f->lock);
    int64_t base;
    switch (whence) {
    case COSMO_SEEK_SET: base = 0; break;
    case COSMO_SEEK_CUR: base = (int64_t)f->pos; break;
    case COSMO_SEEK_END: base = (int64_t)f->vn->size; break;
    default:
        mutex_unlock(&f->lock);
        return -EINVAL;
    }
    if (off > INT64_MAX - base || base + off < 0) {
        mutex_unlock(&f->lock);
        return -EINVAL;
    }
    f->pos = (uint64_t)(base + off);
    int64_t pos = (int64_t)f->pos;
    mutex_unlock(&f->lock);
    return pos;
}

int file_stat(struct file *f, struct cosmo_stat *st)
{
    mutex_lock(&f->vn->lock);
    vnode_stat(f->vn, st);
    mutex_unlock(&f->vn->lock);
    return 0;
}

/* f->lock held. A write-back failure recorded since this file last
 * heard -- this attempt's own (just recorded by pagecache_sync) or a
 * neighbour's -- is reported once: the file advances to the current
 * sequence as it reports, so the next successful call returns 0. */
static int report_once(struct file *f, int rc)
{
    int err;
    uint32_t now;
    bool newer = pagecache_error_since(&f->vn->pc, f->wb_seq_seen, &err, &now);
    f->wb_seq_seen = now;
    if (rc == 0 && newer)
        rc = err;
    return rc;
}

int file_sync(struct file *f)
{
    struct vnode *vn = f->vn;
    int rc = 0;
    mutex_lock(&f->lock);      /* before vn->lock: the order every file operation uses */
    mutex_lock(&vn->lock);
    if (vn->type == VNODE_REG)
        rc = pagecache_sync(vn);
    if (rc == 0 && vn->ops->sync)
        rc = vn->ops->sync(vn);
    mutex_unlock(&vn->lock);
    if (vn->type == VNODE_REG)
        rc = report_once(f, rc);
    mutex_unlock(&f->lock);
    return rc;
}

int file_flush(struct file *f)
{
    struct vnode *vn = f->vn;
    if (vn->type != VNODE_REG)
        return 0;
    int rc = 0;
    mutex_lock(&f->lock);
    mutex_lock(&vn->lock);
    if (vn->pc.nr_dirty && vn->ops->writepage)
        rc = pagecache_sync(vn);
    mutex_unlock(&vn->lock);
    rc = report_once(f, rc);
    mutex_unlock(&f->lock);
    return rc;
}

struct readdir_ctx {
    uint8_t *buf;
    size_t len;
    size_t used;
    unsigned emitted;
    bool too_small;   /* the first entry did not fit */
};

static int readdir_emit(void *arg, const char *name, size_t len, uint64_t ino, enum vnode_type type)
{
    struct readdir_ctx *c = arg;
    size_t reclen = ALIGN_UP(sizeof(struct cosmo_dirent) + len + 1, 8);
    if (c->used + reclen > c->len) {
        if (c->emitted == 0)
            c->too_small = true;
        return 1;   /* buffer full: stop, position stays before this entry */
    }
    struct cosmo_dirent *d = (struct cosmo_dirent *)(c->buf + c->used);
    memset(d, 0, reclen);
    d->ino = ino;
    d->reclen = (uint16_t)reclen;
    d->type = (uint8_t)type;
    d->namelen = (uint8_t)len;
    memcpy(d->name, name, len);
    c->used += reclen;
    c->emitted++;
    return 0;
}

int64_t file_readdir(struct file *f, void *buf, size_t len)
{
    struct vnode *vn = f->vn;
    if (vn->type != VNODE_DIR)
        return -ENOTDIR;
    if (vn->ops->readdir == NULL)
        return -ENOTSUP;
    if (len < sizeof(struct cosmo_dirent) + 2)
        return -EINVAL;
    struct readdir_ctx c = { .buf = buf, .len = len };
    mutex_lock(&f->lock);
    mutex_lock(&vn->lock);
    uint64_t pos = f->pos;
    int rc = vn->ops->readdir(vn, &pos, readdir_emit, &c);
    mutex_unlock(&vn->lock);
    if (rc == 0)
        f->pos = pos;
    mutex_unlock(&f->lock);
    if (rc)
        return rc;
    if (c.used == 0 && c.too_small)
        return -EINVAL;   /* the next entry needs a larger buffer */
    return (int64_t)c.used;
}

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
    struct walk w = { 0 };
    int rc = walk_parent(start, path, &w, namebuf, parent, len, NULL);
    walk_fini(&w);
    if (rc)
        return rc;
    if (*len == 0 || dot_name(namebuf, *len)) {
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
    if (rc == -EINVAL && len > 0 && dot_name(last, len))
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
    int rc = text_busy_locked(vn) ? -ETXTBSY
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
        rc = text_busy_locked(vn) ? -ETXTBSY
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
    struct walk w = { 0 };
    rc = walk_parent(nstart, newpath, &w, last, &parent, &len, NULL);
    walk_fini(&w);
    if (rc) {
        vnode_put(target);
        return rc;
    }
    if (len == 0 || dot_name(last, len))
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
