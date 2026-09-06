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
#include <kernel/cred.h>
#include <kernel/crc32c.h>
#include <kernel/page.h>
#include <kernel/lz4.h>
#include <kernel/string.h>

#include "cosmofs_internal.h"

static const struct vnode_ops cfs_dir_ops;
static const struct vnode_ops cfs_file_ops;

/* --- extents ------------------------------------------------------------- */

/*
 * A run maps logical blocks [lblk, lblk + count) to pool blocks
 * [start, start + count). Runs are kept sorted by lblk; a logical block no
 * run covers is a hole. The inode holds CFS_DIRECT runs and the head of
 * a chain of CFS_KIND_EXTENTS blocks (253 runs each) for the rest.
 */

static int data_write_block(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, const void *buf,
                            enum cfs_alloc_class cls);

static bool extent_valid(const struct cfs *fs, const struct cfs_extent *e)
{
    /* A run lies on one member (cfs_alloc_run never crosses one), so
     * both ends are checked against that member; the logical range must
     * not wrap the 32-bit lblk either. A compressed record occupies
     * fewer physical blocks than it covers logical ones, and never more
     * of either than a record can hold. */
    uint32_t count = cfs_ext_count(e), psize = cfs_ext_psize(e);
    if (count == 0 || psize == 0)
        return false;
    if (cfs_ext_compressed(e)) {
        if (count > CFS_RECORD_BLOCKS || psize > count)
            return false;
        if (cfs_ext_algo(e) != CFS_COMPRESS_LZ4)
            return false;   /* nothing else can be read back */
    }
    return cfs_dva_valid(fs, e->start) && cfs_dva_valid(fs, e->start + psize - 1) &&
           CFS_DVA_VDEV(e->start) == CFS_DVA_VDEV(e->start + psize - 1) &&
           (uint64_t)e->lblk + count <= 0x100000000ull;
}

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
                if (cap >= CFS_MAX_EXTENTS) {
                    cfs_buf_put(fs, b);
                    kfree(ext);
                    return -EFBIG;
                }
                unsigned ncap = cap * 2 > CFS_MAX_EXTENTS ? CFS_MAX_EXTENTS : cap * 2;
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
    for (unsigned i = 0; i < k; i++) {
        if (!extent_valid(fs, &ext[i]) ||
            (i > 0 && ext[i].lblk < (uint64_t)ext[i - 1].lblk + cfs_ext_count(&ext[i - 1]))) {
            kfree(ext);
            return -EIO;   /* out of range, unsorted or overlapping */
        }
    }
    *out = ext;
    *n = k;
    return 0;
}

/* Store runs back: the first CFS_DIRECT in the inode, the rest in the
 * chain (existing blocks copy-on-write, new ones allocated, surplus
 * blocks freed). */
static int extents_store(struct cfs *fs, struct cfs_inode *in, const struct cfs_extent *ext, unsigned n)
{
    if (n > CFS_MAX_EXTENTS)
        return -EFBIG;
    memset(in->direct, 0, sizeof(in->direct));
    for (unsigned i = 0; i < n && i < CFS_DIRECT; i++)
        in->direct[i] = ext[i];
    unsigned left = n > CFS_DIRECT ? n - CFS_DIRECT : 0;
    const struct cfs_extent *rest = ext + CFS_DIRECT;
    uint64_t *slot = &in->indirect;   /* the pointer to the block we are about to fill */
    while (left > 0) {
        struct cfs_buf *b;
        int rc;
        if (*slot) {
            rc = cfs_buf_get(fs, *slot, CFS_KIND_EXTENTS, &b);
            if (rc == 0)
                rc = cfs_buf_cow(fs, &b, slot);
        } else {
            rc = cfs_buf_new(fs, CFS_KIND_EXTENTS, &b);
            if (rc == 0)
                *slot = b->blkno;
        }
        if (rc)
            return rc;
        struct cfs_extent_block *eb = (struct cfs_extent_block *)(b->data + CFS_MHDR_SIZE);
        uint64_t next = eb->next;
        unsigned take = left < CFS_EXTENTS_PER_BLOCK ? left : CFS_EXTENTS_PER_BLOCK;
        memset(eb->ext, 0, sizeof(eb->ext));
        memcpy(eb->ext, rest, take * sizeof(*rest));
        rest += take;
        left -= take;
        if (left == 0) {
            /* Retire the rest of an old chain. */
            eb->next = 0;
            cfs_buf_put(fs, b);
            while (next) {
                struct cfs_buf *ob;
                if (cfs_buf_get(fs, next, CFS_KIND_EXTENTS, &ob))
                    break;
                uint64_t after = ((struct cfs_extent_block *)(ob->data + CFS_MHDR_SIZE))->next;
                cfs_buf_put(fs, ob);
                cfs_free_block_deferred(fs, next);
                next = after;
            }
            break;
        }
        slot = &eb->next;   /* stays valid: the buffer is cached and dirty for this transaction */
        cfs_buf_put(fs, b);
    }
    if (n <= CFS_DIRECT && in->indirect) {
        uint64_t next = in->indirect;
        in->indirect = 0;
        while (next) {
            struct cfs_buf *ob;
            if (cfs_buf_get(fs, next, CFS_KIND_EXTENTS, &ob))
                break;
            uint64_t after = ((struct cfs_extent_block *)(ob->data + CFS_MHDR_SIZE))->next;
            cfs_buf_put(fs, ob);
            cfs_free_block_deferred(fs, next);
            next = after;
        }
    }
    return 0;
}

