/*
 * cosmofs_data.c - A file's blocks: its runs (the direct slots and the
 * extent chain), the map from a logical block to a pool block, the
 * copy-on-write write of a data or directory block with its checksum or
 * seal, compressed records, and truncation. Moved out of cosmofs.c
 * (roadmap 2.0 SA7) so that it can be held to a digest and made CeeSafe
 * on its own.
 */

#include <kernel/cosmofs.h>
#include <kernel/crc32c.h>
#include <kernel/errno.h>
#include <kernel/faultinject.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/lz4.h>
#include <kernel/panic.h>
#include <kernel/string.h>

#include "cosmofs_internal.h"

/* --- extents ------------------------------------------------------------- */

/*
 * A run maps logical blocks [lblk, lblk + count) to pool blocks
 * [start, start + count). Runs are kept sorted by lblk; a logical block no
 * run covers is a hole. The inode holds CFS_DIRECT runs and the head of
 * a chain of CFS_KIND_EXTENTS blocks (253 runs each) for the rest.
 */



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
 * error the caller can retry: cfs_set_extent frees nothing it displaced, and
 * cfs_data_write_block puts its checksum back. The second rewrites the
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
int cfs_extents_count(struct cfs *fs, const struct cfs_inode *in, unsigned *out)
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

/* The test seam at the top of cfs_set_extent (cosmofs-extent-fail): an armed
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
int cfs_set_extent(struct cfs *fs, struct cfs_inode *in, struct cfs_extent ne, uint64_t *old)
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
    return cfs_set_extent(fs, in, ne, old);
}

/*
 * Remove every mapping of [lblk, lblk+count) and free what it named. A
 * plain run is split; a compressed record inside the range goes whole,
 * because half a record is nothing. Unlike cfs_set_extent this puts nothing
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
 * cfs_set_extent to cut the record it is standing on.
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
            rc = cfs_data_write_block(fs, in, e.lblk + i, rec + (size_t)i * CFS_BLOCK, CFS_ALLOC_DATA);
        if (rc)
            cfs_fail(fs, rc);   /* as in cfs_record_write: the dropped record's survivors would read as holes */
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
     * cfs_set_extent and drop_range release theirs. extents_store can fail
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

/* By inode, for the test hook that manufactures a bad entry: writes the
 * block and the inode that names it, in the open transaction. */
int cfs_dir_write_block_at(struct cfs *fs, struct cfs_inode *dir, uint64_t lblk, const uint8_t *buf)
{
    int rc = cfs_data_write_block(fs, dir, lblk, buf, CFS_ALLOC_META);
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

int cfs_data_write_block(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, const void *buf,
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
 * together, written and checksummed as cfs_data_write_block does each one,
 * and mapped by one cfs_set_extent, which merges the run into the one before
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
    /* As cfs_record_write's: the entries the range holds now are the map's
     * until cfs_set_extent succeeds, kept and put back on a failure (CF16). */
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
        rc = cfs_set_extent(fs, in, ne, NULL);
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
int cfs_record_write(struct cfs *fs, struct cfs_inode *in, uint64_t lblk0, const uint8_t *data, unsigned n,
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
                 * now, which are still the map's until cfs_set_extent below
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
                    rc = cfs_set_extent(fs, in, ne, NULL);
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
     * first block alone cannot displace it -- cfs_set_extent refuses a
     * one-block range that would cut a record, and cfs_data_write_block then
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
        int rc = cfs_data_write_block(fs, in, lblk0 + i, data + (size_t)i * CFS_BLOCK, CFS_ALLOC_DATA);
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
