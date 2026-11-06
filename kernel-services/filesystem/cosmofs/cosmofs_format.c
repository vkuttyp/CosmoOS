/*
 * cosmofs_format.c - Making a pool: one device or several, mirrored or
 * not, at any format version this kernel writes, in the clear or
 * encrypted with a key the kernel wraps or slots user space sealed; and
 * the test hooks that write an older format or poison a record. Moved out
 * of cosmofs_core.c (roadmap 2.0 SA7) so that it can be held to a digest
 * and made CeeSafe on its own.
 */

#include <kernel/blk.h>
#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/random.h>
#include <kernel/storage.h>
#include <kernel/string.h>

#include "cosmofs_internal.h"

/* --- format ------------------------------------------------------------------ */

/*
 * Format `n` devices as one pool. Member 0 carries the superblocks, the
 * member table and its own allocation metadata; every other member
 * carries a label at block 0 and its own allocation metadata, so that
 * losing one member does not take another's bitmap with it (design.md,
 * "The member table").
 */
/*
 * `copies` devices per member, taken from `bd` member by member: with
 * copies = 2 and n = 2, bd[0] and bd[1] are member 0's mirror and bd[2]
 * and bd[3] are member 1's.
 */
/* The user key an encrypted format wraps its master key with; NULL for
 * a filesystem in the clear. */
static const void *g_format_key;
static size_t g_format_key_len;

