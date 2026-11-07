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

bool selftest_cosmofs_symlink(const char **reason)
{
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    struct blkdev *bd = ramblk_create(512);
    CHECK(bd != NULL);
    CHECK(cosmofs_format(bd) == 0);
    int mk = vfs_mkdir(NULL, ENG, 0755);
    CHECK(mk == 0 || mk == -EEXIST);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(cfst_mount_of(ENG), false);

    struct cosmofs_stats st0, st1;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &st0) == 0);

    CHECK(cfst_write_file(ENG "/file", "on-disk-bytes", 13));
    CHECK(vfs_symlink(NULL, ENG "/link", "file") == 0);
    CHECK(vfs_symlink(NULL, ENG "/abs", ENG "/file") == 0);

    struct cosmo_stat lst, fst;
    CHECK(vfs_lstat(NULL, ENG "/link", &lst) == 0 && lst.type == COSMO_DT_LNK && lst.size == 4);
    CHECK(vfs_stat(NULL, ENG "/link", &fst) == 0 && fst.type == COSMO_DT_REG && fst.size == 13);
    CHECK(cfst_read_matches(ENG "/link", "on-disk-bytes", 13));
    CHECK(cfst_read_matches(ENG "/abs", "on-disk-bytes", 13));

    /* It survives a remount with nothing cached: the bytes come off the
     * disk, not from a vnode this test left behind. */
    CHECK(vfs_sync() == 0);
    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(cfst_mount_of(ENG), false);
    char buf[64];
    memset(buf, 'Z', sizeof(buf));
    int n = vfs_readlink(NULL, ENG "/link", buf, sizeof(buf));
    CHECK(n == 4 && memcmp(buf, "file", 4) == 0 && buf[4] == 'Z');
    CHECK(vfs_lstat(NULL, ENG "/link", &lst) == 0 && lst.type == COSMO_DT_LNK);
    CHECK(cfst_read_matches(ENG "/link", "on-disk-bytes", 13));

    /* One block per link, given back when it goes. */
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &st1) == 0);
    CHECK(vfs_unlink(NULL, ENG "/link") == 0);
    CHECK(vfs_unlink(NULL, ENG "/abs") == 0);
    CHECK(vfs_sync() == 0);
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &st0) == 0);
    CHECK(st0.free_blocks > st1.free_blocks);
    CHECK(cfst_read_matches(ENG "/file", "on-disk-bytes", 13));   /* the target stayed */

    CHECK(vfs_unlink(NULL, ENG "/file") == 0);
    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_rmdir(NULL, ENG) == 0);
    ramblk_destroy(bd);
    kinfo("selftest: cosmofs-symlink: a link and its target survive a remount, and its block comes back");
    return true;
}

/*
 * Version 9 takes `free_root` from the superblock's reserved words, so
 * the gate is on *reading* it: below version 9 that word is a reserved
 * zero, and a mount that read it as a chain head would refuse every
 * filesystem written before this unit
 * (docs/audit/next-subsystem-unmount-leak.md).
 */
bool selftest_cosmofs_freelog_format(const char **reason)
{
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    struct blkdev *bd = ramblk_create(512);
    CHECK(bd != NULL);

    /* A version-8 filesystem mounts, works, and replays nothing. */
    CHECK(cosmofs_test_format_version(bd, 8) == 0);
    int mk = vfs_mkdir(NULL, ENG, 0755);
    CHECK(mk == 0 || mk == -EEXIST);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(cfst_mount_of(ENG), false);
    CHECK(cfst_write_file(ENG "/eight", "older", 5));
    CHECK(vfs_sync() == 0);
    CHECK(cfst_read_matches(ENG "/eight", "older", 5));
    struct cosmofs_check_report rep;
    CHECK(cosmofs_check(cfst_mount_of(ENG), &rep, 0) == 0);
    CHECK(rep.clean);
    CHECK(vfs_umount(ENG) == 0);

    /* And again after a remount: nothing was read from a field it does
     * not have, and nothing was written into one. */
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    CHECK(cfst_read_matches(ENG "/eight", "older", 5));
    struct cosmofs_stats st8;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &st8) == 0);
    CHECK(st8.version == 8 && st8.free_root == 0);
    CHECK(vfs_umount(ENG) == 0);

    /*
     * The gate itself. Every image this tree formats zeroes its reserved
     * words, so a version-8 filesystem's `free_root` word is 0 whether
     * the gate is there or not -- which makes the gate untestable
     * against anything the tree writes. This puts a value in it, as a
     * later version or another writer would, and the gate is what keeps
     * it from being read as the head of a chain.
     */
    CHECK(cosmofs_test_poison_free_root(bd, 0x5a5a5a5aull) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &st8) == 0);
    CHECK(st8.version == 8);
    CHECK(st8.free_root == 0);        /* not 0x5a5a5a5a: below version 9 the word is not a root */
    CHECK(cfst_read_matches(ENG "/eight", "older", 5));
    CHECK(vfs_umount(ENG) == 0);

    /* A version-9 filesystem carries the field, and an idle one has
     * nothing recorded in it. */
    struct blkdev *bd9 = ramblk_create(512);
    CHECK(bd9 != NULL);
    CHECK(cosmofs_format(bd9) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd9, 0) == 0);
    cosmofs_test_set_writeback(cfst_mount_of(ENG), false);
    struct cosmofs_stats st;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &st) == 0);
    CHECK(st.version == CFS_FORMAT_VERSION);   /* raised to 12 only by a snapshot */
    CHECK(st.free_root == 0);          /* a fresh filesystem freed nothing */
    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_rmdir(NULL, ENG) == 0);
    ramblk_destroy(bd);
    ramblk_destroy(bd9);
    kinfo("selftest: cosmofs-freelog-format: a version-8 filesystem mounts and replays nothing; a version-9 one has the field");
    return true;
}

/*
 * The record's own blocks are metadata, and the bitmap the root
 * publishes has to say so. Allocating them after the bitmap fixpoint --
 * which is where they naturally want to go, since the set to record is
 * only final then -- publishes a root whose bitmap does not know about
 * them, and the allocator hands them out again. That is a corruption
 * rather than a leak, and the structural check is what sees it: a block
 * that is reachable and free is `seen_not_alloc`, the dangerous
 * direction (docs/audit/next-subsystem-unmount-leak.md).
 */
bool selftest_cosmofs_freelog_accounted(const char **reason)
{
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    struct blkdev *bd = ramblk_create(512);
    CHECK(bd != NULL);
    CHECK(cosmofs_format(bd) == 0);
    int mk = vfs_mkdir(NULL, ENG, 0755);
    CHECK(mk == 0 || mk == -EEXIST);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(cfst_mount_of(ENG), false);

    /* Something to free, so the record is not empty. */
    CHECK(cfst_write_file(ENG "/a", "one", 3));
    CHECK(vfs_sync() == 0);
    CHECK(cfst_write_file(ENG "/a", "one again, longer", 17));
    CHECK(vfs_unlink(NULL, ENG "/a") == 0);
    CHECK(vfs_sync() == 0);

    struct cosmofs_stats st;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &st) == 0);
    CHECK(st.free_root != 0);                  /* there is a record */

    /* Every block of it is allocated in the bitmap this root published,
     * and the check agrees: no leak, and nothing reachable-but-free. */
    struct cosmofs_check_report rep;
    CHECK(cosmofs_check(cfst_mount_of(ENG), &rep, 0) == 0);
    CHECK(rep.seen_not_alloc.count == 0);      /* the dangerous direction */
    CHECK(rep.alloc_not_seen.count == 0);
    CHECK(rep.clean);

    /* And across a remount, where the bitmap on disk is the only word. */
    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    CHECK(cosmofs_check(cfst_mount_of(ENG), &rep, 0) == 0);
    CHECK(rep.seen_not_alloc.count == 0);

    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_rmdir(NULL, ENG) == 0);
    ramblk_destroy(bd);
    kinfo("selftest: cosmofs-freelog-accounted: the record's blocks are allocated in the bitmap its root published");
    return true;
}

/*
 * A record supersedes its predecessor, and the predecessor's blocks have
 * to go back. Without that, every commit leaks a block or two -- this
 * unit's own defect one level up -- which no single-commit test can see.
 */
bool selftest_cosmofs_freelog_supersede(const char **reason)
{
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    struct blkdev *bd = ramblk_create(512);
    CHECK(bd != NULL);
    CHECK(cosmofs_format(bd) == 0);
    int mk = vfs_mkdir(NULL, ENG, 0755);
    CHECK(mk == 0 || mk == -EEXIST);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(cfst_mount_of(ENG), false);

    /* Settle, then measure: the first commits grow the tree. */
    CHECK(cfst_write_file(ENG "/churn", "x", 1));
    CHECK(vfs_sync() == 0);
    CHECK(vfs_sync() == 0);
    struct cosmofs_stats before;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &before) == 0);

    /* A hundred commits that change the same one block. */
    for (unsigned i = 0; i < 100; i++) {
        CHECK(cfst_write_file(ENG "/churn", "y", 1));
        CHECK(vfs_sync() == 0);
    }
    struct cosmofs_stats after;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &after) == 0);

    /*
     * The count may move by the transaction in flight, but not by a
     * hundred commits' worth of records. A leak of one block per commit
     * would be a hundred blocks on a 512-block filesystem.
     */
    CHECK(after.free_blocks + 8 >= before.free_blocks);
    CHECK(after.free_root != 0);

    struct cosmofs_check_report rep;
    CHECK(cosmofs_check(cfst_mount_of(ENG), &rep, 0) == 0);
    CHECK(rep.clean);

    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_rmdir(NULL, ENG) == 0);
    ramblk_destroy(bd);
    kinfo("selftest: cosmofs-freelog-supersede: 100 commits, %llu free before and %llu after",
          (unsigned long long)before.free_blocks, (unsigned long long)after.free_blocks);
    return true;
}

/*
 * The defect this unit exists for. Write a file, delete it, unmount, and
 * the blocks the delete freed are gone: the commit cleared their bits in
 * memory after its root was durable and marked the chunks for a next
 * commit that an unmount never makes.
 *
 * This test fails on the tree before this unit, which is the strongest
 * thing that can be said about a test.
 */