/* The direct runs of an inode as read from disk: in range, sorted, no
 * overlap, no run after the first empty slot. */
static bool direct_valid(const struct cfs *fs, const struct cfs_inode *in)
{
    unsigned n = 0;
    while (n < CFS_DIRECT && cfs_ext_count(&in->direct[n]))
        n++;
    for (unsigned i = n; i < CFS_DIRECT; i++)
        if (in->direct[i].count || in->direct[i].start || in->direct[i].lblk)
            return false;   /* a run hiding behind an empty slot */
    for (unsigned i = 0; i < n; i++) {
        if (!extent_valid(fs, &in->direct[i]))
            return false;
        if (i > 0 && in->direct[i].lblk < (uint64_t)in->direct[i - 1].lblk + cfs_ext_count(&in->direct[i - 1]))
            return false;
    }
    return true;
}

int cfs_map(struct cfs *fs, const struct cfs_inode *in, uint64_t lblk, uint64_t *pblk)
{
    /* The direct runs answer most lookups without loading the chain; they
     * get the same checks the chain gets, so a crafted inode cannot map a
     * block of another file or turn data into a hole (Greptile, PR #22). */
    if (!direct_valid(fs, in))
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
    if (!direct_valid(fs, in))
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
    uint64_t lblk = ne.lblk, count = cfs_ext_count(&ne);
    if (lblk + count > 0x100000000ull)
        return -EFBIG;
    struct cfs_extent *ext;
    unsigned n;
    int rc = extents_load(fs, in, &ext, &n);
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
    unsigned m = 0;
    for (unsigned i = 0; i < n; i++) {
        struct cfs_extent e = ext[i];
        uint32_t count = cfs_ext_count(&e), psize = cfs_ext_psize(&e);
        if (e.lblk >= keep) {
            for (uint32_t k = 0; k < psize; k++)
                cfs_free_block_deferred(fs, e.start + k);
        } else if ((uint64_t)e.lblk + count > keep) {
            /* truncate_record has already replaced any record here, so
             * what straddles the end now is a plain run. */
            uint32_t keepc = (uint32_t)(keep - e.lblk);
            for (uint32_t k = keepc; k < count; k++)
                cfs_free_block_deferred(fs, e.start + k);
            e.count = keepc;
            ext[m++] = e;
        } else {
            ext[m++] = e;
        }
    }
    rc = extents_store(fs, in, ext, m);
    kfree(ext);
    if (rc == 0 && keep == 0)
        cfs_csum_free(fs, in);
    return rc;
}

/* --- checksums (docs/kernel-services/filesystem/cosmofs/design.md, version 2) --- */

/* The CSUM block for `lblk`, created (and CoW'd) when `writable`. */
/* `index` is the leaf's position in the checksum index, which differs
 * between the 4-byte CRC entries and the 32-byte authenticated ones. */
