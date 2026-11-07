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

/* --- extents ------------------------------------------------------------- */

/*
 * A run maps logical blocks [lblk, lblk + count) to pool blocks
 * [start, start + count). Runs are kept sorted by lblk; a logical block no
 * run covers is a hole. The inode holds CFS_DIRECT runs and the head of
 * a chain of CFS_KIND_EXTENTS blocks (253 runs each) for the rest.
 */

static int data_write_block(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, const void *buf,
                            enum cfs_alloc_class cls);


/* Gather the inode's runs into a kmalloc'd array (the caller frees). */
static int extents_load(struct cfs *fs, const struct cfs_inode *in, struct cfs_extent **out, unsigned *n)
{
    unsigned cap = CFS_DIRECT + CFS_EXTENTS_PER_BLOCK, k = 0;
    struct cfs_extent *ext = kmalloc(cap * sizeof(*ext), 0);
    if (ext == NULL)
        return -ENOMEM;
    for (unsigned i = 0; i < CFS_DIRECT && cfs_ext_count(&in->direct[i]); i++)
        ext[k++] = in->direct[i];
    uint64_t next = in->indirect;
    unsigned chain = 0;
    while (next) {
        if (++chain > CFS_MAX_EXTENTS / CFS_EXTENTS_PER_BLOCK + 1 || !cfs_dva_valid(fs, next)) {
            kfree(ext);
            return -EIO;   /* a cycle or a wild pointer */
        }
        struct cfs_buf *b;
        int rc = cfs_buf_get(fs, next, CFS_KIND_EXTENTS, &b);
        if (rc) {
            kfree(ext);
            return rc;
        }
        const struct cfs_extent_block *eb = (const struct cfs_extent_block *)(b->data + CFS_MHDR_SIZE);
        for (unsigned i = 0; i < CFS_EXTENTS_PER_BLOCK && cfs_ext_count(&eb->ext[i]); i++) {
            if (k == cap) {
                if (cap >= cfs_max_extents(fs)) {
                    cfs_buf_put(fs, b);
                    kfree(ext);
                    return -EFBIG;
                }
                unsigned ncap = cap * 2 > cfs_max_extents(fs) ? cfs_max_extents(fs) : cap * 2;
                struct cfs_extent *grown = krealloc(ext, ncap * sizeof(*ext), 0);
                if (grown == NULL) {
                    cfs_buf_put(fs, b);
                    kfree(ext);
                    return -ENOMEM;
                }
                ext = grown;
                cap = ncap;
            }
            ext[k++] = eb->ext[i];
        }
        next = eb->next;
        cfs_buf_put(fs, b);
    }
    if (!cfs_extents_valid(fs, ext, k)) {
        kfree(ext);
        return -EIO;   /* out of range, unsorted or overlapping */
    }
    *out = ext;
    *n = k;
    return 0;
}

/* Store runs back: the first CFS_DIRECT in the inode, the rest in the
 * chain (existing blocks copy-on-write, new ones allocated, surplus
 * blocks freed).
 *
 * In two halves (CF17; CFS-6). The first does everything that can fail
 * -- reads the old chain, sets aside every block the copies and the new
 * blocks need -- and changes nothing, so a failure there is an ordinary
 * error the caller can retry: set_extent frees nothing it displaced, and
 * data_write_block puts its checksum back. The second rewrites the
 * inode's runs and the chain and cannot fail. It used to be one loop
 * that abandoned the transaction when the chain's next block could not
 * be had, half the map new and half old. */
struct es_scratch {
    struct cfs_prep prep;
    struct cfs_buf *keep[CFS_PREP_MAX];   /* the old chain's blocks the new one reuses, held */
    uint64_t drop[CFS_PREP_MAX];          /* and the rest of it, freed once the new map is in */
};

