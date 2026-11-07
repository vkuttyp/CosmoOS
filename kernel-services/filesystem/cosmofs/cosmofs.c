/*
 * cosmofs.c - Vnode operations: inodes, extents, directories, data.
 *
 * Every mutation writes the inode through to its (copy-on-write) inode
 * block immediately, so the buffer cache always holds the complete
 * pre-commit state and vnode eviction has nothing to flush but pages.
 * Directory blocks and file data are written to freshly allocated
 * blocks; the previous block goes on the deferred free list.
 */

#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/faultinject.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/cred.h>
#include <kernel/crc32c.h>
#include <kernel/page.h>
#include <kernel/panic.h>
#include <kernel/lz4.h>
#include <kernel/string.h>

#include "cosmofs_internal.h"

static const struct vnode_ops cfs_dir_ops;
static const struct vnode_ops cfs_file_ops;
static const struct vnode_ops cfs_lnk_ops;

/* --- vnodes ---------------------------------------------------------------- */

static void fill_vnode(struct vnode *vn, const struct cfs_inode *in)
{
    unsigned type = CFS_MODE_TYPE(in->mode);
    vn->type = type == CFS_TYPE_DIR ? VNODE_DIR : type == CFS_TYPE_LNK ? VNODE_LNK : VNODE_REG;
    vn->mode = in->mode & 07777;
    vn->uid = in->uid;
    vn->gid = in->gid;
    vn->nlink = in->nlink;
    vn->size = in->size;
    vn->mtime_ns = in->mtime_ns;
    vn->ctime_ns = in->ctime_ns;
    vn->ops = vn->type == VNODE_DIR ? &cfs_dir_ops : vn->type == VNODE_LNK ? &cfs_lnk_ops : &cfs_file_ops;
}

/* Referenced vnode for `ino` in a snapshot's tree (tag != 0) or the live
 * one. fs->lock held. */
static int cfs_vnode_get_tagged(struct cfs *fs, uint64_t ino, unsigned tag, const struct cfs_snapshot *snap,
                                struct vnode **out)
{
    uint64_t key = CFS_SNAP_INO(tag, ino);
    struct vnode *vn = vnode_lookup_cached(fs->mnt, key);
    if (vn) {
        *out = vn;
        return 0;
    }
    struct cfs_vnode *cv = kzalloc(sizeof(*cv));
    if (cv == NULL)
        return -ENOMEM;
    int rc = tag ? cfs_inode_read_at(fs, snap->imap_root, snap->next_ino, ino, &cv->inode)
                 : cfs_inode_read(fs, ino, &cv->inode);
    if (rc) {
        kfree(cv);
        return rc;
    }
    if (tag) {
        cv->snap_tag = tag;
        cv->snap_imap_root = snap->imap_root;
        cv->snap_next_ino = snap->next_ino;
    }
    vn = vnode_alloc(fs->mnt, key);
    if (vn == NULL) {
        kfree(cv);
        return -ENOMEM;
    }
    vn->fs_priv = cv;
    fill_vnode(vn, &cv->inode);
    vnode_hash_insert(vn);
    *out = vn;
    return 0;
}

int cfs_vnode_get(struct cfs *fs, uint64_t ino, struct vnode **out)
{
    /* Inside a clone the vnode is the clone's: its inode numbers carry
     * the clone's tag, as a snapshot's do (design.md, "Clones"). */
    ino = CFS_INO_OF(ino);
    unsigned ctag = fs->cur_clone ? (unsigned)fs->cur_clone->id : 0;
    if (ctag) {
        uint64_t key = CFS_SNAP_INO(ctag, ino);
        struct vnode *cvn = vnode_lookup_cached(fs->mnt, key);
        if (cvn) {
            *out = cvn;
            return 0;
        }
        struct cfs_vnode *cv = kzalloc(sizeof(*cv));
        if (cv == NULL)
            return -ENOMEM;
        int rc = cfs_inode_read(fs, ino, &cv->inode);
        if (rc) {
            kfree(cv);
            return rc;
        }
        cv->snap_tag = ctag;
        cvn = vnode_alloc(fs->mnt, key);
        if (cvn == NULL) {
            kfree(cv);
            return -ENOMEM;
        }
        cvn->fs_priv = cv;
        fill_vnode(cvn, &cv->inode);
        vnode_hash_insert(cvn);
        *out = cvn;
        return 0;
    }
    struct vnode *vn = vnode_lookup_cached(fs->mnt, ino);
    if (vn) {
        *out = vn;
        return 0;
    }
    struct cfs_vnode *cv = kzalloc(sizeof(*cv));
    if (cv == NULL)
        return -ENOMEM;
    int rc = cfs_inode_read(fs, ino, &cv->inode);
    if (rc) {
        kfree(cv);
        return rc;
    }
    vn = vnode_alloc(fs->mnt, ino);
    if (vn == NULL) {
        kfree(cv);
        return -ENOMEM;
    }
    vn->fs_priv = cv;
    fill_vnode(vn, &cv->inode);
    vnode_hash_insert(vn);
    *out = vn;
    return 0;
}

/* Push the vnode's public fields into its inode and write it through.
 * Every caller has already changed what the inode describes -- a block
 * mapped and its predecessor queued free, an entry added or removed, a
 * link count -- which is why each prepared the inode first (CFS-6). */
static int inode_sync(struct cfs *fs, struct vnode *vn)
{
    struct cfs_inode *in = cfs_inode_of(vn);
    in->nlink = vn->nlink;
    in->size = vn->size;
    in->mtime_ns = vn->mtime_ns;
    in->ctime_ns = vn->ctime_ns;
    in->mode = CFS_MODE(vn->type == VNODE_DIR ? CFS_TYPE_DIR : vn->type == VNODE_LNK ? CFS_TYPE_LNK : CFS_TYPE_REG,
                        vn->type == VNODE_LNK ? 0777u : vn->mode);
    in->uid = vn->uid;
    in->gid = vn->gid;
    /* Not expected to fail: every caller made the inode's map path
     * writable before its first change (inode_prepare), so this writes
     * into dirty buffers. Anything else is a defect, and the transaction
     * goes rather than publish what the caller half did. */
    int rc = cfs_inode_write(fs, vn->ino, in);
    if (rc)
        cfs_fail(fs, rc);
    return rc;
}

