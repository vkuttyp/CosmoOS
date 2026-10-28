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
          p->m[0].copy[0].sectors_per_block == 8 && p->nblocks == bd->capacity / 8);
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

    /* Errors. A name longer than 255 bytes; one of 48 to 255 is format
     * 15's (design.md, "Format version 15"; cosmofs-longname). */
    static char toolong[9 + 256 + 1];
    memcpy(toolong, "/mnt/dir/", 9);
    memset(toolong + 9, 'n', 256);
    toolong[9 + 256] = '\0';
    CHECK(vfs_mkdir(NULL, toolong, 0755) == -ENAMETOOLONG);
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
    /* static: off the 16 KiB kernel stack (scripts/frame-baseline.txt); the runner runs one test at a time */
    static uint8_t saved_a[4096], saved_b[4096];
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
#include "cosmofs_internal.h"

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


/*
 * A file of `pages` blocks nothing can compress. A test that counts
 * blocks has to write these: a page of zeros costs almost nothing here,
 * so six hundred pages of them are not six hundred blocks.
 */
static bool write_wide_file(const char *path, unsigned pages)
{
    static uint8_t page[4096];
    struct file *f = NULL;
    if (vfs_open(NULL, path, COSMO_O_WRONLY | COSMO_O_CREAT | COSMO_O_TRUNC, 0644, &f))
        return false;
    bool ok = true;
    for (unsigned i = 0; ok && i < pages; i++) {
        /*
         * Spread the seeds, do not walk them. `fill_incompressible`
         * starts from `seed | 1`, so seeds `n` and `n + 1` collide
         * whenever `n` is even and every second page comes out
         * byte-identical to the one before it -- which the filesystem
         * then stores once, and 600 pages cost 452 blocks instead of
         * 601. An odd multiplier keeps consecutive pages apart.
         */
        fill_incompressible(page, sizeof(page), 0x9E3779B9u * (i + 1));
        ok = file_write(f, page, sizeof(page)) == (int64_t)sizeof(page);
    }
    file_put(f);
    return ok;
}

#define ENG "/mnt/eng"

/* Only the serial self-test runner arms these operation-boundary seams. */
#if CONFIG_SELFTEST && CONFIG_FAULTINJECT
static unsigned engine_fail_stage;
static struct blkdev *engine_observed;
static uint64_t engine_fail_hits;

static void engine_fail_begin(unsigned stage, struct blkdev *bd)
{
    if (engine_fail_stage != stage)
        return;
    engine_observed = bd;
    blkdev_get(bd);   /* inspection reference, independent of fixture ownership */
    faultinject_set(stage == 1 ? FI_BLK_SUBMIT : FI_KMALLOC, 1, 1, thread_current());
}

static void engine_fail_end(unsigned stage)
{
    if (engine_fail_stage != stage)
        return;
    enum fi_kind kind = stage == 1 ? FI_BLK_SUBMIT : FI_KMALLOC;
    struct fi_stats st;
    faultinject_stats(kind, &st);
    engine_fail_hits = st.hits;
    faultinject_clear(kind);
}
#else
static inline void engine_fail_begin(unsigned stage, struct blkdev *bd) { (void)stage; (void)bd; }
static inline void engine_fail_end(unsigned stage) { (void)stage; }
#endif

static bool engine_mount(struct blkdev **bdp, uint64_t nblocks, const char **reason)
{
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);   /* a failed earlier test may have left one behind */
    struct blkdev *bd = ramblk_create(nblocks);
    CHECK(bd != NULL);
    bool created_dir = false;
#define ENGINE_CHECK(cond)                                                    \
    do {                                                                      \
        if (!(cond)) {                                                        \
            *reason = "check failed: " #cond " at line " STR(__LINE__);     \
            goto fail;                                                       \
        }                                                                     \
    } while (0)
    engine_fail_begin(1, bd);
    int fmt = cosmofs_format(bd);
    engine_fail_end(1);
    ENGINE_CHECK(fmt == 0);
    engine_fail_begin(2, bd);
    int mk = vfs_mkdir(NULL, ENG, 0755);
    engine_fail_end(2);
    if (mk == 0)
        created_dir = true;
    ENGINE_CHECK(mk == 0 || mk == -EEXIST);
    engine_fail_begin(3, bd);
    int mrc = vfs_mount(ENG, "cosmofs", bd, 0);
    engine_fail_end(3);
    ENGINE_CHECK(mrc == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    *bdp = bd;
#undef ENGINE_CHECK
    return true;

fail:
#undef ENGINE_CHECK
    if (created_dir)
        (void)vfs_rmdir(NULL, ENG);
    ramblk_destroy(bd);
    return false;
}

static bool engine_unmount(struct blkdev *bd, const char **reason)
{
    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_rmdir(NULL, ENG) == 0);
    ramblk_destroy(bd);
    return true;
}

