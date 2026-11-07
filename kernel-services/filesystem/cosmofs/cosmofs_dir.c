/*
 * cosmofs_dir.c - A directory's vnode operations: lookup (and the
 * .snapshots directory's), create, symlink, readlink, mkdir, link,
 * unlink, rmdir (a snapshot's, through .snapshots), rename and readdir.
 * Moved out of cosmofs.c (roadmap 2.0 SA7) so that it can be held to a
 * digest and made CeeSafe on its own.
 */

#include <kernel/cosmofs.h>
#include <kernel/cred.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/string.h>
#include <kernel/vfs.h>

#include "cosmofs_internal.h"

int cfs_lookup(struct vnode *dir, const char *name, size_t len, struct vnode **out)
{
    struct cfs *fs = cfs_of(dir->mnt);
    if (fs == NULL || fs->failed)
        return -EIO;   /* unmounted, or the transaction was abandoned */
    mutex_lock(&fs->lock);
    int rc;
    if (cfs_is_snapdir(dir)) {
        /* ".." leaves history the only way out: the live root. */
        if (len == 2 && name[0] == '.' && name[1] == '.') {
            rc = cfs_vnode_get(fs, CFS_ROOT_INO, out);
            mutex_unlock(&fs->lock);
            return rc;
        }
        /* Inside .snapshots: each name is a snapshot's root directory. */
        struct cfs_snapshot s;
        unsigned tag;
        rc = cfs_snap_by_name(fs, name, len, &s, &tag);
        if (rc == 0 && (s.flags & CFS_SNAP_F_CLONE)) {
            struct cfs_clone *c = cfs_clone_by_id(fs, s.id);
            if (c == NULL) {
                rc = -EIO;
            } else {
                cfs_clone_enter(fs, c);
                rc = cfs_vnode_get(fs, CFS_ROOT_INO, out);
                cfs_clone_leave(fs);
            }
        } else if (rc == 0) {
            rc = cfs_vnode_get_tagged(fs, CFS_ROOT_INO, tag, &s, out);
        }
        mutex_unlock(&fs->lock);
        return rc;
    }
    if (dir->ino == CFS_ROOT_INO && len == strlen(CFS_SNAPDIR_NAME) &&
        memcmp(name, CFS_SNAPDIR_NAME, len) == 0) {
        /* Found by name only: readdir does not list it, so nothing
         * walking the tree descends into history by accident. */
        rc = cfs_snapdir_get(fs, out);
        mutex_unlock(&fs->lock);
        return rc;
    }
    unsigned tag = cfs_snap_tag_of(dir);
    struct cfs_clone *clone = tag ? cfs_clone_by_id(fs, tag) : NULL;
    if (clone != NULL) {
        /* Inside a clone: the live tree's lookup, in the clone's tree.
         * At the clone's root ".." is .snapshots, as at a snapshot's. */
        if (len == 2 && name[0] == '.' && name[1] == '.' && CFS_INO_OF(dir->ino) == CFS_ROOT_INO) {
            rc = cfs_snapdir_get(fs, out);
            mutex_unlock(&fs->lock);
            return rc;
        }
        cfs_clone_enter(fs, clone);
        if (len == 2 && name[0] == '.' && name[1] == '.') {
            rc = cfs_vnode_get(fs, cfs_inode_of(dir)->parent, out);
        } else {
            uint8_t *block = kmalloc(CFS_BLOCK, 0);
            rc = block ? 0 : -ENOMEM;
            uint64_t lblk;
            unsigned slot;
            if (rc == 0)
                rc = cfs_dir_find(fs, dir, name, len, block, &lblk, &slot);
            if (rc == 0)
                rc = cfs_vnode_get(fs, ((struct cfs_dirent *)block)[slot].ino, out);
            kfree(block);
        }
        cfs_unlock_tree(fs);
        return rc;
    }
    if (len == 2 && name[0] == '.' && name[1] == '.') {
        if (tag) {
            /* ".." inside a snapshot stays inside it. An untagged parent
             * would be the live tree's directory of that inode number:
             * writable, and holding today's contents under a path that
             * says history. At the snapshot's own root the way up is
             * .snapshots, not the live root. */
            struct cfs_vnode *cv = dir->fs_priv;
            if (CFS_INO_OF(dir->ino) == CFS_ROOT_INO) {
                rc = cfs_snapdir_get(fs, out);
            } else {
                struct cfs_snapshot s = { .imap_root = cv->snap_imap_root, .next_ino = cv->snap_next_ino };
                rc = cfs_vnode_get_tagged(fs, cv->inode.parent, tag, &s, out);
            }
        } else {
            rc = cfs_vnode_get(fs, cfs_inode_of(dir)->parent, out);
        }
    } else if (tag) {
        /* Inside a snapshot: an ordinary directory read, through that
         * snapshot's inode map. */
        uint8_t *block = kmalloc(CFS_BLOCK, 0);
        if (block == NULL) {
            mutex_unlock(&fs->lock);
            return -ENOMEM;
        }
        uint64_t lblk;
        unsigned slot;
        rc = cfs_dir_find(fs, dir, name, len, block, &lblk, &slot);
        if (rc == 0) {
            struct cfs_vnode *cv = dir->fs_priv;
            struct cfs_snapshot s = { .imap_root = cv->snap_imap_root, .next_ino = cv->snap_next_ino };
            rc = cfs_vnode_get_tagged(fs, ((struct cfs_dirent *)block)[slot].ino, tag, &s, out);
        }
        kfree(block);
    } else {
        uint8_t *block = kmalloc(CFS_BLOCK, 0);
        if (block == NULL) {
            mutex_unlock(&fs->lock);
            return -ENOMEM;
        }
        uint64_t lblk;
        unsigned slot;
        rc = cfs_dir_find(fs, dir, name, len, block, &lblk, &slot);
        if (rc == 0)
            rc = cfs_vnode_get(fs, ((struct cfs_dirent *)block)[slot].ino, out);
        kfree(block);
    }
    mutex_unlock(&fs->lock);
    return rc;
}

