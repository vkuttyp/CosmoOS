/*
 * cosmofstest.c - Self-tests for the storage pool and cosmofs on the
 * scratch virtio disk (vda). They format the disk, so they run only
 * when it exists, and they leave a filesystem behind for init's
 * user-mode test: /hello.txt and /dir/nested.txt.
 */

#include <kernel/compiler.h>
#include <kernel/blk.h>
#include <kernel/cosmofs.h>
#include <uapi/cosmo/fsctl.h>
#include <kernel/crc32c.h>
#include <kernel/errno.h>
#include <kernel/faultinject.h>
#include <kernel/thread.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/page.h>
#include <kernel/pagecache.h>
#include <kernel/printf.h>
#include <kernel/random.h>
#include <kernel/selftest.h>
#include <kernel/storage.h>
#include <kernel/string.h>
#include <kernel/vfs.h>

#include "cosmofstest.h"

#define STR_(x) #x
#define STR(x)  STR_(x)
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            *reason = "check failed: " #cond " at line " STR(__LINE__);        \
            return false;                                                      \
        }                                                                      \
    } while (0)

/* --- the transaction engine (audit milestone 7), on RAM devices --------------- */

#include <kernel/ramblk.h>
#include <kernel/timer.h>
#include <kernel/wait.h>

#include "cosmofs_format.h"
#include "cosmofs_internal.h"

#define ENG "/mnt/eng"

#if CONFIG_DEBUG
/*
 * Encryption. The claims worth checking are that the plaintext is not on
 * the disk, that a wrong key is refused rather than believed, that a
 * mount without the key still works for everything that is not a file's
 * contents, and that a scrub -- which has no key -- can still read and
 * repair the whole filesystem.
 */
bool selftest_cosmofs_crypt(const char **reason)
{
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    struct blkdev *bd = ramblk_create(512);
    CHECK(bd != NULL);
    static const char key[] = "correct horse battery staple";
    if (!random_ready()) {
        /* The no-entropy boot (make test-entropy): no master key from an
         * unseeded pool. The refusal comes after the bounded wait and
         * before anything reaches the device (invariant S18). */
        uint64_t t0 = clock_now_ns();
        CHECK(cosmofs_format_encrypted(bd, key, sizeof(key) - 1) == -EAGAIN);
        uint64_t waited = clock_now_ns() - t0;
        CHECK(waited >= RANDOM_KEYGEN_WAIT_NS);
        /* The first 64 KiB: both superblock slots and the first blocks. */
        uint32_t ns = 65536u / bd->sector_size;
        uint8_t *sec = kmalloc(65536u, KMEM_ZERO);
        CHECK(sec != NULL);
        bool blank = blk_read(bd, 0, ns, sec) == 0;
        for (unsigned i = 0; blank && i < 65536u; i++)
            blank = sec[i] == 0;
        kfree(sec);
        CHECK(blank);
        ramblk_destroy(bd);
        kinfo("selftest: cosmofs-crypt: unseeded: encrypted format refused with -EAGAIN after %llu ms, device untouched",
              (unsigned long long)(waited / 1000000ull));
        return true;
    }
    CHECK(cosmofs_format_encrypted(bd, key, sizeof(key) - 1) == 0);
    int mk = vfs_mkdir(NULL, ENG, 0755);
    CHECK(mk == 0 || mk == -EEXIST);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(cfst_mount_of(ENG), false);
    /* No key has arrived: the metadata mounted, the contents are shut. */
    CHECK(cosmofs_test_unlock(cfst_mount_of(ENG), "wrong key", 9) == -EKEYREJECTED);
    CHECK(cosmofs_test_unlock(cfst_mount_of(ENG), key, sizeof(key) - 1) == 0);

    static const char secret[] = "the quick brown fox jumps over the lazy dog, repeatedly and at length";
    CHECK(cfst_write_file(ENG "/secret.txt", secret, sizeof(secret) - 1));
    CHECK(vfs_mkdir(NULL, ENG "/private", 0755) == 0);
    CHECK(cfst_write_file(ENG "/private/inner", secret, sizeof(secret) - 1));
    CHECK(vfs_sync() == 0);
    CHECK(cfst_read_matches(ENG "/secret.txt", secret, sizeof(secret) - 1));

    /* The plaintext is not on the disk. Read the block the file's data
     * actually occupies and look for it. */
    struct file *f;
    CHECK(vfs_open(NULL, ENG "/secret.txt", COSMO_O_RDONLY, 0, &f) == 0);
    uint64_t ino = f->vn->ino;
    file_put(f);
    uint64_t dva = 0;
    CHECK(cosmofs_test_block_of(cfst_mount_of(ENG), ino, 0, &dva) == 0);
    struct spool *p;
    CHECK(pool_open(bd, &p) == 0);
    uint8_t *raw = kmalloc(CFS_BLOCK, 0);
    CHECK(raw != NULL);
    CHECK(pool_read(p, dva, raw) == 0);
    bool found = false;
    for (size_t i = 0; i + sizeof(secret) - 1 < CFS_BLOCK; i++)
        found = found || memcmp(raw + i, secret, sizeof(secret) - 1) == 0;
    CHECK(!found);

    /* A block bent behind the filesystem's back is refused, not
     * returned: the tag is what says this is the block that was
     * written, and a CRC could have been recomputed by whoever bent it. */
    raw[100] ^= 0x20;
    CHECK(pool_write(p, dva, raw) == 0 && pool_flush(p) == 0);
    kfree(raw);
    pool_close(p);
    CHECK(vfs_open(NULL, ENG "/secret.txt", COSMO_O_RDONLY, 0, &f) == 0);
    static char got[128];
    CHECK(file_read(f, got, sizeof(got)) < 0);
    file_put(f);

    /* Put it back by rewriting the file, then check the scrub. */
    CHECK(cfst_write_file(ENG "/secret.txt", secret, sizeof(secret) - 1));
    CHECK(vfs_sync() == 0);
    struct cosmofs_scrub_stats sc;
    CHECK(cosmofs_scrub(cfst_mount_of(ENG), &sc, COSMOFS_SCRUB_REPAIR) == 0 && sc.unrecoverable == 0);

    /* A valid block of this very file, moved to another of its offsets,
     * must be refused. The tag authenticates where the block belongs as
     * well as what it holds, and the reader supplies the position it
     * believes it is reading -- so a transplant fails even though the
     * ciphertext, nonce and tag are all genuine. */
    static char two[2 * CFS_BLOCK];
    for (size_t i = 0; i < sizeof(two); i++)
        two[i] = (char)(i / CFS_BLOCK ? 'B' : 'A');
    CHECK(cfst_write_file(ENG "/moved.bin", two, sizeof(two)));
    CHECK(vfs_sync() == 0);
    struct file *mf;
    CHECK(vfs_open(NULL, ENG "/moved.bin", COSMO_O_RDONLY, 0, &mf) == 0);
    uint64_t mino = mf->vn->ino;
    file_put(mf);
    uint64_t dva0 = 0, dva1 = 0;
    CHECK(cosmofs_test_block_of(cfst_mount_of(ENG), mino, 0, &dva0) == 0);
    CHECK(cosmofs_test_block_of(cfst_mount_of(ENG), mino, 1, &dva1) == 0);
    struct spool *mp;
    CHECK(pool_open(bd, &mp) == 0);
    uint8_t *blk0 = kmalloc(CFS_BLOCK, 0);
    CHECK(blk0 != NULL);
    CHECK(pool_read(mp, dva0, blk0) == 0);
    CHECK(pool_write(mp, dva1, blk0) == 0 && pool_flush(mp) == 0);   /* block 0's bytes at block 1 */
    kfree(blk0);
    pool_close(mp);
    /* Its own tag travels with it in the checksum tree? No: the entry
     * stays with the position, so this also stands in for an attacker
     * who moves both. Either way the position is wrong. */
    CHECK(vfs_open(NULL, ENG "/moved.bin", COSMO_O_RDONLY, 0, &mf) == 0);
    CHECK(file_pread(mf, got, 16, CFS_BLOCK) < 0);
    file_put(mf);
    CHECK(vfs_unlink(NULL, ENG "/moved.bin") == 0);

    /* No two writes of a block share a nonce, whatever the filesystem
     * does with generations. Writing the same plaintext to the same
     * logical block over and over must give different ciphertext every
     * time: if it did not, an attacker with two copies would have the
     * xor of the two plaintexts, and after the older-root fallback
     * reuses a generation that is exactly what a generation-derived
     * nonce would produce. */
    /* kmalloc'd, not on the stack: the pool reads into it by DMA, and
     * eight blocks is far more than a kernel stack should carry. */
    uint8_t *seen = kmalloc(8 * CFS_BLOCK, 0);
    CHECK(seen != NULL);
    unsigned kept = 0;
    for (unsigned round = 0; round < 8; round++) {
        CHECK(cfst_write_file(ENG "/nonce.bin", secret, sizeof(secret) - 1));
        CHECK(vfs_sync() == 0);
        struct file *nf;
        CHECK(vfs_open(NULL, ENG "/nonce.bin", COSMO_O_RDONLY, 0, &nf) == 0);
        uint64_t nino = nf->vn->ino;
        file_put(nf);
        uint64_t ndva = 0;
        CHECK(cosmofs_test_block_of(cfst_mount_of(ENG), nino, 0, &ndva) == 0);
        struct spool *np;
        CHECK(pool_open(bd, &np) == 0);
        int prc = pool_read(np, ndva, seen + (size_t)kept * CFS_BLOCK);
        pool_close(np);
        CHECK(prc == 0);
        for (unsigned prev = 0; prev < kept; prev++)
            CHECK(memcmp(seen + (size_t)prev * CFS_BLOCK, seen + (size_t)kept * CFS_BLOCK, CFS_BLOCK) != 0);
        kept++;
    }
    kfree(seen);
    CHECK(cfst_read_matches(ENG "/nonce.bin", secret, sizeof(secret) - 1));
    CHECK(vfs_unlink(NULL, ENG "/nonce.bin") == 0);

    /* Rotation rewrites one block: the old key stops working, the new
     * one starts, and every file is still readable without being
     * rewritten. */
    static const char key2[] = "a different key entirely";
    CHECK(cosmofs_rekey(cfst_mount_of(ENG), key2, sizeof(key2) - 1) == 0);
    CHECK(vfs_sync() == 0);
    CHECK(cfst_read_matches(ENG "/secret.txt", secret, sizeof(secret) - 1));
    CHECK(vfs_umount(ENG) == 0);

    /* Remount with no key at all. The mount succeeds, and then refuses
     * to walk a path: a directory's entries are its inode's data, so
     * with names encrypted there is no way in. That is the design
     * working, not a limitation of it. */
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(cfst_mount_of(ENG), false);
    struct cosmo_stat st;
    CHECK(vfs_stat(NULL, ENG "/secret.txt", &st) == -ENOKEY);
    CHECK(vfs_stat(NULL, ENG, &st) == 0 && st.type == COSMO_DT_DIR);   /* the root itself is a vnode */
    /* And a scrub still runs, because what it checks needs no key. */
    CHECK(cosmofs_scrub(cfst_mount_of(ENG), &sc, COSMOFS_SCRUB_REPAIR) == 0 && sc.unrecoverable == 0 && sc.blocks_read > 0);
    /* The old key is refused; the new one opens it. */
    CHECK(cosmofs_test_unlock(cfst_mount_of(ENG), key, sizeof(key) - 1) == -EKEYREJECTED);
    CHECK(cosmofs_test_unlock(cfst_mount_of(ENG), key2, sizeof(key2) - 1) == 0);
    CHECK(cfst_read_matches(ENG "/secret.txt", secret, sizeof(secret) - 1));
    CHECK(cfst_read_matches(ENG "/private/inner", secret, sizeof(secret) - 1));

    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_rmdir(NULL, ENG) == 0);
    ramblk_destroy(bd);
    kinfo("selftest: cosmofs-crypt: %llu blocks scrubbed without a key", (unsigned long long)sc.blocks_read);
    return true;
}

