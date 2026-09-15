/*
 * cosmofstest.c - Self-tests for the storage pool and cosmofs on the
 * scratch virtio disk (vda). They format the disk, so they run only
 * when it exists, and they leave a filesystem behind for init's
 * user-mode test: /hello.txt and /dir/nested.txt.
 */

#include <kernel/blk.h>
#include <kernel/cosmofs.h>
#include <uapi/cosmo/fsctl.h>
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
    CHECK(p->block_size == 4096 && p->nmembers == 1 && p->m[0].ncopies == 1 &&
          p->m[0].sectors_per_block[0] == 8 && p->nblocks == bd->capacity / 8);
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
    /* Member 0 addresses are the DVAs they always were, and a DVA
     * naming a member the pool does not have addresses nothing. */
    bool ok = pool_write(p, p->nblocks - 1, w) == 0 && pool_flush(p) == 0 && pool_read(p, p->nblocks - 1, r) == 0 &&
              memcmp(w, r, 4096) == 0 && pool_read(p, p->nblocks, r) == -EINVAL &&
              pool_write(p, p->nblocks, w) == -EINVAL && pool_read(p, POOL_DVA(1, 0), r) == -EINVAL &&
              POOL_DVA_VDEV(POOL_DVA(3, 7)) == 3 && POOL_DVA_BLK(POOL_DVA(3, 7)) == 7;
    kfree(w);
    kfree(r);
    pool_close(p);
    blkdev_put(bd);
    CHECK(ok);
    return true;
}

/*
 * A file of `pages` blocks whose contents nothing can compress. A page of
 * zeros costs almost nothing on a filesystem with compressed records, and
 * a test about a number of blocks would measure nothing.
 */