static int format_at(struct blkdev **bd, unsigned n, unsigned copies, unsigned version, const char *label,
                     const struct cfs_format_keys *fk)
{
    if (n == 0 || n > CFS_MAX_MEMBERS || copies == 0 || copies > CFS_MAX_COPIES)
        return -EINVAL;
    if (version < 4 && (n != 1 || copies != 1))
        return -EINVAL;   /* one device is all the older formats can name */
    if (version < 5 && copies != 1)
        return -EINVAL;   /* a mirror group is what version 5 adds */
    struct spool *pool;
    int rc = pool_open(bd[0], &pool);
    if (rc)
        return rc;
    for (unsigned c = 1; c < copies && rc == 0; c++)
        rc = pool_add_copy(pool, 0, bd[c]);
    for (unsigned v = 1; v < n && rc == 0; v++) {
        rc = pool_add_member(pool, bd[v * copies], NULL);
        for (unsigned c = 1; c < copies && rc == 0; c++)
            rc = pool_add_copy(pool, v, bd[v * copies + c]);
    }
    if (rc) {
        pool_close(pool);
        return rc;
    }

    /* On the heap: 255 members of 64 bytes are the whole of a 16 KiB
     * kernel stack. A self-test's format never noticed; a format from a
     * system call (/dev/blkctl, roadmap M2) double-faulted. */
    struct cfs_member *mem = kmalloc(sizeof(*mem) * n, KMEM_ZERO);
    if (mem == NULL) {
        pool_close(pool);
        return -ENOMEM;
    }
    uint8_t uuid[16];
    if (fk != NULL)
        memcpy(uuid, fk->uuid, sizeof(uuid));   /* the slots were sealed for it */
    else
        random_get_bytes(uuid, sizeof(uuid));
    uint64_t total = 0, free_total = 0;
    for (unsigned v = 0; v < n; v++) {
        uint64_t nb = pool->m[v].nblocks;
        if (nb < CFS_MIN_BLOCKS || nb > CFS_MAX_BLOCKS) {
            kfree(mem);
            pool_close(pool);
            return -EINVAL;
        }
        unsigned chunks = (unsigned)((nb + CFS_BITS_PER_BITMAP - 1) / CFS_BITS_PER_BITMAP);
        if (chunks > CFS_PTRS_PER_BLOCK) {
            kfree(mem);
            pool_close(pool);
            return -EINVAL;
        }
        memcpy(mem[v].uuid, uuid, 16);
        mem[v].nblocks = nb;
        mem[v].copies = copies;
        mem[v].first_usable = v == 0 ? 2 : 1;
        total += nb;
    }

    /* Member 0: 0,1 superblocks; 2 member table; 3 alloc index; 4..
     * bitmaps; then imap L1, imap L0, inode block 0.
     * Member v>0: 0 label; 1 alloc index; 2.. bitmaps. */
    unsigned chunks0 = (unsigned)((mem[0].nblocks + CFS_BITS_PER_BITMAP - 1) / CFS_BITS_PER_BITMAP);
    bool v4 = version >= 4;
    /* Version 14 (design.md, "Making an encrypted pool"): the master key
     * and slots user space sealed, and every block MAC'd under it. */
    bool keyed = fk != NULL;
    if (keyed)
        version = 14;
    bool crypt = keyed || (version >= 7 && g_format_key != NULL);
    /* Before version 4 there is no member table, and the allocation
     * index is at block 2 where that format put it. */
    uint64_t members_blk = 2, alloc_idx = v4 ? 3 : 2, bitmap0 = v4 ? 4 : 3;
    uint64_t imap1 = bitmap0 + chunks0, imap0 = imap1 + 1, inodes0 = imap0 + 1;
    uint64_t keys_blk = inodes0 + 1;
    uint64_t used0 = crypt ? keys_blk + 1 : inodes0 + 1;

    uint8_t *block = kmalloc(CFS_BLOCK, KMEM_ZERO);
    if (block == NULL) {
        kfree(mem);
        pool_close(pool);
        return -ENOMEM;
    }
    struct cfs tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.gen = 1;
    tmp.pool = pool;
    if (keyed)
        cfs_mac_derive(fk->master, &tmp.mac);

    for (unsigned v = 0; v < n && rc == 0; v++) {
        uint64_t nb = mem[v].nblocks;
        unsigned chunks = (unsigned)((nb + CFS_BITS_PER_BITMAP - 1) / CFS_BITS_PER_BITMAP);
        uint64_t idx_blk = v == 0 ? alloc_idx : 1;
        (void)members_blk;
        uint64_t bm0 = v == 0 ? bitmap0 : 2;
        uint64_t used = v == 0 ? used0 : bm0 + chunks;
        for (unsigned c = 0; c < chunks && rc == 0; c++) {
            memset(block, 0, CFS_BLOCK);
            uint8_t *bits = block + CFS_MHDR_SIZE;
            for (uint64_t i = (uint64_t)c * CFS_BITS_PER_BITMAP; i < ((uint64_t)c + 1) * CFS_BITS_PER_BITMAP; i++) {
                if (i >= nb || i < used)
                    cfs_bit_set(bits, i - (uint64_t)c * CFS_BITS_PER_BITMAP);   /* past-the-end bits stay allocated */
            }
            cfs_mhdr_seal(&tmp, block, CFS_KIND_BITMAP, CFS_DVA(v, bm0 + c));
            rc = pool_write(pool, CFS_DVA(v, bm0 + c), block);
        }
        if (rc == 0) {
            memset(block, 0, CFS_BLOCK);
            uint64_t *p = (uint64_t *)(block + CFS_MHDR_SIZE);
            for (unsigned c = 0; c < chunks; c++)
                p[c] = CFS_DVA(v, bm0 + c);
            cfs_mhdr_seal(&tmp, block, CFS_KIND_ALLOCIDX, CFS_DVA(v, idx_blk));
            rc = pool_write(pool, CFS_DVA(v, idx_blk), block);
        }
        /* Generation 1: the commit this device took part in. */
        for (unsigned c = 0; c < copies && rc == 0 && v > 0; c++)
            rc = cfs_label_write(pool, &tmp.mac, v, c, 1, uuid, nb);
        mem[v].alloc_root = CFS_DVA(v, idx_blk);
        mem[v].free_blocks = nb - used;
        free_total += nb - used;
    }

    /* The member table. */
    if (rc == 0 && v4) {
        memset(block, 0, CFS_BLOCK);
        struct cfs_member_block *mb = (struct cfs_member_block *)(block + CFS_MHDR_SIZE);
        mb->count = n;
        for (unsigned v = 0; v < n; v++)
            mb->m[v] = mem[v];
        cfs_mhdr_seal(&tmp, block, CFS_KIND_MEMBERS, CFS_DVA(0, members_blk));
        rc = pool_write(pool, CFS_DVA(0, members_blk), block);
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
        /* An encrypted filesystem authenticates rather than checksums:
         * a CRC cannot tell a deliberate change from an accident. */
        root->csum_algo = crypt ? CFS_CSUM_POLY1305 : CFS_CSUM_CRC32C;
        root->compress_algo = CFS_COMPRESS_NONE;
        cfs_mhdr_seal(&tmp, block, CFS_KIND_INODES, inodes0);
        rc = pool_write(pool, inodes0, block);
    }
    if (rc == 0) {
        memset(block, 0, CFS_BLOCK);
        ((uint64_t *)(block + CFS_MHDR_SIZE))[0] = inodes0;
        cfs_mhdr_seal(&tmp, block, CFS_KIND_IMAP0, imap0);
        rc = pool_write(pool, imap0, block);
    }
    if (rc == 0) {
        memset(block, 0, CFS_BLOCK);
        ((uint64_t *)(block + CFS_MHDR_SIZE))[0] = imap0;
        cfs_mhdr_seal(&tmp, block, CFS_KIND_IMAP1, imap1);
        rc = pool_write(pool, imap1, block);
    }
    uint8_t master[CHACHA20_KEY_SIZE];
    if (rc == 0 && keyed) {
        rc = cfs_keys2_write(pool, &tmp.mac, CFS_DVA(0, keys_blk), 1, uuid, fk->slot);
    } else if (rc == 0 && crypt) {
        random_get_bytes(master, sizeof(master));
        rc = cfs_keys_write(pool, CFS_DVA(0, keys_blk), 1, master, g_format_key, g_format_key_len);
        memset(master, 0, sizeof(master));
    }
    if (rc == 0)
        rc = pool_flush(pool);
    /* Superblock A at generation 1; slot B zeroed. Member 0's addresses
     * are numerically what they always were, so a version-3 reader sees
     * the same layout it would have written. */
    if (rc == 0) {
        memset(&tmp.sb, 0, sizeof(tmp.sb));
        memcpy(tmp.sb.magic, CFS_MAGIC, 8);
        tmp.sb.version = version;
        tmp.sb.block_size = CFS_BLOCK;
        tmp.sb.total_blocks = total;
        tmp.sb.generation = 1;
        tmp.sb.imap_root = imap1;
        tmp.sb.alloc_root = v4 ? 0 : CFS_DVA(0, alloc_idx);
        tmp.sb.next_ino = 2;
        tmp.sb.inode_count = 1;
        tmp.sb.free_blocks = free_total;
        tmp.sb.members = v4 ? CFS_DVA(0, members_blk) : 1;
        tmp.sb.key_root = crypt ? CFS_DVA(0, keys_blk) : 0;
        if (v4)
            memcpy(tmp.sb.uuid, uuid, 16);
        if (label != NULL)
            memcpy(tmp.sb.label, label, strnlen(label, CFS_LABEL_MAX));
        memset(block, 0, CFS_BLOCK);
        rc = pool_write(pool, CFS_SUPER_B, block);
        if (rc == 0)
            rc = cfs_super_write(&tmp, CFS_SUPER_A, BIO_PREFLUSH | BIO_FUA);
    }
    /* The installer mounts what it just made: the key is loaded, as the
     * boot's unlock would load it. */
    if (rc == 0 && keyed)
        rc = cfs_keytab_install(uuid, fk->master);
    memset(&tmp.mac, 0, sizeof(tmp.mac));
    kfree(block);
    kfree(mem);
    pool_close(pool);
    if (rc == 0)
        kinfo("cosmofs: formatted %s%s: %u member(s), %llu blocks, %llu free%s", bd[0]->name,
              n > 1 ? " and others" : "", n, (unsigned long long)total, (unsigned long long)free_total,
              keyed ? ", encrypted (format 14)" : "");
    return rc;
}

