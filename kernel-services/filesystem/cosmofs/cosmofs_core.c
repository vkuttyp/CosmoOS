/*
 * cosmofs_core.c - Buffers, allocation, the inode map, transactions,
 * commit, format, mount and unmount.
 *
 * Rules (docs/kernel-services/vfs/design.md): a metadata block from a
 * committed generation is never modified in place; the in-memory bitmap
 * is authoritative during a transaction and its chunks are written at
 * commit through a reserve-then-write fixpoint; blocks freed in a
 * transaction become allocatable only after that transaction's root is
 * on disk.
 */

#include <kernel/blk.h>
#include <kernel/cosmofs.h>
#include <kernel/crc32c.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/string.h>

#include "cosmofs_internal.h"

/* --- checksums and headers -------------------------------------------- */

/* CRC32C over a block with the 4-byte crc field at crc_off taken as zero. */
static uint32_t block_crc(const uint8_t *block, size_t crc_off)
{
    static const uint8_t zero4[4] = { 0 };
    uint32_t c = crc32c(block, crc_off);
    c = crc32c_update(c, zero4, 4);
    return crc32c_update(c, block + crc_off + 4, CFS_BLOCK - crc_off - 4);
}

static void mhdr_seal(struct cfs *fs, uint8_t *block, uint32_t kind, uint64_t blkno)
{
    struct cfs_mhdr *h = (struct cfs_mhdr *)block;
    h->magic = CFS_MHDR_MAGIC;
    h->kind = kind;
    h->generation = fs->gen;
    h->blkno = blkno;
    h->pad = 0;
    h->crc = 0;
    h->crc = block_crc(block, offsetof(struct cfs_mhdr, crc));
}

static int mhdr_check(const uint8_t *block, uint64_t blkno, uint32_t kind)
{
    const struct cfs_mhdr *h = (const struct cfs_mhdr *)block;
    if (h->magic != CFS_MHDR_MAGIC || h->blkno != blkno || (kind && h->kind != kind))
        return -EIO;
    if (block_crc(block, offsetof(struct cfs_mhdr, crc)) != h->crc)
        return -EIO;
    return 0;
}

struct cfs_mhdr *cfs_buf_hdr(struct cfs_buf *b)
{
    return (struct cfs_mhdr *)b->data;
}

int cfs_data_read(struct cfs *fs, uint64_t blk, void *buf)
{
    if (blk < 2 || blk >= fs->nblocks)
        return -EIO;
    return pool_read(fs->pool, blk, buf);
}

int cfs_data_write(struct cfs *fs, uint64_t blk, const void *buf)
{
    if (blk < 2 || blk >= fs->nblocks)
        return -EIO;
    return pool_write(fs->pool, blk, buf);
}

/* --- buffer cache -------------------------------------------------------- */

static struct cfs_buf *buf_find(struct cfs *fs, uint64_t blkno)
{
    struct cfs_buf *b;
    list_for_each_entry(b, &fs->bufs, link) {
        if (b->blkno == blkno)
            return b;
    }
    return NULL;
}

static void buf_evict_clean(struct cfs *fs)
{
    if (fs->nr_bufs < CFS_BUF_CACHE)
        return;
    struct cfs_buf *b;
    list_for_each_entry_reverse(b, &fs->bufs, link) {
        if (!b->dirty && b->refs == 0) {
            list_remove(&b->link);
            fs->nr_bufs--;
            kfree(b->data);
            kfree(b);
            return;
        }
    }
}

static struct cfs_buf *buf_alloc(struct cfs *fs, uint64_t blkno)
{
    buf_evict_clean(fs);
    struct cfs_buf *b = kzalloc(sizeof(*b));
    if (b == NULL)
        return NULL;
    b->data = kmalloc(CFS_BLOCK, KMEM_ZERO);
    if (b->data == NULL) {
        kfree(b);
        return NULL;
    }
    list_init(&b->link);
    b->blkno = blkno;
    b->refs = 1;
    list_push_front(&fs->bufs, &b->link);
    fs->nr_bufs++;
    return b;
}

