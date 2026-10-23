/*
 * cosmofs_keytest.c - Self-test of format version 14, an encrypted root's
 * pool (roadmap 1.2 B5; docs/kernel-services/filesystem/cosmofs/design.md,
 * "Format version 14"): the key table, the key operations, the metadata
 * MAC in every domain, and the two-level checksum index.
 *
 * Everything here runs on RAM devices of its own. The slots are not
 * sealed: the kernel never opens one (cosmo-key does, in user space), so
 * a slot here only has to have a slot's shape.
 */

#include <kernel/blk.h>
#include <kernel/cosmofs.h>
#include <kernel/crc32c.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/ramblk.h>
#include <kernel/random.h>
#include <kernel/selftest.h>
#include <kernel/storage.h>
#include <kernel/string.h>
#include <kernel/vfs.h>

#include <uapi/cosmo/fsctl.h>

#include "cosmofs_format.h"
#include "cosmofs_internal.h"

#define STR_(x) #x
#define STR(x)  STR_(x)
#define KT_CHECK(cond)                                                         \
    do {                                                                       \
        if (!(cond)) {                                                         \
            *reason = "check failed: " #cond " at line " STR(__LINE__);        \
            goto out;                                                          \
        }                                                                      \
    } while (0)

#define KT "/mnt/keytest"
#define KT_FAR_LBLK 70000u   /* past the one-level index of an encrypted file: 508 * 127 = 64516 */

static void kt_slots(struct cosmo_keyslot s[COSMO_KEY_SLOTS])
{
    memset(s, 0, sizeof(struct cosmo_keyslot) * COSMO_KEY_SLOTS);
    s[0].kind = COSMO_KEYSLOT_PASSPHRASE;
    s[0].t_cost = 1;
    s[0].m_cost_kib = 8;
    s[0].lanes = 1;
    random_get_bytes(s[0].salt, sizeof(s[0].salt));
    random_get_bytes(s[0].wrapped, sizeof(s[0].wrapped));
    random_get_bytes(s[0].tag, sizeof(s[0].tag));
}

static int kt_info(struct blkdev *bd, struct mount *mnt, struct cosmo_fsctl_keyinfo *info)
{
    struct cosmofs_key_req req = { .op = COSMOFS_KEY_INFO, .info = info };
    return cosmofs_keyctl(bd, mnt, &req);
}

static int kt_load(struct blkdev *bd, const uint8_t key[32])
{
    struct cosmofs_key_req req = { .op = COSMOFS_KEY_LOAD, .key = key };
    return cosmofs_keyctl(bd, NULL, &req);
}

static int kt_slot(struct mount *mnt, unsigned slot, const uint8_t key[32], const struct cosmo_keyslot *rec)
{
    struct cosmofs_key_req req = { .op = COSMOFS_KEY_SLOT, .slot = slot, .key = key, .rec = rec };
    return cosmofs_keyctl(NULL, mnt, &req);
}

static struct mount *kt_mount(void)
{
    struct vnode *vn;
    if (vfs_lookup(NULL, KT, &vn))
        return NULL;
    struct mount *m = vn->mnt;
    vnode_put(vn);
    return m;
}

static int kt_mount_dev(struct blkdev *bd)
{
    int rc = vfs_mount(KT, "cosmofs", bd, 0);
    if (rc == 0)
        cosmofs_test_set_writeback(kt_mount(), false);
    return rc;
}

/*
 * What a forger with the disk and no key does: change a byte, then make
 * the block look sealed the old way -- csum_algo CRC32C and a CRC32C
 * over the block with the CRC field zero, at `crc_off`. `save` gets the
 * block as it was.
 */
static bool kt_forge(struct blkdev *bd, uint64_t dva, size_t crc_off, bool header, size_t touch, uint8_t *save)
{
    struct spool *p;
    if (pool_open(bd, &p))
        return false;
    uint8_t *b = kmalloc(CFS_BLOCK, 0);
    bool ok = b != NULL && pool_read(p, dva, b) == 0;
    if (ok) {
        memcpy(save, b, CFS_BLOCK);
        b[touch] ^= 0x01;
        if (header)
            ((struct cfs_mhdr *)b)->csum_algo = CFS_CSUM_CRC32C;
        memset(b + crc_off, 0, 4);
        uint32_t c = crc32c(b, CFS_BLOCK);
        memcpy(b + crc_off, &c, 4);
        ok = pool_write(p, dva, b) == 0 && pool_flush(p) == 0;
    }
    kfree(b);
    pool_close(p);
    return ok;
}