static int extents_store(struct cfs *fs, struct cfs_inode *in, const struct cfs_extent *ext, unsigned n)
{
    if (n > cfs_max_extents(fs))
        return -EFBIG;
    unsigned left = n > CFS_DIRECT ? n - CFS_DIRECT : 0;
    unsigned need = (left + CFS_EXTENTS_PER_BLOCK - 1) / CFS_EXTENTS_PER_BLOCK;
    struct es_scratch *w = kzalloc(sizeof(*w));
    if (w == NULL)
        return -ENOMEM;

    /* The first half: read and set aside, change nothing. */
    unsigned nkeep = 0, ndrop = 0, cows = 0, guard = 0;
    uint64_t at = in->indirect;
    int rc = 0;
    while (at != 0 && rc == 0) {
        if (++guard > CFS_PREP_MAX) {
            rc = -EIO;   /* a cycle: extents_load refuses one too */
            break;
        }
        struct cfs_buf *b;
        rc = cfs_buf_get(fs, at, CFS_KIND_EXTENTS, &b);
        if (rc)
            break;
        uint64_t next = ((const struct cfs_extent_block *)(b->data + CFS_MHDR_SIZE))->next;
        if (nkeep < need) {
            w->keep[nkeep++] = b;
            if (cfs_buf_hdr(b)->generation != fs->gen)
                cows++;
        } else {
            w->drop[ndrop++] = at;
            cfs_buf_put(fs, b);
        }
        at = next;
    }
    /* The test seam (cosmofs-nomem): the chain block cannot be had. */
    if (rc == 0 && (cows != 0 || need > nkeep) && faultinject_should_fail(FI_COSMOFS_CHAIN_NOMEM))
        rc = -ENOMEM;
    if (rc == 0)
        rc = cfs_prep_meta(fs, &w->prep, cows + (need - nkeep));
    if (rc) {
        for (unsigned i = 0; i < nkeep; i++)
            cfs_buf_put(fs, w->keep[i]);
        kfree(w);
        return rc;
    }

    /* The second half: nothing below can fail. */
    memset(in->direct, 0, sizeof(in->direct));
    for (unsigned i = 0; i < n && i < CFS_DIRECT; i++)
        in->direct[i] = ext[i];
    const struct cfs_extent *rest = ext + CFS_DIRECT;
    uint64_t *slot = &in->indirect;   /* the pointer to the block we are about to fill */
    for (unsigned i = 0; i < need; i++) {
        struct cfs_buf *b;
        if (i < nkeep) {
            b = w->keep[i];
            cfs_buf_cow_prep(fs, &b, slot, &w->prep);
        } else {
            b = cfs_buf_new_prep(fs, CFS_KIND_EXTENTS, &w->prep);
            *slot = b->blkno;
        }
        struct cfs_extent_block *eb = (struct cfs_extent_block *)(b->data + CFS_MHDR_SIZE);
        unsigned take = left < CFS_EXTENTS_PER_BLOCK ? left : CFS_EXTENTS_PER_BLOCK;
        memset(eb->ext, 0, sizeof(eb->ext));
        memcpy(eb->ext, rest, take * sizeof(*rest));
        rest += take;
        left -= take;
        if (i + 1 == need)
            eb->next = 0;   /* the rest of an old chain is retired below */
        slot = &eb->next;   /* stays valid: the buffer is cached and dirty for this transaction */
        cfs_buf_put(fs, b);
    }
    if (need == 0)
        in->indirect = 0;
    for (unsigned i = 0; i < ndrop; i++)
        cfs_free_block_deferred(fs, w->drop[i]);
    cfs_prep_release(fs, &w->prep);   /* nothing, unless a kept block needed no copy after all */
    kfree(w);
    __atomic_store_n(&fs->map_stores, fs->map_stores + 1, __ATOMIC_RELEASE);
    fs->last_store_in = in;
    fs->last_store_n = n;
    return 0;
}

/* The runs an inode's map holds, counted without gathering them: the
 * direct slots in use and each chain block's, bounded as extents_load
 * bounds the chain. Under fs->lock. */
static int extents_count(struct cfs *fs, const struct cfs_inode *in, unsigned *out)
{
    unsigned k = 0;
    for (unsigned i = 0; i < CFS_DIRECT && cfs_ext_count(&in->direct[i]); i++)
        k++;
    uint64_t next = in->indirect;
    unsigned chain = 0;
    while (next) {
        if (++chain > CFS_MAX_EXTENTS / CFS_EXTENTS_PER_BLOCK + 1 || !cfs_dva_valid(fs, next))
            return -EIO;   /* a cycle or a wild pointer, as extents_load says */
        struct cfs_buf *b;
        int rc = cfs_buf_get(fs, next, CFS_KIND_EXTENTS, &b);
        if (rc)
            return rc;
        const struct cfs_extent_block *eb = (const struct cfs_extent_block *)(b->data + CFS_MHDR_SIZE);
        for (unsigned i = 0; i < CFS_EXTENTS_PER_BLOCK && cfs_ext_count(&eb->ext[i]); i++)
            k++;
        next = eb->next;
        cfs_buf_put(fs, b);
    }
    *out = k;
    return 0;
}


int cfs_map(struct cfs *fs, const struct cfs_inode *in, uint64_t lblk, uint64_t *pblk)
{
    /* The direct runs answer most lookups without loading the chain; they
     * get the same checks the chain gets, so a crafted inode cannot map a
     * block of another file or turn data into a hole (Greptile, PR #22). */
    if (!cfs_direct_valid(fs, in))
        return -EIO;
    if (cfs_map_block(in->direct, CFS_DIRECT, lblk, pblk) == 1)
        return 1;
    if (in->indirect == 0)
        return 0;
    struct cfs_extent *ext;
    unsigned n;
    int rc = extents_load(fs, in, &ext, &n);
    if (rc)
        return rc;
    rc = cfs_map_block(ext, n, lblk, pblk);
    kfree(ext);
    return rc;
}

