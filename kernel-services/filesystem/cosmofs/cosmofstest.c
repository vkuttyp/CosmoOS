/*
 * cosmofstest.c - Self-tests for the storage pool and cosmofs on the
 * scratch virtio disk (vda). They format the disk, so they run only
 * when it exists, and they leave a filesystem behind for init's
 * user-mode test: /hello.txt and /dir/nested.txt.
 */

#include <kernel/blk.h>
#include <kernel/cosmofs.h>
#include <kernel/crc32c.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/page.h>
#include <kernel/printf.h>
#include <kernel/selftest.h>
#include <kernel/storage.h>
#include <kernel/string.h>
#include <kernel/vfs.h>

#define STR_(x) #x
#define STR(x)  STR_(x)
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            *reason = "check failed: " #cond " at line " STR(__LINE__);        \
            return false;                                                      \
        }                                                                      \
    } while (0)

static struct blkdev *scratch(void)
{
    return blk_find("vda");
}

bool selftest_pool(const char **reason)
{
    struct blkdev *bd = scratch();
    if (bd == NULL) {
        kinfo("selftest: no vda; skipping");
        return true;
    }
    struct spool *p;
    CHECK(pool_open(bd, &p) == 0);
    CHECK(p->block_size == 4096 && p->sectors_per_block == 8 && p->nblocks == bd->capacity / 8);
    uint8_t *w = kmalloc(4096, 0), *r = kmalloc(4096, 0);
    if (w == NULL || r == NULL) {
        kfree(w);
        kfree(r);
        pool_close(p);
        blkdev_put(bd);
        *reason = "kmalloc failed";
        return false;
    }
    for (unsigned i = 0; i < 4096; i++)
        w[i] = (uint8_t)(i ^ 0x3c);
    bool ok = pool_write(p, p->nblocks - 1, w) == 0 && pool_flush(p) == 0 && pool_read(p, p->nblocks - 1, r) == 0 &&
              memcmp(w, r, 4096) == 0 && pool_read(p, p->nblocks, r) == -EINVAL && pool_write(p, p->nblocks, w) == -EINVAL;
    kfree(w);
    kfree(r);
    pool_close(p);
    blkdev_put(bd);
    CHECK(ok);
    return true;
}

static bool write_file(const char *path, const void *data, size_t len)
{
    struct file *f;
    if (vfs_open(NULL, path, COSMO_O_WRONLY | COSMO_O_CREAT | COSMO_O_TRUNC, 0644, &f))
        return false;
    int64_t n = file_write(f, data, len);
    file_put(f);
    return n == (int64_t)len;
}

static bool read_matches(const char *path, const void *data, size_t len)
{
    struct file *f;
    if (vfs_open(NULL, path, COSMO_O_RDONLY, 0, &f))
        return false;
    uint8_t *buf = kmalloc(len + 1, 0);
    if (buf == NULL) {
        file_put(f);
        return false;
    }
    int64_t n = file_read(f, buf, len + 1);
    bool ok = n == (int64_t)len && memcmp(buf, data, len) == 0;
    kfree(buf);
    file_put(f);
    return ok;
}

static struct mount *mount_of(const char *path)
{
    struct vnode *vn;
    if (vfs_lookup(NULL, path, &vn))
        return NULL;
    struct mount *m = vn->mnt;
    vnode_put(vn);
    return m;
}

bool selftest_cosmofs_format(const char **reason)
{
    struct blkdev *bd = scratch();
    if (bd == NULL) {
        kinfo("selftest: no vda; skipping");
        return true;
    }
    /* An unformatted (zeroed) disk is refused. */
    CHECK(vfs_mount("/mnt", "cosmofs", bd, 0) == -EIO);
    CHECK(cosmofs_format(bd) == 0);
    CHECK(vfs_mount("/mnt", "cosmofs", bd, 0) == 0);
    struct cosmofs_stats st;
    CHECK(cosmofs_stats(mount_of("/mnt"), &st) == 0);
    CHECK(st.generation == 1 && st.total_blocks == bd->capacity / 8 && st.inode_count == 1);
    CHECK(st.free_blocks == st.total_blocks - 7);   /* 2 supers, index, bitmap, L1, L0, inodes */
    struct cosmo_stat s;
    CHECK(vfs_stat(NULL, "/mnt", &s) == 0 && s.type == COSMO_DT_DIR && s.ino == 1 && s.nlink == 2);
    CHECK(vfs_stat(NULL, "/mnt/anything", &s) == -ENOENT);
    CHECK(vfs_umount("/mnt") == 0);
    /* Unmount committed nothing new: still generation 1. */
    CHECK(vfs_mount("/mnt", "cosmofs", bd, 0) == 0);
    CHECK(cosmofs_stats(mount_of("/mnt"), &st) == 0 && st.generation == 1);
    CHECK(vfs_umount("/mnt") == 0);
    blkdev_put(bd);
    return true;
}