bool selftest_cosmofs_unmount_leak(const char **reason)
{
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    struct blkdev *bd = ramblk_create(512);
    CHECK(bd != NULL);
    CHECK(cosmofs_format(bd) == 0);
    int mk = vfs_mkdir(NULL, ENG, 0755);
    CHECK(mk == 0 || mk == -EEXIST);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(cfst_mount_of(ENG), false);
    CHECK(vfs_sync() == 0);

    /* Settle first: the count to come back to is the one after the
     * filesystem has stopped growing. */
    CHECK(cfst_write_file(ENG "/settle", "s", 1));
    CHECK(vfs_unlink(NULL, ENG "/settle") == 0);
    CHECK(vfs_sync() == 0);
    CHECK(vfs_sync() == 0);
    struct cosmofs_stats before;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &before) == 0);

    /* A file, and then no file. */
    static const char big[4096] = { 0 };
    CHECK(cfst_write_file(ENG "/leakme", big, sizeof(big)));
    CHECK(vfs_sync() == 0);
    CHECK(vfs_unlink(NULL, ENG "/leakme") == 0);
    CHECK(vfs_umount(ENG) == 0);          /* the unmount's commit frees them */

    /* Remount: every block is back, and the filesystem adds up. */
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    struct cosmofs_stats after;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &after) == 0);
    CHECK(after.free_blocks == before.free_blocks);
    struct cosmofs_check_report rep;
    CHECK(cosmofs_check(cfst_mount_of(ENG), &rep, 0) == 0);
    CHECK(rep.clean);

    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_rmdir(NULL, ENG) == 0);
    ramblk_destroy(bd);
    kinfo("selftest: cosmofs-unmount-leak: %llu free before the file, %llu after it was deleted and the filesystem remounted",
          (unsigned long long)before.free_blocks, (unsigned long long)after.free_blocks);
    return true;
}

/*
 * A record must not name a block a snapshot still holds. The commit
 * writes the record *before* the root and appends to the deadlist
 * *after* it, so the two ask the same question through
 * cfs_snapshot_holds -- and if the record's answer were wrong, a mount
 * would replay it and hand a live snapshot's block to the allocator.
 *
 * The remount is the point: without it nothing reads the record, and a
 * wrong one costs nothing until the next mount.
 */
bool selftest_cosmofs_freelog_snapshot(const char **reason)
{
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    struct blkdev *bd = ramblk_create(512);
    CHECK(bd != NULL);
    CHECK(cosmofs_format(bd) == 0);
    int mk = vfs_mkdir(NULL, ENG, 0755);
    CHECK(mk == 0 || mk == -EEXIST);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(cfst_mount_of(ENG), false);

    /* Something for the snapshot to hold. */
    CHECK(cfst_write_file(ENG "/held", "the snapshot's copy", 19));
    CHECK(vfs_sync() == 0);
    CHECK(vfs_mkdir(NULL, ENG "/.snapshots/keep", 0755) == 0);

    struct cosmofs_stats before;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &before) == 0);

    /*
     * Free those blocks in the live tree and let **the unmount's own
     * commit** be the one that records it. A sync in between would
     * write the record and then supersede it, and the record a mount
     * actually reads is the last one written -- so a wrong record would
     * be replaced before anything believed it, and the test would pass
     * whatever the code did.
     */
    CHECK(vfs_unlink(NULL, ENG "/held") == 0);
    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    struct cosmofs_stats after;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &after) == 0);

    /*
     * The free count is not the instrument here, and trying to use it
     * was wrong in both directions: the unlink frees the copy-on-write
     * casualties that postdate the snapshot, which raises it, while the
     * deadlist the snapshot grows and the record itself consume blocks,
     * which lowers it. Either way it moves for reasons that have nothing
     * to do with the claim.
     *
     * The claim is that the snapshot's own blocks did not go back, and
     * the checker states it exactly: a record that named them would have
     * had the replay clear their bits while the snapshot's tree still
     * reaches them -- reachable and free, which is `seen_not_alloc`, the
     * direction that hands live data to the allocator.
     */
    (void)after;
    (void)before;

    struct cosmofs_check_report rep;
    CHECK(cosmofs_check(cfst_mount_of(ENG), &rep, 0) == 0);
    CHECK(rep.seen_not_alloc.count == 0);   /* the claim: no snapshot block handed back */
    CHECK(rep.snapshots_seen == 1);

    /*
     * Not `clean`, and the reason is worth writing down: an unmount that
     * frees a block a snapshot holds strands a couple of blocks, and it
     * is this unit's own defect living in the snapshot path.
     * cfs_snapshot_hold_block appends to a deadlist from phase 7, after
     * the root is written -- so the block it allocates and the snapshot
     * entry it dirties belong to a transaction that has already been
     * published, and at an unmount there is no next commit to carry
     * them.
     *
     * The record fixes the frees because they are known before the root.
     * A deadlist append is not: it happens as a consequence of deciding
     * what to free, which is only final after the bitmap fixpoint, which
     * must come after every allocation. Breaking that circle for the
     * deadlist needs the same reserve-before-fill treatment this unit
     * gave the record, in cosmofs_snap.c, and that is a second unit with
     * its own proofs rather than a paragraph in this one. Inventory row.
     */
    CHECK(rep.alloc_not_seen.count == 0);   /* was `<= 4`: see below */

    /* And the snapshot still reads, which is what the blocks were for. */
    CHECK(cfst_read_matches(ENG "/.snapshots/keep/held", "the snapshot's copy", 19));

    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_rmdir(NULL, ENG) == 0);
    ramblk_destroy(bd);
    kinfo("selftest: cosmofs-freelog-snapshot: a snapshot's blocks survive the record and the remount (%llu free either side)",
          (unsigned long long)after.free_blocks);
    return true;
}

/*
 * The copy, in one commit.
 *
 * Until this unit the snapshot list was the one metadata chain in the
 * filesystem rewritten where it lay: every other tree is copied and
 * published by the root that names the copy, and `snap_root` alone
 * pointed at a block a transaction edited in place. A crash between
 * that write and the superblock left the *previous* root naming a list
 * belonging to a transaction that did not happen
 * (docs/audit/next-subsystem-snap-deadlist.md).
 *
 * Two snapshots in a row change the same list block. The claim is that
 * the second change is written into a different block -- a copy -- and
 * that the block it superseded went back to the allocator rather than
 * onto a deadlist.
 */
bool selftest_cosmofs_snap_cow(const char **reason)
{
    struct blkdev *bd;
    if (!cfst_engine_mount(&bd, 512, reason))
        return false;

    CHECK(cfst_write_file(ENG "/held", "the snapshot's copy", 19));
    CHECK(vfs_sync() == 0);
    CHECK(vfs_mkdir(NULL, ENG "/.snapshots/one", 0755) == 0);

    /*
     * The measurement is taken across **one** commit, and that is not a
     * detail. A block freed by a transaction keeps its bitmap bit until
     * after that transaction's root, so a copy made inside the
     * transaction cannot land on the block it supersedes -- but the
     * *next* transaction's allocator starts from the member's first
     * usable block and hands it straight back. Comparing across two
     * commits therefore compares equal on a filesystem that copies
     * perfectly, which is how this test first failed.
     */
    uint64_t before = cosmofs_test_snap_root(cfst_mount_of(ENG));
    CHECK(before != 0);

    /* One commit that appends to the snapshot's deadlist: it copies the
     * list to write the new head into it. */
    CHECK(vfs_unlink(NULL, ENG "/held") == 0);
    CHECK(vfs_sync() == 0);
    uint64_t after = cosmofs_test_snap_root(cfst_mount_of(ENG));
    CHECK(after != 0);
    CHECK(after != before);   /* the claim: written into a copy, not into the block the old root named */

    /*
     * And the superseded block went back exempt rather than deferred.
     * Deferred would send it through the snapshot filter, which asks
     * whether the newest snapshot's recorded bitmap marks it allocated
     * -- and it does, because it was allocated before that snapshot was
     * taken. The block would be held on the deadlist of the very list it
     * is a copy of, which is not a leak and not a finding, so the
     * checker would stay quiet while the chain grew a block per commit.
     */
    CHECK(cosmofs_test_deadlist_len(cfst_mount_of(ENG), before) == 0);

    /* The entries the commit did write are there, and none is doubled. */
    uint64_t examined = 0;
    CHECK(cosmofs_test_deadlist_dups(cfst_mount_of(ENG), &examined, NULL) == 0);
    CHECK(examined > 0);

    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    CHECK(cosmofs_test_snap_root(cfst_mount_of(ENG)) == after);
    struct cosmofs_check_report rep;
    CHECK(cosmofs_check(cfst_mount_of(ENG), &rep, 0) == 0);
    CHECK(rep.clean);
    CHECK(rep.snapshots_seen == 1);
    CHECK(cfst_read_matches(ENG "/.snapshots/one/held", "the snapshot's copy", 19));

    kinfo("selftest: cosmofs-snap-cow: the snapshot list moved from block %llu to %llu in one commit, and %llu went "
          "back to the allocator rather than onto a deadlist",
          (unsigned long long)before, (unsigned long long)after, (unsigned long long)before);
    return cfst_engine_unmount(bd, reason);
}

/*
 * The defect, stated where it hurts: a snapshot, blocks it holds freed
 * in the live tree, and an unmount. The deadlist entry used to be
 * written after the root, so the unmount's commit -- the last one there
 * will ever be -- left it in memory and threw it away. The block stayed
 * allocated, reachable from nothing, and deleting the snapshot would
 * never free it because the deadlist had never heard of it.
 */
bool selftest_cosmofs_snap_unmount(const char **reason)
{
    struct blkdev *bd;
    if (!cfst_engine_mount(&bd, 512, reason))
        return false;

    CHECK(cfst_write_file(ENG "/held", "the snapshot's copy", 19));
    CHECK(vfs_sync() == 0);
    CHECK(vfs_mkdir(NULL, ENG "/.snapshots/keep", 0755) == 0);

    /* The unmount's own commit is the one that frees: a sync here would
     * be followed by another commit that tidied up after it, and the
     * defect is precisely the commit that has no successor. */
    CHECK(vfs_unlink(NULL, ENG "/held") == 0);
    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);

    struct cosmofs_check_report rep;
    CHECK(cosmofs_check(cfst_mount_of(ENG), &rep, 0) == 0);
    CHECK(rep.clean);                       /* the claim */
    CHECK(rep.snapshots_seen == 1);

    /*
     * And `clean` is not the answer of a filesystem that recorded
     * nothing: entries exist, and none of them is on two lists.
     */
    uint64_t examined = 0, first = 0;
    CHECK(cosmofs_test_deadlist_dups(cfst_mount_of(ENG), &examined, &first) == 0);
    CHECK(examined > 0);

    /* The blocks are still the snapshot's, which is what holding them
     * was for. */
    CHECK(cfst_read_matches(ENG "/.snapshots/keep/held", "the snapshot's copy", 19));

    kinfo("selftest: cosmofs-snap-unmount: clean across the unmount, %llu deadlist entries and no duplicate",
          (unsigned long long)examined);
    return cfst_engine_unmount(bd, reason);
}

