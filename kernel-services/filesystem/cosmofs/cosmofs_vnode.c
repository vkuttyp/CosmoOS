/*
 * cosmofs_vnode.c - cosmofs's vnodes: one made for an inode of the live
 * tree, a snapshot's or a clone's, filled from it, and the inode written
 * through from it. Moved out of cosmofs.c (roadmap 2.0 SA7) so that it
 * can be held to a digest and made CeeSafe on its own.
 */

#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/faultinject.h>
#include <kernel/kmalloc.h>
#include <kernel/vfs.h>

#include "cosmofs_internal.h"

/* --- vnodes ---------------------------------------------------------------- */

void cfs_fill_vnode(struct vnode *vn, const struct cfs_inode *in)
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
int cfs_vnode_get_tagged(struct cfs *fs, uint64_t ino, unsigned tag, const struct cfs_snapshot *snap,
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
    cfs_fill_vnode(vn, &cv->inode);
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
        cfs_fill_vnode(cvn, &cv->inode);
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
    cfs_fill_vnode(vn, &cv->inode);
    vnode_hash_insert(vn);
    *out = vn;
    return 0;
}

/* Push the vnode's public fields into its inode and write it through.
 * Every caller has already changed what the inode describes -- a block
 * mapped and its predecessor queued free, an entry added or removed, a
 * link count -- which is why each prepared the inode first (CFS-6). */
int cfs_vnode_inode_sync(struct cfs *fs, struct vnode *vn)
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
     * writable before its first change (cfs_vnode_prepare), so this writes
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
 * ordinary error the caller can retry, where in cfs_vnode_inode_sync it abandoned
 * the transaction.
 */
int cfs_vnode_prepare(struct cfs *fs, struct vnode *vn)
{
    /* The test seam (cosmofs-nomem): the inode's map path cannot be had. */
    if (faultinject_should_fail(FI_COSMOFS_INODE_NOMEM))
        return -ENOMEM;
    return cfs_inode_prepare(fs, vn->ino);
}