bool selftest_cosmofs_ops(const char **reason)
{
    struct blkdev *bd = scratch();
    if (bd == NULL) {
        kinfo("selftest: no vda; skipping");
        return true;
    }
    CHECK(vfs_mount("/mnt", "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(mount_of("/mnt"), false);   /* the generation arithmetic below is exact */
    struct cosmofs_stats st0, st;
    CHECK(cosmofs_stats(mount_of("/mnt"), &st0) == 0);

    /* Files and directories. */
    CHECK(write_file("/mnt/hello.txt", "hello from the kernel", 21));
    CHECK(vfs_mkdir(NULL, "/mnt/dir", 0755) == 0);
    CHECK(write_file("/mnt/dir/nested.txt", "nested", 6));
    CHECK(read_matches("/mnt/hello.txt", "hello from the kernel", 21));
    struct cosmo_stat s;
    CHECK(vfs_stat(NULL, "/mnt/dir", &s) == 0 && s.nlink == 2);
    CHECK(vfs_stat(NULL, "/mnt", &s) == 0 && s.nlink == 3);
    CHECK(vfs_stat(NULL, "/mnt/dir/..", &s) == 0 && s.ino == 1);

    /* A file spanning many blocks with a rewrite in the middle: the
     * extents split, merge, and read back exactly. */
    size_t big = 45 * 4096 + 123;
    uint8_t *buf = kmalloc(big, 0);
    CHECK(buf != NULL);
    for (size_t i = 0; i < big; i++)
        buf[i] = (uint8_t)(i * 31 + 7);
    CHECK(write_file("/mnt/big.bin", buf, big));
    struct file *f;
    CHECK(vfs_open(NULL, "/mnt/big.bin", COSMO_O_RDWR, 0, &f) == 0);
    memset(buf + 20 * 4096 + 10, 0xee, 5000);
    CHECK(file_pwrite(f, buf + 20 * 4096 + 10, 5000, 20 * 4096 + 10) == 5000);
    CHECK(file_sync(f) == 0);   /* since milestone 7 this commits: one generation */
    file_put(f);
    CHECK(read_matches("/mnt/big.bin", buf, big));
    CHECK(cosmofs_stats(mount_of("/mnt"), &st) == 0 && st.generation == st0.generation + 1);
    st0 = st;

    /* Rename, replace, unlink, rmdir. */
    CHECK(write_file("/mnt/dir/other.txt", "other", 5));
    CHECK(vfs_rename(NULL, "/mnt/dir/other.txt", "/mnt/dir/nested.txt") == 0);   /* replaces */
    CHECK(read_matches("/mnt/dir/nested.txt", "other", 5));
    CHECK(vfs_rename(NULL, "/mnt/dir/nested.txt", "/mnt/moved.txt") == 0);
    CHECK(vfs_stat(NULL, "/mnt/dir/nested.txt", &s) == -ENOENT);
    CHECK(vfs_rmdir(NULL, "/mnt/dir") == 0);
    CHECK(vfs_stat(NULL, "/mnt", &s) == 0 && s.nlink == 2);
    CHECK(vfs_rename(NULL, "/mnt/moved.txt", "/mnt/dir") == 0);
    CHECK(vfs_mkdir(NULL, "/mnt/dir", 0755) == -EEXIST);
    CHECK(vfs_unlink(NULL, "/mnt/dir") == 0);
    CHECK(vfs_mkdir(NULL, "/mnt/dir", 0755) == 0);
    CHECK(write_file("/mnt/dir/nested.txt", "nested", 6));

    /* Commit, remount, and everything is still there. */
    CHECK(vfs_sync() == 0);
    CHECK(cosmofs_stats(mount_of("/mnt"), &st) == 0 && st.generation == st0.generation + 1);
    CHECK(vfs_umount("/mnt") == 0);
    CHECK(vfs_mount("/mnt", "cosmofs", bd, 0) == 0);
    CHECK(read_matches("/mnt/hello.txt", "hello from the kernel", 21));
    CHECK(read_matches("/mnt/big.bin", buf, big));
    CHECK(read_matches("/mnt/dir/nested.txt", "nested", 6));
    CHECK(vfs_stat(NULL, "/mnt/moved.txt", &s) == -ENOENT);
    CHECK(vfs_stat(NULL, "/mnt/dir/..", &s) == 0 && s.ino == 1);

    /* Deleting the big file returns its blocks after the next commit. */
    CHECK(cosmofs_stats(mount_of("/mnt"), &st0) == 0);
    CHECK(vfs_unlink(NULL, "/mnt/big.bin") == 0);
    CHECK(vfs_sync() == 0);
    CHECK(cosmofs_stats(mount_of("/mnt"), &st) == 0);
    CHECK(st.free_blocks > st0.free_blocks + 40);
    CHECK(st.inode_count == st0.inode_count - 1);

    /* Truncate through O_TRUNC and re-extend. */
    CHECK(write_file("/mnt/hello.txt", "hello from the kernel", 21));
    CHECK(vfs_stat(NULL, "/mnt/hello.txt", &s) == 0 && s.size == 21);

    /* Errors. */
    CHECK(vfs_mkdir(NULL, "/mnt/dir/a-very-long-name-that-exceeds-the-forty-seven-byte-limit", 0755) == -ENAMETOOLONG);
    CHECK(vfs_rmdir(NULL, "/mnt/dir") == -ENOTEMPTY);
    CHECK(vfs_rename(NULL, "/mnt/dir", "/tmp/x") == -EXDEV);

    kfree(buf);
    CHECK(vfs_umount("/mnt") == 0);
    blkdev_put(bd);
    return true;
}

bool selftest_cosmofs_crash(const char **reason)
{
    struct blkdev *bd = scratch();
    if (bd == NULL) {
        kinfo("selftest: no vda; skipping");
        return true;
    }
    /* Mutate, then "crash" before the root is written: the previous
     * committed state must be intact and the free space unchanged. */
    CHECK(vfs_mount("/mnt", "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(mount_of("/mnt"), false);   /* nothing may commit before the discard */
    struct cosmofs_stats before, after;
    CHECK(cosmofs_stats(mount_of("/mnt"), &before) == 0);
    CHECK(write_file("/mnt/lost.txt", "this never lands", 16));
    CHECK(vfs_mkdir(NULL, "/mnt/lostdir", 0755) == 0);
    CHECK(vfs_unlink(NULL, "/mnt/hello.txt") == 0);
    CHECK(write_file("/mnt/dir/nested.txt", "overwritten", 11));
    struct cosmo_stat s;
    CHECK(vfs_stat(NULL, "/mnt/hello.txt", &s) == -ENOENT);
    cosmofs_test_discard_on_unmount(mount_of("/mnt"), true);
    CHECK(vfs_umount("/mnt") == 0);

    CHECK(vfs_mount("/mnt", "cosmofs", bd, 0) == 0);
    CHECK(cosmofs_stats(mount_of("/mnt"), &after) == 0);
    CHECK(after.generation == before.generation && after.free_blocks == before.free_blocks);
    CHECK(vfs_stat(NULL, "/mnt/lost.txt", &s) == -ENOENT);
    CHECK(vfs_stat(NULL, "/mnt/lostdir", &s) == -ENOENT);
    CHECK(read_matches("/mnt/hello.txt", "hello from the kernel", 21));
    CHECK(read_matches("/mnt/dir/nested.txt", "nested", 6));
    CHECK(vfs_umount("/mnt") == 0);

    /* A torn superblock slot is ignored in favour of the other. */
    struct spool *p;
    CHECK(pool_open(bd, &p) == 0);
    uint8_t *blk = kmalloc(4096, 0);
    CHECK(blk != NULL);
    uint8_t saved_a[4096], saved_b[4096];
    CHECK(pool_read(p, 0, blk) == 0);
    memcpy(saved_a, blk, 4096);
    CHECK(pool_read(p, 1, blk) == 0);
    memcpy(saved_b, blk, 4096);
    /* Corrupt whichever slot holds the newer generation. */
    uint64_t gen_a = ((const uint64_t *)saved_a)[3], gen_b = ((const uint64_t *)saved_b)[3];
    unsigned newer = gen_a >= gen_b ? 0 : 1;
    memcpy(blk, newer == 0 ? saved_a : saved_b, 4096);
    blk[100] ^= 0xff;
    CHECK(pool_write(p, newer, blk) == 0 && pool_flush(p) == 0);
    kfree(blk);
    pool_close(p);
    CHECK(vfs_mount("/mnt", "cosmofs", bd, 0) == 0);
    CHECK(cosmofs_stats(mount_of("/mnt"), &after) == 0);
    CHECK(after.generation == before.generation - 1 || after.generation == before.generation);
    kinfo("selftest: cosmofs: fell back to generation %llu after a torn slot", (unsigned long long)after.generation);
    /* Leave a healthy pair behind: a commit rewrites the torn slot. */
    CHECK(write_file("/mnt/touch.txt", "x", 1) && vfs_unlink(NULL, "/mnt/touch.txt") == 0);
    CHECK(vfs_sync() == 0);
    CHECK(vfs_umount("/mnt") == 0);
    CHECK(vfs_mount("/mnt", "cosmofs", bd, 0) == 0);
    CHECK(read_matches("/mnt/hello.txt", "hello from the kernel", 21));
    CHECK(read_matches("/mnt/dir/nested.txt", "nested", 6));
    CHECK(vfs_umount("/mnt") == 0);
    blkdev_put(bd);
    return true;
}

/* --- the transaction engine (audit milestone 7), on RAM devices --------------- */

#include <kernel/ramblk.h>
#include <kernel/timer.h>
#include <kernel/wait.h>

#include "cosmofs_format.h"

#if CONFIG_DEBUG

#define ENG "/mnt/eng"

static bool engine_mount(struct blkdev **bdp, uint64_t nblocks, const char **reason)
{
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);   /* a failed earlier test may have left one behind */
    struct blkdev *bd = ramblk_create(nblocks);
    CHECK(bd != NULL);
    CHECK(cosmofs_format(bd) == 0);
    int mk = vfs_mkdir(NULL, ENG, 0755);
    CHECK(mk == 0 || mk == -EEXIST);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    *bdp = bd;
    return true;
}

static bool engine_unmount(struct blkdev *bd, const char **reason)
{
    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_rmdir(NULL, ENG) == 0);
    ramblk_destroy(bd);
    return true;
}

/* Holes: a write far into a file allocates only its own block. */
bool selftest_cosmofs_holes(const char **reason)
{
    struct blkdev *bd;
    if (!engine_mount(&bd, 1024, reason))
        return false;
    struct cosmofs_stats st0, st1;
    CHECK(cosmofs_stats(mount_of(ENG), &st0) == 0);
    struct file *f;
    CHECK(vfs_open(NULL, ENG "/sparse", COSMO_O_RDWR | COSMO_O_CREAT, 0644, &f) == 0);
    static const char tail[] = "tail";
    CHECK(file_pwrite(f, tail, 4, 200ull << 20) == 4);   /* 200 MiB in, on a 4 MiB device */
    CHECK(file_sync(f) == 0);
    CHECK(cosmofs_stats(mount_of(ENG), &st1) == 0);
    CHECK(st0.free_blocks - st1.free_blocks <= 12);   /* one data block and a few metadata blocks, no zero fill */
    uint8_t buf[16];
    CHECK(file_pread(f, buf, 8, 100ull << 20) == 8);
    for (int i = 0; i < 8; i++)
        CHECK(buf[i] == 0);   /* a hole reads as zeros */
    CHECK(file_pread(f, buf, 4, 200ull << 20) == 4 && memcmp(buf, tail, 4) == 0);
    /* A block in the middle of the hole, then the first block: the runs
     * stay sorted and every read agrees. */
    CHECK(file_pwrite(f, "mid", 3, 50ull << 20) == 3);
    CHECK(file_pwrite(f, "head", 4, 0) == 4);
    CHECK(file_sync(f) == 0);
    CHECK(file_pread(f, buf, 4, 0) == 4 && memcmp(buf, "head", 4) == 0);
    CHECK(file_pread(f, buf, 3, 50ull << 20) == 3 && memcmp(buf, "mid", 3) == 0);
    CHECK(file_pread(f, buf, 4, 200ull << 20) == 4 && memcmp(buf, tail, 4) == 0);
    CHECK(file_pread(f, buf, 4, 1ull << 20) == 4 && buf[0] == 0 && buf[3] == 0);
    file_put(f);
    /* Remount: the holes and the data survive the commit. */
    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    CHECK(read_matches(ENG "/sparse", "head", 4) == false);   /* the file is 200 MiB + 4, not 4 bytes */
    CHECK(vfs_open(NULL, ENG "/sparse", COSMO_O_RDONLY, 0, &f) == 0);
    CHECK(file_pread(f, buf, 3, 50ull << 20) == 3 && memcmp(buf, "mid", 3) == 0);
    struct cosmo_stat s;
    file_stat(f, &s);
    CHECK(s.size == (200ull << 20) + 4);
    file_put(f);
    /* Truncate into the hole keeps the head, frees the tail. */
    CHECK(vfs_open(NULL, ENG "/sparse", COSMO_O_WRONLY | COSMO_O_TRUNC, 0, &f) == 0);
    file_put(f);
    CHECK(vfs_sync() == 0);
    CHECK(cosmofs_stats(mount_of(ENG), &st1) == 0);
    CHECK(st1.free_blocks + 6 >= st0.free_blocks);   /* the checksum tree of an empty file is gone too */
    kinfo("selftest: cosmofs-holes: a 200 MiB sparse file cost %llu blocks", (unsigned long long)(st0.free_blocks - st1.free_blocks));
    return engine_unmount(bd, reason);
}

/* Find the pool block holding a 4 KiB pattern (the test's way to corrupt data). */
static int64_t find_block(struct blkdev *bd, const uint8_t *pattern)
{
    struct spool *p;
    if (pool_open(bd, &p))
        return -1;
    uint8_t *blk = kmalloc(4096, 0);
    int64_t found = -1;
    for (uint64_t i = 2; blk && i < p->nblocks && found < 0; i++)
        if (pool_read(p, i, blk) == 0 && memcmp(blk, pattern, 4096) == 0)
            found = (int64_t)i;
    kfree(blk);
    pool_close(p);
    return found;
}

static bool corrupt_block(struct blkdev *bd, uint64_t blkno, unsigned off)
{
    struct spool *p;
    if (pool_open(bd, &p))
        return false;
    uint8_t *blk = kmalloc(4096, 0);
    bool ok = blk && pool_read(p, blkno, blk) == 0;
    if (ok) {
        blk[off] ^= 0x5a;
        ok = pool_write(p, blkno, blk) == 0 && pool_flush(p) == 0;
    }
    kfree(blk);
    pool_close(p);
    return ok;
}

/* Checksums: a flipped byte in a data block or a directory block is -EIO. */
bool selftest_cosmofs_csum(const char **reason)
{
    struct blkdev *bd;
    if (!engine_mount(&bd, 512, reason))
        return false;
    uint8_t *pat = kmalloc(4096, 0);
    CHECK(pat != NULL);
    for (unsigned i = 0; i < 4096; i++)
        pat[i] = (uint8_t)(i * 13 + 5);
    CHECK(write_file(ENG "/data", pat, 4096));
    CHECK(write_file(ENG "/other", "fine", 4));
    CHECK(vfs_mkdir(NULL, ENG "/d", 0755) == 0);
    CHECK(write_file(ENG "/d/x", "x", 1));
    CHECK(vfs_sync() == 0);
    CHECK(vfs_umount(ENG) == 0);   /* drop the page cache so reads go to the device */
    int64_t data_blk = find_block(bd, pat);
    CHECK(data_blk > 0);
    CHECK(corrupt_block(bd, (uint64_t)data_blk, 1000));
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    struct file *f;
    CHECK(vfs_open(NULL, ENG "/data", COSMO_O_RDONLY, 0, &f) == 0);
    uint8_t buf[64];
    CHECK(file_pread(f, buf, 64, 0) == -EIO);   /* refused, not returned wrong */
    file_put(f);
    CHECK(read_matches(ENG "/other", "fine", 4));   /* the rest is untouched */
    struct cosmofs_stats st;
    CHECK(cosmofs_stats(mount_of(ENG), &st) == 0 && st.csum_failures >= 1);
    /* Repair by rewriting: a new block, a new checksum. */
    CHECK(write_file(ENG "/data", pat, 4096));
    CHECK(read_matches(ENG "/data", pat, 4096));
    /* A directory block: /d holds one entry, "x"; flip a byte in it and
     * the lookup that reads the block is refused. */
    CHECK(vfs_sync() == 0);
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
    CHECK(corrupt_block(bd, (uint64_t)dir_blk, 40));   /* inside the entry's name bytes */
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    struct cosmo_stat cs;
    CHECK(vfs_stat(NULL, ENG "/d/x", &cs) == -EIO);
    CHECK(vfs_stat(NULL, ENG "/other", &cs) == 0);   /* the root's block is intact */
    CHECK(cosmofs_stats(mount_of(ENG), &st) == 0 && st.csum_failures >= 1);   /* this mount's count */
    kfree(pat);
    kinfo("selftest: cosmofs-csum: a corrupted data block reads -EIO and a rewrite repairs it (%llu failures counted)",
          (unsigned long long)st.csum_failures);
    return engine_unmount(bd, reason);
}

/* fsync is durable: a file synced before a "crash" survives, one not synced does not. */
bool selftest_cosmofs_fsync(const char **reason)
{
    struct blkdev *bd;
    if (!engine_mount(&bd, 512, reason))
        return false;
    struct cosmofs_stats st0, st1;
    CHECK(cosmofs_stats(mount_of(ENG), &st0) == 0);
    struct file *f;
    CHECK(vfs_open(NULL, ENG "/durable", COSMO_O_RDWR | COSMO_O_CREAT, 0644, &f) == 0);
    CHECK(file_write(f, "kept", 4) == 4);
    CHECK(file_sync(f) == 0);   /* commits the transaction */
    file_put(f);
    CHECK(cosmofs_stats(mount_of(ENG), &st1) == 0);
    CHECK(st1.generation == st0.generation + 1 && st1.commits == st0.commits + 1);
    CHECK(write_file(ENG "/lost", "gone", 4));   /* not synced */
    cosmofs_test_discard_on_unmount(mount_of(ENG), true);
    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    CHECK(read_matches(ENG "/durable", "kept", 4));
    struct cosmo_stat s;
    CHECK(vfs_stat(NULL, ENG "/lost", &s) == -ENOENT);
    kinfo("selftest: cosmofs-fsync: the synced file survived the discarded transaction, the unsynced one did not");
    return engine_unmount(bd, reason);
}

/* The metadata reserve: a full disk can still delete and commit. */
bool selftest_cosmofs_reserve(const char **reason)
{
    struct blkdev *bd;
    if (!engine_mount(&bd, 256, reason))
        return false;
    struct cosmofs_stats st;
    CHECK(cosmofs_stats(mount_of(ENG), &st) == 0);
    CHECK(st.reserve_blocks == 32);
    uint8_t *page = kmalloc(4096, 0);
    CHECK(page != NULL);
    memset(page, 0x42, 4096);
    /* Fill until data allocation is refused (pages are cached at write
     * and allocated at the sync that writes them back); the reserve
     * stays free. */
    unsigned files = 0, pages = 0;
    bool enospc = false;
    while (files < 64 && !enospc) {
        char path[32];
        ksnprintf(path, sizeof(path), ENG "/f%u", files);
        struct file *f;
        int orc = vfs_open(NULL, path, COSMO_O_RDWR | COSMO_O_CREAT, 0644, &f);
        if (orc == -ENOSPC) {
            enospc = true;
            break;
        }
        CHECK(orc == 0);
        files++;
        for (unsigned i = 0; i < 16; i++) {
            int64_t rc = file_write(f, page, 4096);
            if (rc == 4096) {
                pages++;
                continue;
            }
            CHECK(rc == -ENOSPC);
            enospc = true;
            break;
        }
        int src = file_sync(f);
        file_put(f);
        if (src == -ENOSPC)
            enospc = true;
        else
            CHECK(src == 0);
    }
    CHECK(enospc);
    CHECK(cosmofs_stats(mount_of(ENG), &st) == 0);
    CHECK(st.free_blocks <= st.reserve_blocks + 2 && st.free_blocks > 0);   /* stopped at the reserve */
    uint64_t full_free = st.free_blocks;
    /* Deletion needs metadata blocks: the reserve provides them. */
    CHECK(vfs_unlink(NULL, ENG "/f0") == 0);
    CHECK(vfs_unlink(NULL, ENG "/f1") == 0);
    CHECK(vfs_sync() == 0);
    CHECK(cosmofs_stats(mount_of(ENG), &st) == 0);
    CHECK(st.free_blocks > full_free + 20);
    CHECK(write_file(ENG "/again", page, 4096));   /* space is back */
    kfree(page);
    kinfo("selftest: cosmofs-reserve: %u files, %u pages until -ENOSPC with %llu blocks kept for metadata; unlink and commit freed %llu",
          files, pages, (unsigned long long)st.reserve_blocks, (unsigned long long)(st.free_blocks - full_free));
    return engine_unmount(bd, reason);
}

/* The newer root's tree is unreadable: mount falls back to the older slot. */
bool selftest_cosmofs_fallback(const char **reason)
{
    struct blkdev *bd;
    if (!engine_mount(&bd, 512, reason))
        return false;
    CHECK(write_file(ENG "/old", "old", 3));
    CHECK(vfs_sync() == 0);   /* generation 2 */
    CHECK(write_file(ENG "/new", "new", 3));
    CHECK(vfs_sync() == 0);   /* generation 3 */
    struct cosmofs_stats st;
    CHECK(cosmofs_stats(mount_of(ENG), &st) == 0 && st.generation == 3);
    /* Unmount without the bitmap-only commit that reclaims the pending
     * frees, so the slots hold exactly generations 2 and 3. */
    cosmofs_test_discard_on_unmount(mount_of(ENG), true);
    CHECK(vfs_umount(ENG) == 0);
    /* Corrupt the newer root's inode map root block: the tree does not load. */
    struct spool *p;
    CHECK(pool_open(bd, &p) == 0);
    uint8_t *blk = kmalloc(4096, 0);
    CHECK(blk != NULL);
    uint64_t imap_root = 0, newer_gen = 0;
    for (unsigned slot = 0; slot < 2; slot++) {
        CHECK(pool_read(p, slot, blk) == 0);
        const struct cfs_super *sb = (const struct cfs_super *)blk;
        if (memcmp(sb->magic, CFS_MAGIC, 8) == 0 && sb->generation > newer_gen) {
            newer_gen = sb->generation;
            imap_root = sb->imap_root;
        }
    }
    kfree(blk);
    pool_close(p);
    CHECK(newer_gen == 3 && imap_root >= 2);
    CHECK(corrupt_block(bd, imap_root, 100));
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);   /* falls back with a warning */
    CHECK(cosmofs_stats(mount_of(ENG), &st) == 0 && st.generation == 2);
    CHECK(read_matches(ENG "/old", "old", 3));
    struct cosmo_stat s;
    CHECK(vfs_stat(NULL, ENG "/new", &s) == -ENOENT);   /* generation 3's work is gone with its root */
    /* The next commit writes over the broken slot and the pair is healthy. */
    CHECK(write_file(ENG "/after", "after", 5));
    CHECK(vfs_sync() == 0);
    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    CHECK(cosmofs_stats(mount_of(ENG), &st) == 0 && st.generation >= 3);   /* 3, plus the unmount's bitmap commit */
    CHECK(read_matches(ENG "/after", "after", 5) && read_matches(ENG "/old", "old", 3));
    kinfo("selftest: cosmofs-fallback: an unreadable generation-3 tree fell back to generation 2 and was replaced");
    return engine_unmount(bd, reason);
}

/* The writeback thread commits on its own once the interval has passed. */
bool selftest_cosmofs_writeback(const char **reason)
{
    struct blkdev *bd;
    if (!engine_mount(&bd, 512, reason))
        return false;
    struct mount *mnt = mount_of(ENG);
    cosmofs_test_set_writeback(mnt, true);
    cosmofs_test_set_writeback_interval(mnt, 50);
    struct cosmofs_stats st0, st1;
    CHECK(cosmofs_stats(mnt, &st0) == 0);
    CHECK(write_file(ENG "/auto", "auto", 4));
    uint64_t deadline = clock_now_ns() + 2000000000ULL;
    do {
        thread_sleep_ms(20);
        CHECK(cosmofs_stats(mnt, &st1) == 0);
    } while (st1.generation == st0.generation && clock_now_ns() < deadline);
    CHECK(st1.generation == st0.generation + 1 && st1.wb_commits == st0.wb_commits + 1);
    /* Nothing more dirty: no further commits happen on their own. */
    thread_sleep_ms(200);
    struct cosmofs_stats st2;
    CHECK(cosmofs_stats(mnt, &st2) == 0 && st2.generation == st1.generation);
    /* The data is on disk without anyone calling sync. */
    cosmofs_test_discard_on_unmount(mnt, true);
    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    CHECK(read_matches(ENG "/auto", "auto", 4));
    kinfo("selftest: cosmofs-writeback: the thread committed generation %llu on its own", (unsigned long long)st1.generation);
    return engine_unmount(bd, reason);
}

#else
bool selftest_cosmofs_holes(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_csum(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_fsync(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_reserve(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_fallback(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_writeback(const char **reason) { (void)reason; return true; }
#endif

#if CONFIG_DEBUG
/* A crafted inode: two direct runs written out of order (a corruption
 * the header checksum does not see because the block is re-sealed).
 * Before the direct runs were validated on the map fast path, the first
 * block of the file read as a hole; now the inode is -EIO (Greptile on
 * PR #22). */
static uint32_t block_crc_test(const uint8_t *block)
{
    static const uint8_t zero4[4] = { 0 };
    size_t off = offsetof(struct cfs_mhdr, crc);
    uint32_t c = crc32c(block, off);
    c = crc32c_update(c, zero4, 4);
    return crc32c_update(c, block + off + 4, CFS_BLOCK - off - 4);
}

bool selftest_cosmofs_badmap(const char **reason)
{
    struct blkdev *bd;
    if (!engine_mount(&bd, 512, reason))
        return false;
    struct file *f;
    CHECK(vfs_open(NULL, ENG "/two", COSMO_O_RDWR | COSMO_O_CREAT, 0644, &f) == 0);
    uint8_t *page = kmalloc(4096, 0);
    CHECK(page != NULL);
    memset(page, 0x11, 4096);
    CHECK(file_pwrite(f, page, 4096, 0) == 4096);            /* run at lblk 0 */
    memset(page, 0x22, 4096);
    CHECK(file_pwrite(f, page, 4096, 5 * 4096) == 4096);     /* run at lblk 5, a hole between */
    CHECK(file_sync(f) == 0);
    uint64_t ino = f->vn->ino;
    file_put(f);
    cosmofs_test_discard_on_unmount(mount_of(ENG), true);   /* keep the slots as they are */
    CHECK(vfs_umount(ENG) == 0);

    /* Walk superblock -> IMAP1 -> IMAP0 -> INODES through the pool, swap
     * the two direct runs of the inode, re-seal the block. */
    struct spool *p;
    CHECK(pool_open(bd, &p) == 0);
    uint8_t *blk = kmalloc(4096, 0);
    CHECK(blk != NULL);
    uint64_t imap = 0, gen = 0;
    for (unsigned slot = 0; slot < 2; slot++) {
        CHECK(pool_read(p, slot, blk) == 0);
        const struct cfs_super *sb = (const struct cfs_super *)blk;
        if (memcmp(sb->magic, CFS_MAGIC, 8) == 0 && sb->generation > gen) {
            gen = sb->generation;
            imap = sb->imap_root;
        }
    }
    CHECK(imap >= 2);
    CHECK(pool_read(p, imap, blk) == 0);
    uint64_t l0 = ((const uint64_t *)(blk + CFS_MHDR_SIZE))[cfs_imap_l1_index(ino)];
    CHECK(pool_read(p, l0, blk) == 0);
    uint64_t ib = ((const uint64_t *)(blk + CFS_MHDR_SIZE))[cfs_imap_l0_index(ino)];
    CHECK(pool_read(p, ib, blk) == 0);
    struct cfs_inode *in = (struct cfs_inode *)(blk + CFS_MHDR_SIZE + cfs_inode_slot(ino) * CFS_INODE_SIZE);
    CHECK(in->ino == ino && in->direct[0].count == 1 && in->direct[1].count == 1 && in->direct[1].lblk == 5);
    struct cfs_extent tmp = in->direct[0];
    in->direct[0] = in->direct[1];
    in->direct[1] = tmp;   /* unsorted: lblk 5 before lblk 0 */
    struct cfs_mhdr *h = (struct cfs_mhdr *)blk;
    h->crc = 0;
    h->crc = block_crc_test(blk);
    CHECK(pool_write(p, ib, blk) == 0 && pool_flush(p) == 0);
    kfree(blk);
    pool_close(p);

    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    CHECK(vfs_open(NULL, ENG "/two", COSMO_O_RDONLY, 0, &f) == 0);
    CHECK(file_pread(f, page, 4096, 0) == -EIO);   /* not a hole of zeros */
    CHECK(file_pread(f, page, 4096, 5 * 4096) == -EIO);
    file_put(f);
    kfree(page);
    kinfo("selftest: cosmofs-badmap: an inode with unsorted direct runs is refused, not read as holes");
    return engine_unmount(bd, reason);
}

/* Snapshots: what the tree was, kept, while the live tree moves on
 * (design.md, "Format version 3"). */
bool selftest_cosmofs_snapshot(const char **reason)
{
    struct blkdev *bd;
    if (!engine_mount(&bd, 512, reason))
        return false;
    struct cosmofs_stats st0, st1;

    CHECK(write_file(ENG "/keep", "before", 6));
    CHECK(vfs_mkdir(NULL, ENG "/dir", 0755) == 0);
    CHECK(write_file(ENG "/dir/deep", "old", 3));
    CHECK(cosmofs_stats(mount_of(ENG), &st0) == 0);

    /* mkdir inside .snapshots takes one, and it commits. */
    CHECK(vfs_mkdir(NULL, ENG "/.snapshots/first", 0755) == 0);
    CHECK(cosmofs_stats(mount_of(ENG), &st1) == 0);
    CHECK(st1.generation > st0.generation);
    CHECK(vfs_mkdir(NULL, ENG "/.snapshots/first", 0755) == -EEXIST);

    /* The live tree moves: rewrite, delete, add. */
    CHECK(write_file(ENG "/keep", "after!", 6));
    CHECK(vfs_unlink(NULL, ENG "/dir/deep") == 0);
    CHECK(write_file(ENG "/fresh", "new", 3));
    CHECK(read_matches(ENG "/keep", "after!", 6));

    /* The snapshot still has what was there, at every depth. */
    CHECK(read_matches(ENG "/.snapshots/first/keep", "before", 6));
    CHECK(read_matches(ENG "/.snapshots/first/dir/deep", "old", 3));
    struct cosmo_stat s;
    CHECK(vfs_stat(NULL, ENG "/.snapshots/first/fresh", &s) == -ENOENT);   /* born after it */

    /* A snapshot is read-only, and .snapshots is not a place for files. */
    struct file *f;
    CHECK(vfs_open(NULL, ENG "/.snapshots/first/new", COSMO_O_RDWR | COSMO_O_CREAT, 0644, &f) == -EROFS);
    CHECK(vfs_mkdir(NULL, ENG "/.snapshots/first/sub", 0755) == -EROFS);
    CHECK(vfs_unlink(NULL, ENG "/.snapshots/first/keep") == -EROFS);

    /* A second snapshot, with a file born between the two: its blocks
     * belong to `second` alone, so deleting `second` must return them
     * even though `first` still exists. That is what makes the
     * accounting exact rather than conservative (design.md). */
    static char big[8192];
    memset(big, 'x', sizeof(big));
    CHECK(write_file(ENG "/born-late", big, sizeof(big)));
    CHECK(vfs_mkdir(NULL, ENG "/.snapshots/second", 0755) == 0);
    CHECK(read_matches(ENG "/.snapshots/second/keep", "after!", 6));
    CHECK(read_matches(ENG "/.snapshots/first/keep", "before", 6));   /* untouched by the newer one */
    CHECK(vfs_stat(NULL, ENG "/.snapshots/first/born-late", &s) == -ENOENT);

    /* Kill the late file: its blocks die while only `second` names them. */
    CHECK(vfs_unlink(NULL, ENG "/born-late") == 0);
    CHECK(vfs_sync() == 0);
    struct cosmofs_stats before_del, after_del;
    CHECK(cosmofs_stats(mount_of(ENG), &before_del) == 0);
    CHECK(vfs_rmdir(NULL, ENG "/.snapshots/second") == 0);
    CHECK(cosmofs_stats(mount_of(ENG), &after_del) == 0);
    /* Exact: those blocks come back now, not when `first` goes. */
    CHECK(after_del.free_blocks > before_del.free_blocks);
    CHECK(vfs_stat(NULL, ENG "/.snapshots/second", &s) == -ENOENT);
    CHECK(read_matches(ENG "/.snapshots/first/keep", "before", 6));
    CHECK(read_matches(ENG "/keep", "after!", 6));

    /* Deleting the last one returns its blocks: the free count recovers
     * to at least what it was before the snapshot existed. */
    CHECK(cosmofs_stats(mount_of(ENG), &st0) == 0);
    CHECK(vfs_rmdir(NULL, ENG "/.snapshots/first") == 0);
    CHECK(cosmofs_stats(mount_of(ENG), &st1) == 0);
    CHECK(st1.free_blocks >= st0.free_blocks);
    CHECK(vfs_rmdir(NULL, ENG "/.snapshots/first") == -ENOENT);
    CHECK(read_matches(ENG "/keep", "after!", 6));

    /* Storage somebody is reading is not dismantled. Hold a file open
     * inside a snapshot and the deletion is refused: were it allowed,
     * that handle would go on reading blocks the allocator had already
     * given to somebody else. It succeeds once the handle closes. */
    CHECK(vfs_mkdir(NULL, ENG "/.snapshots/busy", 0755) == 0);
    struct file *held;
    CHECK(vfs_open(NULL, ENG "/.snapshots/busy/keep", COSMO_O_RDONLY, 0, &held) == 0);
    CHECK(vfs_rmdir(NULL, ENG "/.snapshots/busy") == -EBUSY);
    CHECK(read_matches(ENG "/.snapshots/busy/keep", "after!", 6));   /* refused, and intact */
    file_put(held);
    CHECK(vfs_rmdir(NULL, ENG "/.snapshots/busy") == 0);
    CHECK(vfs_stat(NULL, ENG "/.snapshots/busy", &s) == -ENOENT);

    /* A snapshot's identity must not be positional: after deleting one,
     * a new snapshot must not inherit a cached vnode from the old. Take
     * A and B, delete A, take C, and read through B and C -- if the tag
     * were an index, C would land on B's cached root. */
    CHECK(write_file(ENG "/ident", "aaa", 3));
    CHECK(vfs_mkdir(NULL, ENG "/.snapshots/ident-a", 0755) == 0);
    CHECK(write_file(ENG "/ident", "bbb", 3));
    CHECK(vfs_mkdir(NULL, ENG "/.snapshots/ident-b", 0755) == 0);
    CHECK(read_matches(ENG "/.snapshots/ident-a/ident", "aaa", 3));
    CHECK(vfs_rmdir(NULL, ENG "/.snapshots/ident-a") == 0);
    CHECK(write_file(ENG "/ident", "ccc", 3));
    CHECK(vfs_mkdir(NULL, ENG "/.snapshots/ident-c", 0755) == 0);
    CHECK(read_matches(ENG "/.snapshots/ident-b/ident", "bbb", 3));
    CHECK(read_matches(ENG "/.snapshots/ident-c/ident", "ccc", 3));
    CHECK(vfs_rmdir(NULL, ENG "/.snapshots/ident-b") == 0);
    CHECK(vfs_rmdir(NULL, ENG "/.snapshots/ident-c") == 0);
    CHECK(vfs_unlink(NULL, ENG "/ident") == 0);

    kinfo("selftest: cosmofs-snapshot: history kept and released (%llu free blocks, %llu after)",
          (unsigned long long)st0.free_blocks, (unsigned long long)st1.free_blocks);
    return engine_unmount(bd, reason);
}

/* A snapshot survives an unmount: its entry is on disk, not in memory. */
bool selftest_cosmofs_snapshot_remount(const char **reason)
{
    struct blkdev *bd;
    if (!engine_mount(&bd, 512, reason))
        return false;
    CHECK(write_file(ENG "/f", "v1", 2));
    CHECK(vfs_mkdir(NULL, ENG "/.snapshots/s1", 0755) == 0);
    CHECK(write_file(ENG "/f", "v2", 2));
    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    CHECK(read_matches(ENG "/f", "v2", 2));
    CHECK(read_matches(ENG "/.snapshots/s1/f", "v1", 2));
    CHECK(vfs_rmdir(NULL, ENG "/.snapshots/s1") == 0);
    kinfo("selftest: cosmofs-snapshot-remount: the snapshot survived the unmount");
    return engine_unmount(bd, reason);
}
#else
bool selftest_cosmofs_badmap(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_snapshot(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_snapshot_remount(const char **reason) { (void)reason; return true; }
#endif