static bool kt_put_back(struct blkdev *bd, uint64_t dva, const uint8_t *save)
{
    struct spool *p;
    if (pool_open(bd, &p))
        return false;
    bool ok = pool_write(p, dva, save) == 0 && pool_flush(p) == 0;
    pool_close(p);
    return ok;
}

static bool kt_super(struct blkdev *bd, struct cfs_super *sb)
{
    struct spool *p;
    if (pool_open(bd, &p))
        return false;
    bool ok = cfs_super_peek(p, sb) == 0;
    pool_close(p);
    return ok;
}

static bool kt_write_at(const char *path, uint64_t off, const void *data, size_t len)
{
    struct file *f;
    if (vfs_open(NULL, path, COSMO_O_WRONLY | COSMO_O_CREAT, 0644, &f))
        return false;
    int64_t n = file_pwrite(f, data, len, off);
    file_put(f);
    return n == (int64_t)len;
}

static bool kt_read_at(const char *path, uint64_t off, const void *want, size_t len)
{
    struct file *f;
    if (vfs_open(NULL, path, COSMO_O_RDONLY, 0, &f))
        return false;
    uint8_t *buf = kmalloc(len, 0);
    bool ok = buf != NULL && file_pread(f, buf, len, off) == (int64_t)len && memcmp(buf, want, len) == 0;
    kfree(buf);
    file_put(f);
    return ok;
}

/* The block a file's two-level checksum root names at `entry`: a CSUMIDX. */
static uint64_t kt_csum_mid(struct mount *mnt, const char *path, unsigned entry, uint32_t *root_kind)
{
    struct file *f;
    if (vfs_open(NULL, path, COSMO_O_RDONLY, 0, &f))
        return 0;
    uint64_t ino = f->vn->ino;
    file_put(f);
    struct cfs *fs = cfs_of(mnt);
    uint64_t mid = 0;
    mutex_lock(&fs->lock);
    struct cfs_inode in;
    struct cfs_buf *b;
    if (cfs_inode_read(fs, ino, &in) == 0 && cfs_buf_get(fs, in.csum_root, 0, &b) == 0) {
        *root_kind = cfs_buf_hdr(b)->kind;
        mid = ((const uint64_t *)(b->data + CFS_MHDR_SIZE))[entry];
        cfs_buf_put(fs, b);
    }
    mutex_unlock(&fs->lock);
    return mid;
}