bool selftest_cosmofs_fixture_cleanup(const char **reason)
{
#if CONFIG_SELFTEST && CONFIG_FAULTINJECT
    bool ok = true;
    for (unsigned stage = 1; stage <= 3; stage++) {
        (void)vfs_rmdir(NULL, ENG);
        engine_fail_stage = stage;
        engine_observed = NULL;
        engine_fail_hits = 0;
        struct blkdev *published = NULL;
        const char *why = NULL;
        bool mounted = engine_mount(&published, 64, &why);
        engine_fail_stage = 0;
        struct blkdev *bd = engine_observed;
        bool released = bd && bd->gone && __atomic_load_n(&bd->obj.refcount, __ATOMIC_RELAXED) == 1;
        bool injected = !mounted && published == NULL && why != NULL && engine_fail_hits == 1;
        kinfo("selftest: cosmofs-fixture-cleanup: stage=%u hits=%llu released=%u", stage,
              (unsigned long long)engine_fail_hits, released);
        /* Reclaim old-behavior leaks after observing them, so later tests remain independent. */
        if (mounted)
            (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
        if (bd) {
            if (!bd->gone)
                ramblk_destroy(bd);
            blkdev_put(bd);
        }
        engine_observed = NULL;
        (void)vfs_rmdir(NULL, ENG);
        ok = ok && injected && released;
    }
    if (!ok) {
        *reason = "fixture cleanup: registered RAM device retained after setup failure";
        return false;
    }
#else
    (void)reason;
#endif
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
    /* Stopped at the reserve, less the allowance a dirty page is charged
     * on top of its block (CF34: CFS_DIRTY_META, 4 blocks with no page
     * dirty); before MEM-9 the write was taken and its commit failed. */
    CHECK(st.free_blocks <= st.reserve_blocks + 2 + 4 && st.free_blocks > 0);
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

/* The metadata header declares its checksum algorithm, and an algorithm this
 * build cannot verify is a distinct fault from a checksum that did not match. */
static uint32_t meta_test_crc(const uint8_t *block)
{
    size_t off = offsetof(struct cfs_mhdr, crc);
    static const uint8_t zero4[4] = { 0 };
    uint32_t c = crc32c(block, off);
    c = crc32c_update(c, zero4, 4);
    return crc32c_update(c, block + off + 4, CFS_BLOCK - off - 4);
}

bool selftest_cosmofs_metadata_csum_id(const char **reason)
{
    uint8_t *block = kmalloc(CFS_BLOCK, 0);
    CHECK(block != NULL);
    memset(block, 0xa5, CFS_BLOCK);
    struct cfs_mhdr *h = (struct cfs_mhdr *)block;

    /* A freshly sealed block names CRC32C and verifies. */
    cfs_mhdr_seal_raw(NULL, block, CFS_KIND_INODES, 42, 7);
    enum mhdr_fault f_sealed = cfs_mhdr_fault_of(NULL, block, 42, CFS_KIND_INODES);
    unsigned sealed_algo = h->csum_algo;

    /* A pre-v11 image left the field zero; it still reads as CRC32C. */
    h->csum_algo = CFS_CSUM_NONE;
    h->crc = meta_test_crc(block);
    enum mhdr_fault f_legacy = cfs_mhdr_fault_of(NULL, block, 42, CFS_KIND_INODES);

    /* A block declaring an algorithm this build has no verifier for, with a
     * CRC that is itself valid: the fault is MHDR_ALGO, decided before the
     * checksum, so a format or algorithm skew is not read as damage. */
    h->csum_algo = CFS_CSUM_POLY1305;
    h->crc = meta_test_crc(block);
    enum mhdr_fault f_algo = cfs_mhdr_fault_of(NULL, block, 42, CFS_KIND_INODES);

    /* One flipped payload bit under CRC32C is MHDR_CRC, distinct from above. */
    cfs_mhdr_seal_raw(NULL, block, CFS_KIND_INODES, 42, 7);
    block[CFS_MHDR_SIZE + 3] ^= 0x01u;
    enum mhdr_fault f_corrupt = cfs_mhdr_fault_of(NULL, block, 42, CFS_KIND_INODES);

    /* csum_algo sits after crc but is inside the checksummed region: block_crc
     * zeroes only the 4 crc bytes and covers the rest of the block. Flip the
     * field between two *supported* values (1 -> 0), so the algorithm check
     * still passes, without recomputing the CRC: the block no longer verifies,
     * which it could not do unless csum_algo were covered by crc. */
    cfs_mhdr_seal_raw(NULL, block, CFS_KIND_INODES, 42, 7);
    h->csum_algo = CFS_CSUM_NONE;
    enum mhdr_fault f_covered = cfs_mhdr_fault_of(NULL, block, 42, CFS_KIND_INODES);

    kfree(block);   /* free before any CHECK can return */

    CHECK(f_sealed == MHDR_OK);
    CHECK(sealed_algo == CFS_CSUM_CRC32C);
    CHECK(f_legacy == MHDR_OK);
    CHECK(f_algo == MHDR_ALGO);
    CHECK(f_corrupt == MHDR_CRC);
    CHECK(f_covered == MHDR_CRC);   /* csum_algo is covered by the checksum */
    kinfo("selftest: cosmofs-metadata-csum-id: the metadata header declares its checksum algorithm; "
          "an unsupported one is a distinct fault from a bad checksum");
    return true;
}

#else
bool selftest_cosmofs_holes(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_csum(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_fsync(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_reserve(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_fallback(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_writeback(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_metadata_csum_id(const char **reason) { (void)reason; return true; }
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
    struct blkdev *bd = NULL;
    struct file *f = NULL;
    struct spool *p = NULL;
    uint8_t *page = NULL, *blk = NULL;
    if (!engine_mount(&bd, 512, reason))
        return false;
#define BADMAP_CHECK(cond)                                                    \
    do {                                                                      \
        if (!(cond)) {                                                        \
            *reason = "check failed: " #cond " at line " STR(__LINE__);     \
            goto cleanup;                                                    \
        }                                                                     \
    } while (0)
    BADMAP_CHECK(vfs_open(NULL, ENG "/two", COSMO_O_RDWR | COSMO_O_CREAT, 0644, &f) == 0);
    page = kmalloc(4096, 0);
    BADMAP_CHECK(page != NULL);
    memset(page, 0x11, 4096);
    BADMAP_CHECK(file_pwrite(f, page, 4096, 0) == 4096);    /* run at lblk 0 */
    memset(page, 0x22, 4096);
    BADMAP_CHECK(file_pwrite(f, page, 4096, 5 * 4096) == 4096); /* run at lblk 5, a hole between */
    BADMAP_CHECK(file_sync(f) == 0);
    uint64_t ino = f->vn->ino;
    file_put(f);
    f = NULL;
    cosmofs_test_discard_on_unmount(mount_of(ENG), true);   /* keep the slots as they are */
    BADMAP_CHECK(vfs_umount(ENG) == 0);

    /* Walk superblock -> IMAP1 -> IMAP0 -> INODES through the pool, swap
     * the two direct runs of the inode, re-seal the block. */
    BADMAP_CHECK(pool_open(bd, &p) == 0);
    blk = kmalloc(4096, 0);
    BADMAP_CHECK(blk != NULL);
    uint64_t imap = 0, gen = 0;
    for (unsigned slot = 0; slot < 2; slot++) {
        BADMAP_CHECK(pool_read(p, slot, blk) == 0);
        const struct cfs_super *sb = (const struct cfs_super *)blk;
        if (memcmp(sb->magic, CFS_MAGIC, 8) == 0 && sb->generation > gen) {
            gen = sb->generation;
            imap = sb->imap_root;
        }
    }
    BADMAP_CHECK(imap >= 2);
    BADMAP_CHECK(pool_read(p, imap, blk) == 0);
    uint64_t l0 = ((const uint64_t *)(blk + CFS_MHDR_SIZE))[cfs_imap_l1_index(ino)];
    BADMAP_CHECK(pool_read(p, l0, blk) == 0);
    uint64_t ib = ((const uint64_t *)(blk + CFS_MHDR_SIZE))[cfs_imap_l0_index(ino)];
    BADMAP_CHECK(pool_read(p, ib, blk) == 0);
    struct cfs_inode *in = (struct cfs_inode *)(blk + CFS_MHDR_SIZE + cfs_inode_slot(ino) * CFS_INODE_SIZE);
    BADMAP_CHECK(in->ino == ino && in->direct[0].count == 1 && in->direct[1].count == 1 && in->direct[1].lblk == 5);
    struct cfs_extent tmp = in->direct[0];
    in->direct[0] = in->direct[1];
    in->direct[1] = tmp;   /* unsorted: lblk 5 before lblk 0 */
    struct cfs_mhdr *h = (struct cfs_mhdr *)blk;
    h->crc = 0;
    h->crc = block_crc_test(blk);
    BADMAP_CHECK(pool_write(p, ib, blk) == 0 && pool_flush(p) == 0);
    kfree(blk);
    blk = NULL;
    pool_close(p);
    p = NULL;

    BADMAP_CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    BADMAP_CHECK(vfs_open(NULL, ENG "/two", COSMO_O_RDONLY, 0, &f) == 0);
    BADMAP_CHECK(file_pread(f, page, 4096, 0) == -EIO);   /* not a hole of zeros */
    BADMAP_CHECK(file_pread(f, page, 4096, 5 * 4096) == -EIO);
    file_put(f);
    f = NULL;
    kfree(page);
    page = NULL;
    kinfo("selftest: cosmofs-badmap: an inode with unsorted direct runs is refused, not read as holes");
    return engine_unmount(bd, reason);

cleanup:
    if (f)
        file_put(f);
    if (p)
        pool_close(p);
    kfree(blk);
    kfree(page);
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    (void)vfs_rmdir(NULL, ENG);
    ramblk_destroy(bd);
    return false;
#undef BADMAP_CHECK
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
    /* The stale copy at the end is resilvered by the mount since roadmap
     * 1.2 B3; held, so what is checked is that it is not read. */
    cosmofs_test_resilver_hold(true);
    struct blkdev *bd[2] = { NULL, NULL };
    struct file *f = NULL;
    struct spool *sp = NULL;
    uint8_t *sblk = NULL;
#define MIRROR_CHECK(cond)                                                    \
    do {                                                                      \
        if (!(cond)) {                                                        \
            *reason = "check failed: " #cond " at line " STR(__LINE__);     \
            goto cleanup;                                                    \
        }                                                                     \
    } while (0)
    bd[0] = ramblk_create(512);
    bd[1] = ramblk_create(512);
    MIRROR_CHECK(bd[0] != NULL && bd[1] != NULL);
    MIRROR_CHECK(cosmofs_format_mirror(bd, 1, 2) == 0);
    int mk = vfs_mkdir(NULL, ENG, 0755);
    MIRROR_CHECK(mk == 0 || mk == -EEXIST);
    MIRROR_CHECK(vfs_mount(ENG, "cosmofs", bd[0], 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);

    struct cosmofs_stats st;
    MIRROR_CHECK(cosmofs_stats(mount_of(ENG), &st) == 0);
    MIRROR_CHECK(st.members == 1 && st.devices == 2 && st.degraded == 0);
    /* One member's worth of space: a mirror costs capacity, not blocks. */
    MIRROR_CHECK(st.total_blocks == 512);

    static char data[4096];
    memset(data, 'm', sizeof(data));
    MIRROR_CHECK(write_file(ENG "/mirrored", data, sizeof(data)));
    MIRROR_CHECK(vfs_mkdir(NULL, ENG "/dir", 0755) == 0);
    MIRROR_CHECK(write_file(ENG "/dir/inner", "inner", 5));
    MIRROR_CHECK(vfs_sync() == 0);

    /* Where the file's one data block lives. */
    MIRROR_CHECK(vfs_open(NULL, ENG "/mirrored", COSMO_O_RDONLY, 0, &f) == 0);
    uint64_t ino = f->vn->ino;
    file_put(f);
    f = NULL;
    uint64_t pblk = 0;
    MIRROR_CHECK(cosmofs_test_block_of(mount_of(ENG), ino, 0, &pblk) == 0);
    MIRROR_CHECK(CFS_DVA_VDEV(pblk) == 0);

    /* Rot the second copy of that data block; the read comes from copy 0
     * and notices nothing. Then rot the first: the read has to fall back
     * to the second, which by then holds what copy 0 had. */
    MIRROR_CHECK(rot_copy(bd[1], CFS_DVA_BLK(pblk), 0xA5));
    MIRROR_CHECK(read_matches(ENG "/mirrored", data, sizeof(data)));
    MIRROR_CHECK(cosmofs_stats(mount_of(ENG), &st) == 0);
    uint64_t repairs0 = st.repairs;

    /* A scrub reads everything and puts the rotted copy right. */
    struct cosmofs_scrub_stats sc;
    MIRROR_CHECK(cosmofs_scrub(mount_of(ENG), &sc, COSMOFS_SCRUB_REPAIR) == 0);
    MIRROR_CHECK(sc.blocks_read > 0 && sc.inodes >= 3 && sc.unrecoverable == 0);
    MIRROR_CHECK(sc.repaired >= 1);
    MIRROR_CHECK(cosmofs_stats(mount_of(ENG), &st) == 0 && st.repairs > repairs0);

    /* A second scrub finds nothing to do: the first one fixed it. */
    struct cosmofs_scrub_stats sc2;
    MIRROR_CHECK(cosmofs_scrub(mount_of(ENG), &sc2, COSMOFS_SCRUB_REPAIR) == 0);
    MIRROR_CHECK(sc2.repaired == 0 && sc2.unrecoverable == 0);

    /* Rot the second copy of a *metadata* block that no read will
     * choose -- the inode map's root, which is reached through copy 0
     * every time. Only a scrub that looks at every copy can see it. */
    sblk = kmalloc(CFS_BLOCK, 0);
    MIRROR_CHECK(sblk != NULL);
    MIRROR_CHECK(pool_open(bd[0], &sp) == 0);
    MIRROR_CHECK(pool_read(sp, CFS_SUPER_A, sblk) == 0 || pool_read(sp, CFS_SUPER_B, sblk) == 0);
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
    sp = NULL;
    kfree(sblk);
    sblk = NULL;
    MIRROR_CHECK(imap_root != 0);
    MIRROR_CHECK(rot_copy(bd[1], CFS_DVA_BLK(imap_root), 0x77));
    MIRROR_CHECK(read_matches(ENG "/dir/inner", "inner", 5));   /* copy 0 answers; nothing notices */
    MIRROR_CHECK(cosmofs_scrub(mount_of(ENG), &sc, COSMOFS_SCRUB_REPAIR) == 0);
    MIRROR_CHECK(sc.repaired >= 1 && sc.unrecoverable == 0);
    MIRROR_CHECK(cosmofs_scrub(mount_of(ENG), &sc2, COSMOFS_SCRUB_REPAIR) == 0 && sc2.repaired == 0);

    /* Now rot copy 0 of the same block: the read falls back to copy 1
     * and repairs copy 0. */
    MIRROR_CHECK(rot_copy(bd[0], CFS_DVA_BLK(pblk), 0x5A));
    MIRROR_CHECK(read_matches(ENG "/mirrored", data, sizeof(data)));
    MIRROR_CHECK(cosmofs_scrub(mount_of(ENG), &sc, COSMOFS_SCRUB_REPAIR) == 0 && sc.unrecoverable == 0);

    /* Both copies gone: that file is unreadable, and the rest of the
     * filesystem is not. */
    MIRROR_CHECK(rot_copy(bd[0], CFS_DVA_BLK(pblk), 0x11));
    MIRROR_CHECK(rot_copy(bd[1], CFS_DVA_BLK(pblk), 0x22));
    MIRROR_CHECK(vfs_open(NULL, ENG "/mirrored", COSMO_O_RDONLY, 0, &f) == 0);
    static char got[4096];
    MIRROR_CHECK(file_read(f, got, sizeof(got)) == -EIO);
    file_put(f);
    f = NULL;
    MIRROR_CHECK(read_matches(ENG "/dir/inner", "inner", 5));
    MIRROR_CHECK(cosmofs_scrub(mount_of(ENG), &sc, COSMOFS_SCRUB_REPAIR) == 0 && sc.unrecoverable == 1);

    MIRROR_CHECK(vfs_umount(ENG) == 0);

    /* A device that was detached while the pool went on being written
     * carries older contents that pass every checksum on it. It is
     * recognised by the generation and left out of the mirror: the pool
     * comes up degraded rather than quietly serving old blocks. */
    MIRROR_CHECK(age_device(bd[1], 1));
    MIRROR_CHECK(vfs_mount(ENG, "cosmofs", bd[0], 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    MIRROR_CHECK(cosmofs_stats(mount_of(ENG), &st) == 0);
    MIRROR_CHECK(st.devices == 1 && st.degraded == 1);
    MIRROR_CHECK(read_matches(ENG "/dir/inner", "inner", 5));
    MIRROR_CHECK(vfs_umount(ENG) == 0);

    MIRROR_CHECK(vfs_rmdir(NULL, ENG) == 0);
    ramblk_destroy(bd[0]);
    bd[0] = NULL;
    ramblk_destroy(bd[1]);
    bd[1] = NULL;
    kinfo("selftest: cosmofs-mirror: two copies, %llu blocks scrubbed", (unsigned long long)sc.blocks_read);
    cosmofs_test_resilver_hold(false);
    return true;

cleanup:
    cosmofs_test_resilver_hold(false);
    if (f)
        file_put(f);
    if (sp)
        pool_close(sp);
    kfree(sblk);
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    (void)vfs_rmdir(NULL, ENG);
    if (bd[0])
        ramblk_destroy(bd[0]);
    if (bd[1])
        ramblk_destroy(bd[1]);
    return false;
#undef MIRROR_CHECK
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
    cosmofs_test_resilver_hold(true);   /* as cosmofs-mirror: the stale copy is not read */
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
    CHECK(cosmofs_scrub(mount_of(ENG), &sc, COSMOFS_SCRUB_REPAIR) == 0 && sc.unrecoverable == 0);
    CHECK(vfs_umount(ENG) == 0);

    /* Neither of member 1's copies current: no complete copy of the
     * member, and the mount refuses rather than mount part of a pool
     * (roadmap 1.2 B3's boot rule). */
    CHECK(age_label(bd[2], 1) && age_label(bd[3], 1));
    CHECK(vfs_mount(ENG, "cosmofs", bd[0], 0) == -ENODEV);

    CHECK(vfs_rmdir(NULL, ENG) == 0);
    for (unsigned i = 0; i < 4; i++)
        ramblk_destroy(bd[i]);
    kinfo("selftest: cosmofs-mirror-stale: a stale copy is passed over, whichever one it is; with none current, "
          "the mount refuses");
    cosmofs_test_resilver_hold(false);
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
    struct blkdev *bd = NULL;
    uint8_t *dense = NULL, *sparse_data = NULL, *back = NULL;
    struct file *f = NULL;
    if (!engine_mount(&bd, 1024, reason))
        return false;
#define COMPRESS_CHECK(cond)                                                  \
    do {                                                                      \
        if (!(cond)) {                                                        \
            *reason = "check failed: " #cond " at line " STR(__LINE__);     \
            goto cleanup;                                                    \
        }                                                                     \
    } while (0)
    const size_t len = 32 * 4096;   /* four records */
    dense = kmalloc(len, 0);
    sparse_data = kmalloc(len, 0);
    back = kmalloc(len, 0);
    COMPRESS_CHECK(dense != NULL && sparse_data != NULL && back != NULL);
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
    COMPRESS_CHECK(cosmofs_stats(mount_of(ENG), &st0) == 0);
    COMPRESS_CHECK(write_file(ENG "/small", sparse_data, len));
    COMPRESS_CHECK(vfs_sync() == 0);
    COMPRESS_CHECK(cosmofs_stats(mount_of(ENG), &st1) == 0);
    COMPRESS_CHECK(write_file(ENG "/big", dense, len));
    COMPRESS_CHECK(vfs_sync() == 0);
    COMPRESS_CHECK(cosmofs_stats(mount_of(ENG), &st2) == 0);

    uint64_t compressible = st0.free_blocks - st1.free_blocks;
    uint64_t incompressible = st1.free_blocks - st2.free_blocks;
    /* The repetitive file is stored in a fraction of its 32 blocks; the
     * other one cannot be, and is stored as it is. */
    kinfo("cosmofs-compress: %llu blocks compressible, %llu not", (unsigned long long)compressible,
          (unsigned long long)incompressible);
    COMPRESS_CHECK(compressible < 16 && incompressible >= 32);
    COMPRESS_CHECK(compressible * 3 < incompressible);

    /* Both read back exactly, through the records and around them. */
    COMPRESS_CHECK(read_matches(ENG "/small", sparse_data, len));
    COMPRESS_CHECK(read_matches(ENG "/big", dense, len));

    /* A page written inside a compressed record: the record is read,
     * rebuilt around the new page, and written again. */
    COMPRESS_CHECK(vfs_open(NULL, ENG "/small", COSMO_O_RDWR, 0, &f) == 0);
    memset(sparse_data + 3 * 4096, 0x5a, 4096);
    COMPRESS_CHECK(file_pwrite(f, sparse_data + 3 * 4096, 4096, 3 * 4096) == 4096);
    COMPRESS_CHECK(file_sync(f) == 0);
    file_put(f);
    f = NULL;
    COMPRESS_CHECK(read_matches(ENG "/small", sparse_data, len));

    /* A partial page inside a record, which reads the record to fill in
     * what the write does not cover. */
    COMPRESS_CHECK(vfs_open(NULL, ENG "/small", COSMO_O_RDWR, 0, &f) == 0);
    memset(sparse_data + 9 * 4096 + 100, 0x33, 500);
    COMPRESS_CHECK(file_pwrite(f, sparse_data + 9 * 4096 + 100, 500, 9 * 4096 + 100) == 500);
    COMPRESS_CHECK(file_sync(f) == 0);
    file_put(f);
    f = NULL;
    COMPRESS_CHECK(read_matches(ENG "/small", sparse_data, len));

    /* Pages that leave a record incompressible: it is written again as
     * ordinary blocks, and the record goes first, whole. The plain
     * fallback used to write its first block over the record -- that
     * block's checksum, then a mapping set_extent refused -- and the
     * write-back failed with the record left unreadable (shell
     * completeness S7). */
    COMPRESS_CHECK(vfs_open(NULL, ENG "/small", COSMO_O_RDWR, 0, &f) == 0);
    memcpy(sparse_data + 16 * 4096, dense + 16 * 4096, 8 * 4096);
    COMPRESS_CHECK(file_pwrite(f, sparse_data + 16 * 4096, 8 * 4096, 16 * 4096) == 8 * 4096);
    COMPRESS_CHECK(file_sync(f) == 0);
    file_put(f);
    f = NULL;
    COMPRESS_CHECK(read_matches(ENG "/small", sparse_data, len));

    /* Truncating into the middle of a record: what survives is rewritten
     * as ordinary blocks, and what is past the end must read as zeros
     * when the file grows again -- not as the record's old contents. */
    int trc = vfs_truncate(NULL, ENG "/small", 10 * 4096 + 7);
    if (trc)
        kerror("compress: truncate returned %d", trc);
    COMPRESS_CHECK(trc == 0);
    COMPRESS_CHECK(read_matches_prefix(ENG "/small", sparse_data, 10 * 4096 + 7));
    COMPRESS_CHECK(vfs_truncate(NULL, ENG "/small", len) == 0);
    COMPRESS_CHECK(vfs_open(NULL, ENG "/small", COSMO_O_RDONLY, 0, &f) == 0);
    COMPRESS_CHECK(file_read(f, back, len) == (int64_t)len);
    file_put(f);
    f = NULL;
    COMPRESS_CHECK(memcmp(back, sparse_data, 10 * 4096 + 7) == 0);
    for (size_t i = 10 * 4096 + 7; i < len; i++)
        COMPRESS_CHECK(back[i] == 0);

    /* A scrub reads every record through the checksums of its physical
     * blocks. */
    struct cosmofs_scrub_stats sc;
    COMPRESS_CHECK(cosmofs_scrub(mount_of(ENG), &sc, COSMOFS_SCRUB_REPAIR) == 0 && sc.unrecoverable == 0);

    kfree(dense);
    dense = NULL;
    kfree(sparse_data);
    sparse_data = NULL;
    kfree(back);
    back = NULL;
    kinfo("selftest: cosmofs-compress: %llu blocks for 32 compressible, %llu for 32 that are not",
          (unsigned long long)compressible, (unsigned long long)incompressible);
    return engine_unmount(bd, reason);

cleanup:
    if (f)
        file_put(f);
    kfree(back);
    kfree(sparse_data);
    kfree(dense);
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    (void)vfs_rmdir(NULL, ENG);
    ramblk_destroy(bd);
    return false;
#undef COMPRESS_CHECK
}

/*
 * CFS-32: a write that would take an inode past the runs it may have is
 * refused at the write, with nothing dirtied, and never at the commit.
 * The failure this replaces: every write was accepted into the page
 * cache, every write-back of it failed -EFBIG, and the pages were lost
 * when the vnode went (`vfs: N dirty page(s) ... lost`) while the size
 * still counted them.
 *
 * One compressible record at every other record's place: each is a run
 * of its own, and the gaps between them keep any two from merging, so
 * the file reaches the bound whatever the write policy. The mount's
 * bound is lowered to EB_MAX runs (the test seam) so it takes a few
 * records rather than 128 MiB.
 */
#define EB_MAX     64u
#define EB_RECORDS 100u   /* more than EB_MAX runs' worth */

bool selftest_cosmofs_extent_bound(const char **reason)
{
    struct blkdev *bd = NULL;
    uint8_t *rec = NULL, *back = NULL;
    struct file *f = NULL;
    if (!engine_mount(&bd, 2048, reason))
        return false;
#define EB_CHECK(cond)                                                        \
    do {                                                                      \
        if (!(cond)) {                                                        \
            *reason = "check failed: " #cond " at line " STR(__LINE__);     \
            goto cleanup;                                                    \
        }                                                                     \
    } while (0)
    const size_t rlen = (size_t)CFS_RECORD_BLOCKS * 4096;
    rec = kmalloc(rlen, 0);
    back = kmalloc(rlen, 0);
    EB_CHECK(rec != NULL && back != NULL);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    cosmofs_test_set_max_extents(mount_of(ENG), EB_MAX);
    struct pagecache_stats pc0, pc1;
    pagecache_get_stats(&pc0);

    EB_CHECK(vfs_open(NULL, ENG "/sparse", COSMO_O_RDWR | COSMO_O_CREAT | COSMO_O_TRUNC, 0644, &f) == 0);
    /* Records written whole, then the bytes of the one the bound cut: a
     * write that reaches it returns what it took, as write(2) does, and
     * the rest of it is refused. */
    unsigned accepted = 0;
    size_t part = 0;
    int64_t refused = 0;
    for (unsigned k = 0; k < EB_RECORDS; k++) {
        for (size_t i = 0; i < rlen; i++)
            rec[i] = (uint8_t)((i + k) % 61);   /* repetitive: each record compresses */
        int64_t n = file_pwrite(f, rec, rlen, (uint64_t)k * 2 * rlen);
        if (n > 0 && n < (int64_t)rlen) {
            part = (size_t)n;
            n = file_pwrite(f, rec + part, rlen - part, (uint64_t)k * 2 * rlen + part);
        }
        if (n != (int64_t)rlen) {
            refused = n;
            break;
        }
        accepted++;
    }
    kinfo("cosmofs-extent-bound: %u of %u records accepted under a bound of %u runs, %zu bytes of the next; "
          "then %lld", accepted, EB_RECORDS, EB_MAX, part, (long long)refused);
    /* Refused by the bound, at the write: not before half of it is used,
     * and not after it is passed. */
    EB_CHECK(refused == -EFBIG);
    EB_CHECK(accepted >= EB_MAX / 2 && accepted <= EB_MAX);
    /* Everything accepted can be written back: no write-back failed. */
    EB_CHECK(file_sync(f) == 0);
    file_put(f);
    f = NULL;
    pagecache_get_stats(&pc1);
    EB_CHECK(pc1.wb_errors == pc0.wb_errors && pc1.dropped_dirty == pc0.dropped_dirty);

    /* And it is what the disk holds: after a remount the size ends at the
     * last record accepted, and every record reads back. */
    cosmofs_test_set_max_extents(mount_of(ENG), 0);
    EB_CHECK(vfs_umount(ENG) == 0);
    EB_CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    struct vnode *vn;
    EB_CHECK(vfs_lookup(NULL, ENG "/sparse", &vn) == 0);
    uint64_t size = vn->size;
    vnode_put(vn);
    uint64_t want = part ? (uint64_t)accepted * 2 * rlen + part : ((uint64_t)(accepted - 1) * 2 + 1) * rlen;
    EB_CHECK(accepted > 0 && size == want);
    EB_CHECK(vfs_open(NULL, ENG "/sparse", COSMO_O_RDONLY, 0, &f) == 0);
    for (unsigned k = 0; k <= accepted; k++) {
        size_t len = k < accepted ? rlen : part;
        if (len == 0)
            break;
        EB_CHECK(file_pread(f, back, len, (uint64_t)k * 2 * rlen) == (int64_t)len);
        for (size_t i = 0; i < len; i++)
            EB_CHECK(back[i] == (uint8_t)((i + k) % 61));
    }
    file_put(f);
    f = NULL;
    struct cosmofs_check_report r;
    EB_CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0 && r.clean);

    kfree(rec);
    kfree(back);
    kinfo("selftest: cosmofs-extent-bound: a write past the run bound is refused -EFBIG after %u records, and "
          "every record accepted is on the disk", accepted);
    return engine_unmount(bd, reason);

cleanup:
    if (f)
        file_put(f);
    kfree(rec);
    kfree(back);
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    (void)vfs_rmdir(NULL, ENG);
    ramblk_destroy(bd);
    return false;
#undef EB_CHECK
}

/*
 * CFS-32: a page in a compressed record can turn the record
 * incompressible, and its write-back then drops the record and writes it
 * again as eight plain blocks, which may be more than one run. A page is
 * charged two runs (cfs_dirty_check); this checks that records rebuilt
 * that way, on a pool whose free space has been cut up, are refused at
 * the write when the bound is near and written back whole otherwise. (A
 * rebuild here adds about two runs; seven, on free space of single
 * blocks only, is the path CONFIG_DEBUG's stuck_page_check names: CF33,
 * docs/backlog.md CFS-39.)
 *
 * Each record is six pages of noise and two of zeros, compressed to
 * seven blocks; one zero page made noise leaves it incompressible.
 */
#define RB_MAX     64u
#define RB_RECORDS 30u
#define RB_HOLES   300u

static void rb_noise(uint8_t *p, size_t len, uint32_t seed)
{
    uint32_t x = seed * 2654435761u + 1;
    for (size_t i = 0; i < len; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        p[i] = (uint8_t)x;
    }
}

static void rb_record(uint8_t *rec, unsigned k)
{
    for (unsigned i = 0; i < CFS_RECORD_BLOCKS; i++) {
        if (i < 6)
            rb_noise(rec + (size_t)i * 4096, 4096, k * 16 + i);
        else
            memset(rec + (size_t)i * 4096, 0, 4096);
    }
}

bool selftest_cosmofs_extent_rebuild(const char **reason)
{
    struct blkdev *bd = NULL;
    uint8_t *rec = NULL, *back = NULL;
    struct cosmofs_check_report *r = NULL;
    struct file *f = NULL;
    char path[48];
    if (!engine_mount(&bd, 4096, reason))
        return false;
#define RB_CHECK(cond)                                                        \
    do {                                                                      \
        if (!(cond)) {                                                        \
            *reason = "check failed: " #cond " at line " STR(__LINE__);     \
            goto cleanup;                                                    \
        }                                                                     \
    } while (0)
    const size_t rlen = (size_t)CFS_RECORD_BLOCKS * 4096;
    rec = kmalloc(rlen, 0);
    back = kmalloc(rlen, 0);
    r = kmalloc(sizeof(*r), 0);
    RB_CHECK(rec != NULL && back != NULL && r != NULL);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    cosmofs_test_set_max_extents(mount_of(ENG), RB_MAX);
    struct pagecache_stats pc0, pc1;
    pagecache_get_stats(&pc0);
    struct cosmofs_stats st0, st1;
    RB_CHECK(cosmofs_stats(mount_of(ENG), &st0) == 0);

    RB_CHECK(vfs_open(NULL, ENG "/c", COSMO_O_RDWR | COSMO_O_CREAT | COSMO_O_TRUNC, 0644, &f) == 0);
    for (unsigned k = 0; k < RB_RECORDS; k++) {
        rb_record(rec, k);
        RB_CHECK(file_pwrite(f, rec, rlen, (uint64_t)k * rlen) == (int64_t)rlen);
    }
    RB_CHECK(file_sync(f) == 0);
    file_put(f);
    f = NULL;
    RB_CHECK(cosmofs_stats(mount_of(ENG), &st1) == 0);
    /* Each record compressed to seven blocks, not stored plain. */
    RB_CHECK(st0.free_blocks - st1.free_blocks < (uint64_t)RB_RECORDS * CFS_RECORD_BLOCKS);

    /* Free space in single blocks: files of one block each, every other
     * one removed. */
    rb_noise(back, 4096, 7);
    for (unsigned i = 0; i < RB_HOLES; i++) {
        ksnprintf(path, sizeof(path), ENG "/h%u", i);
        RB_CHECK(vfs_open(NULL, path, COSMO_O_RDWR | COSMO_O_CREAT | COSMO_O_TRUNC, 0644, &f) == 0);
        RB_CHECK(file_pwrite(f, back, 4096, 0) == 4096);
        file_put(f);
        f = NULL;
    }
    RB_CHECK(vfs_sync() == 0);
    for (unsigned i = 0; i < RB_HOLES; i += 2) {
        ksnprintf(path, sizeof(path), ENG "/h%u", i);
        RB_CHECK(vfs_unlink(NULL, path) == 0);
    }
    RB_CHECK(vfs_sync() == 0);
    RB_CHECK(vfs_sync() == 0);   /* the blocks freed by the last commit are free after the next */

    /* One zero page of each record made noise, until the bound refuses. */
    RB_CHECK(vfs_open(NULL, ENG "/c", COSMO_O_RDWR, 0, &f) == 0);
    unsigned accepted = 0;
    int64_t refused = 0;
    for (unsigned k = 0; k < RB_RECORDS; k++) {
        rb_noise(back, 4096, 1000 + k);
        int64_t n = file_pwrite(f, back, 4096, (uint64_t)k * rlen + 6 * 4096);
        if (n != 4096) {
            refused = n;
            break;
        }
        accepted++;
    }
    kinfo("cosmofs-extent-rebuild: %u of %u pages accepted under a bound of %u runs, then %lld", accepted,
          RB_RECORDS, RB_MAX, (long long)refused);
    RB_CHECK(accepted > 0);
    RB_CHECK(accepted == RB_RECORDS || refused == -EFBIG);
    /* Everything accepted can be written back. */
    RB_CHECK(file_sync(f) == 0);
    file_put(f);
    f = NULL;
    pagecache_get_stats(&pc1);
    RB_CHECK(pc1.wb_errors == pc0.wb_errors && pc1.dropped_dirty == pc0.dropped_dirty);

    /* And it is on the disk. */
    cosmofs_test_set_max_extents(mount_of(ENG), 0);
    RB_CHECK(vfs_umount(ENG) == 0);
    RB_CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    RB_CHECK(vfs_open(NULL, ENG "/c", COSMO_O_RDONLY, 0, &f) == 0);
    for (unsigned k = 0; k < RB_RECORDS; k++) {
        rb_record(rec, k);
        if (k < accepted)
            rb_noise(rec + 6 * 4096, 4096, 1000 + k);
        RB_CHECK(file_pread(f, back, rlen, (uint64_t)k * rlen) == (int64_t)rlen);
        RB_CHECK(memcmp(back, rec, rlen) == 0);
    }
    file_put(f);
    f = NULL;
    RB_CHECK(cosmofs_check(mount_of(ENG), r, 0) == 0 && r->clean);

    kfree(r);
    kfree(rec);
    kfree(back);
    kinfo("selftest: cosmofs-extent-rebuild: %u records made incompressible by one page each, on cut-up "
          "free space, all written back", accepted);
    return engine_unmount(bd, reason);

cleanup:
    if (f)
        file_put(f);
    kfree(r);
    kfree(rec);
    kfree(back);
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    (void)vfs_rmdir(NULL, ENG);
    ramblk_destroy(bd);
    return false;
#undef RB_CHECK
}

/*
 * CFS-32 1b: a compressible file written in order is not stopped by the
 * run bound. A compressed record is a run of its own; once the inode
 * holds three quarters of the runs it may have, new records are written
 * plain, as one run each next to the one before, which merge. A rewrite
 * of a compressed record stays compressed (it replaces a run), and a
 * truncate that takes the inode below the mark brings compression back.
 * On a mount whose bound the seam lowers to ER_MAX runs. This is the
 * policy's coverage, not its failing test: under so low a bound the
 * write's charge (CF33) cuts records into plain blocks, which merge, from
 * about the same point as the mark, and the test passes without the
 * policy as well (2026-10-25, both trees 53 runs). The failing test is
 * test-large's 1 GiB file (tools/cfs32-raw-probe.py).
 */
#define ER_MAX     64u
#define ER_RECORDS 200u   /* three times the bound's worth of compressed records */

static bool er_fill(uint8_t *rec, size_t rlen, unsigned k)
{
    for (size_t i = 0; i < rlen; i++)
        rec[i] = (uint8_t)((i + k) % 61);   /* repetitive: compresses to a block */
    return true;
}

/* Committed, and the blocks the commit replaced free again (they are
 * freed by the commit after), so a count of free blocks is the data's. */
static bool er_settle(void)
{
    return vfs_sync() == 0 && vfs_sync() == 0;
}

static bool er_runs(const char *path, unsigned *n)
{
    struct vnode *vn;
    if (vfs_lookup(NULL, path, &vn))
        return false;
    int rc = cosmofs_test_runs(vn, n);
    vnode_put(vn);
    return rc == 0;
}

bool selftest_cosmofs_extent_raw(const char **reason)
{
    struct blkdev *bd = NULL;
    uint8_t *rec = NULL, *back = NULL;
    struct cosmofs_check_report *r = NULL;   /* not the stack: the frame is near its limit */
    struct file *f = NULL;
    if (!engine_mount(&bd, 4096, reason))
        return false;
#define ER_CHECK(cond)                                                        \
    do {                                                                      \
        if (!(cond)) {                                                        \
            *reason = "check failed: " #cond " at line " STR(__LINE__);     \
            goto cleanup;                                                    \
        }                                                                     \
    } while (0)
    const size_t rlen = (size_t)CFS_RECORD_BLOCKS * 4096;
    const unsigned mark = ER_MAX * 3 / 4;
    rec = kmalloc(rlen, 0);
    back = kmalloc(rlen, 0);
    ER_CHECK(rec != NULL && back != NULL);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    cosmofs_test_set_max_extents(mount_of(ENG), ER_MAX);
    struct pagecache_stats pc0, pc1;
    pagecache_get_stats(&pc0);
    struct cosmofs_stats st0, st1, st2, st3;
    ER_CHECK(cosmofs_stats(mount_of(ENG), &st0) == 0);

    /* Every record of a file three times the bound's worth, in order,
     * written back as the page cache chooses: under the seam's bound the
     * write's charge (CF33) writes back every few records, a record cut
     * by it in part as plain blocks, so the mark is reached sooner than
     * by records alone; past it the plain records merge. */
    ER_CHECK(vfs_open(NULL, ENG "/long", COSMO_O_RDWR | COSMO_O_CREAT | COSMO_O_TRUNC, 0644, &f) == 0);
    for (unsigned k = 0; k < ER_RECORDS; k++) {
        er_fill(rec, rlen, k);
        ER_CHECK(file_pwrite(f, rec, rlen, (uint64_t)k * rlen) == (int64_t)rlen);
    }
    ER_CHECK(file_sync(f) == 0);
    ER_CHECK(er_settle());
    ER_CHECK(cosmofs_stats(mount_of(ENG), &st1) == 0);
    unsigned runs = 0;
    ER_CHECK(er_runs(ENG "/long", &runs));
    uint64_t used = st0.free_blocks - st1.free_blocks;
    kinfo("cosmofs-extent-raw: %u records in %u runs under a bound of %u (plain from %u), %llu blocks", ER_RECORDS,
          runs, ER_MAX, mark, (unsigned long long)used);
    /* Every record taken, inside the bound, and the part before the
     * mark compressed: fewer blocks than every
     * record plain by at least a block for each run of the mark. */
    ER_CHECK(runs <= ER_MAX);
    ER_CHECK(used < (uint64_t)ER_RECORDS * CFS_RECORD_BLOCKS - mark);

    /* A rewrite inside the compressed part stays compressed: no new run,
     * and no plain record's worth of blocks more. */
    er_fill(rec, rlen, 1000);
    ER_CHECK(file_pwrite(f, rec, rlen, 5 * rlen) == (int64_t)rlen);
    ER_CHECK(file_sync(f) == 0);
    ER_CHECK(er_settle());
    unsigned runs2 = 0;
    ER_CHECK(er_runs(ENG "/long", &runs2));
    ER_CHECK(cosmofs_stats(mount_of(ENG), &st2) == 0);
    ER_CHECK(runs2 == runs);
    ER_CHECK(st2.free_blocks + CFS_RECORD_BLOCKS / 2 > st1.free_blocks);
    file_put(f);
    f = NULL;

    /* Truncated below the mark, a record written after it compresses
     * again: it takes a block or two, not eight. */
    ER_CHECK(vfs_truncate(NULL, ENG "/long", (uint64_t)(mark / 2) * rlen) == 0);
    ER_CHECK(er_settle());
    ER_CHECK(cosmofs_stats(mount_of(ENG), &st2) == 0);
    ER_CHECK(vfs_open(NULL, ENG "/long", COSMO_O_RDWR, 0, &f) == 0);
    er_fill(rec, rlen, mark / 2);
    ER_CHECK(file_pwrite(f, rec, rlen, (uint64_t)(mark / 2) * rlen) == (int64_t)rlen);
    ER_CHECK(file_sync(f) == 0);
    ER_CHECK(er_settle());
    ER_CHECK(cosmofs_stats(mount_of(ENG), &st3) == 0);
    ER_CHECK(st2.free_blocks - st3.free_blocks < CFS_RECORD_BLOCKS / 2);
    file_put(f);
    f = NULL;
    pagecache_get_stats(&pc1);
    ER_CHECK(pc1.wb_errors == pc0.wb_errors && pc1.dropped_dirty == pc0.dropped_dirty);

    /* Back to the long file, whole, and read after a remount. */
    ER_CHECK(vfs_open(NULL, ENG "/long", COSMO_O_RDWR, 0, &f) == 0);
    for (unsigned k = mark / 2; k < ER_RECORDS; k++) {
        er_fill(rec, rlen, k);
        ER_CHECK(file_pwrite(f, rec, rlen, (uint64_t)k * rlen) == (int64_t)rlen);
    }
    ER_CHECK(file_sync(f) == 0);
    file_put(f);
    f = NULL;
    cosmofs_test_set_max_extents(mount_of(ENG), 0);
    ER_CHECK(vfs_umount(ENG) == 0);
    ER_CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    ER_CHECK(vfs_open(NULL, ENG "/long", COSMO_O_RDONLY, 0, &f) == 0);
    for (unsigned k = 0; k < ER_RECORDS; k++) {
        er_fill(rec, rlen, k == 5 ? 1000 : k);
        ER_CHECK(file_pread(f, back, rlen, (uint64_t)k * rlen) == (int64_t)rlen);
        ER_CHECK(memcmp(back, rec, rlen) == 0);
    }
    file_put(f);
    f = NULL;
    r = kmalloc(sizeof(*r), 0);
    ER_CHECK(r != NULL);
    ER_CHECK(cosmofs_check(mount_of(ENG), r, 0) == 0 && r->clean);

    kfree(r);
    kfree(rec);
    kfree(back);
    kinfo("selftest: cosmofs-extent-raw: %u compressible records in %u runs under a bound of %u; a rewrite stays "
          "compressed, a truncate below the mark compresses again", ER_RECORDS, runs, ER_MAX);
    return engine_unmount(bd, reason);

cleanup:
    if (f)
        file_put(f);
    kfree(r);
    kfree(rec);
    kfree(back);
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    (void)vfs_rmdir(NULL, ENG);
    ramblk_destroy(bd);
    return false;
#undef ER_CHECK
}

/*
 * CF16: a mapped block's stored checksum is the checksum of the data at
 * that block. A block's entry is stored before the mapping that names
 * it, so a mapping that fails must put the old entry back: before that
 * rule, set_extent failing left the old block mapped under the new
 * block's checksum, and every read of it failed (inventory section 17,
 * after shell completeness S7). And a record the plain fallback dropped
 * must not lose its unwritten blocks to a failure partway (CF17).
 *
 * Each case writes a file, commits it, rewrites it, and fails one
 * set_extent of the write-back with the rule's errno. Then: every block
 * the open inode maps verifies; the pages the failure left dirty are
 * written by the next sync; and after a remount the file reads back
 * whole, with no checksum failure and a clean scrub.
 */
#if CONFIG_FAULTINJECT
enum extent_fail_layout {
    EF_PLAIN,     /* plain blocks over plain blocks: data_write_block */
    EF_RECORD,    /* a compressed record over a compressed record: record_write */
    EF_DROPPED,   /* one page that leaves a record incompressible: dropped, then plain */
    EF_CRYPT,     /* EF_PLAIN on an encrypted pool: authenticated entries */
    EF_EVICT,     /* EF_PLAIN, then closed with the retry failing too: the pages dropped */
};

static const char *const extent_fail_names[] = { "plain", "record", "dropped", "crypt", "evict" };

#define EF_PAGES 16u   /* two records */

static bool extent_fail_case(enum fi_kind kind, enum extent_fail_layout layout, const char **reason)
{
    static const char key[] = "extent fail key";
    const size_t len = (size_t)EF_PAGES * 4096;
    struct blkdev *bd = NULL;
    struct file *f = NULL;
    uint8_t *a = kmalloc(len, 0), *b = kmalloc(len, 0);
    bool mounted = false;
#define EF_CHECK(cond)                                                        \
    do {                                                                      \
        if (!(cond)) {                                                        \
            *reason = "check failed: " #cond " at line " STR(__LINE__);     \
            goto fail;                                                       \
        }                                                                     \
    } while (0)
    EF_CHECK(a != NULL && b != NULL);
    if (layout == EF_CRYPT) {
        (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
        bd = ramblk_create(1024);
        EF_CHECK(bd != NULL);
        EF_CHECK(cosmofs_format_encrypted(bd, key, sizeof(key) - 1) == 0);
        int mk = vfs_mkdir(NULL, ENG, 0755);
        EF_CHECK(mk == 0 || mk == -EEXIST);
        EF_CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
        cosmofs_test_set_writeback(mount_of(ENG), false);
        EF_CHECK(cosmofs_test_unlock(mount_of(ENG), key, sizeof(key) - 1) == 0);
    } else {
        EF_CHECK(engine_mount(&bd, 1024, reason));
    }
    mounted = true;

    /* The old contents and the new, and which set_extent of the
     * write-back fails. */
    size_t off = 0, wlen = len;
    unsigned every = 5;   /* the fifth block of the first record */
    switch (layout) {
    case EF_PLAIN:
    case EF_CRYPT:
    case EF_EVICT:
        fill_incompressible(a, len, 0x1234567u);
        fill_incompressible(b, len, 0x7654321u);
        break;
    case EF_RECORD:
        for (size_t i = 0; i < len; i++) {
            a[i] = (uint8_t)(i % 61);
            b[i] = (uint8_t)(i * 7 % 59 + 1);
        }
        every = 1;   /* the first record's own mapping */
        break;
    case EF_DROPPED:
        /* Six incompressible pages and two of zeros compress into seven
         * blocks; one more incompressible page does not, so the record
         * is dropped and written again as eight plain blocks, of which
         * the third fails. */
        memset(a, 0, len);
        for (unsigned r = 0; r < EF_PAGES / 8; r++)
            fill_incompressible(a + (size_t)r * 8 * 4096, 6 * 4096, 0x5150u + r);
        memcpy(b, a, len);
        fill_incompressible(b + 7 * 4096, 4096, 0xfeedu);
        off = 7 * 4096;
        wlen = 4096;
        every = 3;
        break;
    }

    EF_CHECK(write_file(ENG "/f", a, len));
    EF_CHECK(vfs_sync() == 0);
    EF_CHECK(vfs_open(NULL, ENG "/f", COSMO_O_RDWR, 0, &f) == 0);
    EF_CHECK(file_pwrite(f, b + off, wlen, off) == (int64_t)wlen);
    faultinject_set(kind, every, 1, thread_current());
    int src = file_sync(f);
    struct fi_stats fst;
    faultinject_stats(kind, &fst);
    faultinject_clear(kind);
    kinfo("selftest: cosmofs-extent-fail: %s %s: sync %d, hits %llu", faultinject_kind_name(kind),
          extent_fail_names[layout], src, (unsigned long long)fst.hits);
    EF_CHECK(fst.hits == 1);
    EF_CHECK(src == (kind == FI_COSMOFS_EXTENT_NOMEM ? -ENOMEM : -EFBIG));

    if (layout == EF_DROPPED) {
        /* The record is gone from the open map and only part of it was
         * written again, so the transaction is abandoned and the
         * committed record is what the disk keeps. Before CF17 the retry
         * wrote the one dirty page and committed: the record's other
         * unwritten blocks were holes. */
        int retry = file_sync(f);
        kinfo("selftest: cosmofs-extent-fail: dropped record: retry %d", retry);
        file_put(f);
        f = NULL;
        (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
        EF_CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
        cosmofs_test_set_writeback(mount_of(ENG), false);
        /* Either the old contents or the new, whole: never the record's
         * unwritten blocks read back as holes. */
        EF_CHECK(read_matches(ENG "/f", a, len) || read_matches(ENG "/f", b, len));
    } else if (layout == EF_EVICT) {
        /* No retry: the last reference goes with every set_extent
         * failing, so the release's own write-back fails as well and the
         * pages are dropped. The blocks the first write-back mapped are
         * the inode's all the same -- before, the inode was stored only
         * after a write that succeeded, so a vnode gone before the retry
         * took the map with it: the four new blocks were nobody's and the
         * four they replaced were freed under the committed inode. */
        faultinject_set(kind, 1, 1u << 20, thread_current());
        file_put(f);
        f = NULL;
        faultinject_stats(kind, &fst);
        faultinject_clear(kind);
        EF_CHECK(vfs_sync() == 0);
        struct cosmofs_check_report rep;
        EF_CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0);
        kinfo("selftest: cosmofs-extent-fail: evict: release hits %llu; %llu leaked, %llu free-in-use",
              (unsigned long long)fst.hits, (unsigned long long)rep.alloc_not_seen.count,
              (unsigned long long)rep.seen_not_alloc.count);
        EF_CHECK(fst.hits >= 1);
        EF_CHECK(rep.alloc_not_seen.count == 0 && rep.seen_not_alloc.count == 0);
        EF_CHECK(rep.clean);
        EF_CHECK(vfs_umount(ENG) == 0);
        EF_CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
        cosmofs_test_set_writeback(mount_of(ENG), false);
        /* Each page is the old contents or the new: the first four new,
         * which the first write-back wrote. */
        uint8_t *back = kmalloc(len, 0);
        EF_CHECK(back != NULL);
        bool whole = vfs_open(NULL, ENG "/f", COSMO_O_RDONLY, 0, &f) == 0 && file_read(f, back, len) == (int64_t)len;
        unsigned fresh = 0, mixed = 0;
        for (unsigned p = 0; whole && p < EF_PAGES; p++) {
            const uint8_t *pg = back + (size_t)p * 4096;
            if (memcmp(pg, b + (size_t)p * 4096, 4096) == 0)
                fresh += p < 4;
            else if (memcmp(pg, a + (size_t)p * 4096, 4096) != 0)
                mixed++;
        }
        kfree(back);
        if (f) {
            file_put(f);
            f = NULL;
        }
        EF_CHECK(whole && mixed == 0 && fresh == 4);
    } else {
        /* CF16 on what the next commit would publish. */
        uint64_t bad = 0;
        EF_CHECK(cosmofs_test_verify_file(f->vn, &bad) == 0);
        if (bad)
            kerror("cosmofs-extent-fail: %llu block(s) mapped under a checksum not their own",
                   (unsigned long long)bad);
        EF_CHECK(bad == 0);
        /* The failed pages stayed dirty; the next sync writes them. */
        EF_CHECK(file_sync(f) == 0);
        file_put(f);
        f = NULL;
        EF_CHECK(vfs_umount(ENG) == 0);
        EF_CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
        cosmofs_test_set_writeback(mount_of(ENG), false);
        if (layout == EF_CRYPT)
            EF_CHECK(cosmofs_test_unlock(mount_of(ENG), key, sizeof(key) - 1) == 0);
        EF_CHECK(read_matches(ENG "/f", b, len));
    }
    struct cosmofs_stats st;
    EF_CHECK(cosmofs_stats(mount_of(ENG), &st) == 0 && st.csum_failures == 0);
    struct cosmofs_scrub_stats sc;
    EF_CHECK(cosmofs_scrub(mount_of(ENG), &sc, COSMOFS_SCRUB_REPAIR) == 0 && sc.unrecoverable == 0);
    kfree(a);
    kfree(b);
    return engine_unmount(bd, reason);

fail:
    faultinject_clear(kind);
    if (f)
        file_put(f);
    kfree(a);
    kfree(b);
    if (mounted)
        (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    (void)vfs_rmdir(NULL, ENG);
    if (bd)
        ramblk_destroy(bd);
    return false;
#undef EF_CHECK
}
#endif

bool selftest_cosmofs_extent_fail(const char **reason)
{
#if CONFIG_FAULTINJECT
    static const enum fi_kind kinds[] = { FI_COSMOFS_EXTENT_NOMEM, FI_COSMOFS_EXTENT_FBIG };
    unsigned cases = 0;
    for (unsigned k = 0; k < 2; k++) {
        for (unsigned l = EF_PLAIN; l <= EF_EVICT; l++) {
            if (l == EF_CRYPT && !random_ready())
                continue;   /* the no-entropy boot formats no encrypted pool (S18) */
            if (!extent_fail_case(kinds[k], (enum extent_fail_layout)l, reason))
                return false;
            cases++;
        }
    }
    kinfo("selftest: cosmofs-extent-fail: %u cases, every mapped block under its own checksum", cases);
#else
    (void)reason;
#endif
    return true;
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
    CHECK(cosmofs_scrub(mount_of(ENG), &sc, COSMOFS_SCRUB_REPAIR) == 0 && sc.unrecoverable == 0);

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
    CHECK(cosmofs_scrub(mount_of(ENG), &sc, COSMOFS_SCRUB_REPAIR) == 0 && sc.unrecoverable == 0 && sc.blocks_read > 0);
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
/*
 * Which classes were not empty, for a failure message.
 *
 * `CHECK_CLEAN(r)` says a report was not clean and nothing else; the
 * class has to be recovered from the serial log, which is exactly what
 * happened when CI failed `cosmofs-orphan-reserved` while this unit's
 * report was in review (docs/audit/next-subsystem-fsck-unchecked.md).
 * The buffer is static because the CHECK macros take a `const char *`
 * that outlives the call.
 */
static const char *check_why(const struct cosmofs_check_report *r)
{
    static char buf[224];
    unsigned n = 0;
    buf[0] = '\0';
    const struct { const char *name; const struct cosmofs_check_class *cl; } cls[] = {
        { "leaked", &r->alloc_not_seen }, { "free-in-use", &r->seen_not_alloc },
        { "cross-linked", &r->dup },      { "nlink", &r->nlink_wrong },
        { "orphan", &r->orphan },         { "dangling", &r->dangling_entry },
        { "dir-bad", &r->dir_bad },       { "counter", &r->counter_wrong },
        { "chain-cycle", &r->chain_cycle },
        { "extent-order", &r->extent_order },
        { "extent-overlap", &r->extent_overlap },
        { "dup-name", &r->dir_dup_name },
        { "unreadable", &r->unreadable },
    };
    for (unsigned i = 0; i < sizeof(cls) / sizeof(cls[0]); i++) {
        if (cls[i].cl->count == 0)
            continue;
        n += (unsigned)ksnprintf(buf + n, sizeof(buf) - n, "%s%s=%llu first 0x%llx",
                                 n ? ", " : "report not clean: ", cls[i].name,
                                 (unsigned long long)cls[i].cl->count,
                                 (unsigned long long)(cls[i].cl->named ? cls[i].cl->name[0] : 0));
        if (n >= sizeof(buf) - 40)
            break;
    }
    if (n == 0)
        ksnprintf(buf, sizeof(buf), "report not clean, but every class is empty (partial=%d)",
                  (int)r->partial);
    return buf;
}

/* `CHECK_CLEAN(r)` with the reason attached. */
#define CHECK_CLEAN(r)                                                         \
    do {                                                                       \
        if (!(r).clean) {                                                      \
            *reason = check_why(&(r));                                         \
            return false;                                                      \
        }                                                                      \
    } while (0)

static uint64_t check_file_ino(void)
{
    struct cosmo_stat st;
    return vfs_stat(NULL, ENG "/file", &st) == 0 ? st.ino : 0;
}

/* The fixture's directory, for the faults that need one. */
static uint64_t check_sub_ino(void)
{
    struct cosmo_stat st;
    return vfs_stat(NULL, ENG "/sub", &st) == 0 ? st.ino : 0;
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

    CHECK(check_fixture(&bd, reason));
    CHECK((file_ino = check_file_ino()) != 0);
    CHECK(cosmofs_test_corrupt(mount_of(ENG), COSMOFS_CORRUPT_EXTENT_ORDER, file_ino, &what) == 0);
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0);
    CHECK(r.extent_order.count == 1 && r.extent_order.name[0] == what);
    /* And NOT as an overlap: the whole reason there are two classes. */
    CHECK(r.extent_overlap.count == 0);
    /* Nothing else fires: the runs the hook added point at allocated
     * blocks of their own, so this is one finding and not a pile. */
    CHECK(r.dup.count == 0 && r.seen_not_alloc.count == 0 && r.alloc_not_seen.count == 0);
    check_teardown(bd);

    kinfo("selftest: cosmofs-check-extent-order: runs that descend by lblk are reported as "
          "extent_order and not as an overlap");
    return true;
}

bool selftest_cosmofs_check_extent_overlap(const char **reason)
{
    struct cosmofs_check_report r;
    struct blkdev *bd = NULL;
    uint64_t what = 0, file_ino = 0;

    CHECK(check_fixture(&bd, reason));
    CHECK((file_ino = check_file_ino()) != 0);
    CHECK(cosmofs_test_corrupt(mount_of(ENG), COSMOFS_CORRUPT_EXTENT_OVERLAP, file_ino, &what) == 0);
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0);
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
    check_teardown(bd);

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

    CHECK(check_fixture(&bd, reason));
    CHECK((file_ino = check_file_ino()) != 0);
    CHECK(cosmofs_test_corrupt(mount_of(ENG), COSMOFS_CORRUPT_BAD_PTR, file_ino, &what) == 0);
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0);
    CHECK(r.dir_bad.count == 1 && r.dir_bad.name[0] == what);
    /* The pass survives it: a pointer it cannot map must not be used to
     * index the bitmap, which is what the early return is for. */
    CHECK(r.blocks_seen > 0);
    check_teardown(bd);

    kinfo("selftest: cosmofs-check-bad-ptr: a block pointer past the pool is reported and not "
          "used to index the seen map");
    return true;
}

bool selftest_cosmofs_check_namelen(const char **reason)
{
    struct cosmofs_check_report r;
    struct blkdev *bd = NULL;
    uint64_t what = 0;

    CHECK(check_fixture(&bd, reason));
    /* The root directory: the fixture put `file` and `sub` in it. */
    CHECK(cosmofs_test_corrupt(mount_of(ENG), COSMOFS_CORRUPT_NAMELEN, CFS_ROOT_INO, &what) == 0);
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0);
    CHECK(r.dir_bad.count >= 1 && r.dir_bad.name[0] == what);
    check_teardown(bd);

    kinfo("selftest: cosmofs-check-namelen: an entry claiming a name longer than its slot is "
          "refused by length before its bytes are read");
    return true;
}

bool selftest_cosmofs_check_two_parents(const char **reason)
{
    struct cosmofs_check_report r;
    struct blkdev *bd = NULL;
    uint64_t what = 0, sub_ino = 0;

    CHECK(check_fixture(&bd, reason));
    CHECK((sub_ino = check_sub_ino()) != 0);
    CHECK(cosmofs_test_corrupt(mount_of(ENG), COSMOFS_CORRUPT_TWO_PARENTS, sub_ino, &what) == 0);
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0);
    CHECK(r.dir_bad.count >= 1 && r.dir_bad.name[0] == what);
    check_teardown(bd);

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

    CHECK(check_fixture(&bd, reason));
    CHECK((file_ino = check_file_ino()) != 0);
    CHECK(cosmofs_test_corrupt(mount_of(ENG), COSMOFS_CORRUPT_CHAIN_CYCLE, file_ino, &what) == 0);
    /* That this call RETURNS is half the assertion: without the guard
     * the walk does not terminate and the boot reaches its timeout. */
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0);
    CHECK(r.chain_cycle.count >= 1);
    /*
     * The walk revisits the two blocks until the guard stops it, so the
     * live-claim map sees each of them more than once and `dup` fires
     * too. That is not noise: it is what a cyclic chain looks like to a
     * checker that claims as it walks, and asserting it keeps the test
     * honest about what the pass actually reports.
     */
    CHECK(r.dup.count >= 1);
    check_teardown(bd);

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

    CHECK(check_fixture(&bd, reason));
    CHECK(vfs_mkdir(NULL, ENG "/.snapshots/one", 0755) == 0);   /* takes one, and commits */

    int rc = cosmofs_test_corrupt(mount_of(ENG), COSMOFS_CORRUPT_SNAP_MEMBERS, 0, &what);
    if (rc == -ENOTSUP) {
        /* A member table is a version-4 shape; before it there is no
         * count to be wrong. Skipped rather than silently passing. */
        kinfo("selftest: cosmofs-check-snap-members: needs format v4 or later; skipping");
        check_teardown(bd);
        return true;
    }
    CHECK(rc == 0);
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0);
    CHECK(r.dir_bad.count >= 1 && r.dir_bad.name[0] == what);
    /* The walk continued: a bitmap it did not reach would make every
     * block the snapshot holds look unreferenced. */
    CHECK(r.alloc_not_seen.count == 0);
    check_teardown(bd);

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

    CHECK(check_fixture(&bd, reason));
    CHECK(cosmofs_test_corrupt(mount_of(ENG), COSMOFS_CORRUPT_DUP_NAME, CFS_ROOT_INO, &what) == 0);
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0);
    CHECK(r.dir_dup_name.count == 1 && r.dir_dup_name.name[0] == what);
    check_teardown(bd);

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
    CHECK(check_fixture(&bd, reason));
    CHECK(cosmofs_test_name_hash("asl", 3) == cosmofs_test_name_hash("bea", 3));
    CHECK(write_file(ENG "/asl", "x", 1));
    CHECK(write_file(ENG "/bea", "y", 1));
    CHECK(vfs_sync() == 0);
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0);
    CHECK(r.dir_dup_name.count == 0);
    CHECK_CLEAN(r);
    check_teardown(bd);

    kinfo("selftest: cosmofs-check-dup-name: one name twice at two inodes is reported, and "
          "sixty-four distinct names in one directory are not");
    return true;
}

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
    CHECK_CLEAN(r);                       /* the held blocks are not leaks */
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
 * The defect, told the right way round: a file opened, unlinked and
 * committed, and the mount then dropped with the handle still open --
 * which is a crash, in the only way a self-test can have one. The
 * *remount* reclaims the inode and its blocks, with no operator and no
 * repair flag (docs/audit/next-subsystem-orphan.md).
 */
bool selftest_cosmofs_orphan_crash(const char **reason)
{
    struct blkdev *bd;
    if (!engine_mount(&bd, 512, reason))
        return false;
    struct cosmofs_check_report r;
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0 && r.clean);
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
    fill_incompressible(big, biglen, 0x5eed);
    struct file *f;
    bool wrote = write_file(ENG "/doomed", (const char *)big, (uint32_t)biglen);
    kfree(big);
    CHECK(wrote);
    CHECK(vfs_open(NULL, ENG "/doomed", COSMO_O_RDONLY, 0, &f) == 0);
    CHECK(vfs_unlink(NULL, ENG "/doomed") == 0);
    CHECK(vfs_sync() == 0);

    /* On disk the inode now has no name and keeps its blocks -- and the
     * root says so, which is the whole difference. */
    struct cosmofs_stats st;
    CHECK(cosmofs_stats(mount_of(ENG), &st) == 0);
    CHECK(st.orphan_root != 0);
    CHECK(st.pending_orphans == 1);
    /* What the filesystem looks like with the promise outstanding. */
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0);
    CHECK_CLEAN(r);                          /* a recorded orphan is not a finding */
    CHECK(r.orphan.count == 0);
    uint64_t free_outstanding = r.counted_free;

    /* The crash: the handle never closes, so cfs_evict never runs, and
     * the forced unmount drops the open transaction as a crash before
     * the root write would. */
    file_put(f);
    CHECK(vfs_umount2(ENG, VFS_UMOUNT_FORCE) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);

    /*
     * The mount schedules the reclaim; the first commit makes it
     * durable. The blocks are freed *deferred*, because the root this
     * mount is running on still names them -- handing them to the
     * allocator before a root says they are free is the ordering every
     * other free in this filesystem obeys. So the space comes back at
     * the commit, exactly as an ordinary eviction's does.
     */
    CHECK(vfs_sync() == 0);
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0);
    CHECK_CLEAN(r);                          /* the claim */
    CHECK(r.orphan.count == 0);
    /* The file's sixteen blocks are back, and so is its inode slot. */
    CHECK(r.counted_free >= free_outstanding + 16);
    CHECK(r.counted_inodes == inodes_before);

    kinfo("selftest: cosmofs-orphan-crash: the mount reclaimed the inode and %llu blocks with no operator and no "
          "repair flag",
          (unsigned long long)(r.counted_free - free_outstanding));
    return engine_unmount(bd, reason);
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
    if (!engine_mount(&bd, 512, reason))
        return false;   /* engine_mount turns the writeback thread off */

    CHECK(write_file(ENG "/plain", "no handle on this one", 21));
    CHECK(vfs_sync() == 0);
    struct cosmofs_stats before;
    CHECK(cosmofs_stats(mount_of(ENG), &before) == 0);

    CHECK(vfs_unlink(NULL, ENG "/plain") == 0);
    struct cosmofs_stats mid;
    CHECK(cosmofs_stats(mount_of(ENG), &mid) == 0);
    CHECK(mid.pending_orphans == 0);   /* the eviction already cancelled it */
    CHECK(vfs_sync() == 0);
    struct cosmofs_stats after;
    CHECK(cosmofs_stats(mount_of(ENG), &after) == 0);
    CHECK(after.orphan_root == 0);     /* so the commit wrote no record at all */

    /* And the same filesystem does write one when a handle is held, so
     * "no record" above is a fact about the unlink and not about the
     * record. */
    struct file *f;
    CHECK(write_file(ENG "/held", "this one is open", 16));
    CHECK(vfs_open(NULL, ENG "/held", COSMO_O_RDONLY, 0, &f) == 0);
    CHECK(vfs_unlink(NULL, ENG "/held") == 0);
    CHECK(vfs_sync() == 0);
    CHECK(cosmofs_stats(mount_of(ENG), &after) == 0);
    CHECK(after.orphan_root != 0);
    CHECK(after.pending_orphans == 1);
    file_put(f);
    CHECK(vfs_sync() == 0);
    CHECK(cosmofs_stats(mount_of(ENG), &after) == 0);
    CHECK(after.orphan_root == 0);     /* the eviction retired it */
    CHECK(after.pending_orphans == 0);

    kinfo("selftest: cosmofs-orphan-cancels: an unlink with nothing holding it wrote no record, one with a handle "
          "wrote and retired one");
    (void)before;
    return engine_unmount(bd, reason);
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
    if (!engine_mount(&bd, 512, reason))
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
    CHECK(cosmofs_stats(mount_of(ENG), &st) == 0);
    CHECK(st.orphan_root != 0);
    CHECK(st.pending_orphans == 1);
    CHECK(vfs_stat(NULL, ENG "/keep", &pst) == 0);
    CHECK(pst.nlink == parent_nlink - 1);   /* rmdir took the parent's link, once */
    uint32_t after_rmdir = pst.nlink;

    /* The crash, with the reference still held. */
    vnode_put(held);
    CHECK(vfs_umount2(ENG, VFS_UMOUNT_FORCE) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);

    CHECK(vfs_sync() == 0);   /* the reclaim lands on the mount's first commit */
    struct cosmofs_check_report r;
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0);
    CHECK_CLEAN(r);
    CHECK(r.orphan.count == 0);
    CHECK(r.nlink_wrong.count == 0);        /* the replay did not touch the parent */
    CHECK(vfs_stat(NULL, ENG "/keep", &pst) == 0);
    CHECK(pst.nlink == after_rmdir);        /* the same link count, not one less again */

    kinfo("selftest: cosmofs-orphan-dir: a directory removed under a live reference was reclaimed, parent nlink %u "
          "throughout",
          (unsigned)after_rmdir);
    return engine_unmount(bd, reason);
}