/*
 * A member table is disk data: its geometry decides how many bitmap
 * chunks a mount reads and how far the allocator may reach, so it is
 * checked against what the format can express before any of it is
 * believed. Corrupting a freshly formatted pool leaves no older root to
 * fall back to, so the refusal is the only outcome.
 */
bool selftest_cosmofs_badmembers(const char **reason)
{
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    struct blkdev *bd[2] = { ramblk_create(256), ramblk_create(256) };
    CHECK(bd[0] != NULL && bd[1] != NULL);
    CHECK(cosmofs_format_pool(bd, 2) == 0);
    int mk = vfs_mkdir(NULL, ENG, 0755);
    CHECK(mk == 0 || mk == -EEXIST);

    struct spool *p;
    CHECK(pool_open(bd[0], &p) == 0);
    uint8_t *blk = kmalloc(CFS_BLOCK, 0);
    CHECK(blk != NULL);
    CHECK(pool_read(p, CFS_SUPER_A, blk) == 0);
    const struct cfs_super *sb = (const struct cfs_super *)blk;
    CHECK(memcmp(sb->magic, CFS_MAGIC, 8) == 0 && sb->generation == 1);
    uint64_t members = sb->members;
    CHECK(members != 0);

    CHECK(pool_read(p, members, blk) == 0);
    struct cfs_member_block *mb = (struct cfs_member_block *)(blk + CFS_MHDR_SIZE);
    struct cfs_mhdr *h = (struct cfs_mhdr *)blk;
    CHECK(mb->count == 2);
    uint64_t was = mb->m[1].nblocks;

    /* More bitmap chunks than one allocation index can point at. */
    mb->m[1].nblocks = CFS_MAX_BLOCKS + 1;
    h->crc = 0;
    h->crc = cfst_block_crc(blk);
    CHECK(pool_write(p, members, blk) == 0 && pool_flush(p) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd[0], 0) == -EIO);

    /* A member larger than the device that carries it. */
    mb->m[1].nblocks = was + 1;
    h->crc = 0;
    h->crc = cfst_block_crc(blk);
    CHECK(pool_write(p, members, blk) == 0 && pool_flush(p) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd[0], 0) == -EIO);

    /* A first usable block inside the label. */
    mb->m[1].nblocks = was;
    mb->m[1].first_usable = 0;
    h->crc = 0;
    h->crc = cfst_block_crc(blk);
    CHECK(pool_write(p, members, blk) == 0 && pool_flush(p) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd[0], 0) == -EIO);

    /* Put it back: the mount works, so the refusals above were about the
     * corruption and not about the surgery. */
    mb->m[1].first_usable = 1;
    h->crc = 0;
    h->crc = cfst_block_crc(blk);
    CHECK(pool_write(p, members, blk) == 0 && pool_flush(p) == 0);
    kfree(blk);
    pool_close(p);
    CHECK(vfs_mount(ENG, "cosmofs", bd[0], 0) == 0);

    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_rmdir(NULL, ENG) == 0);
    ramblk_destroy(bd[0]);
    ramblk_destroy(bd[1]);
    kinfo("selftest: cosmofs-badmembers: a member table that cannot be true is refused");
    return true;
}

/*
 * A pool of two members. Everything the format change is for is here:
 * blocks are named by member, the allocator spreads across both, each
 * member carries its own bitmap, and a remount finds the second member
 * by its label rather than being told about it.
 */
/*
 * A version-3 disk mounts, writes and remounts under this kernel. That
 * is the whole benefit of packing the DVA into the eight bytes a pointer
 * already had: an old pointer is a new one on member 0, so there is no
 * conversion and no second decoder (design.md, "The DVA is 64 bits").
 */
/*
 * Symbolic links on disk (docs/audit/next-subsystem-symlink.md). The
 * target is the link's own block, written inside the transaction that
 * writes the inode and adds the entry, so a link either exists whole or
 * not at all -- and a filesystem formatted before version 8 refuses to
 * make one, because a kernel of that vintage would read it as a regular
 * file whose contents are a path.
 */
/*
 * The structural check (docs/audit/next-subsystem-fsck.md). The scrub
 * asks whether every block is still what was written; these ask whether
 * the blocks add up.
 */
bool selftest_cosmofs_check_clean(const char **reason)
{
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    struct blkdev *bd = ramblk_create(512);
    CHECK(bd != NULL);
    CHECK(cosmofs_format(bd) == 0);
    int mk = vfs_mkdir(NULL, ENG, 0755);
    CHECK(mk == 0 || mk == -EEXIST);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(cfst_mount_of(ENG), false);

    /* A filesystem with something of everything in it: files, a
     * directory, a symbolic link, a rewritten file and a removed one. */
    CHECK(cfst_write_file(ENG "/one", "first", 5));
    CHECK(vfs_mkdir(NULL, ENG "/dir", 0755) == 0);
    CHECK(cfst_write_file(ENG "/dir/two", "second", 6));
    CHECK(vfs_symlink(NULL, ENG "/link", "one") == 0);
    CHECK(cfst_write_file(ENG "/one", "first again, longer", 19));
    CHECK(cfst_write_file(ENG "/gone", "x", 1));
    CHECK(vfs_unlink(NULL, ENG "/gone") == 0);
    CHECK(vfs_sync() == 0);

    struct cosmofs_check_report rep;
    CHECK(cfst_check_is_clean(reason, &rep));
    CHECK(rep.clean);
    CHECK(!rep.partial);
    /* The arithmetic is the assertion: a walk that saw nothing would
     * report clean and fail here. */
    struct cosmofs_stats st;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &st) == 0);
    CHECK(rep.blocks_seen + rep.counted_free == st.total_blocks);
    CHECK(rep.counted_free == st.free_blocks);
    CHECK(rep.inodes_seen >= 5);            /* root, one, dir, two, link */
    CHECK(rep.dirs_seen == 2);              /* the root and dir */
    CHECK(rep.bytes_allocated > 0);

    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_rmdir(NULL, ENG) == 0);
    ramblk_destroy(bd);
    kinfo("selftest: cosmofs-check-clean: %llu blocks seen, %llu free, %llu inodes, %llu dirs, clean",
          (unsigned long long)rep.blocks_seen, (unsigned long long)rep.counted_free,
          (unsigned long long)rep.inodes_seen, (unsigned long long)rep.dirs_seen);
    return true;
}

bool selftest_cosmofs_check_leak(const char **reason)
{
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    struct blkdev *bd = ramblk_create(512);
    CHECK(bd != NULL);
    CHECK(cosmofs_format(bd) == 0);
    int mk = vfs_mkdir(NULL, ENG, 0755);
    CHECK(mk == 0 || mk == -EEXIST);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(cfst_mount_of(ENG), false);
    CHECK(cfst_write_file(ENG "/file", "bytes", 5));
    CHECK(vfs_sync() == 0);

    struct cosmofs_check_report rep;
    CHECK(cfst_check_is_clean(reason, &rep) && rep.clean);
    uint64_t free_before = rep.counted_free;

    /* Mark one free block allocated and tell nobody: a leak, exactly as
     * a crash between an allocation and the write that would have used
     * it leaves one. */
    uint64_t leaked = 0;
    CHECK(cosmofs_test_corrupt(cfst_mount_of(ENG), COSMOFS_CORRUPT_LEAK, 0, &leaked) == 0);
    CHECK(cfst_check_is_clean(reason, &rep));
    CHECK(!rep.clean);
    CHECK(rep.alloc_not_seen.count == 1);
    CHECK(rep.alloc_not_seen.named == 1 && rep.alloc_not_seen.name[0] == leaked);
    CHECK(rep.counted_free == free_before - 1);
    CHECK(rep.dup.count == 0 && rep.orphan.count == 0);   /* only the one class */

    /* Repair gives it back, and the second pass is clean. */
    CHECK(cosmofs_check(cfst_mount_of(ENG), &rep, COSMOFS_CHECK_REPAIR) == 0);
    CHECK(rep.alloc_not_seen.repaired == 1);
    CHECK(vfs_sync() == 0);
    CHECK(cfst_check_is_clean(reason, &rep));
    CHECK(rep.clean);
    CHECK(rep.counted_free == free_before);

    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_rmdir(NULL, ENG) == 0);
    ramblk_destroy(bd);
    kinfo("selftest: cosmofs-check-leak: one leaked block found by number and given back");
    return true;
}

/* The fixture's directory, for the faults that need one. */
static uint64_t check_sub_ino(void)
{
    struct cosmo_stat st;
    return vfs_stat(NULL, ENG "/sub", &st) == 0 ? st.ino : 0;
}