/*
 * Before an operation's first change, the inode the operation ends by
 * storing (cfs_inode_prepare, CFS-6): an allocation that fails here is an
 * ordinary error the caller can retry, where in inode_sync it abandoned
 * the transaction.
 */
static int inode_prepare(struct cfs *fs, struct vnode *vn)
{
    /* The test seam (cosmofs-nomem): the inode's map path cannot be had. */
    if (faultinject_should_fail(FI_COSMOFS_INODE_NOMEM))
        return -ENOMEM;
    return cfs_inode_prepare(fs, vn->ino);
}

/* --- directories ---------------------------------------------------------------- */

static int dir_read_block(struct cfs *fs, struct vnode *dir, uint64_t lblk, uint8_t *buf)
{
    return cfs_dir_read_block_at(fs, cfs_inode_of(dir), lblk, buf);
}

/* Write directory block `lblk` copy-on-write and update the inode. */
static int dir_write_block(struct cfs *fs, struct vnode *dir, uint64_t lblk, const uint8_t *buf)
{
    int rc = cfs_data_write_block(fs, cfs_inode_of(dir), lblk, buf, CFS_ALLOC_META);
    if (rc)
        return rc;
    if ((lblk + 1) * CFS_BLOCK > dir->size)
        dir->size = (lblk + 1) * CFS_BLOCK;
    dir->mtime_ns = vfs_now_ns();
    return inode_sync(fs, dir);
}

static uint64_t dir_blocks(const struct vnode *dir)
{
    return (dir->size + CFS_BLOCK - 1) / CFS_BLOCK;
}

/* Find `name`; returns the block index and slot, or -ENOENT. */
static int dir_find(struct cfs *fs, struct vnode *dir, const char *name, size_t len, uint8_t *block,
                    uint64_t *lblk_out, unsigned *slot_out)
{
    if (len > CFS_NAME_MAX_LONG)
        return -ENAMETOOLONG;
    if (len > CFS_NAME_MAX && fs->sb.version < 15)
        return -ENOENT;   /* no entry of a pool below 15 has a name this long */
    for (uint64_t lblk = 0; lblk < dir_blocks(dir); lblk++) {
        int rc = dir_read_block(fs, dir, lblk, block);
        if (rc)
            return rc;
        const struct cfs_dirent *d = (const struct cfs_dirent *)block;
        for (unsigned s = 0; s < CFS_DIRENTS_PER_BLOCK; s++) {
            unsigned slots = cfs_dirent_check(d, s, fs->sb.version);
            if (slots == 0)
                continue;
            if (cfs_dirent_name_eq(d, s, name, (unsigned)len)) {
                *lblk_out = lblk;
                *slot_out = s;
                return 0;
            }
            s += slots - 1;
        }
    }
    return -ENOENT;
}

/* A long name's slots are one run in one block (design.md, "Format
 * version 15"); the caller has had names_allow say yes to `len`. */
static int dir_add(struct cfs *fs, struct vnode *dir, const char *name, size_t len, uint64_t ino, unsigned type,
                   uint8_t *block)
{
    unsigned max = fs->sb.version >= 15 ? CFS_NAME_MAX_LONG : CFS_NAME_MAX;
    if (len == 0 || len > max)
        return -ENAMETOOLONG;
    unsigned need = 1u + cfs_dirent_extra((unsigned)len);
    uint64_t nb = dir_blocks(dir);
    for (uint64_t lblk = 0; lblk <= nb; lblk++) {
        int rc;
        if (lblk == nb)
            memset(block, 0, CFS_BLOCK);
        else if ((rc = dir_read_block(fs, dir, lblk, block)) != 0)
            return rc;
        struct cfs_dirent *d = (struct cfs_dirent *)block;
        int s = cfs_dirent_free_run(d, need);
        if (s >= 0) {
            /* An entry names an inode of its own tree, untagged. */
            cfs_dirent_put(d, (unsigned)s, CFS_INO_OF(ino), type, name, (unsigned)len);
            return dir_write_block(fs, dir, lblk, block);
        }
    }
    return -ENOSPC;
}

/* The entry at `slot`, found by dir_find: its head and continuations. */
static int dir_remove_slot(struct cfs *fs, struct vnode *dir, uint64_t lblk, unsigned slot, uint8_t *block)
{
    cfs_dirent_clear((struct cfs_dirent *)block, slot);
    return dir_write_block(fs, dir, lblk, block);
}

static bool dir_is_empty(struct cfs *fs, struct vnode *dir, uint8_t *block)
{
    for (uint64_t lblk = 0; lblk < dir_blocks(dir); lblk++) {
        if (dir_read_block(fs, dir, lblk, block))
            return false;
        const struct cfs_dirent *d = (const struct cfs_dirent *)block;
        for (unsigned s = 0; s < CFS_DIRENTS_PER_BLOCK; s++) {
            if (d[s].ino)
                return false;
        }
    }
    return true;
}

/* The synthetic .snapshots directory: an inode number no real inode can
 * have, and just enough of an inode to be a directory. */
static int snapdir_get(struct cfs *fs, struct vnode **out)
{
    struct vnode *vn = vnode_lookup_cached(fs->mnt, CFS_SNAPDIR_INO);
    if (vn) {
        *out = vn;
        return 0;
    }
    struct cfs_vnode *cv = kzalloc(sizeof(*cv));
    if (cv == NULL)
        return -ENOMEM;
    cv->inode.mode = CFS_MODE(CFS_TYPE_DIR, 0555);
    cv->inode.nlink = 1;
    cv->inode.ino = CFS_SNAPDIR_INO;
    cv->inode.parent = CFS_ROOT_INO;
    cv->snap_tag = 0xFFFFu;   /* not a snapshot's tree, but not the live one either */
    vn = vnode_alloc(fs->mnt, CFS_SNAPDIR_INO);
    if (vn == NULL) {
        kfree(cv);
        return -ENOMEM;
    }
    vn->fs_priv = cv;
    fill_vnode(vn, &cv->inode);
    vnode_hash_insert(vn);
    *out = vn;
    return 0;
}