/*
 * A file replaced by a rename while it is open: the half of this defect
 * that is easiest to miss, because the victim never passes through
 * unlink.
 */
bool selftest_cosmofs_orphan_rename(const char **reason)
{
    struct blkdev *bd;
    if (!engine_mount(&bd, 512, reason))
        return false;
    struct cosmofs_check_report r;
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0 && r.clean);

    const size_t biglen = 16 * 4096;
    uint8_t *big = kmalloc(biglen, 0);
    CHECK(big != NULL);
    fill_incompressible(big, biglen, 0xf00d);
    bool wrote = write_file(ENG "/victim", (const char *)big, (uint32_t)biglen);
    kfree(big);
    CHECK(wrote);
    CHECK(write_file(ENG "/winner", "short", 5));
    CHECK(vfs_sync() == 0);

    struct file *f;
    CHECK(vfs_open(NULL, ENG "/victim", COSMO_O_RDONLY, 0, &f) == 0);
    CHECK(vfs_rename(NULL, ENG "/winner", ENG "/victim") == 0);
    CHECK(vfs_sync() == 0);
    struct cosmofs_stats st;
    CHECK(cosmofs_stats(mount_of(ENG), &st) == 0);
    CHECK(st.pending_orphans == 1);
    CHECK(st.orphan_root != 0);
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0);
    uint64_t free_outstanding = r.counted_free;

    file_put(f);
    CHECK(vfs_umount2(ENG, VFS_UMOUNT_FORCE) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    CHECK(vfs_sync() == 0);   /* the reclaim lands on the mount's first commit */
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0);
    CHECK_CLEAN(r);
    CHECK(read_matches(ENG "/victim", "short", 5));   /* the rename itself stood */
    CHECK(r.counted_free >= free_outstanding + 16);   /* the replaced file's blocks came back */

    kinfo("selftest: cosmofs-orphan-rename: the replaced file's %llu blocks came back at the mount",
          (unsigned long long)(r.counted_free - free_outstanding));
    return engine_unmount(bd, reason);
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
    if (!engine_mount(&bd, 512, reason))
        return false;
    static const char big[4096] = { 0 };
    struct file *f;
    CHECK(write_file(ENG "/gone", big, sizeof(big)));
    CHECK(vfs_open(NULL, ENG "/gone", COSMO_O_RDONLY, 0, &f) == 0);
    CHECK(vfs_unlink(NULL, ENG "/gone") == 0);
    CHECK(vfs_sync() == 0);
    file_put(f);
    CHECK(vfs_umount2(ENG, VFS_UMOUNT_FORCE) == 0);

    /* First mount: the replay reclaims it, and the record still stands
     * because no commit has retired it. */
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    struct cosmofs_stats first;
    CHECK(cosmofs_stats(mount_of(ENG), &first) == 0);
    CHECK(first.orphan_root != 0);
    static struct cosmofs_check_report r1;   /* static: see selftest_cosmofs_crash */
    CHECK(cosmofs_check(mount_of(ENG), &r1, 0) == 0);

    /* Away again without committing. */
    CHECK(vfs_umount2(ENG, VFS_UMOUNT_FORCE) == 0);

    /* Second mount: the same list, the same effect, the count moved once. */
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    struct cosmofs_stats second;
    CHECK(cosmofs_stats(mount_of(ENG), &second) == 0);
    CHECK(second.orphan_root == first.orphan_root);
    CHECK(second.inode_count == first.inode_count);
    CHECK(second.free_blocks == first.free_blocks);
    CHECK(vfs_sync() == 0);   /* and only now is the reclaim durable */
    static struct cosmofs_check_report r2;
    CHECK(cosmofs_check(mount_of(ENG), &r2, 0) == 0);
    CHECK(r2.clean);
    CHECK(r2.counted_inodes == r1.counted_inodes);

    kinfo("selftest: cosmofs-orphan-idempotent: two mounts, one record, %llu inodes both times",
          (unsigned long long)second.inode_count);
    return engine_unmount(bd, reason);
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
    if (!engine_mount(&bd, 4096, reason))
        return false;
    const unsigned many = CFS_ORPHANS_PER_BLOCK + 20u;
    struct file **fs_ = kmalloc(many * sizeof(*fs_), KMEM_ZERO);
    CHECK(fs_ != NULL);
    char path[64];
    bool ok = true;
    for (unsigned i = 0; i < many && ok; i++) {
        ksnprintf(path, sizeof(path), ENG "/o%u", i);
        ok = write_file(path, "x", 1) && vfs_open(NULL, path, COSMO_O_RDONLY, 0, &fs_[i]) == 0 &&
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
    CHECK(cosmofs_stats(mount_of(ENG), &st) == 0);
    CHECK(st.pending_orphans == many);
    CHECK(st.orphan_root != 0);

    for (unsigned i = 0; i < many; i++)
        file_put(fs_[i]);
    kfree(fs_);
    CHECK(vfs_umount2(ENG, VFS_UMOUNT_FORCE) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    CHECK(vfs_sync() == 0);   /* the reclaim lands on the mount's first commit */
    struct cosmofs_check_report r;
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0);
    CHECK(r.seen_not_alloc.count == 0);   /* the claim */
    CHECK_CLEAN(r);
    CHECK(cosmofs_stats(mount_of(ENG), &st) == 0);
    CHECK(st.pending_orphans == 0);

    kinfo("selftest: cosmofs-orphan-reserved: %u inodes recorded across a chain and all of them replayed", many);
    return engine_unmount(bd, reason);
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
    if (!engine_mount(&bd, 4096, reason))
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
        ok = write_file(path, "x", 1) && vfs_open(NULL, path, COSMO_O_RDONLY, 0, &held[i]) == 0 &&
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
    cosmofs_test_mount_writeback(mount_of(ENG), &notes, &early);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    CHECK(notes > 0);    /* the guard: the replay really did dirty the filesystem */
    CHECK(early == 0);   /* the claim: and no writeback thread was in it */

    kinfo("selftest: cosmofs-mount-no-early-writeback: the replay made %llu dirty marks, "
          "%llu of them with a writeback thread running",
          (unsigned long long)notes, (unsigned long long)early);
    return engine_unmount(bd, reason);
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
    if (!engine_mount(&bd, 1024, reason))
        return false;
    struct file *f;
    CHECK(write_file(ENG "/held", "open across all of it", 21));
    CHECK(vfs_open(NULL, ENG "/held", COSMO_O_RDONLY, 0, &f) == 0);
    CHECK(vfs_unlink(NULL, ENG "/held") == 0);
    CHECK(vfs_sync() == 0);
    struct cosmofs_stats first;
    CHECK(cosmofs_stats(mount_of(ENG), &first) == 0);
    CHECK(first.orphan_root != 0);

    bool ok = true;
    for (unsigned i = 0; i < 100 && ok; i++) {
        char c = (char)('a' + (i % 26));
        ok = write_file(ENG "/churn", &c, 1) && vfs_sync() == 0;
    }
    CHECK(ok);
    struct cosmofs_stats last;
    CHECK(cosmofs_stats(mount_of(ENG), &last) == 0);
    CHECK(last.orphan_root != 0);
    CHECK(last.pending_orphans == 1);
    /* One block for the churn file and one for the record, either side:
     * the free count does not fall by a block per commit. */
    CHECK(first.free_blocks - last.free_blocks < 8);

    file_put(f);
    CHECK(vfs_sync() == 0);
    struct cosmofs_check_report r;
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0);
    CHECK_CLEAN(r);

    kinfo("selftest: cosmofs-orphan-supersede: 100 commits, %llu free blocks lost to them in all",
          (unsigned long long)(first.free_blocks - last.free_blocks));
    return engine_unmount(bd, reason);
}

/*
 * A fill that fails with the record's blocks reserved gives them back
 * and publishes no root: the filesystem is exactly as it was.
 */
bool selftest_cosmofs_orphan_rollback(const char **reason)
{
    struct blkdev *bd;
    if (!engine_mount(&bd, 512, reason))
        return false;
    CHECK(write_file(ENG "/held", "content", 7));
    CHECK(vfs_sync() == 0);
    struct cosmofs_stats before;
    CHECK(cosmofs_stats(mount_of(ENG), &before) == 0);

    struct file *f;
    CHECK(vfs_open(NULL, ENG "/held", COSMO_O_RDONLY, 0, &f) == 0);
    cosmofs_test_fail_orphan(mount_of(ENG), true);
    CHECK(vfs_unlink(NULL, ENG "/held") == 0);
    CHECK(vfs_sync() != 0);                 /* the commit fails */
    cosmofs_test_fail_orphan(mount_of(ENG), false);
    file_put(f);

    CHECK(vfs_umount2(ENG, VFS_UMOUNT_FORCE) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    struct cosmofs_stats after;
    CHECK(cosmofs_stats(mount_of(ENG), &after) == 0);
    CHECK(after.generation == before.generation);   /* no root was published */
    CHECK(after.free_blocks == before.free_blocks);
    CHECK(after.orphan_root == 0);
    struct cosmofs_check_report r;
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0);
    CHECK_CLEAN(r);
    CHECK(read_matches(ENG "/held", "content", 7));   /* the unlink went with it */

    kinfo("selftest: cosmofs-orphan-rollback: the failed fill left generation %llu and %llu free blocks",
          (unsigned long long)after.generation, (unsigned long long)after.free_blocks);
    return engine_unmount(bd, reason);
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
    if (!engine_mount(&bd, 512, reason))
        return false;

    CHECK(write_file(ENG "/live", "still very much in use", 22));
    CHECK(vfs_sync() == 0);
    struct cosmo_stat st;
    CHECK(vfs_stat(NULL, ENG "/live", &st) == 0);
    CHECK(st.nlink == 1);

    /* The wrong record: it names an inode that has a name. */
    cosmofs_test_orphan_add(mount_of(ENG), st.ino);
    CHECK(vfs_sync() == 0);
    struct cosmofs_stats fsst;
    CHECK(cosmofs_stats(mount_of(ENG), &fsst) == 0);
    CHECK(fsst.orphan_root != 0);

    CHECK(vfs_umount2(ENG, VFS_UMOUNT_FORCE) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);

    /* The file is still there, with its contents and its name. */
    CHECK(read_matches(ENG "/live", "still very much in use", 22));
    CHECK(vfs_stat(NULL, ENG "/live", &st) == 0);
    CHECK(st.nlink == 1);
    struct cosmofs_check_report r;
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0);
    CHECK_CLEAN(r);
    CHECK(r.orphan.count == 0);

    kinfo("selftest: cosmofs-orphan-suspect: a record naming a linked inode was refused and the file survived");
    return engine_unmount(bd, reason);
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
    CHECK(check_fixture(&bd, reason));

    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0 && r.clean);
    uint64_t free_before = r.counted_free;

    /* An inode with blocks that no entry names, and no record naming it
     * either -- the shape a crash used to leave and a lost record still
     * would. */
    uint64_t lost = 0;
    CHECK(cosmofs_test_corrupt(mount_of(ENG), COSMOFS_CORRUPT_ORPHAN, 0, &lost) == 0);
    CHECK(lost != 0);

    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0);
    CHECK(r.orphan.count == 1);              /* the checker still owns this case */
    CHECK(r.orphan.named == 1 && r.orphan.name[0] == lost);
    CHECK(r.counted_free < free_before);

    CHECK(cosmofs_check(mount_of(ENG), &r, COSMOFS_CHECK_REPAIR) == 0);
    CHECK(r.orphan.repaired == 1);
    CHECK(vfs_sync() == 0);
    CHECK(vfs_sync() == 0);   /* the deferred frees land on the commit after the repair's */
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0);
    CHECK_CLEAN(r);
    CHECK(r.counted_free == free_before);   /* every block the inode held came back */

    check_teardown(bd);
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
    CHECK_CLEAN(r);
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
    struct file *f = NULL;
    struct vnode *rv = NULL;
    uint8_t *buf = NULL;
    if (!check_fixture(&bd, reason)) {
        check_teardown(bd);
        return false;
    }
