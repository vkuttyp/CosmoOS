/*
 * vfstest.c - Self-tests for CRC32C, the page cache and the VFS on ramfs.
 */

#include <kernel/crc32c.h>
#include <kernel/errno.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/page.h>
#include <kernel/selftest.h>
#include <kernel/string.h>
#include <kernel/vfs.h>
#include <kernel/sched.h>
#include <kernel/wait.h>
#include <kernel/thread.h>
#include <kernel/percpu.h>

#define STR_(x) #x
#define STR(x)  STR_(x)
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            *reason = "check failed: " #cond " at line " STR(__LINE__);        \
            return false;                                                      \
        }                                                                      \
    } while (0)

bool selftest_crc32c(const char **reason)
{
    CHECK(crc32c("123456789", 9) == 0xE3069283u);
    CHECK(crc32c("", 0) == 0);
    uint32_t a = crc32c("12345", 5);
    CHECK(crc32c_update(a, "6789", 4) == 0xE3069283u);
    return true;
}

bool selftest_pagecache(const char **reason)
{
    struct file *f;
    CHECK(vfs_open(NULL, "/tmp/pc-test", COSMO_O_RDWR | COSMO_O_CREAT | COSMO_O_TRUNC, 0600, &f) == 0);
    struct vnode *vn = f->vn;

    /* Holes read as zero; a write past the end grows the file. */
    static uint8_t buf[3 * PAGE_SIZE];
    memset(buf, 0xab, 3 * PAGE_SIZE);
    CHECK(file_pwrite(f, buf, 100, 2 * PAGE_SIZE + 50) == 100);
    CHECK(vn->size == 2 * PAGE_SIZE + 150);
    memset(buf, 0xff, 3 * PAGE_SIZE);
    CHECK(file_pread(f, buf, 3 * PAGE_SIZE, 0) == (int64_t)(2 * PAGE_SIZE + 150));
    bool zero = true;
    for (size_t i = 0; i < 2 * PAGE_SIZE + 50; i++)
        zero = zero && buf[i] == 0;
    CHECK(zero);
    CHECK(buf[2 * PAGE_SIZE + 50] == 0xab && buf[2 * PAGE_SIZE + 149] == 0xab);
    CHECK(vn->pc.nr_pages == 3 && vn->pc.nr_dirty == 1);   /* holes read in clean */

    /* A write crossing a page boundary. */
    memset(buf, 0x5a, 3 * PAGE_SIZE);
    CHECK(file_pwrite(f, buf, 4000, PAGE_SIZE - 2000) == 4000);
    CHECK(file_pread(f, buf, 4000, PAGE_SIZE - 2000) == 4000);
    bool same = true;
    for (size_t i = 0; i < 4000; i++)
        same = same && buf[i] == 0x5a;
    CHECK(same);

    /* Sync clears dirty; truncate drops pages and zeroes the tail. */
    CHECK(file_sync(f) == 0 && vn->pc.nr_dirty == 0);
    mutex_lock(&vn->lock);
    CHECK(vn->ops->truncate(vn, PAGE_SIZE + 10) == 0);
    mutex_unlock(&vn->lock);
    CHECK(vn->size == PAGE_SIZE + 10 && vn->pc.nr_pages == 2);
    CHECK(file_pread(f, buf, 100, PAGE_SIZE) == 10);
    CHECK(file_pread(f, buf, 100, PAGE_SIZE + 10) == 0);
    /* Writing again past the truncated tail must see zeros there. */
    CHECK(file_pwrite(f, "Z", 1, PAGE_SIZE + 100) == 1);
    CHECK(file_pread(f, buf, 101, PAGE_SIZE) == 101);
    CHECK(buf[10] == 0 && buf[99] == 0 && buf[100] == 'Z');

    file_put(f);
    CHECK(vfs_unlink(NULL, "/tmp/pc-test") == 0);
    return true;
}

static int count_cb(void *arg, const char *name, size_t len, uint64_t ino, enum vnode_type type)
{
    (void)name;
    (void)len;
    (void)ino;
    (void)type;
    (*(unsigned *)arg)++;
    return 0;
}

static unsigned dir_entries(const char *path)
{
    struct vnode *d;
    if (vfs_lookup(NULL, path, &d))
        return 0;
    unsigned n = 0;
    uint64_t pos = 0;
    mutex_lock(&d->lock);
    d->ops->readdir(d, &pos, count_cb, &n);
    mutex_unlock(&d->lock);
    vnode_put(d);
    return n;
}