int cfs_map_ext(struct cfs *fs, const struct cfs_inode *in, uint64_t lblk, struct cfs_extent *out)
{
    if (!cfs_direct_valid(fs, in))
        return -EIO;
    for (unsigned i = 0; i < CFS_DIRECT; i++) {
        const struct cfs_extent *e = &in->direct[i];
        if (cfs_ext_count(e) && lblk >= e->lblk && lblk < (uint64_t)e->lblk + cfs_ext_count(e)) {
            *out = *e;
            return 1;
        }
    }
    if (in->indirect == 0)
        return 0;
    struct cfs_extent *ext;
    unsigned n;
    int rc = extents_load(fs, in, &ext, &n);
    if (rc)
        return rc;
    rc = 0;
    for (unsigned i = 0; i < n; i++) {
        if (lblk >= ext[i].lblk && lblk < (uint64_t)ext[i].lblk + cfs_ext_count(&ext[i])) {
            *out = ext[i];
            rc = 1;
            break;
        }
        if (ext[i].lblk > lblk)
            break;
    }
    kfree(ext);
    return rc;
}

/* Merge runs that are adjacent both logically and physically, in place.
 * A compressed record is one object -- its blocks mean nothing apart
 * from each other -- so it neither merges nor is merged into. */
static unsigned extents_merge(struct cfs_extent *ext, unsigned n)
{
    unsigned w = 0;
    for (unsigned i = 0; i < n; i++) {
        if (cfs_ext_count(&ext[i]) == 0)
            continue;
        if (w > 0 && !cfs_ext_compressed(&ext[w - 1]) && !cfs_ext_compressed(&ext[i]) &&
            ext[w - 1].start + ext[w - 1].count == ext[i].start &&
            (uint64_t)ext[w - 1].lblk + ext[w - 1].count == ext[i].lblk &&
            (uint64_t)ext[w - 1].count + ext[i].count < 0x7fffffffu) {
            ext[w - 1].count += ext[i].count;
        } else {
            ext[w++] = ext[i];
        }
    }
    return w;
}

/* The test seam at the top of set_extent (cosmofs-extent-fail): an armed
 * rule makes it fail as it can before it changes anything, -ENOMEM (an
 * allocation) or -EFBIG (a map that would not fit). One load per kind
 * when nothing is armed; nothing in a release build. */
static int faultinject_set_extent(void)
{
    if (faultinject_should_fail(FI_COSMOFS_EXTENT_NOMEM))
        return -ENOMEM;
    if (faultinject_should_fail(FI_COSMOFS_EXTENT_FBIG))
        return -EFBIG;
    return 0;
}

/*
 * Put `ne` into the map, displacing whatever covered its logical range.
 * A plain run that overlaps is split; a compressed record that overlaps
 * is displaced whole and its blocks freed, because a record is one
 * object and half of one is nothing. Records are aligned to
 * CFS_RECORD_BLOCKS and so is every range written over them, so a record
 * that hangs out of the range is a map this code did not write: it is
 * refused rather than half-freed.
 *
 * Blocks that stop being referenced are freed here, except for the one
 * block a single-block write replaces, which the caller frees after its
 * own bookkeeping (that is what `old` is for).
 */
static int set_extent(struct cfs *fs, struct cfs_inode *in, struct cfs_extent ne, uint64_t *old)
{
    int rc = faultinject_set_extent();
    if (rc) {
#if CONFIG_DEBUG
        fs->fbig_injected = rc == -EFBIG;
#endif
        return rc;
    }
    uint64_t lblk = ne.lblk, count = cfs_ext_count(&ne);
    if (lblk + count > 0x100000000ull)
        return -EFBIG;
    struct cfs_extent *ext;
    unsigned n;
    rc = extents_load(fs, in, &ext, &n);
    if (rc)
        return rc;
    if (old)
        *old = 0;
    struct cfs_extent *out = kmalloc((n + 2) * sizeof(*out), 0);
    /* Displaced blocks are noted here and released only once the new map
     * is stored. Releasing them as they are found would hand them to the
     * next commit while the inode still pointed at them, if storing the
     * map then failed -- and a block freed while something references it
     * is the one mistake this filesystem must never make. A range of
     * `count` logical blocks displaces at most `count` physical ones: a
     * plain run uses one each, and a record uses fewer than it covers. */
    uint64_t *freed = kmalloc((size_t)count * sizeof(*freed), 0);
    unsigned nfreed = 0;
    if (out == NULL || freed == NULL) {
        kfree(out);
        kfree(freed);
        kfree(ext);
        return -ENOMEM;
    }
    unsigned m = 0;
    bool placed = false;
    for (unsigned i = 0; i < n; i++) {
        struct cfs_extent e = ext[i];
        uint64_t ecount = cfs_ext_count(&e), eend = e.lblk + ecount, end = lblk + count;
        if (!placed && lblk < e.lblk) {
            out[m++] = ne;
            placed = true;
        }
        if (eend <= lblk || e.lblk >= end) {
            out[m++] = e;   /* no overlap */
            continue;
        }
        if (cfs_ext_compressed(&e)) {
            if (e.lblk < lblk || eend > end) {
                rc = -EIO;   /* a record straddling the range: not ours to cut */
                goto out;
            }
            for (uint32_t k = 0; k < cfs_ext_psize(&e) && nfreed < count; k++)
                freed[nfreed++] = e.start + k;
            continue;   /* displaced whole */
        }
        /* In logical order: what of this run lies before the new
         * extent, the new extent itself, then what lies after. */
        if (e.lblk < lblk)
            out[m++] = (struct cfs_extent){ e.start, (uint32_t)(lblk - e.lblk), e.lblk };
        if (!placed) {
            out[m++] = ne;
            placed = true;
        }
        uint64_t from = lblk > e.lblk ? lblk : e.lblk;
        uint64_t to = end < eend ? end : eend;
        if (count == 1 && old && from == lblk)
            *old = e.start + (lblk - e.lblk);   /* the caller frees this one */
        else
            for (uint64_t k = from; k < to && nfreed < count; k++)
                freed[nfreed++] = e.start + (k - e.lblk);
        if (eend > end)
            out[m++] = (struct cfs_extent){ e.start + (end - e.lblk), (uint32_t)(eend - end), (uint32_t)end };
    }
    if (!placed)
        out[m++] = ne;   /* beyond every run */
    m = extents_merge(out, m);
    rc = extents_store(fs, in, out, m);
    if (rc == 0)
        for (unsigned i = 0; i < nfreed; i++)
            cfs_free_block_deferred(fs, freed[i]);
out:
    kfree(freed);
    kfree(out);
    kfree(ext);
    return rc;
}