/* Zero the slot of an inode a failed creation wrote and no entry names,
 * so the number the creation gives back is not an orphan with a link to
 * the checker. Best effort: the slot is at the number the next creation
 * takes, and that creation writes it whole. */
static void inode_slot_clear(struct cfs *fs, uint64_t ino)
{
    struct cfs_inode zero;
    memset(&zero, 0, sizeof(zero));
    (void)cfs_inode_write(fs, ino, &zero);
}

static int cfs_create_common(struct vnode *dir, const char *name, size_t len, uint32_t mode, unsigned type,
                             struct vnode **out)
{
    struct cfs *fs = cfs_of(dir->mnt);
    if (fs == NULL || fs->failed)
        return -EIO;   /* unmounted, or the transaction was abandoned */
    if (len > CFS_NAME_MAX_LONG)
        return -ENAMETOOLONG;
    uint8_t *block = kmalloc(CFS_BLOCK, 0);
    if (block == NULL)
        return -ENOMEM;
    int lrc = cfs_lock_tree_w(fs, dir);
    if (lrc) {
        kfree(block);
        return lrc;
    }
    /* Every inode this changes, made writable before anything changes (CFS-6). */
    lrc = cfs_vnode_prepare(fs, dir);
    if (lrc) {
        cfs_unlock_tree(fs);
        kfree(block);
        return lrc;
    }
    uint64_t lblk;
    unsigned slot;
    int rc = cfs_dir_find(fs, dir, name, len, block, &lblk, &slot);
    if (rc == 0) {
        rc = -EEXIST;
        goto out;
    }
    if (rc != -ENOENT)
        goto out;
    rc = cfs_names_allow(fs, len);
    if (rc)
        goto out;
    uint64_t ino;
    rc = cfs_inode_alloc(fs, &ino);
    if (rc)
        goto out;
    struct cfs_inode in;
    memset(&in, 0, sizeof(in));
    in.mode = CFS_MODE(type, mode);
    in.uid = cred_current()->euid;   /* owned by its creator */
    in.gid = cred_current()->egid;
    in.nlink = type == CFS_TYPE_DIR ? 2 : 1;
    in.ino = ino;
    in.parent = CFS_INO_OF(dir->ino);   /* a clone's vnode carries its tag; the map does not */
    /* An encrypted filesystem authenticates every file it creates: a
     * CRC would say a block is intact, not that it is the one written. */
    in.csum_algo = fs->encrypted ? CFS_CSUM_POLY1305 : CFS_CSUM_CRC32C;
    /* Regular files compress; a directory's blocks are written one at a
     * time and a record cannot be formed from them. Not below format 6,
     * which added records: a version-2..5 pool written here must stay
     * readable by the kernels of its version (CFS-2). */
    in.compress_algo = type == CFS_TYPE_REG && fs->sb.version >= 6 ? CFS_COMPRESS_LZ4 : CFS_COMPRESS_NONE;
    in.mtime_ns = in.ctime_ns = vfs_now_ns();
    rc = cfs_inode_write(fs, ino, &in);
    if (rc == 0) {
        rc = cfs_dir_add(fs, dir, name, len, ino, type, block);
        if (rc)
            inode_slot_clear(fs, ino);
    }
    if (rc) {
        /* No entry names the inode: the number and the count go back
         * (CF17), as they do for a symlink. */
        cfs_inode_discard(fs, ino);
        goto out;
    }
    if (type == CFS_TYPE_DIR) {
        dir->nlink++;
        rc = cfs_vnode_inode_sync(fs, dir);
        if (rc)
            goto out;
    }
    rc = cfs_vnode_get(fs, ino, out);
out:
    cfs_unlock_tree(fs);
    kfree(block);
    return rc;
}