bool selftest_vfs_ramfs(const char **reason)
{
    unsigned vnodes0 = vfs_vnode_count();
    struct cosmo_stat st;

    /* The root and the boot population. */
    struct vnode *root = vfs_root();
    CHECK(root->type == VNODE_DIR && root->ino == 1);
    vnode_put(root);
    CHECK(vfs_stat(NULL, "/", &st) == 0 && st.type == COSMO_DT_DIR);
    CHECK(vfs_stat(NULL, "/boot/init", &st) == 0 && st.type == COSMO_DT_REG && st.size > 0);
    CHECK(vfs_stat(NULL, "/boot/modules/hello.ko", &st) == 0);
    CHECK(vfs_stat(NULL, "//boot///init", &st) == 0);
    CHECK(vfs_stat(NULL, "/boot/./modules/../init", &st) == 0);
    CHECK(vfs_stat(NULL, "/../boot", &st) == 0);
    CHECK(vfs_stat(NULL, "/boot/init/", &st) == -ENOTDIR);
    CHECK(vfs_stat(NULL, "/boot/nope", &st) == -ENOENT);
    CHECK(vfs_stat(NULL, "", &st) == -ENOENT || vfs_stat(NULL, "", &st) == 0);

    /* A file: create, write, read back through a second open. */
    struct file *f;
    CHECK(vfs_open(NULL, "/tmp/a.txt", COSMO_O_WRONLY | COSMO_O_CREAT, 0644, &f) == 0);
    CHECK(file_write(f, "0123456789", 10) == 10);
    CHECK(file_write(f, "abc", 3) == 3);
    CHECK(file_read(f, (char[4]){ 0 }, 4) == -EBADF);   /* write-only */
    file_put(f);
    char buf[32];
    CHECK(vfs_open(NULL, "/tmp/a.txt", COSMO_O_RDONLY, 0, &f) == 0);
    CHECK(file_read(f, buf, sizeof(buf)) == 13 && memcmp(buf, "0123456789abc", 13) == 0);
    CHECK(file_read(f, buf, sizeof(buf)) == 0);
    CHECK(file_seek(f, 10, COSMO_SEEK_SET) == 10 && file_read(f, buf, 8) == 3 && buf[0] == 'a');
    CHECK(file_seek(f, -20, COSMO_SEEK_CUR) == -EINVAL);
    CHECK(file_write(f, "x", 1) == -EBADF);
    file_put(f);
    CHECK(vfs_open(NULL, "/tmp/a.txt", COSMO_O_WRONLY | COSMO_O_APPEND, 0, &f) == 0);
    CHECK(file_write(f, "!", 1) == 1);
    file_put(f);
    CHECK(vfs_stat(NULL, "/tmp/a.txt", &st) == 0 && st.size == 14 && st.nlink == 1);
    CHECK(vfs_open(NULL, "/tmp/a.txt", COSMO_O_RDONLY | COSMO_O_CREAT | COSMO_O_EXCL, 0, &f) == -EEXIST);
    CHECK(vfs_open(NULL, "/tmp/a.txt", COSMO_O_RDONLY | COSMO_O_DIRECTORY, 0, &f) == -ENOTDIR);
    CHECK(vfs_open(NULL, "/tmp", COSMO_O_RDWR, 0, &f) == -EISDIR);
    CHECK(vfs_open(NULL, "/tmp/a.txt", COSMO_O_ACCMODE, 0, &f) == -EINVAL);

    /* Directories and renames. */
    CHECK(vfs_mkdir(NULL, "/tmp/d1", 0755) == 0);
    CHECK(vfs_mkdir(NULL, "/tmp/d1/d2", 0755) == 0);
    CHECK(vfs_mkdir(NULL, "/tmp/d1", 0755) == -EEXIST);
    CHECK(vfs_mkdir(NULL, "/tmp/nodir/x", 0755) == -ENOENT);
    CHECK(vfs_mkdir(NULL, "/tmp/a.txt/x", 0755) == -ENOTDIR);
    CHECK(vfs_stat(NULL, "/tmp/d1", &st) == 0 && st.nlink == 3);
    CHECK(dir_entries("/tmp/d1") == 3);   /* ., .., d2 */
    CHECK(vfs_rename(NULL, "/tmp/a.txt", "/tmp/d1/d2/b.txt") == 0);
    CHECK(vfs_stat(NULL, "/tmp/a.txt", &st) == -ENOENT);
    CHECK(vfs_stat(NULL, "/tmp/d1/d2/b.txt", &st) == 0 && st.size == 14);
    CHECK(vfs_rename(NULL, "/tmp/d1", "/tmp/d1/d2/loop") == -EINVAL);   /* into itself */
    CHECK(vfs_rename(NULL, "/tmp/d1/d2", "/tmp/e2") == 0);
    CHECK(vfs_stat(NULL, "/tmp/e2/b.txt", &st) == 0);
    CHECK(vfs_stat(NULL, "/tmp/d1", &st) == 0 && st.nlink == 2);
    CHECK(vfs_rmdir(NULL, "/tmp/e2") == -ENOTEMPTY);
    CHECK(vfs_rmdir(NULL, "/tmp/d1") == 0);
    CHECK(vfs_rmdir(NULL, "/tmp/e2/b.txt") == -ENOTDIR);
    CHECK(vfs_unlink(NULL, "/tmp/e2") == -EISDIR);
    CHECK(vfs_rename(NULL, "/tmp/e2/b.txt", "/mnt/b.txt") == 0);      /* same mount */
    CHECK(vfs_rename(NULL, "/tmp/nothing", "/tmp/x") == -ENOENT);

    /* An open file survives unlink; the vnode dies with the last file. */
    CHECK(vfs_open(NULL, "/mnt/b.txt", COSMO_O_RDONLY, 0, &f) == 0);
    CHECK(vfs_unlink(NULL, "/mnt/b.txt") == 0);
    CHECK(vfs_stat(NULL, "/mnt/b.txt", &st) == -ENOENT);
    CHECK(file_read(f, buf, sizeof(buf)) == 14);
    CHECK(f->vn->nlink == 0 && (f->vn->flags & VNODE_DEAD));
    file_put(f);
    CHECK(vfs_rmdir(NULL, "/tmp/e2") == 0);
    CHECK(vfs_rmdir(NULL, "/boot") == -ENOTEMPTY);
    CHECK(vfs_unlink(NULL, "/") == -EEXIST);
    CHECK(vfs_rmdir(NULL, "/tmp/..") == -EINVAL);

    /* Mount a second ramfs on /mnt, use it, unmount it. */
    CHECK(vfs_mount("/mnt", "ramfs", NULL, 0) == 0);
    CHECK(vfs_mount("/mnt", "ramfs", NULL, 0) == -EBUSY);
    CHECK(vfs_mount("/nope", "ramfs", NULL, 0) == -ENOENT);
    CHECK(vfs_mount("/tmp", "nofs", NULL, 0) == -ENODEV);
    CHECK(vfs_mkdir(NULL, "/mnt/inner", 0755) == 0);
    CHECK(vfs_open(NULL, "/mnt/inner/f", COSMO_O_WRONLY | COSMO_O_CREAT, 0644, &f) == 0);
    CHECK(file_write(f, "mounted", 7) == 7);
    CHECK(vfs_stat(NULL, "/mnt/inner/..", &st) == 0 && st.ino == 1);      /* the mount's root */
    CHECK(vfs_stat(NULL, "/mnt/..", &st) == 0 && st.ino == 1);            /* the global root */
    CHECK(vfs_rename(NULL, "/mnt/inner/f", "/tmp/f") == -EXDEV);
    CHECK(vfs_umount("/mnt") == -EBUSY);                                 /* f is open */
    file_put(f);
    CHECK(vfs_rmdir(NULL, "/mnt") == -EBUSY);                            /* a mountpoint */
    CHECK(vfs_mount_count() == 2);
    CHECK(vfs_umount("/mnt") == 0);
    CHECK(vfs_umount("/mnt") == -EINVAL);                                /* not a mount root */
    CHECK(vfs_umount("/") == -EBUSY);
    CHECK(vfs_stat(NULL, "/mnt/inner", &st) == -ENOENT);                /* the ramfs is gone */
    CHECK(vfs_mount_count() == 1);
    CHECK(vfs_vnode_count() == vnodes0);
    return true;
}