/* Map logical block `lblk` to `pblk`, replacing a previous mapping (its
 * block is returned in *old) or filling a hole. */
int cfs_set_block(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, uint64_t pblk, uint64_t *old)
{
    struct cfs_extent ne = { pblk, 1, (uint32_t)lblk };
    return set_extent(fs, in, ne, old);
}

/*
 * Remove every mapping of [lblk, lblk+count) and free what it named. A
 * plain run is split; a compressed record inside the range goes whole,
 * because half a record is nothing. Unlike set_extent this puts nothing
 * back: it is how a record stops existing before its surviving blocks
 * are written again one at a time.
 */
static int drop_range(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, uint64_t count)
{
    struct cfs_extent *ext;
    unsigned n;
    int rc = extents_load(fs, in, &ext, &n);
    if (rc)
        return rc;
    struct cfs_extent *out = kmalloc((n + 2) * sizeof(*out), 0);
    uint64_t *freed = kmalloc((size_t)count * sizeof(*freed), 0);
    unsigned nfreed = 0;
    if (out == NULL || freed == NULL) {
        kfree(out);
        kfree(freed);
        kfree(ext);
        return -ENOMEM;
    }
    uint64_t end = lblk + count;
    unsigned m = 0;
    for (unsigned i = 0; i < n; i++) {
        struct cfs_extent e = ext[i];
        uint64_t ecount = cfs_ext_count(&e), eend = e.lblk + ecount;
        if (eend <= lblk || e.lblk >= end) {
            out[m++] = e;
            continue;
        }
        if (cfs_ext_compressed(&e)) {
            if (e.lblk < lblk || eend > end) {
                rc = -EIO;   /* a record hanging out of the range: not ours to cut */
                goto out;
            }
            for (uint32_t k = 0; k < cfs_ext_psize(&e) && nfreed < count; k++)
                freed[nfreed++] = e.start + k;
            continue;
        }
        if (e.lblk < lblk)
            out[m++] = (struct cfs_extent){ e.start, (uint32_t)(lblk - e.lblk), e.lblk };
        uint64_t from = lblk > e.lblk ? lblk : e.lblk, to = end < eend ? end : eend;
        for (uint64_t k = from; k < to && nfreed < count; k++)
            freed[nfreed++] = e.start + (k - e.lblk);
        if (eend > end)
            out[m++] = (struct cfs_extent){ e.start + (end - e.lblk), (uint32_t)(eend - end), (uint32_t)end };
    }
    m = extents_merge(out, m);
    rc = extents_store(fs, in, out, m);
    if (rc == 0)
        for (unsigned i = 0; i < nfreed; i++)
            cfs_free_block_deferred(fs, freed[i]);
out:
    kfree(freed);
    kfree(out);
    kfree(ext);
    return rc;
}

/*
 * A compressed record that straddles the new end of the file cannot be
 * cut: it is one object. It is read, dropped whole, and the part that
 * survives is written back as ordinary blocks -- which is what makes the
 * blocks past the end unreachable rather than merely unread. Doing it
 * the other way round, writing over the record block by block, would ask
 * set_extent to cut the record it is standing on.
 */