static bool write_wide_file(const char *path, unsigned pages)
{
    static char page[4096];
    struct file *f = NULL;
    if (vfs_open(NULL, path, COSMO_O_WRONLY | COSMO_O_CREAT | COSMO_O_TRUNC, 0644, &f))
        return false;
    uint32_t seed = 0x1234567u;
    bool ok = true;
    for (unsigned i = 0; ok && i < pages; i++) {
        for (unsigned k = 0; k < sizeof(page); k++) {
            seed = seed * 1103515245u + 12345u;
            page[k] = (char)(seed >> 16);
        }
        ok = file_write(f, page, sizeof(page)) == (int64_t)sizeof(page);
    }
    file_put(f);
    return ok;
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
    CHECK(st.free_blocks == st.total_blocks - 8);   /* 2 supers, members, index, bitmap, L1, L0, inodes */
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
    /* Fewer than the 45 blocks it covers: its contents repeat, so it is
     * stored as compressed records (cosmofs-compress measures that). */
    CHECK(st.free_blocks > st0.free_blocks + 5);
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

/* Bytes with nothing worth compressing in them. A test that measures
 * space has to write these, or it measures the compressor instead of
 * whatever it meant to measure. */
static void fill_incompressible(uint8_t *buf, size_t len, uint32_t seed)
{
    uint32_t x = seed | 1u;
    for (size_t i = 0; i < len; i++) {
        x = x * 1103515245u + 12345u;
        buf[i] = (uint8_t)(x >> 16);
    }
}


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
    /*
     * Back to within a few blocks of where it started. The slack covers
     * what an empty file still costs -- its checksum tree is gone, its
     * inode is not -- and, from format version 9, the record of what the
     * last commit freed: a version-9 filesystem always has one, so the
     * count it is compared against was taken before there was one
     * (docs/audit/next-subsystem-unmount-leak.md).
     */
    kinfo("selftest: cosmofs-holes: %llu blocks still held after truncating a 200 MiB sparse file",
          (unsigned long long)(st0.free_blocks - st1.free_blocks));
    CHECK(st1.free_blocks + 8 >= st0.free_blocks);
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
    fill_incompressible(page, 4096, 0x42);   /* the reserve is about space, not compression */
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
            /* Different bytes every time: the same page written sixteen
             * times is a record that compresses to almost nothing, and
             * this test is about running out of space. */
            fill_incompressible(page, 4096, 0x42u + files * 16u + i);
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

    /* ".." stays in history. Were the parent resolved through the live
     * inode map, this path would leave a read-only snapshot for the
     * live tree's directory of the same inode number -- today's
     * contents, and writable, under a path that says otherwise. */
    CHECK(read_matches(ENG "/.snapshots/first/dir/../keep", "before", 6));   /* not "after!" */
    CHECK(vfs_unlink(NULL, ENG "/.snapshots/first/dir/../keep") == -EROFS);
    CHECK(read_matches(ENG "/.snapshots/first/dir/../dir/deep", "old", 3));
    /* At a snapshot's root the way up is .snapshots, and out of that is
     * the live tree again. */
    CHECK(read_matches(ENG "/.snapshots/first/../first/keep", "before", 6));
    CHECK(read_matches(ENG "/.snapshots/../keep", "after!", 6));

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

/* A label's CRC, at its own offset. */
static uint32_t label_crc_test(const uint8_t *block)
{
    static const uint8_t zero4[4] = { 0 };
    size_t off = offsetof(struct cfs_label, crc);
    uint32_t c = crc32c(block, off);
    c = crc32c_update(c, zero4, 4);
    return crc32c_update(c, block + off + 4, CFS_BLOCK - off - 4);
}

/* The superblock's CRC covers the whole block with its own field zeroed,
 * at a different offset than a metadata header's. */
static uint32_t super_crc_test(const uint8_t *block)
{
    static const uint8_t zero4[4] = { 0 };
    size_t off = offsetof(struct cfs_super, crc);
    uint32_t c = crc32c(block, off);
    c = crc32c_update(c, zero4, 4);
    return crc32c_update(c, block + off + 4, CFS_BLOCK - off - 4);
}

/* Make a device look like one that was detached a commit ago: its
 * superblocks say an older generation, and every checksum on it is
 * still perfectly valid. That is exactly the case a mirror cannot
 * detect by checksums alone. */
static bool age_device(struct blkdev *bd, uint64_t older)
{
    struct spool *p;
    if (pool_open(bd, &p))
        return false;
    uint8_t *b = kmalloc(CFS_BLOCK, 0);
    if (b == NULL) {
        pool_close(p);
        return false;
    }
    bool ok = true, aged = false;
    for (unsigned slot = 0; slot < 2 && ok; slot++) {
        if (pool_read_copy(p, CFS_DVA(0, slot), 0, b) != 0)
            continue;
        struct cfs_super *sb = (struct cfs_super *)b;
        if (memcmp(sb->magic, CFS_MAGIC, 8) != 0)
            continue;   /* the unused slot */
        sb->generation = older;
        sb->crc = 0;
        sb->crc = super_crc_test(b);
        ok = pool_write_copy(p, CFS_DVA(0, slot), 0, b) == 0;
        aged = aged || ok;
    }
    ok = ok && aged;   /* it has to have actually aged something */
    ok = ok && pool_flush(p) == 0;
    kfree(b);
    pool_close(p);
    return ok;
}

/* Rewrite a member's label with an older generation: the device now
 * looks like one that missed a commit, with every checksum on it still
 * valid. Returns false when this device carries no label. */
static bool age_label(struct blkdev *bd, uint64_t older)
{
    struct spool *p;
    if (pool_open(bd, &p))
        return false;
    uint8_t *b = kmalloc(CFS_BLOCK, 0);
    if (b == NULL) {
        pool_close(p);
        return false;
    }
    bool ok = false;
    if (pool_read(p, CFS_DVA(0, 0), b) == 0) {
        struct cfs_label *l = (struct cfs_label *)b;
        if (memcmp(l->magic, CFS_LABEL_MAGIC, sizeof(l->magic)) == 0) {
            l->generation = older;
            l->crc = 0;
            l->crc = label_crc_test(b);
            ok = pool_write(p, CFS_DVA(0, 0), b) == 0 && pool_flush(p) == 0;
        }
    }
    kfree(b);
    pool_close(p);
    return ok;
}

/* Overwrite one copy of one block with rubbish, behind the
 * filesystem's back: what a rotting disk does. */
static bool rot_copy(struct blkdev *bd, uint64_t blk, uint8_t fill)
{
    struct spool *p;
    if (pool_open(bd, &p))
        return false;
    uint8_t *b = kmalloc(CFS_BLOCK, 0);
    if (b == NULL) {
        pool_close(p);
        return false;
    }
    memset(b, fill, CFS_BLOCK);
    bool ok = pool_write(p, CFS_DVA(0, blk), b) == 0 && pool_flush(p) == 0;
    kfree(b);
    pool_close(p);
    return ok;
}

/*
 * A mirrored member: two devices holding the same blocks. Rot one copy
 * of a metadata block and one of a data block, and the reads still
 * answer -- from the other copy, which is then written back over the
 * bad one. Rot both and the answer is -EIO, for that file and not for
 * the filesystem.
 */
bool selftest_cosmofs_mirror(const char **reason)
{
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    struct blkdev *bd[2] = { ramblk_create(512), ramblk_create(512) };
    CHECK(bd[0] != NULL && bd[1] != NULL);
    CHECK(cosmofs_format_mirror(bd, 1, 2) == 0);
    int mk = vfs_mkdir(NULL, ENG, 0755);
    CHECK(mk == 0 || mk == -EEXIST);
    CHECK(vfs_mount(ENG, "cosmofs", bd[0], 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);

    struct cosmofs_stats st;
    CHECK(cosmofs_stats(mount_of(ENG), &st) == 0);
    CHECK(st.members == 1 && st.devices == 2 && st.degraded == 0);
    /* One member's worth of space: a mirror costs capacity, not blocks. */
    CHECK(st.total_blocks == 512);

    static char data[4096];
    memset(data, 'm', sizeof(data));
    CHECK(write_file(ENG "/mirrored", data, sizeof(data)));
    CHECK(vfs_mkdir(NULL, ENG "/dir", 0755) == 0);
    CHECK(write_file(ENG "/dir/inner", "inner", 5));
    CHECK(vfs_sync() == 0);

    /* Where the file's one data block lives. */
    struct file *f;
    CHECK(vfs_open(NULL, ENG "/mirrored", COSMO_O_RDONLY, 0, &f) == 0);
    uint64_t ino = f->vn->ino;
    file_put(f);
    uint64_t pblk = 0;
    CHECK(cosmofs_test_block_of(mount_of(ENG), ino, 0, &pblk) == 0);
    CHECK(CFS_DVA_VDEV(pblk) == 0);

    /* Rot the second copy of that data block; the read comes from copy 0
     * and notices nothing. Then rot the first: the read has to fall back
     * to the second, which by then holds what copy 0 had. */
    CHECK(rot_copy(bd[1], CFS_DVA_BLK(pblk), 0xA5));
    CHECK(read_matches(ENG "/mirrored", data, sizeof(data)));
    CHECK(cosmofs_stats(mount_of(ENG), &st) == 0);
    uint64_t repairs0 = st.repairs;

    /* A scrub reads everything and puts the rotted copy right. */
    struct cosmofs_scrub_stats sc;
    CHECK(cosmofs_scrub(mount_of(ENG), &sc) == 0);
    CHECK(sc.blocks_read > 0 && sc.inodes >= 3 && sc.unrecoverable == 0);
    CHECK(sc.repaired >= 1);
    CHECK(cosmofs_stats(mount_of(ENG), &st) == 0 && st.repairs > repairs0);

    /* A second scrub finds nothing to do: the first one fixed it. */
    struct cosmofs_scrub_stats sc2;
    CHECK(cosmofs_scrub(mount_of(ENG), &sc2) == 0);
    CHECK(sc2.repaired == 0 && sc2.unrecoverable == 0);

    /* Rot the second copy of a *metadata* block that no read will
     * choose -- the inode map's root, which is reached through copy 0
     * every time. Only a scrub that looks at every copy can see it. */
    uint8_t *sblk = kmalloc(CFS_BLOCK, 0);
    CHECK(sblk != NULL);
    struct spool *sp;
    CHECK(pool_open(bd[0], &sp) == 0);
    CHECK(pool_read(sp, CFS_SUPER_A, sblk) == 0 || pool_read(sp, CFS_SUPER_B, sblk) == 0);
    uint64_t imap_root = 0, sgen = 0;
    for (unsigned slot = 0; slot < 2; slot++) {
        if (pool_read(sp, slot, sblk) != 0)
            continue;
        const struct cfs_super *sb = (const struct cfs_super *)sblk;
        if (memcmp(sb->magic, CFS_MAGIC, 8) == 0 && sb->generation > sgen) {
            sgen = sb->generation;
            imap_root = sb->imap_root;
        }
    }
    pool_close(sp);
    kfree(sblk);
    CHECK(imap_root != 0);
    CHECK(rot_copy(bd[1], CFS_DVA_BLK(imap_root), 0x77));
    CHECK(read_matches(ENG "/dir/inner", "inner", 5));   /* copy 0 answers; nothing notices */
    CHECK(cosmofs_scrub(mount_of(ENG), &sc) == 0);
    CHECK(sc.repaired >= 1 && sc.unrecoverable == 0);
    CHECK(cosmofs_scrub(mount_of(ENG), &sc2) == 0 && sc2.repaired == 0);

    /* Now rot copy 0 of the same block: the read falls back to copy 1
     * and repairs copy 0. */
    CHECK(rot_copy(bd[0], CFS_DVA_BLK(pblk), 0x5A));
    CHECK(read_matches(ENG "/mirrored", data, sizeof(data)));
    CHECK(cosmofs_scrub(mount_of(ENG), &sc) == 0 && sc.unrecoverable == 0);

    /* Both copies gone: that file is unreadable, and the rest of the
     * filesystem is not. */
    CHECK(rot_copy(bd[0], CFS_DVA_BLK(pblk), 0x11));
    CHECK(rot_copy(bd[1], CFS_DVA_BLK(pblk), 0x22));
    CHECK(vfs_open(NULL, ENG "/mirrored", COSMO_O_RDONLY, 0, &f) == 0);
    static char got[4096];
    CHECK(file_read(f, got, sizeof(got)) == -EIO);
    file_put(f);
    CHECK(read_matches(ENG "/dir/inner", "inner", 5));
    CHECK(cosmofs_scrub(mount_of(ENG), &sc) == -EIO && sc.unrecoverable == 1);

    CHECK(vfs_umount(ENG) == 0);

    /* A device that was detached while the pool went on being written
     * carries older contents that pass every checksum on it. It is
     * recognised by the generation and left out of the mirror: the pool
     * comes up degraded rather than quietly serving old blocks. */
    CHECK(age_device(bd[1], 1));
    CHECK(vfs_mount(ENG, "cosmofs", bd[0], 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    CHECK(cosmofs_stats(mount_of(ENG), &st) == 0);
    CHECK(st.devices == 1 && st.degraded == 1);
    CHECK(read_matches(ENG "/dir/inner", "inner", 5));
    CHECK(vfs_umount(ENG) == 0);

    CHECK(vfs_rmdir(NULL, ENG) == 0);
    ramblk_destroy(bd[0]);
    ramblk_destroy(bd[1]);
    kinfo("selftest: cosmofs-mirror: two copies, %llu blocks scrubbed", (unsigned long long)sc.blocks_read);
    return true;
}

/*
 * A mirrored member whose copy 0 is the stale one. The generation is
 * recorded so that a device which missed a commit is not mirrored; it
 * would be worth nothing if the copy that happens to be labelled first
 * were served anyway.
 */
bool selftest_cosmofs_mirror_stale(const char **reason)
{
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    struct blkdev *bd[4];
    for (unsigned i = 0; i < 4; i++) {
        bd[i] = ramblk_create(256);
        CHECK(bd[i] != NULL);
    }
    /* Two members of two copies: bd[0..1] are member 0, bd[2..3] member 1. */
    CHECK(cosmofs_format_mirror(bd, 2, 2) == 0);
    int mk = vfs_mkdir(NULL, ENG, 0755);
    CHECK(mk == 0 || mk == -EEXIST);
    CHECK(vfs_mount(ENG, "cosmofs", bd[0], 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    struct cosmofs_stats st;
    CHECK(cosmofs_stats(mount_of(ENG), &st) == 0);
    CHECK(st.members == 2 && st.devices == 4 && st.degraded == 0);
    CHECK(write_file(ENG "/across", "two members, two copies", 23));
    CHECK(vfs_sync() == 0);
    CHECK(vfs_umount(ENG) == 0);

    /* Age member 1's copy 0 by rewriting its label to an older
     * generation: still a valid label, still valid checksums, older
     * contents. The mount must pass over it and take the other copy. */
    CHECK(age_label(bd[2], 1) || age_label(bd[3], 1));
    CHECK(vfs_mount(ENG, "cosmofs", bd[0], 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    CHECK(cosmofs_stats(mount_of(ENG), &st) == 0);
    CHECK(st.members == 2 && st.devices == 3 && st.degraded == 1);
    CHECK(read_matches(ENG "/across", "two members, two copies", 23));
    struct cosmofs_scrub_stats sc;
    CHECK(cosmofs_scrub(mount_of(ENG), &sc) == 0 && sc.unrecoverable == 0);

    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_rmdir(NULL, ENG) == 0);
    for (unsigned i = 0; i < 4; i++)
        ramblk_destroy(bd[i]);
    kinfo("selftest: cosmofs-mirror-stale: a stale copy is passed over, whichever one it is");
    return true;
}

/*
 * Compression: what it saves, and that everything still reads back.
 * A record is the unit -- eight logical blocks compressed together --
 * so the interesting cases are the ones that cut across one: writing a
 * page inside a record, and truncating into the middle of one.
 */
static bool read_matches_prefix(const char *path, const void *data, size_t len);

bool selftest_cosmofs_compress(const char **reason)
{
    struct blkdev *bd;
    if (!engine_mount(&bd, 1024, reason))
        return false;
    const size_t len = 32 * 4096;   /* four records */
    uint8_t *dense = kmalloc(len, 0), *sparse_data = kmalloc(len, 0), *back = kmalloc(len, 0);
    CHECK(dense != NULL && sparse_data != NULL && back != NULL);
    /* Repetitive: what compression is for. */
    for (size_t i = 0; i < len; i++)
        sparse_data[i] = (uint8_t)(i % 61);
    /* A counter through a multiplier: no repeats worth finding. */
    uint32_t x = 7;
    for (size_t i = 0; i < len; i++) {
        x = x * 1103515245u + 12345u;
        dense[i] = (uint8_t)(x >> 16);
    }

    struct cosmofs_stats st0, st1, st2;
    CHECK(cosmofs_stats(mount_of(ENG), &st0) == 0);
    CHECK(write_file(ENG "/small", sparse_data, len));
    CHECK(vfs_sync() == 0);
    CHECK(cosmofs_stats(mount_of(ENG), &st1) == 0);
    CHECK(write_file(ENG "/big", dense, len));
    CHECK(vfs_sync() == 0);
    CHECK(cosmofs_stats(mount_of(ENG), &st2) == 0);

    uint64_t compressible = st0.free_blocks - st1.free_blocks;
    uint64_t incompressible = st1.free_blocks - st2.free_blocks;
    /* The repetitive file is stored in a fraction of its 32 blocks; the
     * other one cannot be, and is stored as it is. */
    kinfo("cosmofs-compress: %llu blocks compressible, %llu not", (unsigned long long)compressible,
          (unsigned long long)incompressible);
    CHECK(compressible < 16 && incompressible >= 32);
    CHECK(compressible * 3 < incompressible);

    /* Both read back exactly, through the records and around them. */
    CHECK(read_matches(ENG "/small", sparse_data, len));
    CHECK(read_matches(ENG "/big", dense, len));

    /* A page written inside a compressed record: the record is read,
     * rebuilt around the new page, and written again. */
    struct file *f;
    CHECK(vfs_open(NULL, ENG "/small", COSMO_O_RDWR, 0, &f) == 0);
    memset(sparse_data + 3 * 4096, 0x5a, 4096);
    CHECK(file_pwrite(f, sparse_data + 3 * 4096, 4096, 3 * 4096) == 4096);
    CHECK(file_sync(f) == 0);
    file_put(f);
    CHECK(read_matches(ENG "/small", sparse_data, len));

    /* A partial page inside a record, which reads the record to fill in
     * what the write does not cover. */
    CHECK(vfs_open(NULL, ENG "/small", COSMO_O_RDWR, 0, &f) == 0);
    memset(sparse_data + 9 * 4096 + 100, 0x33, 500);
    CHECK(file_pwrite(f, sparse_data + 9 * 4096 + 100, 500, 9 * 4096 + 100) == 500);
    CHECK(file_sync(f) == 0);
    file_put(f);
    CHECK(read_matches(ENG "/small", sparse_data, len));

    /* Truncating into the middle of a record: what survives is rewritten
     * as ordinary blocks, and what is past the end must read as zeros
     * when the file grows again -- not as the record's old contents. */
    int trc = vfs_truncate(NULL, ENG "/small", 10 * 4096 + 7);
    if (trc)
        kerror("compress: truncate returned %d", trc);
    CHECK(trc == 0);
    CHECK(read_matches_prefix(ENG "/small", sparse_data, 10 * 4096 + 7));
    CHECK(vfs_truncate(NULL, ENG "/small", len) == 0);
    CHECK(vfs_open(NULL, ENG "/small", COSMO_O_RDONLY, 0, &f) == 0);
    CHECK(file_read(f, back, len) == (int64_t)len);
    file_put(f);
    CHECK(memcmp(back, sparse_data, 10 * 4096 + 7) == 0);
    for (size_t i = 10 * 4096 + 7; i < len; i++)
        CHECK(back[i] == 0);

    /* A scrub reads every record through the checksums of its physical
     * blocks. */
    struct cosmofs_scrub_stats sc;
    CHECK(cosmofs_scrub(mount_of(ENG), &sc) == 0 && sc.unrecoverable == 0);

    kfree(dense);
    kfree(sparse_data);
    kfree(back);
    kinfo("selftest: cosmofs-compress: %llu blocks for 32 compressible, %llu for 32 that are not",
          (unsigned long long)compressible, (unsigned long long)incompressible);
    return engine_unmount(bd, reason);
}

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
    CHECK(cosmofs_format_encrypted(bd, key, sizeof(key) - 1) == 0);
    int mk = vfs_mkdir(NULL, ENG, 0755);
    CHECK(mk == 0 || mk == -EEXIST);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    /* No key has arrived: the metadata mounted, the contents are shut. */
    CHECK(cosmofs_test_unlock(mount_of(ENG), "wrong key", 9) == -EKEYREJECTED);
    CHECK(cosmofs_test_unlock(mount_of(ENG), key, sizeof(key) - 1) == 0);

    static const char secret[] = "the quick brown fox jumps over the lazy dog, repeatedly and at length";
    CHECK(write_file(ENG "/secret.txt", secret, sizeof(secret) - 1));
    CHECK(vfs_mkdir(NULL, ENG "/private", 0755) == 0);
    CHECK(write_file(ENG "/private/inner", secret, sizeof(secret) - 1));
    CHECK(vfs_sync() == 0);
    CHECK(read_matches(ENG "/secret.txt", secret, sizeof(secret) - 1));

    /* The plaintext is not on the disk. Read the block the file's data
     * actually occupies and look for it. */
    struct file *f;
    CHECK(vfs_open(NULL, ENG "/secret.txt", COSMO_O_RDONLY, 0, &f) == 0);
    uint64_t ino = f->vn->ino;
    file_put(f);
    uint64_t dva = 0;
    CHECK(cosmofs_test_block_of(mount_of(ENG), ino, 0, &dva) == 0);
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
    CHECK(write_file(ENG "/secret.txt", secret, sizeof(secret) - 1));
    CHECK(vfs_sync() == 0);
    struct cosmofs_scrub_stats sc;
    CHECK(cosmofs_scrub(mount_of(ENG), &sc) == 0 && sc.unrecoverable == 0);

    /* A valid block of this very file, moved to another of its offsets,
     * must be refused. The tag authenticates where the block belongs as
     * well as what it holds, and the reader supplies the position it
     * believes it is reading -- so a transplant fails even though the
     * ciphertext, nonce and tag are all genuine. */
    static char two[2 * CFS_BLOCK];
    for (size_t i = 0; i < sizeof(two); i++)
        two[i] = (char)(i / CFS_BLOCK ? 'B' : 'A');
    CHECK(write_file(ENG "/moved.bin", two, sizeof(two)));
    CHECK(vfs_sync() == 0);
    struct file *mf;
    CHECK(vfs_open(NULL, ENG "/moved.bin", COSMO_O_RDONLY, 0, &mf) == 0);
    uint64_t mino = mf->vn->ino;
    file_put(mf);
    uint64_t dva0 = 0, dva1 = 0;
    CHECK(cosmofs_test_block_of(mount_of(ENG), mino, 0, &dva0) == 0);
    CHECK(cosmofs_test_block_of(mount_of(ENG), mino, 1, &dva1) == 0);
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
        CHECK(write_file(ENG "/nonce.bin", secret, sizeof(secret) - 1));
        CHECK(vfs_sync() == 0);
        struct file *nf;
        CHECK(vfs_open(NULL, ENG "/nonce.bin", COSMO_O_RDONLY, 0, &nf) == 0);
        uint64_t nino = nf->vn->ino;
        file_put(nf);
        uint64_t ndva = 0;
        CHECK(cosmofs_test_block_of(mount_of(ENG), nino, 0, &ndva) == 0);
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
    CHECK(read_matches(ENG "/nonce.bin", secret, sizeof(secret) - 1));
    CHECK(vfs_unlink(NULL, ENG "/nonce.bin") == 0);

    /* Rotation rewrites one block: the old key stops working, the new
     * one starts, and every file is still readable without being
     * rewritten. */
    static const char key2[] = "a different key entirely";
    CHECK(cosmofs_rekey(mount_of(ENG), key2, sizeof(key2) - 1) == 0);
    CHECK(vfs_sync() == 0);
    CHECK(read_matches(ENG "/secret.txt", secret, sizeof(secret) - 1));
    CHECK(vfs_umount(ENG) == 0);

    /* Remount with no key at all. The mount succeeds, and then refuses
     * to walk a path: a directory's entries are its inode's data, so
     * with names encrypted there is no way in. That is the design
     * working, not a limitation of it. */
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    struct cosmo_stat st;
    CHECK(vfs_stat(NULL, ENG "/secret.txt", &st) == -ENOKEY);
    CHECK(vfs_stat(NULL, ENG, &st) == 0 && st.type == COSMO_DT_DIR);   /* the root itself is a vnode */
    /* And a scrub still runs, because what it checks needs no key. */
    CHECK(cosmofs_scrub(mount_of(ENG), &sc) == 0 && sc.unrecoverable == 0 && sc.blocks_read > 0);
    /* The old key is refused; the new one opens it. */
    CHECK(cosmofs_test_unlock(mount_of(ENG), key, sizeof(key) - 1) == -EKEYREJECTED);
    CHECK(cosmofs_test_unlock(mount_of(ENG), key2, sizeof(key2) - 1) == 0);
    CHECK(read_matches(ENG "/secret.txt", secret, sizeof(secret) - 1));
    CHECK(read_matches(ENG "/private/inner", secret, sizeof(secret) - 1));

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
    h->crc = block_crc_test(blk);
    CHECK(pool_write(p, members, blk) == 0 && pool_flush(p) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd[0], 0) == -EIO);

    /* A member larger than the device that carries it. */
    mb->m[1].nblocks = was + 1;
    h->crc = 0;
    h->crc = block_crc_test(blk);
    CHECK(pool_write(p, members, blk) == 0 && pool_flush(p) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd[0], 0) == -EIO);

    /* A first usable block inside the label. */
    mb->m[1].nblocks = was;
    mb->m[1].first_usable = 0;
    h->crc = 0;
    h->crc = block_crc_test(blk);
    CHECK(pool_write(p, members, blk) == 0 && pool_flush(p) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd[0], 0) == -EIO);

    /* Put it back: the mount works, so the refusals above were about the
     * corruption and not about the surgery. */
    mb->m[1].first_usable = 1;
    h->crc = 0;
    h->crc = block_crc_test(blk);
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
static bool check_is_clean(const char **reason, struct cosmofs_check_report *rep)
{
    int rc = cosmofs_check(mount_of(ENG), rep, 0);
    if (rc != 0) {
        *reason = "the check itself failed";
        return false;
    }
    return true;
}

bool selftest_cosmofs_check_clean(const char **reason)
{
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    struct blkdev *bd = ramblk_create(512);
    CHECK(bd != NULL);
    CHECK(cosmofs_format(bd) == 0);
    int mk = vfs_mkdir(NULL, ENG, 0755);
    CHECK(mk == 0 || mk == -EEXIST);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);

    /* A filesystem with something of everything in it: files, a
     * directory, a symbolic link, a rewritten file and a removed one. */
    CHECK(write_file(ENG "/one", "first", 5));
    CHECK(vfs_mkdir(NULL, ENG "/dir", 0755) == 0);
    CHECK(write_file(ENG "/dir/two", "second", 6));
    CHECK(vfs_symlink(NULL, ENG "/link", "one") == 0);
    CHECK(write_file(ENG "/one", "first again, longer", 19));
    CHECK(write_file(ENG "/gone", "x", 1));
    CHECK(vfs_unlink(NULL, ENG "/gone") == 0);
    CHECK(vfs_sync() == 0);

    struct cosmofs_check_report rep;
    CHECK(check_is_clean(reason, &rep));
    CHECK(rep.clean);
    CHECK(!rep.partial);
    /* The arithmetic is the assertion: a walk that saw nothing would
     * report clean and fail here. */
    struct cosmofs_stats st;
    CHECK(cosmofs_stats(mount_of(ENG), &st) == 0);
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
    cosmofs_test_set_writeback(mount_of(ENG), false);
    CHECK(write_file(ENG "/file", "bytes", 5));
    CHECK(vfs_sync() == 0);

    struct cosmofs_check_report rep;
    CHECK(check_is_clean(reason, &rep) && rep.clean);
    uint64_t free_before = rep.counted_free;

    /* Mark one free block allocated and tell nobody: a leak, exactly as
     * a crash between an allocation and the write that would have used
     * it leaves one. */
    uint64_t leaked = 0;
    CHECK(cosmofs_test_corrupt(mount_of(ENG), COSMOFS_CORRUPT_LEAK, 0, &leaked) == 0);
    CHECK(check_is_clean(reason, &rep));
    CHECK(!rep.clean);
    CHECK(rep.alloc_not_seen.count == 1);
    CHECK(rep.alloc_not_seen.named == 1 && rep.alloc_not_seen.name[0] == leaked);
    CHECK(rep.counted_free == free_before - 1);
    CHECK(rep.dup.count == 0 && rep.orphan.count == 0);   /* only the one class */

    /* Repair gives it back, and the second pass is clean. */
    CHECK(cosmofs_check(mount_of(ENG), &rep, COSMOFS_CHECK_REPAIR) == 0);
    CHECK(rep.alloc_not_seen.repaired == 1);
    CHECK(vfs_sync() == 0);
    CHECK(check_is_clean(reason, &rep));
    CHECK(rep.clean);
    CHECK(rep.counted_free == free_before);

    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_rmdir(NULL, ENG) == 0);
    ramblk_destroy(bd);
    kinfo("selftest: cosmofs-check-leak: one leaked block found by number and given back");
    return true;
}

/* A filesystem with one file, mounted, write-back off: the fixture every
 * fault test starts from. */
static bool check_fixture(struct blkdev **bd, const char **reason)
{
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    *bd = ramblk_create(512);
    if (*bd == NULL) {
        *reason = "no ram disk";
        return false;
    }
    if (cosmofs_format(*bd) != 0) {
        *reason = "format failed";
        return false;
    }
    int mk = vfs_mkdir(NULL, ENG, 0755);
    if (mk != 0 && mk != -EEXIST) {
        *reason = "mkdir failed";
        return false;
    }
    if (vfs_mount(ENG, "cosmofs", *bd, 0) != 0) {
        *reason = "mount failed";
        return false;
    }
    cosmofs_test_set_writeback(mount_of(ENG), false);
    if (!write_file(ENG "/file", "bytes", 5) || vfs_mkdir(NULL, ENG "/sub", 0755) != 0 || vfs_sync() != 0) {
        *reason = "populate failed";
        return false;
    }
    return true;
}

/* The fixture's file, by number: the corruption hook takes an inode and
 * not a path, so the test does the one name resolution. */
static uint64_t check_file_ino(void)
{
    struct cosmo_stat st;
    return vfs_stat(NULL, ENG "/file", &st) == 0 ? st.ino : 0;
}

static void check_teardown(struct blkdev *bd)
{
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    (void)vfs_rmdir(NULL, ENG);
    if (bd)
        ramblk_destroy(bd);
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
    CHECK(check_fixture(&bd, reason));
    CHECK((file_ino = check_file_ino()) != 0);
    CHECK(cosmofs_test_corrupt(mount_of(ENG), COSMOFS_CORRUPT_FREE_IN_USE, file_ino, &what) == 0);
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0);
    CHECK(r.seen_not_alloc.count == 1 && r.seen_not_alloc.name[0] == what);
    CHECK(r.alloc_not_seen.count == 0);
    CHECK(cosmofs_check(mount_of(ENG), &r, COSMOFS_CHECK_REPAIR) == 0);
    CHECK(r.seen_not_alloc.repaired == 0);   /* refused, and says so by not counting one */
    check_teardown(bd);

    /* Two inodes over one block. */
    CHECK(check_fixture(&bd, reason));
    CHECK((file_ino = check_file_ino()) != 0);
    CHECK(cosmofs_test_corrupt(mount_of(ENG), COSMOFS_CORRUPT_CROSSLINK, file_ino, &what) == 0);
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0);
    CHECK(r.dup.count == 1 && r.dup.name[0] == what);
    CHECK(cosmofs_check(mount_of(ENG), &r, COSMOFS_CHECK_REPAIR) == 0);
    CHECK(r.dup.repaired == 0);              /* never repaired: choosing a winner is data loss */
    check_teardown(bd);

    /* A link count the entries disagree with. */
    CHECK(check_fixture(&bd, reason));
    CHECK((file_ino = check_file_ino()) != 0);
    CHECK(cosmofs_test_corrupt(mount_of(ENG), COSMOFS_CORRUPT_NLINK, file_ino, &what) == 0);
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0);
    CHECK(r.nlink_wrong.count == 1 && r.nlink_wrong.name[0] == what);
    CHECK(cosmofs_check(mount_of(ENG), &r, COSMOFS_CHECK_REPAIR) == 0);
    CHECK(r.nlink_wrong.repaired == 1);
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0 && r.clean);
    check_teardown(bd);

    /* An inode with blocks that no name reaches: the crash leak. */
    CHECK(check_fixture(&bd, reason));
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0 && r.clean);
    uint64_t free_before = r.counted_free;
    CHECK(cosmofs_test_corrupt(mount_of(ENG), COSMOFS_CORRUPT_ORPHAN, 0, &what) == 0);
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0);
    CHECK(r.orphan.count == 1 && r.orphan.name[0] == what);
    CHECK(cosmofs_check(mount_of(ENG), &r, COSMOFS_CHECK_REPAIR) == 0);
    CHECK(r.orphan.repaired == 1);
    /* Twice: a block freed in a transaction is reusable only after that
     * transaction's root is committed, so the count returns on the
     * commit after the repair's (cosmofs_core.c, the deferred free
     * list). */
    CHECK(vfs_sync() == 0);
    CHECK(vfs_sync() == 0);
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0);
    CHECK(r.clean && r.counted_free == free_before);   /* the space came back */
    check_teardown(bd);

    /* An entry naming a slot nothing allocated. The corruption overwrote
     * the number in a real entry, so the inode it used to name has lost
     * its only name -- and repair must not take that for an orphan and
     * destroy it. The file is still readable afterwards through the
     * entry's own parent, because nothing was freed. */
    CHECK(check_fixture(&bd, reason));
    CHECK(cosmofs_test_corrupt(mount_of(ENG), COSMOFS_CORRUPT_DANGLING, 0, &what) == 0);
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0);
    CHECK(r.dangling_entry.count == 1 && r.dangling_entry.name[0] == what);
    uint64_t orphans_at_dangle = r.orphan.count;
    CHECK(orphans_at_dangle >= 1);             /* the inode that lost its name */
    CHECK(cosmofs_check(mount_of(ENG), &r, COSMOFS_CHECK_REPAIR) == 0);
    CHECK(r.repair_refused);
    CHECK(r.dangling_entry.repaired == 0 && r.orphan.repaired == 0);
    CHECK(r.alloc_not_seen.repaired == 0);
    CHECK(vfs_sync() == 0);
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0);
    /* Still there. Had repair believed the walk, the inode would be gone
     * and this count would be one lower. */
    CHECK(r.orphan.count == orphans_at_dangle);
    check_teardown(bd);

    /* An entry whose type its inode does not have. The walk skips the
     * entry, so the inode it names looks unreferenced -- the same trap as
     * the dangling entry, without anything being unreadable. */
    CHECK(check_fixture(&bd, reason));
    CHECK(cosmofs_test_corrupt(mount_of(ENG), COSMOFS_CORRUPT_DIRENT, 0, &what) == 0);
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0);
    CHECK(r.dir_bad.count >= 1 && r.dir_bad.name[0] == what);
    CHECK(!r.partial);                         /* nothing was unreadable */
    uint64_t orphans_at_dirent = r.orphan.count;
    CHECK(orphans_at_dirent >= 1);             /* the inode whose entry was skipped */
    CHECK(cosmofs_check(mount_of(ENG), &r, COSMOFS_CHECK_REPAIR) == 0);
    CHECK(r.repair_refused);                   /* and repair still refuses */
    CHECK(r.dir_bad.repaired == 0 && r.orphan.repaired == 0);
    CHECK(vfs_sync() == 0);
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0);
    CHECK(r.orphan.count == orphans_at_dirent);
    check_teardown(bd);

    /* Both superblock totals the walk disagrees with: free blocks one
     * too high, inodes one too low. Two counters, two findings -- a
     * check that reported "the superblock is wrong" once would say the
     * same thing whichever of them was broken. */
    CHECK(check_fixture(&bd, reason));
    uint64_t free_was = 0, inodes_was = 0;
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0 && r.clean);
    free_was = r.counted_free;
    inodes_was = r.counted_inodes;
    CHECK(inodes_was >= 2);                  /* the root and the fixture's file */
    CHECK(cosmofs_test_corrupt(mount_of(ENG), COSMOFS_CORRUPT_COUNTER, 0, &what) == 0);
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0);
    CHECK(r.counter_wrong.count == 2);
    CHECK(r.counted_free == free_was && r.counted_inodes == inodes_was);   /* the walk is unmoved */
    CHECK(r.orphan.count == 0);              /* a wrong total is not a missing inode */
    CHECK(cosmofs_check(mount_of(ENG), &r, COSMOFS_CHECK_REPAIR) == 0);
    CHECK(r.counter_wrong.repaired == 2);
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0 && r.clean);
    check_teardown(bd);

    kinfo("selftest: cosmofs-check-faults: seven manufactured faults, each found by name, four repaired and three refused");
    return true;
}