/* --- vfs-concurrency: rename against rmdir, lookup against release --------------
 *
 * Two CPUs when available (docs/kernel/lockdep/testing.md). The audit's two
 * findings: rename locked its parents in address order while rmdir locked
 * parent then child (an ABBA, now excluded by the rename lock and the
 * ancestor-first order), and the vnode cache could instantiate a second
 * vnode for an inode whose first was mid-release (now excluded by the
 * unhash-before-drop in vnode_put). Under the debug-build checker any lock
 * order this test provokes is also verified structurally.
 */
struct vfs_hammer {
    volatile unsigned stop;
    unsigned ops;
    unsigned failures;
    int last_rc;
};

/* Moves /tmp/vc/a/x into /tmp/vc/a/b and back; b may vanish under it. */
static void rename_hammer(void *arg)
{
    struct vfs_hammer *h = arg;
    while (!__atomic_load_n(&h->stop, __ATOMIC_ACQUIRE)) {
        int rc = vfs_rename(NULL, "/tmp/vc/a/x", "/tmp/vc/a/b/y");
        if (rc == 0)
            rc = vfs_rename(NULL, "/tmp/vc/a/b/y", "/tmp/vc/a/x");
        if (rc != 0 && rc != -ENOENT) {   /* -ENOENT: b was removed, or y is gone */
            h->failures++;
            h->last_rc = rc;
        }
        h->ops++;
    }
}