int cfs_buf_get(struct cfs *fs, uint64_t blkno, uint32_t kind, struct cfs_buf **out)
{
    if (blkno < 2 || blkno >= fs->nblocks)
        return -EIO;
    struct cfs_buf *b = buf_find(fs, blkno);
    if (b) {
        b->refs++;
        list_remove(&b->link);
        list_push_front(&fs->bufs, &b->link);
        if (kind && cfs_buf_hdr(b)->kind != kind)
            return -EIO;
        *out = b;
        return 0;
    }
    b = buf_alloc(fs, blkno);
    if (b == NULL)
        return -ENOMEM;
    int rc = pool_read(fs->pool, blkno, b->data);
    if (rc == 0)
        rc = mhdr_check(b->data, blkno, kind);
    if (rc) {
        kerror("cosmofs: block %llu: %s", (unsigned long long)blkno,
               rc == -EIO ? "bad metadata header or checksum" : "read error");
        list_remove(&b->link);
        fs->nr_bufs--;
        kfree(b->data);
        kfree(b);
        return rc;
    }
    *out = b;
    return 0;
}

void cfs_buf_put(struct cfs *fs, struct cfs_buf *b)
{
    (void)fs;
    KASSERT(b->refs > 0);
    b->refs--;
}

static void buf_mark_dirty(struct cfs *fs, struct cfs_buf *b)
{
    if (!b->dirty) {
        b->dirty = true;
        fs->nr_dirty++;
    }
}

int cfs_buf_new(struct cfs *fs, uint32_t kind, struct cfs_buf **out)
{
    uint64_t blk;
    int rc = cfs_alloc_block(fs, &blk);
    if (rc)
        return rc;
    struct cfs_buf *b = buf_alloc(fs, blk);
    if (b == NULL) {
        cfs_free_block_deferred(fs, blk);
        return -ENOMEM;
    }
    memset(b->data, 0, CFS_BLOCK);
    mhdr_seal(fs, b->data, kind, blk);
    buf_mark_dirty(fs, b);
    *out = b;
    return 0;
}

int cfs_buf_cow(struct cfs *fs, struct cfs_buf **bp, uint64_t *parent_slot)
{
    struct cfs_buf *b = *bp;
    struct cfs_mhdr *h = cfs_buf_hdr(b);
    if (h->generation == fs->gen) {
        buf_mark_dirty(fs, b);
        return 0;
    }
    uint64_t nblk;
    int rc = cfs_alloc_block(fs, &nblk);
    if (rc)
        return rc;
    struct cfs_buf *nb = buf_alloc(fs, nblk);
    if (nb == NULL) {
        cfs_free_block_deferred(fs, nblk);
        return -ENOMEM;
    }
    memcpy(nb->data, b->data, CFS_BLOCK);
    mhdr_seal(fs, nb->data, h->kind, nblk);
    buf_mark_dirty(fs, nb);
    cfs_free_block_deferred(fs, b->blkno);
    cfs_buf_put(fs, b);
    *parent_slot = nblk;
    *bp = nb;
    return 0;
}

/* --- allocation ------------------------------------------------------------ */

static inline bool bit_test(const uint8_t *map, uint64_t i) { return (map[i >> 3] >> (i & 7)) & 1; }
static inline void bit_set(uint8_t *map, uint64_t i) { map[i >> 3] |= (uint8_t)(1u << (i & 7)); }
static inline void bit_clear(uint8_t *map, uint64_t i) { map[i >> 3] &= (uint8_t)~(1u << (i & 7)); }

int cfs_alloc_block(struct cfs *fs, uint64_t *out)
{
    if (fs->free_blocks == 0)
        return -ENOSPC;
    uint64_t start = fs->alloc_hint < fs->nblocks ? fs->alloc_hint : 2;
    for (uint64_t n = 0; n < fs->nblocks; n++) {
        uint64_t i = start + n;
        if (i >= fs->nblocks)
            i -= fs->nblocks;
        if (i < 2)
            continue;
        if (!bit_test(fs->bitmap, i)) {
            bit_set(fs->bitmap, i);
            fs->bitmap_dirty[i / CFS_BITS_PER_BITMAP] = 1;
            fs->free_blocks--;
            fs->alloc_hint = i + 1;
            *out = i;
            return 0;
        }
    }
    return -ENOSPC;
}

