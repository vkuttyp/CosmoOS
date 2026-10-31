/*
 * cfsparse.c - cosmofs's decoders of on-disk metadata (roadmap 2.0 SA5,
 * the cosmofs family, batch 1): metadata block headers and their CRC,
 * directory entries, block addresses, and extents. Every check made on
 * bytes read from disk before they are trusted, moved here unchanged
 * from cosmofs_core.c, cosmofs.c, cosmofs_member.c and cosmofs_format.h:
 * the C step, to be replaced by cfsparse.cee once tests/host/test_cfsparse.c's
 * recorded digest agrees. They read the pool's geometry (fs->mem,
 * fs->nmembers) and nothing else of its state.
 */

#include <kernel/crc32c.h>
#include <kernel/string.h>

#include "cosmofs_internal.h"

/* CRC32C over a block with the 4-byte crc field at crc_off taken as zero. */
uint32_t cfs_block_crc(const uint8_t *block, size_t crc_off)
{
    static const uint8_t zero4[4] = { 0 };
    uint32_t c = crc32c(block, crc_off);
    c = crc32c_update(c, zero4, 4);
    return crc32c_update(c, block + crc_off + 4, CFS_BLOCK - crc_off - 4);
}

/* A metadata header's checksum algorithm. CRC32C is the only one the format
 * computes today; 0 is a pre-v11 image's zeroed pad and is also CRC32C. Any
 * other value is a block this build cannot verify (MHDR_ALGO), which is a
 * different finding from a checksum that simply did not match. On a
 * version-14 encrypted pool the field is half the tag and means nothing on
 * its own. */
static bool meta_csum_supported(uint32_t algo)
{
    return algo == CFS_CSUM_NONE || algo == CFS_CSUM_CRC32C;
}

/* A metadata block's header against its expected DVA and kind (0: any),
 * and its CRC32C unless `mac_on` (the caller then checks the tag): an
 * enum mhdr_fault. */
uint32_t cfs_mhdr_decode(const void *block, uint64_t blkno, uint32_t kind, bool mac_on)
{
    const struct cfs_mhdr *h = (const struct cfs_mhdr *)block;
    if (h->magic != CFS_MHDR_MAGIC)
        return MHDR_MAGIC;
    if (h->blkno != blkno)
        return MHDR_BLKNO;
    if (kind && h->kind != kind)
        return MHDR_KIND;
    if (mac_on)
        return MHDR_OK;
    if (!meta_csum_supported(h->csum_algo))
        return MHDR_ALGO;
    if (cfs_block_crc(block, offsetof(struct cfs_mhdr, crc)) != h->crc)
        return MHDR_CRC;
    return MHDR_OK;
}

bool cfs_dva_valid(const struct cfs *fs, uint64_t dva)
{
    unsigned v = CFS_DVA_VDEV(dva);
    if (v >= fs->nmembers)
        return false;
    uint64_t b = CFS_DVA_BLK(dva);
    return b >= fs->mem[v].first_usable && b < fs->mem[v].nblocks;
}

uint64_t cfs_dva_lin(const struct cfs *fs, uint64_t dva)
{
    unsigned v = CFS_DVA_VDEV(dva);
    if (v >= fs->nmembers)
        return CFS_DVA_NONE;
    uint64_t b = CFS_DVA_BLK(dva);
    if (b >= fs->mem[v].nblocks)
        return CFS_DVA_NONE;
    return fs->mem[v].base + b;
}

bool cfs_extent_valid(const struct cfs *fs, const struct cfs_extent *e)
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

/* The direct runs of an inode as read from disk: in range, sorted, no
 * overlap, no run after the first empty slot. */