static int csum_block_at(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, unsigned index, bool writable,
                         struct cfs_buf **out)
{
    if (lblk >= CFS_CSUM_MAX_BLOCKS || index >= CFS_PTRS_PER_BLOCK)
        return -EFBIG;
    struct cfs_buf *idx, *cb;
    int rc;
    if (in->csum_root == 0) {
        if (!writable)
            return -ENOENT;
        rc = cfs_buf_new(fs, CFS_KIND_CSUMIDX, &idx);
        if (rc)
            return rc;
        in->csum_root = idx->blkno;
    } else {
        rc = cfs_buf_get(fs, in->csum_root, CFS_KIND_CSUMIDX, &idx);
        if (rc)
            return rc;
        if (writable && (rc = cfs_buf_cow(fs, &idx, &in->csum_root)) != 0) {
            cfs_buf_put(fs, idx);
            return rc;
        }
    }
    uint64_t *slot = &((uint64_t *)(idx->data + CFS_MHDR_SIZE))[index];
    if (*slot == 0) {
        if (!writable) {
            rc = -ENOENT;
            goto out;
        }
        rc = cfs_buf_new(fs, CFS_KIND_CSUM, &cb);
        if (rc)
            goto out;
        *slot = cb->blkno;
    } else {
        rc = cfs_buf_get(fs, *slot, CFS_KIND_CSUM, &cb);
        if (rc)
            goto out;
        if (writable && (rc = cfs_buf_cow(fs, &cb, slot)) != 0) {
            cfs_buf_put(fs, cb);
            goto out;
        }
    }
    *out = cb;
out:
    cfs_buf_put(fs, idx);
    return rc;
}

static int csum_block(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, bool writable, struct cfs_buf **out)
{
    return csum_block_at(fs, in, lblk, cfs_csum_index(lblk), writable, out);
}

/* The authenticated entry for a block: the tag, the generation that is
 * half its nonce, and a keyless CRC of the same ciphertext. */
int cfs_aead_put(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, const struct cfs_csum_aead *e)
{
    struct cfs_buf *cb;
    int rc = csum_block_at(fs, in, lblk, cfs_aead_index(lblk), true, &cb);
    if (rc)
        return rc;
    ((struct cfs_csum_aead *)(cb->data + CFS_MHDR_SIZE))[cfs_aead_slot(lblk)] = *e;
    cfs_buf_put(fs, cb);
    return 0;
}

int cfs_aead_get(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, struct cfs_csum_aead *e)
{
    struct cfs_buf *cb;
    int rc = csum_block_at(fs, in, lblk, cfs_aead_index(lblk), false, &cb);
    if (rc)
        return rc;
    *e = ((const struct cfs_csum_aead *)(cb->data + CFS_MHDR_SIZE))[cfs_aead_slot(lblk)];
    cfs_buf_put(fs, cb);
    return 0;
}

int cfs_csum_put(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, uint32_t crc)
{
    struct cfs_buf *cb;
    int rc = csum_block(fs, in, lblk, true, &cb);
    if (rc)
        return rc;
    ((uint32_t *)(cb->data + CFS_MHDR_SIZE))[cfs_csum_slot(lblk)] = crc;
    cfs_buf_put(fs, cb);
    return 0;
}

int cfs_csum_get(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, uint32_t *crc)
{
    struct cfs_buf *cb;
    int rc = csum_block(fs, in, lblk, false, &cb);
    if (rc)
        return rc;
    *crc = ((const uint32_t *)(cb->data + CFS_MHDR_SIZE))[cfs_csum_slot(lblk)];
    cfs_buf_put(fs, cb);
    return 0;
}

/* Verify a data or directory block just read for logical block `lblk`. */
struct data_want {
    struct cfs *fs;
    struct cfs_inode *in;
    uint64_t lblk;
    uint32_t crc;
    bool have_crc;
};

/* The verifier cfs_read_repair calls for a data or directory block: the
 * checksum its inode recorded when the block was written. An inode with
 * no checksums at all (csum_algo NONE) has nothing to compare, so the
 * first copy is the answer and a mirror cannot help it. */
static bool data_ok(const void *block, void *arg)
{
    const struct data_want *w = arg;
    if (!w->have_crc)
        return true;
    return crc32c(block, CFS_BLOCK) == w->crc;
}

/* An encrypted block is verified by the CRC of its ciphertext -- which
 * needs no key -- and then authenticated and decrypted with one. The
 * order matters: a forged block is refused before its plaintext exists. */
static bool cipher_crc_ok(const void *block, void *arg)
{
    const struct data_want *w = arg;
    return crc32c(block, CFS_BLOCK) == w->crc;
}