static int truncate_record(struct cfs *fs, struct cfs_inode *in, uint64_t keep)
{
    struct cfs_extent e;
    int rc = cfs_map_ext(fs, in, keep, &e);
    if (rc != 1 || !cfs_ext_compressed(&e) || e.lblk >= keep)
        return rc < 0 ? rc : 0;
    uint32_t count = cfs_ext_count(&e);
    uint8_t *rec = kmalloc((size_t)count * CFS_BLOCK, 0);
    if (rec == NULL)
        return -ENOMEM;
    rc = cfs_record_read(fs, in, &e, rec);
    if (rc == 0)
        rc = drop_range(fs, in, e.lblk, count);
    if (rc == 0) {
        unsigned survives = (unsigned)(keep - e.lblk);
        for (unsigned i = 0; i < survives && rc == 0; i++)
            rc = data_write_block(fs, in, e.lblk + i, rec + (size_t)i * CFS_BLOCK, CFS_ALLOC_DATA);
        if (rc)
            cfs_fail(fs, rc);   /* as in record_write: the dropped record's survivors would read as holes */
    }
    kfree(rec);
    return rc;
}

/* Keep logical blocks below `keep`; free the rest. */
int cfs_truncate_blocks(struct cfs *fs, struct cfs_inode *in, uint64_t keep)
{
    int rc = keep > 0 ? truncate_record(fs, in, keep) : 0;
    if (rc)
        return rc;
    struct cfs_extent *ext;
    unsigned n;
    rc = extents_load(fs, in, &ext, &n);
    if (rc)
        return rc;
    /* The shortened map in `out`, the old one kept in `ext`: the blocks
     * past the end are released only once the new map is stored, as
     * set_extent and drop_range release theirs. extents_store can fail
     * as an ordinary error since CFS-6, and frees queued before it would
     * have a later commit release blocks the old map still names (Codex
     * on #433). */
    struct cfs_extent *out = kmalloc((n ? n : 1) * sizeof(*out), 0);
    if (out == NULL) {
        kfree(ext);
        return -ENOMEM;
    }
    unsigned m = 0;
    for (unsigned i = 0; i < n; i++) {
        struct cfs_extent e = ext[i];
        uint32_t count = cfs_ext_count(&e);
        if (e.lblk >= keep)
            continue;
        if ((uint64_t)e.lblk + count > keep)
            e.count = (uint32_t)(keep - e.lblk);   /* truncate_record left only a plain run straddling the end */
        out[m++] = e;
    }
    rc = extents_store(fs, in, out, m);
    for (unsigned i = 0; rc == 0 && i < n; i++) {
        const struct cfs_extent *e = &ext[i];
        uint32_t count = cfs_ext_count(e), psize = cfs_ext_psize(e);
        if (e->lblk >= keep) {
            for (uint32_t k = 0; k < psize; k++)
                cfs_free_block_deferred(fs, e->start + k);
        } else if ((uint64_t)e->lblk + count > keep) {
            for (uint32_t k = (uint32_t)(keep - e->lblk); k < count; k++)
                cfs_free_block_deferred(fs, e->start + k);
        }
    }
    kfree(out);
    kfree(ext);
    if (rc == 0 && keep == 0)
        cfs_csum_free(fs, in);
    return rc;
}

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

/* Read directory block `lblk` into buf (zeros for a hole). */
/* By inode: the VFS path has a vnode, the structural check has only the
 * inode it read out of the map, and both want the same bytes. */
int cfs_dir_read_block_at(struct cfs *fs, const struct cfs_inode *dir, uint64_t lblk, uint8_t *buf)
{
    uint64_t pblk = 0;
    int rc = cfs_map(fs, dir, lblk, &pblk);
    if (rc < 0)
        return rc;
    if (rc == 0) {
        memset(buf, 0, CFS_BLOCK);
        return 0;
    }
    /* The verified read takes a mutable inode because it may repair a
     * checksum entry; a caller that owns only a copy gives it the copy,
     * which is what the structural check has. */
    struct cfs_inode tmp = *dir;
    return cfs_data_read_verified(fs, &tmp, lblk, pblk, buf);
}

static int dir_read_block(struct cfs *fs, struct vnode *dir, uint64_t lblk, uint8_t *buf)
{
    return cfs_dir_read_block_at(fs, cfs_inode_of(dir), lblk, buf);
}

/* By inode, for the test hook that manufactures a bad entry: writes the
 * block and the inode that names it, in the open transaction. */
int cfs_dir_write_block_at(struct cfs *fs, struct cfs_inode *dir, uint64_t lblk, const uint8_t *buf)
{
    int rc = data_write_block(fs, dir, lblk, buf, CFS_ALLOC_META);
    if (rc)
        return rc;
    return cfs_inode_write(fs, dir->ino, dir);
}

/* The block after the file's previous logical block, when it is mapped:
 * the allocation hint that keeps a sequentially written file in one run. */