/*
 * The control for the whole snapshot step: a snapshot holds blocks the
 * live tree has released, and those blocks are allocated and unreachable
 * from the live tree. A checker that did not walk snapshots would call
 * every one of them a leak.
 */
bool selftest_cosmofs_check_snapshot(const char **reason)
{
    struct blkdev *bd = NULL;
    struct cosmofs_check_report r;
    CHECK(check_fixture(&bd, reason));

    CHECK(write_file(ENG "/held", "the snapshot's copy", 19));
    CHECK(vfs_sync() == 0);
    CHECK(vfs_mkdir(NULL, ENG "/.snapshots/keep", 0755) == 0);
    /* Rewrite it: the old blocks are now the snapshot's alone. */
    CHECK(write_file(ENG "/held", "the live copy, different length", 31));
    CHECK(vfs_sync() == 0);

    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0);
    CHECK(r.snapshots_seen == 1);
    CHECK(r.clean);                       /* the held blocks are not leaks */
    CHECK(r.dup.count == 0);              /* nor cross-links: sharing is the point */
    CHECK(read_matches(ENG "/.snapshots/keep/held", "the snapshot's copy", 19));

    /* And after the snapshot goes, still clean: the blocks it held are
     * free or the live tree's, and nothing is left claiming them. */
    CHECK(vfs_rmdir(NULL, ENG "/.snapshots/keep") == 0);
    CHECK(vfs_sync() == 0);
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0);
    CHECK(r.snapshots_seen == 0 && r.clean);

    check_teardown(bd);
    kinfo("selftest: cosmofs-check-snapshot: a snapshot's held blocks are neither leaks nor cross-links");
    return true;
}