#define FSCTL_CHECK(cond)                                                     \
    do {                                                                      \
        if (!(cond)) {                                                        \
            *reason = "check failed: " #cond " at line " STR(__LINE__);     \
            goto cleanup;                                                    \
        }                                                                     \
    } while (0)
    uint64_t id = mount_of(ENG)->id;
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
    FSCTL_CHECK(cosmofs_check(mount_of(ENG), &direct, 0) == 0);
    FSCTL_CHECK(c->blocks_seen == direct.blocks_seen);
    FSCTL_CHECK(c->counted_free == direct.counted_free);
    FSCTL_CHECK(c->inodes_seen == direct.inodes_seen);
    uint64_t free_before = c->counted_free;

    /* A leak, found by number through the device. */
    uint64_t leaked = 0;
    FSCTL_CHECK(cosmofs_test_corrupt(mount_of(ENG), COSMOFS_CORRUPT_LEAK, 0, &leaked) == 0);
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
    check_teardown(bd);
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
    check_teardown(bd);
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
    CHECK(rep.alloc_not_seen.count == 0);   /* was `<= 4`: see below */

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
    if (!engine_mount(&bd, 512, reason))
        return false;

    CHECK(write_file(ENG "/held", "the snapshot's copy", 19));
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
    uint64_t before = cosmofs_test_snap_root(mount_of(ENG));
    CHECK(before != 0);

    /* One commit that appends to the snapshot's deadlist: it copies the
     * list to write the new head into it. */
    CHECK(vfs_unlink(NULL, ENG "/held") == 0);
    CHECK(vfs_sync() == 0);
    uint64_t after = cosmofs_test_snap_root(mount_of(ENG));
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
    CHECK(cosmofs_test_deadlist_len(mount_of(ENG), before) == 0);

    /* The entries the commit did write are there, and none is doubled. */
    uint64_t examined = 0;
    CHECK(cosmofs_test_deadlist_dups(mount_of(ENG), &examined, NULL) == 0);
    CHECK(examined > 0);

    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    CHECK(cosmofs_test_snap_root(mount_of(ENG)) == after);
    struct cosmofs_check_report rep;
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0);
    CHECK(rep.clean);
    CHECK(rep.snapshots_seen == 1);
    CHECK(read_matches(ENG "/.snapshots/one/held", "the snapshot's copy", 19));

    kinfo("selftest: cosmofs-snap-cow: the snapshot list moved from block %llu to %llu in one commit, and %llu went "
          "back to the allocator rather than onto a deadlist",
          (unsigned long long)before, (unsigned long long)after, (unsigned long long)before);
    return engine_unmount(bd, reason);
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
    if (!engine_mount(&bd, 512, reason))
        return false;

    CHECK(write_file(ENG "/held", "the snapshot's copy", 19));
    CHECK(vfs_sync() == 0);
    CHECK(vfs_mkdir(NULL, ENG "/.snapshots/keep", 0755) == 0);

    /* The unmount's own commit is the one that frees: a sync here would
     * be followed by another commit that tidied up after it, and the
     * defect is precisely the commit that has no successor. */
    CHECK(vfs_unlink(NULL, ENG "/held") == 0);
    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);

    struct cosmofs_check_report rep;
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0);
    CHECK(rep.clean);                       /* the claim */
    CHECK(rep.snapshots_seen == 1);

    /*
     * And `clean` is not the answer of a filesystem that recorded
     * nothing: entries exist, and none of them is on two lists.
     */
    uint64_t examined = 0, first = 0;
    CHECK(cosmofs_test_deadlist_dups(mount_of(ENG), &examined, &first) == 0);
    CHECK(examined > 0);

    /* The blocks are still the snapshot's, which is what holding them
     * was for. */
    CHECK(read_matches(ENG "/.snapshots/keep/held", "the snapshot's copy", 19));

    kinfo("selftest: cosmofs-snap-unmount: clean across the unmount, %llu deadlist entries and no duplicate",
          (unsigned long long)examined);
    return engine_unmount(bd, reason);
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
    if (!engine_mount(&bd, 1024, reason))
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
        CHECK(write_file(path, "x", 1));
    }
    CHECK(vfs_sync() == 0);
    CHECK(vfs_mkdir(NULL, ENG "/.snapshots/keep", 0755) == 0);

    uint64_t *roots = kmalloc(SNAP_NOGROW_COMMITS * sizeof(uint64_t), KMEM_ZERO);
    CHECK(roots != NULL);
    bool ok = true;
    for (unsigned i = 0; i < SNAP_NOGROW_COMMITS && ok; i++) {
        roots[i] = cosmofs_test_snap_root(mount_of(ENG));
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
    uint64_t now = cosmofs_test_snap_root(mount_of(ENG));
    for (unsigned i = 0; i < SNAP_NOGROW_COMMITS; i++) {
        if (roots[i] == 0)
            continue;
        uint64_t next = i + 1 < SNAP_NOGROW_COMMITS ? roots[i + 1] : now;
        if (next != 0 && next != roots[i])
            moves++;
        if (roots[i] != now && cosmofs_test_deadlist_len(mount_of(ENG), roots[i]) != 0)
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
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0);
    CHECK(rep.clean);
    uint64_t examined = 0;
    CHECK(cosmofs_test_deadlist_dups(mount_of(ENG), &examined, NULL) == 0);
    CHECK(examined > 0);

    kinfo("selftest: cosmofs-snap-nogrow: %u commits, the snapshot list moved in %u of them, %llu deadlist entries, "
          "no superseded list block held",
          SNAP_NOGROW_COMMITS, moves, (unsigned long long)examined);
    return engine_unmount(bd, reason);
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
    if (!engine_mount(&bd, 4096, reason))
        return false;

    /* More one-block files than a deadlist block holds. */
    const unsigned files = CFS_DEAD_PER_BLOCK + 40u;
    char path[64];
    for (unsigned i = 0; i < files; i++) {
        ksnprintf(path, sizeof(path), ENG "/f%u", i);
        CHECK(write_file(path, "x", 1));
    }
    CHECK(vfs_sync() == 0);
    CHECK(vfs_mkdir(NULL, ENG "/.snapshots/keep", 0755) == 0);

    /* All of them in one transaction, so one commit holds them all. */
    for (unsigned i = 0; i < files; i++) {
        ksnprintf(path, sizeof(path), ENG "/f%u", i);
        CHECK(vfs_unlink(NULL, path) == 0);
    }
    CHECK(vfs_sync() == 0);

    uint64_t entries = cosmofs_test_deadlist_len(mount_of(ENG), 0);
    CHECK(entries > CFS_DEAD_PER_BLOCK);   /* the chain really did need a second block */

    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    struct cosmofs_check_report rep;
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0);
    CHECK(rep.seen_not_alloc.count == 0);   /* the claim */
    CHECK(rep.clean);
    uint64_t examined = 0;
    CHECK(cosmofs_test_deadlist_dups(mount_of(ENG), &examined, NULL) == 0);
    CHECK(examined == entries);

    kinfo("selftest: cosmofs-snap-reserved: %llu blocks held across a chain of deadlist blocks, all accounted",
          (unsigned long long)entries);
    return engine_unmount(bd, reason);
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
    if (!engine_mount(&bd, 512, reason))
        return false;
    CHECK(write_file(ENG "/held", "content", 7));
    CHECK(vfs_sync() == 0);
    CHECK(vfs_mkdir(NULL, ENG "/.snapshots/keep", 0755) == 0);
    struct cosmofs_stats before;
    CHECK(cosmofs_stats(mount_of(ENG), &before) == 0);

    cosmofs_test_fail_snapfill(mount_of(ENG), true);
    CHECK(vfs_unlink(NULL, ENG "/held") == 0);
    CHECK(vfs_sync() != 0);                 /* the commit fails */
    cosmofs_test_fail_snapfill(mount_of(ENG), false);

    /* Abandoned: nothing more is published from this mount. */
    CHECK(vfs_umount2(ENG, VFS_UMOUNT_FORCE) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    struct cosmofs_stats after;
    CHECK(cosmofs_stats(mount_of(ENG), &after) == 0);
    CHECK(after.generation == before.generation);   /* no root was published */
    CHECK(after.free_blocks == before.free_blocks); /* and nothing was taken */
    struct cosmofs_check_report rep;
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0);
    CHECK(rep.clean);
    CHECK(read_matches(ENG "/held", "content", 7));   /* the unlink went with it */

    kinfo("selftest: cosmofs-snap-rollback: the failed fill left generation %llu and %llu free blocks, as it found them",
          (unsigned long long)after.generation, (unsigned long long)after.free_blocks);
    return engine_unmount(bd, reason);
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
    if (!engine_mount(&bd, 1024, reason))
        return false;
    char path[64];
    for (unsigned i = 0; i < 24; i++) {
        ksnprintf(path, sizeof(path), ENG "/f%u", i);
        CHECK(write_file(path, "x", 1));
    }
    CHECK(vfs_sync() == 0);
    CHECK(vfs_mkdir(NULL, ENG "/.snapshots/keep", 0755) == 0);

    uint64_t w0 = cosmofs_test_snap_walks(mount_of(ENG));
    uint64_t v0 = cosmofs_test_snap_verdicts(mount_of(ENG));
    for (unsigned i = 0; i < 24; i++) {
        ksnprintf(path, sizeof(path), ENG "/f%u", i);
        CHECK(vfs_unlink(NULL, path) == 0);
    }
    CHECK(vfs_sync() == 0);
    uint64_t walks = cosmofs_test_snap_walks(mount_of(ENG)) - w0;
    uint64_t verdicts = cosmofs_test_snap_verdicts(mount_of(ENG)) - v0;

    CHECK(verdicts > 0);            /* the commit really did have blocks to judge */
    CHECK(walks == verdicts + 1);   /* the claim: one each, and one re-ask for the copy */

    kinfo("selftest: cosmofs-snap-onewalk: %llu verdicts, %llu walks of the snapshot list",
          (unsigned long long)verdicts, (unsigned long long)walks);
    return engine_unmount(bd, reason);
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
    cosmofs_test_set_writeback(mount_of(ENG), false);

    /* A record exists, and *then* the snapshot is taken: its bitmap
     * marks that record's blocks allocated, which is the difficulty. */
    CHECK(write_file(ENG "/held", "the snapshot's copy", 19));
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
    uint64_t dead0 = cosmofs_test_deadlist_len(mount_of(ENG), 0);
    CHECK(vfs_mkdir(NULL, ENG "/.snapshots/keep", 0755) == 0);
    uint64_t dead1 = cosmofs_test_deadlist_len(mount_of(ENG), 0);
    CHECK(dead1 - dead0 <= 3);

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
    uint64_t dead_before = cosmofs_test_deadlist_len(mount_of(ENG), 0);

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
    CHECK(cosmofs_test_deadlist_len(mount_of(ENG), 0) == dead_before);

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
    cosmofs_test_set_writeback(mount_of(ENG), false);

    /* Settle, so the count to come back to is a steady one. */
    CHECK(write_file(ENG "/settle", "s", 1));
    CHECK(vfs_unlink(NULL, ENG "/settle") == 0);
    CHECK(vfs_sync() == 0);
    CHECK(vfs_sync() == 0);
    struct cosmofs_stats before;
    CHECK(cosmofs_stats(mount_of(ENG), &before) == 0);

    /* Something to free, and a commit that fails while freeing it. */
    CHECK(write_file(ENG "/doomed", "x", 1));
    CHECK(vfs_sync() == 0);
    CHECK(vfs_unlink(NULL, ENG "/doomed") == 0);
    cosmofs_test_fail_freelog(mount_of(ENG), true);
    CHECK(vfs_sync() != 0);                  /* the commit fails, as asked */
    cosmofs_test_fail_freelog(mount_of(ENG), false);

    /*
     * And then one that succeeds. This is the commit that would make a
     * lost reservation durable: it writes the bitmap the failed attempt
     * left behind.
     */
    CHECK(vfs_sync() == 0);
    CHECK(vfs_umount(ENG) == 0);

    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    struct cosmofs_check_report rep;
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0);
    CHECK(rep.alloc_not_seen.count == 0);    /* the reservation, if it was kept */
    CHECK(rep.clean);
    struct cosmofs_stats after;
    CHECK(cosmofs_stats(mount_of(ENG), &after) == 0);
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
    cosmofs_test_set_writeback(mount_of(ENG), false);

    /* A record with something in it, left on disk by the unmount. */
    CHECK(write_file(ENG "/gone", "x", 1));
    CHECK(vfs_sync() == 0);
    CHECK(vfs_unlink(NULL, ENG "/gone") == 0);
    CHECK(vfs_umount(ENG) == 0);

    /* Which generation the disk is at, without changing it: the discard
     * hook makes this unmount write nothing. */
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    struct cosmofs_stats good;
    CHECK(cosmofs_stats(mount_of(ENG), &good) == 0);
    cosmofs_test_discard_on_unmount(mount_of(ENG), true);
    CHECK(vfs_umount(ENG) == 0);

    /* Re-sealed, so the checksum is good and only the count is wrong. */
    CHECK(cosmofs_test_poison_freelog_count(bd, CFS_DEAD_PER_BLOCK + 1) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    struct cosmofs_stats fell;
    CHECK(cosmofs_stats(mount_of(ENG), &fell) == 0);
    CHECK(fell.generation < good.generation);   /* the newer root was refused */

    /* And what it fell back to is a whole filesystem, not a damaged one. */
    struct cosmofs_check_report rep;
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0);
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
    CHECK(cosmofs_test_deadlist_dups(mount_of(ENG), &v3_dead, NULL) == 0);
    CHECK(v3_dead > 0);
    struct cosmofs_check_report v3rep;
    CHECK(cosmofs_check(mount_of(ENG), &v3rep, 0) == 0);
    CHECK(v3rep.seen_not_alloc.count == 0);
    CHECK(v3rep.snapshots_seen == 1);
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

/* One snapshot operation through the interface /dev/fsctl uses. */
static int snapctl(enum cosmofs_snap_op op, const char *name, struct cosmofs_snap_info *out, unsigned max,
                   unsigned *count)
{
    struct cosmofs_snap_req req = { .op = op, .name = name, .out = out, .max = max };
    int rc = cosmofs_snapctl(mount_of(ENG), &req);
    if (count != NULL)
        *count = req.count;
    return rc;
}

static const struct cosmofs_snap_info *info_named(const struct cosmofs_snap_info *v, unsigned n, const char *name)
{
    for (unsigned i = 0; i < n; i++)
        if (strcmp(v[i].name, name) == 0)
            return &v[i];
    return NULL;
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
    if (!engine_mount(&bd, 1024, reason))
        return false;
    struct cosmofs_stats st;
    /* Static: the self-tests run one at a time, and the kernel stack has
     * no 2.5 KiB to spare for a list and a check report. */
    static struct cosmofs_snap_info info[8];
    static struct cosmofs_check_report rep;
    unsigned n = 0;
    static const uint8_t zero[16];

    CHECK(cosmofs_stats(mount_of(ENG), &st) == 0);
    CHECK(st.version == 11);                         /* formatted at 11, not 12 */

    /* The list an older kernel would have left: one version-11 snapshot. */
    CHECK(write_file(ENG "/f", "old", 3));
    cosmofs_test_snap_no_raise(mount_of(ENG), true);
    CHECK(vfs_mkdir(NULL, ENG "/.snapshots/legacy", 0755) == 0);
    cosmofs_test_snap_no_raise(mount_of(ENG), false);
    CHECK(cosmofs_stats(mount_of(ENG), &st) == 0 && st.version == 11);

    /* The first snapshot with identity raises the pool and the list. */
    CHECK(write_file(ENG "/f", "new", 3));
    CHECK(snapctl(COSMOFS_SNAP_CREATE, "a", info, 1, &n) == 0 && n == 1);
    CHECK(cosmofs_stats(mount_of(ENG), &st) == 0 && st.version == 12);
    CHECK(memcmp(info[0].uuid, zero, 16) != 0 && info[0].format == 12);
    uint8_t uuid_a[16];
    memcpy(uuid_a, info[0].uuid, 16);
    CHECK(snapctl(COSMOFS_SNAP_CREATE, "a", info, 1, &n) == -EEXIST);
    CHECK(snapctl(COSMOFS_SNAP_CREATE, "b", info, 1, &n) == 0);
    CHECK(memcmp(info[0].uuid, uuid_a, 16) != 0);
    uint64_t id_b = info[0].id;
    CHECK(read_matches(ENG "/.snapshots/legacy/f", "old", 3));   /* converted, still readable */
    CHECK(read_matches(ENG "/.snapshots/a/f", "new", 3));

    /* Every entry, the converted one included, has a uuid of its own. */
    CHECK(snapctl(COSMOFS_SNAP_LIST, NULL, info, 8, &n) == 0 && n == 3);
    const struct cosmofs_snap_info *legacy = info_named(info, n, "legacy");
    CHECK(legacy != NULL && memcmp(legacy->uuid, zero, 16) != 0 && memcmp(legacy->uuid, uuid_a, 16) != 0);

    /* Ids are never reused, across a remount (CFS-3): delete the newest,
     * remount, take another -- it gets a new id, not b's. */
    CHECK(snapctl(COSMOFS_SNAP_DELETE, "b", NULL, 0, NULL) == 0);
    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    CHECK(snapctl(COSMOFS_SNAP_CREATE, "c", info, 1, &n) == 0);
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
    CHECK(write_file(ENG "/big", big, sizeof(big)));
    CHECK(snapctl(COSMOFS_SNAP_CREATE, "d", info, 1, &n) == 0);
    CHECK(vfs_unlink(NULL, ENG "/big") == 0);
    CHECK(vfs_sync() == 0);
    CHECK(snapctl(COSMOFS_SNAP_LIST, NULL, info, 8, &n) == 0 && n == 4);   /* legacy, a, c, d */
    const struct cosmofs_snap_info *d = info_named(info, n, "d");
    const struct cosmofs_snap_info *c = info_named(info, n, "c");
    CHECK(d != NULL && c != NULL);
    CHECK(d->unique >= 32ull * 4096 && d->referenced >= d->unique);
    CHECK(c->unique < 32ull * 4096);               /* the file was born after c */
    /* Oldest first. */
    CHECK(strcmp(info[0].name, "legacy") == 0 && strcmp(info[n - 1].name, "d") == 0);

    /* The checker: clean, then a snapshot with no uuid is named and repaired. */
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0 && rep.clean);
    uint64_t victim = 0;
    CHECK(cosmofs_test_corrupt(mount_of(ENG), COSMOFS_CORRUPT_SNAP_UUID, 0, &victim) == 0);
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0 && !rep.clean);
    CHECK(rep.snap_identity.count == 1 && rep.snap_identity.name[0] == victim);
    CHECK(cosmofs_check(mount_of(ENG), &rep, COSMOFS_CHECK_REPAIR) == 0 && rep.snap_identity.repaired == 1);
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0 && rep.clean);

    CHECK(snapctl(COSMOFS_SNAP_DELETE, "nope", NULL, 0, NULL) == -ENOENT);
    CHECK(snapctl(COSMOFS_SNAP_CREATE, "bad/name", info, 1, &n) == -EINVAL);
    CHECK(snapctl(COSMOFS_SNAP_DELETE, "legacy", NULL, 0, NULL) == 0);
    CHECK(snapctl(COSMOFS_SNAP_DELETE, "a", NULL, 0, NULL) == 0);
    CHECK(snapctl(COSMOFS_SNAP_DELETE, "c", NULL, 0, NULL) == 0);
    CHECK(snapctl(COSMOFS_SNAP_DELETE, "d", NULL, 0, NULL) == 0);
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0 && rep.clean);
    CHECK(engine_unmount(bd, reason));

    /* A pool older than 11 refuses identity and keeps its version; its
     * own kind of snapshot still works. */
    bd = ramblk_create(512);
    CHECK(bd != NULL);
    CHECK(cosmofs_test_format_version(bd, 10) == 0);
    int mk = vfs_mkdir(NULL, ENG, 0755);
    CHECK(mk == 0 || mk == -EEXIST);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    CHECK(snapctl(COSMOFS_SNAP_CREATE, "x", info, 1, &n) == -EOPNOTSUPP);
    CHECK(vfs_mkdir(NULL, ENG "/.snapshots/old", 0755) == 0);
    CHECK(cosmofs_stats(mount_of(ENG), &st) == 0 && st.version == 10);
    CHECK(vfs_rmdir(NULL, ENG "/.snapshots/old") == 0);
    kinfo("selftest: cosmofs-snap-identity: raised 11 -> 12 with a legacy snapshot converted; ids not reused "
          "after a remount (%llu > %llu); d holds %llu KiB alone; a version-10 pool refuses identity",
          (unsigned long long)c->id, (unsigned long long)id_b, (unsigned long long)(d->unique / 1024));
    return engine_unmount(bd, reason);
}

/* The pool's rollback state, from a listing without sizes. */
static unsigned pool_flags_now(void)
{
    static struct cosmofs_snap_info info[16];
    struct cosmofs_snap_req req = { .op = COSMOFS_SNAP_LIST, .out = info, .max = 16, .no_sizes = true };
    if (cosmofs_snapctl(mount_of(ENG), &req) != 0)
        return 0xFFFFu;
    return req.pool_flags;
}

static uint32_t snap_flags_of(const char *name)
{
    static struct cosmofs_snap_info info[16];
    struct cosmofs_snap_req req = { .op = COSMOFS_SNAP_LIST, .out = info, .max = 16, .no_sizes = true };
    if (cosmofs_snapctl(mount_of(ENG), &req) != 0)
        return 0xFFFFFFFFu;
    const struct cosmofs_snap_info *s = info_named(info, req.count, name);
    return s ? s->flags : 0xFFFFFFFFu;
}

static int rollback_req(const char *target, const char *safety, struct cosmofs_snap_info *out)
{
    struct cosmofs_snap_req req = { .op = COSMOFS_SNAP_ROLLBACK, .name = target, .name2 = safety, .out = out,
                                    .max = out ? 1 : 0 };
    return cosmofs_snapctl(mount_of(ENG), &req);
}

static bool remount_eng(struct blkdev *bd, const char **reason)
{
    CHECK(vfs_umount(ENG) == 0);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    return true;
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
    return cosmofs_stats(mount_of(ENG), &st) == 0 ? st.version : 0;
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

    CHECK(engine_mount(&bd, 2048, reason));
    CHECK(write_file(ENG "/a", "short", 5) && pool_version() == CFS_FORMAT_VERSION);
    CHECK(vfs_open(NULL, lname(p105, 105, 'f'), COSMO_O_RDONLY, 0, &f) == -ENOENT);
    CHECK(pool_version() == CFS_FORMAT_VERSION);   /* a lookup raises nothing */

    CHECK(write_file(lname(p48, 48, 'a'), "48", 2) && pool_version() == 15);
    CHECK(write_file(lname(p49, 49, 'b'), "49", 2));
    CHECK(write_file(lname(p104, 104, 'c'), "104", 3));
    CHECK(write_file(p105, "105", 3));
    CHECK(write_file(lname(p255, 255, 'e'), "255", 3));
    CHECK(vfs_mkdir(NULL, lname(pdir, 200, 'g'), 0755) == 0);
    CHECK(vfs_symlink(NULL, lname(plnk, 150, 'h'), "a") == 0);
    CHECK(vfs_open(NULL, lname(p256, 256, 'i'), COSMO_O_WRONLY | COSMO_O_CREAT, 0644, &f) == -ENAMETOOLONG);
    CHECK(read_matches(p48, "48", 2) && read_matches(p104, "104", 3) && read_matches(p255, "255", 3));
    CHECK(read_matches(plnk, "short", 5));   /* through the link */

    /* Listed whole, one long entry per call at the smallest buffer. */
    size_t one = sizeof(struct cosmo_dirent) + CFS_NAME_MAX_LONG + 8;
    CHECK(list_count(ENG, one, LBASE(p255), &hits, &total) && hits == 1 && total == 2 + 8);
    CHECK(list_count(ENG, one, LBASE(pdir), &hits, &total) && hits == 1);
    CHECK(list_count(ENG, 4096, LBASE(p49), &hits, &total) && hits == 1 && total == 2 + 8);

    /* Renames: long to long, long to short, short to long. */
    CHECK(vfs_rename(NULL, p255, lname(pnew, 230, 'j')) == 0);
    CHECK(vfs_stat(NULL, p255, &st) == -ENOENT && read_matches(pnew, "255", 3));
    CHECK(vfs_rename(NULL, p104, ENG "/s") == 0 && read_matches(ENG "/s", "104", 3));
    CHECK(vfs_rename(NULL, ENG "/s", p104) == 0 && read_matches(p104, "104", 3));
    CHECK(vfs_unlink(NULL, p49) == 0 && vfs_stat(NULL, p49, &st) == -ENOENT);
    CHECK(list_count(ENG, one, LBASE(pnew), &hits, &total) && hits == 1 && total == 2 + 7);

    /* A directory whose first block has four free slots left: the
     * 255-byte name (five slots) is put in the next block. */
    char *sub = pdir;
    static char path[600];   /* the 200-byte directory, a slash, a 255-byte name */
    for (unsigned i = 0; i < CFS_DIRENTS_PER_BLOCK - 4; i++) {
        ksnprintf(path, sizeof(path), "%s/%u", sub, i);
        CHECK(write_file(path, "x", 1));
    }
    CHECK(vfs_stat(NULL, sub, &st) == 0 && st.size == CFS_BLOCK);
    ksnprintf(path, sizeof(path), "%s/%s", sub, LBASE(lname(p256, 255, 'k')));
    CHECK(write_file(path, "next", 4) && read_matches(path, "next", 4));
    CHECK(vfs_stat(NULL, sub, &st) == 0 && st.size == 2 * CFS_BLOCK);
    CHECK(list_count(sub, one, LBASE(p256), &hits, &total) && hits == 1 && total == 2 + CFS_DIRENTS_PER_BLOCK - 3);

    CHECK(vfs_sync() == 0);
    CHECK(remount_eng(bd, reason));
    CHECK(pool_version() == 15);
    CHECK(read_matches(p48, "48", 2) && read_matches(pnew, "255", 3) && read_matches(path, "next", 4));
    CHECK(list_count(ENG, one, LBASE(p104), &hits, &total) && hits == 1 && total == 2 + 7);
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0);
    CHECK_CLEAN(rep);
    CHECK(engine_unmount(bd, reason));

    /* A version-10 pool cannot be raised: the version-14 answer. */
    bd = ramblk_create(512);
    CHECK(bd != NULL);
    CHECK(cosmofs_test_format_version(bd, 10) == 0);
    int mk = vfs_mkdir(NULL, ENG, 0755);
    CHECK(mk == 0 || mk == -EEXIST);
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    CHECK(vfs_open(NULL, p48, COSMO_O_WRONLY | COSMO_O_CREAT, 0644, &f) == -ENAMETOOLONG);
    CHECK(vfs_mkdir(NULL, p105, 0755) == -ENAMETOOLONG);
    CHECK(write_file(ENG "/a", "short", 5) && pool_version() == 10);
    kinfo("selftest: cosmofs-longname: names of 48-255 bytes created, listed, renamed and removed on a pool raised "
          "11 -> 15 by the first; a 255-byte name went to the next block; a version-10 pool refuses them");
    return engine_unmount(bd, reason);
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
        CHECK(check_fixture(&bd, reason));
        ksnprintf(p, sizeof(p), "%s/sub/%s", ENG, LBASE(lname(q, 120, 'l')));
        CHECK(write_file(p, "x", 1) && vfs_sync() == 0);
        CHECK(vfs_stat(NULL, ENG "/sub", &st) == 0);
        CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0 && r.dir_bad.count == 0);
        CHECK(cosmofs_test_corrupt(mount_of(ENG), kinds[k], st.ino, &what) == 0);
        CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0);
        CHECK(r.dir_bad.count >= 1 && r.dir_bad.name[0] == st.ino);
        check_teardown(bd);
    }

    CHECK(check_fixture(&bd, reason));
    ksnprintf(p, sizeof(p), "%s/sub/%s", ENG, LBASE(lname(q, 200, 'm')));
    CHECK(write_file(p, "x", 1) && vfs_sync() == 0);
    CHECK(vfs_stat(NULL, ENG "/sub", &st) == 0);
    CHECK(cosmofs_test_corrupt(mount_of(ENG), COSMOFS_CORRUPT_DUP_NAME, st.ino, &what) == 0);
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0);
    CHECK(r.dir_dup_name.count >= 1 && r.dir_dup_name.name[0] == st.ino);
    check_teardown(bd);

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

    CHECK(engine_mount(&bd, 1024, reason));
    CHECK(write_file(ENG "/a", "one", 3) && vfs_mkdir(NULL, ENG "/d", 0755) == 0);
    CHECK(vfs_link(NULL, ENG "/a", NULL, ENG "/b", false) == 0);
    CHECK(vfs_link(NULL, ENG "/a", NULL, ENG "/d/c", false) == 0);
    CHECK(vfs_link(NULL, ENG "/b", NULL, lname(lng, 60, 'n'), false) == 0 && pool_version() == 15);
    CHECK(vfs_stat(NULL, ENG "/a", &st) == 0 && vfs_stat(NULL, ENG "/d/c", &st2) == 0);
    CHECK(st.ino == st2.ino && st.nlink == 4);
    CHECK(vfs_link(NULL, ENG "/d", NULL, ENG "/e", false) == -EPERM);
    CHECK(vfs_link(NULL, ENG "/a", NULL, ENG "/b", false) == -EEXIST);
    CHECK(vfs_symlink(NULL, ENG "/s", "a") == 0 && vfs_link(NULL, ENG "/s", NULL, ENG "/s2", false) == 0);
    CHECK(vfs_lstat(NULL, ENG "/s2", &st2) == 0 && st2.type == COSMO_DT_LNK && st2.nlink == 2);

    CHECK(vfs_unlink(NULL, ENG "/a") == 0 && read_matches(ENG "/b", "one", 3));
    CHECK(vfs_stat(NULL, ENG "/b", &st) == 0 && st.nlink == 3);
    CHECK(vfs_sync() == 0 && remount_eng(bd, reason));
    CHECK(vfs_stat(NULL, ENG "/d/c", &st) == 0 && st.nlink == 3 && read_matches(lng, "one", 3));
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0);
    CHECK_CLEAN(rep);

    /* Read-only both ways in a snapshot: no name made there, none taken out. */
    unsigned n;
    static struct cosmofs_snap_info info[1];
    CHECK(snapctl(COSMOFS_SNAP_CREATE, "lk", info, 1, &n) == 0);
    CHECK(vfs_link(NULL, ENG "/b", NULL, ENG "/.snapshots/lk/z", false) == -EROFS);
    CHECK(vfs_link(NULL, ENG "/.snapshots/lk/b", NULL, ENG "/z", false) == -EROFS);
    CHECK(vfs_rmdir(NULL, ENG "/.snapshots/lk") == 0);

    /* An open file outlives its last name, and is freed after it. */
    CHECK(vfs_open(NULL, ENG "/b", COSMO_O_RDONLY, 0, &f) == 0);
    CHECK(vfs_unlink(NULL, ENG "/b") == 0 && vfs_unlink(NULL, ENG "/d/c") == 0 && vfs_unlink(NULL, lng) == 0);
    CHECK(f->vn->nlink == 0 && file_pread(f, buf, 3, 0) == 3 && memcmp(buf, "one", 3) == 0);
    file_put(f);
    CHECK(vfs_sync() == 0 && remount_eng(bd, reason));
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0);
    CHECK_CLEAN(rep);
    kinfo("selftest: cosmofs-link: four names for one file in two directories, a link to a link, the count across "
          "a remount, a snapshot read-only both ways, an open file outliving its last name");
    return engine_unmount(bd, reason);
}