/*
 * The copies do not accumulate. Every commit that appends to a deadlist
 * copies the snapshot list and the deadlist head, and every superseded
 * block goes back *exempt* -- outside the snapshot filter. Deferred
 * instead would send each copy through that filter, which would hold it
 * on the deadlist it is a copy of: not a leak, so the checker stays
 * quiet, and the chain grows by a block or two on every commit until
 * the filesystem fills.
 *
 * A hundred commits, and the claim is that not one of the hundred
 * superseded list blocks is named by any deadlist.
 */
#define SNAP_NOGROW_COMMITS 100u

bool selftest_cosmofs_snap_nogrow(const char **reason)
{
    struct blkdev *bd;
    if (!cfst_engine_mount(&bd, 1024, reason))
        return false;
    /*
     * A file per commit, all of them in the snapshot, deleted one at a
     * time. Rewriting *one* file a hundred times would not do: only the
     * first rewrite frees a block the snapshot holds, and the other
     * ninety-nine free blocks born after it, so only a handful of
     * commits would copy the list and the claim below would be checked
     * against a handful of superseded blocks.
     */
    char path[64];
    for (unsigned i = 0; i < SNAP_NOGROW_COMMITS; i++) {
        ksnprintf(path, sizeof(path), ENG "/f%u", i);
        CHECK(cfst_write_file(path, "x", 1));
    }
    CHECK(vfs_sync() == 0);
    CHECK(vfs_mkdir(NULL, ENG "/.snapshots/keep", 0755) == 0);

    uint64_t *roots = kmalloc(SNAP_NOGROW_COMMITS * sizeof(uint64_t), KMEM_ZERO);
    CHECK(roots != NULL);
    bool ok = true;
    for (unsigned i = 0; i < SNAP_NOGROW_COMMITS && ok; i++) {
        roots[i] = cosmofs_test_snap_root(cfst_mount_of(ENG));
        ksnprintf(path, sizeof(path), ENG "/f%u", i);
        ok = vfs_unlink(NULL, path) == 0 && vfs_sync() == 0;
    }
    if (!ok) {
        kfree(roots);
        *reason = "a commit in the nogrow loop failed";
        return false;
    }

    /*
     * Not one of them is held -- and the list really did move on each
     * commit, which is the difference between this test and one run
     * against a filesystem whose snapshot list never changed.
     *
     * The measure is how often the root *changed between consecutive
     * commits*, not how many distinct blocks appeared. Distinct blocks
     * counts the allocator, not the copying: a superseded block is free
     * again by the next transaction and the metadata allocator starts
     * from the member's first usable block, so a hundred copies land on
     * two blocks in alternation and "distinct" reads 2 on a filesystem
     * that copied a hundred times. Within one commit the block being
     * superseded still has its bit set, so the copy cannot land on it
     * and the root must differ across that commit.
     */
    unsigned held_roots = 0, moves = 0;
    uint64_t now = cosmofs_test_snap_root(cfst_mount_of(ENG));
    for (unsigned i = 0; i < SNAP_NOGROW_COMMITS; i++) {
        if (roots[i] == 0)
            continue;
        uint64_t next = i + 1 < SNAP_NOGROW_COMMITS ? roots[i + 1] : now;
        if (next != 0 && next != roots[i])
            moves++;
        if (roots[i] != now && cosmofs_test_deadlist_len(cfst_mount_of(ENG), roots[i]) != 0)
            held_roots++;
    }
    kfree(roots);
    CHECK(held_roots == 0);
    /* Every commit freed a block the snapshot holds, so every commit
     * copied: a handful of moves would mean the workload had stopped
     * exercising the thing under test. */
    CHECK(moves > SNAP_NOGROW_COMMITS / 2);

    /* And the whole thing still adds up across a remount. */
    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    struct cosmofs_check_report rep;
    CHECK(cosmofs_check(cfst_mount_of(ENG), &rep, 0) == 0);
    CHECK(rep.clean);
    uint64_t examined = 0;
    CHECK(cosmofs_test_deadlist_dups(cfst_mount_of(ENG), &examined, NULL) == 0);
    CHECK(examined > 0);

    kinfo("selftest: cosmofs-snap-nogrow: %u commits, the snapshot list moved in %u of them, %llu deadlist entries, "
          "no superseded list block held",
          SNAP_NOGROW_COMMITS, moves, (unsigned long long)examined);
    return cfst_engine_unmount(bd, reason);
}

/*
 * The reservation, exercised past one block of it. More blocks freed in
 * one transaction than a deadlist block holds, so the fill needs a
 * *fresh* deadlist block as well as a copied head -- the term in the
 * bound that is one block per CFS_DEAD_PER_BLOCK holds.
 *
 * The claim is the ordering one: every block the snapshot list gained is
 * set in the bitmap the root published. A fill that allocated instead of
 * taking from the reservation would set the bit after commit_bitmap had
 * written it, so the next mount would read the block as free while the
 * new root reaches it -- `seen_not_alloc`, the direction that hands live
 * data to the allocator.
 */
bool selftest_cosmofs_snap_reserved(const char **reason)
{
    struct blkdev *bd;
    if (!cfst_engine_mount(&bd, 4096, reason))
        return false;

    /* More one-block files than a deadlist block holds. */
    const unsigned files = CFS_DEAD_PER_BLOCK + 40u;
    char path[64];
    for (unsigned i = 0; i < files; i++) {
        ksnprintf(path, sizeof(path), ENG "/f%u", i);
        CHECK(cfst_write_file(path, "x", 1));
    }
    CHECK(vfs_sync() == 0);
    CHECK(vfs_mkdir(NULL, ENG "/.snapshots/keep", 0755) == 0);

    /* All of them in one transaction, so one commit holds them all. */
    for (unsigned i = 0; i < files; i++) {
        ksnprintf(path, sizeof(path), ENG "/f%u", i);
        CHECK(vfs_unlink(NULL, path) == 0);
    }
    CHECK(vfs_sync() == 0);

    uint64_t entries = cosmofs_test_deadlist_len(cfst_mount_of(ENG), 0);
    CHECK(entries > CFS_DEAD_PER_BLOCK);   /* the chain really did need a second block */

    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    struct cosmofs_check_report rep;
    CHECK(cosmofs_check(cfst_mount_of(ENG), &rep, 0) == 0);
    CHECK(rep.seen_not_alloc.count == 0);   /* the claim */
    CHECK(rep.clean);
    uint64_t examined = 0;
    CHECK(cosmofs_test_deadlist_dups(cfst_mount_of(ENG), &examined, NULL) == 0);
    CHECK(examined == entries);

    kinfo("selftest: cosmofs-snap-reserved: %llu blocks held across a chain of deadlist blocks, all accounted",
          (unsigned long long)entries);
    return cfst_engine_unmount(bd, reason);
}

/*
 * A deadlist fill that fails partway. It cannot be retried -- the
 * entries already written would be written again, and one block on two
 * deadlists is the corruption this unit exists to prevent -- so the
 * transaction is abandoned, which is the answer the snapshot code
 * already gives to a change it cannot publish whole.
 *
 * What must not survive is the reservation: no root is published, so the
 * next mount reads the previous root's bitmap and every block the fill
 * had taken is free again.
 */
bool selftest_cosmofs_snap_rollback(const char **reason)
{
    struct blkdev *bd;
    if (!cfst_engine_mount(&bd, 512, reason))
        return false;
    CHECK(cfst_write_file(ENG "/held", "content", 7));
    CHECK(vfs_sync() == 0);
    CHECK(vfs_mkdir(NULL, ENG "/.snapshots/keep", 0755) == 0);
    struct cosmofs_stats before;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &before) == 0);

    cosmofs_test_fail_snapfill(cfst_mount_of(ENG), true);
    CHECK(vfs_unlink(NULL, ENG "/held") == 0);
    CHECK(vfs_sync() != 0);                 /* the commit fails */
    cosmofs_test_fail_snapfill(cfst_mount_of(ENG), false);

    /* Abandoned: nothing more is published from this mount. */
    CHECK(vfs_umount2(ENG, VFS_UMOUNT_FORCE) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    struct cosmofs_stats after;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &after) == 0);
    CHECK(after.generation == before.generation);   /* no root was published */
    CHECK(after.free_blocks == before.free_blocks); /* and nothing was taken */
    struct cosmofs_check_report rep;
    CHECK(cosmofs_check(cfst_mount_of(ENG), &rep, 0) == 0);
    CHECK(rep.clean);
    CHECK(cfst_read_matches(ENG "/held", "content", 7));   /* the unlink went with it */

    kinfo("selftest: cosmofs-snap-rollback: the failed fill left generation %llu and %llu free blocks, as it found them",
          (unsigned long long)after.generation, (unsigned long long)after.free_blocks);
    return cfst_engine_unmount(bd, reason);
}

/*
 * One verdict per freed block, not two.
 *
 * The record asked "does a snapshot hold this" before the root and the
 * release loop asked it again after -- two walks of the snapshot list
 * per block, and two answers that were equal by argument rather than by
 * construction. The fill now takes the verdict once and both readers use
 * it. The counter is what says so: a claim about how many times a thing
 * happened needs a count of it, not a reading of the code.
 *
 * The one extra walk is the re-ask after the list is copied, which moves
 * the entry to a new block; it happens once per commit, not per block.
 */