/*
 * The leak the design admits by omission, as a test: a file unlinked
 * while a handle still holds it keeps its blocks until the last
 * reference goes (cfs_evict frees them), and there is no on-disk record
 * of that intention. Interrupt it and the inode is an orphan: no name
 * reaches it, its blocks are allocated, and nothing ever reconsiders
 * them. This is what the structural check was built to find.
 */
bool selftest_cosmofs_check_orphan_crash(const char **reason)
{
    struct blkdev *bd = NULL;
    struct cosmofs_check_report r;
    CHECK(check_fixture(&bd, reason));

    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0 && r.clean);
    uint64_t free_before = r.counted_free;

    /* Open it, unlink it, and commit while the handle is still open: on
     * disk the inode now has no name and keeps its blocks. */
    struct file *f;
    CHECK(write_file(ENG "/doomed", "still open when it went", 23));
    CHECK(vfs_open(NULL, ENG "/doomed", COSMO_O_RDONLY, 0, &f) == 0);
    CHECK(vfs_unlink(NULL, ENG "/doomed") == 0);
    CHECK(vfs_sync() == 0);

    /* The crash: the handle never closes, so cfs_evict never runs. The
     * force unmount drops the open transaction, as a crash before the
     * root write would. */
    struct cosmofs_check_report during;
    CHECK(cosmofs_check(mount_of(ENG), &during, 0) == 0);
    CHECK(during.orphan.count == 1);   /* already an orphan on disk, handle or no handle */
    file_put(f);

    CHECK(vfs_umount2(ENG, VFS_UMOUNT_FORCE) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);

    /* After the remount nothing holds the inode, and nothing frees it
     * either: the space is gone until somebody checks. */
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0);
    CHECK(r.orphan.count == 1);
    CHECK(r.counted_free < free_before);
    uint64_t lost = r.orphan.name[0];

    CHECK(cosmofs_check(mount_of(ENG), &r, COSMOFS_CHECK_REPAIR) == 0);
    CHECK(r.orphan.repaired == 1);
    CHECK(vfs_sync() == 0);
    CHECK(vfs_sync() == 0);   /* the deferred frees land on the commit after the repair's */
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0);
    CHECK(r.clean);
    CHECK(r.counted_free == free_before);   /* every block the file held came back */

    check_teardown(bd);
    kinfo("selftest: cosmofs-check-orphan-crash: inode %llu survived its unlink with its blocks, and the check reclaimed them",
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

    CHECK(check_fixture(&bd, reason));
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0 && r.clean);
    uint64_t free_before = r.counted_free;

    const unsigned many = 12;                  /* more than CFS_CHECK_NAMES */
    for (unsigned i = 0; i < many; i++)
        CHECK(cosmofs_test_corrupt(mount_of(ENG), COSMOFS_CORRUPT_ORPHAN, 0, &what) == 0);

    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0);
    CHECK(r.orphan.count == many);
    CHECK(r.orphan.named == CFS_CHECK_NAMES);  /* eight named, twelve found */
    /* Each orphan holds one block, and the inode map may have grown to
     * hold the slots, so the free count is lower by at least the twelve. */
    CHECK(r.counted_free <= free_before - many);
    uint64_t free_with_orphans = r.counted_free;

    /* One pass, every one of them. */
    CHECK(cosmofs_check(mount_of(ENG), &r, COSMOFS_CHECK_REPAIR) == 0);
    CHECK(!r.repair_refused);
    CHECK(r.orphan.repaired == many);
    CHECK(vfs_sync() == 0);
    CHECK(vfs_sync() == 0);                    /* the deferred frees land next commit */
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0);
    /* Nothing left: the point of the test. A repair that worked from the
     * eight names would report eight repaired and leave four behind, and
     * this pass would find them. */
    CHECK(r.clean);
    CHECK(r.orphan.count == 0);
    CHECK(r.counted_free > free_with_orphans);   /* and the space came back */
    check_teardown(bd);

    kinfo("selftest: cosmofs-check-many-orphans: %u orphans, %u named, all repaired in one pass",
          many, (unsigned)CFS_CHECK_NAMES);
    return true;
}