void cfs_free_block_deferred(struct cfs *fs, uint64_t blk)
{
    if (blk < 2 || blk >= fs->nblocks)
        return;
    if (fs->nr_pending == fs->pending_cap) {
        unsigned cap = fs->pending_cap ? fs->pending_cap * 2 : 64;
        uint64_t *n = krealloc(fs->pending_free, cap * sizeof(*n), 0);
        if (n == NULL) {
            kerror("cosmofs: leaking block %llu (no memory for the free list)", (unsigned long long)blk);
            return;
        }
        fs->pending_free = n;
        fs->pending_cap = cap;
    }
    fs->pending_free[fs->nr_pending++] = blk;
}

/* --- inode map ----------------------------------------------------------- */

static uint64_t *ptrs(struct cfs_buf *b)
{
    return (uint64_t *)(b->data + CFS_MHDR_SIZE);
}

/* Fetch (and when `writable`, CoW) the L1, L0 and inode block for `ino`.
 * Missing L0/inode blocks are created when `create` is set. */
static int inode_block(struct cfs *fs, uint64_t ino, bool writable, bool create, struct cfs_buf **out)
{
    if (ino == 0 || ino >= CFS_MAX_INODES)
        return -EIO;
    struct cfs_buf *l1, *l0, *ib;
    int rc = cfs_buf_get(fs, fs->sb.imap_root, CFS_KIND_IMAP1, &l1);
    if (rc)
        return rc;
    if (writable && (rc = cfs_buf_cow(fs, &l1, &fs->sb.imap_root)) != 0) {
        cfs_buf_put(fs, l1);
        return rc;
    }
    uint64_t *l1p = &ptrs(l1)[cfs_imap_l1_index(ino)];
    if (*l1p == 0) {
        if (!create) {
            cfs_buf_put(fs, l1);
            return -ENOENT;
        }
        rc = cfs_buf_new(fs, CFS_KIND_IMAP0, &l0);
        if (rc) {
            cfs_buf_put(fs, l1);
            return rc;
        }
        *l1p = l0->blkno;
    } else {
        rc = cfs_buf_get(fs, *l1p, CFS_KIND_IMAP0, &l0);
        if (rc) {
            cfs_buf_put(fs, l1);
            return rc;
        }
        if (writable && (rc = cfs_buf_cow(fs, &l0, l1p)) != 0) {
            cfs_buf_put(fs, l0);
            cfs_buf_put(fs, l1);
            return rc;
        }
    }
    uint64_t *l0p = &ptrs(l0)[cfs_imap_l0_index(ino)];
    if (*l0p == 0) {
        if (!create) {
            rc = -ENOENT;
            goto out;
        }
        rc = cfs_buf_new(fs, CFS_KIND_INODES, &ib);
        if (rc)
            goto out;
        *l0p = ib->blkno;
    } else {
        rc = cfs_buf_get(fs, *l0p, CFS_KIND_INODES, &ib);
        if (rc)
            goto out;
        if (writable && (rc = cfs_buf_cow(fs, &ib, l0p)) != 0) {
            cfs_buf_put(fs, ib);
            goto out;
        }
    }
    *out = ib;
out:
    cfs_buf_put(fs, l0);
    cfs_buf_put(fs, l1);
    return rc;
}

static struct cfs_inode *inode_slot(struct cfs_buf *ib, uint64_t ino)
{
    return (struct cfs_inode *)(ib->data + CFS_MHDR_SIZE + cfs_inode_slot(ino) * CFS_INODE_SIZE);
}

int cfs_inode_read(struct cfs *fs, uint64_t ino, struct cfs_inode *out)
{
    if (ino == 0 || ino >= fs->sb.next_ino)
        return -ENOENT;
    struct cfs_buf *ib;
    int rc = inode_block(fs, ino, false, false, &ib);
    if (rc)
        return rc;
    memcpy(out, inode_slot(ib, ino), sizeof(*out));
    cfs_buf_put(fs, ib);
    if (out->ino != ino || out->nlink == 0)
        return -ENOENT;   /* free slot */
    return 0;
}