bool selftest_cosmofs_check_faults(const char **reason)
{
    struct cosmofs_check_report r;
    struct blkdev *bd = NULL;
    uint64_t what = 0;
    uint64_t file_ino = 0;

    /* A block a file uses, marked free: the dangerous direction, and the
     * one repair refuses because the allocator may already have handed
     * it out. */
    CHECK(cfst_check_fixture(&bd, reason));
    CHECK((file_ino = cfst_check_file_ino()) != 0);
    CHECK(cosmofs_test_corrupt(cfst_mount_of(ENG), COSMOFS_CORRUPT_FREE_IN_USE, file_ino, &what) == 0);
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0);
    CHECK(r.seen_not_alloc.count == 1 && r.seen_not_alloc.name[0] == what);
    CHECK(r.alloc_not_seen.count == 0);
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, COSMOFS_CHECK_REPAIR) == 0);
    CHECK(r.seen_not_alloc.repaired == 0);   /* refused, and says so by not counting one */
    cfst_check_teardown(bd);

    /* Two inodes over one block. */
    CHECK(cfst_check_fixture(&bd, reason));
    CHECK((file_ino = cfst_check_file_ino()) != 0);
    CHECK(cosmofs_test_corrupt(cfst_mount_of(ENG), COSMOFS_CORRUPT_CROSSLINK, file_ino, &what) == 0);
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0);
    CHECK(r.dup.count == 1 && r.dup.name[0] == what);
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, COSMOFS_CHECK_REPAIR) == 0);
    CHECK(r.dup.repaired == 0);              /* never repaired: choosing a winner is data loss */
    cfst_check_teardown(bd);

    /* A link count the entries disagree with. */
    CHECK(cfst_check_fixture(&bd, reason));
    CHECK((file_ino = cfst_check_file_ino()) != 0);
    CHECK(cosmofs_test_corrupt(cfst_mount_of(ENG), COSMOFS_CORRUPT_NLINK, file_ino, &what) == 0);
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0);
    CHECK(r.nlink_wrong.count == 1 && r.nlink_wrong.name[0] == what);
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, COSMOFS_CHECK_REPAIR) == 0);
    CHECK(r.nlink_wrong.repaired == 1);
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0 && r.clean);
    cfst_check_teardown(bd);

    /* An inode with blocks that no name reaches: the crash leak. */
    CHECK(cfst_check_fixture(&bd, reason));
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0 && r.clean);
    uint64_t free_before = r.counted_free;
    CHECK(cosmofs_test_corrupt(cfst_mount_of(ENG), COSMOFS_CORRUPT_ORPHAN, 0, &what) == 0);
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0);
    CHECK(r.orphan.count == 1 && r.orphan.name[0] == what);
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, COSMOFS_CHECK_REPAIR) == 0);
    CHECK(r.orphan.repaired == 1);
    /* Twice: a block freed in a transaction is reusable only after that
     * transaction's root is committed, so the count returns on the
     * commit after the repair's (cosmofs_core.c, the deferred free
     * list). */
    CHECK(vfs_sync() == 0);
    CHECK(vfs_sync() == 0);
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0);
    CHECK(r.clean && r.counted_free == free_before);   /* the space came back */
    cfst_check_teardown(bd);

    /* An entry naming a slot nothing allocated. The corruption overwrote
     * the number in a real entry, so the inode it used to name has lost
     * its only name -- and repair must not take that for an orphan and
     * destroy it. The file is still readable afterwards through the
     * entry's own parent, because nothing was freed. */
    CHECK(cfst_check_fixture(&bd, reason));
    CHECK(cosmofs_test_corrupt(cfst_mount_of(ENG), COSMOFS_CORRUPT_DANGLING, 0, &what) == 0);
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0);
    CHECK(r.dangling_entry.count == 1 && r.dangling_entry.name[0] == what);
    uint64_t orphans_at_dangle = r.orphan.count;
    CHECK(orphans_at_dangle >= 1);             /* the inode that lost its name */
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, COSMOFS_CHECK_REPAIR) == 0);
    CHECK(r.repair_refused);
    CHECK(r.dangling_entry.repaired == 0 && r.orphan.repaired == 0);
    CHECK(r.alloc_not_seen.repaired == 0);
    CHECK(vfs_sync() == 0);
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0);
    /* Still there. Had repair believed the walk, the inode would be gone
     * and this count would be one lower. */
    CHECK(r.orphan.count == orphans_at_dangle);
    cfst_check_teardown(bd);

    /* An entry whose type its inode does not have. The walk skips the
     * entry, so the inode it names looks unreferenced -- the same trap as
     * the dangling entry, without anything being unreadable. */
    CHECK(cfst_check_fixture(&bd, reason));
    CHECK(cosmofs_test_corrupt(cfst_mount_of(ENG), COSMOFS_CORRUPT_DIRENT, 0, &what) == 0);
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0);
    CHECK(r.dir_bad.count >= 1 && r.dir_bad.name[0] == what);
    CHECK(!r.partial);                         /* nothing was unreadable */
    uint64_t orphans_at_dirent = r.orphan.count;
    CHECK(orphans_at_dirent >= 1);             /* the inode whose entry was skipped */
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, COSMOFS_CHECK_REPAIR) == 0);
    CHECK(r.repair_refused);                   /* and repair still refuses */
    CHECK(r.dir_bad.repaired == 0 && r.orphan.repaired == 0);
    CHECK(vfs_sync() == 0);
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0);
    CHECK(r.orphan.count == orphans_at_dirent);
    cfst_check_teardown(bd);

    /* Both superblock totals the walk disagrees with: free blocks one
     * too high, inodes one too low. Two counters, two findings -- a
     * check that reported "the superblock is wrong" once would say the
     * same thing whichever of them was broken. */
    CHECK(cfst_check_fixture(&bd, reason));
    uint64_t free_was = 0, inodes_was = 0;
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0 && r.clean);
    free_was = r.counted_free;
    inodes_was = r.counted_inodes;
    CHECK(inodes_was >= 2);                  /* the root and the fixture's file */
    CHECK(cosmofs_test_corrupt(cfst_mount_of(ENG), COSMOFS_CORRUPT_COUNTER, 0, &what) == 0);
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0);
    CHECK(r.counter_wrong.count == 2);
    CHECK(r.counted_free == free_was && r.counted_inodes == inodes_was);   /* the walk is unmoved */
    CHECK(r.orphan.count == 0);              /* a wrong total is not a missing inode */
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, COSMOFS_CHECK_REPAIR) == 0);
    CHECK(r.counter_wrong.repaired == 2);
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0 && r.clean);
    cfst_check_teardown(bd);

    kinfo("selftest: cosmofs-check-faults: seven manufactured faults, each found by name, four repaired and three refused");
    return true;
}

/*
 * The control for the whole snapshot step: a snapshot holds blocks the
 * live tree has released, and those blocks are allocated and unreachable
 * from the live tree. A checker that did not walk snapshots would call
 * every one of them a leak.
 */
/*
 * The format says runs are sorted by lblk and never overlap
 * (docs/kernel-services/filesystem/cosmofs/design.md). Until this unit
 * the checker verified neither, and said so.
 *
 * The two are separate tests because they are separate classes, and the
 * ordering one exists to keep them separate: a descending pair ALSO
 * satisfies "this begins inside the previous", so a checker that tested
 * overlap first would report every ordering fault as an overlap.
 */
bool selftest_cosmofs_check_extent_order(const char **reason)
{
    struct cosmofs_check_report r;
    struct blkdev *bd = NULL;
    uint64_t what = 0, file_ino = 0;

    CHECK(cfst_check_fixture(&bd, reason));
    CHECK((file_ino = cfst_check_file_ino()) != 0);
    CHECK(cosmofs_test_corrupt(cfst_mount_of(ENG), COSMOFS_CORRUPT_EXTENT_ORDER, file_ino, &what) == 0);
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0);
    CHECK(r.extent_order.count == 1 && r.extent_order.name[0] == what);
    /* And NOT as an overlap: the whole reason there are two classes. */
    CHECK(r.extent_overlap.count == 0);
    /* Nothing else fires: the runs the hook added point at allocated
     * blocks of their own, so this is one finding and not a pile. */
    CHECK(r.dup.count == 0 && r.seen_not_alloc.count == 0 && r.alloc_not_seen.count == 0);
    cfst_check_teardown(bd);

    kinfo("selftest: cosmofs-check-extent-order: runs that descend by lblk are reported as "
          "extent_order and not as an overlap");
    return true;
}

bool selftest_cosmofs_check_extent_overlap(const char **reason)
{
    struct cosmofs_check_report r;
    struct blkdev *bd = NULL;
    uint64_t what = 0, file_ino = 0;

    CHECK(cfst_check_fixture(&bd, reason));
    CHECK((file_ino = cfst_check_file_ino()) != 0);
    CHECK(cosmofs_test_corrupt(cfst_mount_of(ENG), COSMOFS_CORRUPT_EXTENT_OVERLAP, file_ino, &what) == 0);
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0);
    CHECK(r.extent_overlap.count == 1 && r.extent_overlap.name[0] == what);
    CHECK(r.extent_order.count == 0);        /* the runs ascend; only the ranges overlap */
    /*
     * THE VACUITY GUARD, named in the report before the build. The
     * existing `block_seen` map already catches an overlap that shares
     * a POOL block, so a careless fixture would be caught by the old
     * code and prove nothing about the new comparison. The hook points
     * the overlapping run at a different, freshly allocated block, so
     * `dup` must stay empty -- if it fires, this test is measuring the
     * cross-link checker instead.
     */
    CHECK(r.dup.count == 0);
    CHECK(r.seen_not_alloc.count == 0 && r.alloc_not_seen.count == 0);
    cfst_check_teardown(bd);

    kinfo("selftest: cosmofs-check-extent-overlap: two runs of one inode covering one lblk at "
          "DIFFERENT pool blocks are reported, which the cross-link map cannot see");
    return true;
}

/*
 * Two of the four `dir_bad` reporting paths that nothing had ever made
 * fire (docs/audit/next-subsystem-fsck-unchecked.md). Each is one site
 * in the checker, and until now each was a line of code with no
 * evidence it worked.
 */