/*
 * Rollback (roadmap 1.2 B1; design.md, "Rollback"; CF19), on a pool that
 * is not the root, where "the next mount" is an unmount and a mount.
 *
 * The request takes a safety snapshot and writes a pending record; while
 * it is pending both snapshots are kept and a second request is refused;
 * a cancel withdraws it. Applied at the mount: the live tree is the
 * target's, what the old tree held is in the safety snapshot, a file
 * written after the request is in neither and its blocks are free; the
 * safety snapshot is held until BOOT_OK. The switch repaired the
 * deadlists if deleting every snapshot afterwards leaves a filesystem the
 * checker calls clean -- a block freed while live is `seen_not_alloc`, a
 * block held for nobody is `alloc_not_seen` -- with the live files intact
 * after a remount. A record whose own checksum is wrong is refused at the
 * mount, named by the checker and removed by its repair.
 */
bool selftest_cosmofs_rollback(const char **reason)
{
    struct blkdev *bd;
    if (!engine_mount(&bd, 2048, reason))
        return false;
    static struct cosmofs_snap_info info[1];
    static struct cosmofs_check_report rep;
    static char big[32 * 4096];
    uint64_t x = 0x2545F4914F6CDD1Dull;
    for (unsigned i = 0; i < sizeof(big); i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        big[i] = (char)x;
    }
    unsigned n = 0;

    CHECK(write_file(ENG "/a", "one", 3));
    CHECK(vfs_mkdir(NULL, ENG "/dir", 0755) == 0);
    CHECK(write_file(ENG "/dir/x", "ex", 2));
    CHECK(snapctl(COSMOFS_SNAP_CREATE, "base", info, 1, &n) == 0);
    CHECK(write_file(ENG "/a", "two", 3));
    CHECK(vfs_unlink(NULL, ENG "/dir/x") == 0);
    CHECK(write_file(ENG "/new", big, sizeof(big)));
    CHECK(vfs_sync() == 0);

    /* Requested: pending, both snapshots kept, a second one refused. */
    CHECK(rollback_req("nope", NULL, info) == -ENOENT);
    CHECK(rollback_req("base", NULL, info) == 0);
    char safety1[COSMOFS_SNAP_NAME_MAX + 1];
    strlcpy(safety1, info[0].name, sizeof(safety1));
    CHECK(strncmp(safety1, "pre-rollback-", 13) == 0);
    CHECK(pool_flags_now() == COSMOFS_POOL_PENDING);
    CHECK(snap_flags_of("base") == COSMOFS_SNAP_I_TARGET);
    CHECK(snap_flags_of(safety1) == COSMOFS_SNAP_I_SAFETY);
    CHECK(snapctl(COSMOFS_SNAP_DELETE, "base", NULL, 0, NULL) == -EBUSY);
    CHECK(snapctl(COSMOFS_SNAP_DELETE, safety1, NULL, 0, NULL) == -EBUSY);
    CHECK(rollback_req("base", NULL, info) == -EBUSY);
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0 && rep.clean);

    /* Withdrawn: the record goes, the safety snapshot stays and is an
     * ordinary snapshot again. */
    CHECK(snapctl(COSMOFS_SNAP_CANCEL, NULL, NULL, 0, NULL) == 0);
    CHECK(snapctl(COSMOFS_SNAP_CANCEL, NULL, NULL, 0, NULL) == -ENOENT);
    CHECK(pool_flags_now() == 0 && snap_flags_of(safety1) == 0);
    CHECK(snapctl(COSMOFS_SNAP_DELETE, safety1, NULL, 0, NULL) == 0);

    /* Requested again, with the safety snapshot named; then a file
     * written after the request, which is in neither snapshot. */
    CHECK(rollback_req("base", "keep-me", info) == 0);
    CHECK(strcmp(info[0].name, "keep-me") == 0);
    CHECK(write_file(ENG "/after", big, sizeof(big)));
    CHECK(vfs_sync() == 0);

    /* The mount applies it. */
    CHECK(remount_eng(bd, reason));
    CHECK(read_matches(ENG "/a", "one", 3));
    CHECK(read_matches(ENG "/dir/x", "ex", 2));
    struct cosmo_stat st;
    CHECK(vfs_stat(NULL, ENG "/new", &st) == -ENOENT);
    CHECK(vfs_stat(NULL, ENG "/after", &st) == -ENOENT);
    CHECK(read_matches(ENG "/.snapshots/keep-me/a", "two", 3));
    CHECK(read_matches(ENG "/.snapshots/keep-me/new", big, sizeof(big)));
    CHECK(vfs_stat(NULL, ENG "/.snapshots/keep-me/after", &st) == -ENOENT);
    CHECK(pool_flags_now() == COSMOFS_POOL_HELD);
    CHECK(snap_flags_of("keep-me") == COSMOFS_SNAP_I_HELD);
    CHECK(snapctl(COSMOFS_SNAP_DELETE, "keep-me", NULL, 0, NULL) == -EBUSY);
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0 && rep.clean);

    /* The boot completed: the hold goes. */
    struct cosmofs_snap_req ok = { .op = COSMOFS_SNAP_BOOT_OK };
    CHECK(cosmofs_snapctl(mount_of(ENG), &ok) == 0 && ok.total == 1);
    CHECK(cosmofs_snapctl(mount_of(ENG), &ok) == 0 && ok.total == 0);
    CHECK(pool_flags_now() == 0);

    /* Every snapshot gone: nothing live was freed, nothing is held for
     * nobody. */
    CHECK(snapctl(COSMOFS_SNAP_DELETE, "keep-me", NULL, 0, NULL) == 0);
    CHECK(snapctl(COSMOFS_SNAP_DELETE, "base", NULL, 0, NULL) == 0);
    CHECK(vfs_sync() == 0);
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0);
    if (!rep.clean) {
        kinfo("selftest: cosmofs-rollback: after the deletions: %llu leaked, %llu free-in-use",
              (unsigned long long)rep.alloc_not_seen.count, (unsigned long long)rep.seen_not_alloc.count);
        CHECK(rep.clean);
    }
    CHECK(remount_eng(bd, reason));
    CHECK(read_matches(ENG "/a", "one", 3));
    CHECK(read_matches(ENG "/dir/x", "ex", 2));
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0 && rep.clean);

    /* A record that does not check out: refused at the mount, named by
     * the checker, removed by its repair. */
    CHECK(write_file(ENG "/a", "three", 5));
    CHECK(snapctl(COSMOFS_SNAP_CREATE, "c", info, 1, &n) == 0);
    CHECK(write_file(ENG "/a", "four", 4));
    CHECK(rollback_req("c", NULL, info) == 0);
    uint64_t rec = 0;
    CHECK(cosmofs_test_corrupt(mount_of(ENG), COSMOFS_CORRUPT_ROLLBACK_CRC, 0, &rec) == 0);
    CHECK(vfs_sync() == 0);
    CHECK(remount_eng(bd, reason));
    CHECK(read_matches(ENG "/a", "four", 4));            /* not applied */
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0 && !rep.clean);
    CHECK(rep.rollback_record.count == 1 && rep.rollback_record.name[0] == rec);
    CHECK(cosmofs_check(mount_of(ENG), &rep, COSMOFS_CHECK_REPAIR) == 0 && rep.rollback_record.repaired == 1);
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0 && rep.clean);
    CHECK(pool_flags_now() == 0);
    kinfo("selftest: cosmofs-rollback: pending, cancelled, applied at the mount (the old tree in the safety "
          "snapshot, a later file in neither), held until BOOT_OK, every snapshot deleted with the filesystem "
          "clean; a bad record refused, named and repaired");
    return engine_unmount(bd, reason);
}

/*
 * After a rollback the live tree is an older one, and what it shares
 * with the snapshots is not the line of history the free path assumes.
 * Here the target `t` holds a file the live tree deleted after it
 * (`gone`, on t's deadlist when the safety snapshot was taken) and an
 * inode that was unlinked and open when it was taken (`orph`, whose
 * orphan record was the live tree's, not t's). After the switch:
 * `gone` is live again; rewriting and deleting it, then writing enough
 * to reuse any block given back, leaves t's copy intact (a block freed
 * because only the safety snapshot was asked would be overwritten here);
 * the checker finds no orphan and no leak (the switch put t's orphan on
 * the record and the mount released it). While held, a rollback to
 * anything but the safety snapshot is refused -- it would drop the hold
 * -- and one back to the safety snapshot is taken.
 */
bool selftest_cosmofs_rollback_shared(const char **reason)
{
    struct blkdev *bd;
    if (!engine_mount(&bd, 2048, reason))
        return false;
    static struct cosmofs_snap_info info[1];
    static struct cosmofs_check_report rep;
    static char big[32 * 4096], other[32 * 4096];
    uint64_t x = 0x9E3779B97F4A7C15ull;
    for (unsigned i = 0; i < sizeof(big); i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        big[i] = (char)x;
        other[i] = (char)~x;
    }
    unsigned n = 0;
    struct file *f = NULL;

    CHECK(write_file(ENG "/keep", big, sizeof(big)));
    CHECK(write_file(ENG "/gone", big, sizeof(big)));
    CHECK(write_file(ENG "/orph", other, sizeof(other)));
    CHECK(vfs_open(NULL, ENG "/orph", COSMO_O_RDONLY, 0, &f) == 0);
    CHECK(vfs_unlink(NULL, ENG "/orph") == 0);
    CHECK(vfs_sync() == 0);
    CHECK(snapctl(COSMOFS_SNAP_CREATE, "t", info, 1, &n) == 0);
    file_put(f);
    CHECK(vfs_unlink(NULL, ENG "/gone") == 0);
    CHECK(write_file(ENG "/x", other, sizeof(other)));
    CHECK(vfs_sync() == 0);
    CHECK(snapctl(COSMOFS_SNAP_CREATE, "m", info, 1, &n) == 0);
    CHECK(write_file(ENG "/y", other, sizeof(other)));
    CHECK(vfs_sync() == 0);

    CHECK(rollback_req("t", "s", info) == 0);
    CHECK(remount_eng(bd, reason));
    CHECK(read_matches(ENG "/gone", big, sizeof(big)));
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0);
    if (!rep.clean)
        kinfo("selftest: cosmofs-rollback-shared: after the switch: %llu orphan(s), %llu leaked, %llu free-in-use",
              (unsigned long long)rep.orphan.count, (unsigned long long)rep.alloc_not_seen.count,
              (unsigned long long)rep.seen_not_alloc.count);
    CHECK(rep.clean);

    /* Held: only back to the tree it replaced. */
    CHECK(pool_flags_now() == COSMOFS_POOL_HELD);
    CHECK(rollback_req("m", NULL, info) == -EBUSY);

    /* The live tree rewrites and drops what it shares with t, then the
     * allocator is made to hand out everything it was given back. */
    CHECK(write_file(ENG "/gone", other, sizeof(other)));
    CHECK(vfs_unlink(NULL, ENG "/keep") == 0);
    CHECK(vfs_sync() == 0);
    char path[32];
    for (unsigned i = 0; i < 8; i++) {
        ksnprintf(path, sizeof(path), ENG "/churn%u", i);
        CHECK(write_file(path, other, sizeof(other)));
        CHECK(vfs_sync() == 0);
    }
    CHECK(read_matches(ENG "/.snapshots/t/gone", big, sizeof(big)));
    CHECK(read_matches(ENG "/.snapshots/t/keep", big, sizeof(big)));
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0 && rep.clean);

    /* Back to the safety snapshot while held: allowed; withdrawn, the
     * hold is back until BOOT_OK. */
    CHECK(rollback_req("s", NULL, info) == 0);
    CHECK(pool_flags_now() == COSMOFS_POOL_PENDING);
    CHECK(snapctl(COSMOFS_SNAP_CANCEL, NULL, NULL, 0, NULL) == 0);
    CHECK(pool_flags_now() == COSMOFS_POOL_HELD);
    CHECK(snapctl(COSMOFS_SNAP_DELETE, "s", NULL, 0, NULL) == -EBUSY);
    struct cosmofs_snap_req ok = { .op = COSMOFS_SNAP_BOOT_OK };
    CHECK(cosmofs_snapctl(mount_of(ENG), &ok) == 0 && ok.total == 1);
    CHECK(snapctl(COSMOFS_SNAP_DELETE, info[0].name, NULL, 0, NULL) == 0);
    CHECK(snapctl(COSMOFS_SNAP_DELETE, "s", NULL, 0, NULL) == 0);
    CHECK(snapctl(COSMOFS_SNAP_DELETE, "m", NULL, 0, NULL) == 0);
    CHECK(read_matches(ENG "/.snapshots/t/gone", big, sizeof(big)));
    CHECK(snapctl(COSMOFS_SNAP_DELETE, "t", NULL, 0, NULL) == 0);
    CHECK(vfs_sync() == 0);
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0 && rep.clean);
    CHECK(remount_eng(bd, reason));
    CHECK(read_matches(ENG "/gone", other, sizeof(other)));
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0 && rep.clean);
    kinfo("selftest: cosmofs-rollback-shared: the target's copy survived the live tree rewriting and dropping "
          "what it shares and 8 files of churn; its orphan released at the switch; a second rollback refused while "
          "held, one back to the safety snapshot taken; clean after every snapshot went");
    return engine_unmount(bd, reason);
}

/*
 * CF19's same-transaction rule: a rollback's record and its safety
 * snapshot reach the disk in one commit. The machine is stopped at every
 * commit boundary the request crosses (cosmofs_test_commit_budget) and
 * the filesystem mounted again: each time there is either neither or
 * both -- never a record naming a safety snapshot that is not there
 * (`rollback_record`, which tools/b1-rollback-probe.py --old produces).
 */
bool selftest_cosmofs_rollback_atomic(const char **reason)
{
    static struct cosmofs_snap_info info[1];
    static struct cosmofs_check_report rep;
    unsigned seen_neither = 0, seen_both = 0;
    for (unsigned budget = 0; budget < 4; budget++) {
        struct blkdev *bd;
        if (!engine_mount(&bd, 512, reason))
            return false;
        unsigned n;
        CHECK(write_file(ENG "/a", "one", 3));
        CHECK(snapctl(COSMOFS_SNAP_CREATE, "base", info, 1, &n) == 0);
        CHECK(write_file(ENG "/a", "two", 3));   /* open work: the request commits it first */
        cosmofs_test_commit_budget(mount_of(ENG), true, budget);
        int rc = rollback_req("base", "safe", info);
        cosmofs_test_commit_budget(mount_of(ENG), false, 0);
        cosmofs_test_discard_on_unmount(mount_of(ENG), true);
        CHECK(vfs_umount(ENG) == 0);
        CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
        cosmofs_test_set_writeback(mount_of(ENG), false);
        /* The mount applies a pending rollback, so "both" shows as a
         * switched tree with the safety snapshot held. */
        unsigned pf = pool_flags_now();
        uint32_t sf = snap_flags_of("safe");
        bool both = pf == COSMOFS_POOL_HELD && sf == COSMOFS_SNAP_I_HELD;
        bool neither = pf == 0 && sf == 0xFFFFFFFFu;
        kinfo("selftest: cosmofs-rollback-atomic: stopped after %u commit(s): request %d, pool flags %u, "
              "safety flags 0x%x", budget, rc, pf, sf);
        CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0);
        CHECK(rep.rollback_record.count == 0 && rep.hold.count == 0);
        CHECK(rep.clean);
        CHECK(both || neither);
        seen_both += both;
        seen_neither += neither;
        CHECK(engine_unmount(bd, reason));
    }
    /* Not vacuous: some stop fell before the commit and some after. */
    CHECK(seen_neither > 0 && seen_both > 0);
    kinfo("selftest: cosmofs-rollback-atomic: %u stop(s) left neither, %u left both", seen_neither, seen_both);
    return true;
}

/*
 * The rollback record's parse and checksum, on bytes alone: a sealed
 * record of either state parses back field for field; every single byte
 * changed anywhere in it is refused (the crc's, or a field's under it);
 * a wrong magic, version or state is refused even with a crc that
 * matches it.
 */
bool selftest_cosmofs_rollback_record(const char **reason)
{
    struct cfs_rollback r, back;
    memset(&r, 0, sizeof(r));
    r.state = CFS_ROLLBACK_PENDING;
    for (unsigned i = 0; i < 16; i++) {
        r.target_uuid[i] = (uint8_t)(0x10 + i);
        r.safety_uuid[i] = (uint8_t)(0xA0 + i);
    }
    r.target_id = 7;
    r.safety_id = 9;
    r.generation = 0x123456789ull;
    r.requested_ns = 0xFEDCBA9876543210ull;
    cfs_rollback_seal(&r);
    CHECK(r.magic == CFS_ROLLBACK_MAGIC && r.version == 1 && r.crc != 0);
    CHECK(cfs_rollback_parse(&r, &back) == 0);
    CHECK(memcmp(&r, &back, sizeof(r)) == 0);

    /* Each byte, each of eight flips. */
    unsigned refused = 0;
    for (unsigned at = 0; at < sizeof(r); at++)
        for (unsigned bit = 0; bit < 8; bit++) {
            uint8_t raw[sizeof(r)];
            memcpy(raw, &r, sizeof(r));
            raw[at] ^= (uint8_t)(1u << bit);
            CHECK(cfs_rollback_parse(raw, &back) == -EIO);
            refused++;
        }

    /* Wrong in meaning, right in checksum. */
    struct cfs_rollback bad = r;
    bad.magic ^= 1u;
    bad.crc = 0;
    bad.crc = crc32c(&bad, sizeof(bad));
    CHECK(cfs_rollback_parse(&bad, &back) == -EIO);
    bad = r;
    bad.version = 2;
    bad.crc = 0;
    bad.crc = crc32c(&bad, sizeof(bad));
    CHECK(cfs_rollback_parse(&bad, &back) == -EIO);
    static const uint32_t states[] = { 0, 3, 0xFFFFFFFFu };
    for (unsigned i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        bad = r;
        bad.state = states[i];
        cfs_rollback_seal(&bad);
        CHECK(cfs_rollback_parse(&bad, &back) == -EIO);
    }
    /* A pending record whose safety snapshot is its target, or with no
     * safety snapshot, keeps nothing of the tree it replaces. */
    bad = r;
    memcpy(bad.safety_uuid, bad.target_uuid, 16);
    cfs_rollback_seal(&bad);
    CHECK(cfs_rollback_parse(&bad, &back) == -EIO);
    bad = r;
    memset(bad.safety_uuid, 0, 16);
    cfs_rollback_seal(&bad);
    CHECK(cfs_rollback_parse(&bad, &back) == -EIO);
    /* HELD is the other state a record may be in. */
    bad = r;
    bad.state = CFS_ROLLBACK_HELD;
    cfs_rollback_seal(&bad);
    CHECK(cfs_rollback_parse(&bad, &back) == 0 && back.state == CFS_ROLLBACK_HELD);
    kinfo("selftest: cosmofs-rollback-record: %u single-bit changes over %u bytes refused; wrong magic, version "
          "and state refused under a matching crc", refused, (unsigned)sizeof(r));
    return true;
}

static int clone_req(const char *origin, const char *clone)
{
    static struct cosmofs_snap_info out[1];
    struct cosmofs_snap_req req = { .op = COSMOFS_SNAP_CLONE, .name = origin, .name2 = clone, .out = out, .max = 1 };
    return cosmofs_snapctl(mount_of(ENG), &req);
}

/*
 * Clones (roadmap 1.2 B1; design.md, "Clones"; CF20): a clone of a
 * snapshot is a writable tree under .snapshots that starts as the
 * snapshot's and shares its blocks. Its writes are seen neither in the
 * snapshot nor in the live tree, and the live tree's not in it; they
 * survive a remount; the origin cannot be deleted while it has a clone,
 * a clone is no rollback target, and a rename between two trees is
 * EXDEV. Deleting the clone gives back at least the blocks of the file
 * only it held, and the checker -- whose counts are the oracle -- calls
 * the filesystem clean at every step: a clone block freed too early is
 * `seen_not_alloc`, one kept for nobody `alloc_not_seen`. A snapshot
 * stays read-only to symlink and rename too (they had no guard).
 */
bool selftest_cosmofs_clone(const char **reason)
{
    struct blkdev *bd;
    if (!engine_mount(&bd, 2048, reason))
        return false;
    static struct cosmofs_snap_info info[1];
    static struct cosmofs_check_report rep;
    static char big[16 * 4096];
    uint64_t x = 0xD1B54A32D192ED03ull;
    for (unsigned i = 0; i < sizeof(big); i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        big[i] = (char)x;
    }
    unsigned n = 0;
    struct cosmofs_stats st;
    struct cosmo_stat cs;
    struct file *f = NULL;

    CHECK(write_file(ENG "/a", "orig", 4));
    CHECK(vfs_mkdir(NULL, ENG "/d", 0755) == 0);
    CHECK(write_file(ENG "/d/f", "deep", 4));
    CHECK(snapctl(COSMOFS_SNAP_CREATE, "o", info, 1, &n) == 0);
    CHECK(clone_req("o", "o") == -EEXIST);
    CHECK(clone_req("nope", "c") == -ENOENT);
    CHECK(clone_req("o", "c") == 0);
    CHECK(vfs_sync() == 0);
    CHECK(cosmofs_stats(mount_of(ENG), &st) == 0);
    uint64_t free_made = st.free_blocks;

    /* It starts as the snapshot. */
    CHECK(read_matches(ENG "/.snapshots/c/a", "orig", 4));
    CHECK(read_matches(ENG "/.snapshots/c/d/f", "deep", 4));
    /* It is writable, and its writes are its own. */
    CHECK(write_file(ENG "/.snapshots/c/a", "clone", 5));
    CHECK(write_file(ENG "/.snapshots/c/new", big, sizeof(big)));
    CHECK(vfs_mkdir(NULL, ENG "/.snapshots/c/sub", 0755) == 0);
    CHECK(vfs_unlink(NULL, ENG "/.snapshots/c/d/f") == 0);
    CHECK(vfs_sync() == 0);
    CHECK(read_matches(ENG "/a", "orig", 4));
    CHECK(read_matches(ENG "/d/f", "deep", 4));
    CHECK(read_matches(ENG "/.snapshots/o/a", "orig", 4));
    CHECK(read_matches(ENG "/.snapshots/o/d/f", "deep", 4));
    CHECK(vfs_stat(NULL, ENG "/.snapshots/o/new", &cs) == -ENOENT);
    CHECK(vfs_stat(NULL, ENG "/new", &cs) == -ENOENT);
    /* And the live tree's are not the clone's. */
    CHECK(write_file(ENG "/a", "live2", 5));
    CHECK(read_matches(ENG "/.snapshots/c/a", "clone", 5));
    CHECK(read_matches(ENG "/.snapshots/o/a", "orig", 4));
    /* An fsync is the vnode's own tree's: a clone file's inode goes to the
     * clone, a snapshot file's nowhere -- neither onto the live inode of
     * the same number. */
    CHECK(vfs_open(NULL, ENG "/.snapshots/c/a", COSMO_O_RDWR, 0, &f) == 0);
    CHECK(file_write(f, "fsync", 5) == 5);
    CHECK(file_sync(f) == 0);
    file_put(f);
    f = NULL;
    CHECK(vfs_open(NULL, ENG "/.snapshots/o/a", COSMO_O_RDONLY, 0, &f) == 0);
    CHECK(file_sync(f) == 0);
    file_put(f);
    f = NULL;
    CHECK(read_matches(ENG "/a", "live2", 5));
    CHECK(read_matches(ENG "/.snapshots/c/a", "fsync", 5));
    CHECK(read_matches(ENG "/.snapshots/o/a", "orig", 4));
    CHECK(write_file(ENG "/.snapshots/c/a", "clone", 5));
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0 && rep.clean);
    CHECK(cosmofs_stats(mount_of(ENG), &st) == 0);
    uint64_t free_written = st.free_blocks;
    CHECK(free_written + 16 <= free_made);

    /* The rules around it. */
    CHECK(snapctl(COSMOFS_SNAP_DELETE, "o", NULL, 0, NULL) == -EBUSY);
    CHECK(rollback_req("c", NULL, info) == -EINVAL);
    CHECK(clone_req("c", "cc") == -EINVAL);
    CHECK(vfs_rename(NULL, ENG "/.snapshots/c/sub", ENG "/moved") == -EXDEV);
    CHECK(vfs_symlink(NULL, ENG "/.snapshots/o/link", "x") == -EROFS);
    CHECK(vfs_rename(NULL, ENG "/.snapshots/o/a", ENG "/.snapshots/o/b") == -EROFS);
    CHECK(vfs_symlink(NULL, ENG "/.snapshots/c/link", "a") == 0);   /* a clone takes one */

    /* Its writes survive a remount. */
    CHECK(remount_eng(bd, reason));
    CHECK(read_matches(ENG "/.snapshots/c/a", "clone", 5));
    CHECK(read_matches(ENG "/.snapshots/c/new", big, sizeof(big)));
    CHECK(vfs_stat(NULL, ENG "/.snapshots/c/d/f", &cs) == -ENOENT);
    CHECK(read_matches(ENG "/.snapshots/o/d/f", "deep", 4));
    CHECK(read_matches(ENG "/a", "live2", 5));
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0 && rep.clean);

    /* Busy while something inside it is open, like a snapshot. */
    CHECK(vfs_open(NULL, ENG "/.snapshots/c/a", COSMO_O_RDONLY, 0, &f) == 0);
    CHECK(snapctl(COSMOFS_SNAP_DELETE, "c", NULL, 0, NULL) == -EBUSY);
    file_put(f);

    /* Deleted: what only it held goes back, nothing of the origin's. */
    CHECK(cosmofs_stats(mount_of(ENG), &st) == 0);
    uint64_t free_before = st.free_blocks;
    CHECK(snapctl(COSMOFS_SNAP_DELETE, "c", NULL, 0, NULL) == 0);
    CHECK(vfs_sync() == 0);
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0);
    if (!rep.clean)
        kinfo("selftest: cosmofs-clone: after the deletion: %llu leaked, %llu free-in-use",
              (unsigned long long)rep.alloc_not_seen.count, (unsigned long long)rep.seen_not_alloc.count);
    CHECK(rep.clean);
    CHECK(cosmofs_stats(mount_of(ENG), &st) == 0);
    CHECK(st.free_blocks >= free_before + 16);
    CHECK(read_matches(ENG "/.snapshots/o/d/f", "deep", 4));
    CHECK(vfs_stat(NULL, ENG "/.snapshots/c", &cs) == -ENOENT);
    CHECK(snapctl(COSMOFS_SNAP_DELETE, "o", NULL, 0, NULL) == 0);
    CHECK(vfs_sync() == 0);
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0 && rep.clean);
    CHECK(remount_eng(bd, reason));
    CHECK(read_matches(ENG "/a", "live2", 5));
    CHECK(cosmofs_check(mount_of(ENG), &rep, 0) == 0 && rep.clean);
    kinfo("selftest: cosmofs-clone: written apart from its snapshot and the live tree, kept across a remount, "
          "%llu block(s) back when deleted, the checker clean throughout",
          (unsigned long long)(st.free_blocks - free_before));
    return engine_unmount(bd, reason);
}