static int read_encrypted(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, uint64_t dva, void *buf)
{
    struct cfs_csum_aead e;
    int rc = cfs_aead_get(fs, in, lblk, &e);
    if (rc == -ENOENT)
        rc = -EIO;   /* a mapped block with no entry: the tree is damaged */
    if (rc)
        return rc;

    /* Reading the ciphertext needs no key: the CRC beside the tag is
     * what a mirror repairs against (design.md, "Integrity"). */
    struct data_want w = { .fs = fs, .in = in, .lblk = lblk, .crc = e.crc, .have_crc = true };
    rc = cfs_read_repair(fs, dva, buf, cipher_crc_ok, &w, NULL);
    if (rc) {
        kerror("cosmofs: inode %llu block %llu: no copy matches its checksum", (unsigned long long)in->ino,
               (unsigned long long)lblk);
        fs->csum_failures++;
        return rc;
    }
    if (!fs->have_key)
        return -ENOKEY;   /* the metadata mounted; the contents did not */

    uint8_t key[CHACHA20_KEY_SIZE], nonce[CHACHA20_NONCE_SIZE];
    cfs_file_key(fs, in->ino, key);
    cfs_block_nonce(lblk, e.generation, nonce);
    bool ok = chacha20_open(key, nonce, buf, CFS_BLOCK, e.tag);
    memset(key, 0, sizeof(key));
    if (!ok) {
        kerror("cosmofs: inode %llu block %llu: authentication failed", (unsigned long long)in->ino,
               (unsigned long long)lblk);
        fs->csum_failures++;
        return -EKEYREJECTED;   /* the block is not the one that was written */
    }
    return 0;
}

int cfs_data_read_verified(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, uint64_t dva, void *buf)
{
    if (in->csum_algo == CFS_CSUM_POLY1305) {
        if (!cfs_dva_valid(fs, dva))
            return -EIO;
        return read_encrypted(fs, in, lblk, dva, buf);
    }
    struct data_want w = { .fs = fs, .in = in, .lblk = lblk, .have_crc = false };
    if (in->csum_algo != CFS_CSUM_NONE) {
        int rc = cfs_csum_get(fs, in, lblk, &w.crc);
        if (rc == -ENOENT)
            rc = -EIO;   /* a mapped block without a checksum: the tree is damaged */
        if (rc)
            return rc;
        w.have_crc = true;
    }
    if (!cfs_dva_valid(fs, dva))
        return -EIO;
    int rc = cfs_read_repair(fs, dva, buf, data_ok, &w, NULL);
    if (rc && w.have_crc) {
        kerror("cosmofs: inode %llu block %llu: no copy matches its checksum", (unsigned long long)in->ino,
               (unsigned long long)lblk);
        fs->csum_failures++;
    }
    return rc;
}

/*
 * A compressed record, read and decompressed whole. Its checksums are
 * checksums of what is on the disk, so they cover its physical blocks:
 * entry i of the record's logical range is the i'th physical block. That
 * is what lets a mirrored copy be checked and repaired without
 * decompressing anything.
 */
int cfs_record_read(struct cfs *fs, struct cfs_inode *in, const struct cfs_extent *e, uint8_t *out)
{
    uint32_t psize = cfs_ext_psize(e), count = cfs_ext_count(e);
    if (count == 0 || count > CFS_RECORD_BLOCKS || psize == 0 || psize > count)
        return -EIO;
    if (cfs_ext_algo(e) != CFS_COMPRESS_LZ4)
        return -EIO;   /* an algorithm this kernel does not have */
    uint8_t *packed = kmalloc((size_t)psize * CFS_BLOCK, 0);
    if (packed == NULL)
        return -ENOMEM;
    int rc = 0;
    for (uint32_t i = 0; i < psize && rc == 0; i++)
        rc = cfs_data_read_verified(fs, in, e->lblk + i, e->start + i, packed + (size_t)i * CFS_BLOCK);
    if (rc == 0) {
        /* The compressed length is not stored: the record occupies whole
         * blocks and the codec stops when it has produced the logical
         * length, so the padding after it is never read. */
        size_t got = lz4_decompress(packed, (size_t)psize * CFS_BLOCK, out, (size_t)count * CFS_BLOCK);
        if (got != (size_t)count * CFS_BLOCK) {
            kerror("cosmofs: inode %llu record at %u: %zu bytes out of a %u-block record",
                   (unsigned long long)in->ino, e->lblk, got, count);
            rc = -EIO;
        }
    }
    kfree(packed);
    return rc;
}