/* The tag a snapshot's vnodes carry is its id, which is never reused:
 * a positional index would let a cached vnode be handed to a different
 * snapshot after a deletion compacted the list. */
int cfs_snap_by_name(struct cfs *fs, const char *name, size_t len, struct cfs_snapshot *out, unsigned *tag)
{
    char buf[CFS_SNAP_NAME_MAX + 1];
    if (len > CFS_SNAP_NAME_MAX)
        return -ENOENT;
    memcpy(buf, name, len);
    buf[len] = '\0';
    if (!cfs_snapshot_find(fs, buf, out))   /* walks every list block */
        return -ENOENT;
    if (out->id == 0 || out->id > CFS_SNAP_ID_MAX)
        return -EIO;
    *tag = (unsigned)out->id;
    return 0;
}

static bool is_snapdir(struct vnode *vn)
{
    struct cfs_vnode *cv = vn->fs_priv;
    return cv && cv->snap_tag == 0xFFFFu;
}

static unsigned snap_tag_of(struct vnode *vn)
{
    struct cfs_vnode *cv = vn->fs_priv;
    return cv && cv->snap_tag != 0xFFFFu ? cv->snap_tag : 0;
}

/*
 * The tree an operation on `vn` writes: the live tree, or a clone, which
 * it enters for the length of the operation (cosmofs_clone.cee). A vnode
 * of a snapshot is read-only, and so is one of a clone since deleted.
 */
static bool tree_readonly(struct cfs *fs, struct vnode *vn)
{
    if (is_snapdir(vn))
        return true;
    unsigned tag = snap_tag_of(vn);
    if (tag == 0)
        return false;
    mutex_lock(&fs->lock);
    bool clone = cfs_clone_by_id(fs, tag) != NULL;
    mutex_unlock(&fs->lock);
    return !clone;
}

/*
 * The tree an operation that writes on `vn` takes, under the one hold
 * that enters the clone; -EROFS (and the lock not held) for a vnode whose
 * tree is not writable -- .snapshots, a snapshot, or a clone deleted since the vnode
 * was found. Deciding it here and not before taking the lock is what
 * keeps a write racing a clone's deletion off the live tree, whose inode
 * numbers a clone's share.
 */
static int cfs_lock_tree_w(struct cfs *fs, struct vnode *vn)
{
    if (is_snapdir(vn))
        return -EROFS;
    mutex_lock(&fs->lock);
    unsigned tag = snap_tag_of(vn);
    if (tag == 0)
        return 0;
    struct cfs_clone *c = cfs_clone_by_id(fs, tag);
    if (c == NULL) {
        mutex_unlock(&fs->lock);
        return -EROFS;
    }
    cfs_clone_enter(fs, c);
    return 0;
}

static void cfs_unlock_tree(struct cfs *fs)
{
    cfs_clone_leave(fs);
    mutex_unlock(&fs->lock);
}