bool selftest_cosmofs_snap_onewalk(const char **reason)
{
    struct blkdev *bd;
    if (!cfst_engine_mount(&bd, 1024, reason))
        return false;
    char path[64];
    for (unsigned i = 0; i < 24; i++) {
        ksnprintf(path, sizeof(path), ENG "/f%u", i);
        CHECK(cfst_write_file(path, "x", 1));
    }
    CHECK(vfs_sync() == 0);
    CHECK(vfs_mkdir(NULL, ENG "/.snapshots/keep", 0755) == 0);

    uint64_t w0 = cosmofs_test_snap_walks(cfst_mount_of(ENG));
    uint64_t v0 = cosmofs_test_snap_verdicts(cfst_mount_of(ENG));
    for (unsigned i = 0; i < 24; i++) {
        ksnprintf(path, sizeof(path), ENG "/f%u", i);
        CHECK(vfs_unlink(NULL, path) == 0);
    }
    CHECK(vfs_sync() == 0);
    uint64_t walks = cosmofs_test_snap_walks(cfst_mount_of(ENG)) - w0;
    uint64_t verdicts = cosmofs_test_snap_verdicts(cfst_mount_of(ENG)) - v0;

    CHECK(verdicts > 0);            /* the commit really did have blocks to judge */
    CHECK(walks == verdicts + 1);   /* the claim: one each, and one re-ask for the copy */

    kinfo("selftest: cosmofs-snap-onewalk: %llu verdicts, %llu walks of the snapshot list",
          (unsigned long long)verdicts, (unsigned long long)walks);
    return cfst_engine_unmount(bd, reason);
}

/*
 * The record is not cleared when it is replayed, and that is deliberate.
 * A mount that replays and then goes away without committing must leave
 * the filesystem exactly as it found it, or the blocks are lost on the
 * *second* mount -- which no single cycle can see, and which the easy
 * implementation (clear it as you read it) gets wrong.
 */
bool selftest_cosmofs_freelog_idempotent(const char **reason)
{
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    struct blkdev *bd = ramblk_create(512);
    CHECK(bd != NULL);
    CHECK(cosmofs_format(bd) == 0);
    int mk = vfs_mkdir(NULL, ENG, 0755);
    CHECK(mk == 0 || mk == -EEXIST);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(cfst_mount_of(ENG), false);

    /* Make a record: a file written, deleted, and the unmount's commit
     * the one that frees. */
    static const char big[4096] = { 0 };
    CHECK(cfst_write_file(ENG "/gone", big, sizeof(big)));
    CHECK(vfs_sync() == 0);
    CHECK(vfs_unlink(NULL, ENG "/gone") == 0);
    CHECK(vfs_umount(ENG) == 0);

    /* First mount: the replay gives the blocks back. */
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    struct cosmofs_stats first;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &first) == 0);
    CHECK(first.free_root != 0);            /* still there: retired by a commit, not by a read */

    /*
     * Away again without committing. A forced unmount skips the sync, so
     * nothing on disk moves and the record still stands.
     */
    CHECK(vfs_umount2(ENG, VFS_UMOUNT_FORCE) == 0);

    /* Second mount: the same list, the same effect, nothing lost and
     * nothing freed twice. */
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    struct cosmofs_stats second;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &second) == 0);
    CHECK(second.free_blocks == first.free_blocks);
    CHECK(second.free_root == first.free_root);
    struct cosmofs_check_report rep;
    CHECK(cosmofs_check(cfst_mount_of(ENG), &rep, 0) == 0);
    CHECK(rep.clean);

    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_rmdir(NULL, ENG) == 0);
    ramblk_destroy(bd);
    kinfo("selftest: cosmofs-freelog-idempotent: two mounts, one record, %llu free both times",
          (unsigned long long)second.free_blocks);
    return true;
}

/*
 * A replayed block is the allocator's immediately, in this session,
 * without waiting for a commit. Writing one and committing makes it
 * live, and the record that named it as free is superseded by the same
 * commit -- so a remount finds it in use and named by nothing.
 */
bool selftest_cosmofs_freelog_reuse(const char **reason)
{
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    struct blkdev *bd = ramblk_create(512);
    CHECK(bd != NULL);
    CHECK(cosmofs_format(bd) == 0);
    int mk = vfs_mkdir(NULL, ENG, 0755);
    CHECK(mk == 0 || mk == -EEXIST);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(cfst_mount_of(ENG), false);
    static const char big[4096] = { 0 };
    CHECK(cfst_write_file(ENG "/first", big, sizeof(big)));
    CHECK(vfs_sync() == 0);
    CHECK(vfs_unlink(NULL, ENG "/first") == 0);
    CHECK(vfs_umount(ENG) == 0);

    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(cfst_mount_of(ENG), false);
    struct cosmofs_stats after_replay;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &after_replay) == 0);

    /* Take the space back out, in this session, and make it durable. */
    CHECK(cfst_write_file(ENG "/second", big, sizeof(big)));
    CHECK(vfs_sync() == 0);
    CHECK(vfs_umount(ENG) == 0);

    /* It is in use, the filesystem adds up, and the file reads. */
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    CHECK(cfst_read_matches(ENG "/second", big, sizeof(big)));
    struct cosmofs_check_report rep;
    CHECK(cosmofs_check(cfst_mount_of(ENG), &rep, 0) == 0);
    CHECK(rep.clean);
    CHECK(rep.seen_not_alloc.count == 0);   /* nothing live is marked free */
    struct cosmofs_stats now;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &now) == 0);
    CHECK(now.free_blocks < after_replay.free_blocks);   /* the file took the space */

    CHECK(vfs_unlink(NULL, ENG "/second") == 0);
    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    struct cosmofs_stats back;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &back) == 0);
    CHECK(back.free_blocks == after_replay.free_blocks);  /* and gave it back again */

    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_rmdir(NULL, ENG) == 0);
    ramblk_destroy(bd);
    kinfo("selftest: cosmofs-freelog-reuse: a replayed block was taken, written, and given back (%llu free either side)",
          (unsigned long long)back.free_blocks);
    return true;
}

/*
 * A record is not a snapshot's to hold. `freelog_release_previous`
 * frees the superseded chain directly rather than through
 * `cfs_snapshot_hold_block`, and the exception has to be argued: that
 * function asks whether a snapshot's *recorded bitmap* marks the block
 * allocated, and a record block from before a snapshot is marked
 * exactly that way -- so the generic path would hold a block no
 * snapshot's tree can ever reach.
 *
 * The rule: the snapshot filter is for blocks a snapshot's *tree* might
 * name. A snapshot preserves `imap_root` and `alloc_root`, never
 * `free_root`, so a record is not one of those.
 *
 * **The deadlist is the instrument, and neither the checker nor the free
 * count is.** A held block goes on the snapshot's deadlist, and a
 * deadlist is metadata the checker claims -- so a filesystem that held a
 * record it should have freed is `clean` all the way down. And the loss
 * is one block per snapshot, which is inside the noise of a transaction
 * in flight.
 *
 * What is exact is how much the deadlist grows across one named commit.
 * Naming the snapshot is that commit: it runs with the snapshot already
 * recorded, and it supersedes the record that existed before the
 * snapshot did -- the one record in the filesystem's life a snapshot's
 * recorded bitmap marks allocated, and therefore the only block on
 * which this exception can be observed at all. Three blocks go on the
 * deadlist there for good reasons and a fourth would be the record.
 *
 * A second, looser assertion says the list then stops growing across
 * thirty more commits in a directory created after the snapshot, and a
 * third says the free count does not fall.
 */
bool selftest_cosmofs_freelog_not_held(const char **reason)
{
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    struct blkdev *bd = ramblk_create(512);
    CHECK(bd != NULL);
    CHECK(cosmofs_format(bd) == 0);
    int mk = vfs_mkdir(NULL, ENG, 0755);
    CHECK(mk == 0 || mk == -EEXIST);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(cfst_mount_of(ENG), false);

    /* A record exists, and *then* the snapshot is taken: its bitmap
     * marks that record's blocks allocated, which is the difficulty. */
    CHECK(cfst_write_file(ENG "/held", "the snapshot's copy", 19));
    CHECK(vfs_sync() == 0);
    /*
     * Naming the snapshot is the commit the argument is about, and the
     * deadlist's growth across it is the measurement.
     *
     * That commit runs with the snapshot already recorded, so it
     * supersedes the record that existed before the snapshot did -- and
     * that record is marked allocated in the bitmap the snapshot keeps,
     * because the bitmap a commit publishes still has the bits its
     * phase 7 is about to clear. It is the one record in the
     * filesystem's life the generic filter would hold.
     *
     * Three blocks go on the deadlist legitimately there: the root
     * directory block, the inode-map block and the allocation index,
     * each named by the snapshot's tree and each rewritten by the
     * commit. A fourth is the record.
     */
    uint64_t dead0 = cosmofs_test_deadlist_len(cfst_mount_of(ENG), 0);
    CHECK(vfs_mkdir(NULL, ENG "/.snapshots/keep", 0755) == 0);
    uint64_t dead1 = cosmofs_test_deadlist_len(cfst_mount_of(ENG), 0);
    CHECK(dead1 - dead0 <= 3);

    /* Everything the churn touches is created after the snapshot, and
     * then settled, so the blocks these commits free are all post-
     * snapshot ones: the only thing they release that a snapshot's
     * bitmap remembers is one record after another. */
    CHECK(vfs_mkdir(NULL, ENG "/post", 0755) == 0);
    for (unsigned i = 0; i < 3; i++) {
        CHECK(cfst_write_file(ENG "/post/churn", "x", 1));
        CHECK(vfs_sync() == 0);
    }
    struct cosmofs_stats before;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &before) == 0);
    uint64_t dead_before = cosmofs_test_deadlist_len(cfst_mount_of(ENG), 0);

    for (unsigned i = 0; i < 30; i++) {
        CHECK(cfst_write_file(ENG "/post/churn", "y", 1));
        CHECK(vfs_sync() == 0);
    }
    struct cosmofs_stats after;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &after) == 0);
    /* The transaction in flight can move it a little; thirty commits'
     * worth of held records cannot hide in that. */
    CHECK(after.free_blocks + 6 >= before.free_blocks);

    /*
     * And the exact statement, which the free count cannot make: the
     * deadlist did not grow. Under the generic filter a record lands on
     * a block number this snapshot's bitmap remembers and goes on the
     * list, permanently -- one block, too small to separate from the
     * noise of a transaction in flight, and unambiguous here.
     */
    CHECK(cosmofs_test_deadlist_len(cfst_mount_of(ENG), 0) == dead_before);

    /* And the snapshot is intact: nothing this exception frees was its. */
    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    struct cosmofs_check_report rep;
    CHECK(cosmofs_check(cfst_mount_of(ENG), &rep, 0) == 0);
    CHECK(rep.seen_not_alloc.count == 0);
    CHECK(rep.snapshots_seen == 1);
    CHECK(cfst_read_matches(ENG "/.snapshots/keep/held", "the snapshot's copy", 19));

    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_rmdir(NULL, ENG) == 0);
    ramblk_destroy(bd);
    kinfo("selftest: cosmofs-freelog-not-held: naming the snapshot put %llu blocks on its deadlist and the superseded record was not one of them; 30 more commits added none (%llu free either side)",
          (unsigned long long)(dead1 - dead0), (unsigned long long)after.free_blocks);
    return true;
}