/*
 * A symbolic link's target is its own data block, written inside the
 * transaction that writes the inode and adds the entry -- not through
 * the page cache, which reaches disk at write-back and could commit a
 * zero-length link whose target is not there yet
 * (docs/audit/next-subsystem-symlink.md, "cosmofs"). Every failure after
 * an allocation gives the block and the inode number back, so a refused
 * symlink leaves nothing unreachable behind.
 */
int cfs_symlink(struct vnode *dir, const char *name, size_t len, const char *target,
                struct vnode **out)
{
    struct cfs *fs = cfs_of(dir->mnt);
    if (fs == NULL || fs->failed)
        return -EIO;
    if (len == 0 || len > CFS_NAME_MAX_LONG)
        return -ENAMETOOLONG;
    size_t tlen = strnlen(target, CFS_BLOCK);
    if (tlen == 0 || tlen >= CFS_BLOCK)
        return -ENAMETOOLONG;   /* a target is one block, as on any filesystem that stores it there */
    if (fs->sb.version < 8)
        return -EOPNOTSUPP;     /* an older kernel would read this as a regular file */
    if (cfs_tree_readonly(fs, dir))
        return -EROFS;          /* in .snapshots or a snapshot: what the tree was */
    uint8_t *block = kmalloc(CFS_BLOCK, 0);
    if (block == NULL)
        return -ENOMEM;

    int lrc = cfs_lock_tree_w(fs, dir);
    if (lrc) {
        kfree(block);
        return lrc;
    }
    /* Every inode this changes, made writable before anything changes (CFS-6). */
    lrc = cfs_vnode_prepare(fs, dir);
    if (lrc) {
        cfs_unlock_tree(fs);
        kfree(block);
        return lrc;
    }
    uint64_t lblk;
    unsigned slot;
    uint64_t ino = 0;
    bool ino_taken = false;
    struct cfs_inode in;
    int rc = cfs_dir_find(fs, dir, name, len, block, &lblk, &slot);
    if (rc == 0) {
        rc = -EEXIST;
        goto out;
    }
    if (rc != -ENOENT)
        goto out;
    rc = cfs_names_allow(fs, len);
    if (rc)
        goto out;
    rc = cfs_inode_alloc(fs, &ino);
    if (rc)
        goto out;
    ino_taken = true;
    memset(&in, 0, sizeof(in));
    in.mode = CFS_MODE(CFS_TYPE_LNK, 0777);   /* a link's own mode is never consulted */
    in.uid = cred_current()->euid;
    in.gid = cred_current()->egid;
    in.nlink = 1;
    in.ino = ino;
    in.parent = CFS_INO_OF(dir->ino);   /* a clone's vnode carries its tag; the map does not */
    in.csum_algo = fs->encrypted ? CFS_CSUM_POLY1305 : CFS_CSUM_CRC32C;
    in.compress_algo = CFS_COMPRESS_NONE;   /* one short block: nothing to gain, and readlink stays a read */
    in.size = tlen;
    in.mtime_ns = in.ctime_ns = vfs_now_ns();
    memset(block, 0, CFS_BLOCK);
    memcpy(block, target, tlen);
    rc = cfs_data_write_block(fs, &in, 0, block, CFS_ALLOC_META);
    if (rc) {
        cfs_csum_free(fs, &in);   /* the entry may have been stored before the mapping failed */
        goto out;
    }
    rc = cfs_inode_write(fs, ino, &in);
    if (rc)
        goto undo;
    rc = cfs_dir_add(fs, dir, name, len, ino, CFS_TYPE_LNK, block);
    if (rc) {
        inode_slot_clear(fs, ino);
        goto undo;
    }
    /* The entry names the inode from here on, so the number is not this
     * call's to give back whatever happens next: rolling it back would
     * hand the next creation an inode a directory already points at. */
    ino_taken = false;
    rc = cfs_vnode_get(fs, ino, out);
    goto out;
undo:
    /* The block this call allocated, and the inode number, go back: the
     * directory never named either, so nothing else can find them. */
    if (cfs_ext_count(&in.direct[0]) != 0)
        cfs_free_block_deferred(fs, in.direct[0].start);
    cfs_csum_free(fs, &in);
    cfs_inode_discard(fs, ino);
    ino_taken = false;
out:
    if (ino_taken)
        cfs_inode_discard(fs, ino);
    cfs_unlock_tree(fs);
    kfree(block);
    return rc;
}