int cfs_inode_write(struct cfs *fs, uint64_t ino, const struct cfs_inode *in)
{
    struct cfs_buf *ib;
    int rc = inode_block(fs, ino, true, true, &ib);
    if (rc)
        return rc;
    struct cfs_inode *slot = inode_slot(ib, ino);
    memcpy(slot, in, sizeof(*slot));
    slot->generation = fs->gen;
    cfs_buf_put(fs, ib);
    return 0;
}

int cfs_inode_alloc(struct cfs *fs, uint64_t *ino)
{
    if (fs->sb.next_ino >= CFS_MAX_INODES)
        return -ENOSPC;
    *ino = fs->sb.next_ino++;
    fs->sb.inode_count++;
    return 0;
}

/* --- commit ----------------------------------------------------------------- */

/* Write the in-memory bitmap chunks that changed, copy-on-write, until
 * no allocation made by the writing itself leaves a chunk dirty. */
static int commit_bitmap(struct cfs *fs)
{
    uint64_t *reserved = kzalloc(fs->nr_chunks * sizeof(*reserved));
    if (reserved == NULL)
        return -ENOMEM;
    struct cfs_buf *idx;
    int rc = cfs_buf_get(fs, fs->sb.alloc_root, CFS_KIND_ALLOCIDX, &idx);
    if (rc) {
        kfree(reserved);
        return rc;
    }
    rc = cfs_buf_cow(fs, &idx, &fs->sb.alloc_root);
    if (rc)
        goto out;

    /* Reserve a destination block for every dirty chunk; reserving may
     * dirty further chunks, so iterate to a fixpoint. */
    for (bool progress = true; progress;) {
        progress = false;
        for (unsigned c = 0; c < fs->nr_chunks; c++) {
            if (fs->bitmap_dirty[c] && reserved[c] == 0) {
                rc = cfs_alloc_block(fs, &reserved[c]);
                if (rc)
                    goto out;
                progress = true;
            }
        }
    }
    uint64_t *slots = ptrs(idx);
    for (unsigned c = 0; c < fs->nr_chunks; c++) {
        if (!fs->bitmap_dirty[c])
            continue;
        struct cfs_buf *nb = buf_alloc(fs, reserved[c]);
        if (nb == NULL) {
            rc = -ENOMEM;
            goto out;
        }
        size_t bytes = CFS_BITS_PER_BITMAP / 8;
        size_t off = (size_t)c * bytes;
        size_t total = (fs->nblocks + 7) / 8;
        size_t n = off + bytes <= total ? bytes : total - off;
        memset(nb->data, 0, CFS_BLOCK);
        memcpy(nb->data + CFS_MHDR_SIZE, fs->bitmap + off, n);
        mhdr_seal(fs, nb->data, CFS_KIND_BITMAP, reserved[c]);
        buf_mark_dirty(fs, nb);
        cfs_buf_put(fs, nb);
        if (slots[c])
            cfs_free_block_deferred(fs, slots[c]);
        slots[c] = reserved[c];
        fs->bitmap_dirty[c] = 0;
    }
    /* Re-seal the index (it was CoW'd or dirtied above). */
    mhdr_seal(fs, idx->data, CFS_KIND_ALLOCIDX, idx->blkno);
out:
    cfs_buf_put(fs, idx);
    kfree(reserved);
    return rc;
}

static int super_write(struct cfs *fs, unsigned slot)
{
    uint8_t *block = kmalloc(CFS_BLOCK, KMEM_ZERO);
    if (block == NULL)
        return -ENOMEM;
    struct cfs_super *sb = (struct cfs_super *)block;
    *sb = fs->sb;
    sb->crc = 0;
    sb->crc = block_crc(block, offsetof(struct cfs_super, crc));
    int rc = pool_write(fs->pool, slot, block);
    kfree(block);
    return rc;
}

void cfs_fail(struct cfs *fs, int rc)
{
    if (fs->failed == 0) {
        fs->failed = rc ? rc : -EIO;
        kerror("cosmofs: transaction abandoned after error %d; the last committed root stays current", rc);
    }
}