int cfs_data_scrub_block(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, uint64_t dva, void *buf,
                         unsigned *repaired)
{
    if (in->csum_algo == CFS_CSUM_POLY1305) {
        /* The keyless CRC of the ciphertext: a scrub runs, and repairs,
         * on a machine that cannot decrypt any of this. */
        struct cfs_csum_aead e;
        int rc = cfs_aead_get(fs, in, lblk, &e);
        if (rc)
            return rc == -ENOENT ? -EIO : rc;
        if (!cfs_dva_valid(fs, dva))
            return -EIO;
        struct data_want w = { .fs = fs, .in = in, .lblk = lblk, .crc = e.crc, .have_crc = true };
        rc = cfs_verify_all(fs, dva, buf, cipher_crc_ok, &w, repaired);
        if (rc)
            fs->csum_failures++;
        return rc;
    }
    struct data_want w = { .fs = fs, .in = in, .lblk = lblk, .have_crc = false };
    if (in->csum_algo != CFS_CSUM_NONE) {
        int rc = cfs_csum_get(fs, in, lblk, &w.crc);
        if (rc == -ENOENT)
            rc = -EIO;
        if (rc)
            return rc;
        w.have_crc = true;
    }
    if (!cfs_dva_valid(fs, dva))
        return -EIO;
    int rc = cfs_verify_all(fs, dva, buf, data_ok, &w, repaired);
    if (rc && w.have_crc)
        fs->csum_failures++;
    return rc;
}

int cfs_csum_verify(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, const void *block)
{
    if (in->csum_algo == CFS_CSUM_NONE)
        return 0;
    uint32_t want;
    int rc = cfs_csum_get(fs, in, lblk, &want);
    if (rc == -ENOENT)
        rc = -EIO;   /* a mapped block without a checksum: the tree is damaged */
    if (rc)
        return rc;
    if (crc32c(block, CFS_BLOCK) != want) {
        kerror("cosmofs: inode %llu block %llu: data checksum mismatch", (unsigned long long)in->ino,
               (unsigned long long)lblk);
        fs->csum_failures++;
        return -EIO;
    }
    return 0;
}

/* Release the whole checksum tree (the inode is being emptied). */
void cfs_csum_free(struct cfs *fs, struct cfs_inode *in)
{
    if (in->csum_root == 0)
        return;
    struct cfs_buf *idx;
    if (cfs_buf_get(fs, in->csum_root, CFS_KIND_CSUMIDX, &idx) == 0) {
        const uint64_t *slots = (const uint64_t *)(idx->data + CFS_MHDR_SIZE);
        for (unsigned i = 0; i < CFS_PTRS_PER_BLOCK; i++)
            if (slots[i])
                cfs_free_block_deferred(fs, slots[i]);
        cfs_buf_put(fs, idx);
    }
    cfs_free_block_deferred(fs, in->csum_root);
    in->csum_root = 0;
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
    in->uid = vn->uid;
    in->gid = vn->gid;
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
    rc = cfs_data_read_verified(fs, cfs_inode_of(dir), lblk, pblk, buf);
    return rc;
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
    uint8_t key[CHACHA20_KEY_SIZE], nonce[CHACHA20_NONCE_SIZE];
    cfs_file_key(fs, in->ino, key);
    cfs_block_nonce(lblk, fs->gen, nonce);
    memset(e, 0, sizeof(*e));
    chacha20_seal(key, nonce, out, CFS_BLOCK, e->tag);
    memset(key, 0, sizeof(key));
    e->generation = fs->gen;
    e->crc = crc32c(out, CFS_BLOCK);
    return 0;
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
    rc = cfs_data_write(fs, nblk, buf);
    if (rc == 0)
        rc = cipher ? cfs_aead_put(fs, in, lblk, &entry) : cfs_csum_put(fs, in, lblk, crc32c(buf, CFS_BLOCK));
    kfree(cipher);
    if (rc == 0)
        rc = cfs_set_block(fs, in, lblk, nblk, &old);
    if (rc) {
        cfs_free_block_deferred(fs, nblk);
        return rc;
    }
    if (old)
        cfs_free_block_deferred(fs, old);
    return 0;
}