bool selftest_cosmofs_check_bad_ptr(const char **reason)
{
    struct cosmofs_check_report r;
    struct blkdev *bd = NULL;
    uint64_t what = 0, file_ino = 0;

    CHECK(cfst_check_fixture(&bd, reason));
    CHECK((file_ino = cfst_check_file_ino()) != 0);
    CHECK(cosmofs_test_corrupt(cfst_mount_of(ENG), COSMOFS_CORRUPT_BAD_PTR, file_ino, &what) == 0);
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0);
    CHECK(r.dir_bad.count == 1 && r.dir_bad.name[0] == what);
    /* The pass survives it: a pointer it cannot map must not be used to
     * index the bitmap, which is what the early return is for. */
    CHECK(r.blocks_seen > 0);
    cfst_check_teardown(bd);

    kinfo("selftest: cosmofs-check-bad-ptr: a block pointer past the pool is reported and not "
          "used to index the seen map");
    return true;
}

bool selftest_cosmofs_check_namelen(const char **reason)
{
    struct cosmofs_check_report r;
    struct blkdev *bd = NULL;
    uint64_t what = 0;

    CHECK(cfst_check_fixture(&bd, reason));
    /* The root directory: the fixture put `file` and `sub` in it. */
    CHECK(cosmofs_test_corrupt(cfst_mount_of(ENG), COSMOFS_CORRUPT_NAMELEN, CFS_ROOT_INO, &what) == 0);
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0);
    CHECK(r.dir_bad.count >= 1 && r.dir_bad.name[0] == what);
    cfst_check_teardown(bd);

    kinfo("selftest: cosmofs-check-namelen: an entry claiming a name longer than its slot is "
          "refused by length before its bytes are read");
    return true;
}

bool selftest_cosmofs_check_two_parents(const char **reason)
{
    struct cosmofs_check_report r;
    struct blkdev *bd = NULL;
    uint64_t what = 0, sub_ino = 0;

    CHECK(cfst_check_fixture(&bd, reason));
    CHECK((sub_ino = check_sub_ino()) != 0);
    CHECK(cosmofs_test_corrupt(cfst_mount_of(ENG), COSMOFS_CORRUPT_TWO_PARENTS, sub_ino, &what) == 0);
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0);
    CHECK(r.dir_bad.count >= 1 && r.dir_bad.name[0] == what);
    cfst_check_teardown(bd);

    kinfo("selftest: cosmofs-check-two-parents: a directory named from two directories is "
          "reported, so the directory graph is checked for being a tree");
    return true;
}

/*
 * The extent chain's OWN guard, which is a different constant from the
 * one the other four `chain_cycle` sites use: this walker is bounded by
 * CFS_MAX_EXTENTS / CFS_EXTENTS_PER_BLOCK + 2, and the other sites by
 * CFS_CHECK_MAX_CHAIN or CFS_CHECK_MAX_DEPTH. This unit tests this one
 * and the report says the other four stay untested rather than
 * implying one test covers five walkers.
 */
bool selftest_cosmofs_check_chain_cycle(const char **reason)
{
    struct cosmofs_check_report r;
    struct blkdev *bd = NULL;
    uint64_t what = 0, file_ino = 0;

    CHECK(cfst_check_fixture(&bd, reason));
    CHECK((file_ino = cfst_check_file_ino()) != 0);
    CHECK(cosmofs_test_corrupt(cfst_mount_of(ENG), COSMOFS_CORRUPT_CHAIN_CYCLE, file_ino, &what) == 0);
    /* That this call RETURNS is half the assertion: without the guard
     * the walk does not terminate and the boot reaches its timeout. */
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0);
    CHECK(r.chain_cycle.count >= 1);
    /*
     * The walk revisits the two blocks until the guard stops it, so the
     * live-claim map sees each of them more than once and `dup` fires
     * too. That is not noise: it is what a cyclic chain looks like to a
     * checker that claims as it walks, and asserting it keeps the test
     * honest about what the pass actually reports.
     */
    CHECK(r.dup.count >= 1);
    cfst_check_teardown(bd);

    kinfo("selftest: cosmofs-check-chain-cycle: an extent chain that returns to itself is "
          "reported and the walk terminates on its own guard");
    return true;
}

/*
 * The last of the five reporting paths nothing had ever fired. The
 * comment beside that site says a count past the block is "the table
 * being wrong, not the snapshot holding nothing" -- so the check must
 * report the table AND still walk what the block does hold, or every
 * member's blocks come back as leaks. Both halves are asserted, because
 * the second is the one the comment is about and the one that had no
 * evidence behind it.
 */
bool selftest_cosmofs_check_snap_members(const char **reason)
{
    struct cosmofs_check_report r;
    struct blkdev *bd = NULL;
    uint64_t what = 0;

    CHECK(cfst_check_fixture(&bd, reason));
    CHECK(vfs_mkdir(NULL, ENG "/.snapshots/one", 0755) == 0);   /* takes one, and commits */

    int rc = cosmofs_test_corrupt(cfst_mount_of(ENG), COSMOFS_CORRUPT_SNAP_MEMBERS, 0, &what);
    if (rc == -ENOTSUP) {
        /* A member table is a version-4 shape; before it there is no
         * count to be wrong. Skipped rather than silently passing. */
        kinfo("selftest: cosmofs-check-snap-members: needs format v4 or later; skipping");
        cfst_check_teardown(bd);
        return true;
    }
    CHECK(rc == 0);
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0);
    CHECK(r.dir_bad.count >= 1 && r.dir_bad.name[0] == what);
    /* The walk continued: a bitmap it did not reach would make every
     * block the snapshot holds look unreferenced. */
    CHECK(r.alloc_not_seen.count == 0);
    cfst_check_teardown(bd);

    kinfo("selftest: cosmofs-check-snap-members: a member count past its block is reported as "
          "the table being wrong, and the walk still reads what the block holds");
    return true;
}

/*
 * The one invariant here that is about strings rather than numbers.
 * The pass keeps a fixed bitmap, hashes each name into it, and re-scans
 * the directory only when a bit says "maybe" -- so a hash collision
 * costs a re-scan and never a wrong answer
 * (docs/audit/next-subsystem-fsck-unchecked.md).
 */
bool selftest_cosmofs_check_dup_name(const char **reason)
{
    struct cosmofs_check_report r;
    struct blkdev *bd = NULL;
    uint64_t what = 0;

    CHECK(cfst_check_fixture(&bd, reason));
    CHECK(cosmofs_test_corrupt(cfst_mount_of(ENG), COSMOFS_CORRUPT_DUP_NAME, CFS_ROOT_INO, &what) == 0);
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0);
    CHECK(r.dir_dup_name.count == 1 && r.dir_dup_name.name[0] == what);
    cfst_check_teardown(bd);

    /*
     * The other half, which is the one a bitmap makes easy to get
     * wrong: two DIFFERENT names that hash to the same bit must report
     * nothing. Only the re-scan keeps this quiet -- with the bitmap hit
     * trusted, a sound filesystem is reported as having a duplicate.
     *
     * The pair is deterministic rather than "enough names that
     * something probably collides": sixty-four distinct names collide
     * in a 32768-bit map only about six per cent of the time, so that
     * version of this test would have passed against a missing re-scan
     * nineteen times in twenty. The collision is ASSERTED first, so a
     * change to the hash fails here loudly instead of quietly making
     * the rest of this test vacuous.
     */
    CHECK(cfst_check_fixture(&bd, reason));
    CHECK(cosmofs_test_name_hash("asl", 3) == cosmofs_test_name_hash("bea", 3));
    CHECK(cfst_write_file(ENG "/asl", "x", 1));
    CHECK(cfst_write_file(ENG "/bea", "y", 1));
    CHECK(vfs_sync() == 0);
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0);
    CHECK(r.dir_dup_name.count == 0);
    CHECK_CLEAN(r);
    cfst_check_teardown(bd);

    kinfo("selftest: cosmofs-check-dup-name: one name twice at two inodes is reported, and "
          "sixty-four distinct names in one directory are not");
    return true;
}

bool selftest_cosmofs_check_snapshot(const char **reason)
{
    struct blkdev *bd = NULL;
    struct cosmofs_check_report r;
    CHECK(cfst_check_fixture(&bd, reason));

    CHECK(cfst_write_file(ENG "/held", "the snapshot's copy", 19));
    CHECK(vfs_sync() == 0);
    CHECK(vfs_mkdir(NULL, ENG "/.snapshots/keep", 0755) == 0);
    /* Rewrite it: the old blocks are now the snapshot's alone. */
    CHECK(cfst_write_file(ENG "/held", "the live copy, different length", 31));
    CHECK(vfs_sync() == 0);

    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0);
    CHECK(r.snapshots_seen == 1);
    CHECK_CLEAN(r);                       /* the held blocks are not leaks */
    CHECK(r.dup.count == 0);              /* nor cross-links: sharing is the point */
    CHECK(cfst_read_matches(ENG "/.snapshots/keep/held", "the snapshot's copy", 19));

    /* And after the snapshot goes, still clean: the blocks it held are
     * free or the live tree's, and nothing is left claiming them. */
    CHECK(vfs_rmdir(NULL, ENG "/.snapshots/keep") == 0);
    CHECK(vfs_sync() == 0);
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0);
    CHECK(r.snapshots_seen == 0 && r.clean);

    cfst_check_teardown(bd);
    kinfo("selftest: cosmofs-check-snapshot: a snapshot's held blocks are neither leaks nor cross-links");
    return true;
}

/*
 * The defect, told the right way round: a file opened, unlinked and
 * committed, and the mount then dropped with the handle still open --
 * which is a crash, in the only way a self-test can have one. The
 * *remount* reclaims the inode and its blocks, with no operator and no
 * repair flag (docs/audit/next-subsystem-orphan.md).
 */