bool cfs_direct_valid(const struct cfs *fs, const struct cfs_inode *in)
{
    unsigned n = 0;
    while (n < CFS_DIRECT && cfs_ext_count(&in->direct[n]))
        n++;
    for (unsigned i = n; i < CFS_DIRECT; i++)
        if (in->direct[i].count || in->direct[i].start || in->direct[i].lblk)
            return false;   /* a run hiding behind an empty slot */
    for (unsigned i = 0; i < n; i++) {
        if (!cfs_extent_valid(fs, &in->direct[i]))
            return false;
        if (i > 0 && in->direct[i].lblk < (uint64_t)in->direct[i - 1].lblk + cfs_ext_count(&in->direct[i - 1]))
            return false;
    }
    return true;
}

/* Runs read from disk, `n` of them: each in range, sorted, no overlap. */
bool cfs_extents_valid(const struct cfs *fs, const struct cfs_extent *ext, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        if (!cfs_extent_valid(fs, &ext[i]) ||
            (i > 0 && ext[i].lblk < (uint64_t)ext[i - 1].lblk + cfs_ext_count(&ext[i - 1])))
            return false;
    }
    return true;
}

/* The continuations a name of `len` bytes needs. */
unsigned cfs_dirent_extra(unsigned len)
{
    return len <= CFS_DIRENT_HEAD_NAME ? 0
                                       : (len - CFS_DIRENT_HEAD_NAME + CFS_DIRENT_CONT_NAME - 1) / CFS_DIRENT_CONT_NAME;
}

/*
 * The entry whose head is slot `s` of `blk`, checked before a byte of its
 * name is read: the slots it takes (1 + extra), or 0 when slot `s` is not
 * a well-formed head for a pool of `version` -- free, a continuation, an
 * impossible length, a wrong `extra`, a run that leaves the block, or a
 * continuation that is not marked.
 */
unsigned cfs_dirent_check(const struct cfs_dirent *blk, unsigned s, unsigned version)
{
    const struct cfs_dirent *d = &blk[s];
    if (d->ino == 0 || d->ino == CFS_DIRENT_CONT || d->namelen == 0)
        return 0;
    unsigned max = version >= 15 ? CFS_NAME_MAX_LONG : CFS_NAME_MAX;
    if (d->namelen > max || d->extra != cfs_dirent_extra(d->namelen))
        return 0;
    if (s + d->extra >= CFS_BLOCK / sizeof(struct cfs_dirent))
        return 0;
    for (unsigned k = 1; k <= d->extra; k++)
        if (((const struct cfs_dirent_cont *)&blk[s + k])->mark != CFS_DIRENT_CONT)
            return 0;
    return 1u + d->extra;
}

/* The name of the checked entry at slot `s` into `out` (CFS_NAME_MAX_LONG
 * bytes at least, not terminated); returns its length. */
unsigned cfs_dirent_name(const struct cfs_dirent *blk, unsigned s, char *out)
{
    const struct cfs_dirent *d = &blk[s];
    unsigned len = d->namelen, n = 0;
    for (; n < len && n < CFS_DIRENT_HEAD_NAME; n++)
        out[n] = d->name[n];
    for (unsigned k = 1; n < len; k++) {
        const struct cfs_dirent_cont *c = (const struct cfs_dirent_cont *)&blk[s + k];
        for (unsigned i = 0; i < CFS_DIRENT_CONT_NAME && n < len; i++)
            out[n++] = c->name[i];
    }
    return len;
}

/* Whether the checked entry at slot `s` is `name` (`len` bytes). */
int cfs_dirent_name_eq(const struct cfs_dirent *blk, unsigned s, const char *name, unsigned len)
{
    const struct cfs_dirent *d = &blk[s];
    if (d->namelen != len)
        return 0;
    unsigned n = 0;
    for (; n < len && n < CFS_DIRENT_HEAD_NAME; n++)
        if (d->name[n] != name[n])
            return 0;
    for (unsigned k = 1; n < len; k++) {
        const struct cfs_dirent_cont *c = (const struct cfs_dirent_cont *)&blk[s + k];
        for (unsigned i = 0; i < CFS_DIRENT_CONT_NAME && n < len; i++, n++)
            if (c->name[i] != name[n])
                return 0;
    }
    return 1;
}