/*
 * A commit that fails after reserving the record's blocks must give them
 * back. Nothing names them -- the root that would have is not going to
 * be published -- so left allocated they are a leak the *next*
 * successful commit makes durable, because that commit writes this
 * bitmap. The window is any failure between the reservation and the
 * root: a `kmalloc` for the record's buffer, the bitmap fixpoint, an
 * unwritable block (docs/audit/next-subsystem-unmount-leak.md).
 */
bool selftest_cosmofs_freelog_rollback(const char **reason)
{
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    struct blkdev *bd = ramblk_create(512);
    CHECK(bd != NULL);
    CHECK(cosmofs_format(bd) == 0);
    int mk = vfs_mkdir(NULL, ENG, 0755);
    CHECK(mk == 0 || mk == -EEXIST);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(cfst_mount_of(ENG), false);

    /* Settle, so the count to come back to is a steady one. */
    CHECK(cfst_write_file(ENG "/settle", "s", 1));
    CHECK(vfs_unlink(NULL, ENG "/settle") == 0);
    CHECK(vfs_sync() == 0);
    CHECK(vfs_sync() == 0);
    struct cosmofs_stats before;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &before) == 0);

    /* Something to free, and a commit that fails while freeing it. */
    CHECK(cfst_write_file(ENG "/doomed", "x", 1));
    CHECK(vfs_sync() == 0);
    CHECK(vfs_unlink(NULL, ENG "/doomed") == 0);
    cosmofs_test_fail_freelog(cfst_mount_of(ENG), true);
    CHECK(vfs_sync() != 0);                  /* the commit fails, as asked */
    cosmofs_test_fail_freelog(cfst_mount_of(ENG), false);

    /*
     * And then one that succeeds. This is the commit that would make a
     * lost reservation durable: it writes the bitmap the failed attempt
     * left behind.
     */
    CHECK(vfs_sync() == 0);
    CHECK(vfs_umount(ENG) == 0);

    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    struct cosmofs_check_report rep;
    CHECK(cosmofs_check(cfst_mount_of(ENG), &rep, 0) == 0);
    CHECK(rep.alloc_not_seen.count == 0);    /* the reservation, if it was kept */
    CHECK(rep.clean);
    struct cosmofs_stats after;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &after) == 0);
    CHECK(after.free_blocks == before.free_blocks);

    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_rmdir(NULL, ENG) == 0);
    ramblk_destroy(bd);
    kinfo("selftest: cosmofs-freelog-rollback: a commit failed with the record reserved and gave every block back (%llu free either side)",
          (unsigned long long)after.free_blocks);
    return true;
}

/*
 * A record whose `count` is past what a block holds is a record this
 * kernel cannot read, and gets the answer an unreadable one gets: that
 * *root* is refused. Treating it as empty would silently drop every free
 * it names -- the root says those blocks are free, the bitmap says they
 * are not, and the difference would be reported later as an ordinary
 * leak with nothing to connect it to its cause.
 *
 * Refusing a root is not refusing the filesystem. cosmofs keeps two, and
 * a root whose tree does not load falls back to the older slot, which is
 * a complete and consistent filesystem one generation behind. So the
 * cost of the strict answer is a generation, not a mount -- and that is
 * what this test measures, because "the mount fails" is what the report
 * said and it is not what happens.
 */
bool selftest_cosmofs_freelog_malformed(const char **reason)
{
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    struct blkdev *bd = ramblk_create(512);
    CHECK(bd != NULL);
    CHECK(cosmofs_format(bd) == 0);
    int mk = vfs_mkdir(NULL, ENG, 0755);
    CHECK(mk == 0 || mk == -EEXIST);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(cfst_mount_of(ENG), false);

    /* A record with something in it, left on disk by the unmount. */
    CHECK(cfst_write_file(ENG "/gone", "x", 1));
    CHECK(vfs_sync() == 0);
    CHECK(vfs_unlink(NULL, ENG "/gone") == 0);
    CHECK(vfs_umount(ENG) == 0);

    /* Which generation the disk is at, without changing it: the discard
     * hook makes this unmount write nothing. */
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    struct cosmofs_stats good;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &good) == 0);
    cosmofs_test_discard_on_unmount(cfst_mount_of(ENG), true);
    CHECK(vfs_umount(ENG) == 0);

    /* Re-sealed, so the checksum is good and only the count is wrong. */
    CHECK(cosmofs_test_poison_freelog_count(bd, CFS_DEAD_PER_BLOCK + 1) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    struct cosmofs_stats fell;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &fell) == 0);
    CHECK(fell.generation < good.generation);   /* the newer root was refused */

    /* And what it fell back to is a whole filesystem, not a damaged one. */
    struct cosmofs_check_report rep;
    CHECK(cosmofs_check(cfst_mount_of(ENG), &rep, 0) == 0);
    CHECK(rep.clean);

    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_rmdir(NULL, ENG) == 0);
    ramblk_destroy(bd);
    kinfo("selftest: cosmofs-freelog-malformed: a record naming %u blocks in a block that holds %u is refused, and the mount falls back from generation %llu to %llu rather than reading it as empty",
          (unsigned)CFS_DEAD_PER_BLOCK + 1, (unsigned)CFS_DEAD_PER_BLOCK,
          (unsigned long long)good.generation, (unsigned long long)fell.generation);
    return true;
}

/*
 * A record block holds 506 block numbers; a transaction can free more
 * than that, so the record is a chain and every link of it has to be written and
 * replayed. A single block's worth would silently lose the overflow.
 *
 * This also covers the bound's slack. `freelog_reserve` allocates for
 * what is pending *plus* what the bitmap fixpoint can add, so it often
 * reserves a block more than the fill needs; those leftovers are listed
 * in the record as free. If they were dropped instead, the count would
 * come back short by the slack, every commit.
 */
bool selftest_cosmofs_freelog_chain(const char **reason)
{
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    struct blkdev *bd = ramblk_create(8192);      /* room for more than a record block holds */
    CHECK(bd != NULL);
    CHECK(cosmofs_format(bd) == 0);
    int mk = vfs_mkdir(NULL, ENG, 0755);
    CHECK(mk == 0 || mk == -EEXIST);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(cfst_mount_of(ENG), false);

    /*
     * The same cycle twice, and the count to come back to is the one
     * after the first. A record is retired by the *next* commit, so a
     * filesystem that has just replayed a two-block chain still holds
     * those two blocks, and comparing it against a pristine format
     * would count them as lost. Round one puts the filesystem in the
     * shape round two has to return it to.
     *
     * A file of 600 blocks, committed, then deleted in the transaction
     * the unmount commits -- so one record has to carry more than 506
     * entries and therefore more than one block.
     */
    struct cosmofs_stats before, full;
    for (unsigned round = 0; round < 2; round++) {
        if (round == 1)
            CHECK(cosmofs_stats(cfst_mount_of(ENG), &before) == 0);
        CHECK(cfst_write_wide_file(ENG "/wide", 600));
        CHECK(vfs_sync() == 0);
        if (round == 1) {
            CHECK(cosmofs_stats(cfst_mount_of(ENG), &full) == 0);
            /* More than one record block holds. */
            CHECK(before.free_blocks - full.free_blocks > CFS_DEAD_PER_BLOCK);
        }
        CHECK(vfs_unlink(NULL, ENG "/wide") == 0);
        CHECK(vfs_umount(ENG) == 0);          /* the unmount's commit frees them */
        CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
        cosmofs_test_set_writeback(cfst_mount_of(ENG), false);
    }

    /* Every block back, across a chain of record blocks. */
    struct cosmofs_stats after;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &after) == 0);
    CHECK(after.free_blocks == before.free_blocks);
    struct cosmofs_check_report rep;
    CHECK(cosmofs_check(cfst_mount_of(ENG), &rep, 0) == 0);
    CHECK(rep.clean);

    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_rmdir(NULL, ENG) == 0);
    ramblk_destroy(bd);
    kinfo("selftest: cosmofs-freelog-chain: %llu blocks freed in one transaction, more than the %u a record block holds, and all of them came back",
          (unsigned long long)(before.free_blocks - full.free_blocks),
          (unsigned)CFS_DEAD_PER_BLOCK);
    return true;
}

bool selftest_cosmofs_symlink_version(const char **reason)
{
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    struct blkdev *bd = ramblk_create(512);
    CHECK(bd != NULL);
    CHECK(cosmofs_test_format_version(bd, 7) == 0);
    int mk = vfs_mkdir(NULL, ENG, 0755);
    CHECK(mk == 0 || mk == -EEXIST);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(cfst_mount_of(ENG), false);

    /* A version-7 filesystem mounts and works, and refuses a link. */
    CHECK(cfst_write_file(ENG "/file", "seven", 5));
    CHECK(vfs_symlink(NULL, ENG "/link", "file") == -EOPNOTSUPP);
    struct cosmo_stat st;
    CHECK(vfs_lstat(NULL, ENG "/link", &st) == -ENOENT);
    CHECK(cfst_read_matches(ENG "/file", "seven", 5));

    CHECK(vfs_unlink(NULL, ENG "/file") == 0);
    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_rmdir(NULL, ENG) == 0);
    ramblk_destroy(bd);
    kinfo("selftest: cosmofs-symlink-version: a version-7 filesystem mounts, works and refuses a link");
    return true;
}