int cfs_commit(struct cfs *fs)
{
    if (fs->failed)
        return fs->failed;
    if (fs->nr_dirty == 0 && fs->nr_pending == 0) {
        bool any = false;
        for (unsigned c = 0; c < fs->nr_chunks; c++)
            any = any || fs->bitmap_dirty[c];
        if (!any)
            return 0;
    }
    int rc = commit_bitmap(fs);
    if (rc)
        return rc;

    /* Every dirty metadata block, re-sealed with its final contents. */
    struct cfs_buf *b;
    list_for_each_entry(b, &fs->bufs, link) {
        if (!b->dirty)
            continue;
        struct cfs_mhdr *h = cfs_buf_hdr(b);
        mhdr_seal(fs, b->data, h->kind, b->blkno);
        rc = pool_write(fs->pool, b->blkno, b->data);
        if (rc)
            return rc;
    }
    rc = pool_flush(fs->pool);
    if (rc)
        return rc;

    /* The root, into the other slot. free_blocks excludes this
     * transaction's pending frees: they are still referenced by the
     * previous root until this one is on disk. */
    fs->sb.generation = fs->gen;
    fs->sb.free_blocks = fs->free_blocks;
    unsigned slot = fs->sb_slot == CFS_SUPER_A ? CFS_SUPER_B : CFS_SUPER_A;
    rc = super_write(fs, slot);
    if (rc)
        return rc;
    rc = pool_flush(fs->pool);
    if (rc)
        return rc;
    fs->sb_slot = slot;
    fs->commits++;

    /* The new root is durable: the old generation's blocks are free. */
    list_for_each_entry(b, &fs->bufs, link) {
        if (b->dirty) {
            b->dirty = false;
        }
    }
    fs->nr_dirty = 0;
    for (unsigned i = 0; i < fs->nr_pending; i++) {
        uint64_t blk = fs->pending_free[i];
        if (bit_test(fs->bitmap, blk)) {
            bit_clear(fs->bitmap, blk);
            fs->bitmap_dirty[blk / CFS_BITS_PER_BITMAP] = 1;
            fs->free_blocks++;
        }
    }
    fs->nr_pending = 0;
    fs->gen++;
    kdebug("cosmofs: committed generation %llu (%llu free blocks)", (unsigned long long)fs->sb.generation,
           (unsigned long long)fs->free_blocks);
    return 0;
}

/* Write back every dirty page of every cached regular file. Called with
 * no cfs lock held (writepage takes it). */
int cfs_sync_vnodes(struct cfs *fs)
{
    struct mount *mnt = fs->mnt;
    int rc = 0;
    for (unsigned b = 0; b < VNODE_HASH; b++) {
        for (;;) {
            struct vnode *vn = NULL;
            mutex_lock(&mnt->lock);
            struct vnode *it;
            list_for_each_entry(it, &mnt->vnodes[b], hash_link) {
                if (it->type == VNODE_REG && it->pc.nr_dirty && kobject_refcount(&it->obj) > 0) {
                    vn = it;
                    vnode_get(vn);
                    break;
                }
            }
            mutex_unlock(&mnt->lock);
            if (vn == NULL)
                break;
            mutex_lock(&vn->lock);
            int r = pagecache_sync(vn);
            mutex_unlock(&vn->lock);
            vnode_put(vn);
            if (r) {
                rc = r;
                break;
            }
        }
    }
    return rc;
}

/* --- format ------------------------------------------------------------------ */

