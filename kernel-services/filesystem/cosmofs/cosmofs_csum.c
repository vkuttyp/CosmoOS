/*
 * cosmofs_csum.c - A file's checksums: the one- and two-level checksum
 * index, the CRC and authenticated entries, the verified read of a data
 * block (and its repair from a mirror), compressed records, the scrub's
 * block check, the raises to formats 14 and 15. Moved out of cosmofs.c
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
#include <kernel/string.h>

#include "cosmofs_internal.h"

/* --- checksums (docs/kernel-services/filesystem/cosmofs/design.md, version 2) --- */

/*
 * A file outgrows the one-level checksum index (CFS-10; design.md, "The
 * two-level checksum index"): the format must say a CSUMIDX2 may be met.
 * A pool below 14 is raised, through 12 as a snapshot would raise it; one
 * below 11, or one encrypted with a version-1 key block (which version 14
 * would read as MAC'd), keeps the old limit.
 */
static int csum_raise14(struct cfs *fs)
{
    if (fs->sb.version >= 14)
        return 0;
    if (fs->sb.version < 11 || fs->sb.key_root != 0)
        return -EFBIG;
    unsigned was = fs->sb.version;
    int rc = cfs_snap_raise(fs);
    if (rc)
        return rc;
    fs->sb.version = 14;
    kinfo("cosmofs: format version %u -> 14 (two-level checksum index): a file outgrew the one-level index; a "
          "kernel that reads format 13 at most (CosmoOS 1.0.12 and older) cannot mount this pool", was);
    return 0;
}

/*
 * A name longer than 47 bytes (design.md, "Format version 15"): the format
 * must say an entry may take several slots. Raised as 14 is, through 12;
 * a pool below 11, or one encrypted with a version-1 key block, keeps the
 * old limit. Under the tree's write lock, before anything is allocated.
 */
int cfs_names_allow(struct cfs *fs, size_t len)
{
    if (len <= CFS_NAME_MAX || fs->sb.version >= 15)
        return len > CFS_NAME_MAX_LONG ? -ENAMETOOLONG : 0;
    if (len > CFS_NAME_MAX_LONG || fs->sb.version < 11 || (fs->sb.key_root != 0 && fs->sb.version < 14))
        return -ENAMETOOLONG;
    unsigned was = fs->sb.version;
    int rc = cfs_snap_raise(fs);
    if (rc)
        return rc;
    fs->sb.version = 15;
    kinfo("cosmofs: format version %u -> 15 (long names): a name of %zu bytes; a kernel that reads format 14 at "
          "most (CosmoOS 1.0.14 and older) cannot mount this pool", was, len);
    return 0;
}

/* An index block below `*slot`: read (and CoW'd when `writable`), or made
 * when there is none and `writable`; -ENOENT when there is none to read.
 * On failure `*out` is NULL: the caller has nothing to release. */
static int csum_level(struct cfs *fs, uint64_t *slot, uint32_t kind, bool writable, struct cfs_buf **out)
{
    int rc;
    if (*slot == 0) {
        if (!writable)
            return -ENOENT;
        rc = cfs_buf_new(fs, kind, out);
        if (rc == 0)
            *slot = (*out)->blkno;
        return rc;
    }
    rc = cfs_buf_get(fs, *slot, kind, out);
    if (rc != 0) {
        *out = NULL;
        return rc;
    }
    if (writable) {
        /* The test seam (cosmofs-encrypted-root): an index block's copy
         * cannot be had, as when the pool is full. */
        rc = kind == CFS_KIND_CSUMIDX && faultinject_should_fail(FI_COSMOFS_CSUM_COW) ? -ENOMEM
                                                                                      : cfs_buf_cow(fs, out, slot);
        if (rc != 0) {
            cfs_buf_put(fs, *out);
            *out = NULL;   /* a two-level walk's cleanup releases `mid` (Codex on #447) */
        }
    }
    return rc;
}

/* The kind of block an inode's csum_root names: CSUMIDX or, from version
 * 14, CSUMIDX2. Anything else is a damaged inode. */
static int csum_root_kind(struct cfs *fs, uint64_t root, uint32_t *kind)
{
    struct cfs_buf *b;
    int rc = cfs_buf_get(fs, root, 0, &b);
    if (rc)
        return rc;
    *kind = cfs_buf_hdr(b)->kind;
    cfs_buf_put(fs, b);
    if (*kind == CFS_KIND_CSUMIDX || (*kind == CFS_KIND_CSUMIDX2 && fs->sb.version >= 14))
        return 0;
    return -EIO;
}

/*
 * The CSUM block for `lblk`, created (and CoW'd) when `writable`.
 * `index` is the leaf's position in the checksum index, which differs
 * between the 4-byte CRC entries and the 32-byte authenticated ones. Up
 * to CFS_CSUMIDX_SPAN leaves the root is a CSUMIDX; past it a CSUMIDX2
 * whose entries are CSUMIDX blocks, and the old root becomes its entry 0
 * so no checksum moves.
 */