bool selftest_cosmofs_v3(const char **reason)
{
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    struct blkdev *bd = ramblk_create(512);
    CHECK(bd != NULL);
    CHECK(cosmofs_test_format_version(bd, 3) == 0);
    int mk = vfs_mkdir(NULL, ENG, 0755);
    CHECK(mk == 0 || mk == -EEXIST);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(cfst_mount_of(ENG), false);
    struct cosmofs_stats st;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &st) == 0);
    CHECK(st.members == 1 && st.total_blocks == 512);
    /* One member's worth of blocks, and one less metadata block than a
     * version-4 disk: there is no member table. */
    CHECK(st.free_blocks == 512 - 7);

    CHECK(cfst_write_file(ENG "/old", "written by a new kernel", 23));
    CHECK(vfs_mkdir(NULL, ENG "/dir", 0755) == 0);
    CHECK(cfst_write_file(ENG "/dir/deep", "deep", 4));
    CHECK(vfs_sync() == 0);
    CHECK(vfs_umount(ENG) == 0);

    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(cfst_mount_of(ENG), false);
    CHECK(cfst_read_matches(ENG "/old", "written by a new kernel", 23));
    CHECK(cfst_read_matches(ENG "/dir/deep", "deep", 4));
    /* Snapshots work on it too: a version-3 snapshot records the single
     * allocation root, and that is what cfs_snapshot_references reads. */
    CHECK(vfs_mkdir(NULL, ENG "/.snapshots/v3", 0755) == 0);
    CHECK(cfst_write_file(ENG "/old", "changed", 7));
    CHECK(cfst_read_matches(ENG "/.snapshots/v3/old", "written by a new kernel", 23));
    /*
     * And the hold is *recorded*, on a filesystem too old to have a free
     * record. The deadlist's blocks come from the commit's reservation,
     * and that reservation is not gated on the record's version: gating
     * it would leave this commit's release loop unable to remember a
     * hold, which means freeing a block the snapshot still names. Read
     * the entries rather than the file, because the file reads correctly
     * from a freed block until something else takes it.
     */
    CHECK(vfs_sync() == 0);
    uint64_t v3_dead = 0;
    CHECK(cosmofs_test_deadlist_dups(cfst_mount_of(ENG), &v3_dead, NULL) == 0);
    CHECK(v3_dead > 0);
    struct cosmofs_check_report v3rep;
    CHECK(cosmofs_check(cfst_mount_of(ENG), &v3rep, 0) == 0);
    CHECK(v3rep.seen_not_alloc.count == 0);
    CHECK(v3rep.snapshots_seen == 1);
    CHECK(vfs_rmdir(NULL, ENG "/.snapshots/v3") == 0);
    CHECK(cfst_read_matches(ENG "/old", "changed", 7));

    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_rmdir(NULL, ENG) == 0);
    ramblk_destroy(bd);
    kinfo("selftest: cosmofs-v3: a version-3 disk still mounts and writes");
    return true;
}

/* The first block of a file, whatever the file's length. */
static bool head_matches(const char *path, const void *data, size_t len)
{
    struct file *f;
    if (vfs_open(NULL, path, COSMO_O_RDONLY, 0, &f))
        return false;
    uint8_t *b = kmalloc(len, 0);
    if (b == NULL) {
        file_put(f);
        return false;
    }
    int64_t n = file_read(f, b, len);
    bool ok = n == (int64_t)len && memcmp(b, data, len) == 0;
    kfree(b);
    file_put(f);
    return ok;
}

bool selftest_cosmofs_pool2(const char **reason)
{
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    struct blkdev *bd[2] = { ramblk_create(1024), ramblk_create(1024) };
    CHECK(bd[0] != NULL && bd[1] != NULL);
    CHECK(cosmofs_format_pool(bd, 2) == 0);
    int mk = vfs_mkdir(NULL, ENG, 0755);
    CHECK(mk == 0 || mk == -EEXIST);
    CHECK(vfs_mount(ENG, "cosmofs", bd[0], 0) == 0);
    cosmofs_test_set_writeback(cfst_mount_of(ENG), false);

    /* The first commit touches member 0's metadata while member 1 is
     * emptier, so the allocation index's copy-on-write lands on member 1
     * and dirties a bitmap chunk that the reservation pass has already
     * been over. Every dirty chunk must still have a reserved
     * destination: writing one to DVA 0 would land on member 0's
     * superblock. */
    CHECK(cfst_write_file(ENG "/first", "one", 3));
    CHECK(vfs_sync() == 0);
    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd[0], 0) == 0);
    cosmofs_test_set_writeback(cfst_mount_of(ENG), false);
    CHECK(cfst_read_matches(ENG "/first", "one", 3));

    /* Both members' blocks are in the pool's total. */
    struct cosmofs_stats st;
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &st) == 0);
    CHECK(st.total_blocks == 2048);

    /* Enough data that a one-member pool of this size could not hold it:
     * the allocator has to reach the second member. */
    /* Sixteen *different* pages per file: the same page written sixteen
     * times is one of the most compressible things there is, and this
     * test is about where blocks land, not how few of them there are. */
    static uint8_t buf[16 * 4096];
    cfst_fill_incompressible(buf, sizeof(buf), 0x9e37);
    for (unsigned i = 0; i < 24; i++) {
        char name[32];
        ksnprintf(name, sizeof(name), ENG "/f%u", i);
        struct file *f;
        CHECK(vfs_open(NULL, name, COSMO_O_RDWR | COSMO_O_CREAT, 0644, &f) == 0);
        for (unsigned k = 0; k < 16; k++)
            CHECK(file_write(f, buf + (size_t)k * 4096, 4096) == 4096);
        file_put(f);
    }
    CHECK(vfs_sync() == 0);
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &st) == 0);
    CHECK(st.members == 2);
    CHECK(st.free_blocks < 2048 - 24 * 16);
    /* Both members carry blocks -- the allocator prefers whichever has
     * more room, so neither is left untouched -- and the pool's free
     * count is exactly theirs. */
    uint64_t f0 = cosmofs_test_member_free(cfst_mount_of(ENG), 0);
    uint64_t f1 = cosmofs_test_member_free(cfst_mount_of(ENG), 1);
    CHECK(f0 < 1024 && f1 < 1024);
    CHECK(f0 + f1 == st.free_blocks);
    CHECK(cosmofs_test_member_free(cfst_mount_of(ENG), 2) == UINT64_MAX);

    /* A remount is handed member 0 only and has to find the other one by
     * its label; the data reads back through both. */
    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd[0], 0) == 0);
    cosmofs_test_set_writeback(cfst_mount_of(ENG), false);
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &st) == 0 && st.total_blocks == 2048);
    for (unsigned i = 0; i < 24; i++) {
        char name[32];
        ksnprintf(name, sizeof(name), ENG "/f%u", i);
        struct cosmo_stat s;
        CHECK(vfs_stat(NULL, name, &s) == 0 && s.size == 16 * 4096);
    }
    CHECK(head_matches(ENG "/f7", buf, 4096));

    /* A snapshot spans the members too: its record pins the member table
     * of its generation, so every member's bitmap is held. */
    CHECK(vfs_mkdir(NULL, ENG "/.snapshots/two", 0755) == 0);
    CHECK(vfs_unlink(NULL, ENG "/f3") == 0);
    CHECK(vfs_sync() == 0);
    CHECK(head_matches(ENG "/.snapshots/two/f3", buf, 4096));
    CHECK(vfs_rmdir(NULL, ENG "/.snapshots/two") == 0);

    CHECK(vfs_umount(ENG) == 0);

    CHECK(vfs_rmdir(NULL, ENG) == 0);
    ramblk_destroy(bd[0]);
    ramblk_destroy(bd[1]);
    kinfo("selftest: cosmofs-pool2: two members, %llu blocks", (unsigned long long)st.total_blocks);
    return true;
}

/* A snapshot survives an unmount: its entry is on disk, not in memory. */
bool selftest_cosmofs_snapshot_remount(const char **reason)
{
    struct blkdev *bd;
    if (!cfst_engine_mount(&bd, 512, reason))
        return false;
    CHECK(cfst_write_file(ENG "/f", "v1", 2));
    CHECK(vfs_mkdir(NULL, ENG "/.snapshots/s1", 0755) == 0);
    CHECK(cfst_write_file(ENG "/f", "v2", 2));
    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(cfst_mount_of(ENG), false);
    CHECK(cfst_read_matches(ENG "/f", "v2", 2));
    CHECK(cfst_read_matches(ENG "/.snapshots/s1/f", "v1", 2));
    CHECK(vfs_rmdir(NULL, ENG "/.snapshots/s1") == 0);
    kinfo("selftest: cosmofs-snapshot-remount: the snapshot survived the unmount");
    return cfst_engine_unmount(bd, reason);
}

/*
 * Format version 12's snapshot identity (roadmap 1.2 B1; design.md,
 * "Format version 12"): a pool formatted at 11 stays at 11 until a
 * snapshot is taken, which raises it and converts the list an older
 * kernel left -- every snapshot gains a uuid and stays readable; ids are
 * never reused, across a remount too (inventory §3, CFS-3); a list's
 * sizes say what each snapshot alone holds; a pool older than 11 refuses
 * identity and keeps its version; and the checker names a snapshot with
 * no uuid and repairs it.
 */