int cfs_readlink(struct vnode *vn, char *buf, size_t len)
{
    struct cfs *fs = cfs_of(vn->mnt);
    if (fs == NULL || fs->failed)
        return -EIO;
    uint64_t tlen = vn->size;
    if (tlen == 0 || tlen >= CFS_BLOCK)
        return -EIO;
    uint8_t *block = kmalloc(CFS_BLOCK, 0);
    if (block == NULL)
        return -ENOMEM;
    mutex_lock(&fs->lock);
    uint64_t pblk = 0;
    int rc = cfs_map(fs, cfs_inode_of(vn), 0, &pblk);
    if (rc > 0)
        rc = cfs_data_read_verified(fs, cfs_inode_of(vn), 0, pblk, block);
    else if (rc == 0)
        rc = -EIO;   /* a link with no block is not a link */
    mutex_unlock(&fs->lock);
    if (rc == 0) {
        size_t n = tlen < len ? (size_t)tlen : len;
        memcpy(buf, block, n);
        rc = (int)n;
    }
    kfree(block);
    return rc;
}

int cfs_create(struct vnode *dir, const char *name, size_t len, uint32_t mode, struct vnode **out)
{
    if (cfs_tree_readonly(cfs_of(dir->mnt), dir))
        return -EROFS;   /* .snapshots holds snapshots, not files; a snapshot is what the tree was */
    return cfs_create_common(dir, name, len, mode, CFS_TYPE_REG, out);
}

int cfs_mkdir(struct vnode *dir, const char *name, size_t len, uint32_t mode, struct vnode **out)
{
    /* mkdir inside .snapshots takes a snapshot: the surface is what
     * mkdir already is (design.md, "The interface"). */
    if (cfs_is_snapdir(dir)) {
        struct cfs *fs = cfs_of(dir->mnt);
        if (fs == NULL || fs->failed)
            return -EIO;
        if (dir->mnt->flags & MOUNT_RDONLY)
            return -EROFS;
        char buf[CFS_SNAP_NAME_MAX + 1];
        if (len > CFS_SNAP_NAME_MAX)
            return -ENAMETOOLONG;
        memcpy(buf, name, len);
        buf[len] = '\0';
        mutex_lock(&fs->lock);
        int rc = cfs_snapshot_create(fs, buf, false, NULL);
        struct cfs_snapshot s;
        unsigned tag;
        if (rc == 0 && out)
            rc = cfs_snap_by_name(fs, name, len, &s, &tag) == 0 ? cfs_vnode_get_tagged(fs, CFS_ROOT_INO, tag, &s, out)
                                                            : -EIO;
        mutex_unlock(&fs->lock);
        if (rc == 0)
            kinfo("cosmofs: snapshot '%s' taken", buf);
        return rc;
    }
    if (cfs_tree_readonly(cfs_of(dir->mnt), dir))
        return -EROFS;   /* a snapshot is not writable (a clone is) */
    return cfs_create_common(dir, name, len, mode, CFS_TYPE_DIR, out);
}

/*
 * A second name for `target` (VFS-4; design.md, "Hard links"): an entry
 * naming its inode and one more link, in one transaction. The checker
 * already counts every entry that names an inode (CF13), and unlink and
 * rename's replace already take one link of several, so this is the only
 * operation hard links add. Within one tree: a snapshot's is read-only,
 * and a name in a clone for a file of the live tree would cross trees.
 */
