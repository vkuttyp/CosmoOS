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

/* --- directories ---------------------------------------------------------------- */

int cfs_dir_read_block(struct cfs *fs, struct vnode *dir, uint64_t lblk, uint8_t *buf)
{
    return cfs_dir_read_block_at(fs, cfs_inode_of(dir), lblk, buf);
}

/* Write directory block `lblk` copy-on-write and update the inode. */
int cfs_dir_write_block(struct cfs *fs, struct vnode *dir, uint64_t lblk, const uint8_t *buf)
{
    int rc = cfs_data_write_block(fs, cfs_inode_of(dir), lblk, buf, CFS_ALLOC_META);
    if (rc)
        return rc;
    if ((lblk + 1) * CFS_BLOCK > dir->size)
        dir->size = (lblk + 1) * CFS_BLOCK;
    dir->mtime_ns = vfs_now_ns();
    return cfs_vnode_inode_sync(fs, dir);
}

uint64_t cfs_dir_blocks(const struct vnode *dir)
{
    return (dir->size + CFS_BLOCK - 1) / CFS_BLOCK;
}

/* Find `name`; returns the block index and slot, or -ENOENT. */
int cfs_dir_find(struct cfs *fs, struct vnode *dir, const char *name, size_t len, uint8_t *block,
                 uint64_t *lblk_out, unsigned *slot_out)
{
    if (len > CFS_NAME_MAX_LONG)
        return -ENAMETOOLONG;
    if (len > CFS_NAME_MAX && fs->sb.version < 15)
        return -ENOENT;   /* no entry of a pool below 15 has a name this long */
    for (uint64_t lblk = 0; lblk < cfs_dir_blocks(dir); lblk++) {
        int rc = cfs_dir_read_block(fs, dir, lblk, block);
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
int cfs_dir_add(struct cfs *fs, struct vnode *dir, const char *name, size_t len, uint64_t ino, unsigned type,
                uint8_t *block)
{
    unsigned max = fs->sb.version >= 15 ? CFS_NAME_MAX_LONG : CFS_NAME_MAX;
    if (len == 0 || len > max)
        return -ENAMETOOLONG;
    unsigned need = 1u + cfs_dirent_extra((unsigned)len);
    uint64_t nb = cfs_dir_blocks(dir);
    for (uint64_t lblk = 0; lblk <= nb; lblk++) {
        int rc;
        if (lblk == nb)
            memset(block, 0, CFS_BLOCK);
        else if ((rc = cfs_dir_read_block(fs, dir, lblk, block)) != 0)
            return rc;
        struct cfs_dirent *d = (struct cfs_dirent *)block;
        int s = cfs_dirent_free_run(d, need);
        if (s >= 0) {
            /* An entry names an inode of its own tree, untagged. */
            cfs_dirent_put(d, (unsigned)s, CFS_INO_OF(ino), type, name, (unsigned)len);
            return cfs_dir_write_block(fs, dir, lblk, block);
        }
    }
    return -ENOSPC;
}

/* The entry at `slot`, found by cfs_dir_find: its head and continuations. */
int cfs_dir_remove_slot(struct cfs *fs, struct vnode *dir, uint64_t lblk, unsigned slot, uint8_t *block)
{
    cfs_dirent_clear((struct cfs_dirent *)block, slot);
    return cfs_dir_write_block(fs, dir, lblk, block);
}

bool cfs_dir_is_empty(struct cfs *fs, struct vnode *dir, uint8_t *block)
{
    for (uint64_t lblk = 0; lblk < cfs_dir_blocks(dir); lblk++) {
        if (cfs_dir_read_block(fs, dir, lblk, block))
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
int cfs_snapdir_get(struct cfs *fs, struct vnode **out)
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
    cfs_fill_vnode(vn, &cv->inode);
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

bool cfs_is_snapdir(struct vnode *vn)
{
    struct cfs_vnode *cv = vn->fs_priv;
    return cv && cv->snap_tag == 0xFFFFu;
}

unsigned cfs_snap_tag_of(struct vnode *vn)
{
    struct cfs_vnode *cv = vn->fs_priv;
    return cv && cv->snap_tag != 0xFFFFu ? cv->snap_tag : 0;
}

/*
 * The tree an operation on `vn` writes: the live tree, or a clone, which
 * it enters for the length of the operation (cosmofs_clone.cee). A vnode
 * of a snapshot is read-only, and so is one of a clone since deleted.
 */
bool cfs_tree_readonly(struct cfs *fs, struct vnode *vn)
{
    if (cfs_is_snapdir(vn))
        return true;
    unsigned tag = cfs_snap_tag_of(vn);
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
int cfs_lock_tree_w(struct cfs *fs, struct vnode *vn)
{
    if (cfs_is_snapdir(vn))
        return -EROFS;
    mutex_lock(&fs->lock);
    unsigned tag = cfs_snap_tag_of(vn);
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

void cfs_unlock_tree(struct cfs *fs)
{
    cfs_clone_leave(fs);
    mutex_unlock(&fs->lock);
}

const struct vnode_ops cfs_dir_ops = {
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

const struct vnode_ops cfs_file_ops = {
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
const struct vnode_ops cfs_lnk_ops = {
    .readlink = cfs_readlink,
    .evict = cfs_evict,
    .setattr = cfs_vnode_setattr,   /* lchown */
};
