/*
 * cosmofstest.c - Self-tests for the storage pool and cosmofs on the
 * scratch virtio disk (vda). They format the disk, so they run only
 * when it exists, and they leave a filesystem behind for init's
 * user-mode test: /hello.txt and /dir/nested.txt.
 */

#include <kernel/blk.h>
#include <kernel/cosmofs.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/page.h>
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
    CHECK(file_sync(f) == 0);
    file_put(f);
    CHECK(read_matches("/mnt/big.bin", buf, big));

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