bool selftest_cosmofs_check_slot_identity(const char **reason)
{
    struct cosmofs_check_report r;
    struct blkdev *bd = NULL;
    uint64_t what = 0;

    CHECK(check_fixture(&bd, reason));
    uint64_t file_ino = check_file_ino();
    CHECK(file_ino != 0);
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0 && r.clean);
    uint64_t free_before = r.counted_free;

    CHECK(cosmofs_test_corrupt(mount_of(ENG), COSMOFS_CORRUPT_INO_SLOT, file_ino, &what) == 0);
    CHECK(what == file_ino);

    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0);
    CHECK(!r.clean);
    CHECK(r.dir_bad.count >= 1);
    CHECK(r.dir_bad.name[0] == file_ino);      /* named by position, not by the field */
    CHECK(r.partial);                          /* the accounting is incomplete, and says so */

    /* Repair refuses: the inode's blocks are unclaimed only because the
     * pass would not trust the structure holding them. */
    CHECK(cosmofs_check(mount_of(ENG), &r, COSMOFS_CHECK_REPAIR) == 0);
    CHECK(r.repair_refused);
    CHECK(r.alloc_not_seen.repaired == 0 && r.orphan.repaired == 0);
    CHECK(vfs_sync() == 0);
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0);
    CHECK(r.counted_free == free_before);      /* not one block given away */
    check_teardown(bd);

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
    CHECK(check_fixture(&bd, reason));
    uint64_t id = mount_of(ENG)->id;
    CHECK(id != 0);

    struct file *f = NULL;
    CHECK(vfs_open(NULL, "/dev/fsctl", COSMO_O_RDWR, 0, &f) == 0 && f != NULL);
    size_t cap = sizeof(struct cosmo_fsctl_result) + sizeof(struct cosmo_fsctl_check);
    uint8_t *buf = kmalloc(cap, KMEM_ZERO);
    CHECK(buf != NULL);
    struct cosmo_fsctl_result *h = (struct cosmo_fsctl_result *)buf;
    struct cosmo_fsctl_check *c = (struct cosmo_fsctl_check *)(buf + sizeof(*h));

    struct cosmo_fsctl cmd = { .version = COSMO_FSCTL_VERSION, .op = COSMO_FSCTL_CHECK,
                               .flags = 0, .mount_id = id };

    /* Clean, through the device, and the same numbers the pass reports. */
    CHECK(file_write(f, &cmd, sizeof(cmd)) == (int64_t)sizeof(cmd));
    CHECK(file_read(f, buf, cap) == (int64_t)cap);
    CHECK(h->kind == COSMO_FSCTL_CHECK && h->count == 1);
    CHECK(h->bytes == sizeof(struct cosmo_fsctl_check));
    CHECK(c->nclasses == COSMO_FSCTL_CLASSES);
    CHECK((c->flags & COSMO_FSCTL_R_CLEAN) != 0);
    CHECK((c->flags & COSMO_FSCTL_R_PARTIAL) == 0);
    CHECK(cosmofs_check(mount_of(ENG), &direct, 0) == 0);
    CHECK(c->blocks_seen == direct.blocks_seen);
    CHECK(c->counted_free == direct.counted_free);
    CHECK(c->inodes_seen == direct.inodes_seen);
    uint64_t free_before = c->counted_free;

    /* A leak, found by number through the device. */
    uint64_t leaked = 0;
    CHECK(cosmofs_test_corrupt(mount_of(ENG), COSMOFS_CORRUPT_LEAK, 0, &leaked) == 0);
    CHECK(file_write(f, &cmd, sizeof(cmd)) == (int64_t)sizeof(cmd));
    CHECK(file_read(f, buf, cap) == (int64_t)cap);
    CHECK((c->flags & COSMO_FSCTL_R_CLEAN) == 0);
    CHECK(c->class[0].count == 1);                  /* index 0 is alloc_not_seen, and that is ABI */
    CHECK(c->class[0].named == 1 && c->class[0].name[0] == leaked);
    CHECK(c->counted_free == free_before - 1);
    /* A finding is not an error: the write succeeded and said so. */

    /* And repaired through the device, which is the half that mutates. */
    cmd.flags = COSMO_FSCTL_F_REPAIR;
    CHECK(file_write(f, &cmd, sizeof(cmd)) == (int64_t)sizeof(cmd));
    CHECK(file_read(f, buf, cap) == (int64_t)cap);
    CHECK(c->class[0].repaired == 1);
    CHECK((c->flags & COSMO_FSCTL_R_REPAIR_REFUSED) == 0);
    cmd.flags = 0;
    CHECK(vfs_sync() == 0);
    CHECK(file_write(f, &cmd, sizeof(cmd)) == (int64_t)sizeof(cmd));
    CHECK(file_read(f, buf, cap) == (int64_t)cap);
    CHECK((c->flags & COSMO_FSCTL_R_CLEAN) != 0);
    CHECK(c->counted_free == free_before);

    /* A scrub through the same channel, against the same name. */
    struct cosmo_fsctl scmd = { .version = COSMO_FSCTL_VERSION, .op = COSMO_FSCTL_SCRUB,
                                .flags = 0, .mount_id = id };
    size_t scap = sizeof(struct cosmo_fsctl_result) + sizeof(struct cosmo_fsctl_scrub);
    CHECK(file_write(f, &scmd, sizeof(scmd)) == (int64_t)sizeof(scmd));
    CHECK(file_read(f, buf, scap) == (int64_t)scap);
    struct cosmo_fsctl_scrub *sc = (struct cosmo_fsctl_scrub *)(buf + sizeof(*h));
    CHECK(h->kind == COSMO_FSCTL_SCRUB);
    CHECK(sc->blocks_read > 0 && sc->unrecoverable == 0);

    /* The refusals. A ramfs has neither pass; a name nothing holds is
     * not a mount this namespace has. Both answer before any lock. */
    struct mount *rootm = NULL;
    struct vnode *rv = NULL;
    CHECK(vfs_lookup(NULL, "/tmp", &rv) == 0);
    rootm = rv->mnt;
    vnode_put(rv);
    cmd.mount_id = rootm->id;
    CHECK(file_write(f, &cmd, sizeof(cmd)) == -EOPNOTSUPP);
    CHECK(file_read(f, buf, cap) == 0);             /* a failed command leaves no result */

    cmd.mount_id = ~0ull;
    CHECK(file_write(f, &cmd, sizeof(cmd)) == -ENOENT);
    cmd.mount_id = id;
    cmd.version = COSMO_FSCTL_VERSION + 1;
    CHECK(file_write(f, &cmd, sizeof(cmd)) == -EINVAL);
    cmd.version = COSMO_FSCTL_VERSION;
    CHECK(file_write(f, &cmd, sizeof(cmd) - 1) == -EINVAL);   /* whole, at its exact size */

    /*
     * Every bit of `flags` must mean something to the op it is sent
     * with. A bit nobody defined is a caller's mistake now and an
     * ambiguity later, when a version gives it a meaning and an old
     * writer turns out to have been setting it.
     */
    cmd.flags = 1u << 31;
    CHECK(file_write(f, &cmd, sizeof(cmd)) == -EINVAL);
    scmd.flags = COSMO_FSCTL_F_REPAIR;          /* CHECK-only, on a SCRUB */
    CHECK(file_write(f, &scmd, sizeof(scmd)) == -EINVAL);
    struct cosmo_fsctl lcmd = { .version = COSMO_FSCTL_VERSION, .op = COSMO_FSCTL_LIST,
                                .flags = COSMO_FSCTL_F_REPAIR };
    CHECK(file_write(f, &lcmd, sizeof(lcmd)) == -EINVAL);   /* LIST takes none */
    cmd.flags = 0;
    scmd.flags = 0;

    kfree(buf);
    file_put(f);
    check_teardown(bd);
    kinfo("selftest: fsctl-check: a leak found and repaired through /dev/fsctl against mount %llu",
          (unsigned long long)id);
    return true;
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
    cosmofs_test_set_writeback(mount_of(ENG), false);

    CHECK(vfs_mkdir(NULL, ENG "/d", 0755) == 0);
    CHECK(write_file(ENG "/d/x", "held by a name in the block about to go", 39));
    CHECK(write_file(ENG "/keep", "reached another way", 19));
    CHECK(vfs_sync() == 0);

    struct cosmofs_check_report rep;
    CHECK(check_is_clean(reason, &rep) && rep.clean);
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
    CHECK(corrupt_block(bd, (uint64_t)dir_blk, 40));
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);

    /* The pass finishes and says so: an answer, and a warning that it is
     * not the whole answer. */
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0);
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
    CHECK(cosmofs_check(mount_of(ENG), &rep, COSMOFS_CHECK_REPAIR) == 0);
    CHECK(rep.repair_refused);
    CHECK(rep.alloc_not_seen.repaired == 0);
    CHECK(rep.orphan.repaired == 0);
    CHECK(rep.counter_wrong.repaired == 0);
    CHECK(vfs_sync() == 0);
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0);
    CHECK(rep.alloc_not_seen.count == lost_blocks);   /* nothing was given away */
    /* The file the broken block named is still whole: the block it lives
     * in was never freed, so the entry that survives elsewhere still
     * reaches it. */
    CHECK(read_matches(ENG "/keep", "reached another way", 19));

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
    cosmofs_test_set_writeback(mount_of(ENG), false);

    struct cosmofs_stats st0, st1;
    CHECK(cosmofs_stats(mount_of(ENG), &st0) == 0);

    CHECK(write_file(ENG "/file", "on-disk-bytes", 13));
    CHECK(vfs_symlink(NULL, ENG "/link", "file") == 0);
    CHECK(vfs_symlink(NULL, ENG "/abs", ENG "/file") == 0);

    struct cosmo_stat lst, fst;
    CHECK(vfs_lstat(NULL, ENG "/link", &lst) == 0 && lst.type == COSMO_DT_LNK && lst.size == 4);
    CHECK(vfs_stat(NULL, ENG "/link", &fst) == 0 && fst.type == COSMO_DT_REG && fst.size == 13);
    CHECK(read_matches(ENG "/link", "on-disk-bytes", 13));
    CHECK(read_matches(ENG "/abs", "on-disk-bytes", 13));

    /* It survives a remount with nothing cached: the bytes come off the
     * disk, not from a vnode this test left behind. */
    CHECK(vfs_sync() == 0);
    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    char buf[64];
    memset(buf, 'Z', sizeof(buf));
    int n = vfs_readlink(NULL, ENG "/link", buf, sizeof(buf));
    CHECK(n == 4 && memcmp(buf, "file", 4) == 0 && buf[4] == 'Z');
    CHECK(vfs_lstat(NULL, ENG "/link", &lst) == 0 && lst.type == COSMO_DT_LNK);
    CHECK(read_matches(ENG "/link", "on-disk-bytes", 13));

    /* One block per link, given back when it goes. */
    CHECK(cosmofs_stats(mount_of(ENG), &st1) == 0);
    CHECK(vfs_unlink(NULL, ENG "/link") == 0);
    CHECK(vfs_unlink(NULL, ENG "/abs") == 0);
    CHECK(vfs_sync() == 0);
    CHECK(cosmofs_stats(mount_of(ENG), &st0) == 0);
    CHECK(st0.free_blocks > st1.free_blocks);
    CHECK(read_matches(ENG "/file", "on-disk-bytes", 13));   /* the target stayed */

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
    cosmofs_test_set_writeback(mount_of(ENG), false);
    CHECK(write_file(ENG "/eight", "older", 5));
    CHECK(vfs_sync() == 0);
    CHECK(read_matches(ENG "/eight", "older", 5));
    struct cosmofs_check_report rep;
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0);
    CHECK(rep.clean);
    CHECK(vfs_umount(ENG) == 0);

    /* And again after a remount: nothing was read from a field it does
     * not have, and nothing was written into one. */
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    CHECK(read_matches(ENG "/eight", "older", 5));
    struct cosmofs_stats st8;
    CHECK(cosmofs_stats(mount_of(ENG), &st8) == 0);
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
    CHECK(cosmofs_stats(mount_of(ENG), &st8) == 0);
    CHECK(st8.version == 8);
    CHECK(st8.free_root == 0);        /* not 0x5a5a5a5a: below version 9 the word is not a root */
    CHECK(read_matches(ENG "/eight", "older", 5));
    CHECK(vfs_umount(ENG) == 0);

    /* A version-9 filesystem carries the field, and an idle one has
     * nothing recorded in it. */
    struct blkdev *bd9 = ramblk_create(512);
    CHECK(bd9 != NULL);
    CHECK(cosmofs_format(bd9) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd9, 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    struct cosmofs_stats st;
    CHECK(cosmofs_stats(mount_of(ENG), &st) == 0);
    CHECK(st.version == CFS_VERSION);
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
    cosmofs_test_set_writeback(mount_of(ENG), false);

    /* Something to free, so the record is not empty. */
    CHECK(write_file(ENG "/a", "one", 3));
    CHECK(vfs_sync() == 0);
    CHECK(write_file(ENG "/a", "one again, longer", 17));
    CHECK(vfs_unlink(NULL, ENG "/a") == 0);
    CHECK(vfs_sync() == 0);

    struct cosmofs_stats st;
    CHECK(cosmofs_stats(mount_of(ENG), &st) == 0);
    CHECK(st.free_root != 0);                  /* there is a record */

    /* Every block of it is allocated in the bitmap this root published,
     * and the check agrees: no leak, and nothing reachable-but-free. */
    struct cosmofs_check_report rep;
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0);
    CHECK(rep.seen_not_alloc.count == 0);      /* the dangerous direction */
    CHECK(rep.alloc_not_seen.count == 0);
    CHECK(rep.clean);

    /* And across a remount, where the bitmap on disk is the only word. */
    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0);
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
    cosmofs_test_set_writeback(mount_of(ENG), false);

    /* Settle, then measure: the first commits grow the tree. */
    CHECK(write_file(ENG "/churn", "x", 1));
    CHECK(vfs_sync() == 0);
    CHECK(vfs_sync() == 0);
    struct cosmofs_stats before;
    CHECK(cosmofs_stats(mount_of(ENG), &before) == 0);

    /* A hundred commits that change the same one block. */
    for (unsigned i = 0; i < 100; i++) {
        CHECK(write_file(ENG "/churn", "y", 1));
        CHECK(vfs_sync() == 0);
    }
    struct cosmofs_stats after;
    CHECK(cosmofs_stats(mount_of(ENG), &after) == 0);

    /*
     * The count may move by the transaction in flight, but not by a
     * hundred commits' worth of records. A leak of one block per commit
     * would be a hundred blocks on a 512-block filesystem.
     */
    CHECK(after.free_blocks + 8 >= before.free_blocks);
    CHECK(after.free_root != 0);

    struct cosmofs_check_report rep;
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0);
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
    cosmofs_test_set_writeback(mount_of(ENG), false);
    CHECK(vfs_sync() == 0);

    /* Settle first: the count to come back to is the one after the
     * filesystem has stopped growing. */
    CHECK(write_file(ENG "/settle", "s", 1));
    CHECK(vfs_unlink(NULL, ENG "/settle") == 0);
    CHECK(vfs_sync() == 0);
    CHECK(vfs_sync() == 0);
    struct cosmofs_stats before;
    CHECK(cosmofs_stats(mount_of(ENG), &before) == 0);

    /* A file, and then no file. */
    static const char big[4096] = { 0 };
    CHECK(write_file(ENG "/leakme", big, sizeof(big)));
    CHECK(vfs_sync() == 0);
    CHECK(vfs_unlink(NULL, ENG "/leakme") == 0);
    CHECK(vfs_umount(ENG) == 0);          /* the unmount's commit frees them */

    /* Remount: every block is back, and the filesystem adds up. */
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    struct cosmofs_stats after;
    CHECK(cosmofs_stats(mount_of(ENG), &after) == 0);
    CHECK(after.free_blocks == before.free_blocks);
    struct cosmofs_check_report rep;
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0);
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
    cosmofs_test_set_writeback(mount_of(ENG), false);

    /* Something for the snapshot to hold. */
    CHECK(write_file(ENG "/held", "the snapshot's copy", 19));
    CHECK(vfs_sync() == 0);
    CHECK(vfs_mkdir(NULL, ENG "/.snapshots/keep", 0755) == 0);

    struct cosmofs_stats before;
    CHECK(cosmofs_stats(mount_of(ENG), &before) == 0);

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
    CHECK(cosmofs_stats(mount_of(ENG), &after) == 0);

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
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0);
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
    CHECK(rep.alloc_not_seen.count <= 4);   /* the deadlist's, not the record's */

    /* And the snapshot still reads, which is what the blocks were for. */
    CHECK(read_matches(ENG "/.snapshots/keep/held", "the snapshot's copy", 19));

    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_rmdir(NULL, ENG) == 0);
    ramblk_destroy(bd);
    kinfo("selftest: cosmofs-freelog-snapshot: a snapshot's blocks survive the record and the remount (%llu free either side)",
          (unsigned long long)after.free_blocks);
    return true;
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
    cosmofs_test_set_writeback(mount_of(ENG), false);

    /* Make a record: a file written, deleted, and the unmount's commit
     * the one that frees. */
    static const char big[4096] = { 0 };
    CHECK(write_file(ENG "/gone", big, sizeof(big)));
    CHECK(vfs_sync() == 0);
    CHECK(vfs_unlink(NULL, ENG "/gone") == 0);
    CHECK(vfs_umount(ENG) == 0);

    /* First mount: the replay gives the blocks back. */
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    struct cosmofs_stats first;
    CHECK(cosmofs_stats(mount_of(ENG), &first) == 0);
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
    CHECK(cosmofs_stats(mount_of(ENG), &second) == 0);
    CHECK(second.free_blocks == first.free_blocks);
    CHECK(second.free_root == first.free_root);
    struct cosmofs_check_report rep;
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0);
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
    cosmofs_test_set_writeback(mount_of(ENG), false);
    static const char big[4096] = { 0 };
    CHECK(write_file(ENG "/first", big, sizeof(big)));
    CHECK(vfs_sync() == 0);
    CHECK(vfs_unlink(NULL, ENG "/first") == 0);
    CHECK(vfs_umount(ENG) == 0);

    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    struct cosmofs_stats after_replay;
    CHECK(cosmofs_stats(mount_of(ENG), &after_replay) == 0);

    /* Take the space back out, in this session, and make it durable. */
    CHECK(write_file(ENG "/second", big, sizeof(big)));
    CHECK(vfs_sync() == 0);
    CHECK(vfs_umount(ENG) == 0);

    /* It is in use, the filesystem adds up, and the file reads. */
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    CHECK(read_matches(ENG "/second", big, sizeof(big)));
    struct cosmofs_check_report rep;
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0);
    CHECK(rep.clean);
    CHECK(rep.seen_not_alloc.count == 0);   /* nothing live is marked free */
    struct cosmofs_stats now;
    CHECK(cosmofs_stats(mount_of(ENG), &now) == 0);
    CHECK(now.free_blocks < after_replay.free_blocks);   /* the file took the space */

    CHECK(vfs_unlink(NULL, ENG "/second") == 0);
    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    struct cosmofs_stats back;
    CHECK(cosmofs_stats(mount_of(ENG), &back) == 0);
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
 * What is exact is that the deadlist stops growing. It legitimately
 * grows at first -- the root directory block and the first imap and
 * bitmap blocks predate the snapshot, and rewriting anything frees them
 * -- so the churn runs in a directory created after the snapshot, and
 * once that has settled nothing it frees is a block the snapshot's tree
 * names. Any later entry got there by asking the wrong question about a
 * record.
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
    cosmofs_test_set_writeback(mount_of(ENG), false);

    /* A record exists, and *then* the snapshot is taken: its bitmap
     * marks that record's blocks allocated, which is the difficulty. */
    CHECK(write_file(ENG "/held", "the snapshot's copy", 19));
    CHECK(vfs_sync() == 0);
    struct cosmofs_stats rec;
    CHECK(cosmofs_stats(mount_of(ENG), &rec) == 0);
    CHECK(rec.free_root != 0);
    CHECK(vfs_mkdir(NULL, ENG "/.snapshots/keep", 0755) == 0);

    /* Everything the churn touches is created after the snapshot, and
     * then settled, so the blocks these commits free are all post-
     * snapshot ones: the only thing they release that a snapshot's
     * bitmap remembers is one record after another. */
    CHECK(vfs_mkdir(NULL, ENG "/post", 0755) == 0);
    for (unsigned i = 0; i < 3; i++) {
        CHECK(write_file(ENG "/post/churn", "x", 1));
        CHECK(vfs_sync() == 0);
    }
    struct cosmofs_stats before;
    CHECK(cosmofs_stats(mount_of(ENG), &before) == 0);
    uint64_t dead_before = cosmofs_test_deadlist_len(mount_of(ENG));

    for (unsigned i = 0; i < 30; i++) {
        CHECK(write_file(ENG "/post/churn", "y", 1));
        CHECK(vfs_sync() == 0);
    }
    struct cosmofs_stats after;
    CHECK(cosmofs_stats(mount_of(ENG), &after) == 0);
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
    CHECK(cosmofs_test_deadlist_len(mount_of(ENG)) == dead_before);

    /* And the snapshot is intact: nothing this exception frees was its. */
    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    struct cosmofs_check_report rep;
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0);
    CHECK(rep.seen_not_alloc.count == 0);
    CHECK(rep.snapshots_seen == 1);
    CHECK(read_matches(ENG "/.snapshots/keep/held", "the snapshot's copy", 19));

    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_rmdir(NULL, ENG) == 0);
    ramblk_destroy(bd);
    kinfo("selftest: cosmofs-freelog-not-held: 30 records superseded under a snapshot, %llu free before and %llu after, %llu on the deadlist either side, and the snapshot still reads",
          (unsigned long long)before.free_blocks, (unsigned long long)after.free_blocks,
          (unsigned long long)dead_before);
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
    cosmofs_test_set_writeback(mount_of(ENG), false);

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
            CHECK(cosmofs_stats(mount_of(ENG), &before) == 0);
        CHECK(write_wide_file(ENG "/wide", 600));
        CHECK(vfs_sync() == 0);
        if (round == 1) {
            CHECK(cosmofs_stats(mount_of(ENG), &full) == 0);
            /* More than one record block holds. */
            CHECK(before.free_blocks - full.free_blocks > CFS_DEAD_PER_BLOCK);
        }
        CHECK(vfs_unlink(NULL, ENG "/wide") == 0);
        CHECK(vfs_umount(ENG) == 0);          /* the unmount's commit frees them */
        CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
        cosmofs_test_set_writeback(mount_of(ENG), false);
    }

    /* Every block back, across a chain of record blocks. */
    struct cosmofs_stats after;
    CHECK(cosmofs_stats(mount_of(ENG), &after) == 0);
    CHECK(after.free_blocks == before.free_blocks);
    struct cosmofs_check_report rep;
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0);
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
    cosmofs_test_set_writeback(mount_of(ENG), false);

    /* A version-7 filesystem mounts and works, and refuses a link. */
    CHECK(write_file(ENG "/file", "seven", 5));
    CHECK(vfs_symlink(NULL, ENG "/link", "file") == -EOPNOTSUPP);
    struct cosmo_stat st;
    CHECK(vfs_lstat(NULL, ENG "/link", &st) == -ENOENT);
    CHECK(read_matches(ENG "/file", "seven", 5));

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
    cosmofs_test_set_writeback(mount_of(ENG), false);
    struct cosmofs_stats st;
    CHECK(cosmofs_stats(mount_of(ENG), &st) == 0);
    CHECK(st.members == 1 && st.total_blocks == 512);
    /* One member's worth of blocks, and one less metadata block than a
     * version-4 disk: there is no member table. */
    CHECK(st.free_blocks == 512 - 7);

    CHECK(write_file(ENG "/old", "written by a new kernel", 23));
    CHECK(vfs_mkdir(NULL, ENG "/dir", 0755) == 0);
    CHECK(write_file(ENG "/dir/deep", "deep", 4));
    CHECK(vfs_sync() == 0);
    CHECK(vfs_umount(ENG) == 0);

    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    CHECK(read_matches(ENG "/old", "written by a new kernel", 23));
    CHECK(read_matches(ENG "/dir/deep", "deep", 4));
    /* Snapshots work on it too: a version-3 snapshot records the single
     * allocation root, and that is what cfs_snapshot_references reads. */
    CHECK(vfs_mkdir(NULL, ENG "/.snapshots/v3", 0755) == 0);
    CHECK(write_file(ENG "/old", "changed", 7));
    CHECK(read_matches(ENG "/.snapshots/v3/old", "written by a new kernel", 23));
    CHECK(vfs_rmdir(NULL, ENG "/.snapshots/v3") == 0);
    CHECK(read_matches(ENG "/old", "changed", 7));

    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_rmdir(NULL, ENG) == 0);
    ramblk_destroy(bd);
    kinfo("selftest: cosmofs-v3: a version-3 disk still mounts and writes");
    return true;
}