int cosmofs_format_pool(struct blkdev **bd, unsigned n)
{
    return format_at(bd, n, 1, CFS_FORMAT_VERSION, NULL, NULL);
}

int cosmofs_format_mirror(struct blkdev **bd, unsigned n, unsigned copies)
{
    return format_at(bd, n, copies, CFS_FORMAT_VERSION, NULL, NULL);
}

/* The VFS's mirror format: one member, a copy on each device, named
 * `label` (NULL or "" for none; roadmap 1.2 B3, an installed mirrored
 * root is named as a single one is). */
int cfs_format_mirror_devs(struct blkdev **bd, unsigned n, const char *label)
{
    if (label != NULL && strlen(label) > CFS_LABEL_MAX)
        return -EINVAL;
    return format_at(bd, 1, n, CFS_FORMAT_VERSION, label, NULL);
}

int cosmofs_format(struct blkdev *bd)
{
    return format_at(&bd, 1, 1, CFS_FORMAT_VERSION, NULL, NULL);
}

/* A single-device pool named `label` (NULL or "" for none): the fs_type's
 * format, which /dev/blkctl reaches (shell completeness S6). */
int cosmofs_format_label(struct blkdev *bd, const char *label)
{
    if (label != NULL && strlen(label) > CFS_LABEL_MAX)
        return -EINVAL;
    return format_at(&bd, 1, 1, CFS_FORMAT_VERSION, label, NULL);
}