int cfs_link(struct vnode *dir, const char *name, size_t len, struct vnode *target)
{
    struct cfs *fs = cfs_of(dir->mnt);
    if (fs == NULL || fs->failed)
        return -EIO;   /* unmounted, or the transaction was abandoned */
    if (len == 0 || len > CFS_NAME_MAX_LONG)
        return -ENAMETOOLONG;
    if (cfs_tree_readonly(fs, dir) || cfs_tree_readonly(fs, target))
        return -EROFS;          /* in .snapshots or a snapshot: what the tree was */
    if (cfs_snap_tag_of(dir) != cfs_snap_tag_of(target))
        return -EXDEV;          /* between the live tree and a clone, or two clones */
    if (target->type == VNODE_DIR)
        return -EPERM;
    unsigned type = target->type == VNODE_LNK ? CFS_TYPE_LNK : CFS_TYPE_REG;
    uint8_t *block = kmalloc(CFS_BLOCK, 0);
    if (block == NULL)
        return -ENOMEM;
    int rc = cfs_lock_tree_w(fs, dir);
    if (rc) {
        kfree(block);
        return rc;
    }
    /* Every inode this changes, made writable before anything changes (CFS-6). */
    rc = cfs_vnode_prepare(fs, dir);
    if (rc == 0)
        rc = cfs_vnode_prepare(fs, target);
    uint64_t lblk;
    unsigned slot;
    if (rc == 0) {
        rc = cfs_dir_find(fs, dir, name, len, block, &lblk, &slot);
        if (rc == 0)
            rc = -EEXIST;
        else if (rc == -ENOENT)
            rc = cfs_names_allow(fs, len);
    }
    if (rc == 0)
        rc = cfs_dir_add(fs, dir, name, len, CFS_INO_OF(target->ino), type, block);
    if (rc == 0) {
        target->nlink++;
        target->ctime_ns = vfs_now_ns();
        rc = cfs_vnode_inode_sync(fs, target);
        if (rc) {
            /* The count could not be written: take the name back, or, if
             * even that fails, abandon the transaction (as rename does). */
            target->nlink--;
            int rb = cfs_dir_find(fs, dir, name, len, block, &lblk, &slot);
            if (rb == 0)
                rb = cfs_dir_remove_slot(fs, dir, lblk, slot, block);
            if (rb)
                cfs_fail(fs, rb);
        }
    }
    cfs_unlock_tree(fs);
    kfree(block);
    return rc;
}

static int cfs_unlink_common(struct vnode *dir, const char *name, size_t len, struct vnode *victim, bool rmdir)
{
    struct cfs *fs = cfs_of(dir->mnt);
    if (fs == NULL || fs->failed)
        return -EIO;   /* unmounted, or the transaction was abandoned */
    uint8_t *block = kmalloc(CFS_BLOCK, 0);
    if (block == NULL)
        return -ENOMEM;
    int lrc = cfs_lock_tree_w(fs, dir);
    if (lrc) {
        kfree(block);
        return lrc;
    }
    /* Every inode this changes, made writable before anything changes (CFS-6). */
    lrc = cfs_vnode_prepare(fs, dir);
    if (lrc == 0)
        lrc = cfs_vnode_prepare(fs, victim);
    if (lrc) {
        cfs_unlock_tree(fs);
        kfree(block);
        return lrc;
    }
    uint64_t lblk;
    unsigned slot;
    int rc = cfs_dir_find(fs, dir, name, len, block, &lblk, &slot);
    if (rc)
        goto out;
    if (((struct cfs_dirent *)block)[slot].ino != CFS_INO_OF(victim->ino)) {
        rc = -ENOENT;
        goto out;
    }
    if (rmdir) {
        uint8_t *vb = kmalloc(CFS_BLOCK, 0);
        bool empty = vb && cfs_dir_is_empty(fs, victim, vb);
        kfree(vb);
        if (!empty) {
            rc = vb ? -ENOTEMPTY : -ENOMEM;
            goto out;
        }
    }
    /*
     * Room for the orphan record's entry, taken while the name is still
     * there. Past cfs_dir_remove_slot the removal is in the transaction and
     * "no memory to record it" would mean committing a deletion nothing
     * records, which is this unit's own defect on an allocation failure
     * (docs/audit/next-subsystem-orphan.md).
     */
    if ((rmdir || victim->nlink == 1) && !cfs_orphan_reserve(fs)) {
        rc = -ENOMEM;
        goto out;
    }
    rc = cfs_dir_remove_slot(fs, dir, lblk, slot, block);
    if (rc)
        goto out;
    if (rmdir) {
        victim->nlink = 0;
        dir->nlink--;
        rc = cfs_vnode_inode_sync(fs, dir);
    } else {
        victim->nlink--;
    }
    /*
     * The name is gone from disk and the blocks are not free: something
     * still holds this vnode, or will until the VFS drops the last
     * reference and cfs_evict runs. Record the intention, so that a
     * crash or a forced unmount before that eviction does not lose the
     * inode and its blocks (docs/audit/next-subsystem-orphan.md).
     *
     * A directory reaches this too: an empty one can be removed while a
     * process's working directory references it, which is a referenced
     * vnode (`cwd_locked`).
     */
    if (victim->nlink == 0)
        cfs_orphan_add(fs, victim->ino);
    victim->ctime_ns = vfs_now_ns();
    if (rc == 0)
        rc = cfs_vnode_inode_sync(fs, victim);
out:
    cfs_unlock_tree(fs);
    kfree(block);
    return rc;
}