bool selftest_cosmofs_encrypted_root(const char **reason)
{
    if (!random_ready()) {
        kinfo("selftest: cosmofs-encrypted-root: the entropy pool is not seeded; skipping");
        return true;
    }
    (void)vfs_umount2(KT, VFS_UMOUNT_FORCE);
    bool ok = false, mounted = false;
    struct blkdev *bd = ramblk_create(4096), *two[2] = { NULL, NULL }, *plain = NULL;
    uint8_t master[32], wrong[32], master2[32];
    struct cosmo_keyslot *slots = kmalloc(sizeof(*slots) * COSMO_KEY_SLOTS, 0);
    struct cosmo_fsctl_keyinfo *info = kmalloc(sizeof(*info), 0);
    uint8_t *save = kmalloc(CFS_BLOCK, 0), *save2 = kmalloc(CFS_BLOCK, 0), *blk = kmalloc(CFS_BLOCK, 0);
    static const char secret[] = "an encrypted root keeps this sentence off the disk";
    struct cosmofs_check_report *rep = kmalloc(sizeof(*rep), 0);
    struct cosmofs_dev_report dev;
    struct cfs_super sb;
    KT_CHECK(bd != NULL && slots != NULL && info != NULL && save != NULL && save2 != NULL && blk != NULL &&
             rep != NULL);
    random_get_bytes(master, sizeof(master));
    random_get_bytes(master2, sizeof(master2));
    memcpy(wrong, master, sizeof(wrong));
    wrong[7] ^= 0x40;
    for (unsigned i = 0; i < CFS_BLOCK; i++)
        blk[i] = (uint8_t)(i * 13 + 1);
    kt_slots(slots);
    int mk = vfs_mkdir(NULL, KT, 0755);
    KT_CHECK(mk == 0 || mk == -EEXIST);

    /* A slot of the wrong shape, and no usable slot at all, are refused
     * before anything is written. */
    struct cosmo_keyslot bad[COSMO_KEY_SLOTS];
    memcpy(bad, slots, sizeof(bad));
    bad[3].kind = COSMO_KEYSLOT_KEYFILE;
    KT_CHECK(cosmofs_format_keys(&bd, 1, "keytest", master, bad) == -EINVAL);
    memset(bad, 0, sizeof(bad));
    KT_CHECK(cosmofs_format_keys(&bd, 1, "keytest", master, bad) == -EINVAL);

    /* Made at 14, MAC'd, and unlocked by the format: it mounts at once. */
    KT_CHECK(cosmofs_format_keys(&bd, 1, "keytest", master, slots) == 0);
    KT_CHECK(kt_super(bd, &sb) && sb.version == 14 && sb.key_root != 0 && cfs_super_is_mac(&sb));
    KT_CHECK(kt_mount_dev(bd) == 0);
    mounted = true;
    KT_CHECK(kt_write_at(KT "/secret", 0, secret, sizeof(secret) - 1));
    /* A sparse file with a block past the one-level index: its checksum
     * root becomes a CSUMIDX2 whose entry 0 is the old index (CFS-10). */
    KT_CHECK(kt_write_at(KT "/far", 0, blk, CFS_BLOCK));
    KT_CHECK(kt_write_at(KT "/far", (uint64_t)KT_FAR_LBLK * CFS_BLOCK, blk, CFS_BLOCK));
    KT_CHECK(vfs_sync() == 0);
    KT_CHECK(kt_read_at(KT "/far", 0, blk, CFS_BLOCK));
    KT_CHECK(kt_read_at(KT "/far", (uint64_t)KT_FAR_LBLK * CFS_BLOCK, blk, CFS_BLOCK));
    uint32_t root_kind = 0;
    uint64_t mid = kt_csum_mid(kt_mount(), KT "/far", KT_FAR_LBLK / 127 / CFS_PTRS_PER_BLOCK, &root_kind);
    KT_CHECK(root_kind == CFS_KIND_CSUMIDX2 && mid != 0);
    KT_CHECK(cosmofs_check(kt_mount(), rep, 0) == 0 && rep->clean && rep->csum_two_level == 1);
    KT_CHECK(kt_info(NULL, kt_mount(), info) == 0);
    KT_CHECK((info->flags & (COSMO_FSCTL_KEY_F_ENCRYPTED | COSMO_FSCTL_KEY_F_MAC | COSMO_FSCTL_KEY_F_UNLOCKED)) ==
             (COSMO_FSCTL_KEY_F_ENCRYPTED | COSMO_FSCTL_KEY_F_MAC | COSMO_FSCTL_KEY_F_UNLOCKED));
    KT_CHECK(info->nslots == COSMO_KEY_SLOTS && info->slot[0].kind == COSMO_KEYSLOT_PASSPHRASE &&
             memcmp(info->slot[0].salt, slots[0].salt, 16) == 0);

    /* The slots of a mounted pool: written with its master key only, and
     * never all emptied. */
    struct cosmo_keyslot rec = slots[0], empty;
    memset(&empty, 0, sizeof(empty));
    random_get_bytes(rec.salt, sizeof(rec.salt));
    KT_CHECK(kt_slot(kt_mount(), 1, wrong, &rec) == -EKEYREJECTED);
    KT_CHECK(kt_slot(kt_mount(), 1, master, &rec) == 0);
    KT_CHECK(kt_slot(kt_mount(), 0, master, &empty) == 0);
    KT_CHECK(kt_slot(kt_mount(), 1, master, &empty) == -EBUSY);

    /* The unmount takes the key with it: the pool is locked, so it neither
     * mounts nor checks, and says which. */
    KT_CHECK(vfs_umount(KT) == 0);
    mounted = false;
    KT_CHECK(kt_info(bd, NULL, info) == 0);
    KT_CHECK((info->flags & COSMO_FSCTL_KEY_F_MAC) && !(info->flags & COSMO_FSCTL_KEY_F_UNLOCKED));
    KT_CHECK(info->slot[0].kind == COSMO_KEYSLOT_EMPTY && info->slot[1].kind == COSMO_KEYSLOT_PASSPHRASE &&
             memcmp(info->slot[1].salt, rec.salt, 16) == 0);   /* the slot change was durable */
    KT_CHECK(kt_mount_dev(bd) == -ENOKEY);
    KT_CHECK(cosmofs_check_dev(bd, 0, rep, &dev) == 0 && (dev.flags & COSMO_FSCTL_DEV_F_LOCKED));
    KT_CHECK(kt_load(bd, wrong) == -EKEYREJECTED);
    KT_CHECK(kt_load(bd, master) == 0);
    KT_CHECK(cosmofs_check_dev(bd, 0, rep, &dev) == 0 && (dev.flags & COSMO_FSCTL_DEV_F_CHECKED) && rep->clean);
    /* The check's mount held nothing: the key is still there. */
    KT_CHECK(kt_mount_dev(bd) == 0);
    mounted = true;
    KT_CHECK(kt_read_at(KT "/secret", 0, secret, sizeof(secret) - 1));
    KT_CHECK(kt_read_at(KT "/far", (uint64_t)KT_FAR_LBLK * CFS_BLOCK, blk, CFS_BLOCK));
    KT_CHECK(vfs_umount(KT) == 0);
    mounted = false;

    /*
     * Each domain, forged the way a CRC'd pool would accept: refused.
     * The superblock names the roots; a header is any CFS_KIND block,
     * here the inode map's root, which every mount reads; the key block;
     * and a block the mount does not read -- the far file's mid-level
     * checksum index -- which the checker reports and a read refuses.
     */
    KT_CHECK(kt_super(bd, &sb));
    struct {
        const char *what;
        uint64_t dva;
    } hdrs[] = { { "the inode map's root", sb.imap_root }, { "the member table", sb.members },
                 { "the key block", sb.key_root } };
    for (unsigned i = 0; i < sizeof(hdrs) / sizeof(hdrs[0]); i++) {
        KT_CHECK(cfs_keytab_has(sb.uuid) || kt_load(bd, master) == 0);
        KT_CHECK(kt_forge(bd, hdrs[i].dva, offsetof(struct cfs_mhdr, crc), true, CFS_MHDR_SIZE + 9, save));
        /* Refused is: the newest root does not load. The check's mount
         * then fails, or falls back to the older slot's root -- which
         * names an older copy of a block the commit since rewrote, and
         * loads (design.md, "Older-slot fallback at mount"). */
        int crc = cosmofs_check_dev(bd, 0, rep, &dev);
        bool refused = crc == 0 && (dev.flags & (COSMO_FSCTL_DEV_F_MOUNT_FAILED | COSMO_FSCTL_DEV_F_FELL_BACK));
        kinfo("selftest: cosmofs-encrypted-root: %s forged with its CRC32C recomputed: %s (check %d, flags 0x%x, "
              "mount error %d)", hdrs[i].what, refused ? "refused" : "ACCEPTED", crc, dev.flags, dev.mount_error);
        KT_CHECK(refused);
        KT_CHECK(kt_put_back(bd, hdrs[i].dva, save));
    }
    /* Both superblock slots: a recomputed CRC is not a seal here. */
    KT_CHECK(kt_forge(bd, CFS_SUPER_A, offsetof(struct cfs_super, crc), false, offsetof(struct cfs_super, free_blocks),
                      save));
    KT_CHECK(kt_forge(bd, CFS_SUPER_B, offsetof(struct cfs_super, crc), false, offsetof(struct cfs_super, free_blocks),
                      save2));
    int src = kt_mount_dev(bd);
    if (src == 0)
        (void)vfs_umount(KT);
    KT_CHECK(src == -EIO);
    KT_CHECK(cosmofs_check_dev(bd, 0, rep, &dev) == 0 && (dev.flags & COSMO_FSCTL_DEV_F_SUPER_NONE));
    KT_CHECK(kt_put_back(bd, CFS_SUPER_A, save) && kt_put_back(bd, CFS_SUPER_B, save2));
    kinfo("selftest: cosmofs-encrypted-root: both superblock slots forged with their CRC32C recomputed: mount "
          "refused (%d)", src);
    /* The mid-level index, which only the far block's read and a walk
     * reach. */
    KT_CHECK(kt_forge(bd, mid, offsetof(struct cfs_mhdr, crc), true, CFS_MHDR_SIZE + 3, save));
    KT_CHECK(kt_mount_dev(bd) == 0);
    mounted = true;
    KT_CHECK(kt_read_at(KT "/far", 0, blk, CFS_BLOCK));
    KT_CHECK(!kt_read_at(KT "/far", (uint64_t)KT_FAR_LBLK * CFS_BLOCK, blk, CFS_BLOCK));
    KT_CHECK(cosmofs_check(kt_mount(), rep, 0) == 0 && !rep->clean && rep->unreadable.count >= 1);
    kinfo("selftest: cosmofs-encrypted-root: a two-level index's mid block forged: read refused, the check counts "
          "%llu unreadable", (unsigned long long)rep->unreadable.count);
    KT_CHECK(vfs_umount(KT) == 0);
    mounted = false;
    KT_CHECK(kt_put_back(bd, mid, save));
    KT_CHECK(kt_load(bd, master) == 0);
    KT_CHECK(kt_mount_dev(bd) == 0);
    mounted = true;
    KT_CHECK(cosmofs_check(kt_mount(), rep, 0) == 0 && rep->clean);
    KT_CHECK(vfs_umount(KT) == 0);
    mounted = false;

#if CONFIG_DEBUG
    /* The labels: a pool of two members, the second's label forged (the
     * format of two members is a test hook). */
    two[0] = ramblk_create(1024);
    two[1] = ramblk_create(1024);
    KT_CHECK(two[0] != NULL && two[1] != NULL);
    KT_CHECK(cosmofs_test_format_keyed_members(two, 2, master2, slots) == 0);
    KT_CHECK(kt_mount_dev(two[0]) == 0);
    mounted = true;
    KT_CHECK(kt_write_at(KT "/x", 0, secret, sizeof(secret) - 1));
    KT_CHECK(vfs_umount(KT) == 0);
    mounted = false;
    KT_CHECK(kt_load(two[0], master2) == 0);
    KT_CHECK(kt_forge(two[1], 0, offsetof(struct cfs_label, crc), false, offsetof(struct cfs_label, nblocks) + 7,
                      save));
    int lrc = kt_mount_dev(two[0]);
    if (lrc == 0)
        (void)vfs_umount(KT);
    KT_CHECK(lrc != 0);
    KT_CHECK(kt_put_back(two[1], 0, save));
    KT_CHECK(kt_mount_dev(two[0]) == 0);
    mounted = true;
    KT_CHECK(kt_read_at(KT "/x", 0, secret, sizeof(secret) - 1));
    KT_CHECK(vfs_umount(KT) == 0);
    mounted = false;
    kinfo("selftest: cosmofs-encrypted-root: member 1's label forged with its CRC32C recomputed: mount refused (%d)",
          lrc);
#endif

    /* CFS-31, on a plain pool: the member table damaged in place -- one
     * byte of member 0's advisory free count, its CRC32C left as it was --
     * is refused like any other metadata block. It used to be read with
     * only its magic, kind and address checked, and the check's mount
     * loaded the newest root with it. */
    plain = ramblk_create(1024);
    KT_CHECK(plain != NULL);
    KT_CHECK(cosmofs_format(plain) == 0);
    KT_CHECK(kt_mount_dev(plain) == 0);
    mounted = true;
    KT_CHECK(kt_write_at(KT "/a", 0, secret, sizeof(secret) - 1));
    KT_CHECK(vfs_sync() == 0);
    KT_CHECK(kt_write_at(KT "/b", 0, secret, sizeof(secret) - 1));
    KT_CHECK(vfs_sync() == 0);
    KT_CHECK(vfs_umount(KT) == 0);
    mounted = false;
    KT_CHECK(kt_super(plain, &sb));
    {
        size_t free_at = CFS_MHDR_SIZE + 8 + offsetof(struct cfs_member, free_blocks);
        struct spool *p;
        KT_CHECK(pool_open(plain, &p) == 0);
        bool w = pool_read(p, sb.members, save) == 0;
        memcpy(save2, save, CFS_BLOCK);
        save2[free_at] ^= 0x01;
        w = w && pool_write(p, sb.members, save2) == 0 && pool_flush(p) == 0;
        pool_close(p);
        KT_CHECK(w);
    }
    int prc = cosmofs_check_dev(plain, 0, rep, &dev);
    bool plain_refused = prc == 0 && (dev.flags & (COSMO_FSCTL_DEV_F_MOUNT_FAILED | COSMO_FSCTL_DEV_F_FELL_BACK));
    kinfo("selftest: cosmofs-encrypted-root: a plain pool's member table damaged in place: %s (check %d, flags "
          "0x%x)", plain_refused ? "refused" : "ACCEPTED", prc, dev.flags);
    KT_CHECK(plain_refused);
    KT_CHECK(kt_put_back(plain, sb.members, save));
    ramblk_destroy(plain);
    plain = NULL;

    /* A plain pool whose file outgrows the one-level index is raised to
     * 14 and keeps CRC32C: no key block, so no MAC (design.md, "Import
     * rule"). 600000 blocks is entry 590 of an index of CRCs. */
    plain = ramblk_create(1024);
    KT_CHECK(plain != NULL);
    KT_CHECK(cosmofs_format(plain) == 0);
    KT_CHECK(kt_super(plain, &sb) && sb.version == CFS_FORMAT_VERSION);
    KT_CHECK(kt_mount_dev(plain) == 0);
    mounted = true;
    KT_CHECK(kt_write_at(KT "/far", 600000ull * CFS_BLOCK, blk, CFS_BLOCK));
    KT_CHECK(vfs_sync() == 0);
    KT_CHECK(vfs_umount(KT) == 0);
    mounted = false;
    KT_CHECK(kt_super(plain, &sb) && sb.version == 14 && sb.key_root == 0 && !cfs_super_is_mac(&sb));
    KT_CHECK(kt_mount_dev(plain) == 0);
    mounted = true;
    KT_CHECK(kt_read_at(KT "/far", 600000ull * CFS_BLOCK, blk, CFS_BLOCK));
    KT_CHECK(cosmofs_check(kt_mount(), rep, 0) == 0 && rep->clean && rep->csum_two_level == 1);
    KT_CHECK(vfs_umount(KT) == 0);
    mounted = false;
    kinfo("selftest: cosmofs-encrypted-root: a plain pool's file past the one-level index: raised to 14, CRC32C "
          "kept, read back, checked clean");
    ok = true;

out:
    if (mounted)
        (void)vfs_umount2(KT, VFS_UMOUNT_FORCE);
    (void)vfs_rmdir(NULL, KT);
    if (bd != NULL) {
        struct cfs_super s2;
        if (kt_super(bd, &s2))
            (void)cfs_keytab_unload(s2.uuid);
        ramblk_destroy(bd);
    }
    for (unsigned i = 0; i < 2; i++)
        if (two[i] != NULL) {
            struct cfs_super s2;
            if (i == 0 && kt_super(two[0], &s2))
                (void)cfs_keytab_unload(s2.uuid);
            ramblk_destroy(two[i]);
        }
    if (plain != NULL)
        ramblk_destroy(plain);
    memset(master, 0, sizeof(master));
    memset(master2, 0, sizeof(master2));
    kfree(slots);
    kfree(info);
    kfree(save);
    kfree(save2);
    kfree(blk);
    kfree(rep);
    return ok;
}