bool selftest_cosmofs_orphan_crash(const char **reason)
{
    struct blkdev *bd;
    if (!cfst_engine_mount(&bd, 512, reason))
        return false;
    struct cosmofs_check_report r;
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0 && r.clean);
    uint64_t inodes_before = r.counted_inodes;

    /*
     * Sixteen blocks, so that the measurement below is about the file
     * and not about the records. Comparing the free count with a
     * baseline taken before any of this would compare against a
     * filesystem that had no free record either, and be off by the
     * record's own block -- which is a true difference and not the one
     * under test. The claim is the *reclaim*: how much came back.
     */
    const size_t biglen = 16 * 4096;
    uint8_t *big = kmalloc(biglen, 0);
    CHECK(big != NULL);
    cfst_fill_incompressible(big, biglen, 0x5eed);
    struct file *f;
    bool wrote = cfst_write_file(ENG "/doomed", (const char *)big, (uint32_t)biglen);
    kfree(big);
    CHECK(wrote);
    CHECK(vfs_open(NULL, ENG "/doomed", COSMO_O_RDONLY, 0, &f) == 0);
    CHECK(vfs_unlink(NULL, ENG "/doomed") == 0);
    CHECK(vfs_sync() == 0);

    /* On disk the inode now has no name and keeps its blocks -- and the
     * root says so, which is the whole difference. */
    struct cosmofs_stats st;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &st) == 0);
    CHECK(st.orphan_root != 0);
    CHECK(st.pending_orphans == 1);
    /* What the filesystem looks like with the promise outstanding. */
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0);
    CHECK_CLEAN(r);                          /* a recorded orphan is not a finding */
    CHECK(r.orphan.count == 0);
    uint64_t free_outstanding = r.counted_free;

    /* The crash: the handle never closes, so cfs_evict never runs, and
     * the forced unmount drops the open transaction as a crash before
     * the root write would. */
    file_put(f);
    CHECK(vfs_umount2(ENG, VFS_UMOUNT_FORCE) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(cfst_mount_of(ENG), false);

    /*
     * The mount schedules the reclaim; the first commit makes it
     * durable. The blocks are freed *deferred*, because the root this
     * mount is running on still names them -- handing them to the
     * allocator before a root says they are free is the ordering every
     * other free in this filesystem obeys. So the space comes back at
     * the commit, exactly as an ordinary eviction's does.
     */
    CHECK(vfs_sync() == 0);
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0);
    CHECK_CLEAN(r);                          /* the claim */
    CHECK(r.orphan.count == 0);
    /* The file's sixteen blocks are back, and so is its inode slot. */
    CHECK(r.counted_free >= free_outstanding + 16);
    CHECK(r.counted_inodes == inodes_before);

    kinfo("selftest: cosmofs-orphan-crash: the mount reclaimed the inode and %llu blocks with no operator and no "
          "repair flag",
          (unsigned long long)(r.counted_free - free_outstanding));
    return cfst_engine_unmount(bd, reason);
}

/*
 * An ordinary unlink writes no record -- and the *held* case does write
 * one, which is the other half and belongs in the same test. Separately,
 * either passes for the wrong reason: "no record" is also true of a
 * filesystem whose record never works.
 *
 * The condition, stated rather than relied on: the writeback thread is
 * off, so no commit falls between the unlink and the eviction. With it
 * on, a commit landing in that gap writes a record for an inode about to
 * be evicted and the next commit retires it -- correct, and not free,
 * which is why the report's claim is the narrow one.
 */
bool selftest_cosmofs_orphan_cancels(const char **reason)
{
    struct blkdev *bd;
    if (!cfst_engine_mount(&bd, 512, reason))
        return false;   /* engine_mount turns the writeback thread off */

    CHECK(cfst_write_file(ENG "/plain", "no handle on this one", 21));
    CHECK(vfs_sync() == 0);
    struct cosmofs_stats before;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &before) == 0);

    CHECK(vfs_unlink(NULL, ENG "/plain") == 0);
    struct cosmofs_stats mid;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &mid) == 0);
    CHECK(mid.pending_orphans == 0);   /* the eviction already cancelled it */
    CHECK(vfs_sync() == 0);
    struct cosmofs_stats after;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &after) == 0);
    CHECK(after.orphan_root == 0);     /* so the commit wrote no record at all */

    /* And the same filesystem does write one when a handle is held, so
     * "no record" above is a fact about the unlink and not about the
     * record. */
    struct file *f;
    CHECK(cfst_write_file(ENG "/held", "this one is open", 16));
    CHECK(vfs_open(NULL, ENG "/held", COSMO_O_RDONLY, 0, &f) == 0);
    CHECK(vfs_unlink(NULL, ENG "/held") == 0);
    CHECK(vfs_sync() == 0);
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &after) == 0);
    CHECK(after.orphan_root != 0);
    CHECK(after.pending_orphans == 1);
    file_put(f);
    CHECK(vfs_sync() == 0);
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &after) == 0);
    CHECK(after.orphan_root == 0);     /* the eviction retired it */
    CHECK(after.pending_orphans == 0);

    kinfo("selftest: cosmofs-orphan-cancels: an unlink with nothing holding it wrote no record, one with a handle "
          "wrote and retired one");
    (void)before;
    return cfst_engine_unmount(bd, reason);
}

/*
 * An empty directory removed while a process's working directory
 * references it. The cwd is a referenced vnode
 * (docs/audit/next-subsystem-cwd-ref.md), so rmdir succeeds and the
 * eviction waits -- the same defect with a directory's blocks.
 *
 * And the trap: rmdir already decremented the *parent's* link count in
 * the same transaction, so the replay must not touch the parent. The
 * parent's nlink is asserted at every point in the cycle.
 */
bool selftest_cosmofs_orphan_dir(const char **reason)
{
    struct blkdev *bd;
    if (!cfst_engine_mount(&bd, 512, reason))
        return false;

    CHECK(vfs_mkdir(NULL, ENG "/keep", 0755) == 0);
    CHECK(vfs_mkdir(NULL, ENG "/keep/going", 0755) == 0);
    CHECK(vfs_sync() == 0);

    struct cosmo_stat pst;
    CHECK(vfs_stat(NULL, ENG "/keep", &pst) == 0);
    uint32_t parent_nlink = pst.nlink;

    /* A reference on the directory that outlives its name: a vnode held
     * the way a working directory holds one. */
    struct vnode *held;
    CHECK(vfs_lookup(NULL, ENG "/keep/going", &held) == 0);
    CHECK(vfs_rmdir(NULL, ENG "/keep/going") == 0);
    CHECK(vfs_sync() == 0);

    struct cosmofs_stats st;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &st) == 0);
    CHECK(st.orphan_root != 0);
    CHECK(st.pending_orphans == 1);
    CHECK(vfs_stat(NULL, ENG "/keep", &pst) == 0);
    CHECK(pst.nlink == parent_nlink - 1);   /* rmdir took the parent's link, once */
    uint32_t after_rmdir = pst.nlink;

    /* The crash, with the reference still held. */
    vnode_put(held);
    CHECK(vfs_umount2(ENG, VFS_UMOUNT_FORCE) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(cfst_mount_of(ENG), false);

    CHECK(vfs_sync() == 0);   /* the reclaim lands on the mount's first commit */
    struct cosmofs_check_report r;
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0);
    CHECK_CLEAN(r);
    CHECK(r.orphan.count == 0);
    CHECK(r.nlink_wrong.count == 0);        /* the replay did not touch the parent */
    CHECK(vfs_stat(NULL, ENG "/keep", &pst) == 0);
    CHECK(pst.nlink == after_rmdir);        /* the same link count, not one less again */

    kinfo("selftest: cosmofs-orphan-dir: a directory removed under a live reference was reclaimed, parent nlink %u "
          "throughout",
          (unsigned)after_rmdir);
    return cfst_engine_unmount(bd, reason);
}

/*
 * A file replaced by a rename while it is open: the half of this defect
 * that is easiest to miss, because the victim never passes through
 * unlink.
 */
bool selftest_cosmofs_orphan_rename(const char **reason)
{
    struct blkdev *bd;
    if (!cfst_engine_mount(&bd, 512, reason))
        return false;
    struct cosmofs_check_report r;
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0 && r.clean);

    const size_t biglen = 16 * 4096;
    uint8_t *big = kmalloc(biglen, 0);
    CHECK(big != NULL);
    cfst_fill_incompressible(big, biglen, 0xf00d);
    bool wrote = cfst_write_file(ENG "/victim", (const char *)big, (uint32_t)biglen);
    kfree(big);
    CHECK(wrote);
    CHECK(cfst_write_file(ENG "/winner", "short", 5));
    CHECK(vfs_sync() == 0);

    struct file *f;
    CHECK(vfs_open(NULL, ENG "/victim", COSMO_O_RDONLY, 0, &f) == 0);
    CHECK(vfs_rename(NULL, ENG "/winner", ENG "/victim") == 0);
    CHECK(vfs_sync() == 0);
    struct cosmofs_stats st;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &st) == 0);
    CHECK(st.pending_orphans == 1);
    CHECK(st.orphan_root != 0);
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0);
    uint64_t free_outstanding = r.counted_free;

    file_put(f);
    CHECK(vfs_umount2(ENG, VFS_UMOUNT_FORCE) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(cfst_mount_of(ENG), false);
    CHECK(vfs_sync() == 0);   /* the reclaim lands on the mount's first commit */
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0);
    CHECK_CLEAN(r);
    CHECK(cfst_read_matches(ENG "/victim", "short", 5));   /* the rename itself stood */
    CHECK(r.counted_free >= free_outstanding + 16);   /* the replaced file's blocks came back */

    kinfo("selftest: cosmofs-orphan-rename: the replaced file's %llu blocks came back at the mount",
          (unsigned long long)(r.counted_free - free_outstanding));
    return cfst_engine_unmount(bd, reason);
}

/*
 * The record is not cleared when it is replayed, for the reason the free
 * record's is not: a mount that replays and goes away without committing
 * must leave the filesystem as it found it, or the second mount loses
 * what the first reclaimed. And `inode_count` must move exactly once --
 * the replay skips an inode whose slot is already empty *before* the
 * decrement, which is the only thing standing between this and a count
 * that drops twice.
 */
bool selftest_cosmofs_orphan_idempotent(const char **reason)
{
    struct blkdev *bd;
    if (!cfst_engine_mount(&bd, 512, reason))
        return false;
    static const char big[4096] = { 0 };
    struct file *f;
    CHECK(cfst_write_file(ENG "/gone", big, sizeof(big)));
    CHECK(vfs_open(NULL, ENG "/gone", COSMO_O_RDONLY, 0, &f) == 0);
    CHECK(vfs_unlink(NULL, ENG "/gone") == 0);
    CHECK(vfs_sync() == 0);
    file_put(f);
    CHECK(vfs_umount2(ENG, VFS_UMOUNT_FORCE) == 0);

    /* First mount: the replay reclaims it, and the record still stands
     * because no commit has retired it. */
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(cfst_mount_of(ENG), false);
    struct cosmofs_stats first;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &first) == 0);
    CHECK(first.orphan_root != 0);
    static struct cosmofs_check_report r1;   /* static: see selftest_cosmofs_crash */
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r1, 0) == 0);

    /* Away again without committing. */
    CHECK(vfs_umount2(ENG, VFS_UMOUNT_FORCE) == 0);

    /* Second mount: the same list, the same effect, the count moved once. */
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(cfst_mount_of(ENG), false);
    struct cosmofs_stats second;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &second) == 0);
    CHECK(second.orphan_root == first.orphan_root);
    CHECK(second.inode_count == first.inode_count);
    CHECK(second.free_blocks == first.free_blocks);
    CHECK(vfs_sync() == 0);   /* and only now is the reclaim durable */
    static struct cosmofs_check_report r2;
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r2, 0) == 0);
    CHECK(r2.clean);
    CHECK(r2.counted_inodes == r1.counted_inodes);

    kinfo("selftest: cosmofs-orphan-idempotent: two mounts, one record, %llu inodes both times",
          (unsigned long long)second.inode_count);
    return cfst_engine_unmount(bd, reason);
}