int cosmofs_format(struct blkdev *bd)
{
    struct spool *pool;
    int rc = pool_open(bd, &pool);
    if (rc)
        return rc;
    if (pool->nblocks < CFS_MIN_BLOCKS || pool->nblocks > CFS_MAX_BLOCKS) {
        pool_close(pool);
        return -EINVAL;
    }
    uint64_t nblocks = pool->nblocks;
    unsigned nr_chunks = (unsigned)((nblocks + CFS_BITS_PER_BITMAP - 1) / CFS_BITS_PER_BITMAP);
    if (nr_chunks > CFS_PTRS_PER_BLOCK) {
        pool_close(pool);
        return -EINVAL;
    }

    /* Layout: 0,1 superblocks; 2 alloc index; 3.. bitmaps; then imap L1,
     * imap L0, inode block 0. */
    uint64_t alloc_idx = 2, bitmap0 = 3, imap1 = bitmap0 + nr_chunks, imap0 = imap1 + 1, inodes0 = imap0 + 1;
    uint64_t used = inodes0 + 1;

    uint8_t *block = kmalloc(CFS_BLOCK, KMEM_ZERO);
    if (block == NULL) {
        pool_close(pool);
        return -ENOMEM;
    }
    struct cfs tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.gen = 1;

    /* Bitmaps: blocks [0, used) allocated. */
    for (unsigned c = 0; c < nr_chunks && rc == 0; c++) {
        memset(block, 0, CFS_BLOCK);
        uint8_t *bits = block + CFS_MHDR_SIZE;
        for (uint64_t i = (uint64_t)c * CFS_BITS_PER_BITMAP; i < ((uint64_t)c + 1) * CFS_BITS_PER_BITMAP; i++) {
            if (i >= nblocks || i < used)
                bit_set(bits, i - (uint64_t)c * CFS_BITS_PER_BITMAP);   /* past-the-end bits stay allocated */
        }
        mhdr_seal(&tmp, block, CFS_KIND_BITMAP, bitmap0 + c);
        rc = pool_write(pool, bitmap0 + c, block);
    }
    /* Alloc index. */
    if (rc == 0) {
        memset(block, 0, CFS_BLOCK);
        uint64_t *p = (uint64_t *)(block + CFS_MHDR_SIZE);
        for (unsigned c = 0; c < nr_chunks; c++)
            p[c] = bitmap0 + c;
        mhdr_seal(&tmp, block, CFS_KIND_ALLOCIDX, alloc_idx);
        rc = pool_write(pool, alloc_idx, block);
    }
    /* Inode block 0 with the root directory (inode 1). */
    if (rc == 0) {
        memset(block, 0, CFS_BLOCK);
        struct cfs_inode *root = (struct cfs_inode *)(block + CFS_MHDR_SIZE + CFS_ROOT_INO * CFS_INODE_SIZE);
        root->mode = CFS_MODE(CFS_TYPE_DIR, 0755);
        root->nlink = 2;
        root->ino = CFS_ROOT_INO;
        root->parent = CFS_ROOT_INO;
        root->generation = 1;
        mhdr_seal(&tmp, block, CFS_KIND_INODES, inodes0);
        rc = pool_write(pool, inodes0, block);
    }
    if (rc == 0) {
        memset(block, 0, CFS_BLOCK);
        ((uint64_t *)(block + CFS_MHDR_SIZE))[0] = inodes0;
        mhdr_seal(&tmp, block, CFS_KIND_IMAP0, imap0);
        rc = pool_write(pool, imap0, block);
    }
    if (rc == 0) {
        memset(block, 0, CFS_BLOCK);
        ((uint64_t *)(block + CFS_MHDR_SIZE))[0] = imap0;
        mhdr_seal(&tmp, block, CFS_KIND_IMAP1, imap1);
        rc = pool_write(pool, imap1, block);
    }
    if (rc == 0)
        rc = pool_flush(pool);
    /* Superblock A at generation 1; slot B zeroed. */
    if (rc == 0) {
        memset(&tmp.sb, 0, sizeof(tmp.sb));
        memcpy(tmp.sb.magic, CFS_MAGIC, 8);
        tmp.sb.version = CFS_VERSION;
        tmp.sb.block_size = CFS_BLOCK;
        tmp.sb.total_blocks = nblocks;
        tmp.sb.generation = 1;
        tmp.sb.imap_root = imap1;
        tmp.sb.alloc_root = alloc_idx;
        tmp.sb.next_ino = 2;
        tmp.sb.inode_count = 1;
        tmp.sb.free_blocks = nblocks - used;
        tmp.sb.members = 1;
        tmp.pool = pool;
        memset(block, 0, CFS_BLOCK);
        rc = pool_write(pool, CFS_SUPER_B, block);
        if (rc == 0)
            rc = super_write(&tmp, CFS_SUPER_A);
        if (rc == 0)
            rc = pool_flush(pool);
    }
    kfree(block);
    pool_close(pool);
    if (rc == 0)
        kinfo("cosmofs: formatted %s: %llu blocks, %llu free", bd->name, (unsigned long long)nblocks,
              (unsigned long long)(nblocks - used));
    return rc;
}