/* Format with encryption: a random master key, wrapped with `key`. */
int cosmofs_format_encrypted(struct blkdev *bd, const void *key, size_t len)
{
    if (key == NULL || len == 0)
        return -EINVAL;
    int rc = cfs_need_seeded("create an encrypted pool");
    if (rc)
        return rc;
    g_format_key = key;
    g_format_key_len = len;
    rc = format_at(&bd, 1, 1, CFS_FORMAT_VERSION, NULL, NULL);
    g_format_key = NULL;
    g_format_key_len = 0;
    return rc;
}

/*
 * An encrypted pool (roadmap 1.2 B5): one member on `n` devices (a
 * mirror when n is 2), version 14, the slots as user space sealed them.
 * At least one slot must be usable; a key-file slot only at 0.
 */
int cosmofs_format_keyed(struct blkdev **bd, unsigned n, const char *label, const struct cfs_format_keys *fk)
{
    if (label != NULL && strlen(label) > CFS_LABEL_MAX)
        return -EINVAL;
    unsigned usable = 0;
    for (unsigned i = 0; i < CFS_KEY_SLOTS; i++) {
        if (!cfs_keyslot_valid(i, &fk->slot[i]))
            return -EINVAL;
        usable += fk->slot[i].kind != CFS_KEYSLOT_EMPTY;
    }
    static const uint8_t zero[16] = { 0 };
    if (usable == 0 || memcmp(fk->uuid, zero, 16) == 0)
        return -EINVAL;   /* no way in; or no uuid, which is how a pool before version 4 says it has none */
    int rc = cfs_need_seeded("create an encrypted pool");
    if (rc)
        return rc;
    return format_at(bd, 1, n, 14, label, fk);
}

int cosmofs_format_keys(struct blkdev **bd, unsigned n, const char *label, const uint8_t uuid[16], const uint8_t master[32],
                        const struct cosmo_keyslot *slots)
{
    struct cfs_format_keys *fk = kmalloc(sizeof(*fk), 0);
    if (fk == NULL)
        return -ENOMEM;
    memcpy(fk->uuid, uuid, sizeof(fk->uuid));
    memcpy(fk->master, master, sizeof(fk->master));
    memcpy(fk->slot, slots, sizeof(fk->slot));
    int rc = cosmofs_format_keyed(bd, n, label, fk);
    memset(fk, 0, sizeof(*fk));
    kfree(fk);
    return rc;
}

#if CONFIG_DEBUG
/* Test hook: an encrypted pool of `members` single-device members, so the
 * labels -- which only a member past the first has -- are MAC'd and can
 * be forged (cosmofs-encrypted-root). */
