/*
 * cosmofs.c - Vnode operations: inodes, extents, directories, data.
 *
 * Every mutation writes the inode through to its (copy-on-write) inode
 * block immediately, so the buffer cache always holds the complete
 * pre-commit state and vnode eviction has nothing to flush but pages.
 * Directory blocks and file data are written to freshly allocated
 * blocks; the previous block goes on the deferred free list.
 */

#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/page.h>
#include <kernel/string.h>

#include "cosmofs_internal.h"

static const struct vnode_ops cfs_dir_ops;
static const struct vnode_ops cfs_file_ops;

/* --- extents ------------------------------------------------------------- */

/* Gather the inode's runs into ext[] (up to CFS_MAX_EXTENTS). */
static int extents_load(struct cfs *fs, const struct cfs_inode *in, struct cfs_extent *ext, unsigned *n)
{
    unsigned k = 0;
    for (unsigned i = 0; i < CFS_DIRECT && in->direct[i].count; i++)
        ext[k++] = in->direct[i];
    if (in->indirect) {
        struct cfs_buf *b;
        int rc = cfs_buf_get(fs, in->indirect, CFS_KIND_EXTENTS, &b);
        if (rc)
            return rc;
        const struct cfs_extent *ind = (const struct cfs_extent *)(b->data + CFS_MHDR_SIZE);
        for (unsigned i = 0; i < CFS_EXTENTS_PER_BLOCK && ind[i].count; i++)
            ext[k++] = ind[i];
        cfs_buf_put(fs, b);
    }
    for (unsigned i = 0; i < k; i++) {
        /* Overflow-safe: start below the end and count within the remainder. */
        if (ext[i].start < 2 || ext[i].start >= fs->nblocks || ext[i].count == 0 ||
            ext[i].count > fs->nblocks - ext[i].start)
            return -EIO;
    }
    *n = k;
    return 0;
}

/* Store runs back: direct first, the rest in the indirect block. */
static int extents_store(struct cfs *fs, struct cfs_inode *in, const struct cfs_extent *ext, unsigned n)
{
    if (n > CFS_MAX_EXTENTS)
        return -EFBIG;
    memset(in->direct, 0, sizeof(in->direct));
    for (unsigned i = 0; i < n && i < CFS_DIRECT; i++)
        in->direct[i] = ext[i];
    if (n <= CFS_DIRECT) {
        if (in->indirect) {
            cfs_free_block_deferred(fs, in->indirect);
            in->indirect = 0;
        }
        return 0;
    }
    struct cfs_buf *b;
    int rc;
    if (in->indirect) {
        rc = cfs_buf_get(fs, in->indirect, CFS_KIND_EXTENTS, &b);
        if (rc == 0)
            rc = cfs_buf_cow(fs, &b, &in->indirect);
    } else {
        rc = cfs_buf_new(fs, CFS_KIND_EXTENTS, &b);
        if (rc == 0)
            in->indirect = b->blkno;
    }
    if (rc)
        return rc;
    struct cfs_extent *ind = (struct cfs_extent *)(b->data + CFS_MHDR_SIZE);
    memset(ind, 0, CFS_PAYLOAD);
    for (unsigned i = CFS_DIRECT; i < n; i++)
        ind[i - CFS_DIRECT] = ext[i];
    cfs_buf_put(fs, b);
    return 0;
}

int cfs_map(struct cfs *fs, const struct cfs_inode *in, uint64_t lblk, uint64_t *pblk)
{
    struct cfs_extent ext[CFS_MAX_EXTENTS];
    unsigned n;
    int rc = extents_load(fs, in, ext, &n);
    if (rc)
        return rc;
    return cfs_map_block(ext, n, lblk, pblk);
}

/* Merge physically adjacent runs in place. */
static unsigned extents_merge(struct cfs_extent *ext, unsigned n)
{
    unsigned w = 0;
    for (unsigned i = 0; i < n; i++) {
        if (ext[i].count == 0)
            continue;
        if (w > 0 && ext[w - 1].start + ext[w - 1].count == ext[i].start &&
            ext[w - 1].count + ext[i].count < 0xffffffffu) {
            ext[w - 1].count += ext[i].count;
        } else {
            ext[w++] = ext[i];
        }
    }
    return w;
}