/* The first `len` bytes of a file, whatever its length. */
static bool read_matches_prefix(const char *path, const void *data, size_t len)
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
    cosmofs_test_set_writeback(mount_of(ENG), false);

    /* The first commit touches member 0's metadata while member 1 is
     * emptier, so the allocation index's copy-on-write lands on member 1
     * and dirties a bitmap chunk that the reservation pass has already
     * been over. Every dirty chunk must still have a reserved
     * destination: writing one to DVA 0 would land on member 0's
     * superblock. */
    CHECK(write_file(ENG "/first", "one", 3));
    CHECK(vfs_sync() == 0);
    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd[0], 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    CHECK(read_matches(ENG "/first", "one", 3));

    /* Both members' blocks are in the pool's total. */
    struct cosmofs_stats st;
    CHECK(cosmofs_stats(mount_of(ENG), &st) == 0);
    CHECK(st.total_blocks == 2048);

    /* Enough data that a one-member pool of this size could not hold it:
     * the allocator has to reach the second member. */
    /* Sixteen *different* pages per file: the same page written sixteen
     * times is one of the most compressible things there is, and this
     * test is about where blocks land, not how few of them there are. */
    static uint8_t buf[16 * 4096];
    fill_incompressible(buf, sizeof(buf), 0x9e37);
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
    CHECK(cosmofs_stats(mount_of(ENG), &st) == 0);
    CHECK(st.members == 2);
    CHECK(st.free_blocks < 2048 - 24 * 16);
    /* Both members carry blocks -- the allocator prefers whichever has
     * more room, so neither is left untouched -- and the pool's free
     * count is exactly theirs. */
    uint64_t f0 = cosmofs_test_member_free(mount_of(ENG), 0);
    uint64_t f1 = cosmofs_test_member_free(mount_of(ENG), 1);
    CHECK(f0 < 1024 && f1 < 1024);
    CHECK(f0 + f1 == st.free_blocks);
    CHECK(cosmofs_test_member_free(mount_of(ENG), 2) == UINT64_MAX);

    /* A remount is handed member 0 only and has to find the other one by
     * its label; the data reads back through both. */
    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd[0], 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    CHECK(cosmofs_stats(mount_of(ENG), &st) == 0 && st.total_blocks == 2048);
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