/* Removes and recreates /tmp/vc/a/b; it may be non-empty (y inside). */
static void rmdir_hammer(void *arg)
{
    struct vfs_hammer *h = arg;
    while (!__atomic_load_n(&h->stop, __ATOMIC_ACQUIRE)) {
        int rc = vfs_rmdir(NULL, "/tmp/vc/a/b");
        if (rc == 0 || rc == -ENOENT)
            rc = vfs_mkdir(NULL, "/tmp/vc/a/b", 0755);
        if (rc != 0 && rc != -ENOTEMPTY && rc != -EEXIST) {
            h->failures++;
            h->last_rc = rc;
        }
        h->ops++;
    }
}

/* Opens and closes one file: the vnode is instantiated and released over
 * and over on two CPUs at once. */
static void open_hammer(void *arg)
{
    struct vfs_hammer *h = arg;
    while (!__atomic_load_n(&h->stop, __ATOMIC_ACQUIRE)) {
        struct file *f;
        int rc = vfs_open(NULL, "/tmp/vc/shared", COSMO_O_RDONLY, 0, &f);
        if (rc == 0)
            file_put(f);
        else {
            h->failures++;
            h->last_rc = rc;
        }
        h->ops++;
    }
}

static struct thread *hammer_on(void (*fn)(void *), struct vfs_hammer *h, unsigned cpu)
{
    return thread_create_on(fn, h, "vfs-hammer", SCHED_PRIO_DEFAULT, CPUMASK_OF(cpu));
}