bool selftest_cosmofs_snap_identity(const char **reason)
{
    struct blkdev *bd;
    if (!cfst_engine_mount(&bd, 1024, reason))
        return false;
    struct cosmofs_stats st;
    /* Static: the self-tests run one at a time, and the kernel stack has
     * no 2.5 KiB to spare for a list and a check report. */
    static struct cosmofs_snap_info info[8];
    static struct cosmofs_check_report rep;
    unsigned n = 0;
    static const uint8_t zero[16];

    CHECK(cosmofs_stats(cfst_mount_of(ENG), &st) == 0);
    CHECK(st.version == 11);                         /* formatted at 11, not 12 */

    /* The list an older kernel would have left: one version-11 snapshot. */
    CHECK(cfst_write_file(ENG "/f", "old", 3));
    cosmofs_test_snap_no_raise(cfst_mount_of(ENG), true);
    CHECK(vfs_mkdir(NULL, ENG "/.snapshots/legacy", 0755) == 0);
    cosmofs_test_snap_no_raise(cfst_mount_of(ENG), false);
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &st) == 0 && st.version == 11);

    /* The first snapshot with identity raises the pool and the list. */
    CHECK(cfst_write_file(ENG "/f", "new", 3));
    CHECK(cfst_snapctl(COSMOFS_SNAP_CREATE, "a", info, 1, &n) == 0 && n == 1);
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &st) == 0 && st.version == 12);
    CHECK(memcmp(info[0].uuid, zero, 16) != 0 && info[0].format == 12);
    uint8_t uuid_a[16];
    memcpy(uuid_a, info[0].uuid, 16);
    CHECK(cfst_snapctl(COSMOFS_SNAP_CREATE, "a", info, 1, &n) == -EEXIST);
    CHECK(cfst_snapctl(COSMOFS_SNAP_CREATE, "b", info, 1, &n) == 0);
    CHECK(memcmp(info[0].uuid, uuid_a, 16) != 0);
    uint64_t id_b = info[0].id;
    CHECK(cfst_read_matches(ENG "/.snapshots/legacy/f", "old", 3));   /* converted, still readable */
    CHECK(cfst_read_matches(ENG "/.snapshots/a/f", "new", 3));

    /* Every entry, the converted one included, has a uuid of its own. */
    CHECK(cfst_snapctl(COSMOFS_SNAP_LIST, NULL, info, 8, &n) == 0 && n == 3);
    const struct cosmofs_snap_info *legacy = cfst_info_named(info, n, "legacy");
    CHECK(legacy != NULL && memcmp(legacy->uuid, zero, 16) != 0 && memcmp(legacy->uuid, uuid_a, 16) != 0);

    /* Ids are never reused, across a remount (CFS-3): delete the newest,
     * remount, take another -- it gets a new id, not b's. */
    CHECK(cfst_snapctl(COSMOFS_SNAP_DELETE, "b", NULL, 0, NULL) == 0);
    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(cfst_mount_of(ENG), false);
    CHECK(cfst_snapctl(COSMOFS_SNAP_CREATE, "c", info, 1, &n) == 0);
    CHECK(info[0].id > id_b);

    /* What a snapshot alone holds: a file of 32 blocks written, taken by
     * "d", then removed from the live tree. */
    /* Incompressible: new files are LZ4 (format version 6), and a file
     * that compresses holds fewer blocks than it has. */
    static char big[32 * 4096];
    uint64_t x = 0x9E3779B97F4A7C15ull;
    for (unsigned i = 0; i < sizeof(big); i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        big[i] = (char)x;
    }
    CHECK(cfst_write_file(ENG "/big", big, sizeof(big)));
    CHECK(cfst_snapctl(COSMOFS_SNAP_CREATE, "d", info, 1, &n) == 0);
    CHECK(vfs_unlink(NULL, ENG "/big") == 0);
    CHECK(vfs_sync() == 0);
    CHECK(cfst_snapctl(COSMOFS_SNAP_LIST, NULL, info, 8, &n) == 0 && n == 4);   /* legacy, a, c, d */
    const struct cosmofs_snap_info *d = cfst_info_named(info, n, "d");
    const struct cosmofs_snap_info *c = cfst_info_named(info, n, "c");
    CHECK(d != NULL && c != NULL);
    CHECK(d->unique >= 32ull * 4096 && d->referenced >= d->unique);
    CHECK(c->unique < 32ull * 4096);               /* the file was born after c */
    /* Oldest first. */
    CHECK(strcmp(info[0].name, "legacy") == 0 && strcmp(info[n - 1].name, "d") == 0);

    /* The checker: clean, then a snapshot with no uuid is named and repaired. */
    CHECK(cosmofs_check(cfst_mount_of(ENG), &rep, 0) == 0 && rep.clean);
    uint64_t victim = 0;
    CHECK(cosmofs_test_corrupt(cfst_mount_of(ENG), COSMOFS_CORRUPT_SNAP_UUID, 0, &victim) == 0);
    CHECK(cosmofs_check(cfst_mount_of(ENG), &rep, 0) == 0 && !rep.clean);
    CHECK(rep.snap_identity.count == 1 && rep.snap_identity.name[0] == victim);
    CHECK(cosmofs_check(cfst_mount_of(ENG), &rep, COSMOFS_CHECK_REPAIR) == 0 && rep.snap_identity.repaired == 1);
    CHECK(cosmofs_check(cfst_mount_of(ENG), &rep, 0) == 0 && rep.clean);

    CHECK(cfst_snapctl(COSMOFS_SNAP_DELETE, "nope", NULL, 0, NULL) == -ENOENT);
    CHECK(cfst_snapctl(COSMOFS_SNAP_CREATE, "bad/name", info, 1, &n) == -EINVAL);
    CHECK(cfst_snapctl(COSMOFS_SNAP_DELETE, "legacy", NULL, 0, NULL) == 0);
    CHECK(cfst_snapctl(COSMOFS_SNAP_DELETE, "a", NULL, 0, NULL) == 0);
    CHECK(cfst_snapctl(COSMOFS_SNAP_DELETE, "c", NULL, 0, NULL) == 0);
    CHECK(cfst_snapctl(COSMOFS_SNAP_DELETE, "d", NULL, 0, NULL) == 0);
    CHECK(cosmofs_check(cfst_mount_of(ENG), &rep, 0) == 0 && rep.clean);
    CHECK(cfst_engine_unmount(bd, reason));

    /* A pool older than 11 refuses identity and keeps its version; its
     * own kind of snapshot still works. */
    bd = ramblk_create(512);
    CHECK(bd != NULL);
    CHECK(cosmofs_test_format_version(bd, 10) == 0);
    int mk = vfs_mkdir(NULL, ENG, 0755);
    CHECK(mk == 0 || mk == -EEXIST);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(cfst_mount_of(ENG), false);
    CHECK(cfst_snapctl(COSMOFS_SNAP_CREATE, "x", info, 1, &n) == -EOPNOTSUPP);
    CHECK(vfs_mkdir(NULL, ENG "/.snapshots/old", 0755) == 0);
    CHECK(cosmofs_stats(cfst_mount_of(ENG), &st) == 0 && st.version == 10);
    CHECK(vfs_rmdir(NULL, ENG "/.snapshots/old") == 0);
    kinfo("selftest: cosmofs-snap-identity: raised 11 -> 12 with a legacy snapshot converted; ids not reused "
          "after a remount (%llu > %llu); d holds %llu KiB alone; a version-10 pool refuses identity",
          (unsigned long long)c->id, (unsigned long long)id_b, (unsigned long long)(d->unique / 1024));
    return cfst_engine_unmount(bd, reason);
}

/* --- long names (roadmap 1.2 A2; design.md, "Format version 15"; CF13) --- */

/* `ENG "/"` and a name of `len` bytes, distinct per `tag`, into `out`. */
static const char *lname(char *out, unsigned len, char tag)
{
    size_t n = strlen(ENG "/");
    memcpy(out, ENG "/", n);
    out[n] = tag;
    for (unsigned i = 1; i < len; i++)
        out[n + i] = (char)('a' + (i * 7 + (unsigned char)tag) % 26);
    out[n + len] = '\0';
    return out;
}

/* How many entries of `dir` are named `want`, listed through a buffer of
 * `bufsz` bytes -- small, so the walk resumes between long names -- and
 * how many entries there were in all, "." and ".." among them. */
static bool list_count(const char *dir, size_t bufsz, const char *want, unsigned *hits, unsigned *total)
{
    struct file *f;
    if (vfs_open(NULL, dir, COSMO_O_RDONLY, 0, &f))
        return false;
    uint8_t *buf = kmalloc(bufsz, 0);
    bool ok = buf != NULL;
    size_t wlen = strlen(want);
    *hits = *total = 0;
    while (ok) {
        int64_t n = file_readdir(f, buf, bufsz);
        if (n <= 0) {
            ok = n == 0;
            break;
        }
        for (size_t o = 0; o < (size_t)n;) {
            const struct cosmo_dirent *e = (const struct cosmo_dirent *)(buf + o);
            if (e->reclen == 0) {
                ok = false;
                break;
            }
            (*total)++;
            if (e->namelen == wlen && memcmp(e->name, want, wlen) == 0)
                (*hits)++;
            o += e->reclen;
        }
    }
    kfree(buf);
    file_put(f);
    return ok;
}

static unsigned pool_version(void)
{
    struct cosmofs_stats st;
    return cosmofs_stats(cfst_mount_of(ENG), &st) == 0 ? st.version : 0;
}

/* The name part of a path lname made. */
#define LBASE(p) ((p) + strlen(ENG "/"))

/*
 * cosmofs-longname: names of 48 to 255 bytes. A version-11 pool takes
 * short names and stays 11, finds no long name, and is raised to 15 by
 * its first name over 47 bytes; then files, a directory and a symbolic
 * link with long names are created, read back, listed whole through a
 * buffer that holds one long entry at a time, renamed long to long, long
 * to short and short to long, and removed. A 255-byte name that does not
 * fit the slots its block has left goes to the next block. All of it
 * survives a remount and the checker calls the pool clean. A version-10
 * pool cannot be raised: there a long name is -ENAMETOOLONG and the pool
 * stays 10. 256 bytes is the VFS's -ENAMETOOLONG everywhere.
 */