static int cfs_lookup(struct vnode *dir, const char *name, size_t len, struct vnode **out)
{
    struct cfs *fs = cfs_of(dir->mnt);
    if (fs == NULL || fs->failed)
        return -EIO;   /* unmounted, or the transaction was abandoned */
    mutex_lock(&fs->lock);
    int rc;
    if (is_snapdir(dir)) {
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
        rc = snapdir_get(fs, out);
        mutex_unlock(&fs->lock);
        return rc;
    }
    unsigned tag = snap_tag_of(dir);
    struct cfs_clone *clone = tag ? cfs_clone_by_id(fs, tag) : NULL;
    if (clone != NULL) {
        /* Inside a clone: the live tree's lookup, in the clone's tree.
         * At the clone's root ".." is .snapshots, as at a snapshot's. */
        if (len == 2 && name[0] == '.' && name[1] == '.' && CFS_INO_OF(dir->ino) == CFS_ROOT_INO) {
            rc = snapdir_get(fs, out);
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
                rc = dir_find(fs, dir, name, len, block, &lblk, &slot);
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
                rc = snapdir_get(fs, out);
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
        rc = dir_find(fs, dir, name, len, block, &lblk, &slot);
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
        rc = dir_find(fs, dir, name, len, block, &lblk, &slot);
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
    lrc = inode_prepare(fs, dir);
    if (lrc) {
        cfs_unlock_tree(fs);
        kfree(block);
        return lrc;
    }
    uint64_t lblk;
    unsigned slot;
    int rc = dir_find(fs, dir, name, len, block, &lblk, &slot);
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
        rc = dir_add(fs, dir, name, len, ino, type, block);
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
        rc = inode_sync(fs, dir);
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
static int cfs_symlink(struct vnode *dir, const char *name, size_t len, const char *target,
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
    if (tree_readonly(fs, dir))
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
    lrc = inode_prepare(fs, dir);
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
    int rc = dir_find(fs, dir, name, len, block, &lblk, &slot);
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
    rc = dir_add(fs, dir, name, len, ino, CFS_TYPE_LNK, block);
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

static int cfs_readlink(struct vnode *vn, char *buf, size_t len)
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

static int cfs_create(struct vnode *dir, const char *name, size_t len, uint32_t mode, struct vnode **out)
{
    if (tree_readonly(cfs_of(dir->mnt), dir))
        return -EROFS;   /* .snapshots holds snapshots, not files; a snapshot is what the tree was */
    return cfs_create_common(dir, name, len, mode, CFS_TYPE_REG, out);
}

static int cfs_mkdir(struct vnode *dir, const char *name, size_t len, uint32_t mode, struct vnode **out)
{
    /* mkdir inside .snapshots takes a snapshot: the surface is what
     * mkdir already is (design.md, "The interface"). */
    if (is_snapdir(dir)) {
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
    if (tree_readonly(cfs_of(dir->mnt), dir))
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
static int cfs_link(struct vnode *dir, const char *name, size_t len, struct vnode *target)
{
    struct cfs *fs = cfs_of(dir->mnt);
    if (fs == NULL || fs->failed)
        return -EIO;   /* unmounted, or the transaction was abandoned */
    if (len == 0 || len > CFS_NAME_MAX_LONG)
        return -ENAMETOOLONG;
    if (tree_readonly(fs, dir) || tree_readonly(fs, target))
        return -EROFS;          /* in .snapshots or a snapshot: what the tree was */
    if (snap_tag_of(dir) != snap_tag_of(target))
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
    rc = inode_prepare(fs, dir);
    if (rc == 0)
        rc = inode_prepare(fs, target);
    uint64_t lblk;
    unsigned slot;
    if (rc == 0) {
        rc = dir_find(fs, dir, name, len, block, &lblk, &slot);
        if (rc == 0)
            rc = -EEXIST;
        else if (rc == -ENOENT)
            rc = cfs_names_allow(fs, len);
    }
    if (rc == 0)
        rc = dir_add(fs, dir, name, len, CFS_INO_OF(target->ino), type, block);
    if (rc == 0) {
        target->nlink++;
        target->ctime_ns = vfs_now_ns();
        rc = inode_sync(fs, target);
        if (rc) {
            /* The count could not be written: take the name back, or, if
             * even that fails, abandon the transaction (as rename does). */
            target->nlink--;
            int rb = dir_find(fs, dir, name, len, block, &lblk, &slot);
            if (rb == 0)
                rb = dir_remove_slot(fs, dir, lblk, slot, block);
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
    lrc = inode_prepare(fs, dir);
    if (lrc == 0)
        lrc = inode_prepare(fs, victim);
    if (lrc) {
        cfs_unlock_tree(fs);
        kfree(block);
        return lrc;
    }
    uint64_t lblk;
    unsigned slot;
    int rc = dir_find(fs, dir, name, len, block, &lblk, &slot);
    if (rc)
        goto out;
    if (((struct cfs_dirent *)block)[slot].ino != CFS_INO_OF(victim->ino)) {
        rc = -ENOENT;
        goto out;
    }
    if (rmdir) {
        uint8_t *vb = kmalloc(CFS_BLOCK, 0);
        bool empty = vb && dir_is_empty(fs, victim, vb);
        kfree(vb);
        if (!empty) {
            rc = vb ? -ENOTEMPTY : -ENOMEM;
            goto out;
        }
    }
    /*
     * Room for the orphan record's entry, taken while the name is still
     * there. Past dir_remove_slot the removal is in the transaction and
     * "no memory to record it" would mean committing a deletion nothing
     * records, which is this unit's own defect on an allocation failure
     * (docs/audit/next-subsystem-orphan.md).
     */
    if ((rmdir || victim->nlink == 1) && !cfs_orphan_reserve(fs)) {
        rc = -ENOMEM;
        goto out;
    }
    rc = dir_remove_slot(fs, dir, lblk, slot, block);
    if (rc)
        goto out;
    if (rmdir) {
        victim->nlink = 0;
        dir->nlink--;
        rc = inode_sync(fs, dir);
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
        rc = inode_sync(fs, victim);
out:
    cfs_unlock_tree(fs);
    kfree(block);
    return rc;
}

static int cfs_unlink(struct vnode *dir, const char *name, size_t len, struct vnode *victim)
{
    if (tree_readonly(cfs_of(dir->mnt), dir))
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

static int cfs_rmdir(struct vnode *dir, const char *name, size_t len, struct vnode *victim)
{
    if (is_snapdir(dir)) {
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
    if (tree_readonly(cfs_of(dir->mnt), dir))
        return -EROFS;
    return cfs_unlink_common(dir, name, len, victim, true);
}

static int cfs_rename(struct vnode *odir, const char *oname, size_t olen, struct vnode *victim, struct vnode *ndir,
                      const char *nname, size_t nlen, struct vnode *replaced)
{
    struct cfs *fs = cfs_of(odir->mnt);
    if (fs == NULL || fs->failed)
        return -EIO;   /* unmounted, or the transaction was abandoned */
    if (nlen > CFS_NAME_MAX_LONG)
        return -ENAMETOOLONG;
    if (tree_readonly(fs, odir) || tree_readonly(fs, ndir))
        return -EROFS;          /* in .snapshots or a snapshot: what the tree was */
    if (snap_tag_of(odir) != snap_tag_of(ndir))
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
    lrc = inode_prepare(fs, odir);
    if (lrc == 0)
        lrc = inode_prepare(fs, ndir);
    if (lrc == 0)
        lrc = inode_prepare(fs, victim);
    if (lrc == 0 && replaced != NULL)
        lrc = inode_prepare(fs, replaced);
    if (lrc) {
        cfs_unlock_tree(fs);
        kfree(block);
        return lrc;
    }
    uint64_t lblk;
    unsigned slot;
    int rc = dir_find(fs, odir, oname, olen, block, &lblk, &slot);
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
            bool empty = vb && dir_is_empty(fs, replaced, vb);
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
        rc = dir_find(fs, ndir, nname, nlen, block, &rl, &rs);
        if (rc)
            goto out;
        struct cfs_dirent *rd = &((struct cfs_dirent *)block)[rs];
        rd->ino = CFS_INO_OF(victim->ino);
        rd->type = (uint8_t)type;
        rc = dir_write_block(fs, ndir, rl, block);
        if (rc)
            goto out;
    } else {
        rc = cfs_names_allow(fs, nlen);
        if (rc == 0)
            rc = dir_add(fs, ndir, nname, nlen, CFS_INO_OF(victim->ino), type, block);
        if (rc)
            goto out;
    }
    /* The source block may have been the one just rewritten (same
     * directory): locate the source entry again before removing it. */
    rc = dir_find(fs, odir, oname, olen, block, &lblk, &slot);
    if (rc == 0)
        rc = dir_remove_slot(fs, odir, lblk, slot, block);
    if (rc) {
        /* Roll the destination back so the source stays the only entry. */
        uint64_t rl;
        unsigned rs;
        int rb = dir_find(fs, ndir, nname, nlen, block, &rl, &rs);
        if (rb == 0) {
            if (replaced) {
                struct cfs_dirent *rd = &((struct cfs_dirent *)block)[rs];
                rd->ino = CFS_INO_OF(replaced->ino);
                rd->type = (uint8_t)(replaced->type == VNODE_DIR ? CFS_TYPE_DIR : CFS_TYPE_REG);
                rb = dir_write_block(fs, ndir, rl, block);
            } else {
                rb = dir_remove_slot(fs, ndir, rl, rs, block);
            }
        }
        if (rb) {
            /* Neither state can be reached: drop the whole transaction. */
            cfs_fail(fs, rb);
        } else if (!fs->failed) {   /* a failed inode write abandoned it already (inode_sync) */
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
        rc = inode_sync(fs, replaced);
    }
    if (rc == 0 && odir != ndir) {
        cfs_inode_of(victim)->parent = CFS_INO_OF(ndir->ino);
        if (victim->type == VNODE_DIR) {
            odir->nlink--;
            ndir->nlink++;
        }
        rc = inode_sync(fs, victim);
        if (rc == 0)
            rc = inode_sync(fs, odir);
        if (rc == 0)
            rc = inode_sync(fs, ndir);
    }
    if (rc)
        cfs_fail(fs, rc);
out:
    cfs_unlock_tree(fs);
    kfree(block);
    return rc;
}

static int cfs_readdir(struct vnode *dir, uint64_t *pos, vfs_dirent_cb cb, void *arg)
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
    uint64_t total = dir_blocks(dir) * CFS_DIRENTS_PER_BLOCK;
    for (uint64_t i = *pos - 2; i < total; i++) {
        uint64_t lblk = i / CFS_DIRENTS_PER_BLOCK;
        unsigned s = (unsigned)(i % CFS_DIRENTS_PER_BLOCK);
        if (s == 0 || i == *pos - 2) {
            rc = dir_read_block(fs, dir, lblk, block);
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

/* --- regular files ------------------------------------------------------------------ */

/* One logical block, wherever it lives: a hole, a plain block, or one
 * page of a compressed record. */
static int read_logical_block(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, void *buf)
{
    struct cfs_extent e;
    int rc = cfs_map_ext(fs, in, lblk, &e);
    if (rc < 0)
        return rc;
    if (rc == 0) {
        memset(buf, 0, CFS_BLOCK);   /* a hole */
        return 0;
    }
    if (!cfs_ext_compressed(&e))
        return cfs_data_read_verified(fs, in, lblk, e.start + (lblk - e.lblk), buf);
    /* One page of a record costs the whole record: there is no cache of
     * decompressed records (design.md, "What this costs"). */
    uint8_t *rec = kmalloc((size_t)cfs_ext_count(&e) * CFS_BLOCK, 0);
    if (rec == NULL)
        return -ENOMEM;
    rc = cfs_record_read(fs, in, &e, rec);
    if (rc == 0)
        memcpy(buf, rec + (size_t)(lblk - e.lblk) * CFS_BLOCK, CFS_BLOCK);
    kfree(rec);
    return rc;
}

#if CONFIG_DEBUG
/* Test hook (CF16): every block below the file's size, read through the
 * map and checksums its open inode holds -- what the next commit would
 * publish -- and verified. `*bad` counts the blocks that fail. Debug
 * builds only, as the self-test that calls it. */
int cosmofs_test_verify_file(struct vnode *vn, uint64_t *bad)
{
    struct cfs *fs = cfs_of(vn->mnt);
    if (fs == NULL)
        return -EINVAL;
    uint8_t *buf = kmalloc(CFS_BLOCK, 0);
    if (buf == NULL)
        return -ENOMEM;
    *bad = 0;
    mutex_lock(&fs->lock);
    uint64_t nblocks = (vn->size + CFS_BLOCK - 1) / CFS_BLOCK;
    for (uint64_t lblk = 0; lblk < nblocks; lblk++) {
        int rc = read_logical_block(fs, cfs_inode_of(vn), lblk, buf);
        if (rc == -ENOMEM) {
            mutex_unlock(&fs->lock);
            kfree(buf);
            return rc;
        }
        if (rc < 0)
            (*bad)++;
    }
    mutex_unlock(&fs->lock);
    kfree(buf);
    return 0;
}
#endif

static int cfs_readpage(struct vnode *vn, uint64_t index, void *buf)
{
    struct cfs *fs = cfs_of(vn->mnt);
    if (fs == NULL || fs->failed)
        return -EIO;   /* unmounted, or the transaction was abandoned */
    mutex_lock(&fs->lock);
    int rc = read_logical_block(fs, cfs_inode_of(vn), index, buf);
    mutex_unlock(&fs->lock);
    return rc > 0 ? 0 : rc;
}

/*
 * One page. If it falls inside a compressed record the record has to be
 * rebuilt around it: a record is one object, and there is no way to
 * patch a block of it in place. That is the cost of compression, paid
 * by the write that caused it.
 */
static int write_one_page(struct cfs *fs, struct cfs_inode *in, uint64_t index, const void *buf)
{
    struct cfs_extent e;
    int rc = cfs_map_ext(fs, in, index, &e);
    if (rc < 0)
        return rc;
    if (rc == 1 && cfs_ext_compressed(&e)) {
        uint32_t count = cfs_ext_count(&e);
        uint8_t *rec = kmalloc((size_t)count * CFS_BLOCK, 0);
        if (rec == NULL)
            return -ENOMEM;
        rc = cfs_record_read(fs, in, &e, rec);
        if (rc == 0) {
            memcpy(rec + (size_t)(index - e.lblk) * CFS_BLOCK, buf, CFS_BLOCK);
            rc = cfs_record_write(fs, in, e.lblk, rec, count, true);   /* a record replaced by a record */
        }
        kfree(rec);
        return rc;
    }
    /* Holes are representable: only this block is written. */
    return cfs_data_write_block(fs, in, index, buf, CFS_ALLOC_DATA);
}

/*
 * The inode is stored after a write-back whether or not the write
 * succeeded. One that fails partway -- the fifth block of a record finds
 * no room -- has mapped the blocks before it in the open inode, and
 * replaced the ones they displace; only the inode names the new blocks,
 * and the old ones are already queued free. Left unstored, a vnode that
 * goes before the retry (its last write-back failing too, the pages
 * dropped) takes that map with it: the new blocks were nobody's and the
 * old ones were freed under the committed inode that still named them
 * (found by test-pkgtx's full root: 3 to 7 leaked blocks). What was
 * written is the new contents under their own checksums, so it is kept.
 * An abandoned transaction has nothing to store.
 */
static int sync_after_write(struct cfs *fs, struct vnode *vn, int rc)
{
    if (rc != 0 && fs->failed)
        return rc;
    int src = inode_sync(fs, vn);
    return rc ? rc : src;
}

/*
 * The run bound, charged when a page becomes dirty rather than found at
 * its write-back (CFS-32; design.md, "Refused at the write"). A dirty
 * page's write-back is one cfs_set_extent, which adds at most two runs (the
 * run it lands in split around it); a whole record's is one cfs_set_extent
 * for eight pages, so two a page is a bound and not a guess. A truncate
 * adds runs of its own -- a record it cuts is written again as plain
 * blocks, at most seven, and a partial last block once more -- so
 * cfs_truncate writes the dirty pages first when its additions could
 * reach the room they were charged; CFS_TRUNC_ADDS of headroom covers a
 * store that faults a page dirty between that test and the trim.
 */
#define CFS_TRUNC_ADDS CFS_RECORD_BLOCKS

static int inode_runs(struct cfs *fs, struct vnode *vn, unsigned *out)
{
    struct cfs_vnode *cv = vn->fs_priv;
    if (!cv->runs_valid || cv->runs_at != __atomic_load_n(&fs->map_stores, __ATOMIC_ACQUIRE)) {
        int rc = cfs_lock_tree_w(fs, vn);
        if (rc)
            return rc;
        unsigned n;
        rc = cfs_extents_count(fs, cfs_inode_of(vn), &n);
        if (rc == 0) {
            cv->runs = n;
            cv->runs_at = fs->map_stores;
            cv->runs_valid = true;
        }
        cfs_unlock_tree(fs);
        if (rc)
            return rc;
    }
    *out = cv->runs;
    return 0;
}

static bool runs_fit(struct cfs *fs, unsigned runs, unsigned dirty)
{
    return (uint64_t)runs + 2ull * dirty + CFS_TRUNC_ADDS <= cfs_max_extents(fs);
}

/* After a write-back or a truncate under fs->lock: when the last map
 * stored is this vnode's, its length is the inode's runs now, so the next
 * page asked about needs no count. */
static void note_runs(struct cfs *fs, struct vnode *vn)
{
    struct cfs_vnode *cv = vn->fs_priv;
    if (fs->last_store_in == cfs_inode_of(vn)) {
        cv->runs = fs->last_store_n;
        cv->runs_at = fs->map_stores;
        cv->runs_valid = true;
    }
}

/* A page cfs_dirty_check let be dirtied that write-back cannot store is
 * this file's bug, not the writer's: named, under CONFIG_DEBUG, where it
 * happens (CFS-32). The extent-fail seam's -EFBIG is the one expected. */
static void stuck_page_check(struct cfs *fs, struct vnode *vn, uint64_t index, int rc)
{
#if CONFIG_DEBUG
    if ((rc == -EFBIG && !fs->fbig_injected) || rc == -EROFS)
        panic("cosmofs: inode %llu page %llu is dirty and write-back cannot store it (%d): cfs_dirty_check "
              "admitted it (CFS-32)",
              (unsigned long long)cfs_inode_of(vn)->ino, (unsigned long long)index, rc);
    fs->fbig_injected = false;
#else
    (void)fs;
    (void)vn;
    (void)index;
    (void)rc;
#endif
}

/*
 * The write policy that keeps a compressible file inside the run bound
 * (CFS-32 1b; design.md, "The run bound, charged at the write"). A
 * compressed record is a run of its own, and a plain record next to the
 * run before it is no new run at all, so once the inode holds
 * CFS_RAW_AT_PERCENT of the runs it may have, a new record is written
 * plain. A record that replaces a compressed one stays compressed: it
 * takes that record's run rather than adding one. The inode's runs fall
 * below the mark again when a truncate or rewrite takes them away, and
 * compression resumes. Under fs->lock.
 */
#define CFS_RAW_AT_PERCENT 75u

static bool record_compresses(struct cfs *fs, struct vnode *vn, uint64_t index)
{
    struct cfs_vnode *cv = vn->fs_priv;
    unsigned runs;
    if (cv->runs_valid && cv->runs_at == fs->map_stores) {
        runs = cv->runs;
    } else if (cfs_extents_count(fs, cfs_inode_of(vn), &runs) != 0) {
        return true;   /* the write itself will meet what the count did */
    }
    if ((uint64_t)runs * 100 < (uint64_t)cfs_max_extents(fs) * CFS_RAW_AT_PERCENT)
        return true;
    struct cfs_extent e;
    return cfs_map_ext(fs, cfs_inode_of(vn), index, &e) == 1 && cfs_ext_compressed(&e);
}

int cosmofs_test_runs(struct vnode *vn, unsigned *n)
{
#if CONFIG_SELFTEST
    struct cfs *fs = cfs_of(vn->mnt);
    int rc = cfs_lock_tree_w(fs, vn);
    if (rc)
        return rc;
    rc = cfs_extents_count(fs, cfs_inode_of(vn), n);
    cfs_unlock_tree(fs);
    return rc;
#else
    (void)vn;
    (void)n;
    return -ENOTSUP;
#endif
}

/*
 * The pool's space, charged at the write as the run bound is (MEM-9;
 * design.md, "Space, charged at the write"). A dirty page is written
 * back copy-on-write into a new block, so each dirty page of the mount
 * is a data block it will take: a page may become dirty while the data
 * blocks free above the metadata reserve cover every dirty page of the
 * mount, this one and CFS_DIRTY_META's allowance, and is refused -ENOSPC
 * otherwise. The checksum and map blocks a write-back adds are metadata,
 * which may take the reserve; the allowance is for writers who read the
 * count at once (one page each) and a margin that grows with the pages.
 * The charge is the dirty page itself: write-back takes the block it
 * stood for, and a truncate or a drop gives it back. A compressed record
 * takes fewer than its pages, so the charge is never short. Read without
 * the lock: two writers may each take the last page, which the allowance
 * covers.
 */
#define CFS_DIRTY_META(pages) ((pages) / 512u + 4u)

static bool space_fits(struct cfs *fs, struct vnode *vn)
{
    uint64_t free = __atomic_load_n(&fs->free_blocks, __ATOMIC_RELAXED);
    uint64_t usable = free > fs->reserve ? free - fs->reserve : 0;
    uint64_t pages = (uint64_t)__atomic_load_n(&vn->mnt->cache_dirty, __ATOMIC_RELAXED) + 1;
    return pages + CFS_DIRTY_META(pages) <= usable;
}

/* The VFS `dirty_check` operation. */
static int cfs_dirty_check(struct vnode *vn, uint64_t index, unsigned dirty)
{
    struct cfs *fs = cfs_of(vn->mnt);
    if (fs == NULL || fs->failed)
        return -EIO;     /* write-back would say the same, for ever */
    if (tree_readonly(fs, vn))
        return -EROFS;   /* a snapshot's file: its write-back is refused */
    if (index >= 0x100000000ull)
        return -EFBIG;   /* past a 32-bit logical block, which no run can name */
    unsigned runs;
    int rc = inode_runs(fs, vn, &runs);
    if (rc)
        return rc;
    if (!runs_fit(fs, runs, dirty + 1))
        return -EFBIG;
    /* A directory's blocks come from the metadata class, the reserve
     * included, so a deletion on a full pool still has one to write. */
    return vn->type != VNODE_REG || space_fits(fs, vn) ? 0 : -ENOSPC;
}

static int cfs_writepage(struct vnode *vn, uint64_t index, const void *buf)
{
    if (tree_readonly(cfs_of(vn->mnt), vn))
        return -EROFS;   /* a snapshot is what the tree was, not what it is */
    struct cfs *fs = cfs_of(vn->mnt);
    if (fs == NULL || fs->failed)
        return -EIO;   /* unmounted, or the transaction was abandoned */
    int rc = cfs_lock_tree_w(fs, vn);
    if (rc)
        return rc;
    rc = inode_prepare(fs, vn);   /* sync_after_write's inode write cannot fail after this (CFS-6) */
    if (rc) {
        cfs_unlock_tree(fs);
        return rc;
    }
    rc = write_one_page(fs, cfs_inode_of(vn), index, buf);
    rc = sync_after_write(fs, vn, rc);
    note_runs(fs, vn);
    stuck_page_check(fs, vn, index, rc);
    cfs_unlock_tree(fs);
    return rc;
}

/*
 * Several consecutive dirty pages at once. A whole aligned record is
 * written as one -- that is the only shape compression can shrink -- and
 * anything else falls back to one page, which the cache will offer again
 * with what is left.
 */
static int cfs_writepages(struct vnode *vn, uint64_t index, void *const *pages, unsigned n, unsigned *done)
{
    if (tree_readonly(cfs_of(vn->mnt), vn))
        return -EROFS;
    struct cfs *fs = cfs_of(vn->mnt);
    if (fs == NULL || fs->failed)
        return -EIO;
    int rc = cfs_lock_tree_w(fs, vn);
    if (rc)
        return rc;
    rc = inode_prepare(fs, vn);   /* as cfs_writepage */
    if (rc) {
        cfs_unlock_tree(fs);
        return rc;
    }
    struct cfs_inode *in = cfs_inode_of(vn);
    if (index % CFS_RECORD_BLOCKS == 0 && n >= CFS_RECORD_BLOCKS && in->compress_algo != CFS_COMPRESS_NONE) {
        bool compress = record_compresses(fs, vn, index);
        /* Gather the record; the pages are separate frames. */
        uint8_t *rec = kmalloc((size_t)CFS_RECORD_BLOCKS * CFS_BLOCK, 0);
        if (rec == NULL) {
            cfs_unlock_tree(fs);
            return -ENOMEM;
        }
        for (unsigned i = 0; i < CFS_RECORD_BLOCKS; i++)
            memcpy(rec + (size_t)i * CFS_BLOCK, pages[i], CFS_BLOCK);
        rc = cfs_record_write(fs, in, index, rec, CFS_RECORD_BLOCKS, compress);
        kfree(rec);
        if (rc == 0)
            *done = CFS_RECORD_BLOCKS;
    } else {
        rc = write_one_page(fs, in, index, pages[0]);
        if (rc == 0)
            *done = 1;
    }
    rc = sync_after_write(fs, vn, rc);
    note_runs(fs, vn);
    stuck_page_check(fs, vn, index, rc);
    cfs_unlock_tree(fs);
    return rc;
}

static int cfs_truncate(struct vnode *vn, uint64_t size)
{
    if (tree_readonly(cfs_of(vn->mnt), vn))
        return -EROFS;
    struct cfs *fs = cfs_of(vn->mnt);
    if (fs == NULL || fs->failed)
        return -EIO;   /* unmounted, or the transaction was abandoned */
    /*
     * The inode made writable before the page cache drops anything
     * (CFS-6): the trim cannot be undone, so an allocation that fails
     * must fail before it, with the file as it was (Codex on #433). The
     * trim runs without the tree lock, as it always has, and the
     * preparation is made again under it -- a no-op unless a commit came
     * between, which leaves this the one window where -ENOMEM can follow
     * a trim.
     */
    int lrc = cfs_lock_tree_w(fs, vn);
    if (lrc)
        return lrc;
    lrc = inode_prepare(fs, vn);
    cfs_unlock_tree(fs);
    if (lrc)
        return lrc;
    pagecache_truncate(vn, size);
    /* What the trim adds to the map must not come out of the room the
     * pages still dirty were charged for: near the bound, write them
     * first. The caller holds the vnode lock, the order pagecache_sync
     * runs in. */
    unsigned runs;
    lrc = inode_runs(fs, vn, &runs);
    if (lrc)
        return lrc;
    if (vn->pc.nr_dirty != 0 && !runs_fit(fs, runs + CFS_TRUNC_ADDS, vn->pc.nr_dirty)) {
        lrc = pagecache_sync(vn);
        if (lrc)
            return lrc;
    }
    lrc = cfs_lock_tree_w(fs, vn);
    if (lrc)
        return lrc;
    lrc = inode_prepare(fs, vn);
    if (lrc) {
        cfs_unlock_tree(fs);
        return lrc;
    }
    uint64_t keep = (size + CFS_BLOCK - 1) / CFS_BLOCK;
    int rc = cfs_truncate_blocks(fs, cfs_inode_of(vn), keep);
    /* The last block may now be partial. What is past the new end has to
     * read as zeros if the file grows again, and the page cache only
     * cleared its own copy, so the block itself is cleared here -- on
     * disk, where a later read will look for it. */
    if (rc == 0 && size % CFS_BLOCK != 0) {
        uint64_t lblk = size / CFS_BLOCK;
        struct cfs_extent e;
        int mrc = cfs_map_ext(fs, cfs_inode_of(vn), lblk, &e);
        if (mrc < 0) {
            rc = mrc;
        } else if (mrc == 1) {
            uint8_t *page = kmalloc(CFS_BLOCK, 0);
            if (page == NULL)
                rc = -ENOMEM;
            else {
                rc = read_logical_block(fs, cfs_inode_of(vn), lblk, page);
                if (rc == 0) {
                    memset(page + size % CFS_BLOCK, 0, CFS_BLOCK - size % CFS_BLOCK);
                    rc = write_one_page(fs, cfs_inode_of(vn), lblk, page);
                }
                kfree(page);
            }
        }
    }
    if (rc == 0) {
        vn->size = size;
        vn->mtime_ns = vfs_now_ns();
        rc = inode_sync(fs, vn);
    }
    note_runs(fs, vn);
    cfs_unlock_tree(fs);
    return rc;
}

/* The VFS `setattr` operation (chmod, utimensat, chown): the vnode's new
 * mode, times and owner go into the inode, as a write's new size does.
 * Directories, files and -- for chown, shell completeness S4 -- links,
 * whose inode keeps its type and its 0777. */
static int cfs_vnode_setattr(struct vnode *vn)
{
    struct cfs *fs = cfs_of(vn->mnt);
    if (fs == NULL)
        return 0;
    if (tree_readonly(fs, vn))
        return -EROFS;   /* a snapshot's metadata is what it was, as its data is (the caller restores the vnode) */
    int rc = cfs_lock_tree_w(fs, vn);
    if (rc)
        return rc;
    rc = inode_prepare(fs, vn);   /* a failure here changed nothing: the VFS puts the vnode back (CFS-6) */
    if (rc == 0)
        rc = inode_sync(fs, vn);
    cfs_unlock_tree(fs);
    return rc;
}

/* The VFS `sync` operation (file_sync after the page cache wrote the
 * file's pages): the inode goes through, then the open transaction is
 * committed, which is what durability means here (design.md, "fsync
 * commits"). */
static int cfs_vnode_sync(struct vnode *vn)
{
    struct cfs *fs = cfs_of(vn->mnt);
    if (fs == NULL)
        return 0;
    /* In the vnode's own tree: a clone's inode is the clone's, and the
     * commit leaves the clone first. A snapshot's has nothing to write. */
    int rc = cfs_lock_tree_w(fs, vn);
    if (rc)
        return rc == -EROFS ? 0 : rc;
    rc = inode_prepare(fs, vn);
    if (rc == 0)
        rc = inode_sync(fs, vn);
    if (rc == 0)
        rc = cfs_commit(fs);
    cfs_unlock_tree(fs);
    return rc;
}

static void cfs_evict(struct vnode *vn)
{
    struct cfs *fs = cfs_of(vn->mnt);
    struct cfs_vnode *cv = vn->fs_priv;
    /* A snapshot's vnode, or a clone's whose clone is gone (deleting it
     * freed its blocks), has nothing here to release: its inode number
     * is not the live tree's. */
    bool other_tree = false;
    if (fs && cv && cv->snap_tag != 0) {
        mutex_lock(&fs->lock);
        other_tree = cv->snap_tag == 0xFFFFu || cfs_clone_by_id(fs, cv->snap_tag) == NULL;
        mutex_unlock(&fs->lock);
    }
    if (fs && cv && vn->nlink == 0 && !other_tree) {
        /* The last link and the last reference are gone: release the
         * data blocks and the inode slot. */
        int trc = cfs_lock_tree_w(fs, vn);
        if (trc != 0)
            goto gone;   /* its clone went since the check above: nothing here is the live tree's */
        trc = cfs_truncate_blocks(fs, &cv->inode, 0);
        if (trc != 0) {
            /* The same hazard as a failed slot write, one step earlier:
             * the truncate frees every extent before storing the
             * shortened list, so a failure leaves blocks queued free
             * under an inode that still names them. */
            cfs_fail(fs, trc);
        } else {
            struct cfs_inode empty;
            memset(&empty, 0, sizeof(empty));
            /*
             * Only when the slot is actually cleared. Retiring the
             * record on a failed write would let the next commit publish
             * the deferred frees while the old slot still names those
             * blocks, and no later mount would retry the inode because
             * nothing would say it was owed.
             */
            if (cfs_inode_write(fs, vn->ino, &empty) == 0) {
                if (fs->sb.inode_count > 0)
                    fs->sb.inode_count--;
                cfs_orphan_remove(fs, vn->ino);
            } else {
                /* The blocks are queued to be freed and the slot still
                 * names them: no root may publish that. */
                cfs_fail(fs, -EIO);
            }
        }
        cfs_unlock_tree(fs);
    }
gone:
    kfree(cv);
    vn->fs_priv = NULL;
}

static const struct vnode_ops cfs_dir_ops = {
    .lookup = cfs_lookup,
    .create = cfs_create,
    .symlink = cfs_symlink,
    .mkdir = cfs_mkdir,
    .unlink = cfs_unlink,
    .link = cfs_link,
    .rmdir = cfs_rmdir,
    .rename = cfs_rename,
    .readdir = cfs_readdir,
    .sync = cfs_vnode_sync,
    .evict = cfs_evict,
    .setattr = cfs_vnode_setattr,
};

static const struct vnode_ops cfs_file_ops = {
    .readpage = cfs_readpage,
    .writepage = cfs_writepage,
    .writepages = cfs_writepages,
    .dirty_check = cfs_dirty_check,
    .truncate = cfs_truncate,
    .sync = cfs_vnode_sync,
    .evict = cfs_evict,
    .setattr = cfs_vnode_setattr,
};

/* A link is never a page: its target is read from its block directly,
 * so there is no readpage or writepage here. */
static const struct vnode_ops cfs_lnk_ops = {
    .readlink = cfs_readlink,
    .evict = cfs_evict,
    .setattr = cfs_vnode_setattr,   /* lchown */
};