/*
 * The record's blocks are allocated in the bitmap the root publishes,
 * because they come from the commit's reservation rather than from an
 * allocation after the fixpoint. A fill that allocated instead would set
 * the bit after commit_bitmap had written it, so the next mount would
 * read the block as free while the root reaches it: `seen_not_alloc`,
 * the direction that hands live data to the allocator.
 *
 * Also the chain: more orphans than one block holds.
 */
bool selftest_cosmofs_orphan_reserved(const char **reason)
{
    struct blkdev *bd;
    if (!cfst_engine_mount(&bd, 4096, reason))
        return false;
    const unsigned many = CFS_ORPHANS_PER_BLOCK + 20u;
    struct file **fs_ = kmalloc(many * sizeof(*fs_), KMEM_ZERO);
    CHECK(fs_ != NULL);
    char path[64];
    bool ok = true;
    for (unsigned i = 0; i < many && ok; i++) {
        ksnprintf(path, sizeof(path), ENG "/o%u", i);
        ok = cfst_write_file(path, "x", 1) && vfs_open(NULL, path, COSMO_O_RDONLY, 0, &fs_[i]) == 0 &&
             vfs_unlink(NULL, path) == 0;
    }
    if (!ok) {
        for (unsigned i = 0; i < many; i++)
            if (fs_[i])
                file_put(fs_[i]);
        kfree(fs_);
        *reason = "could not build the orphan set";
        return false;
    }
    CHECK(vfs_sync() == 0);
    struct cosmofs_stats st;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &st) == 0);
    CHECK(st.pending_orphans == many);
    CHECK(st.orphan_root != 0);

    for (unsigned i = 0; i < many; i++)
        file_put(fs_[i]);
    kfree(fs_);
    CHECK(vfs_umount2(ENG, VFS_UMOUNT_FORCE) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(cfst_mount_of(ENG), false);
    CHECK(vfs_sync() == 0);   /* the reclaim lands on the mount's first commit */
    struct cosmofs_check_report r;
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0);
    CHECK(r.seen_not_alloc.count == 0);   /* the claim */
    CHECK_CLEAN(r);
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &st) == 0);
    CHECK(st.pending_orphans == 0);

    kinfo("selftest: cosmofs-orphan-reserved: %u inodes recorded across a chain and all of them replayed", many);
    return cfst_engine_unmount(bd, reason);
}

/*
 * A mount replays with fs->lock unheld, which is sound only while the
 * filesystem has no other thread in it. The writeback thread used to
 * start on the replay's first dirty buffer, from inside the replay: it
 * takes the mount's sync lock, which the mount path does not hold, sees
 * a mount that is not unmounted, and commits a half-built filesystem
 * across an unlocked fs->bufs. That is what made cosmofs-orphan-reserved
 * fail on CI with a metadata block that would not verify, and what
 * panicked x86_64 in list_remove under a writeback commit.
 *
 * Whether that second thread wins the race is timing; whether it exists
 * during the replay is not, so that is the claim here. The first count
 * is the test's own guard: a mount whose replay dirtied nothing would
 * satisfy the second for no reason.
 */
bool selftest_cosmofs_mount_no_early_wb(const char **reason)
{
    struct blkdev *bd;
    if (!cfst_engine_mount(&bd, 4096, reason))
        return false;
    /* An open transaction big enough that the force-unmount strands it
     * and the next mount has a long replay to do. */
    const unsigned many = CFS_ORPHANS_PER_BLOCK + 20u;
    struct file **held = kmalloc(many * sizeof(*held), KMEM_ZERO);
    CHECK(held != NULL);
    char path[64];
    bool ok = true;
    for (unsigned i = 0; i < many && ok; i++) {
        ksnprintf(path, sizeof(path), ENG "/w%u", i);
        ok = cfst_write_file(path, "x", 1) && vfs_open(NULL, path, COSMO_O_RDONLY, 0, &held[i]) == 0 &&
             vfs_unlink(NULL, path) == 0;
    }
    if (!ok) {
        for (unsigned i = 0; i < many; i++)
            if (held[i])
                file_put(held[i]);
        kfree(held);
        *reason = "could not build the orphan set";
        return false;
    }
    CHECK(vfs_sync() == 0);
    for (unsigned i = 0; i < many; i++)
        file_put(held[i]);
    kfree(held);
    CHECK(vfs_umount2(ENG, VFS_UMOUNT_FORCE) == 0);

    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    uint64_t notes = 0, early = 0;
    cosmofs_test_mount_writeback(cfst_mount_of(ENG), &notes, &early);
    cosmofs_test_set_writeback(cfst_mount_of(ENG), false);
    CHECK(notes > 0);    /* the guard: the replay really did dirty the filesystem */
    CHECK(early == 0);   /* the claim: and no writeback thread was in it */

    kinfo("selftest: cosmofs-mount-no-early-writeback: the replay made %llu dirty marks, "
          "%llu of them with a writeback thread running",
          (unsigned long long)notes, (unsigned long long)early);
    return cfst_engine_unmount(bd, reason);
}

/*
 * A hundred commits with a handle held leave one chain and no residue:
 * each writes its own record and releases its predecessor's. Not
 * releasing it would cost a block a commit, which no single-commit test
 * can see.
 */
bool selftest_cosmofs_orphan_supersede(const char **reason)
{
    struct blkdev *bd;
    if (!cfst_engine_mount(&bd, 1024, reason))
        return false;
    struct file *f;
    CHECK(cfst_write_file(ENG "/held", "open across all of it", 21));
    CHECK(vfs_open(NULL, ENG "/held", COSMO_O_RDONLY, 0, &f) == 0);
    CHECK(vfs_unlink(NULL, ENG "/held") == 0);
    CHECK(vfs_sync() == 0);
    struct cosmofs_stats first;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &first) == 0);
    CHECK(first.orphan_root != 0);

    bool ok = true;
    for (unsigned i = 0; i < 100 && ok; i++) {
        char c = (char)('a' + (i % 26));
        ok = cfst_write_file(ENG "/churn", &c, 1) && vfs_sync() == 0;
    }
    CHECK(ok);
    struct cosmofs_stats last;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &last) == 0);
    CHECK(last.orphan_root != 0);
    CHECK(last.pending_orphans == 1);
    /* One block for the churn file and one for the record, either side:
     * the free count does not fall by a block per commit. */
    CHECK(first.free_blocks - last.free_blocks < 8);

    file_put(f);
    CHECK(vfs_sync() == 0);
    struct cosmofs_check_report r;
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0);
    CHECK_CLEAN(r);

    kinfo("selftest: cosmofs-orphan-supersede: 100 commits, %llu free blocks lost to them in all",
          (unsigned long long)(first.free_blocks - last.free_blocks));
    return cfst_engine_unmount(bd, reason);
}

/*
 * A fill that fails with the record's blocks reserved gives them back
 * and publishes no root: the filesystem is exactly as it was.
 */
bool selftest_cosmofs_orphan_rollback(const char **reason)
{
    struct blkdev *bd;
    if (!cfst_engine_mount(&bd, 512, reason))
        return false;
    CHECK(cfst_write_file(ENG "/held", "content", 7));
    CHECK(vfs_sync() == 0);
    struct cosmofs_stats before;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &before) == 0);

    struct file *f;
    CHECK(vfs_open(NULL, ENG "/held", COSMO_O_RDONLY, 0, &f) == 0);
    cosmofs_test_fail_orphan(cfst_mount_of(ENG), true);
    CHECK(vfs_unlink(NULL, ENG "/held") == 0);
    CHECK(vfs_sync() != 0);                 /* the commit fails */
    cosmofs_test_fail_orphan(cfst_mount_of(ENG), false);
    file_put(f);

    CHECK(vfs_umount2(ENG, VFS_UMOUNT_FORCE) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(cfst_mount_of(ENG), false);
    struct cosmofs_stats after;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &after) == 0);
    CHECK(after.generation == before.generation);   /* no root was published */
    CHECK(after.free_blocks == before.free_blocks);
    CHECK(after.orphan_root == 0);
    struct cosmofs_check_report r;
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0);
    CHECK_CLEAN(r);
    CHECK(cfst_read_matches(ENG "/held", "content", 7));   /* the unlink went with it */

    kinfo("selftest: cosmofs-orphan-rollback: the failed fill left generation %llu and %llu free blocks",
          (unsigned long long)after.generation, (unsigned long long)after.free_blocks);
    return cfst_engine_unmount(bd, reason);
}

/*
 * A record that names a live inode must not be believed.
 *
 * The replay's job is to delete, so the one thing it must never do is
 * delete something still in use. A record whose checksum is good and
 * whose contents are wrong is the case -- a bit rotted in the right
 * place, or a kernel bug -- and the replay checks the inode rather than
 * trusting the record: an inode with links is refused, loudly, and left
 * for the checker.
 *
 * Refused rather than treated as a corrupt filesystem, and the choice is
 * deliberate: skipping leaves the filesystem exactly as it was and still
 * mountable, while refusing the mount would take it away from the
 * operator in order to protect it from a leak.
 */