bool selftest_vfs_concurrency(const char **reason)
{
    unsigned other = 0;
    for (unsigned c = 1; c < cpu_count(); c++)
        if (cpu_online(c)) {
            other = c;
            break;
        }
    unsigned vnodes0 = vfs_vnode_count();
    struct file *f;
    CHECK(vfs_mkdir(NULL, "/tmp/vc", 0755) == 0);
    CHECK(vfs_mkdir(NULL, "/tmp/vc/a", 0755) == 0);
    CHECK(vfs_mkdir(NULL, "/tmp/vc/a/b", 0755) == 0);
    CHECK(vfs_open(NULL, "/tmp/vc/a/x", COSMO_O_WRONLY | COSMO_O_CREAT, 0644, &f) == 0);
    file_put(f);
    CHECK(vfs_open(NULL, "/tmp/vc/shared", COSMO_O_WRONLY | COSMO_O_CREAT, 0644, &f) == 0);
    file_put(f);

    /* 1. rename vs rmdir/mkdir of the destination directory, 200 ms. */
    struct vfs_hammer rn = { 0 }, rm = { 0 };
    struct thread *t1 = hammer_on(rename_hammer, &rn, 0);
    struct thread *t2 = hammer_on(rmdir_hammer, &rm, other);
    CHECK(t1 != NULL && t2 != NULL);
    thread_sleep_ms(200);
    __atomic_store_n(&rn.stop, 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&rm.stop, 1u, __ATOMIC_RELEASE);
    thread_join(t1);
    thread_join(t2);
    CHECK(rn.failures == 0);
    CHECK(rm.failures == 0);
    CHECK(rn.ops > 0 && rm.ops > 0);

    /* The tree is consistent: x is in exactly one of its two places, b may or may not exist. */
    struct cosmo_stat st;
    int at_a = vfs_stat(NULL, "/tmp/vc/a/x", &st);
    int at_b = vfs_stat(NULL, "/tmp/vc/a/b/y", &st);
    CHECK((at_a == 0) != (at_b == 0));
    if (at_b == 0)
        CHECK(vfs_rename(NULL, "/tmp/vc/a/b/y", "/tmp/vc/a/x") == 0);

    /* 2. Two CPUs open and close one file: one vnode per inode, always. */
    struct vfs_hammer o1 = { 0 }, o2 = { 0 };
    t1 = hammer_on(open_hammer, &o1, 0);
    t2 = hammer_on(open_hammer, &o2, other);
    CHECK(t1 != NULL && t2 != NULL);
    thread_sleep_ms(200);
    __atomic_store_n(&o1.stop, 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&o2.stop, 1u, __ATOMIC_RELEASE);
    thread_join(t1);
    thread_join(t2);
    CHECK(o1.failures == 0 && o2.failures == 0);
    CHECK(o1.ops > 0 && o2.ops > 0);
    /* ramfs pins its vnodes, so the count is exact: the five we created and nothing duplicated. */
    CHECK(vfs_vnode_count() == vnodes0 + 5 || vfs_vnode_count() == vnodes0 + 4);   /* b may be absent */

    CHECK(vfs_unlink(NULL, "/tmp/vc/shared") == 0);
    CHECK(vfs_unlink(NULL, "/tmp/vc/a/x") == 0);
    if (vfs_stat(NULL, "/tmp/vc/a/b", &st) == 0)
        CHECK(vfs_rmdir(NULL, "/tmp/vc/a/b") == 0);
    CHECK(vfs_rmdir(NULL, "/tmp/vc/a") == 0);
    CHECK(vfs_rmdir(NULL, "/tmp/vc") == 0);
    CHECK(vfs_vnode_count() == vnodes0);
    kinfo("selftest: vfs-concurrency: %u rename rounds against %u rmdir/mkdir rounds, %u+%u open/close on CPUs 0 and %u",
          rn.ops, rm.ops, o1.ops, o2.ops, other);
    return true;
}

/* --- the ramfs page budget and the global page-cache limit with reclaim --- */

#include <kernel/blk.h>
#include <kernel/cosmofs.h>
#include <kernel/pagecache.h>
#include <kernel/ramblk.h>

static struct mount *mount_at(const char *path)
{
    struct vnode *vn;
    if (vfs_lookup(NULL, path, &vn))
        return NULL;
    struct mount *m = vn->mnt;
    vnode_put(vn);
    return m;
}