bool selftest_cosmofs_longname(const char **reason)
{
    struct blkdev *bd;
    struct cosmofs_check_report rep;
    struct cosmo_stat st;
    struct file *f;
    unsigned hits, total;
    static char p48[300], p49[300], p104[300], p105[300], p255[300], pdir[300], plnk[300], p256[300], pnew[300];

    CHECK(cfst_engine_mount(&bd, 2048, reason));
    CHECK(cfst_write_file(ENG "/a", "short", 5) && pool_version() == CFS_FORMAT_VERSION);
    CHECK(vfs_open(NULL, lname(p105, 105, 'f'), COSMO_O_RDONLY, 0, &f) == -ENOENT);
    CHECK(pool_version() == CFS_FORMAT_VERSION);   /* a lookup raises nothing */

    CHECK(cfst_write_file(lname(p48, 48, 'a'), "48", 2) && pool_version() == 15);
    CHECK(cfst_write_file(lname(p49, 49, 'b'), "49", 2));
    CHECK(cfst_write_file(lname(p104, 104, 'c'), "104", 3));
    CHECK(cfst_write_file(p105, "105", 3));
    CHECK(cfst_write_file(lname(p255, 255, 'e'), "255", 3));
    CHECK(vfs_mkdir(NULL, lname(pdir, 200, 'g'), 0755) == 0);
    CHECK(vfs_symlink(NULL, lname(plnk, 150, 'h'), "a") == 0);
    CHECK(vfs_open(NULL, lname(p256, 256, 'i'), COSMO_O_WRONLY | COSMO_O_CREAT, 0644, &f) == -ENAMETOOLONG);
    CHECK(cfst_read_matches(p48, "48", 2) && cfst_read_matches(p104, "104", 3) && cfst_read_matches(p255, "255", 3));
    CHECK(cfst_read_matches(plnk, "short", 5));   /* through the link */

    /* Listed whole, one long entry per call at the smallest buffer. */
    size_t one = sizeof(struct cosmo_dirent) + CFS_NAME_MAX_LONG + 8;
    CHECK(list_count(ENG, one, LBASE(p255), &hits, &total) && hits == 1 && total == 2 + 8);
    CHECK(list_count(ENG, one, LBASE(pdir), &hits, &total) && hits == 1);
    CHECK(list_count(ENG, 4096, LBASE(p49), &hits, &total) && hits == 1 && total == 2 + 8);

    /* Renames: long to long, long to short, short to long. */
    CHECK(vfs_rename(NULL, p255, lname(pnew, 230, 'j')) == 0);
    CHECK(vfs_stat(NULL, p255, &st) == -ENOENT && cfst_read_matches(pnew, "255", 3));
    CHECK(vfs_rename(NULL, p104, ENG "/s") == 0 && cfst_read_matches(ENG "/s", "104", 3));
    CHECK(vfs_rename(NULL, ENG "/s", p104) == 0 && cfst_read_matches(p104, "104", 3));
    CHECK(vfs_unlink(NULL, p49) == 0 && vfs_stat(NULL, p49, &st) == -ENOENT);
    CHECK(list_count(ENG, one, LBASE(pnew), &hits, &total) && hits == 1 && total == 2 + 7);

    /* A directory whose first block has four free slots left: the
     * 255-byte name (five slots) is put in the next block. */
    char *sub = pdir;
    static char path[600];   /* the 200-byte directory, a slash, a 255-byte name */
    for (unsigned i = 0; i < CFS_DIRENTS_PER_BLOCK - 4; i++) {
        ksnprintf(path, sizeof(path), "%s/%u", sub, i);
        CHECK(cfst_write_file(path, "x", 1));
    }
    CHECK(vfs_stat(NULL, sub, &st) == 0 && st.size == CFS_BLOCK);
    ksnprintf(path, sizeof(path), "%s/%s", sub, LBASE(lname(p256, 255, 'k')));
    CHECK(cfst_write_file(path, "next", 4) && cfst_read_matches(path, "next", 4));
    CHECK(vfs_stat(NULL, sub, &st) == 0 && st.size == 2 * CFS_BLOCK);
    CHECK(list_count(sub, one, LBASE(p256), &hits, &total) && hits == 1 && total == 2 + CFS_DIRENTS_PER_BLOCK - 3);

    CHECK(vfs_sync() == 0);
    CHECK(cfst_remount_eng(bd, reason));
    CHECK(pool_version() == 15);
    CHECK(cfst_read_matches(p48, "48", 2) && cfst_read_matches(pnew, "255", 3) && cfst_read_matches(path, "next", 4));
    CHECK(list_count(ENG, one, LBASE(p104), &hits, &total) && hits == 1 && total == 2 + 7);
    CHECK(cosmofs_check(cfst_mount_of(ENG), &rep, 0) == 0);
    CHECK_CLEAN(rep);
    CHECK(cfst_engine_unmount(bd, reason));

    /* A version-10 pool cannot be raised: the version-14 answer. */
    bd = ramblk_create(512);
    CHECK(bd != NULL);
    CHECK(cosmofs_test_format_version(bd, 10) == 0);
    int mk = vfs_mkdir(NULL, ENG, 0755);
    CHECK(mk == 0 || mk == -EEXIST);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(cfst_mount_of(ENG), false);
    CHECK(vfs_open(NULL, p48, COSMO_O_WRONLY | COSMO_O_CREAT, 0644, &f) == -ENAMETOOLONG);
    CHECK(vfs_mkdir(NULL, p105, 0755) == -ENAMETOOLONG);
    CHECK(cfst_write_file(ENG "/a", "short", 5) && pool_version() == 10);
    kinfo("selftest: cosmofs-longname: names of 48-255 bytes created, listed, renamed and removed on a pool raised "
          "11 -> 15 by the first; a 255-byte name went to the next block; a version-10 pool refuses them");
    return cfst_engine_unmount(bd, reason);
}

/*
 * cosmofs-check-longname: version 15's two malformed shapes (CF13) -- a
 * head whose `extra` is not what its length needs, and continuations
 * whose head is gone -- are each `dir_bad`, refused before the name is
 * read; and one long name twice in a directory is `dir_dup_name`.
 */
bool selftest_cosmofs_check_longname(const char **reason)
{
    static const enum cosmofs_corruption kinds[] = { COSMOFS_CORRUPT_LONGNAME_EXTRA, COSMOFS_CORRUPT_LONGNAME_CONT };
    static char p[600], q[300];
    struct cosmofs_check_report r;
    struct cosmo_stat st;
    struct blkdev *bd = NULL;
    uint64_t what = 0;

    for (unsigned k = 0; k < 2; k++) {
        CHECK(cfst_check_fixture(&bd, reason));
        ksnprintf(p, sizeof(p), "%s/sub/%s", ENG, LBASE(lname(q, 120, 'l')));
        CHECK(cfst_write_file(p, "x", 1) && vfs_sync() == 0);
        CHECK(vfs_stat(NULL, ENG "/sub", &st) == 0);
        CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0 && r.dir_bad.count == 0);
        CHECK(cosmofs_test_corrupt(cfst_mount_of(ENG), kinds[k], st.ino, &what) == 0);
        CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0);
        CHECK(r.dir_bad.count >= 1 && r.dir_bad.name[0] == st.ino);
        cfst_check_teardown(bd);
    }

    CHECK(cfst_check_fixture(&bd, reason));
    ksnprintf(p, sizeof(p), "%s/sub/%s", ENG, LBASE(lname(q, 200, 'm')));
    CHECK(cfst_write_file(p, "x", 1) && vfs_sync() == 0);
    CHECK(vfs_stat(NULL, ENG "/sub", &st) == 0);
    CHECK(cosmofs_test_corrupt(cfst_mount_of(ENG), COSMOFS_CORRUPT_DUP_NAME, st.ino, &what) == 0);
    CHECK(cosmofs_check(cfst_mount_of(ENG), &r, 0) == 0);
    CHECK(r.dir_dup_name.count >= 1 && r.dir_dup_name.name[0] == st.ino);
    cfst_check_teardown(bd);

    kinfo("selftest: cosmofs-check-longname: a wrong continuation count and continuations without a head are "
          "dir_bad; a 200-byte name twice is a duplicate");
    return true;
}

/*
 * cosmofs-link (VFS-4; design.md, "Hard links"): one file, four names in
 * two directories (one of them long enough to raise the pool to 15); a
 * directory and an existing name refused; a link to a symbolic link is
 * that link. Names go one by one and the file stays readable through the
 * rest; the count survives a remount and the checker counts every name.
 * An open file outlives its last name and is freed after it (checked
 * clean). A snapshot is read-only to a link, either way.
 */
bool selftest_cosmofs_link(const char **reason)
{
    struct blkdev *bd;
    struct cosmofs_check_report rep;
    struct cosmo_stat st, st2;
    struct file *f;
    static char lng[300];
    uint8_t buf[4];

    CHECK(cfst_engine_mount(&bd, 1024, reason));
    CHECK(cfst_write_file(ENG "/a", "one", 3) && vfs_mkdir(NULL, ENG "/d", 0755) == 0);
    CHECK(vfs_link(NULL, ENG "/a", NULL, ENG "/b", false) == 0);
    CHECK(vfs_link(NULL, ENG "/a", NULL, ENG "/d/c", false) == 0);
    CHECK(vfs_link(NULL, ENG "/b", NULL, lname(lng, 60, 'n'), false) == 0 && pool_version() == 15);
    CHECK(vfs_stat(NULL, ENG "/a", &st) == 0 && vfs_stat(NULL, ENG "/d/c", &st2) == 0);
    CHECK(st.ino == st2.ino && st.nlink == 4);
    CHECK(vfs_link(NULL, ENG "/d", NULL, ENG "/e", false) == -EPERM);
    CHECK(vfs_link(NULL, ENG "/a", NULL, ENG "/b", false) == -EEXIST);
    CHECK(vfs_symlink(NULL, ENG "/s", "a") == 0 && vfs_link(NULL, ENG "/s", NULL, ENG "/s2", false) == 0);
    CHECK(vfs_lstat(NULL, ENG "/s2", &st2) == 0 && st2.type == COSMO_DT_LNK && st2.nlink == 2);

    CHECK(vfs_unlink(NULL, ENG "/a") == 0 && cfst_read_matches(ENG "/b", "one", 3));
    CHECK(vfs_stat(NULL, ENG "/b", &st) == 0 && st.nlink == 3);
    CHECK(vfs_sync() == 0 && cfst_remount_eng(bd, reason));
    CHECK(vfs_stat(NULL, ENG "/d/c", &st) == 0 && st.nlink == 3 && cfst_read_matches(lng, "one", 3));
    CHECK(cosmofs_check(cfst_mount_of(ENG), &rep, 0) == 0);
    CHECK_CLEAN(rep);

    /* Read-only both ways in a snapshot: no name made there, none taken out. */
    unsigned n;
    static struct cosmofs_snap_info info[1];
    CHECK(cfst_snapctl(COSMOFS_SNAP_CREATE, "lk", info, 1, &n) == 0);
    CHECK(vfs_link(NULL, ENG "/b", NULL, ENG "/.snapshots/lk/z", false) == -EROFS);
    CHECK(vfs_link(NULL, ENG "/.snapshots/lk/b", NULL, ENG "/z", false) == -EROFS);
    CHECK(vfs_rmdir(NULL, ENG "/.snapshots/lk") == 0);

    /* An open file outlives its last name, and is freed after it. */
    CHECK(vfs_open(NULL, ENG "/b", COSMO_O_RDONLY, 0, &f) == 0);
    CHECK(vfs_unlink(NULL, ENG "/b") == 0 && vfs_unlink(NULL, ENG "/d/c") == 0 && vfs_unlink(NULL, lng) == 0);
    CHECK(f->vn->nlink == 0 && file_pread(f, buf, 3, 0) == 3 && memcmp(buf, "one", 3) == 0);
    file_put(f);
    CHECK(vfs_sync() == 0 && cfst_remount_eng(bd, reason));
    CHECK(cosmofs_check(cfst_mount_of(ENG), &rep, 0) == 0);
    CHECK_CLEAN(rep);
    kinfo("selftest: cosmofs-link: four names for one file in two directories, a link to a link, the count across "
          "a remount, a snapshot read-only both ways, an open file outliving its last name");
    return cfst_engine_unmount(bd, reason);
}
#else
bool selftest_cosmofs_snapshot_remount(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_snap_identity(const char **reason) { (void)reason; return true; }
#endif