/*
 * Write an aligned record's worth of logical blocks, compressed if that
 * makes it strictly smaller in whole blocks. Anything else -- it does
 * not compress, the algorithm is off, there is no room for a contiguous
 * run -- is written as ordinary blocks, which is always allowed and
 * never wrong.
 */
static int record_write(struct cfs *fs, struct cfs_inode *in, uint64_t lblk0, const uint8_t *data, unsigned n)
{
    if (n > 1 && in->compress_algo == CFS_COMPRESS_LZ4) {
        /* Room for one block less than it takes now: a record that
         * compresses to the same number of blocks has cost work and
         * saved nothing, and would have to be decompressed on every
         * read for ever after. */
        size_t cap = (size_t)(n - 1) * CFS_BLOCK;
        uint8_t *packed = kmalloc(cap, KMEM_ZERO);
        if (packed == NULL)
            return -ENOMEM;
        size_t clen = lz4_compress(data, (size_t)n * CFS_BLOCK, packed, cap);
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
    /* Plain blocks. A record that used to cover this range is displaced
     * by the first of them, which set_extent frees whole. */
    for (unsigned i = 0; i < n; i++) {
        int rc = data_write_block(fs, in, lblk0 + i, data + (size_t)i * CFS_BLOCK, CFS_ALLOC_DATA);
        if (rc)
            return rc;
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
static int snap_by_name(struct cfs *fs, const char *name, size_t len, struct cfs_snapshot *out, unsigned *tag)
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
        rc = snap_by_name(fs, name, len, &s, &tag);
        if (rc == 0)
            rc = cfs_vnode_get_tagged(fs, CFS_ROOT_INO, tag, &s, out);
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

static int cfs_create_common(struct vnode *dir, const char *name, size_t len, uint32_t mode, unsigned type,
                             struct vnode **out)
{
    struct cfs *fs = cfs_of(dir->mnt);
    if (fs == NULL || fs->failed)
        return -EIO;   /* unmounted, or the transaction was abandoned */
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
    in.uid = cred_current()->euid;   /* owned by its creator */
    in.gid = cred_current()->egid;
    in.nlink = type == CFS_TYPE_DIR ? 2 : 1;
    in.ino = ino;
    in.parent = dir->ino;
    /* An encrypted filesystem authenticates every file it creates: a
     * CRC would say a block is intact, not that it is the one written. */
    in.csum_algo = fs->encrypted ? CFS_CSUM_POLY1305 : CFS_CSUM_CRC32C;
    /* Regular files compress; a directory's blocks are written one at a
     * time and a record cannot be formed from them. */
    in.compress_algo = type == CFS_TYPE_REG ? CFS_COMPRESS_LZ4 : CFS_COMPRESS_NONE;
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
    if (is_snapdir(dir) || snap_tag_of(dir))
        return -EROFS;   /* .snapshots holds snapshots, not files */
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
        int rc = cfs_snapshot_create(fs, buf);
        struct cfs_snapshot s;
        unsigned tag;
        if (rc == 0 && out)
            rc = snap_by_name(fs, name, len, &s, &tag) == 0 ? cfs_vnode_get_tagged(fs, CFS_ROOT_INO, tag, &s, out)
                                                            : -EIO;
        mutex_unlock(&fs->lock);
        if (rc == 0)
            kinfo("cosmofs: snapshot '%s' taken", buf);
        return rc;
    }
    if (snap_tag_of(dir))
        return -EROFS;   /* a snapshot is not writable */
    return cfs_create_common(dir, name, len, mode, CFS_TYPE_DIR, out);
}

static int cfs_unlink_common(struct vnode *dir, const char *name, size_t len, struct vnode *victim, bool rmdir)
{
    struct cfs *fs = cfs_of(dir->mnt);
    if (fs == NULL || fs->failed)
        return -EIO;   /* unmounted, or the transaction was abandoned */
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
    if (is_snapdir(dir) || snap_tag_of(dir))
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
        int rc = snap_by_name(fs, name, len, &snap, &tag);
        if (rc == 0) {
            struct snap_users u = { .tag = tag, .victim = victim };
            if (vnode_cache_any(dir->mnt, snap_vnode_in_use, &u))
                rc = -EBUSY;
        }
        if (rc == 0)
            rc = cfs_snapshot_delete(fs, buf);
        mutex_unlock(&fs->lock);
        if (rc == 0)
            kinfo("cosmofs: snapshot '%s' deleted", buf);
        return rc;
    }
    if (snap_tag_of(dir))
        return -EROFS;
    return cfs_unlink_common(dir, name, len, victim, true);
}

static int cfs_rename(struct vnode *odir, const char *oname, size_t olen, struct vnode *victim, struct vnode *ndir,
                      const char *nname, size_t nlen, struct vnode *replaced)
{
    struct cfs *fs = cfs_of(odir->mnt);
    if (fs == NULL || fs->failed)
        return -EIO;   /* unmounted, or the transaction was abandoned */
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
        int rb = dir_find(fs, ndir, nname, nlen, block, &rl, &rs);
        if (rb == 0) {
            if (replaced) {
                struct cfs_dirent *rd = &((struct cfs_dirent *)block)[rs];
                rd->ino = replaced->ino;
                rd->type = (uint8_t)(replaced->type == VNODE_DIR ? CFS_TYPE_DIR : CFS_TYPE_REG);
                rb = dir_write_block(fs, ndir, rl, block);
            } else {
                rb = dir_remove_slot(fs, ndir, rl, rs, block);
            }
        }
        if (rb) {
            /* Neither state can be reached: drop the whole transaction. */
            cfs_fail(fs, rb);
        } else {
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
            if (d[s].namelen == 0 || d[s].namelen > CFS_NAME_MAX) {
                rc = -EIO;   /* on-disk length must never size a copy */
                break;
            }
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
            rc = record_write(fs, in, e.lblk, rec, count);
        }
        kfree(rec);
        return rc;
    }
    /* Holes are representable: only this block is written. */
    return data_write_block(fs, in, index, buf, CFS_ALLOC_DATA);
}