int cfs_unlink(struct vnode *dir, const char *name, size_t len, struct vnode *victim)
{
    if (cfs_tree_readonly(cfs_of(dir->mnt), dir))
        return -EROFS;
    return cfs_unlink_common(dir, name, len, victim, false);
}

/* A deletion frees the blocks only this snapshot reaches. An open file
 * inside it reads those blocks through extents it already holds, so a
 * deletion under an open handle would hand that reader whatever the
 * allocator gave the blocks next. The snapshot is therefore busy while
 * any of its vnodes is in use, exactly as a mount is (V23).
 *
 * The victim is the snapshot's root directory, which rmdir itself holds
 * one reference on; anything beyond that, on it or on any other vnode of
 * the snapshot, is somebody else's. Nothing new can appear during the
 * check: every path into the snapshot goes through .snapshots, whose
 * lock this call holds, and a walk already inside it holds a reference
 * on the tagged vnode it is standing on. */
struct snap_users {
    unsigned tag;
    const struct vnode *victim;
};

static bool snap_vnode_in_use(const struct vnode *vn, void *arg)
{
    const struct snap_users *u = arg;
    if (CFS_SNAP_TAG(vn->ino) != u->tag)
        return false;
    return kobject_refcount(&vn->obj) > (vn == u->victim ? 1u : 0u);
}

bool cfs_snap_in_use(struct mount *mnt, unsigned tag, const struct vnode *victim)
{
    struct snap_users u = { .tag = tag, .victim = victim };
    return vnode_cache_any(mnt, snap_vnode_in_use, &u);
}

int cfs_rmdir(struct vnode *dir, const char *name, size_t len, struct vnode *victim)
{
    if (cfs_is_snapdir(dir)) {
        struct cfs *fs = cfs_of(dir->mnt);
        if (fs == NULL || fs->failed)
            return -EIO;
        if (dir->mnt->flags & MOUNT_RDONLY)
            return -EROFS;
        char buf[CFS_SNAP_NAME_MAX + 1];
        if (len > CFS_SNAP_NAME_MAX)
            return -ENAMETOOLONG;
        memcpy(buf, name, len);
        buf[len] = '\0';
        mutex_lock(&fs->lock);
        struct cfs_snapshot snap;
        unsigned tag;
        int rc = cfs_snap_by_name(fs, name, len, &snap, &tag);
        if (rc == 0 && cfs_snap_in_use(dir->mnt, tag, victim))
            rc = -EBUSY;
        if (rc == 0)
            rc = cfs_snapshot_delete(fs, buf);
        mutex_unlock(&fs->lock);
        if (rc == 0)
            kinfo("cosmofs: snapshot '%s' deleted", buf);
        return rc;
    }
    if (cfs_tree_readonly(cfs_of(dir->mnt), dir))
        return -EROFS;
    return cfs_unlink_common(dir, name, len, victim, true);
}