static uint64_t next_block_hint(struct cfs *fs, const struct cfs_inode *in, uint64_t lblk)
{
    uint64_t prev;
    if (lblk > 0 && cfs_map(fs, in, lblk - 1, &prev) == 1)
        return prev + 1;
    return 0;
}

/* Write a data block copy-on-write: a new pool block near the previous
 * one (file data from the data class, directory blocks from the metadata
 * class so a deletion on a full disk still has a block to write), its
 * checksum recorded, the mapping replaced. */
/*
 * Encrypt a block into `out` and record what a reader needs: the tag
 * that authenticates it, the generation that is half its nonce, and a
 * keyless CRC of the ciphertext for whoever has no key.
 */
static int seal_block(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, const void *plain, void *out,
                      struct cfs_csum_aead *e)
{
    if (!fs->have_key)
        return -ENOKEY;
    memcpy(out, plain, CFS_BLOCK);
    uint8_t key[CHACHA20_KEY_SIZE];
    cfs_file_key(fs, in->ino, key);
    memset(e, 0, sizeof(*e));
    cfs_block_nonce(e->nonce);
    /* The position is authenticated with the block, so a valid block
     * cannot be moved to another offset and opened there. */
    struct cfs_block_aad aad = { .ino = in->ino, .lblk = lblk };
    chacha20_seal(key, e->nonce, &aad, sizeof(aad), out, CFS_BLOCK, e->tag);
    memset(key, 0, sizeof(key));
    e->crc = crc32c(out, CFS_BLOCK);
    return 0;
}

/*
 * A checksum entry a write is about to replace (CF16). A block's entry is
 * stored before the mapping that names the block, so when the mapping
 * fails the old block is still mapped and must get its own entry back.
 * An entry never stored (-ENOENT) has nothing to put back: no block was
 * mapped there, since a mapped block without one is a damaged tree.
 */
struct csum_saved {
    bool have, aead;
    uint32_t crc;
    struct cfs_csum_aead entry;
};

static int csum_save(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, bool aead, struct csum_saved *s)
{
    int rc = aead ? cfs_aead_get(fs, in, lblk, &s->entry) : cfs_csum_get(fs, in, lblk, &s->crc);
    s->have = rc == 0;
    s->aead = aead;
    return rc == -ENOENT ? 0 : rc;
}

/* Put a saved entry back. The put lands in a CSUM block this transaction
 * already copied; if it fails anyway, a mapped block is left under a
 * checksum that is not its own, which no commit may publish, so the
 * transaction is abandoned. */
static void csum_restore(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, const struct csum_saved *s)
{
    if (!s->have)
        return;
    int rc = s->aead ? cfs_aead_put(fs, in, lblk, &s->entry) : cfs_csum_put(fs, in, lblk, s->crc);
    if (rc)
        cfs_fail(fs, rc);
}

static int data_write_block(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, const void *buf,
                            enum cfs_alloc_class cls)
{
    uint8_t *cipher = NULL;
    struct cfs_csum_aead entry;
    if (in->csum_algo == CFS_CSUM_POLY1305) {
        cipher = kmalloc(CFS_BLOCK, 0);
        if (cipher == NULL)
            return -ENOMEM;
        int crc = seal_block(fs, in, lblk, buf, cipher, &entry);
        if (crc) {
            kfree(cipher);
            return crc;
        }
        buf = cipher;
    }
    uint64_t nblk, got, old;
    int rc = cfs_alloc_run(fs, cls, next_block_hint(fs, in, lblk), 1, &nblk, &got);
    if (rc) {
        kfree(cipher);
        return rc;
    }
    /* The entry is stored before the mapping (the mapping is what makes
     * the block readable), so the one it replaces is kept: a mapping that
     * fails leaves the old block mapped, under its own checksum (CF16). */
    struct csum_saved prev;
    rc = cfs_data_write(fs, nblk, buf);
    if (rc == 0)
        rc = csum_save(fs, in, lblk, cipher != NULL, &prev);
    if (rc == 0)
        rc = cipher ? cfs_aead_put(fs, in, lblk, &entry) : cfs_csum_put(fs, in, lblk, crc32c(buf, CFS_BLOCK));
    kfree(cipher);
    if (rc == 0) {
        rc = cfs_set_block(fs, in, lblk, nblk, &old);
        if (rc)
            csum_restore(fs, in, lblk, &prev);
    }
    if (rc) {
        cfs_free_block_deferred(fs, nblk);
        return rc;
    }
    if (old)
        cfs_free_block_deferred(fs, old);
    return 0;
}

/*
 * A record's blocks as one plain run: `n` consecutive blocks allocated
 * together, written and checksummed as data_write_block does each one,
 * and mapped by one set_extent, which merges the run into the one before
 * it when they are adjacent on the disk. -ENOSPC when no run of `n` is
 * free next to the hint, which leaves everything as it was and the
 * caller to write the blocks one at a time.
 */