int cfs_set_block(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, uint64_t pblk, uint64_t *old)
{
    struct cfs_extent ext[CFS_MAX_EXTENTS + 2];
    unsigned n;
    int rc = extents_load(fs, in, ext, &n);
    if (rc)
        return rc;
    uint64_t covered = cfs_extent_blocks(ext, n);
    *old = 0;

    if (lblk >= covered) {
        /* Beyond the mapped span: holes in between are not representable
         * as runs, so the span must be extended contiguously. */
        if (lblk != covered)
            return -EINVAL;
        ext[n].start = pblk;
        ext[n].count = 1;
        ext[n].pad = 0;
        n++;
    } else {
        /* Split the run that covers lblk into up to three pieces. */
        struct cfs_extent out[CFS_MAX_EXTENTS + 2];
        unsigned m = 0;
        uint64_t base = 0;
        for (unsigned i = 0; i < n; i++) {
            struct cfs_extent e = ext[i];
            if (lblk >= base && lblk < base + e.count) {
                uint64_t off = lblk - base;
                *old = e.start + off;
                if (off > 0)
                    out[m++] = (struct cfs_extent){ e.start, (uint32_t)off, 0 };
                out[m++] = (struct cfs_extent){ pblk, 1, 0 };
                if (off + 1 < e.count)
                    out[m++] = (struct cfs_extent){ e.start + off + 1, (uint32_t)(e.count - off - 1), 0 };
            } else {
                out[m++] = e;
            }
            base += e.count;
            if (m > CFS_MAX_EXTENTS + 1)
                return -EFBIG;
        }
        memcpy(ext, out, m * sizeof(*out));
        n = m;
    }
    n = extents_merge(ext, n);
    return extents_store(fs, in, ext, n);
}

int cfs_truncate_blocks(struct cfs *fs, struct cfs_inode *in, uint64_t keep)
{
    struct cfs_extent ext[CFS_MAX_EXTENTS];
    unsigned n;
    int rc = extents_load(fs, in, ext, &n);
    if (rc)
        return rc;
    uint64_t base = 0;
    unsigned m = 0;
    for (unsigned i = 0; i < n; i++) {
        struct cfs_extent e = ext[i];
        if (base >= keep) {
            for (uint32_t k = 0; k < e.count; k++)
                cfs_free_block_deferred(fs, e.start + k);
        } else if (base + e.count > keep) {
            uint32_t keepc = (uint32_t)(keep - base);
            for (uint32_t k = keepc; k < e.count; k++)
                cfs_free_block_deferred(fs, e.start + k);
            e.count = keepc;
            ext[m++] = e;
        } else {
            ext[m++] = e;
        }
        base += e.count;
    }
    return extents_store(fs, in, ext, m);
}

/* --- vnodes ---------------------------------------------------------------- */

static void fill_vnode(struct vnode *vn, const struct cfs_inode *in)
{
    unsigned type = CFS_MODE_TYPE(in->mode);
    vn->type = type == CFS_TYPE_DIR ? VNODE_DIR : VNODE_REG;
    vn->mode = in->mode & 07777;
    vn->uid = in->uid;
    vn->gid = in->gid;
    vn->nlink = in->nlink;
    vn->size = in->size;
    vn->mtime_ns = in->mtime_ns;
    vn->ctime_ns = in->ctime_ns;
    vn->ops = vn->type == VNODE_DIR ? &cfs_dir_ops : &cfs_file_ops;
}