int cfs_rename(struct vnode *odir, const char *oname, size_t olen, struct vnode *victim, struct vnode *ndir,
               const char *nname, size_t nlen, struct vnode *replaced)
{
    struct cfs *fs = cfs_of(odir->mnt);
    if (fs == NULL || fs->failed)
        return -EIO;   /* unmounted, or the transaction was abandoned */
    if (nlen > CFS_NAME_MAX_LONG)
        return -ENAMETOOLONG;
    if (cfs_tree_readonly(fs, odir) || cfs_tree_readonly(fs, ndir))
        return -EROFS;          /* in .snapshots or a snapshot: what the tree was */
    if (cfs_snap_tag_of(odir) != cfs_snap_tag_of(ndir))
        return -EXDEV;          /* between the live tree and a clone, or two clones: two trees */
    uint8_t *block = kmalloc(CFS_BLOCK, 0);
    if (block == NULL)
        return -ENOMEM;
    int lrc = cfs_lock_tree_w(fs, odir);
    if (lrc) {
        kfree(block);
        return lrc;
    }
    /* Every inode this changes, made writable before anything changes (CFS-6). */
    lrc = cfs_vnode_prepare(fs, odir);
    if (lrc == 0)
        lrc = cfs_vnode_prepare(fs, ndir);
    if (lrc == 0)
        lrc = cfs_vnode_prepare(fs, victim);
    if (lrc == 0 && replaced != NULL)
        lrc = cfs_vnode_prepare(fs, replaced);
    if (lrc) {
        cfs_unlock_tree(fs);
        kfree(block);
        return lrc;
    }
    uint64_t lblk;
    unsigned slot;
    int rc = cfs_dir_find(fs, odir, oname, olen, block, &lblk, &slot);
    if (rc)
        goto out;
    if (((struct cfs_dirent *)block)[slot].ino != CFS_INO_OF(victim->ino)) {
        rc = -ENOENT;
        goto out;
    }
    unsigned type = ((struct cfs_dirent *)block)[slot].type;

    /* Order for failure safety: publish the destination first (the only
     * step that can hit ENOSPC in a fresh block), then retire the source;
     * a failure before the source is removed leaves the namespace as it
     * was, and a failure after it is rolled back by removing the new
     * entry again. */
    if (replaced) {
        if (replaced->type == VNODE_DIR) {
            uint8_t *vb = kmalloc(CFS_BLOCK, 0);
            bool empty = vb && cfs_dir_is_empty(fs, replaced, vb);
            kfree(vb);
            if (!empty) {
                rc = vb ? -ENOTEMPTY : -ENOMEM;
                goto out;
            }
        }
        /* Room for the record's entry, while the namespace is still
         * untouched: the replaced file loses its last name below, and a
         * rename that cannot record that must not happen at all. */
        if ((replaced->type == VNODE_DIR || replaced->nlink == 1) && !cfs_orphan_reserve(fs)) {
            rc = -ENOMEM;
            goto out;
        }
        /* Overwrite the existing entry in place: same name, new inode. */
        uint64_t rl;
        unsigned rs;
        rc = cfs_dir_find(fs, ndir, nname, nlen, block, &rl, &rs);
        if (rc)
            goto out;
        struct cfs_dirent *rd = &((struct cfs_dirent *)block)[rs];
        rd->ino = CFS_INO_OF(victim->ino);
        rd->type = (uint8_t)type;
        rc = cfs_dir_write_block(fs, ndir, rl, block);
        if (rc)
            goto out;
    } else {
        rc = cfs_names_allow(fs, nlen);
        if (rc == 0)
            rc = cfs_dir_add(fs, ndir, nname, nlen, CFS_INO_OF(victim->ino), type, block);
        if (rc)
            goto out;
    }
    /* The source block may have been the one just rewritten (same
     * directory): locate the source entry again before removing it. */
    rc = cfs_dir_find(fs, odir, oname, olen, block, &lblk, &slot);
    if (rc == 0)
        rc = cfs_dir_remove_slot(fs, odir, lblk, slot, block);
    if (rc) {
        /* Roll the destination back so the source stays the only entry. */
        uint64_t rl;
        unsigned rs;
        int rb = cfs_dir_find(fs, ndir, nname, nlen, block, &rl, &rs);
        if (rb == 0) {
            if (replaced) {
                struct cfs_dirent *rd = &((struct cfs_dirent *)block)[rs];
                rd->ino = CFS_INO_OF(replaced->ino);
                rd->type = (uint8_t)(replaced->type == VNODE_DIR ? CFS_TYPE_DIR : CFS_TYPE_REG);
                rb = cfs_dir_write_block(fs, ndir, rl, block);
            } else {
                rb = cfs_dir_remove_slot(fs, ndir, rl, rs, block);
            }
        }
        if (rb) {
            /* Neither state can be reached: drop the whole transaction. */
            cfs_fail(fs, rb);
        } else if (!fs->failed) {   /* a failed inode write abandoned it already (cfs_vnode_inode_sync) */
            kerror("cosmofs: rename of %.*s failed (%d); namespace restored", (int)olen, oname, rc);
        }
        goto out;
    }
    /* The namespace change is published. The metadata updates below are
     * write-throughs into copy-on-write buffers; if one of them fails the
     * in-memory state can no longer be made consistent, so the whole open
     * transaction is abandoned: nothing of it reaches the disk and the
     * last committed root remains the truth. */
    if (replaced) {
        replaced->nlink = replaced->type == VNODE_DIR ? 0 : replaced->nlink - 1;
        /* The same record, for the half of this that is easiest to
         * miss: a file replaced by a rename while it is open. */
        if (replaced->nlink == 0)
            cfs_orphan_add(fs, replaced->ino);
        if (replaced->type == VNODE_DIR)
            ndir->nlink--;
        rc = cfs_vnode_inode_sync(fs, replaced);
    }
    if (rc == 0 && odir != ndir) {
        cfs_inode_of(victim)->parent = CFS_INO_OF(ndir->ino);
        if (victim->type == VNODE_DIR) {
            odir->nlink--;
            ndir->nlink++;
        }
        rc = cfs_vnode_inode_sync(fs, victim);
        if (rc == 0)
            rc = cfs_vnode_inode_sync(fs, odir);
        if (rc == 0)
            rc = cfs_vnode_inode_sync(fs, ndir);
    }
    if (rc)
        cfs_fail(fs, rc);
out:
    cfs_unlock_tree(fs);
    kfree(block);
    return rc;
}