bool selftest_cache_limits(const char **reason)
{
    static uint8_t buf[PAGE_SIZE];
    struct pagecache_stats s0, s1;

    /* 1. A ramfs mount with a budget of four pages: the fifth page is
     * -ENOSPC, the write before it is short, freeing a file makes room. */
    int mk = vfs_mkdir(NULL, "/mnt/rcap", 0755);
    CHECK(mk == 0 || mk == -EEXIST);
    CHECK(vfs_mount("/mnt/rcap", "ramfs", NULL, 0) == 0);
    struct mount *rm = mount_at("/mnt/rcap");
    CHECK(rm != NULL && (rm->flags & MOUNT_CACHE_IS_STORE) && rm->cache_limit_pages == 16384);
    rm->cache_limit_pages = 4;
    struct file *f;
    CHECK(vfs_open(NULL, "/mnt/rcap/a", COSMO_O_RDWR | COSMO_O_CREAT, 0644, &f) == 0);
    memset(buf, 0x5a, sizeof(buf));
    for (int i = 0; i < 4; i++)
        CHECK(file_write(f, buf, PAGE_SIZE) == PAGE_SIZE);
    CHECK(rm->cache_pages == 4);
    pagecache_get_stats(&s0);
    CHECK(file_write(f, buf, PAGE_SIZE) == -ENOSPC);
    CHECK(file_write(f, buf, 16) == -ENOSPC);
    pagecache_get_stats(&s1);
    CHECK(s1.budget_refusals == s0.budget_refusals + 2);
    file_put(f);
    struct file *g;
    CHECK(vfs_open(NULL, "/mnt/rcap/b", COSMO_O_RDWR | COSMO_O_CREAT, 0644, &g) == 0);
    CHECK(file_write(g, buf, PAGE_SIZE) == -ENOSPC);   /* the budget is the mount's, not the file's */
    CHECK(vfs_unlink(NULL, "/mnt/rcap/a") == 0);
    CHECK(rm->cache_pages == 0);
    CHECK(file_write(g, buf, 2 * PAGE_SIZE) == 2 * PAGE_SIZE);
    file_put(g);
    CHECK(vfs_unlink(NULL, "/mnt/rcap/b") == 0);
    CHECK(vfs_umount("/mnt/rcap") == 0);
    CHECK(vfs_rmdir(NULL, "/mnt/rcap") == 0);

    /* 2. cosmofs on a RAM device: a 2 MiB file read back under a global
     * limit that forces reclaim of its clean pages. ramfs pages (the
     * root's) are never touched. */
    struct blkdev *bd = ramblk_create(1024);
    CHECK(bd != NULL);
    CHECK(cosmofs_format(bd) == 0);
    mk = vfs_mkdir(NULL, "/mnt/rcl", 0755);
    CHECK(mk == 0 || mk == -EEXIST);
    CHECK(vfs_mount("/mnt/rcl", "cosmofs", bd, 0) == 0);
    struct mount *root_mnt = mount_at("/");
    uint64_t root_pages0 = root_mnt->cache_pages;
    CHECK(vfs_open(NULL, "/mnt/rcl/big", COSMO_O_RDWR | COSMO_O_CREAT, 0644, &f) == 0);
    const unsigned NPAGES = 512;
    for (unsigned i = 0; i < NPAGES; i++) {
        memset(buf, (int)(i & 0xff), sizeof(buf));
        CHECK(file_write(f, buf, PAGE_SIZE) == PAGE_SIZE);
        if ((i + 1) % 64 == 0) {
            int src = file_sync(f);   /* every page clean: all reclaimable */
            if (src)
                kerror("cache-limits: sync after %u pages: %d", i + 1, src);
            CHECK(src == 0);
        }
    }
    pagecache_get_stats(&s0);
    CHECK(s0.pages >= NPAGES);
    uint64_t saved = pagecache_limit();
    pagecache_set_limit(s0.pages - NPAGES / 2);   /* below what is cached: reads must evict */
    for (unsigned i = 0; i < NPAGES; i++) {
        CHECK(file_pread(f, buf, PAGE_SIZE, (uint64_t)i * PAGE_SIZE) == PAGE_SIZE);
        CHECK(buf[0] == (uint8_t)(i & 0xff) && buf[PAGE_SIZE - 1] == (uint8_t)(i & 0xff));
    }
    pagecache_get_stats(&s1);
    pagecache_set_limit(saved);
    CHECK(s1.reclaimed > s0.reclaimed);
    CHECK(s1.pages <= s0.pages - NPAGES / 2 + 64);   /* at most one reclaim batch over the limit */
    CHECK(root_mnt->cache_pages == root_pages0);       /* nothing of ramfs was evicted */
    uint64_t reclaimed = s1.reclaimed - s0.reclaimed;
    /* A dirty page is not reclaimable: writes beyond the limit stay cached. */
    pagecache_get_stats(&s0);
    pagecache_set_limit(1);
    memset(buf, 0x77, sizeof(buf));
    CHECK(file_pwrite(f, buf, PAGE_SIZE, 0) == PAGE_SIZE);
    CHECK(file_pread(f, buf, PAGE_SIZE, 0) == PAGE_SIZE && buf[10] == 0x77);
    pagecache_set_limit(saved);
    file_put(f);
    CHECK(vfs_unlink(NULL, "/mnt/rcl/big") == 0);
    CHECK(vfs_umount("/mnt/rcl") == 0);
    CHECK(vfs_rmdir(NULL, "/mnt/rcl") == 0);
    ramblk_destroy(bd);
    kinfo("selftest: cache-limits: ramfs budget refused %llu misses; %llu clean pages reclaimed under the global limit",
          (unsigned long long)s1.budget_refusals, (unsigned long long)reclaimed);
    return true;
}