/* --- mount / unmount ------------------------------------------------------------ */

static int super_read(struct cfs *fs, unsigned slot, struct cfs_super *out)
{
    uint8_t *block = kmalloc(CFS_BLOCK, 0);
    if (block == NULL)
        return -ENOMEM;
    int rc = pool_read(fs->pool, slot, block);
    if (rc == 0) {
        const struct cfs_super *sb = (const struct cfs_super *)block;
        if (memcmp(sb->magic, CFS_MAGIC, 8) != 0 || sb->version != CFS_VERSION || sb->block_size != CFS_BLOCK ||
            sb->total_blocks != fs->nblocks || sb->generation == 0 ||
            block_crc(block, offsetof(struct cfs_super, crc)) != sb->crc || sb->imap_root >= fs->nblocks ||
            sb->alloc_root >= fs->nblocks)
            rc = -EIO;
        else
            *out = *sb;
    }
    kfree(block);
    return rc;
}

static int load_bitmap(struct cfs *fs)
{
    fs->nr_chunks = (unsigned)((fs->nblocks + CFS_BITS_PER_BITMAP - 1) / CFS_BITS_PER_BITMAP);
    fs->bitmap = kzalloc((size_t)((fs->nblocks + 7) / 8));
    fs->bitmap_dirty = kzalloc(fs->nr_chunks);
    if (fs->bitmap == NULL || fs->bitmap_dirty == NULL)
        return -ENOMEM;
    struct cfs_buf *idx;
    int rc = cfs_buf_get(fs, fs->sb.alloc_root, CFS_KIND_ALLOCIDX, &idx);
    if (rc)
        return rc;
    uint64_t *slots = ptrs(idx);
    uint64_t free = 0;
    for (unsigned c = 0; c < fs->nr_chunks && rc == 0; c++) {
        struct cfs_buf *bm;
        rc = cfs_buf_get(fs, slots[c], CFS_KIND_BITMAP, &bm);
        if (rc)
            break;
        size_t bytes = CFS_BITS_PER_BITMAP / 8;
        size_t off = (size_t)c * bytes;
        size_t total = (size_t)((fs->nblocks + 7) / 8);
        size_t n = off + bytes <= total ? bytes : total - off;
        memcpy(fs->bitmap + off, bm->data + CFS_MHDR_SIZE, n);
        cfs_buf_put(fs, bm);
    }
    cfs_buf_put(fs, idx);
    if (rc)
        return rc;
    for (uint64_t i = 2; i < fs->nblocks; i++)
        free += bit_test(fs->bitmap, i) ? 0 : 1;
    fs->free_blocks = free;
    if (free != fs->sb.free_blocks)
        kwarn("cosmofs: free block count %llu differs from the superblock's %llu; using the bitmap",
              (unsigned long long)free, (unsigned long long)fs->sb.free_blocks);
    return 0;
}

static void cfs_destroy(struct cfs *fs)
{
    struct cfs_buf *b, *tmp;
    list_for_each_entry_safe(b, tmp, &fs->bufs, link) {
        list_remove(&b->link);
        kfree(b->data);
        kfree(b);
    }
    kfree(fs->bitmap);
    kfree(fs->bitmap_dirty);
    kfree(fs->pending_free);
    if (fs->pool)
        pool_close(fs->pool);
    kfree(fs);
}