int cosmofs_test_format_keyed_members(struct blkdev **bd, unsigned members, const uint8_t uuid[16], const uint8_t master[32],
                                      const struct cosmo_keyslot *slots)
{
    struct cfs_format_keys *fk = kmalloc(sizeof(*fk), 0);
    if (fk == NULL)
        return -ENOMEM;
    memcpy(fk->uuid, uuid, sizeof(fk->uuid));
    memcpy(fk->master, master, sizeof(fk->master));
    memcpy(fk->slot, slots, sizeof(fk->slot));
    int rc = format_at(bd, members, 1, 14, NULL, fk);
    memset(fk, 0, sizeof(*fk));
    kfree(fk);
    return rc;
}
#endif

/* Test hook: write an older format, so that "versions 2 and 3 mount
 * unchanged" is a claim something checks (design.md, "The DVA is 64
 * bits"). */
int cosmofs_test_format_version(struct blkdev *bd, unsigned version)
{
    if (version < CFS_VERSION_MIN || version > CFS_VERSION)
        return -EINVAL;
    return format_at(&bd, 1, 1, version, NULL, NULL);
}

/*
 * Test hook: write a value into the superblock word that version 9 calls
 * `free_root`, on a filesystem too old to have one.
 *
 * Every image this tree formats zeroes its reserved words, so the gate
 * that stops an older filesystem's reserved zero being read as a chain
 * head is unobservable without this: the bug-proof for that gate passes
 * on a zero either way. An image from another writer -- a later version
 * that used the word, a different implementation -- is what the gate is
 * actually for, and this manufactures one.
 */
/*
 * Write a count past the payload into the record the live root names: a
 * block whose checksum is good and whose contents are not. The live root
 * is the valid slot with the higher generation, which is not always slot
 * 0 -- a commit writes the other one.
 */
int cosmofs_test_poison_freelog_count(struct blkdev *bd, uint64_t count)
{
    struct spool *pool = NULL;
    int rc = pool_open(bd, &pool);
    if (rc)
        return rc;
    uint8_t *block = kmalloc(CFS_BLOCK, KMEM_ZERO);
    if (block == NULL) {
        pool_close(pool);
        return -ENOMEM;
    }
    uint64_t at = 0, best_gen = 0;
    for (unsigned slot = 0; slot < 2; slot++) {
        if (pool_read(pool, slot, block))
            continue;
        const struct cfs_super *sb = (const struct cfs_super *)block;
        uint32_t want = sb->crc;
        ((struct cfs_super *)block)->crc = 0;
        uint32_t have = cfs_block_crc(block, offsetof(struct cfs_super, crc));
        if (want != have || sb->generation == 0 || sb->generation < best_gen)
            continue;
        best_gen = sb->generation;
        at = sb->free_root;
    }
    rc = at == 0 ? -ENOENT : 0;
    if (rc == 0)
        rc = pool_read(pool, at, block);
    if (rc == 0) {
        struct cfs_dead_block *d = (struct cfs_dead_block *)(block + CFS_MHDR_SIZE);
        d->count = count;
        struct cfs_mhdr *h = (struct cfs_mhdr *)block;
        h->crc = 0;
        h->crc = cfs_block_crc(block, offsetof(struct cfs_mhdr, crc));
        rc = pool_write(pool, at, block);
        if (rc == 0)
            rc = pool_flush(pool);
    }
    kfree(block);
    pool_close(pool);
    return rc;
}

int cosmofs_test_poison_free_root(struct blkdev *bd, uint64_t value)
{
    struct spool *pool = NULL;
    int rc = pool_open(bd, &pool);
    if (rc)
        return rc;
    uint8_t *block = kmalloc(CFS_BLOCK, KMEM_ZERO);
    if (block == NULL) {
        pool_close(pool);
        return -ENOMEM;
    }
    rc = pool_read(pool, 0, block);
    if (rc == 0) {
        struct cfs_super *sb = (struct cfs_super *)block;
        sb->free_root = value;
        sb->crc = 0;
        sb->crc = cfs_block_crc(block, offsetof(struct cfs_super, crc));
        rc = pool_write(pool, 0, block);
        if (rc == 0)
            rc = pool_flush(pool);
    }
    kfree(block);
    pool_close(pool);
    return rc;
}