int cfs_readdir(struct vnode *dir, uint64_t *pos, vfs_dirent_cb cb, void *arg)
{
    struct cfs *fs = cfs_of(dir->mnt);
    if (fs == NULL || fs->failed)
        return -EIO;   /* unmounted, or the transaction was abandoned */
    if (*pos == 0) {
        if (cb(arg, ".", 1, dir->ino, VNODE_DIR))
            return 0;
        *pos = 1;
    }
    if (*pos == 1) {
        if (cb(arg, "..", 2, cfs_inode_of(dir)->parent, VNODE_DIR))
            return 0;
        *pos = 2;
    }
    /* The block, then room to assemble a long name (off the stack). */
    uint8_t *block = kmalloc(CFS_BLOCK + CFS_NAME_MAX_LONG, 0);
    if (block == NULL)
        return -ENOMEM;
    char *name = (char *)block + CFS_BLOCK;
    mutex_lock(&fs->lock);
    int rc = 0;
    uint64_t total = cfs_dir_blocks(dir) * CFS_DIRENTS_PER_BLOCK;
    for (uint64_t i = *pos - 2; i < total; i++) {
        uint64_t lblk = i / CFS_DIRENTS_PER_BLOCK;
        unsigned s = (unsigned)(i % CFS_DIRENTS_PER_BLOCK);
        if (s == 0 || i == *pos - 2) {
            rc = cfs_dir_read_block(fs, dir, lblk, block);
            if (rc)
                break;
        }
        const struct cfs_dirent *d = (const struct cfs_dirent *)block;
        if (d[s].ino == CFS_DIRENT_CONT) {
            *pos = i + 3;   /* the rest of a name: its head was returned, or changed under a resumed walk */
            continue;
        }
        if (d[s].ino) {
            unsigned slots = cfs_dirent_check(d, s, fs->sb.version);
            if (slots == 0) {
                rc = -EIO;   /* an on-disk length or shape must never size a copy */
                break;
            }
            unsigned nlen = cfs_dirent_name(d, s, name);
            enum vnode_type t = d[s].type == CFS_TYPE_DIR ? VNODE_DIR : VNODE_REG;
            if (cb(arg, name, nlen, d[s].ino, t))
                break;
            i += slots - 1;   /* past its continuations, which stay in this block */
        }
        *pos = i + 3;
    }
    mutex_unlock(&fs->lock);
    kfree(block);
    return rc;
}