/* Referenced vnode for `ino`, cached or instantiated. fs->lock held. */
int cfs_vnode_get(struct cfs *fs, uint64_t ino, struct vnode **out)
{
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

/* Push the vnode's public fields into its inode and write it through. */
static int inode_sync(struct cfs *fs, struct vnode *vn)
{
    struct cfs_inode *in = cfs_inode_of(vn);
    in->nlink = vn->nlink;
    in->size = vn->size;
    in->mtime_ns = vn->mtime_ns;
    in->ctime_ns = vn->ctime_ns;
    in->mode = CFS_MODE(vn->type == VNODE_DIR ? CFS_TYPE_DIR : CFS_TYPE_REG, vn->mode);
    return cfs_inode_write(fs, vn->ino, in);
}

/* --- directories ---------------------------------------------------------------- */

/* Read directory block `lblk` into buf (zeros for a hole). */
static int dir_read_block(struct cfs *fs, struct vnode *dir, uint64_t lblk, uint8_t *buf)
{
    uint64_t pblk = 0;
    int rc = cfs_map(fs, cfs_inode_of(dir), lblk, &pblk);
    if (rc < 0)
        return rc;
    if (rc == 0) {
        memset(buf, 0, CFS_BLOCK);
        return 0;
    }
    return cfs_data_read(fs, pblk, buf);
}

/* Write directory block `lblk` copy-on-write and update the inode. */
static int dir_write_block(struct cfs *fs, struct vnode *dir, uint64_t lblk, const uint8_t *buf)
{
    uint64_t nblk, old;
    int rc = cfs_alloc_block(fs, &nblk);
    if (rc)
        return rc;
    rc = cfs_data_write(fs, nblk, buf);
    if (rc == 0)
        rc = cfs_set_block(fs, cfs_inode_of(dir), lblk, nblk, &old);
    if (rc) {
        cfs_free_block_deferred(fs, nblk);
        return rc;
    }
    if (old)
        cfs_free_block_deferred(fs, old);
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
    if (len > CFS_NAME_MAX)
        return -ENAMETOOLONG;
    for (uint64_t lblk = 0; lblk < dir_blocks(dir); lblk++) {
        int rc = dir_read_block(fs, dir, lblk, block);
        if (rc)
            return rc;
        const struct cfs_dirent *d = (const struct cfs_dirent *)block;
        for (unsigned s = 0; s < CFS_DIRENTS_PER_BLOCK; s++) {
            if (d[s].ino && d[s].namelen == len && memcmp(d[s].name, name, len) == 0) {
                *lblk_out = lblk;
                *slot_out = s;
                return 0;
            }
        }
    }
    return -ENOENT;
}

static int dir_add(struct cfs *fs, struct vnode *dir, const char *name, size_t len, uint64_t ino, unsigned type,
                   uint8_t *block)
{
    if (len == 0 || len > CFS_NAME_MAX)
        return -ENAMETOOLONG;
    uint64_t nb = dir_blocks(dir);
    for (uint64_t lblk = 0; lblk <= nb; lblk++) {
        int rc;
        if (lblk == nb)
            memset(block, 0, CFS_BLOCK);
        else if ((rc = dir_read_block(fs, dir, lblk, block)) != 0)
            return rc;
        struct cfs_dirent *d = (struct cfs_dirent *)block;
        for (unsigned s = 0; s < CFS_DIRENTS_PER_BLOCK; s++) {
            if (d[s].ino == 0) {
                memset(&d[s], 0, sizeof(d[s]));
                d[s].ino = ino;
                d[s].type = (uint8_t)type;
                d[s].namelen = (uint8_t)len;
                memcpy(d[s].name, name, len);
                return dir_write_block(fs, dir, lblk, block);
            }
        }
    }
    return -ENOSPC;
}

static int dir_remove_slot(struct cfs *fs, struct vnode *dir, uint64_t lblk, unsigned slot, uint8_t *block)
{
    struct cfs_dirent *d = (struct cfs_dirent *)block;
    memset(&d[slot], 0, sizeof(d[slot]));
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

static int cfs_lookup(struct vnode *dir, const char *name, size_t len, struct vnode **out)
{
    struct cfs *fs = cfs_of(dir->mnt);
    if (fs == NULL)
        return -EIO;
    mutex_lock(&fs->lock);
    int rc;
    if (len == 2 && name[0] == '.' && name[1] == '.') {
        rc = cfs_vnode_get(fs, cfs_inode_of(dir)->parent, out);
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

static int cfs_create_common(struct vnode *dir, const char *name, size_t len, uint32_t mode, unsigned type,
                             struct vnode **out)
{
    struct cfs *fs = cfs_of(dir->mnt);
    if (fs == NULL)
        return -EIO;
    if (len > CFS_NAME_MAX)
        return -ENAMETOOLONG;
    uint8_t *block = kmalloc(CFS_BLOCK, 0);
    if (block == NULL)
        return -ENOMEM;
    mutex_lock(&fs->lock);
    uint64_t lblk;
    unsigned slot;
    int rc = dir_find(fs, dir, name, len, block, &lblk, &slot);
    if (rc == 0) {
        rc = -EEXIST;
        goto out;
    }
    if (rc != -ENOENT)
        goto out;
    uint64_t ino;
    rc = cfs_inode_alloc(fs, &ino);
    if (rc)
        goto out;
    struct cfs_inode in;
    memset(&in, 0, sizeof(in));
    in.mode = CFS_MODE(type, mode);
    in.nlink = type == CFS_TYPE_DIR ? 2 : 1;
    in.ino = ino;
    in.parent = dir->ino;
    in.mtime_ns = in.ctime_ns = vfs_now_ns();
    rc = cfs_inode_write(fs, ino, &in);
    if (rc)
        goto out;
    rc = dir_add(fs, dir, name, len, ino, type, block);
    if (rc)
        goto out;
    if (type == CFS_TYPE_DIR) {
        dir->nlink++;
        rc = inode_sync(fs, dir);
        if (rc)
            goto out;
    }
    rc = cfs_vnode_get(fs, ino, out);
out:
    mutex_unlock(&fs->lock);
    kfree(block);
    return rc;
}

static int cfs_create(struct vnode *dir, const char *name, size_t len, uint32_t mode, struct vnode **out)
{
    return cfs_create_common(dir, name, len, mode, CFS_TYPE_REG, out);
}

static int cfs_mkdir(struct vnode *dir, const char *name, size_t len, uint32_t mode, struct vnode **out)
{
    return cfs_create_common(dir, name, len, mode, CFS_TYPE_DIR, out);
}

static int cfs_unlink_common(struct vnode *dir, const char *name, size_t len, struct vnode *victim, bool rmdir)
{
    struct cfs *fs = cfs_of(dir->mnt);
    if (fs == NULL)
        return -EIO;
    uint8_t *block = kmalloc(CFS_BLOCK, 0);
    if (block == NULL)
        return -ENOMEM;
    mutex_lock(&fs->lock);
    uint64_t lblk;
    unsigned slot;
    int rc = dir_find(fs, dir, name, len, block, &lblk, &slot);
    if (rc)
        goto out;
    if (((struct cfs_dirent *)block)[slot].ino != victim->ino) {
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
    victim->ctime_ns = vfs_now_ns();
    if (rc == 0)
        rc = inode_sync(fs, victim);
out:
    mutex_unlock(&fs->lock);
    kfree(block);
    return rc;
}

static int cfs_unlink(struct vnode *dir, const char *name, size_t len, struct vnode *victim)
{
    return cfs_unlink_common(dir, name, len, victim, false);
}

static int cfs_rmdir(struct vnode *dir, const char *name, size_t len, struct vnode *victim)
{
    return cfs_unlink_common(dir, name, len, victim, true);
}

static int cfs_rename(struct vnode *odir, const char *oname, size_t olen, struct vnode *victim, struct vnode *ndir,
                      const char *nname, size_t nlen, struct vnode *replaced)
{
    struct cfs *fs = cfs_of(odir->mnt);
    if (fs == NULL)
        return -EIO;
    if (nlen > CFS_NAME_MAX)
        return -ENAMETOOLONG;
    uint8_t *block = kmalloc(CFS_BLOCK, 0);
    if (block == NULL)
        return -ENOMEM;
    mutex_lock(&fs->lock);
    uint64_t lblk;
    unsigned slot;
    int rc = dir_find(fs, odir, oname, olen, block, &lblk, &slot);
    if (rc)
        goto out;
    if (((struct cfs_dirent *)block)[slot].ino != victim->ino) {
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
        /* Overwrite the existing entry in place: same name, new inode. */
        uint64_t rl;
        unsigned rs;
        rc = dir_find(fs, ndir, nname, nlen, block, &rl, &rs);
        if (rc)
            goto out;
        struct cfs_dirent *rd = &((struct cfs_dirent *)block)[rs];
        rd->ino = victim->ino;
        rd->type = (uint8_t)type;
        rc = dir_write_block(fs, ndir, rl, block);
        if (rc)
            goto out;
    } else {
        rc = dir_add(fs, ndir, nname, nlen, victim->ino, type, block);
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
        if (dir_find(fs, ndir, nname, nlen, block, &rl, &rs) == 0) {
            if (replaced) {
                struct cfs_dirent *rd = &((struct cfs_dirent *)block)[rs];
                rd->ino = replaced->ino;
                rd->type = (uint8_t)(replaced->type == VNODE_DIR ? CFS_TYPE_DIR : CFS_TYPE_REG);
                dir_write_block(fs, ndir, rl, block);
            } else {
                dir_remove_slot(fs, ndir, rl, rs, block);
            }
        }
        kerror("cosmofs: rename of %.*s failed (%d); namespace restored", (int)olen, oname, rc);
        goto out;
    }
    /* The namespace change is published. The metadata updates below are
     * write-throughs into copy-on-write buffers; if one of them fails the
     * in-memory state can no longer be made consistent, so the whole open
     * transaction is abandoned: nothing of it reaches the disk and the
     * last committed root remains the truth. */
    if (replaced) {
        replaced->nlink = replaced->type == VNODE_DIR ? 0 : replaced->nlink - 1;
        if (replaced->type == VNODE_DIR)
            ndir->nlink--;
        rc = inode_sync(fs, replaced);
    }
    if (rc == 0 && odir != ndir) {
        cfs_inode_of(victim)->parent = ndir->ino;
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
    mutex_unlock(&fs->lock);
    kfree(block);
    return rc;
}

static int cfs_readdir(struct vnode *dir, uint64_t *pos, vfs_dirent_cb cb, void *arg)
{
    struct cfs *fs = cfs_of(dir->mnt);
    if (fs == NULL)
        return -EIO;
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
    uint8_t *block = kmalloc(CFS_BLOCK, 0);
    if (block == NULL)
        return -ENOMEM;
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
        if (d[s].ino) {
            enum vnode_type t = d[s].type == CFS_TYPE_DIR ? VNODE_DIR : VNODE_REG;
            if (cb(arg, d[s].name, d[s].namelen, d[s].ino, t))
                break;
        }
        *pos = i + 3;
    }
    mutex_unlock(&fs->lock);
    kfree(block);
    return rc;
}

/* --- regular files ------------------------------------------------------------------ */

static int cfs_readpage(struct vnode *vn, uint64_t index, void *buf)
{
    struct cfs *fs = cfs_of(vn->mnt);
    if (fs == NULL)
        return -EIO;
    mutex_lock(&fs->lock);
    uint64_t pblk = 0;
    int rc = cfs_map(fs, cfs_inode_of(vn), index, &pblk);
    if (rc == 0)
        memset(buf, 0, CFS_BLOCK);
    else if (rc == 1)
        rc = cfs_data_read(fs, pblk, buf);
    mutex_unlock(&fs->lock);
    return rc > 0 ? 0 : rc;
}

static int cfs_writepage(struct vnode *vn, uint64_t index, const void *buf)
{
    struct cfs *fs = cfs_of(vn->mnt);
    if (fs == NULL)
        return -EIO;
    mutex_lock(&fs->lock);
    struct cfs_inode *in = cfs_inode_of(vn);
    /* Runs cannot express holes: pages before `index` that were never
     * written get zero blocks first. */
    uint64_t covered = cfs_extent_blocks(in->direct, CFS_DIRECT);
    if (in->indirect) {
        struct cfs_extent ext[CFS_MAX_EXTENTS];
        unsigned n;
        int r = extents_load(fs, in, ext, &n);
        if (r) {
            mutex_unlock(&fs->lock);
            return r;
        }
        covered = cfs_extent_blocks(ext, n);
    }
    int rc = 0;
    uint8_t *zero = NULL;
    for (uint64_t l = covered; l < index && rc == 0; l++) {
        if (zero == NULL) {
            zero = kmalloc(CFS_BLOCK, KMEM_ZERO);
            if (zero == NULL) {
                rc = -ENOMEM;
                break;
            }
        }
        uint64_t nblk, old;
        rc = cfs_alloc_block(fs, &nblk);
        if (rc == 0)
            rc = cfs_data_write(fs, nblk, zero);
        if (rc == 0)
            rc = cfs_set_block(fs, in, l, nblk, &old);
    }
    kfree(zero);
    if (rc == 0) {
        uint64_t nblk, old;
        rc = cfs_alloc_block(fs, &nblk);
        if (rc == 0)
            rc = cfs_data_write(fs, nblk, buf);
        if (rc == 0)
            rc = cfs_set_block(fs, in, index, nblk, &old);
        if (rc == 0 && old)
            cfs_free_block_deferred(fs, old);
        else if (rc)
            cfs_free_block_deferred(fs, nblk);
    }
    if (rc == 0)
        rc = inode_sync(fs, vn);
    mutex_unlock(&fs->lock);
    return rc;
}

static int cfs_truncate(struct vnode *vn, uint64_t size)
{
    struct cfs *fs = cfs_of(vn->mnt);
    if (fs == NULL)
        return -EIO;
    pagecache_truncate(vn, size);
    mutex_lock(&fs->lock);
    uint64_t keep = (size + CFS_BLOCK - 1) / CFS_BLOCK;
    int rc = cfs_truncate_blocks(fs, cfs_inode_of(vn), keep);
    if (rc == 0) {
        vn->size = size;
        vn->mtime_ns = vfs_now_ns();
        rc = inode_sync(fs, vn);
    }
    mutex_unlock(&fs->lock);
    return rc;
}

static int cfs_vnode_sync(struct vnode *vn)
{
    struct cfs *fs = cfs_of(vn->mnt);
    if (fs == NULL)
        return 0;
    mutex_lock(&fs->lock);
    int rc = inode_sync(fs, vn);
    mutex_unlock(&fs->lock);
    return rc;
}

static void cfs_evict(struct vnode *vn)
{
    struct cfs *fs = cfs_of(vn->mnt);
    struct cfs_vnode *cv = vn->fs_priv;
    if (fs && cv && vn->nlink == 0) {
        /* The last link and the last reference are gone: release the
         * data blocks and the inode slot. */
        mutex_lock(&fs->lock);
        if (cfs_truncate_blocks(fs, &cv->inode, 0) == 0) {
            struct cfs_inode empty;
            memset(&empty, 0, sizeof(empty));
            cfs_inode_write(fs, vn->ino, &empty);
            if (fs->sb.inode_count > 0)
                fs->sb.inode_count--;
        }
        mutex_unlock(&fs->lock);
    }
    kfree(cv);
    vn->fs_priv = NULL;
}

static const struct vnode_ops cfs_dir_ops = {
    .lookup = cfs_lookup,
    .create = cfs_create,
    .mkdir = cfs_mkdir,
    .unlink = cfs_unlink,
    .rmdir = cfs_rmdir,
    .rename = cfs_rename,
    .readdir = cfs_readdir,
    .sync = cfs_vnode_sync,
    .evict = cfs_evict,
};

static const struct vnode_ops cfs_file_ops = {
    .readpage = cfs_readpage,
    .writepage = cfs_writepage,
    .truncate = cfs_truncate,
    .sync = cfs_vnode_sync,
    .evict = cfs_evict,
};