static int plain_run_write(struct cfs *fs, struct cfs_inode *in, uint64_t lblk0, const uint8_t *data, unsigned n)
{
    uint64_t start, got;
    int rc = cfs_alloc_run(fs, CFS_ALLOC_DATA, next_block_hint(fs, in, lblk0), n, &start, &got);
    if (rc)
        return rc;
    if (got < n) {
        for (uint64_t k = 0; k < got; k++)
            cfs_free_block_deferred(fs, start + k);
        return -ENOSPC;
    }
    uint8_t *cipher = NULL;
    if (in->csum_algo == CFS_CSUM_POLY1305) {
        cipher = kmalloc(CFS_BLOCK, 0);
        if (cipher == NULL)
            rc = -ENOMEM;
    }
    /* As record_write's: the entries the range holds now are the map's
     * until set_extent succeeds, kept and put back on a failure (CF16). */
    struct csum_saved prev[CFS_RECORD_BLOCKS];
    uint32_t saved = 0;
    for (; saved < n && rc == 0; saved++)
        rc = csum_save(fs, in, lblk0 + saved, cipher != NULL, &prev[saved]);
    for (unsigned i = 0; i < n && rc == 0; i++) {
        const uint8_t *blk = data + (size_t)i * CFS_BLOCK;
        if (cipher) {
            struct cfs_csum_aead entry;
            rc = seal_block(fs, in, lblk0 + i, blk, cipher, &entry);
            if (rc == 0)
                rc = cfs_data_write(fs, start + i, cipher);
            if (rc == 0)
                rc = cfs_aead_put(fs, in, lblk0 + i, &entry);
            continue;
        }
        rc = cfs_data_write(fs, start + i, blk);
        if (rc == 0)
            rc = cfs_csum_put(fs, in, lblk0 + i, crc32c(blk, CFS_BLOCK));
    }
    kfree(cipher);
    if (rc == 0) {
        struct cfs_extent ne = { start, n, (uint32_t)lblk0 };
        rc = set_extent(fs, in, ne, NULL);
    }
    if (rc) {
        for (uint32_t i = 0; i < saved; i++)
            csum_restore(fs, in, lblk0 + i, &prev[i]);
        for (unsigned i = 0; i < n; i++)
            cfs_free_block_deferred(fs, start + i);
    }
    return rc;
}

/*
 * Write an aligned record's worth of logical blocks, compressed if
 * `compress` and that makes it strictly smaller in whole blocks. Anything
 * else -- it does not compress, the algorithm is off, the write policy
 * says plain (cfs_writepages), there is no room for a contiguous run -- is
 * written as ordinary blocks, which is always allowed and never wrong:
 * one run when the policy chose it and a run of `n` is free, else a block
 * at a time.
 */