bool selftest_cosmofs_orphan_suspect(const char **reason)
{
    struct blkdev *bd;
    if (!cfst_engine_mount(&bd, 512, reason))
        return false;

    CHECK(cfst_write_file(ENG "/live", "still very much in use", 22));
    CHECK(vfs_sync() == 0);
    struct cosmo_stat st;
    CHECK(vfs_stat(NULL, ENG "/live", &st) == 0);
    CHECK(st.nlink == 1);

    /* The wrong record: it names an inode that has a name. */
    cosmofs_test_orphan_add(cfst_mount_of(ENG), st.ino);
    CHECK(vfs_sync() == 0);
    struct cosmofs_stats fsst;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &fsst) == 0);
    CHECK(fsst.orphan_root != 0);

    CHECK(vfs_umount2(ENG, VFS_UMOUNT_FORCE) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(cfst_mount_of(ENG), false);

    /* The file is still there, with its contents and its name. */
    CHECK(cfst_read_matches(ENG "/live", "still very much in use", 22));
    CHECK(vfs_stat(NULL, ENG "/live", &st) == 0);
    CHECK(st.nlink == 1);
    struct cosmofs_check_report r;
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0);
    CHECK_CLEAN(r);
    CHECK(r.orphan.count == 0);

    kinfo("selftest: cosmofs-orphan-suspect: a record naming a linked inode was refused and the file survived");
    return cfst_engine_unmount(bd, reason);
}

/*
 * An orphan the record did *not* name.
 *
 * This test used to be the defect's own description: it opened a file,
 * unlinked it, committed, dropped the mount with the handle still open,
 * and asserted that the inode survived with its blocks and that only an
 * operator running the checker with the repair flag ever got them back.
 * Its comment called that "the leak the design admits by omission". The
 * omission is now filled -- the root records what it still owes and the
 * next mount does what cfs_evict would have done
 * (docs/audit/next-subsystem-orphan.md, and `cosmofs-orphan-crash` is
 * that story told the right way round).
 *
 * So what is left for this test is the case the checker still owns: an
 * inode with blocks and no name that no record explains, which is what a
 * record that was lost or never written leaves behind. The poison hook
 * makes one directly, because after the orphan unit an ordinary
 * unlinked-but-open file is no longer one.
 */
bool selftest_cosmofs_check_orphan_crash(const char **reason)
{
    struct blkdev *bd = NULL;
    struct cosmofs_check_report r;
    CHECK(cfst_check_fixture(&bd, reason));

    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0 && r.clean);
    uint64_t free_before = r.counted_free;

    /* An inode with blocks that no entry names, and no record naming it
     * either -- the shape a crash used to leave and a lost record still
     * would. */
    uint64_t lost = 0;
    CHECK(cosmofs_test_corrupt(cfst_mount_of(ENG), COSMOFS_CORRUPT_ORPHAN, 0, &lost) == 0);
    CHECK(lost != 0);

    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0);
    CHECK(r.orphan.count == 1);              /* the checker still owns this case */
    CHECK(r.orphan.named == 1 && r.orphan.name[0] == lost);
    CHECK(r.counted_free < free_before);

    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, COSMOFS_CHECK_REPAIR) == 0);
    CHECK(r.orphan.repaired == 1);
    CHECK(vfs_sync() == 0);
    CHECK(vfs_sync() == 0);   /* the deferred frees land on the commit after the repair's */
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0);
    CHECK_CLEAN(r);
    CHECK(r.counted_free == free_before);   /* every block the inode held came back */

    cfst_check_teardown(bd);
    kinfo("selftest: cosmofs-check-orphan-crash: inode %llu had blocks and no name and no record, and the check "
          "reclaimed them",
          (unsigned long long)lost);
    return true;
}

/*
 * A metadata read that fails is not an answer: the pass marks the report
 * incomplete and keeps going, because "one block is unreadable" and "the
 * other nine classes were never looked at" are different facts and an
 * operator needs both. A directory block is the block to break: the walk
 * loses the names it held, and everything those names reached becomes
 * unreachable, which is exactly the cascade a real bad block causes.
 */
/*
 * Two things the first version of the repair got wrong, and the tests
 * that would have caught them.
 *
 * The report names at most CFS_CHECK_NAMES offenders per class, for a
 * reader. Repair used that list as its work list, so a filesystem with
 * more orphans than names kept the rest -- and a checker that reports
 * "repaired" while leaving the same fault behind is worse than one that
 * refuses. Twelve orphans, one repair pass, nothing left.
 *
 * And a slot whose number is not its position is a malformed inode. The
 * walk reaches it by position; believing the number in it would file its
 * accounting under an inode that does not exist, silently, because the
 * maps are sized on next_ino and drop what falls outside.
 */
bool selftest_cosmofs_check_many_orphans(const char **reason)
{
    struct cosmofs_check_report r;
    struct blkdev *bd = NULL;
    uint64_t what = 0;

    CHECK(cfst_check_fixture(&bd, reason));
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0 && r.clean);
    uint64_t free_before = r.counted_free;

    const unsigned many = 12;                  /* more than CFS_CHECK_NAMES */
    for (unsigned i = 0; i < many; i++)
        CHECK(cosmofs_test_corrupt(cfst_mount_of(ENG), COSMOFS_CORRUPT_ORPHAN, 0, &what) == 0);

    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0);
    CHECK(r.orphan.count == many);
    CHECK(r.orphan.named == CFS_CHECK_NAMES);  /* eight named, twelve found */
    /* Each orphan holds one block, and the inode map may have grown to
     * hold the slots, so the free count is lower by at least the twelve. */
    CHECK(r.counted_free <= free_before - many);
    uint64_t free_with_orphans = r.counted_free;

    /* One pass, every one of them. */
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, COSMOFS_CHECK_REPAIR) == 0);
    CHECK(!r.repair_refused);
    CHECK(r.orphan.repaired == many);
    CHECK(vfs_sync() == 0);
    CHECK(vfs_sync() == 0);                    /* the deferred frees land next commit */
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0);
    /* Nothing left: the point of the test. A repair that worked from the
     * eight names would report eight repaired and leave four behind, and
     * this pass would find them. */
    CHECK_CLEAN(r);
    CHECK(r.orphan.count == 0);
    CHECK(r.counted_free > free_with_orphans);   /* and the space came back */
    cfst_check_teardown(bd);

    kinfo("selftest: cosmofs-check-many-orphans: %u orphans, %u named, all repaired in one pass",
          many, (unsigned)CFS_CHECK_NAMES);
    return true;
}

bool selftest_cosmofs_check_slot_identity(const char **reason)
{
    struct cosmofs_check_report r;
    struct blkdev *bd = NULL;
    uint64_t what = 0;

    CHECK(cfst_check_fixture(&bd, reason));
    uint64_t file_ino = cfst_check_file_ino();
    CHECK(file_ino != 0);
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0 && r.clean);
    uint64_t free_before = r.counted_free;

    CHECK(cosmofs_test_corrupt(cfst_mount_of(ENG), COSMOFS_CORRUPT_INO_SLOT, file_ino, &what) == 0);
    CHECK(what == file_ino);

    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0);
    CHECK(!r.clean);
    CHECK(r.dir_bad.count >= 1);
    CHECK(r.dir_bad.name[0] == file_ino);      /* named by position, not by the field */
    CHECK(r.partial);                          /* the accounting is incomplete, and says so */

    /* Repair refuses: the inode's blocks are unclaimed only because the
     * pass would not trust the structure holding them. */
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, COSMOFS_CHECK_REPAIR) == 0);
    CHECK(r.repair_refused);
    CHECK(r.alloc_not_seen.repaired == 0 && r.orphan.repaired == 0);
    CHECK(vfs_sync() == 0);
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0);
    CHECK(r.counted_free == free_before);      /* not one block given away */
    cfst_check_teardown(bd);

    kinfo("selftest: cosmofs-check-slot-identity: inode %llu's slot disowned itself; named, marked incomplete, repair refused",
          (unsigned long long)file_ino);
    return true;
}

/*
 * The whole point of the unit: a fault found and fixed through the
 * device, by an operator naming a mount, rather than by a self-test
 * holding a struct mount the only way anything could.
 *
 * The numbers are compared against the pass called directly, not merely
 * asserted non-zero: a device that ran nothing and returned a zeroed
 * report would pass a weaker test.
 */
