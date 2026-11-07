/*
 * cosmofs_file.c - A regular file's vnode operations: pages read, written
 * and written back (as plain blocks or compressed records, under the run
 * bound and the space a dirty page is charged), truncation, setattr, sync
 * and eviction (with the orphan record). Moved out of cosmofs.c (roadmap
 * 2.0 SA7) so that it can be held to a digest and made CeeSafe on its own.
 */

#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/faultinject.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/page.h>
#include <kernel/panic.h>
#include <kernel/string.h>

#include "cosmofs_internal.h"

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

int cfs_readpage(struct vnode *vn, uint64_t index, void *buf)
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
    int src = cfs_vnode_inode_sync(fs, vn);
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
int cfs_dirty_check(struct vnode *vn, uint64_t index, unsigned dirty)
{
    struct cfs *fs = cfs_of(vn->mnt);
    if (fs == NULL || fs->failed)
        return -EIO;     /* write-back would say the same, for ever */
    if (cfs_tree_readonly(fs, vn))
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

int cfs_writepage(struct vnode *vn, uint64_t index, const void *buf)
{
    if (cfs_tree_readonly(cfs_of(vn->mnt), vn))
        return -EROFS;   /* a snapshot is what the tree was, not what it is */
    struct cfs *fs = cfs_of(vn->mnt);
    if (fs == NULL || fs->failed)
        return -EIO;   /* unmounted, or the transaction was abandoned */
    int rc = cfs_lock_tree_w(fs, vn);
    if (rc)
        return rc;
    rc = cfs_vnode_prepare(fs, vn);   /* sync_after_write's inode write cannot fail after this (CFS-6) */
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
int cfs_writepages(struct vnode *vn, uint64_t index, void *const *pages, unsigned n, unsigned *done)
{
    if (cfs_tree_readonly(cfs_of(vn->mnt), vn))
        return -EROFS;
    struct cfs *fs = cfs_of(vn->mnt);
    if (fs == NULL || fs->failed)
        return -EIO;
    int rc = cfs_lock_tree_w(fs, vn);
    if (rc)
        return rc;
    rc = cfs_vnode_prepare(fs, vn);   /* as cfs_writepage */
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

int cfs_truncate(struct vnode *vn, uint64_t size)
{
    if (cfs_tree_readonly(cfs_of(vn->mnt), vn))
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
    lrc = cfs_vnode_prepare(fs, vn);
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
    lrc = cfs_vnode_prepare(fs, vn);
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
        rc = cfs_vnode_inode_sync(fs, vn);
    }
    note_runs(fs, vn);
    cfs_unlock_tree(fs);
    return rc;
}

/* The VFS `setattr` operation (chmod, utimensat, chown): the vnode's new
 * mode, times and owner go into the inode, as a write's new size does.
 * Directories, files and -- for chown, shell completeness S4 -- links,
 * whose inode keeps its type and its 0777. */
int cfs_vnode_setattr(struct vnode *vn)
{
    struct cfs *fs = cfs_of(vn->mnt);
    if (fs == NULL)
        return 0;
    if (cfs_tree_readonly(fs, vn))
        return -EROFS;   /* a snapshot's metadata is what it was, as its data is (the caller restores the vnode) */
    int rc = cfs_lock_tree_w(fs, vn);
    if (rc)
        return rc;
    rc = cfs_vnode_prepare(fs, vn);   /* a failure here changed nothing: the VFS puts the vnode back (CFS-6) */
    if (rc == 0)
        rc = cfs_vnode_inode_sync(fs, vn);
    cfs_unlock_tree(fs);
    return rc;
}

/* The VFS `sync` operation (file_sync after the page cache wrote the
 * file's pages): the inode goes through, then the open transaction is
 * committed, which is what durability means here (design.md, "fsync
 * commits"). */
int cfs_vnode_sync(struct vnode *vn)
{
    struct cfs *fs = cfs_of(vn->mnt);
    if (fs == NULL)
        return 0;
    /* In the vnode's own tree: a clone's inode is the clone's, and the
     * commit leaves the clone first. A snapshot's has nothing to write. */
    int rc = cfs_lock_tree_w(fs, vn);
    if (rc)
        return rc == -EROFS ? 0 : rc;
    rc = cfs_vnode_prepare(fs, vn);
    if (rc == 0)
        rc = cfs_vnode_inode_sync(fs, vn);
    if (rc == 0)
        rc = cfs_commit(fs);
    cfs_unlock_tree(fs);
    return rc;
}

void cfs_evict(struct vnode *vn)
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