static int record_write(struct cfs *fs, struct cfs_inode *in, uint64_t lblk0, const uint8_t *data, unsigned n,
                        bool compress)
{
    if (!compress && n == CFS_RECORD_BLOCKS && lblk0 % CFS_RECORD_BLOCKS == 0) {
        int rc = plain_run_write(fs, in, lblk0, data, n);
        if (rc != -ENOSPC)
            return rc;
    }
    /* And the format again, for a file an older kernel of this family
     * created with the flag on a pool below version 6 (CFS-2). */
    if (compress && n > 1 && in->compress_algo == CFS_COMPRESS_LZ4 && fs->sb.version >= 6) {
        /* Room for one block less than it takes now: a record that
         * compresses to the same number of blocks has cost work and
         * saved nothing, and would have to be decompressed on every
         * read for ever after. */
        size_t cap = (size_t)(n - 1) * CFS_BLOCK;
        uint8_t *packed = kmalloc(cap, KMEM_ZERO);
        void *work = kmalloc(LZ4_WORK_BYTES, 0);   /* not the stack: see lz4.h */
        if (packed == NULL || work == NULL) {
            kfree(packed);
            kfree(work);
            return -ENOMEM;
        }
        size_t clen = lz4_compress(data, (size_t)n * CFS_BLOCK, packed, cap, work);
        kfree(work);
        if (clen > 0) {
            uint32_t psize = (uint32_t)((clen + CFS_BLOCK - 1) / CFS_BLOCK);
            uint64_t start, got;
            int rc = cfs_alloc_run(fs, CFS_ALLOC_DATA, next_block_hint(fs, in, lblk0), psize, &start, &got);
            if (rc == 0 && got < psize) {
                /* Not contiguous enough for a record; give it back. */
                for (uint64_t k = 0; k < got; k++)
                    cfs_free_block_deferred(fs, start + k);
                rc = -ENOSPC;
            }
            if (rc == 0) {
                /* The tail of the last block is whatever kzalloc left:
                 * zeros, so the record's blocks are deterministic and a
                 * repeated write of the same data gives the same bytes. */
                /* Compress, then encrypt: the other order would leave
                 * nothing worth compressing. Each physical block of the
                 * record is sealed on its own, so a mirror repairs one
                 * without the key and a reader authenticates it with
                 * one. */
                uint8_t *cipher = NULL;
                if (in->csum_algo == CFS_CSUM_POLY1305) {
                    cipher = kmalloc(CFS_BLOCK, 0);
                    if (cipher == NULL)
                        rc = -ENOMEM;
                }
                /* The record's entries replace the ones its range holds
                 * now, which are still the map's until set_extent below
                 * succeeds: kept, and put back on any failure (CF16). */
                struct csum_saved prev[CFS_RECORD_BLOCKS];
                uint32_t saved = 0;   /* csum_save marks a failed save as nothing to restore */
                for (; saved < psize && rc == 0; saved++)
                    rc = csum_save(fs, in, lblk0 + saved, cipher != NULL, &prev[saved]);
                for (uint32_t i = 0; i < psize && rc == 0; i++) {
                    const uint8_t *blk = packed + (size_t)i * CFS_BLOCK;
                    if (cipher) {
                        struct cfs_csum_aead entry;
                        rc = seal_block(fs, in, lblk0 + i, blk, cipher, &entry);
                        if (rc == 0)
                            rc = cfs_data_write(fs, start + i, cipher);
                        if (rc == 0)
                            rc = cfs_aead_put(fs, in, lblk0 + i, &entry);
                        continue;
                    }
                    rc = cfs_data_write(fs, start + i, blk);
                    if (rc == 0)
                        rc = cfs_csum_put(fs, in, lblk0 + i, crc32c(blk, CFS_BLOCK));
                }
                kfree(cipher);
                if (rc == 0) {
                    struct cfs_extent ne = { start, cfs_ext_pack(CFS_COMPRESS_LZ4, psize, n), (uint32_t)lblk0 };
                    rc = set_extent(fs, in, ne, NULL);
                }
                if (rc) {
                    for (uint32_t i = 0; i < saved; i++)
                        csum_restore(fs, in, lblk0 + i, &prev[i]);
                    for (uint32_t i = 0; i < psize; i++)
                        cfs_free_block_deferred(fs, start + i);
                } else {
                    kfree(packed);
                    return 0;
                }
            }
            if (rc != -ENOSPC) {
                kfree(packed);
                return rc;   /* a real failure, not "it did not fit" */
            }
        }
        kfree(packed);
    }
    /* Plain blocks. A record covering this range goes first, whole: the
     * first block alone cannot displace it -- set_extent refuses a
     * one-block range that would cut a record, and data_write_block then
     * left that block's checksum over the record's (until CF16), leaving the
     * record unreadable (found in shell completeness S7: a record written
     * while its last page was part empty, rewritten once that page was
     * filled and no longer compressed). Records are aligned and so is
     * this range, so a record here starts at lblk0. */
    struct cfs_extent cur;
    int mrc = cfs_map_ext(fs, in, lblk0, &cur);
    if (mrc < 0)
        return mrc;
    bool dropped = false;
    if (mrc == 1 && cfs_ext_compressed(&cur)) {
        int rc = drop_range(fs, in, cur.lblk, cfs_ext_count(&cur));
        if (rc)
            return rc;
        dropped = true;
    }
    for (unsigned i = 0; i < n; i++) {
        int rc = data_write_block(fs, in, lblk0 + i, data + (size_t)i * CFS_BLOCK, CFS_ALLOC_DATA);
        if (rc) {
            /* The record is out of the map and its blocks are on the
             * deferred list: what is not yet rewritten would read back as
             * a hole. The committed record is the only good copy, so the
             * transaction is abandoned rather than publish the holes. */
            if (dropped)
                cfs_fail(fs, rc);
            return rc;
        }
    }
    return 0;
}

/* Write directory block `lblk` copy-on-write and update the inode. */
static int dir_write_block(struct cfs *fs, struct vnode *dir, uint64_t lblk, const uint8_t *buf)
{
    int rc = data_write_block(fs, cfs_inode_of(dir), lblk, buf, CFS_ALLOC_META);
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
    rc = data_write_block(fs, &in, 0, block, CFS_ALLOC_META);
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
            rc = record_write(fs, in, e.lblk, rec, count, true);   /* a record replaced by a record */
        }
        kfree(rec);
        return rc;
    }
    /* Holes are representable: only this block is written. */
    return data_write_block(fs, in, index, buf, CFS_ALLOC_DATA);
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
 * page's write-back is one set_extent, which adds at most two runs (the
 * run it lands in split around it); a whole record's is one set_extent
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
        rc = extents_count(fs, cfs_inode_of(vn), &n);
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
    } else if (extents_count(fs, cfs_inode_of(vn), &runs) != 0) {
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
    rc = extents_count(fs, cfs_inode_of(vn), n);
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
        rc = record_write(fs, in, index, rec, CFS_RECORD_BLOCKS, compress);
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