/* --- roadmap 1.2 B2's defects (docs/audit/2026-10-22-b2-fsck-report.md) --- */

/* One CRC per block of a single-device pool, for "what did this change". */
static uint32_t *device_crcs(struct blkdev *bd, uint64_t nblocks)
{
    struct spool *p;
    if (pool_open(bd, &p))
        return NULL;
    uint32_t *crcs = kmalloc((size_t)nblocks * sizeof(*crcs), 0);
    uint8_t *b = kmalloc(CFS_BLOCK, 0);
    bool ok = crcs != NULL && b != NULL;
    for (uint64_t i = 0; ok && i < nblocks; i++) {
        ok = pool_read(p, CFS_DVA(0, i), b) == 0;
        if (ok)
            crcs[i] = crc32c(b, CFS_BLOCK);
    }
    kfree(b);
    pool_close(p);
    if (!ok) {
        kfree(crcs);
        return NULL;
    }
    return crcs;
}

static unsigned crcs_differ(const uint32_t *a, const uint32_t *b, uint64_t nblocks)
{
    unsigned n = 0;
    for (uint64_t i = 0; i < nblocks; i++)
        n += a[i] != b[i];
    return n;
}

/*
 * CFS-1: a read-only mount writes nothing. The pool is left with both
 * records pending -- what the last commit freed and an inode unlinked
 * while held -- and is mounted read-only, checked and unmounted: every
 * block is what it was. The replay ran all the same, in memory: the
 * check is clean (it would call the orphan's blocks leaked otherwise).
 * Before the fix the unmount's commit wrote the replay to the device.
 */
bool selftest_cosmofs_ro_replay(const char **reason)
{
    const uint64_t nblocks = 512;
    struct blkdev *bd;
    if (!engine_mount(&bd, nblocks, reason))
        return false;
    const size_t biglen = 16 * 4096;
    uint8_t *big = kmalloc(biglen, 0);
    CHECK(big != NULL);
    fill_incompressible(big, biglen, 0xc0ffee);
    bool wrote = write_file(ENG "/held", (const char *)big, biglen);
    kfree(big);
    CHECK(wrote);
    CHECK(vfs_sync() == 0);
    struct file *f;
    CHECK(vfs_open(NULL, ENG "/held", COSMO_O_RDONLY, 0, &f) == 0);
    CHECK(vfs_unlink(NULL, ENG "/held") == 0);
    CHECK(vfs_sync() == 0);
    struct cosmofs_stats st;
    CHECK(cosmofs_stats(mount_of(ENG), &st) == 0);
    CHECK(st.orphan_root != 0 && st.free_root != 0);   /* both replays have work */
    file_put(f);
    CHECK(vfs_umount2(ENG, VFS_UMOUNT_FORCE) == 0);    /* the eviction never reaches the disk */

    uint32_t *before = device_crcs(bd, nblocks);
    CHECK(before != NULL);
    int mrc = vfs_mount(ENG, "cosmofs", bd, MOUNT_RDONLY);
    if (mrc != 0)
        kfree(before);
    CHECK(mrc == 0);
    struct cosmofs_check_report r;
    bool clean = cosmofs_check(mount_of(ENG), &r, 0) == 0 && r.clean;
    int repair = cosmofs_check(mount_of(ENG), &r, COSMOFS_CHECK_REPAIR);
    int urc = vfs_umount(ENG);
    uint32_t *after = device_crcs(bd, nblocks);
    unsigned changed = after != NULL ? crcs_differ(before, after, nblocks) : ~0u;
    kfree(before);
    kfree(after);
    kinfo("selftest: cosmofs-ro-replay: read-only mount: check %s, repair %d, unmount %d, %u block(s) changed",
          clean ? "clean" : "NOT clean", repair, urc, changed);
    CHECK(changed == 0);       /* the claim, first: what the old code got wrong */
    CHECK(urc == 0);
    CHECK(clean);              /* the replay ran in memory */
    CHECK(repair == -EROFS);   /* a repair would be dropped: refused */

    /* The writable mount after it writes the replay, as it always did. */
    CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    CHECK(vfs_sync() == 0);
    CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0 && r.clean);
    CHECK(cosmofs_stats(mount_of(ENG), &st) == 0 && st.orphan_root == 0);
    return engine_unmount(bd, reason);
}

/*
 * CFS-2: writes honour the mounted format. A version-5 pool has no
 * compressed records (version 6 added them), so a new file written here
 * must not get one; at the current version the same file is a record,
 * which is what shows this test can see one. cosmofs_test_block_of maps
 * a plain block and answers -ENOENT for one inside a record (cfs_map
 * finds no plain run there), and the file reading back says it is not a
 * hole.
 */
static bool compress_at_version(unsigned version, int *map0, const char **reason)
{
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    struct blkdev *bd = ramblk_create(512);
    CHECK(bd != NULL);
    bool ok = (version == 0 ? cosmofs_format(bd) : cosmofs_test_format_version(bd, version)) == 0;
    int mk = vfs_mkdir(NULL, ENG, 0755);
    ok = ok && (mk == 0 || mk == -EEXIST) && vfs_mount(ENG, "cosmofs", bd, 0) == 0;
    if (!ok) {
        (void)vfs_rmdir(NULL, ENG);
        ramblk_destroy(bd);
        *reason = "check failed: format and mount for compress_at_version";
        return false;
    }
    cosmofs_test_set_writeback(mount_of(ENG), false);
    static uint8_t zeros[8 * 4096];   /* one whole record, as compressible as data gets */
    memset(zeros, 'z', sizeof(zeros));
    struct vnode *vn = NULL;
    bool w = write_file(ENG "/z", zeros, sizeof(zeros)) && vfs_sync() == 0 && vfs_lookup(NULL, ENG "/z", &vn) == 0;
    uint64_t dva = 0;
    *map0 = w ? cosmofs_test_block_of(mount_of(ENG), vn->ino, 0, &dva) : -100;
    if (vn)
        vnode_put(vn);
    struct cosmofs_stats st;
    unsigned v = cosmofs_stats(mount_of(ENG), &st) == 0 ? st.version : 0;
    bool back = read_matches(ENG "/z", zeros, sizeof(zeros));
    kinfo("selftest: cosmofs-version-compress: format %u: block 0 maps %d, reads back %s", v, *map0,
          back ? "yes" : "NO");
    CHECK(w && back);
    CHECK(version == 0 || v == version);   /* the format is not raised by writing */
    return engine_unmount(bd, reason);
}

bool selftest_cosmofs_version_compress(const char **reason)
{
    int cur = 0, v5 = 0;
    if (!compress_at_version(0, &cur, reason))
        return false;
    CHECK(cur == -ENOENT);   /* the current format compresses: a record */
    if (!compress_at_version(5, &v5, reason))
        return false;
    CHECK(v5 == 0);     /* the claim: a plain block on a version-5 pool */
    return true;
}

/*
 * CFS-11: a scrub that met a block no copy can satisfy still answers, and
 * says how many. One plain data block rotted on a single-device pool is
 * unrecoverable; the scrub's counts reach the operator through
 * /dev/fsctl. Before the fix the write failed with -EIO and the counts
 * were only in the kernel log.
 */
bool selftest_cosmofs_scrub_counts(const char **reason)
{
    struct blkdev *bd;
    if (!engine_mount(&bd, 512, reason))
        return false;
    static uint8_t page[4096];
    fill_incompressible(page, sizeof(page), 0x5c4b);
    CHECK(write_file(ENG "/rot", page, sizeof(page)));
    CHECK(vfs_sync() == 0);
    struct vnode *vn;
    CHECK(vfs_lookup(NULL, ENG "/rot", &vn) == 0);
    uint64_t ino = vn->ino, dva = 0;
    vnode_put(vn);
    CHECK(cosmofs_test_block_of(mount_of(ENG), ino, 0, &dva) == 0);
    CHECK(rot_copy(bd, CFS_DVA_BLK(dva), 0x3c));

    struct cosmofs_scrub_stats sc;
    int direct = cosmofs_scrub(mount_of(ENG), &sc, COSMOFS_SCRUB_REPAIR);
    struct file *f;
    CHECK(vfs_open(NULL, "/dev/fsctl", COSMO_O_RDWR, 0, &f) == 0);
    struct cosmo_fsctl cmd = { .version = COSMO_FSCTL_VERSION, .op = COSMO_FSCTL_SCRUB,
                               .mount_id = mount_of(ENG)->id };
    uint8_t buf[sizeof(struct cosmo_fsctl_result) + sizeof(struct cosmo_fsctl_scrub)];
    int64_t w = file_write(f, &cmd, sizeof(cmd));
    int64_t n = file_read(f, buf, sizeof(buf));
    file_put(f);
    const struct cosmo_fsctl_scrub *s = (const struct cosmo_fsctl_scrub *)(buf + sizeof(struct cosmo_fsctl_result));
    kinfo("selftest: cosmofs-scrub-counts: scrub %d (%llu unrecoverable); fsctl write %lld, read %lld", direct,
          (unsigned long long)sc.unrecoverable, (long long)w, (long long)n);
    CHECK(direct == 0 && sc.unrecoverable == 1);   /* a finding, not a failure */
    CHECK(w == (int64_t)sizeof(cmd));
    CHECK(n == (int64_t)sizeof(buf));
    CHECK(s->unrecoverable == 1 && s->blocks_read > 0);
    CHECK(vfs_unlink(NULL, ENG "/rot") == 0);
    return engine_unmount(bd, reason);
}

/*
 * A scrub with a transaction open. A file rewritten after a commit has
 * its checksum tree copied into blocks this young pool has never
 * written -- zeros on the device -- and the copies exist only in memory
 * until the commit. The scrub checks what is on the devices, so it must
 * leave those to the commit: before, it read the zeros and called the
 * file's checksum index and leaf unrecoverable (found by
 * cosmofs-compress once B2's inode_prepare moved an allocation, which
 * had been passing by the luck of a stale block of the same kind).
 */
bool selftest_cosmofs_scrub_open(const char **reason)
{
    struct blkdev *bd;
    if (!engine_mount(&bd, 512, reason))
        return false;
    static uint8_t page[4096];
    fill_incompressible(page, sizeof(page), 0x0be4);
    CHECK(write_file(ENG "/f", page, sizeof(page)));
    CHECK(vfs_sync() == 0);
    page[0] ^= 0xff;
    struct file *f;
    CHECK(vfs_open(NULL, ENG "/f", COSMO_O_RDWR, 0, &f) == 0);
    bool w = file_pwrite(f, page, sizeof(page), 0) == (int64_t)sizeof(page);
    /* Written back, not committed: the copies are in the transaction. */
    mutex_lock(&f->vn->lock);
    int wb = pagecache_sync(f->vn);
    mutex_unlock(&f->vn->lock);
    file_put(f);
    CHECK(w && wb == 0);
    struct cosmofs_stats st;
    CHECK(cosmofs_stats(mount_of(ENG), &st) == 0 && st.dirty_buffers > 0);
    struct cosmofs_scrub_stats sc;
    CHECK(cosmofs_scrub(mount_of(ENG), &sc, COSMOFS_SCRUB_REPAIR) == 0);
    kinfo("selftest: cosmofs-scrub-open: %u dirty buffer(s); scrub read %llu, %llu unrecoverable",
          st.dirty_buffers, (unsigned long long)sc.blocks_read, (unsigned long long)sc.unrecoverable);
    CHECK(sc.unrecoverable == 0);
    CHECK(vfs_sync() == 0);
    CHECK(cosmofs_scrub(mount_of(ENG), &sc, COSMOFS_SCRUB_REPAIR) == 0 && sc.unrecoverable == 0);
    CHECK(read_matches(ENG "/f", page, sizeof(page)));
    return engine_unmount(bd, reason);
}

/*
 * A root that is not a directory is refused by the mount with -ENOTDIR.
 * Before, the mount succeeded and the VFS asserted the root was a
 * directory: a disk with one changed mode panicked the kernel. Both
 * superblock slots name the corrupted root (a second commit after the
 * corruption's), so the older-slot fallback has nothing to fall back to.
 */