static int cfs_writepage(struct vnode *vn, uint64_t index, const void *buf)
{
    if (snap_tag_of(vn))
        return -EROFS;   /* a snapshot is what the tree was, not what it is */
    struct cfs *fs = cfs_of(vn->mnt);
    if (fs == NULL || fs->failed)
        return -EIO;   /* unmounted, or the transaction was abandoned */
    mutex_lock(&fs->lock);
    int rc = write_one_page(fs, cfs_inode_of(vn), index, buf);
    if (rc == 0)
        rc = inode_sync(fs, vn);
    mutex_unlock(&fs->lock);
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
    if (snap_tag_of(vn))
        return -EROFS;
    struct cfs *fs = cfs_of(vn->mnt);
    if (fs == NULL || fs->failed)
        return -EIO;
    mutex_lock(&fs->lock);
    struct cfs_inode *in = cfs_inode_of(vn);
    int rc;
    if (index % CFS_RECORD_BLOCKS == 0 && n >= CFS_RECORD_BLOCKS && in->compress_algo != CFS_COMPRESS_NONE) {
        /* Gather the record; the pages are separate frames. */
        uint8_t *rec = kmalloc((size_t)CFS_RECORD_BLOCKS * CFS_BLOCK, 0);
        if (rec == NULL) {
            mutex_unlock(&fs->lock);
            return -ENOMEM;
        }
        for (unsigned i = 0; i < CFS_RECORD_BLOCKS; i++)
            memcpy(rec + (size_t)i * CFS_BLOCK, pages[i], CFS_BLOCK);
        rc = record_write(fs, in, index, rec, CFS_RECORD_BLOCKS);
        kfree(rec);
        if (rc == 0)
            *done = CFS_RECORD_BLOCKS;
    } else {
        rc = write_one_page(fs, in, index, pages[0]);
        if (rc == 0)
            *done = 1;
    }
    if (rc == 0)
        rc = inode_sync(fs, vn);
    mutex_unlock(&fs->lock);
    return rc;
}

static int cfs_truncate(struct vnode *vn, uint64_t size)
{
    if (snap_tag_of(vn))
        return -EROFS;
    struct cfs *fs = cfs_of(vn->mnt);
    if (fs == NULL || fs->failed)
        return -EIO;   /* unmounted, or the transaction was abandoned */
    pagecache_truncate(vn, size);
    mutex_lock(&fs->lock);
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
    mutex_unlock(&fs->lock);
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
    mutex_lock(&fs->lock);
    int rc = inode_sync(fs, vn);
    if (rc == 0)
        rc = cfs_commit(fs);
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
    .writepages = cfs_writepages,
    .truncate = cfs_truncate,
    .sync = cfs_vnode_sync,
    .evict = cfs_evict,
};