static int csum_block_at(struct cfs *fs, struct cfs_inode *in, uint64_t lblk, unsigned index, bool writable,
                         struct cfs_buf **out)
{
    (void)lblk;
    if (index >= CFS_CSUMIDX_SPAN * CFS_CSUMIDX_SPAN)
        return -EFBIG;
    uint32_t kind = CFS_KIND_CSUMIDX;
    int rc;
    if (in->csum_root != 0 && (rc = csum_root_kind(fs, in->csum_root, &kind)) != 0)
        return rc;
    bool two = kind == CFS_KIND_CSUMIDX2;
    if (!two && index >= CFS_CSUMIDX_SPAN) {
        if (!writable)
            return -ENOENT;   /* past the one-level index: nothing was ever stored there */
        rc = csum_raise14(fs);
        if (rc)
            return rc;
        /* The new root's entry 0 is the old one-level index, as it is. */
        struct cfs_buf *top;
        rc = cfs_buf_new(fs, CFS_KIND_CSUMIDX2, &top);
        if (rc)
            return rc;
        ((uint64_t *)(top->data + CFS_MHDR_SIZE))[0] = in->csum_root;
        in->csum_root = top->blkno;
        cfs_buf_put(fs, top);
        two = true;
    }
    struct cfs_buf *root, *mid = NULL, *cb;
    rc = csum_level(fs, &in->csum_root, two ? CFS_KIND_CSUMIDX2 : CFS_KIND_CSUMIDX, writable, &root);
    if (rc)
        return rc;
    struct cfs_buf *idx = root;
    unsigned at = index;
    if (two) {
        uint64_t *upper = &((uint64_t *)(root->data + CFS_MHDR_SIZE))[index / CFS_CSUMIDX_SPAN];
        rc = csum_level(fs, upper, CFS_KIND_CSUMIDX, writable, &mid);
        if (rc)
            goto out;
        idx = mid;
        at = index % CFS_CSUMIDX_SPAN;
    }
    rc = csum_level(fs, &((uint64_t *)(idx->data + CFS_MHDR_SIZE))[at], CFS_KIND_CSUM, writable, &cb);
    if (rc == 0)
        *out = cb;
out:
    if (mid != NULL)
        cfs_buf_put(fs, mid);
    cfs_buf_put(fs, root);
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
    rc = cfs_read_repair(fs, dva, buf, cipher_crc_ok, &w, NULL, NULL);
    if (rc) {
        kerror("cosmofs: inode %llu block %llu: no copy matches its checksum", (unsigned long long)in->ino,
               (unsigned long long)lblk);
        fs->csum_failures++;
        return rc;
    }
    if (!fs->have_key)
        return -ENOKEY;   /* the metadata mounted; the contents did not */

    uint8_t key[CHACHA20_KEY_SIZE];
    cfs_file_key(fs, in->ino, key);
    struct cfs_block_aad aad = { .ino = in->ino, .lblk = lblk };
    bool ok = chacha20_open(key, e.nonce, &aad, sizeof(aad), buf, CFS_BLOCK, e.tag);
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
    int rc = cfs_read_repair(fs, dva, buf, data_ok, &w, NULL, NULL);
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
                         bool repair, unsigned *bad, unsigned *repaired)
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
        rc = cfs_verify_all(fs, dva, buf, cipher_crc_ok, &w, repair, bad, repaired);
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
    int rc = cfs_verify_all(fs, dva, buf, data_ok, &w, repair, bad, repaired);
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

/* Release the whole checksum tree (the inode is being emptied): a
 * one-level index and its CSUM blocks, or a two-level root, its index
 * blocks and theirs. */
static void csum_free_index(struct cfs *fs, uint64_t blk)
{
    struct cfs_buf *idx;
    if (cfs_buf_get(fs, blk, CFS_KIND_CSUMIDX, &idx) == 0) {
        const uint64_t *slots = (const uint64_t *)(idx->data + CFS_MHDR_SIZE);
        for (unsigned i = 0; i < CFS_PTRS_PER_BLOCK; i++)
            if (slots[i])
                cfs_free_block_deferred(fs, slots[i]);
        cfs_buf_put(fs, idx);
    }
    cfs_free_block_deferred(fs, blk);
}

void cfs_csum_free(struct cfs *fs, struct cfs_inode *in)
{
    if (in->csum_root == 0)
        return;
    uint32_t kind = 0;
    if (csum_root_kind(fs, in->csum_root, &kind) == 0 && kind == CFS_KIND_CSUMIDX2) {
        struct cfs_buf *top;
        if (cfs_buf_get(fs, in->csum_root, CFS_KIND_CSUMIDX2, &top) == 0) {
            const uint64_t *slots = (const uint64_t *)(top->data + CFS_MHDR_SIZE);
            for (unsigned i = 0; i < CFS_PTRS_PER_BLOCK; i++)
                if (slots[i])
                    csum_free_index(fs, slots[i]);
            cfs_buf_put(fs, top);
        }
        cfs_free_block_deferred(fs, in->csum_root);
    } else {
        csum_free_index(fs, in->csum_root);
    }
    in->csum_root = 0;
}