static int cosmofs_mount(struct fs_type *fst, struct blkdev *bdev, unsigned flags, struct mount *mnt)
{
    (void)fst;
    (void)flags;
    if (bdev == NULL)
        return -EINVAL;
    struct cfs *fs = kzalloc(sizeof(*fs));
    if (fs == NULL)
        return -ENOMEM;
    list_init(&fs->bufs);
    mutex_init(&fs->lock, "cosmofs");
    fs->mnt = mnt;
    int rc = pool_open(bdev, &fs->pool);
    if (rc) {
        kfree(fs);
        return rc;
    }
    fs->nblocks = fs->pool->nblocks;

    struct cfs_super a, b;
    int ra = super_read(fs, CFS_SUPER_A, &a);
    int rb = super_read(fs, CFS_SUPER_B, &b);
    if (ra && rb) {
        kerror("cosmofs: %s: no valid superblock", bdev->name);
        cfs_destroy(fs);
        return -EIO;
    }
    if (ra == 0 && (rb != 0 || a.generation >= b.generation)) {
        fs->sb = a;
        fs->sb_slot = CFS_SUPER_A;
    } else {
        fs->sb = b;
        fs->sb_slot = CFS_SUPER_B;
    }
    fs->gen = fs->sb.generation + 1;
    fs->alloc_hint = 2;
    rc = load_bitmap(fs);
    if (rc) {
        cfs_destroy(fs);
        return rc;
    }
    mnt->fs_priv = fs;
    struct vnode *root;
    rc = cfs_vnode_get(fs, CFS_ROOT_INO, &root);
    if (rc) {
        mnt->fs_priv = NULL;
        cfs_destroy(fs);
        return rc;
    }
    mnt->root = root;
    kinfo("cosmofs: %s: generation %llu, %llu/%llu blocks free, %llu inodes", bdev->name,
          (unsigned long long)fs->sb.generation, (unsigned long long)fs->free_blocks,
          (unsigned long long)fs->nblocks, (unsigned long long)fs->sb.inode_count);
    return 0;
}

static int cosmofs_sync(struct mount *mnt)
{
    struct cfs *fs = cfs_of(mnt);
    if (fs == NULL)
        return 0;
    if (fs->discard_on_unmount)
        return 0;   /* test hook: behave as if the root write never happened */
    int rc = cfs_sync_vnodes(fs);
    if (rc)
        return rc;
    mutex_lock(&fs->lock);
    rc = cfs_commit(fs);
    mutex_unlock(&fs->lock);
    return rc;
}

/* The VFS committed through cosmofs_sync before calling this; what is
 * left is either nothing, a deliberately discarded transaction (test
 * hook) or an abandoned one (cfs_fail), and both are dropped here so the
 * on-disk state stays at the last committed root. */
static int cosmofs_unmount(struct mount *mnt)
{
    struct cfs *fs = cfs_of(mnt);
    if (fs->discard_on_unmount)
        kwarn("cosmofs: discarding the open transaction (test hook)");
    else if (fs->failed)
        kwarn("cosmofs: dropping the abandoned transaction (%d)", fs->failed);
    mnt->fs_priv = NULL;   /* the root's eviction sees no filesystem */
    cfs_destroy(fs);
    return 0;
}

int cosmofs_stats(struct mount *mnt, struct cosmofs_stats *out)
{
    struct cfs *fs = cfs_of(mnt);
    if (fs == NULL)
        return -EINVAL;
    mutex_lock(&fs->lock);
    out->generation = fs->sb.generation;
    out->free_blocks = fs->free_blocks;
    out->total_blocks = fs->nblocks;
    out->inode_count = fs->sb.inode_count;
    out->dirty_buffers = fs->nr_dirty;
    out->pending_frees = fs->nr_pending;
    mutex_unlock(&fs->lock);
    return 0;
}

void cosmofs_test_discard_on_unmount(struct mount *mnt, bool discard)
{
    struct cfs *fs = cfs_of(mnt);
    if (fs)
        fs->discard_on_unmount = discard;
}

struct fs_type cosmofs_fs_type = {
    .name = "cosmofs",
    .mount = cosmofs_mount,
    .unmount = cosmofs_unmount,
    .sync = cosmofs_sync,
};

void cosmofs_init(void)
{
    if (vfs_register_fs(&cosmofs_fs_type))
        panic("cosmofs: cannot register");
}