bool selftest_fsctl_check(const char **reason)
{
    struct cosmofs_check_report direct;
    struct blkdev *bd = NULL;
    struct file *f = NULL;
    struct vnode *rv = NULL;
    uint8_t *buf = NULL;
    if (!cfst_check_fixture(&bd, reason)) {
        cfst_check_teardown(bd);
        return false;
    }
#define FSCTL_CHECK(cond)                                                     \
    do {                                                                      \
        if (!(cond)) {                                                        \
            *reason = "check failed: " #cond " at line " STR(__LINE__);     \
            goto cleanup;                                                    \
        }                                                                     \
    } while (0)
    uint64_t id = cfst_mount_of(ENG)->id;
    FSCTL_CHECK(id != 0);

    FSCTL_CHECK(vfs_open(NULL, "/dev/fsctl", COSMO_O_RDWR, 0, &f) == 0 && f != NULL);
    size_t cap = sizeof(struct cosmo_fsctl_result) + sizeof(struct cosmo_fsctl_check);
    buf = kmalloc(cap, KMEM_ZERO);
    FSCTL_CHECK(buf != NULL);
    struct cosmo_fsctl_result *h = (struct cosmo_fsctl_result *)buf;
    struct cosmo_fsctl_check *c = (struct cosmo_fsctl_check *)(buf + sizeof(*h));

    struct cosmo_fsctl cmd = { .version = COSMO_FSCTL_VERSION, .op = COSMO_FSCTL_CHECK,
                               .flags = 0, .mount_id = id };

    /* Clean, through the device, and the same numbers the pass reports. */
    FSCTL_CHECK(file_write(f, &cmd, sizeof(cmd)) == (int64_t)sizeof(cmd));
    FSCTL_CHECK(file_read(f, buf, cap) == (int64_t)cap);
    FSCTL_CHECK(h->kind == COSMO_FSCTL_CHECK && h->count == 1);
    FSCTL_CHECK(h->bytes == sizeof(struct cosmo_fsctl_check));
    FSCTL_CHECK(c->nclasses == COSMO_FSCTL_CLASSES);
    FSCTL_CHECK((c->flags & COSMO_FSCTL_R_CLEAN) != 0);
    FSCTL_CHECK((c->flags & COSMO_FSCTL_R_PARTIAL) == 0);
    FSCTL_CHECK(cosmofs_check(cfst_mount_of(ENG), &direct, 0) == 0);
    FSCTL_CHECK(c->blocks_seen == direct.blocks_seen);
    FSCTL_CHECK(c->counted_free == direct.counted_free);
    FSCTL_CHECK(c->inodes_seen == direct.inodes_seen);
    uint64_t free_before = c->counted_free;

    /* A leak, found by number through the device. */
    uint64_t leaked = 0;
    FSCTL_CHECK(cosmofs_test_corrupt(cfst_mount_of(ENG), COSMOFS_CORRUPT_LEAK, 0, &leaked) == 0);
    FSCTL_CHECK(file_write(f, &cmd, sizeof(cmd)) == (int64_t)sizeof(cmd));
    FSCTL_CHECK(file_read(f, buf, cap) == (int64_t)cap);
    FSCTL_CHECK((c->flags & COSMO_FSCTL_R_CLEAN) == 0);
    FSCTL_CHECK(c->class[0].count == 1);                  /* index 0 is alloc_not_seen, and that is ABI */
    FSCTL_CHECK(c->class[0].named == 1 && c->class[0].name[0] == leaked);
    FSCTL_CHECK(c->counted_free == free_before - 1);
    /* A finding is not an error: the write succeeded and said so. */

    /* And repaired through the device, which is the half that mutates. */
    cmd.flags = COSMO_FSCTL_F_REPAIR;
    FSCTL_CHECK(file_write(f, &cmd, sizeof(cmd)) == (int64_t)sizeof(cmd));
    FSCTL_CHECK(file_read(f, buf, cap) == (int64_t)cap);
    FSCTL_CHECK(c->class[0].repaired == 1);
    FSCTL_CHECK((c->flags & COSMO_FSCTL_R_REPAIR_REFUSED) == 0);
    cmd.flags = 0;
    FSCTL_CHECK(vfs_sync() == 0);
    FSCTL_CHECK(file_write(f, &cmd, sizeof(cmd)) == (int64_t)sizeof(cmd));
    FSCTL_CHECK(file_read(f, buf, cap) == (int64_t)cap);
    FSCTL_CHECK((c->flags & COSMO_FSCTL_R_CLEAN) != 0);
    FSCTL_CHECK(c->counted_free == free_before);

    /* A scrub through the same channel, against the same name. */
    struct cosmo_fsctl scmd = { .version = COSMO_FSCTL_VERSION, .op = COSMO_FSCTL_SCRUB,
                                .flags = 0, .mount_id = id };
    size_t scap = sizeof(struct cosmo_fsctl_result) + sizeof(struct cosmo_fsctl_scrub);
    FSCTL_CHECK(file_write(f, &scmd, sizeof(scmd)) == (int64_t)sizeof(scmd));
    FSCTL_CHECK(file_read(f, buf, scap) == (int64_t)scap);
    struct cosmo_fsctl_scrub *sc = (struct cosmo_fsctl_scrub *)(buf + sizeof(*h));
    FSCTL_CHECK(h->kind == COSMO_FSCTL_SCRUB);
    FSCTL_CHECK(sc->blocks_read > 0 && sc->unrecoverable == 0);

    /* The refusals. A ramfs has neither pass; a name nothing holds is
     * not a mount this namespace has. Both answer before any lock. */
    struct mount *rootm = NULL;
    FSCTL_CHECK(vfs_lookup(NULL, "/tmp", &rv) == 0);
    rootm = rv->mnt;
    vnode_put(rv);
    rv = NULL;
    cmd.mount_id = rootm->id;
    FSCTL_CHECK(file_write(f, &cmd, sizeof(cmd)) == -EOPNOTSUPP);
    FSCTL_CHECK(file_read(f, buf, cap) == 0);             /* a failed command leaves no result */

    cmd.mount_id = ~0ull;
    FSCTL_CHECK(file_write(f, &cmd, sizeof(cmd)) == -ENOENT);
    cmd.mount_id = id;
    cmd.version = COSMO_FSCTL_VERSION + 1;
    FSCTL_CHECK(file_write(f, &cmd, sizeof(cmd)) == -EINVAL);
    cmd.version = COSMO_FSCTL_VERSION;
    FSCTL_CHECK(file_write(f, &cmd, sizeof(cmd) - 1) == -EINVAL);   /* whole, at its exact size */

    /*
     * Every bit of `flags` must mean something to the op it is sent
     * with. A bit nobody defined is a caller's mistake now and an
     * ambiguity later, when a version gives it a meaning and an old
     * writer turns out to have been setting it.
     */
    cmd.flags = 1u << 31;
    FSCTL_CHECK(file_write(f, &cmd, sizeof(cmd)) == -EINVAL);
    scmd.flags = COSMO_FSCTL_F_REPAIR;          /* CHECK-only, on a SCRUB */
    FSCTL_CHECK(file_write(f, &scmd, sizeof(scmd)) == -EINVAL);
    struct cosmo_fsctl lcmd = { .version = COSMO_FSCTL_VERSION, .op = COSMO_FSCTL_LIST,
                                .flags = COSMO_FSCTL_F_REPAIR };
    FSCTL_CHECK(file_write(f, &lcmd, sizeof(lcmd)) == -EINVAL);   /* LIST takes none */
    cmd.flags = 0;
    scmd.flags = 0;

    kfree(buf);
    buf = NULL;
    file_put(f);
    f = NULL;
    cfst_check_teardown(bd);
    bd = NULL;
    kinfo("selftest: fsctl-check: a leak found and repaired through /dev/fsctl against mount %llu",
          (unsigned long long)id);
    return true;

cleanup:
    if (rv)
        vnode_put(rv);
    if (f)
        file_put(f);
    kfree(buf);
    cfst_check_teardown(bd);
    return false;
#undef FSCTL_CHECK
}

bool selftest_cosmofs_check_partial(const char **reason)
{
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    struct blkdev *bd = ramblk_create(512);
    CHECK(bd != NULL);
    CHECK(cosmofs_format(bd) == 0);
    int mk = vfs_mkdir(NULL, ENG, 0755);
    CHECK(mk == 0 || mk == -EEXIST);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(cfst_mount_of(ENG), false);

    CHECK(vfs_mkdir(NULL, ENG "/d", 0755) == 0);
    CHECK(cfst_write_file(ENG "/d/x", "held by a name in the block about to go", 39));
    CHECK(cfst_write_file(ENG "/keep", "reached another way", 19));
    CHECK(vfs_sync() == 0);

    struct cosmofs_check_report rep;
    CHECK(cfst_check_is_clean(reason, &rep) && rep.clean);
    struct cosmo_stat dst;
    CHECK(vfs_stat(NULL, ENG "/d", &dst) == 0);
    uint64_t dir_ino = dst.ino;

    /* Break the one block that holds /d's entries, off the mount so the
     * page cache cannot answer from memory. */
    CHECK(vfs_umount(ENG) == 0);
    struct spool *p;
    CHECK(pool_open(bd, &p) == 0);
    uint8_t *blk = kmalloc(4096, 0);
    CHECK(blk != NULL);
    int64_t dir_blk = -1;
    for (uint64_t i = 2; i < p->nblocks && dir_blk < 0; i++) {
        if (pool_read(p, i, blk) != 0)
            continue;
        const struct cfs_dirent *d = (const struct cfs_dirent *)blk;
        if (d[0].ino != 0 && d[0].ino < 1000 && d[0].namelen == 1 && d[0].name[0] == 'x' && d[1].ino == 0)
            dir_blk = (int64_t)i;
    }
    kfree(blk);
    pool_close(p);
    CHECK(dir_blk > 0);
    CHECK(cfst_corrupt_block(bd, (uint64_t)dir_blk, 40));
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(cfst_mount_of(ENG), false);

    /* The pass finishes and says so: an answer, and a warning that it is
     * not the whole answer. */
    CHECK(cosmofs_check(cfst_mount_of(ENG), &rep, 0) == 0);
    CHECK(rep.partial);
    CHECK(!rep.clean);
    CHECK(rep.unreadable.count == 1);
    CHECK(rep.unreadable.named == 1 && rep.unreadable.name[0] == dir_ino);
    /*
     * The comparison is the last phase, so a finding only it can produce
     * is the proof that the pass reached the end rather than stopping at
     * the unreadable block. That finding is the **orphan**: /d/x's inode
     * is in the map and no name reaches it, which only the final walk of
     * the inode maps can conclude.
     *
     * This used to assert a leaked block instead, and that assertion was
     * being satisfied by something else entirely: before format version
     * 9 every unmount stranded its last transaction's frees, so there
     * was always a leak to find and the test passed for a reason its own
     * comment did not give. /d/x's blocks are *seen* -- the pass reaches
     * them through the orphaned inode -- so they were never the leak
     * this line was reading (docs/audit/next-subsystem-unmount-leak.md).
     */
    CHECK(rep.orphan.count >= 1);
    CHECK(rep.blocks_seen > 0);

    /*
     * And repair must refuse. Every repair this pass makes is an argument
     * from absence, and the absence here is the pass's own: /d/x is a
     * live file whose name went with the block. Freeing "its leaked
     * blocks" and clearing "its orphaned inode" would destroy it.
     */
    uint64_t lost_blocks = rep.alloc_not_seen.count;
    CHECK(cosmofs_check(cfst_mount_of(ENG), &rep, COSMOFS_CHECK_REPAIR) == 0);
    CHECK(rep.repair_refused);
    CHECK(rep.alloc_not_seen.repaired == 0);
    CHECK(rep.orphan.repaired == 0);
    CHECK(rep.counter_wrong.repaired == 0);
    CHECK(vfs_sync() == 0);
    CHECK(cosmofs_check(cfst_mount_of(ENG), &rep, 0) == 0);
    CHECK(rep.alloc_not_seen.count == lost_blocks);   /* nothing was given away */
    /* The file the broken block named is still whole: the block it lives
     * in was never freed, so the entry that survives elsewhere still
     * reaches it. */
    CHECK(cfst_read_matches(ENG "/keep", "reached another way", 19));

    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_rmdir(NULL, ENG) == 0);
    ramblk_destroy(bd);
    kinfo("selftest: cosmofs-check-partial: directory inode %llu unreadable, %llu blocks stranded, report marked incomplete, repair refused",
          (unsigned long long)dir_ino, (unsigned long long)lost_blocks);
    return true;
}

}

}
#else
bool selftest_cosmofs_snapshot_remount(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_snap_identity(const char **reason) { (void)reason; return true; }
#endif