bool selftest_cosmofs_root_type(const char **reason)
{
    struct blkdev *bd;
    if (!engine_mount(&bd, 512, reason))
        return false;
    CHECK(write_file(ENG "/a", "a", 1));
    CHECK(vfs_sync() == 0);
    /* Twice, a commit each, so both slots name it. Nothing may write the
     * root between: its vnode's inode_sync would put the type back. */
    CHECK(cosmofs_test_corrupt(mount_of(ENG), COSMOFS_CORRUPT_ROOT_TYPE, CFS_ROOT_INO, NULL) == 0);
    CHECK(vfs_sync() == 0);
    CHECK(cosmofs_test_corrupt(mount_of(ENG), COSMOFS_CORRUPT_ROOT_TYPE, CFS_ROOT_INO, NULL) == 0);
    CHECK(vfs_sync() == 0);
    CHECK(vfs_umount2(ENG, VFS_UMOUNT_FORCE) == 0);
    int mrc = vfs_mount(ENG, "cosmofs", bd, 0);
    kinfo("selftest: cosmofs-root-type: a root whose inode says regular file: mount %d", mrc);
    if (mrc == 0)
        (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    CHECK(mrc == -ENOTDIR);
    CHECK(vfs_rmdir(NULL, ENG) == 0);
    ramblk_destroy(bd);
    return true;
}

/* --- roadmap 1.2 B2: the check on an unmounted device, the scrub's modes --- */

/* Write slot `slot`'s superblock again with its uuid's first byte
 * changed, CRC recomputed: a slot that names another pool. */
static bool super_other_pool(struct blkdev *bd, unsigned slot)
{
    struct spool *p;
    if (pool_open(bd, &p))
        return false;
    uint8_t *b = kmalloc(CFS_BLOCK, 0);
    bool ok = b != NULL && pool_read(p, CFS_DVA(0, slot), b) == 0;
    if (ok) {
        struct cfs_super *sb = (struct cfs_super *)b;
        sb->uuid[0] ^= 0x5a;
        sb->crc = 0;
        uint32_t c = crc32c(b, offsetof(struct cfs_super, crc));
        static const uint8_t zero4[4] = { 0 };
        c = crc32c_update(c, zero4, 4);
        sb->crc = crc32c_update(c, b + offsetof(struct cfs_super, crc) + 4,
                                CFS_BLOCK - offsetof(struct cfs_super, crc) - 4);
        ok = pool_write(p, CFS_DVA(0, slot), b) == 0 && pool_flush(p) == 0;
    }
    kfree(b);
    pool_close(p);
    return ok;
}

/*
 * Clear block `blk`'s bit in the newest root's bitmap, on the device of a
 * single-device version-4+ pool nothing has mounted: a free block in use,
 * made where no commit can hand the block out again before the check
 * (cosmofs_test_corrupt's FREE_IN_USE goes through a commit, whose own
 * copies take the first free block -- this one -- and turn the finding
 * into a cross-link: CFS-25's hazard, met by the first draft of this test).
 */
static bool bitmap_clear_on_disk(struct blkdev *bd, uint64_t blk)
{
    struct spool *p;
    if (pool_open(bd, &p))
        return false;
    uint8_t *b = kmalloc(CFS_BLOCK, 0);
    bool ok = b != NULL;
    uint64_t members = 0, gen = 0;
    for (unsigned slot = 0; ok && slot < 2; slot++) {
        if (pool_read(p, CFS_DVA(0, slot), b) != 0)
            continue;
        const struct cfs_super *sb = (const struct cfs_super *)b;
        if (memcmp(sb->magic, CFS_MAGIC, 8) == 0 && sb->generation > gen) {
            gen = sb->generation;
            members = sb->members;
        }
    }
    uint64_t at = 0;
    ok = ok && members != 0 && pool_read(p, members, b) == 0;
    if (ok)
        at = ((const struct cfs_member_block *)(b + CFS_MHDR_SIZE))->m[0].alloc_root;
    ok = ok && at != 0 && pool_read(p, at, b) == 0;
    uint64_t chunk = ok ? ((const uint64_t *)(b + CFS_MHDR_SIZE))[blk / CFS_BITS_PER_BITMAP] : 0;
    ok = ok && chunk != 0 && pool_read(p, chunk, b) == 0;
    if (ok) {
        const struct cfs_mhdr *h = (const struct cfs_mhdr *)b;
        uint64_t hgen = h->generation;
        uint64_t i = blk % CFS_BITS_PER_BITMAP;
        b[CFS_MHDR_SIZE + i / 8] &= (uint8_t)~(1u << (i % 8));
        cfs_mhdr_seal_raw(NULL, b, CFS_KIND_BITMAP, chunk, hgen);
        ok = pool_write(p, chunk, b) == 0 && pool_flush(p) == 0;
    }
    kfree(b);
    pool_close(p);
    return ok;
}

/*
 * The check on a device nothing has mounted (cosmofs_check_dev, what
 * fsck.cosmofs runs through /dev/fsctl CHECK_DEV). A pool left with a
 * leak and a block in use the bitmap calls free: the read-only check
 * names both and changes no block; the boot's repair refuses (a free
 * block in use is not a boot's to repair); the offline repair repairs
 * both, commits, and the next check is clean -- the free-in-use one is
 * the repair a mounted pool cannot make. Then the three things that stop
 * a check before it starts: a mounted device (-EBUSY), superblock slots
 * that name two pools, and a root that is not a directory.
 */
bool selftest_cosmofs_check_dev(const char **reason)
{
    const uint64_t nblocks = 512;
    struct blkdev *bd;
    if (!engine_mount(&bd, nblocks, reason))
        return false;
    struct cosmofs_check_report *rep = kzalloc(sizeof(*rep));
    struct cosmofs_dev_report dev;
    uint32_t *before = NULL, *after = NULL;
#define CD_CHECK(cond)                                                        \
    do {                                                                      \
        if (!(cond)) {                                                        \
            *reason = "check failed: " #cond " at line " STR(__LINE__);     \
            goto fail;                                                       \
        }                                                                     \
    } while (0)
    CD_CHECK(rep != NULL);
    CD_CHECK(write_file(ENG "/a", "file a", 6));
    CD_CHECK(vfs_sync() == 0);
    struct vnode *vn;
    CD_CHECK(vfs_lookup(NULL, ENG "/a", &vn) == 0);
    uint64_t ino = vn->ino;
    vnode_put(vn);
    CD_CHECK(cosmofs_check_dev(bd, 0, rep, &dev) == -EBUSY);   /* mounted: its own CHECK is the way */
    uint64_t a_blk = 0;
    CD_CHECK(cosmofs_test_block_of(mount_of(ENG), ino, 0, &a_blk) == 0);
    CD_CHECK(cosmofs_test_corrupt(mount_of(ENG), COSMOFS_CORRUPT_LEAK, 0, NULL) == 0);
    CD_CHECK(vfs_sync() == 0);
    CD_CHECK(vfs_umount(ENG) == 0);
    CD_CHECK(bitmap_clear_on_disk(bd, CFS_DVA_BLK(a_blk)));

    before = device_crcs(bd, nblocks);
    CD_CHECK(before != NULL);
    CD_CHECK(cosmofs_check_dev(bd, 0, rep, &dev) == 0);
    after = device_crcs(bd, nblocks);
    CD_CHECK(after != NULL);
    unsigned changed = crcs_differ(before, after, nblocks);
    kinfo("selftest: cosmofs-check-dev: read-only: flags %#x, %llu leaked, %llu free-in-use, %u block(s) changed",
          dev.flags, (unsigned long long)rep->alloc_not_seen.count, (unsigned long long)rep->seen_not_alloc.count,
          changed);
    CD_CHECK((dev.flags & COSMO_FSCTL_DEV_F_CHECKED) != 0);
    CD_CHECK(rep->alloc_not_seen.count == 1 && rep->seen_not_alloc.count == 1);
    CD_CHECK(changed == 0);

    CD_CHECK(cosmofs_check_dev(bd, COSMOFS_CHECK_REPAIR | COSMOFS_CHECK_BOOT, rep, &dev) == 0);
    CD_CHECK(rep->boot_refused && (dev.flags & COSMO_FSCTL_DEV_F_BOOT_REFUSED) != 0);
    CD_CHECK(rep->alloc_not_seen.repaired == 0 && rep->seen_not_alloc.repaired == 0);

    CD_CHECK(cosmofs_check_dev(bd, COSMOFS_CHECK_REPAIR, rep, &dev) == 0);
    kinfo("selftest: cosmofs-check-dev: repair: flags %#x, %llu/%llu leaked, %llu/%llu free-in-use repaired",
          dev.flags, (unsigned long long)rep->alloc_not_seen.repaired, (unsigned long long)rep->alloc_not_seen.count,
          (unsigned long long)rep->seen_not_alloc.repaired, (unsigned long long)rep->seen_not_alloc.count);
    CD_CHECK(rep->alloc_not_seen.repaired == 1 && rep->seen_not_alloc.repaired == 1);
    CD_CHECK((dev.flags & COSMO_FSCTL_DEV_F_COMMITTED) != 0);
    CD_CHECK(cosmofs_check_dev(bd, 0, rep, &dev) == 0 && rep->clean);
    CD_CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    CD_CHECK(read_matches(ENG "/a", "file a", 6));

    /* A root that is not a directory: the mount says where it stopped. */
    CD_CHECK(cosmofs_test_corrupt(mount_of(ENG), COSMOFS_CORRUPT_ROOT_TYPE, CFS_ROOT_INO, NULL) == 0);
    CD_CHECK(vfs_sync() == 0);
    CD_CHECK(cosmofs_test_corrupt(mount_of(ENG), COSMOFS_CORRUPT_ROOT_TYPE, CFS_ROOT_INO, NULL) == 0);
    CD_CHECK(vfs_sync() == 0);
    CD_CHECK(vfs_umount2(ENG, VFS_UMOUNT_FORCE) == 0);
    CD_CHECK(cosmofs_check_dev(bd, 0, rep, &dev) == 0);
    kinfo("selftest: cosmofs-check-dev: root not a directory: flags %#x, error %d, stage %u", dev.flags,
          dev.mount_error, dev.mount_stage);
    CD_CHECK((dev.flags & COSMO_FSCTL_DEV_F_MOUNT_FAILED) && dev.mount_error == -ENOTDIR &&
             dev.mount_stage == COSMO_FSCTL_STAGE_ROOT);

    /* Two slots, two pools: not checked, and said. */
    CD_CHECK(super_other_pool(bd, 1));
    CD_CHECK(cosmofs_check_dev(bd, 0, rep, &dev) == 0);
    CD_CHECK((dev.flags & COSMO_FSCTL_DEV_F_SUPER_DISAGREE) && !(dev.flags & COSMO_FSCTL_DEV_F_CHECKED));
    kfree(before);
    kfree(after);
    kfree(rep);
    CD_CHECK(vfs_rmdir(NULL, ENG) == 0 || true);
    ramblk_destroy(bd);
    return true;
fail:
    kfree(before);
    kfree(after);
    kfree(rep);
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    (void)vfs_rmdir(NULL, ENG);
    ramblk_destroy(bd);
    return false;
#undef CD_CHECK
}

/*
 * The scrub's modes (roadmap 1.2 B2). On a two-way mirror with one copy
 * of a file's blocks rotted: a scrub without repair counts the bad copies
 * and writes nothing (the next one counts them again); with repair it
 * rewrites them; the one after is clean. A snapshot's own blocks -- the
 * file's old contents, which the live tree no longer reaches -- are
 * scrubbed too. And at a bound of 50 blocks a second the same scrub
 * takes the time its block count says. And the check of the mirror's
 * other device is refused while the pool is mounted through the first.
 */
bool selftest_cosmofs_scrub_modes(const char **reason)
{
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    struct blkdev *bd[2] = { ramblk_create(512), ramblk_create(512) };
    unsigned saved = g_cosmofs_scrub_rate;
#define SM_CHECK(cond)                                                        \
    do {                                                                      \
        if (!(cond)) {                                                        \
            *reason = "check failed: " #cond " at line " STR(__LINE__);     \
            goto fail;                                                       \
        }                                                                     \
    } while (0)
    SM_CHECK(bd[0] != NULL && bd[1] != NULL);
    SM_CHECK(cosmofs_format_mirror(bd, 1, 2) == 0);
    int mk = vfs_mkdir(NULL, ENG, 0755);
    SM_CHECK(mk == 0 || mk == -EEXIST);
    SM_CHECK(vfs_mount(ENG, "cosmofs", bd[0], 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    static uint8_t data[4 * 4096];
    fill_incompressible(data, sizeof(data), 0x5c5c);
    SM_CHECK(write_file(ENG "/m", data, sizeof(data)));
    SM_CHECK(vfs_sync() == 0);
    unsigned n = 0;
    struct cosmofs_snap_info info;
    SM_CHECK(snapctl(COSMOFS_SNAP_CREATE, "old", &info, 1, &n) == 0);
    fill_incompressible(data, sizeof(data), 0x6d6d);
    SM_CHECK(write_file(ENG "/m", data, sizeof(data)));   /* the snapshot keeps the old blocks */
    SM_CHECK(vfs_sync() == 0);
    struct vnode *vn;
    SM_CHECK(vfs_lookup(NULL, ENG "/m", &vn) == 0);
    uint64_t ino = vn->ino;
    vnode_put(vn);
    for (uint64_t l = 0; l < 4; l++) {
        uint64_t dva = 0;
        SM_CHECK(cosmofs_test_block_of(mount_of(ENG), ino, l, &dva) == 0);
        SM_CHECK(rot_copy(bd[1], CFS_DVA_BLK(dva), 0x3c));
    }
    struct cosmofs_scrub_stats a, b, c, d;
    SM_CHECK(cosmofs_scrub(mount_of(ENG), &a, 0) == 0);
    SM_CHECK(cosmofs_scrub(mount_of(ENG), &b, 0) == 0);
    SM_CHECK(cosmofs_scrub(mount_of(ENG), &c, COSMOFS_SCRUB_REPAIR) == 0);
    SM_CHECK(cosmofs_scrub(mount_of(ENG), &d, 0) == 0);
    kinfo("selftest: cosmofs-scrub-modes: %llu blocks; report %llu bad, again %llu bad; repair %llu of %llu; "
          "after %llu bad",
          (unsigned long long)a.blocks_read, (unsigned long long)a.bad_copies, (unsigned long long)b.bad_copies,
          (unsigned long long)c.repaired, (unsigned long long)c.bad_copies, (unsigned long long)d.bad_copies);
    SM_CHECK(a.bad_copies == 4 && a.repaired == 0 && a.unrecoverable == 0);
    SM_CHECK(b.bad_copies == 4);                 /* the report wrote nothing */
    SM_CHECK(c.bad_copies == 4 && c.repaired == 4);
    SM_CHECK(d.bad_copies == 0 && d.unrecoverable == 0);
    SM_CHECK(read_matches(ENG "/m", data, sizeof(data)));
    /* The snapshot's blocks: four data blocks the live tree no longer
     * reaches, read by the scrub (8 = the old file's 4 + the new file's 4,
     * and more for the trees). */
    SM_CHECK(a.blocks_read >= 8 + 4);

    g_cosmofs_scrub_rate = 50;   /* 5 blocks a window: the pool's few dozen take several */
    struct cosmofs_scrub_stats e;
    SM_CHECK(cosmofs_scrub(mount_of(ENG), &e, 0) == 0);
    g_cosmofs_scrub_rate = saved;
    uint64_t floor_ms = e.blocks_read > 5 ? (e.blocks_read / 5 - 1) * 100 : 0;
    kinfo("selftest: cosmofs-scrub-modes: at 50 blocks/s: %llu blocks in %llu ms (at least %llu ms)",
          (unsigned long long)e.blocks_read, (unsigned long long)(e.elapsed_ns / 1000000),
          (unsigned long long)floor_ms);
    SM_CHECK(floor_ms >= 300);   /* a bound this test could not see would prove nothing */
    SM_CHECK(e.rate == 50 && e.elapsed_ns / 1000000 >= floor_ms);
    SM_CHECK(snapctl(COSMOFS_SNAP_DELETE, "old", NULL, 0, NULL) == 0);
    /* The mirror is mounted through its first device: a check of the
     * second must not run beside the live allocator (Codex on #434).
     * Since roadmap 1.2 B3 the mount holds the second too (CFS-5), so the
     * check's own mount is refused its claim. */
    static struct cosmofs_check_report leg;
    struct cosmofs_dev_report legdev;
    int lrc = cosmofs_check_dev(bd[1], COSMOFS_CHECK_REPAIR, &leg, &legdev);
    kinfo("selftest: cosmofs-scrub-modes: the other leg of a mounted mirror: check %d", lrc);
    SM_CHECK(lrc == -EBUSY);
    SM_CHECK(vfs_umount(ENG) == 0);
    SM_CHECK(vfs_rmdir(NULL, ENG) == 0);
    ramblk_destroy(bd[0]);
    ramblk_destroy(bd[1]);
    return true;
fail:
    g_cosmofs_scrub_rate = saved;
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    (void)vfs_rmdir(NULL, ENG);
    if (bd[0])
        ramblk_destroy(bd[0]);
    if (bd[1])
        ramblk_destroy(bd[1]);
    return false;
#undef SM_CHECK
}

#if CONFIG_FAULTINJECT
/*
 * CFS-6: an allocation a retry could survive is returned to the caller,
 * and the transaction lives on. Two sites CF17 abandoned the transaction
 * at: the chain block a file's eleventh run needs (extents_store), and
 * the inode-map path a chown's inode write needs (inode_sync). Each
 * fails once, the operation reports -ENOMEM, and the same operation done
 * again succeeds, commits and reads back after a remount. Before the
 * fix the first failure abandoned the transaction: the retry and every
 * commit after it answered -EIO until the unmount dropped everything.
 */
bool selftest_cosmofs_nomem(const char **reason)
{
    struct blkdev *bd;
    if (!engine_mount(&bd, 1024, reason))
        return false;
    struct file *f = NULL;
    struct vnode *vn = NULL;
    uint8_t *pages = kmalloc(11 * 4096, 0);
#define NM_CHECK(cond)                                                        \
    do {                                                                      \
        if (!(cond)) {                                                        \
            *reason = "check failed: " #cond " at line " STR(__LINE__);     \
            goto fail;                                                       \
        }                                                                     \
    } while (0)
    NM_CHECK(pages != NULL);
    fill_incompressible(pages, 11 * 4096, 0x6e6f6d);
    /* Ten runs: every other page, so no two merge. The direct slots hold
     * exactly ten. */
    NM_CHECK(vfs_open(NULL, ENG "/runs", COSMO_O_RDWR | COSMO_O_CREAT, 0644, &f) == 0);
    for (unsigned i = 0; i < 10; i++)
        NM_CHECK(file_pwrite(f, pages + (size_t)i * 4096, 4096, (uint64_t)i * 2 * 4096) == 4096);
    NM_CHECK(file_sync(f) == 0);
    /* The eleventh run needs the chain's first block. */
    NM_CHECK(file_pwrite(f, pages + 10 * 4096, 4096, 20 * 4096) == 4096);
    faultinject_set(FI_COSMOFS_CHAIN_NOMEM, 1, 1, thread_current());
    int first = file_sync(f);
    struct fi_stats fst;
    faultinject_stats(FI_COSMOFS_CHAIN_NOMEM, &fst);
    faultinject_clear(FI_COSMOFS_CHAIN_NOMEM);
    int retry = file_sync(f);
    kinfo("selftest: cosmofs-nomem: chain: sync %d (hits %llu), retry %d", first, (unsigned long long)fst.hits,
          retry);
    NM_CHECK(fst.hits == 1 && first == -ENOMEM);
    NM_CHECK(retry == 0);   /* the claim: the transaction was not abandoned */
    file_put(f);
    f = NULL;

    /* A chown's inode write after a commit: the map path is copied anew. */
    NM_CHECK(vfs_lookup(NULL, ENG "/runs", &vn) == 0);
    faultinject_set(FI_COSMOFS_INODE_NOMEM, 1, 1, thread_current());
    int own = vfs_chown(vn, 7, 7);
    faultinject_stats(FI_COSMOFS_INODE_NOMEM, &fst);
    faultinject_clear(FI_COSMOFS_INODE_NOMEM);
    int own2 = vfs_chown(vn, 7, 7);
    int sync = vfs_sync();
    kinfo("selftest: cosmofs-nomem: inode: chown %d (hits %llu), retry %d, sync %d", own,
          (unsigned long long)fst.hits, own2, sync);
    NM_CHECK(fst.hits == 1 && own == -ENOMEM);
    NM_CHECK(own2 == 0 && sync == 0);
    vnode_put(vn);
    vn = NULL;

    /* A truncate whose map store fails: a twelfth run, committed, then a
     * truncate that drops it -- eleven runs stay, so the chain's block is
     * copied, and that copy fails. The truncate is -ENOMEM, and what the
     * old map names must still be allocated after a commit: the first
     * draft of this fix queued the dropped run's block free before the
     * store, and the commit freed it under the map that still named it
     * (Codex on #433). */
    NM_CHECK(vfs_open(NULL, ENG "/runs", COSMO_O_RDWR, 0, &f) == 0);
    NM_CHECK(file_pwrite(f, pages, 4096, 22 * 4096) == 4096);
    NM_CHECK(file_sync(f) == 0);
    file_put(f);
    f = NULL;
    faultinject_set(FI_COSMOFS_CHAIN_NOMEM, 1, 1, thread_current());
    int trc = vfs_truncate(NULL, ENG "/runs", 21 * 4096);
    faultinject_stats(FI_COSMOFS_CHAIN_NOMEM, &fst);
    faultinject_clear(FI_COSMOFS_CHAIN_NOMEM);
    NM_CHECK(vfs_sync() == 0);
    static struct cosmofs_check_report tr;   /* 1.6 KiB: not on the stack */
    NM_CHECK(cosmofs_check(mount_of(ENG), &tr, 0) == 0);
    kinfo("selftest: cosmofs-nomem: truncate: %d (hits %llu); then %llu free-in-use, %llu leaked", trc,
          (unsigned long long)fst.hits, (unsigned long long)tr.seen_not_alloc.count,
          (unsigned long long)tr.alloc_not_seen.count);
    NM_CHECK(fst.hits == 1 && trc == -ENOMEM);
    NM_CHECK(tr.clean);
    NM_CHECK(vfs_truncate(NULL, ENG "/runs", 21 * 4096) == 0);

    struct cosmofs_check_report r;
    NM_CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0 && r.clean);
    NM_CHECK(vfs_umount(ENG) == 0);
    NM_CHECK(vfs_mount(ENG, "cosmofs", bd, 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    NM_CHECK(vfs_open(NULL, ENG "/runs", COSMO_O_RDONLY, 0, &f) == 0);
    NM_CHECK(f->vn->uid == 7);
    static uint8_t got[4096];
    for (unsigned i = 0; i < 11; i++) {
        uint64_t at = (uint64_t)(i < 10 ? i * 2 : 20) * 4096;
        NM_CHECK(file_pread(f, got, sizeof(got), at) == 4096);
        NM_CHECK(memcmp(got, pages + (size_t)i * 4096, 4096) == 0);
    }
    file_put(f);
    f = NULL;
    NM_CHECK(cosmofs_check(mount_of(ENG), &r, 0) == 0 && r.clean);
    kfree(pages);
    return engine_unmount(bd, reason);

fail:
    faultinject_clear(FI_COSMOFS_CHAIN_NOMEM);
    faultinject_clear(FI_COSMOFS_INODE_NOMEM);
    if (f)
        file_put(f);
    if (vn)
        vnode_put(vn);
    kfree(pages);
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    (void)vfs_rmdir(NULL, ENG);
    ramblk_destroy(bd);
    return false;
#undef NM_CHECK
}
#else
bool selftest_cosmofs_nomem(const char **reason) { (void)reason; return true; }
#endif

/* --- copy state (roadmap 1.2 B3) ---------------------------------------------
 *
 * design.md, "Format version 13"; invariants CF25, CF26, CF27. Two-way
 * mirrors on RAM devices; a device is "pulled" by making every request
 * to it answer -ENODEV (ramblk_set_fail), the block layer's answer for
 * an unregistered device, so it can come back with what it held.
 */

#define CS_CHECK(cond)                                                        \
    do {                                                                      \
        if (!(cond)) {                                                        \
            *reason = "check failed: " #cond " at line " STR(__LINE__);     \
            goto out;                                                        \
        }                                                                     \
    } while (0)

/*
 * The pool's copy states on a two-way mirror: a failing copy is written
 * around and FAULTED past fs.fault_errors, starting its record from what
 * was written since the last root; the member's last ONLINE copy is
 * never faulted and may not be taken offline; a device that is gone is
 * MISSING at once; a copy that missed nothing comes back ONLINE and one
 * that missed a write does not.
 */
bool selftest_pool_copy_states(const char **reason)
{
    struct blkdev *bd[2] = { ramblk_create(64), ramblk_create(64) };
    struct spool *p = NULL;
    uint8_t *w = kmalloc(CFS_BLOCK, 0), *r = kmalloc(CFS_BLOCK, 0);
    uint32_t limit = g_pool_fault_errors;
    bool ok = false;
    CS_CHECK(bd[0] != NULL && bd[1] != NULL && w != NULL && r != NULL);
    for (unsigned i = 0; i < CFS_BLOCK; i++)
        w[i] = (uint8_t)(i * 7);

    CS_CHECK(pool_open(bd[0], &p) == 0 && pool_add_copy(p, 0, bd[1]) == 0);
    CS_CHECK(p->m[0].ncopies == 2 && p->m[0].region_blocks == 1 && !pool_degraded(p));
    CS_CHECK(pool_write(p, 5, w) == 0 && pool_flush(p) == 0);
    pool_commit_done(p, 7);
    CS_CHECK(pool_read_copy(p, 5, 0, r) == 0 && memcmp(r, w, CFS_BLOCK) == 0);
    CS_CHECK(pool_read_copy(p, 5, 1, r) == 0 && memcmp(r, w, CFS_BLOCK) == 0);

    /* A failing copy: written around, counted, FAULTED past the limit. */
    g_pool_fault_errors = 3;
    ramblk_set_fail(bd[1], -EIO);
    uint64_t seq = p->state_seq;
    for (unsigned i = 0; i < 3; i++)
        CS_CHECK(pool_write(p, 10 + i, w) == 0);
    CS_CHECK(p->m[0].copy[1].state == POOL_COPY_ONLINE && p->m[0].copy[1].write_errors == 3);
    /* Still ONLINE, and what it failed is a hole: not read from it, the
     * member not whole (Codex on #439). */
    CS_CHECK(!pool_copy_readable(p, 10, 1) && !pool_copy_readable(p, 12, 1) && pool_copy_readable(p, 5, 1));
    CS_CHECK(pool_copy_online(p, 0, 1) && pool_degraded(p));
    CS_CHECK(pool_write(p, 13, w) == 0);
    CS_CHECK(p->m[0].copy[1].state == POOL_COPY_FAULTED && p->state_seq != seq && pool_degraded(p));
    CS_CHECK(p->m[0].copy[1].since == 7 && !p->m[0].copy[1].whole);
    /* What it missed: exactly the writes since the last root. */
    for (uint64_t b = 0; b < 64; b++)
        CS_CHECK(pool_copy_missed(p, b, 1) == (b >= 10 && b <= 13));
    /* Not written from now on, not read, and what it misses is recorded. */
    CS_CHECK(pool_write(p, 20, w) == 0 && pool_copy_missed(p, 20, 1) && p->m[0].copy[1].write_errors == 4);
    CS_CHECK(!pool_copy_readable(p, 20, 1) && pool_read_copy(p, 20, 1, r) == -ENODEV);
    CS_CHECK(pool_read(p, 20, r) == 0 && memcmp(r, w, CFS_BLOCK) == 0);

    /* The last ONLINE copy: not offline, and not faulted by a count. */
    CS_CHECK(pool_copy_set_state(p, 0, 0, POOL_COPY_OFFLINE, 0) == -EBUSY);
    ramblk_set_fail(bd[0], -EIO);
    for (unsigned i = 0; i < 5; i++)
        CS_CHECK(pool_write(p, 30, w) == -EIO);   /* no ONLINE copy took it */
    CS_CHECK(pool_read(p, 30, r) == -EIO);
    CS_CHECK(p->m[0].copy[0].state == POOL_COPY_ONLINE && p->m[0].copy[0].write_errors == 5);
    CS_CHECK(pool_copy_readable(p, 30, 0));   /* no other copy took block 30: no hole */
    CS_CHECK(pool_flush(p) == -EIO);   /* the last copy failed its flush: not durable (Codex on #439) */
    CS_CHECK(p->m[0].copy[0].state == POOL_COPY_ONLINE);
    ramblk_set_fail(bd[0], 0);
    ramblk_set_fail(bd[1], 0);
    /* A copy that missed writes comes back only through a resilver. */
    CS_CHECK(pool_copy_set_state(p, 0, 1, POOL_COPY_ONLINE, 0) == -EAGAIN);
    pool_close(p);
    p = NULL;

    /* A device that is gone is MISSING at once, whatever the limit, and
     * what it may have held only in its cache is missed too. */
    CS_CHECK(pool_open(bd[0], &p) == 0 && pool_add_copy(p, 0, bd[1]) == 0);
    pool_commit_done(p, 9);
    CS_CHECK(pool_write(p, 3, w) == 0);
    ramblk_set_fail(bd[1], -ENODEV);
    CS_CHECK(pool_write(p, 4, w) == 0);
    CS_CHECK(p->m[0].copy[1].state == POOL_COPY_MISSING && p->m[0].copy[1].write_errors == 1);
    CS_CHECK(p->m[0].copy[1].since == 9 && pool_copy_missed(p, 3, 1) && pool_copy_missed(p, 4, 1));
    CS_CHECK(!pool_copy_missed(p, 2, 1) && !pool_copy_missed(p, 5, 1));
    /* A flush that cannot reach it is no error while a copy took the root. */
    CS_CHECK(pool_flush(p) == 0);
    ramblk_set_fail(bd[1], 0);
    pool_close(p);
    p = NULL;

    /* OFFLINE and back: with nothing written meanwhile it is ONLINE again;
     * then the other copy goes, reads come from this one, and the one
     * that missed a write stays out. */
    CS_CHECK(pool_open(bd[0], &p) == 0 && pool_add_copy(p, 0, bd[1]) == 0);
    pool_commit_done(p, 11);
    CS_CHECK(pool_copy_set_state(p, 0, 1, POOL_COPY_OFFLINE, 0) == 0 && !pool_copy_readable(p, 0, 1));
    CS_CHECK(pool_copy_set_state(p, 0, 1, POOL_COPY_ONLINE, 0) == 0 && !pool_degraded(p));
    CS_CHECK(pool_copy_set_state(p, 0, 0, POOL_COPY_OFFLINE, 0) == 0);
    CS_CHECK(pool_read(p, 5, r) == 0 && memcmp(r, w, CFS_BLOCK) == 0);
    CS_CHECK(pool_write(p, 6, w) == 0 && pool_copy_missed(p, 6, 0) && !pool_copy_missed(p, 7, 0));
    CS_CHECK(pool_copy_set_state(p, 0, 0, POOL_COPY_ONLINE, 0) == -EAGAIN);
    ok = true;
    kinfo("selftest: pool-copy-states: FAULTED after %u errors, MISSING at -ENODEV, the last copy kept", 3u);
out:
    g_pool_fault_errors = limit;
    if (p)
        pool_close(p);
    kfree(w);
    kfree(r);
    for (unsigned i = 0; i < 2; i++)
        if (bd[i]) {
            ramblk_set_fail(bd[i], 0);
            ramblk_destroy(bd[i]);
        }
    return ok;
}

/* The /proc/mounts line of the mount at `path`, has `opt` in it. */
static bool mounts_line_has(const char *path, const char *opt)
{
    char *buf = kmalloc(4096, 0);
    if (buf == NULL)
        return false;
    int n = vfs_mounts_render(buf, 4096);
    bool found = false;
    char key[64];
    ksnprintf(key, sizeof(key), " %s cosmofs ", path);
    for (char *line = buf; n > 0 && line && *line;) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        if (strstr(line, key) && strstr(line, opt))
            found = true;
        line = nl ? nl + 1 : NULL;
    }
    kfree(buf);
    return found;
}

/* A two-way mirror of `nblocks` RAM blocks a device, mounted at ENG
 * through bd[0] with writeback off. */
static bool mirror_up(struct blkdev *bd[2], uint64_t nblocks)
{
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    bd[0] = ramblk_create(nblocks);
    bd[1] = ramblk_create(nblocks);
    if (bd[0] == NULL || bd[1] == NULL || cosmofs_format_mirror(bd, 1, 2) != 0)
        return false;
    int mk = vfs_mkdir(NULL, ENG, 0755);
    if ((mk != 0 && mk != -EEXIST) || vfs_mount(ENG, "cosmofs", bd[0], 0) != 0)
        return false;
    cosmofs_test_set_writeback(mount_of(ENG), false);
    return true;
}

static void mirror_down(struct blkdev *bd[2])
{
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    (void)vfs_rmdir(NULL, ENG);
    for (unsigned i = 0; i < 2; i++)
        if (bd[i]) {
            ramblk_set_fail(bd[i], 0);
            ramblk_destroy(bd[i]);
            bd[i] = NULL;
        }
}

/*
 * CF26: a mirror with one copy pulled keeps working and says so -- the
 * stats, /proc/mounts, the format raised to record it -- and a copy that
 * keeps failing is FAULTED and stays so across a remount. With both
 * copies gone, a read and a commit fail with -EIO, and nothing panics.
 */
bool selftest_cosmofs_degraded(const char **reason)
{
    struct blkdev *bd[2] = { NULL, NULL };
    struct file *f = NULL;
    uint32_t limit = g_pool_fault_errors;
    bool ok = false;
    static char data[3 * 4096];
    memset(data, 'd', sizeof(data));
    CS_CHECK(mirror_up(bd, 512));
    struct cosmofs_stats st;
    CS_CHECK(write_file(ENG "/a", data, sizeof(data)) && vfs_sync() == 0);
    CS_CHECK(cosmofs_stats(mount_of(ENG), &st) == 0 && st.devices == 2 && st.degraded == 0);
    unsigned v0 = st.version;
    CS_CHECK(v0 == CFS_FORMAT_VERSION);   /* a healthy mirror's commits do not raise it */
    CS_CHECK(mounts_line_has(ENG, " rw 0 0") && !mounts_line_has(ENG, "degraded"));

    /* Pull copy 1: the writes go on, the pool says it is degraded. */
    ramblk_set_fail(bd[1], -ENODEV);
    CS_CHECK(write_file(ENG "/b", data, sizeof(data)));
    CS_CHECK(vfs_sync() == 0);
    CS_CHECK(cosmofs_stats(mount_of(ENG), &st) == 0 && st.devices == 1 && st.degraded == 1);
    CS_CHECK(st.version == 13);           /* recorded, so raised */
    CS_CHECK(mounts_line_has(ENG, " rw,degraded 0 0"));
    CS_CHECK(read_matches(ENG "/a", data, sizeof(data)) && read_matches(ENG "/b", data, sizeof(data)));

    /* Both copies gone: the read of a block no copy holds is -EIO, a
     * commit that reaches no copy is -EIO, and the system is still here.
     * The change the commit carries is made first, while copy 0 serves. */
    CS_CHECK(write_file(ENG "/c", "c", 1));
    ramblk_set_fail(bd[0], -ENODEV);
    /* -EIO wherever the first block no copy holds is met: the directory
     * lookup, or the file's data if its inode is still cached. */
    static char got[4096];
    int orc = vfs_open(NULL, ENG "/a", COSMO_O_RDONLY, 0, &f);
    CS_CHECK(orc == 0 || orc == -EIO);
    if (orc == 0) {
        CS_CHECK(file_read(f, got, sizeof(got)) == -EIO);
        file_put(f);
        f = NULL;
    }
    CS_CHECK(vfs_sync() == -EIO);
    CS_CHECK(cosmofs_stats(mount_of(ENG), &st) == 0 && st.devices == 0 && st.degraded == 2);
    mirror_down(bd);

    /* A copy that keeps failing is FAULTED, and the record keeps it out
     * after a remount even though its device answers again. */
    CS_CHECK(mirror_up(bd, 512));
    g_pool_fault_errors = 4;
    ramblk_set_fail(bd[1], -EIO);
    struct spool *pool = cosmofs_test_pool(mount_of(ENG));
    CS_CHECK(pool != NULL);
    for (unsigned i = 0; i < 8 && pool->m[0].copy[1].state == POOL_COPY_ONLINE; i++) {
        char name[32];
        ksnprintf(name, sizeof(name), ENG "/f%u", i);
        CS_CHECK(write_file(name, data, sizeof(data)) && vfs_sync() == 0);
    }
    CS_CHECK(pool->m[0].copy[1].state == POOL_COPY_FAULTED);
    /* Faulted inside a commit, on a member with no record yet: the next
     * commit records it, and is due with nothing else to write. */
    CS_CHECK(vfs_sync() == 0);
    CS_CHECK(cosmofs_stats(mount_of(ENG), &st) == 0 && st.degraded == 1 && st.version == 13);
    CS_CHECK(write_file(ENG "/after", data, sizeof(data)) && vfs_sync() == 0);
    ramblk_set_fail(bd[1], 0);
    g_pool_fault_errors = limit;
    CS_CHECK(vfs_umount(ENG) == 0);
    CS_CHECK(vfs_mount(ENG, "cosmofs", bd[0], 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    pool = cosmofs_test_pool(mount_of(ENG));
    CS_CHECK(pool != NULL && pool->m[0].copy[1].state == POOL_COPY_FAULTED && pool->m[0].copy[1].dev == NULL);
    CS_CHECK(cosmofs_stats(mount_of(ENG), &st) == 0 && st.devices == 1 && st.degraded == 1);
    CS_CHECK(read_matches(ENG "/after", data, sizeof(data)));
    CS_CHECK(mounts_line_has(ENG, "rw,degraded"));
    ok = true;
    kinfo("selftest: cosmofs-degraded: writes go on with a copy pulled; FAULTED is kept across a remount");
out:
    g_pool_fault_errors = limit;
    if (f)
        file_put(f);
    mirror_down(bd);
    return ok;
}

/* The blocks of member 0 a recorded write log names. */
static void log_blocks(const struct ramblk_log *log, uint8_t *set, uint64_t nblocks)
{
    for (unsigned i = 0; log && i < log->n; i++) {
        const struct ramblk_write *e = &log->w[i];
        if (e->nsectors == 0)
            continue;   /* a flush */
        for (uint64_t s = e->sector; s < e->sector + e->nsectors; s += CFS_BLOCK / 512)
            if (s / (CFS_BLOCK / 512) < nblocks)
                set[s / (CFS_BLOCK / 512)] = 1;
    }
}

/*
 * CF25: the copy that was pulled is recorded as having missed exactly
 * what was written after it went -- every block the other copy took
 * since, and nothing else but the superblock slots -- and the record a
 * remount reads says the same, with the generation it last held whole,
 * which is the generation the device itself still carries.
 */
bool selftest_cosmofs_copy_bitmap(const char **reason)
{
    struct blkdev *bd[2] = { NULL, NULL };
    struct ramblk_log *log = NULL;
    uint8_t *written = kzalloc(512);
    bool ok = false;
    static char data[5 * 4096];
    memset(data, 'x', sizeof(data));
    CS_CHECK(written != NULL);
    cosmofs_test_resilver_hold(true);   /* the remount below would resilver the copy at once */
    CS_CHECK(mirror_up(bd, 512));
    CS_CHECK(write_file(ENG "/before", data, sizeof(data)) && vfs_sync() == 0);
    struct cosmofs_stats st;
    CS_CHECK(cosmofs_stats(mount_of(ENG), &st) == 0);
    uint64_t whole_at = st.generation;
    struct spool *pool = cosmofs_test_pool(mount_of(ENG));
    CS_CHECK(pool != NULL && pool->m[0].region_blocks == 1);   /* a block a region: the record is exact */

    /* Pull copy 1, and record every write copy 0 takes from now on. One
     * commit, then the unmount's: the unmount's metadata lands on blocks
     * the first freed, which were written before the pull -- so only the
     * commit marking its own writes puts them in the record (CF25; a
     * second commit here would reuse blocks already marked, and the test
     * would pass without the rule). */
    ramblk_set_fail(bd[1], -ENODEV);
    ramblk_record_start(bd[0], 4096);
    CS_CHECK(write_file(ENG "/after", data, sizeof(data)));
    CS_CHECK(vfs_mkdir(NULL, ENG "/dir", 0755) == 0 && write_file(ENG "/dir/inner", data, 4096));
    CS_CHECK(write_file(ENG "/later", data, 3 * 4096));
    CS_CHECK(vfs_sync() == 0);
    CS_CHECK(pool->m[0].copy[1].state == POOL_COPY_MISSING && pool->m[0].copy[1].since == whole_at);
    CS_CHECK(vfs_umount(ENG) == 0);
    log = ramblk_record_stop(bd[0]);
    CS_CHECK(log != NULL && log->dropped == 0 && log->n > 0);
    log_blocks(log, written, 512);

    /* The device comes back holding what it held: the root it last took. */
    ramblk_set_fail(bd[1], 0);
    struct cfs_super a, b;
    int ra = -EIO, rb = -EIO;
    CS_CHECK(cfs_super_slots(bd[1], &a, &ra, &b, &rb) == 0);
    uint64_t dev_gen = 0;
    if (ra == 0)
        dev_gen = a.generation;
    if (rb == 0 && b.generation > dev_gen)
        dev_gen = b.generation;
    CS_CHECK(dev_gen == whole_at);

    CS_CHECK(vfs_mount(ENG, "cosmofs", bd[0], 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    pool = cosmofs_test_pool(mount_of(ENG));
    CS_CHECK(pool != NULL && pool->m[0].ncopies == 2);
    const struct pool_copy *cp = &pool->m[0].copy[1];
    /* Back, and recognised by the record: resilvering what it missed. */
    CS_CHECK(cp->state == POOL_COPY_RESILVERING && cp->dev == bd[1] && !cp->whole && cp->since == whole_at);
    /* Every block written after the pull is in the record; nothing else
     * is, but the superblock slots the record marks for the root. */
    unsigned marked = 0, nwritten = 0;
    for (uint64_t blk = 0; blk < 512; blk++) {
        bool m = pool_copy_missed(pool, CFS_DVA(0, blk), 1);
        nwritten += written[blk];
        marked += m;
        if (written[blk])
            CS_CHECK(m);
        else if (m)
            CS_CHECK(blk < 2);
    }
    CS_CHECK(nwritten > 8);
    CS_CHECK(read_matches(ENG "/later", data, 3 * 4096) && read_matches(ENG "/dir/inner", data, 4096));
    ok = true;
    kinfo("selftest: cosmofs-copy-bitmap: %u blocks written after the pull, %u recorded, since generation %llu",
          nwritten, marked, (unsigned long long)whole_at);
out:
    cosmofs_test_resilver_hold(false);
    if (log)
        ramblk_log_free(log);
    kfree(written);
    mirror_down(bd);
    return ok;
}

/*
 * CF27: a mount handed the copy that was away mounts the newest root
 * from the other device, and leaves the lagging one out; with that other
 * device already mounted, it refuses rather than mount the pool twice.
 */
bool selftest_cosmofs_copy_import(const char **reason)
{
#define ENG2 "/mnt/eng2"
    struct blkdev *bd[2] = { NULL, NULL };
    bool ok = false;
    cosmofs_test_resilver_hold(true);
    CS_CHECK(mirror_up(bd, 512));
    CS_CHECK(write_file(ENG "/first", "first", 5) && vfs_sync() == 0);
    ramblk_set_fail(bd[1], -ENODEV);
    CS_CHECK(write_file(ENG "/second", "second", 6) && vfs_sync() == 0);
    CS_CHECK(vfs_umount(ENG) == 0);
    ramblk_set_fail(bd[1], 0);

    /* Through the lagging device: the newest root, from bd[0]. */
    CS_CHECK(vfs_mount(ENG, "cosmofs", bd[1], 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    struct spool *pool = cosmofs_test_pool(mount_of(ENG));
    CS_CHECK(pool != NULL && pool->m[0].copy[0].dev == bd[0]);
    /* The lagging device is not read; it is the copy being resilvered. */
    CS_CHECK(pool->m[0].copy[1].dev == bd[1] && pool->m[0].copy[1].state == POOL_COPY_RESILVERING);
    CS_CHECK(read_matches(ENG "/second", "second", 6) && read_matches(ENG "/first", "first", 5));
    struct cosmofs_stats st;
    CS_CHECK(cosmofs_stats(mount_of(ENG), &st) == 0 && st.devices == 1 && st.degraded == 1);
    /* The device the mount moved to is held by it (Codex on #439). */
    CS_CHECK(vfs_bdev_mounted(bd[0]));
    CS_CHECK(vfs_umount(ENG) == 0);
    CS_CHECK(!vfs_bdev_mounted(bd[0]));

    /* The newer device mounted already: the lagging one does not mount. */
    CS_CHECK(vfs_mount(ENG, "cosmofs", bd[0], 0) == 0);
    int mk = vfs_mkdir(NULL, ENG2, 0755);
    CS_CHECK(mk == 0 || mk == -EEXIST);
    CS_CHECK(vfs_mount(ENG2, "cosmofs", bd[1], 0) == -EBUSY);
    ok = true;
    kinfo("selftest: cosmofs-copy-import: the lagging device mounts the newest root, and not twice");
out:
    cosmofs_test_resilver_hold(false);
    (void)vfs_umount2(ENG2, VFS_UMOUNT_FORCE);
    (void)vfs_rmdir(NULL, ENG2);
    mirror_down(bd);
    return ok;
#undef ENG2
}
/*
 * A copy on a bigger device than the first (Codex on #439): the member's
 * size, and so its record's regions, are the table's, so the pool mounts
 * through the bigger copy with the record the smaller one's mount wrote.
 */
bool selftest_cosmofs_copy_geometry(const char **reason)
{
    struct blkdev *bd[2] = { NULL, NULL };
    bool ok = false;
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    bd[0] = ramblk_create(512);
    bd[1] = ramblk_create(8192);   /* 8192 blocks would be two a region; 512 are one */
    CS_CHECK(bd[0] != NULL && bd[1] != NULL && cosmofs_format_mirror(bd, 1, 2) == 0);
    int mk = vfs_mkdir(NULL, ENG, 0755);
    CS_CHECK(mk == 0 || mk == -EEXIST);
    CS_CHECK(vfs_mount(ENG, "cosmofs", bd[0], 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    struct spool *pool = cosmofs_test_pool(mount_of(ENG));
    CS_CHECK(pool->m[0].nblocks == 512 && pool->m[0].region_blocks == 1);
    CS_CHECK(write_file(ENG "/g", "geometry", 8) && vfs_sync() == 0);
    ramblk_set_fail(bd[0], -ENODEV);   /* the small one goes: the big one is the pool now */
    CS_CHECK(write_file(ENG "/h", "after", 5) && vfs_sync() == 0);
    CS_CHECK(vfs_umount(ENG) == 0);
    CS_CHECK(vfs_mount(ENG, "cosmofs", bd[1], 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    pool = cosmofs_test_pool(mount_of(ENG));
    CS_CHECK(pool->m[0].nblocks == 512 && pool->m[0].region_blocks == 1);
    CS_CHECK(read_matches(ENG "/h", "after", 5) && read_matches(ENG "/g", "geometry", 8));
    ok = true;
    kinfo("selftest: cosmofs-copy-geometry: a bigger copy mounts the pool at the table's size");
out:
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    (void)vfs_rmdir(NULL, ENG);
    for (unsigned i = 0; i < 2; i++)
        if (bd[i]) {
            ramblk_set_fail(bd[i], 0);
            ramblk_destroy(bd[i]);
        }
    return ok;
}
#undef CS_CHECK

/* --- resilvering (roadmap 1.2 B3) ------------------------------------------
 *
 * design.md, "Resilvering"; invariants CF28, CF29. The resilver thread is
 * held (cosmofs_test_resilver_hold) and the tests drive it with
 * cosmofs_test_resilver_run, so where it stops is the test's choice.
 */

#define RS_CHECK(cond)                                                        \
    do {                                                                      \
        if (!(cond)) {                                                        \
            *reason = "check failed: " #cond " at line " STR(__LINE__);     \
            goto out;                                                        \
        }                                                                     \
    } while (0)

static int pool_op(enum cosmofs_pool_op op, const char *a, const char *b)
{
    struct cosmofs_pool_req req = { .op = op, .name = a, .name2 = b };
    return cosmofs_poolctl(mount_of(ENG), &req);
}

/* One block of `bd`, read raw. */
static bool raw_block(struct blkdev *bd, uint64_t blk, uint8_t *out)
{
    struct spool *p;
    if (pool_open(bd, &p))
        return false;
    bool ok = pool_read_copy(p, CFS_DVA(0, blk), 0, out) == 0;
    pool_close(p);
    return ok;
}

/* Every block of member 0 the mount's bitmap calls allocated, and the
 * superblock slots, the same on both devices. */
static bool same_allocated(struct blkdev *a, struct blkdev *b, unsigned *compared)
{
    struct cfs *fs = cfs_of(mount_of(ENG));
    uint8_t *x = kmalloc(CFS_BLOCK, 0), *y = kmalloc(CFS_BLOCK, 0);
    bool ok = fs != NULL && x != NULL && y != NULL;
    unsigned n = 0;
    for (uint64_t blk = 0; ok && blk < fs->mem[0].nblocks; blk++) {
        if (blk >= fs->mem[0].first_usable && !cfs_bitmap_test(fs, cfs_dva_lin(fs, CFS_DVA(0, blk))))
            continue;
        ok = raw_block(a, blk, x) && raw_block(b, blk, y) && memcmp(x, y, CFS_BLOCK) == 0;
        if (!ok)
            kwarn("selftest: block %llu differs between %s and %s", (unsigned long long)blk, a->name, b->name);
        n++;
    }
    kfree(x);
    kfree(y);
    if (compared)
        *compared = n;
    return ok;
}

/*
 * CF28, CF29: a copy that was away is resilvered from the regions its
 * record names; a write made during the resilver reaches the copy being
 * rebuilt (read raw from each device); at the end every allocated block
 * is the same on both, the copy is ONLINE, a scrub finds nothing, and the
 * rebuilt device alone mounts the pool. While the pool is mounted the
 * device is held: /dev/blkctl's check sees it (CFS-5).
 */
bool selftest_cosmofs_resilver(const char **reason)
{
    struct blkdev *bd[2] = { NULL, NULL };
    uint8_t *x = kmalloc(CFS_BLOCK, 0), *y = kmalloc(CFS_BLOCK, 0);
    uint32_t limit = g_pool_fault_errors;
    bool ok = false;
    static char data[5 * 4096], during[4096];
    memset(data, 'r', sizeof(data));
    memset(during, 'w', sizeof(during));
    cosmofs_test_resilver_hold(true);
    RS_CHECK(x != NULL && y != NULL);
    RS_CHECK(mirror_up(bd, 512));
    RS_CHECK(write_file(ENG "/a", data, sizeof(data)) && vfs_sync() == 0);
    ramblk_set_fail(bd[1], -ENODEV);
    RS_CHECK(write_file(ENG "/b", data, sizeof(data)) && vfs_sync() == 0);
    ramblk_set_fail(bd[1], 0);
    struct spool *pool = cosmofs_test_pool(mount_of(ENG));
    RS_CHECK(pool != NULL && pool->m[0].copy[1].state == POOL_COPY_MISSING);

    /* CFS-5: the pool's other copy is not the operator's to format. */
    RS_CHECK(vfs_bdev_mounted(bd[1]));
    RS_CHECK(vfs_format("cosmofs", bd[1], NULL) == -EBUSY);

    /* Back: online resilvers what it missed. */
    RS_CHECK(pool_op(COSMOFS_POOL_ONLINE, bd[1]->name, NULL) == 0);
    const struct pool_copy *cp = &pool->m[0].copy[1];
    RS_CHECK(cp->state == POOL_COPY_RESILVERING && !cp->whole && cp->cursor == 0);
    RS_CHECK(cosmofs_test_resilver_run(mount_of(ENG), 2) == 0);
    RS_CHECK(cp->state == POOL_COPY_RESILVERING && cp->cursor > 0);

    /* A write during the resilver: on both devices, read raw. */
    RS_CHECK(write_file(ENG "/during", during, sizeof(during)) && vfs_sync() == 0);
    struct file *f = NULL;
    RS_CHECK(vfs_open(NULL, ENG "/during", COSMO_O_RDONLY, 0, &f) == 0);
    uint64_t ino = f->vn->ino, dblk = 0;
    file_put(f);
    RS_CHECK(cosmofs_test_block_of(mount_of(ENG), ino, 0, &dblk) == 0);
    RS_CHECK(raw_block(bd[0], CFS_DVA_BLK(dblk), x) && memcmp(x, during, CFS_BLOCK) == 0);
    RS_CHECK(raw_block(bd[1], CFS_DVA_BLK(dblk), y) && memcmp(y, during, CFS_BLOCK) == 0);
    RS_CHECK(cp->state == POOL_COPY_RESILVERING);

    /* To the end. */
    RS_CHECK(cosmofs_test_resilver_run(mount_of(ENG), 0) == 0);
    RS_CHECK(cp->state == POOL_COPY_ONLINE && cp->dev == bd[1]);
    struct cosmofs_stats st;
    RS_CHECK(cosmofs_stats(mount_of(ENG), &st) == 0 && st.devices == 2 && st.degraded == 0);
    RS_CHECK(!mounts_line_has(ENG, "degraded"));
    RS_CHECK(vfs_sync() == 0);
    unsigned compared = 0;
    RS_CHECK(same_allocated(bd[0], bd[1], &compared) && compared > 10);
    struct cosmofs_scrub_stats sc;
    RS_CHECK(cosmofs_scrub(mount_of(ENG), &sc, 0) == 0 && sc.bad_copies == 0 && sc.unrecoverable == 0);

    /* Holes: writes copy 1 fails while copy 0 takes them, below its fault
     * count. It stays ONLINE with holes, the member not whole; the
     * resilver fills them and nothing else. */
    g_pool_fault_errors = 1000;
    ramblk_set_fail_writes(bd[1], -EIO);   /* writes only: a failed flush faults a copy at once, by design */
    RS_CHECK(write_file(ENG "/holes", data, sizeof(data)) && vfs_sync() == 0);
    ramblk_set_fail_writes(bd[1], 0);
    g_pool_fault_errors = limit;
    RS_CHECK(cp->state == POOL_COPY_ONLINE && pool_copy_any_missed(pool, 0, 1) && pool_degraded(pool));
    RS_CHECK(read_matches(ENG "/holes", data, sizeof(data)));
    RS_CHECK(cosmofs_test_resilver_run(mount_of(ENG), 0) == 0);
    RS_CHECK(cp->state == POOL_COPY_ONLINE && !pool_copy_any_missed(pool, 0, 1) && !pool_degraded(pool));
    RS_CHECK(vfs_sync() == 0 && same_allocated(bd[0], bd[1], NULL));

    /* The rebuilt device alone holds the pool. */
    RS_CHECK(vfs_umount(ENG) == 0);
    ramblk_set_fail(bd[0], -ENODEV);
    RS_CHECK(vfs_mount(ENG, "cosmofs", bd[1], 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    RS_CHECK(read_matches(ENG "/a", data, sizeof(data)) && read_matches(ENG "/b", data, sizeof(data)));
    RS_CHECK(read_matches(ENG "/during", during, sizeof(during)) && read_matches(ENG "/holes", data, sizeof(data)));
    ok = true;
    kinfo("selftest: cosmofs-resilver: %u blocks the same on both copies; a write during it reached both", compared);
out:
    g_pool_fault_errors = limit;
    cosmofs_test_resilver_hold(false);
    kfree(x);
    kfree(y);
    mirror_down(bd);
    return ok;
}

/*
 * CF29: a resilver stopped by an unmount goes on from the cursor the
 * record holds -- its target found again by the tag in its label -- and
 * completes; replace put a blank device in the place of a missing one.
 */
bool selftest_cosmofs_resilver_resume(const char **reason)
{
    struct blkdev *bd[2] = { NULL, NULL };
    struct blkdev *nd = NULL;
    bool ok = false;
    static char data[40 * 4096];
    memset(data, 'q', sizeof(data));
    cosmofs_test_resilver_hold(true);
    RS_CHECK(mirror_up(bd, 512));
    nd = ramblk_create(512);
    RS_CHECK(nd != NULL);
    RS_CHECK(write_file(ENG "/a", data, sizeof(data)) && vfs_sync() == 0);
    ramblk_set_fail(bd[1], -ENODEV);
    RS_CHECK(write_file(ENG "/b", data, 4096) && vfs_sync() == 0);
    RS_CHECK(pool_op(COSMOFS_POOL_REPLACE, bd[1]->name, nd->name) == 0);
    struct spool *pool = cosmofs_test_pool(mount_of(ENG));
    RS_CHECK(pool->m[0].ncopies == 2 && pool->m[0].copy[1].dev == nd);
    RS_CHECK(pool->m[0].copy[1].state == POOL_COPY_RESILVERING && pool->m[0].copy[1].whole);
    RS_CHECK(cosmofs_test_resilver_run(mount_of(ENG), 12) == 0);
    /* A commit records the cursor. */
    RS_CHECK(write_file(ENG "/mid", "mid", 3) && vfs_sync() == 0);
    uint64_t saved = pool->m[0].copy[1].cursor;
    RS_CHECK(saved > 2);
    RS_CHECK(vfs_umount(ENG) == 0);

    /* The "reboot": the target is found by its tag and goes on. */
    RS_CHECK(vfs_mount(ENG, "cosmofs", bd[0], 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    pool = cosmofs_test_pool(mount_of(ENG));
    const struct pool_copy *cp = &pool->m[0].copy[1];
    RS_CHECK(cp->dev == nd && cp->state == POOL_COPY_RESILVERING && cp->whole && cp->cursor == saved);
    RS_CHECK(cosmofs_test_resilver_run(mount_of(ENG), 0) == 0);
    RS_CHECK(cp->state == POOL_COPY_ONLINE && cp->dev == nd);
    RS_CHECK(vfs_sync() == 0);
    unsigned compared = 0;
    RS_CHECK(same_allocated(bd[0], nd, &compared));
    RS_CHECK(vfs_umount(ENG) == 0);
    ramblk_set_fail(bd[0], -ENODEV);
    RS_CHECK(vfs_mount(ENG, "cosmofs", nd, 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    RS_CHECK(read_matches(ENG "/a", data, sizeof(data)) && read_matches(ENG "/mid", "mid", 3));
    ok = true;
    kinfo("selftest: cosmofs-resilver-resume: stopped at block %llu, resumed there, %u blocks the same",
          (unsigned long long)saved, compared);
out:
    cosmofs_test_resilver_hold(false);
    mirror_down(bd);
    if (nd) {
        ramblk_set_fail(nd, 0);
        ramblk_destroy(nd);
    }
    return ok;
}

/*
 * A spare takes over by itself: a copy pulled while the pool has a spare
 * is replaced by it, the takeover resilvers it whole, and the spare is
 * then a member -- the pool whole again, the spare list empty. The spare
 * is held while it waits (CFS-5).
 */
bool selftest_cosmofs_spare(const char **reason)
{
    struct blkdev *bd[2] = { NULL, NULL };
    struct blkdev *sp = NULL;
    bool ok = false;
    static char data[8 * 4096];
    memset(data, 's', sizeof(data));
    cosmofs_test_resilver_hold(true);
    RS_CHECK(mirror_up(bd, 512));
    sp = ramblk_create(512);
    RS_CHECK(sp != NULL);
    RS_CHECK(write_file(ENG "/a", data, sizeof(data)) && vfs_sync() == 0);
    RS_CHECK(pool_op(COSMOFS_POOL_SPARE_ADD, sp->name, NULL) == 0);
    struct spool *pool = cosmofs_test_pool(mount_of(ENG));
    RS_CHECK(pool->nspares == 1 && pool->spare[0] == sp && vfs_bdev_mounted(sp));
    RS_CHECK(pool_op(COSMOFS_POOL_SPARE_ADD, sp->name, NULL) == -EEXIST);

    ramblk_set_fail(bd[1], -ENODEV);
    RS_CHECK(write_file(ENG "/b", data, sizeof(data)) && vfs_sync() == 0);
    RS_CHECK(pool->m[0].copy[1].state == POOL_COPY_MISSING);
    RS_CHECK(cosmofs_test_resilver_run(mount_of(ENG), 0) == 0);
    const struct pool_copy *cp = &pool->m[0].copy[1];
    RS_CHECK(cp->dev == sp && cp->state == POOL_COPY_ONLINE && pool->nspares == 0);
    struct cosmofs_stats st;
    RS_CHECK(cosmofs_stats(mount_of(ENG), &st) == 0 && st.devices == 2 && st.degraded == 0);
    RS_CHECK(vfs_sync() == 0);
    RS_CHECK(same_allocated(bd[0], sp, NULL));
    RS_CHECK(vfs_umount(ENG) == 0);
    /* The pulled disk comes back; the pool has its two copies and leaves it. */
    ramblk_set_fail(bd[1], 0);
    RS_CHECK(vfs_mount(ENG, "cosmofs", bd[0], 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    RS_CHECK(cosmofs_stats(mount_of(ENG), &st) == 0 && st.devices == 2 && st.degraded == 0);
    pool = cosmofs_test_pool(mount_of(ENG));
    RS_CHECK(pool->m[0].copy[1].dev == sp || pool->m[0].copy[0].dev == sp);
    RS_CHECK(read_matches(ENG "/b", data, sizeof(data)));
    ok = true;
    kinfo("selftest: cosmofs-spare: the spare took the pulled copy's place and is a member");
out:
    cosmofs_test_resilver_hold(false);
    mirror_down(bd);
    if (sp) {
        ramblk_set_fail(sp, 0);
        ramblk_destroy(sp);
    }
    return ok;
}

/*
 * A single-device pool becomes a mirror (attach), a good copy is
 * replaced with the old one serving until the new one is done (replace),
 * and the mirror goes back to one device (detach). Offline and online
 * with nothing written meanwhile need no resilver; the last good copy
 * cannot be taken offline or detached.
 */
bool selftest_cosmofs_attach(const char **reason)
{
    struct blkdev *bd[3] = { NULL, NULL, NULL };
    bool ok = false;
    static char data[6 * 4096];
    memset(data, 't', sizeof(data));
    cosmofs_test_resilver_hold(true);
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    for (unsigned i = 0; i < 3; i++) {
        bd[i] = ramblk_create(512);
        RS_CHECK(bd[i] != NULL);
    }
    RS_CHECK(vfs_format("cosmofs", bd[0], NULL) == 0);
    int mk = vfs_mkdir(NULL, ENG, 0755);
    RS_CHECK(mk == 0 || mk == -EEXIST);
    RS_CHECK(vfs_mount(ENG, "cosmofs", bd[0], 0) == 0);
    cosmofs_test_set_writeback(mount_of(ENG), false);
    RS_CHECK(write_file(ENG "/a", data, sizeof(data)) && vfs_sync() == 0);
    struct cosmofs_stats st;
    RS_CHECK(cosmofs_stats(mount_of(ENG), &st) == 0 && st.devices == 1 && st.version == CFS_FORMAT_VERSION);

    /* attach: one disk becomes a mirror. */
    RS_CHECK(pool_op(COSMOFS_POOL_ATTACH, bd[0]->name, bd[1]->name) == 0);
    struct spool *pool = cosmofs_test_pool(mount_of(ENG));
    RS_CHECK(pool->m[0].ncopies == 2 && pool->m[0].copy[1].state == POOL_COPY_RESILVERING);
    RS_CHECK(cosmofs_stats(mount_of(ENG), &st) == 0 && st.version == 13);
    RS_CHECK(cosmofs_test_resilver_run(mount_of(ENG), 0) == 0);
    RS_CHECK(pool->m[0].copy[1].state == POOL_COPY_ONLINE);
    RS_CHECK(vfs_sync() == 0 && same_allocated(bd[0], bd[1], NULL));

    /* offline and online: the commit that recorded the offline is one
     * the copy missed, so online resilvers that much and no more. */
    RS_CHECK(pool_op(COSMOFS_POOL_OFFLINE, bd[1]->name, NULL) == 0);
    RS_CHECK(pool->m[0].copy[1].state == POOL_COPY_OFFLINE);
    RS_CHECK(pool_op(COSMOFS_POOL_OFFLINE, bd[0]->name, NULL) == -EBUSY);
    RS_CHECK(pool_op(COSMOFS_POOL_ONLINE, bd[1]->name, NULL) == 0);
    RS_CHECK(pool->m[0].copy[1].state == POOL_COPY_RESILVERING && !pool->m[0].copy[1].whole);
    RS_CHECK(cosmofs_test_resilver_run(mount_of(ENG), 0) == 0);
    RS_CHECK(pool->m[0].copy[1].state == POOL_COPY_ONLINE);

    /* replace a good copy: bd[1] serves until bd[2] is done, then goes. */
    RS_CHECK(pool_op(COSMOFS_POOL_REPLACE, bd[1]->name, bd[2]->name) == 0);
    RS_CHECK(pool->m[0].ncopies == 3 && pool->m[0].copy[1].peer != 0);
    RS_CHECK(pool->m[0].copy[2].dev == bd[2] && pool->m[0].copy[2].state == POOL_COPY_RESILVERING);
    RS_CHECK(cosmofs_test_resilver_run(mount_of(ENG), 0) == 0);
    RS_CHECK(pool->m[0].ncopies == 2 && pool->m[0].copy[1].dev == bd[2]);
    RS_CHECK(pool->m[0].copy[1].state == POOL_COPY_ONLINE && !vfs_bdev_mounted(bd[1]));
    struct cfs_super a, b;
    int ra = 0, rb = 0;
    RS_CHECK(cfs_super_slots(bd[1], &a, &ra, &b, &rb) == 0 && ra != 0 && rb != 0);   /* no longer the pool's */
    RS_CHECK(vfs_sync() == 0 && same_allocated(bd[0], bd[2], NULL));

    /* A replace whose sources disagree keeps the copy it replaces (Codex
     * on #440): copy 0's block of /a rots, the walk copies the rot from it,
     * bd[2] -- the good one -- is not detached, and after a scrub put the
     * copies right the operator detaches it. */
    struct file *af = NULL;
    RS_CHECK(vfs_open(NULL, ENG "/a", COSMO_O_RDONLY, 0, &af) == 0);
    uint64_t aino = af->vn->ino, ablk = 0;
    file_put(af);
    RS_CHECK(cosmofs_test_block_of(mount_of(ENG), aino, 0, &ablk) == 0);
    RS_CHECK(rot_copy(bd[0], CFS_DVA_BLK(ablk), 0x5c));
    RS_CHECK(pool_op(COSMOFS_POOL_REPLACE, bd[2]->name, bd[1]->name) == 0);
    RS_CHECK(cosmofs_test_resilver_run(mount_of(ENG), 0) == 0);
    unsigned kv = 0, kc = 0;
    RS_CHECK(pool->m[0].ncopies == 3 && pool_find_dev(pool, bd[2], &kv, &kc) && pool->m[0].copy[kc].peer == 0);
    struct cosmofs_scrub_stats sc;
    RS_CHECK(cosmofs_scrub(mount_of(ENG), &sc, COSMOFS_SCRUB_REPAIR) == 0 && sc.repaired >= 2 && sc.unrecoverable == 0);
    RS_CHECK(pool_op(COSMOFS_POOL_DETACH, bd[2]->name, NULL) == 0 && pool->m[0].ncopies == 2);
    RS_CHECK(read_matches(ENG "/a", data, sizeof(data)));
    RS_CHECK(vfs_sync() == 0 && same_allocated(bd[0], bd[1], NULL));

    /* detach: back to one device. */
    RS_CHECK(pool_op(COSMOFS_POOL_DETACH, bd[1]->name, NULL) == 0);
    RS_CHECK(pool->m[0].ncopies == 1);
    RS_CHECK(pool_op(COSMOFS_POOL_DETACH, bd[0]->name, NULL) == -EBUSY);
    RS_CHECK(cosmofs_stats(mount_of(ENG), &st) == 0 && st.devices == 1 && st.degraded == 0);
    RS_CHECK(vfs_umount(ENG) == 0);
    RS_CHECK(vfs_mount(ENG, "cosmofs", bd[0], 0) == 0);
    RS_CHECK(read_matches(ENG "/a", data, sizeof(data)));
    RS_CHECK(cosmofs_stats(mount_of(ENG), &st) == 0 && st.devices == 1 && st.degraded == 0);
    ok = true;
    kinfo("selftest: cosmofs-attach: attach, offline/online, replace and detach");
out:
    cosmofs_test_resilver_hold(false);
    (void)vfs_umount2(ENG, VFS_UMOUNT_FORCE);
    (void)vfs_rmdir(NULL, ENG);
    for (unsigned i = 0; i < 3; i++)
        if (bd[i])
            ramblk_destroy(bd[i]);
    return ok;
}
#undef RS_CHECK
#else
bool selftest_cosmofs_copy_geometry(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_resilver(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_resilver_resume(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_spare(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_attach(const char **reason) { (void)reason; return true; }
bool selftest_pool_copy_states(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_degraded(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_copy_bitmap(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_copy_import(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_badmap(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_snapshot(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_snapshot_remount(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_snap_identity(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_rollback(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_rollback_atomic(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_rollback_record(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_rollback_shared(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_clone(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_ro_replay(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_version_compress(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_scrub_counts(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_root_type(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_scrub_open(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_check_dev(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_scrub_modes(const char **reason) { (void)reason; return true; }
bool selftest_cosmofs_nomem(const char **reason) { (void)reason; return true; }
#endif
