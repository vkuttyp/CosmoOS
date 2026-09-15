/*
 * vfstest.c - Self-tests for CRC32C, the page cache and the VFS on ramfs.
 */

#include <kernel/blk.h>
#include <kernel/cosmofs.h>
#include <kernel/crc32c.h>
#include <kernel/faultinject.h>
#include <kernel/object.h>
#include <kernel/pagecache.h>
#include <kernel/pipe.h>
#include <kernel/ramblk.h>
#include <kernel/syscall.h>
#include <kernel/errno.h>
#include <kernel/handle.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/mountns.h>
#include <kernel/page.h>
#include <kernel/utsns.h>
#include <kernel/selftest.h>
#include <kernel/printf.h>
#include <kernel/string.h>
#include <kernel/timer.h>
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

/*
 * Symbolic links (docs/audit/next-subsystem-symlink.md). Everything here
 * is ramfs under /tmp; cosmofs's own links are cosmofs-symlink.
 *
 * Every "the link was followed" assertion is paired with a target whose
 * contents, type or size differ from the link's, so a test cannot pass
 * by reading the same bytes either way.
 */
static int mk_file(const char *path, const char *text)
{
    struct file *f;
    int rc = vfs_open(NULL, path, COSMO_O_WRONLY | COSMO_O_CREAT | COSMO_O_TRUNC, 0644, &f);
    if (rc)
        return rc;
    int64_t n = file_write(f, text, strlen(text));
    file_put(f);
    return n == (int64_t)strlen(text) ? 0 : (int)n;
}

static int read_all(const char *path, char *buf, size_t len)
{
    struct file *f;
    int rc = vfs_open(NULL, path, COSMO_O_RDONLY, 0, &f);
    if (rc)
        return rc;
    int64_t n = file_read(f, buf, len - 1);
    file_put(f);
    if (n < 0)
        return (int)n;
    buf[n] = '\0';
    return (int)n;
}

bool selftest_vfs_symlink(const char **reason)
{
    struct cosmo_stat st, lst;
    char buf[64];

    CHECK(vfs_mkdir(NULL, "/tmp/sl", 0755) == 0);
    CHECK(mk_file("/tmp/sl/file", "target-bytes") == 0);

    /* Create, read back exactly, and see a fourth type. */
    CHECK(vfs_symlink(NULL, "/tmp/sl/link", "file") == 0);
    CHECK(vfs_symlink(NULL, "/tmp/sl/link", "file") == -EEXIST);
    memset(buf, 'Z', sizeof(buf));
    int n = vfs_readlink(NULL, "/tmp/sl/link", buf, sizeof(buf));
    CHECK(n == 4 && memcmp(buf, "file", 4) == 0);
    CHECK(buf[4] == 'Z');              /* not terminated, as POSIX says */
    CHECK(vfs_readlink(NULL, "/tmp/sl/file", buf, sizeof(buf)) == -EINVAL);

    /* lstat is the link, stat is the target, and they differ in type and size. */
    CHECK(vfs_lstat(NULL, "/tmp/sl/link", &lst) == 0);
    CHECK(lst.type == COSMO_DT_LNK && lst.size == 4);
    CHECK(vfs_stat(NULL, "/tmp/sl/link", &st) == 0);
    CHECK(st.type == COSMO_DT_REG && st.size == strlen("target-bytes"));
    CHECK(st.ino != lst.ino);

    /* Opening it reads the target's bytes. */
    CHECK(read_all("/tmp/sl/link", buf, sizeof(buf)) == (int)strlen("target-bytes"));
    CHECK(strcmp(buf, "target-bytes") == 0);

    /* Removing the link leaves the target. */
    CHECK(vfs_unlink(NULL, "/tmp/sl/link") == 0);
    CHECK(vfs_stat(NULL, "/tmp/sl/file", &st) == 0 && st.size == strlen("target-bytes"));
    CHECK(vfs_lstat(NULL, "/tmp/sl/link", &lst) == -ENOENT);

    CHECK(vfs_unlink(NULL, "/tmp/sl/file") == 0);
    CHECK(vfs_rmdir(NULL, "/tmp/sl") == 0);
    kinfo("selftest: vfs-symlink: created, read without a terminator, lstat the link and stat the target");
    return true;
}

bool selftest_vfs_symlink_walk(const char **reason)
{
    struct cosmo_stat st;
    char buf[64];

    CHECK(vfs_mkdir(NULL, "/tmp/w", 0755) == 0);
    CHECK(vfs_mkdir(NULL, "/tmp/w/d", 0755) == 0);
    CHECK(mk_file("/tmp/w/d/inner", "inner-bytes") == 0);
    CHECK(mk_file("/tmp/w/outer", "outer-bytes") == 0);

    /* A link to a directory is walked through. */
    CHECK(vfs_symlink(NULL, "/tmp/w/dlink", "d") == 0);
    CHECK(read_all("/tmp/w/dlink/inner", buf, sizeof(buf)) > 0);
    CHECK(strcmp(buf, "inner-bytes") == 0);
    CHECK(vfs_stat(NULL, "/tmp/w/dlink/", &st) == 0 && st.type == COSMO_DT_DIR);

    /* A relative target resolves against the link's own directory, not
     * the caller's start: this link lives in d and names ../outer. */
    CHECK(vfs_symlink(NULL, "/tmp/w/d/up", "../outer") == 0);
    CHECK(read_all("/tmp/w/d/up", buf, sizeof(buf)) > 0);
    CHECK(strcmp(buf, "outer-bytes") == 0);

    /* ".." after an expansion names the target's parent. */
    CHECK(read_all("/tmp/w/dlink/../outer", buf, sizeof(buf)) > 0);
    CHECK(strcmp(buf, "outer-bytes") == 0);

    /* An absolute target. */
    CHECK(vfs_symlink(NULL, "/tmp/w/abs", "/tmp/w/d/inner") == 0);
    CHECK(read_all("/tmp/w/abs", buf, sizeof(buf)) > 0);
    CHECK(strcmp(buf, "inner-bytes") == 0);

    /* A link in the middle of a path, and one to a link. */
    CHECK(vfs_symlink(NULL, "/tmp/w/d/second", "up") == 0);
    CHECK(read_all("/tmp/w/dlink/second", buf, sizeof(buf)) > 0);
    CHECK(strcmp(buf, "outer-bytes") == 0);

    CHECK(vfs_unlink(NULL, "/tmp/w/d/second") == 0);
    CHECK(vfs_unlink(NULL, "/tmp/w/abs") == 0);
    CHECK(vfs_unlink(NULL, "/tmp/w/d/up") == 0);
    CHECK(vfs_unlink(NULL, "/tmp/w/dlink") == 0);
    CHECK(vfs_unlink(NULL, "/tmp/w/outer") == 0);
    CHECK(vfs_unlink(NULL, "/tmp/w/d/inner") == 0);
    CHECK(vfs_rmdir(NULL, "/tmp/w/d") == 0);
    CHECK(vfs_rmdir(NULL, "/tmp/w") == 0);
    kinfo("selftest: vfs-symlink-walk: through a directory link, relative to the link's own directory, .. past an expansion");
    return true;
}

bool selftest_vfs_symlink_loop(const char **reason)
{
    struct cosmo_stat st;
    char name[32], target[32];

    CHECK(vfs_mkdir(NULL, "/tmp/lp", 0755) == 0);

    /* a -> b -> a, and a link to itself. */
    CHECK(vfs_symlink(NULL, "/tmp/lp/a", "b") == 0);
    CHECK(vfs_symlink(NULL, "/tmp/lp/b", "a") == 0);
    CHECK(vfs_stat(NULL, "/tmp/lp/a", &st) == -ELOOP);
    CHECK(vfs_symlink(NULL, "/tmp/lp/self", "self") == 0);
    CHECK(vfs_stat(NULL, "/tmp/lp/self", &st) == -ELOOP);
    /* ... while lstat and readlink still answer, because they do not follow. */
    CHECK(vfs_lstat(NULL, "/tmp/lp/a", &st) == 0 && st.type == COSMO_DT_LNK);

    /* The budget is pinned: a test built only from the macro moves with
     * it and can never see it change. */
    CHECK(VFS_MAX_SYMLINKS == 8);
    /* A chain of exactly VFS_MAX_SYMLINKS resolves; one more is ELOOP.
     * c0 -> c1 -> ... -> c<N-1> -> end, so naming c0 expands N links. */
    CHECK(mk_file("/tmp/lp/end", "chain-end") == 0);
    for (unsigned i = 0; i < VFS_MAX_SYMLINKS; i++) {
        ksnprintf(name, sizeof(name), "/tmp/lp/c%u", i);
        if (i + 1 == VFS_MAX_SYMLINKS)
            ksnprintf(target, sizeof(target), "end");
        else
            ksnprintf(target, sizeof(target), "c%u", i + 1);
        CHECK(vfs_symlink(NULL, name, target) == 0);
    }
    CHECK(vfs_stat(NULL, "/tmp/lp/c0", &st) == 0 && st.size == strlen("chain-end"));
    ksnprintf(name, sizeof(name), "/tmp/lp/c%u", VFS_MAX_SYMLINKS);
    CHECK(vfs_symlink(NULL, name, "c0") == 0);      /* one link longer */
    CHECK(vfs_stat(NULL, name, &st) == -ELOOP);

    for (unsigned i = 0; i <= VFS_MAX_SYMLINKS; i++) {
        ksnprintf(name, sizeof(name), "/tmp/lp/c%u", i);
        CHECK(vfs_unlink(NULL, name) == 0);
    }
    CHECK(vfs_unlink(NULL, "/tmp/lp/end") == 0);
    CHECK(vfs_unlink(NULL, "/tmp/lp/self") == 0);
    CHECK(vfs_unlink(NULL, "/tmp/lp/b") == 0);
    CHECK(vfs_unlink(NULL, "/tmp/lp/a") == 0);
    CHECK(vfs_rmdir(NULL, "/tmp/lp") == 0);
    kinfo("selftest: vfs-symlink-loop: a cycle and a chain of %u are ELOOP; a chain of %u resolves",
          VFS_MAX_SYMLINKS + 1, VFS_MAX_SYMLINKS);
    return true;
}

bool selftest_vfs_symlink_nofollow(const char **reason)
{
    struct cosmo_stat st;
    struct file *f;
    char buf[VFS_PATH_MAX + 8];

    CHECK(vfs_mkdir(NULL, "/tmp/nf", 0755) == 0);
    CHECK(mk_file("/tmp/nf/file", "nf-bytes") == 0);
    CHECK(vfs_symlink(NULL, "/tmp/nf/link", "file") == 0);

    /* O_NOFOLLOW refuses a link as the last component, takes a file, and
     * says nothing about links in the middle. */
    CHECK(vfs_open(NULL, "/tmp/nf/link", COSMO_O_RDONLY | COSMO_O_NOFOLLOW, 0, &f) == -ELOOP);
    CHECK(vfs_open(NULL, "/tmp/nf/file", COSMO_O_RDONLY | COSMO_O_NOFOLLOW, 0, &f) == 0);
    file_put(f);
    CHECK(vfs_mkdir(NULL, "/tmp/nf/d", 0755) == 0);
    CHECK(mk_file("/tmp/nf/d/inner", "inner") == 0);
    CHECK(vfs_symlink(NULL, "/tmp/nf/dlink", "d") == 0);
    CHECK(vfs_open(NULL, "/tmp/nf/dlink/inner", COSMO_O_RDONLY | COSMO_O_NOFOLLOW, 0, &f) == 0);
    file_put(f);

    /* A dangling link: open finds nothing, the link itself is there. */
    CHECK(vfs_symlink(NULL, "/tmp/nf/dangling", "nowhere") == 0);
    CHECK(vfs_open(NULL, "/tmp/nf/dangling", COSMO_O_RDONLY, 0, &f) == -ENOENT);
    CHECK(vfs_stat(NULL, "/tmp/nf/dangling", &st) == -ENOENT);
    CHECK(vfs_lstat(NULL, "/tmp/nf/dangling", &st) == 0 && st.type == COSMO_DT_LNK);
    CHECK(vfs_unlink(NULL, "/tmp/nf/dangling") == 0);

    /* A target that cannot fit a path is refused rather than truncated. */
    memset(buf, 'x', sizeof(buf));
    buf[VFS_PATH_MAX + 7] = '\0';
    CHECK(vfs_symlink(NULL, "/tmp/nf/toolong", buf) == -ENAMETOOLONG);
    /* ... and one that fits alone but not with a remainder after it.
     * Built from short components on purpose: a single 1000-byte name
     * would be refused for its own length, and the test would pass
     * without the expansion's length check ever running. */
    size_t at = 0;
    while (at + 3 < VFS_PATH_MAX - 16) {
        buf[at++] = 'a';
        buf[at++] = 'a';
        buf[at++] = '/';
    }
    buf[at - 1] = '\0';   /* no trailing slash */
    CHECK(vfs_symlink(NULL, "/tmp/nf/long", buf) == 0);
    CHECK(vfs_stat(NULL, "/tmp/nf/long", &st) == -ENOENT);   /* it fits: it simply is not there */
    CHECK(vfs_stat(NULL, "/tmp/nf/long/and/more/components/still", &st) == -ENAMETOOLONG);
    CHECK(vfs_unlink(NULL, "/tmp/nf/long") == 0);

    CHECK(vfs_unlink(NULL, "/tmp/nf/dlink") == 0);
    CHECK(vfs_unlink(NULL, "/tmp/nf/d/inner") == 0);
    CHECK(vfs_rmdir(NULL, "/tmp/nf/d") == 0);
    CHECK(vfs_unlink(NULL, "/tmp/nf/link") == 0);
    CHECK(vfs_unlink(NULL, "/tmp/nf/file") == 0);
    CHECK(vfs_rmdir(NULL, "/tmp/nf") == 0);
    kinfo("selftest: vfs-symlink-nofollow: O_NOFOLLOW is ELOOP on a link and nothing in the middle; dangling and over-long are refused");
    return true;
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
    /* Onto its own parent: the destination lookup returns a directory
     * this rename has already locked, so every check on it must be one
     * that does not take the lock again. */
    CHECK(vfs_rename(NULL, "/tmp/d1/d2", "/tmp/d1") == -ENOTEMPTY);
    CHECK(vfs_stat(NULL, "/tmp/d1/d2", &st) == 0);
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

    /* Mount a second ramfs on /mnt, use it, unmount it. Counted
     * against what was mounted when this started rather than against a
     * number: what the system mounts at boot is not this test's
     * business, and it grew a /proc. */
    unsigned mounts0 = vfs_mount_count();
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
    CHECK(vfs_mount_count() == mounts0 + 1);
    CHECK(vfs_umount("/mnt") == 0);
    CHECK(vfs_umount("/mnt") == -EINVAL);                                /* not a mount root */
    CHECK(vfs_umount("/") == -EBUSY);
    CHECK(vfs_stat(NULL, "/mnt/inner", &st) == -ENOENT);                /* the ramfs is gone */
    CHECK(vfs_mount_count() == mounts0);
    CHECK(vfs_vnode_count() == vnodes0);
    return true;
}

/* fsync(fd) is the exact path SYS_fsync runs: resolve a handle to a file and
 * commit that one file (not every mount, which is SYS_sync). A bad handle is
 * -EBADF. The device-side flush that leans on it lives in test_vblk_dev; this
 * proves the handle-to-file-to-sync path the syscall wraps. */
bool selftest_fsync_handle(const char **reason)
{
    struct handle_table *t = kmalloc(sizeof(*t), KMEM_ZERO);
    CHECK(t != NULL);
    handle_table_init(t);

    struct file *f;
    CHECK(vfs_open(NULL, "/tmp/fsync.txt", COSMO_O_RDWR | COSMO_O_CREAT, 0644, &f) == 0);
    CHECK(file_write(f, "durable", 7) == 7);
    int h = handle_install(t, &f->obj, HANDLE_RIGHT_OWNER);
    CHECK(h >= 0);
    file_put(f);   /* the table holds the reference now */

    /* what sys_fsync does: look the handle up, take the file, sync it */
    struct kobject *obj = handle_lookup(t, h, 0);
    CHECK(obj != NULL);
    struct file *lf = file_from_kobject(obj);
    CHECK(lf != NULL);
    CHECK(file_sync(lf) == 0);
    kobject_put(obj);

    /* a handle that names nothing is where the syscall returns -EBADF */
    CHECK(handle_lookup(t, 99, 0) == NULL);

    handle_table_destroy(t);
    kfree(t);
    CHECK(vfs_unlink(NULL, "/tmp/fsync.txt") == 0);
    return true;
}

/* --- vfs-concurrency: rename against rmdir, lookup against release --------------
 *
 * Two CPUs when available (docs/kernel/lockdep/testing.md). The audit's two
 * findings: rename locked its parents in address order while rmdir locked
 * parent then child (an ABBA, now excluded by the rename lock and the
 * ancestor-first order), and the vnode cache could instantiate a second
 * vnode for an inode whose first was mid-release (excluded now by
 * vnode_put reaching zero and unhashing under the mount lock in one
 * act; vfs-put-race is the direct test of that). Under the debug-build checker any lock
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

/* --- the uts namespace (docs/kernel/security/design.md §1e, S13) -------------
 *
 * The namespace API takes the namespace to act on, so unlike the mount
 * side this can be shown directly: a namespace made now carries the
 * name current at that moment, and the two then move independently.
 */
bool selftest_utsns(const char **reason)
{
    /* On namespaces of its own, never on the one the system boots in:
     * a CHECK that fails returns on the spot, and a test that had
     * renamed the machine would leave the rest of the boot -- including
     * the user-mode test, which compares names -- reading a test value
     * and failing somewhere that says nothing about the cause. */
    struct uts_ns *a = NULL, *b = NULL;
    char got[COSMO_HOST_NAME_MAX];
    CHECK(utsns_create(utsns_initial(), &a) == 0);
    struct uts_ns *init = a;

    CHECK(utsns_sethostname(init, "before", 6) == 0);
    CHECK(utsns_create(init, &b) == 0);
    /* The copy is of the name at the split. */
    CHECK(utsns_gethostname(b, got, sizeof(got)) == 6 && strcmp(got, "before") == 0);

    /* And the two then move independently, in both directions. */
    CHECK(utsns_sethostname(init, "after", 5) == 0);
    CHECK(utsns_gethostname(b, got, sizeof(got)) == 6 && strcmp(got, "before") == 0);
    CHECK(utsns_sethostname(b, "child", 5) == 0);
    CHECK(utsns_gethostname(init, got, sizeof(got)) == 5 && strcmp(got, "after") == 0);

    /* A name that would arrive different from how it was sent is
     * refused rather than trimmed: one holding a newline can forge a
     * log line, one holding a NUL is not the string the setter set. */
    CHECK(utsns_sethostname(init, "a\nb", 3) == -EINVAL);
    CHECK(utsns_sethostname(init, "a\0b", 3) == -EINVAL);
    CHECK(utsns_sethostname(init, "", 0) == -EINVAL);
    char toolong[COSMO_HOST_NAME_MAX + 8];
    memset(toolong, 'x', sizeof(toolong));
    CHECK(utsns_sethostname(init, toolong, COSMO_HOST_NAME_MAX) == -EINVAL);
    /* One byte short of the limit fits, terminator included. */
    CHECK(utsns_sethostname(init, toolong, COSMO_HOST_NAME_MAX - 1) == 0);
    CHECK(utsns_gethostname(init, got, sizeof(got)) == COSMO_HOST_NAME_MAX - 1);

    /* A short buffer truncates and says how much it wrote, rather than
     * overrunning or reporting the length it wanted. */
    char small[4];
    CHECK(utsns_gethostname(init, small, sizeof(small)) == 3 && small[3] == 0);

    utsns_put(b);
    utsns_put(a);
    kinfo("selftest: utsns: a namespace carries the name it was made with, and the two move apart");
    return true;
}

/* --- mount namespaces (docs/kernel/security/design.md §1d, V28) ---------------
 *
 * These run in the namespace the system boots in, which is the only one
 * kmain has, so the isolation is shown through the mount count rather
 * than by walking paths from two sides: a mount two namespaces can see
 * survives the first unmount and goes on the second, and a mount made
 * after a namespace was created is one that namespace never gets.
 * Walking it from both sides is what the user-mode test does.
 */
bool selftest_mountns(const char **reason)
{
    unsigned n0 = vfs_mount_count();
    CHECK(vfs_mkdir(NULL, "/tmp/ns", 0755) == 0);

    /* A namespace made now can see the mount made before it. */
    CHECK(vfs_mount("/tmp/ns", "ramfs", NULL, 0) == 0);
    CHECK(vfs_mount_count() == n0 + 1);
    struct mount_ns *b = NULL;
    CHECK(mountns_create(mountns_initial(), &b) == 0);

    /* Unmounting here only drops this namespace's view: b still sees
     * it, so the filesystem stays and the count does not move. */
    CHECK(vfs_umount("/tmp/ns") == 0);
    CHECK(vfs_mount_count() == n0 + 1);
    /* And it is gone from *this* namespace, so the directory is free
     * for another mount even though the first one still exists. */
    CHECK(vfs_mount("/tmp/ns", "ramfs", NULL, 0) == 0);
    CHECK(vfs_mount_count() == n0 + 2);

    /* The second mount was made after b, so b never saw it: unmounting
     * it here is the last namespace out and takes it away. If b had
     * copied it, this count would stay. */
    CHECK(vfs_umount("/tmp/ns") == 0);
    CHECK(vfs_mount_count() == n0 + 1);

    /* And the last namespace that can see the first mount takes it
     * with it when it goes. */
    mountns_put(b);
    CHECK(vfs_mount_count() == n0);

    /* A namespace that goes away having mounted nothing costs nothing. */
    struct mount_ns *c = NULL;
    CHECK(mountns_create(mountns_initial(), &c) == 0);
    mountns_put(c);
    CHECK(vfs_mount_count() == n0);

    CHECK(vfs_rmdir(NULL, "/tmp/ns") == 0);
    kinfo("selftest: mountns: a mount two namespaces see survives the first unmount and goes on the second");
    return true;
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

/* Two threads fill and free distinct files on a four-page mount while a
 * third samples the mount's page count: the reservation is the admission,
 * so the count never exceeds the budget (Greptile on PR #21 found the
 * earlier read-then-charge window). */
struct budget_stress {
    const char *path;
    unsigned enospc, pages;
};

static volatile bool g_budget_stop;
static volatile uint64_t g_budget_peak;
static struct mount *g_budget_mnt;

static void budget_writer(void *arg)
{
    struct budget_stress *st = arg;
    static const uint8_t page[PAGE_SIZE] = { 1 };
    for (unsigned round = 0; round < 40; round++) {
        struct file *f;
        if (vfs_open(NULL, st->path, COSMO_O_RDWR | COSMO_O_CREAT | COSMO_O_TRUNC, 0644, &f) != 0)
            continue;
        for (unsigned i = 0; i < 6; i++) {
            int64_t n = file_write(f, page, PAGE_SIZE);
            if (n == PAGE_SIZE)
                st->pages++;
            else if (n == -ENOSPC)
                st->enospc++;
        }
        file_put(f);
        vfs_unlink(NULL, st->path);
    }
}

static void budget_sampler(void *arg)
{
    (void)arg;
    while (!g_budget_stop) {
        uint64_t n = __atomic_load_n(&g_budget_mnt->cache_pages, __ATOMIC_RELAXED);
        if (n > g_budget_peak)
            g_budget_peak = n;
        sched_yield();
    }
}

bool selftest_cache_budget_race(const char **reason)
{
    int mk = vfs_mkdir(NULL, "/mnt/rrace", 0755);
    CHECK(mk == 0 || mk == -EEXIST);
    CHECK(vfs_mount("/mnt/rrace", "ramfs", NULL, 0) == 0);
    struct vnode *rv;
    CHECK(vfs_lookup(NULL, "/mnt/rrace", &rv) == 0);
    g_budget_mnt = rv->mnt;
    vnode_put(rv);
    g_budget_mnt->cache_limit_pages = 4;
    g_budget_stop = false;
    g_budget_peak = 0;
    struct budget_stress a = { .path = "/mnt/rrace/a" }, b = { .path = "/mnt/rrace/b" };
    struct thread *sampler = thread_create(budget_sampler, NULL, "budget-sampler", SCHED_PRIO_DEFAULT);
    struct thread *ta = thread_create(budget_writer, &a, "budget-a", SCHED_PRIO_DEFAULT);
    struct thread *tb = thread_create(budget_writer, &b, "budget-b", SCHED_PRIO_DEFAULT);
    CHECK(sampler && ta && tb);
    thread_join(ta);
    thread_join(tb);
    g_budget_stop = true;
    thread_join(sampler);
    CHECK(a.enospc + b.enospc > 0);   /* twelve pages wanted per round, four allowed */
    CHECK(a.pages + b.pages > 0);
    CHECK(g_budget_peak <= 4);
    CHECK(g_budget_mnt->cache_pages == 0);   /* every reservation was charged or returned */
    CHECK(vfs_umount("/mnt/rrace") == 0);
    CHECK(vfs_rmdir(NULL, "/mnt/rrace") == 0);
    kinfo("selftest: cache-budget-race: %u pages admitted, %u refused across two writers; peak %llu of a budget of 4",
          a.pages + b.pages, a.enospc + b.enospc, (unsigned long long)g_budget_peak);
    return true;
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

/* --- vfs-put-race: the last references of one vnode, dropped at once ------------
 *
 * One vnode, one reference per CPU, every CPU told to drop at the same
 * moment, thousands of times. The version of vnode_put that read the
 * count before deciding lets two of those drops both read 2 and neither
 * unhash, and the release then asserts on a vnode still in the hash. The
 * vnode is a bare one on the root mount with no ops, so the release has
 * nothing to sync or evict: this is a test of the cache protocol alone.
 */
/*
 * The handshake is release/acquire, not volatile: the driver publishes
 * `vn` and then bumps `generation` with a release, and a racer that
 * acquires the new generation is thereby guaranteed to see the new `vn`
 * and not the previous round's, which has been freed. The first version
 * used volatile loads, which order nothing on a weakly ordered machine
 * and passed only because QEMU's TCG does not reorder loads as silicon
 * does -- a test of vnode_put's ordering with an ordering bug of its own.
 */
struct put_racer {
    struct vnode *vn;       /* the vnode to drop; published before `generation` */
    unsigned generation;    /* bumped (release) by the driver for each round */
    unsigned acks;          /* racers that have dropped this round */
    unsigned stop;
};

static void put_racer_main(void *arg)
{
    struct put_racer *r = arg;
    unsigned seen = 0;
    while (!__atomic_load_n(&r->stop, __ATOMIC_ACQUIRE)) {
        unsigned g = __atomic_load_n(&r->generation, __ATOMIC_ACQUIRE);
        if (g == seen) {
            /* Spin, not yield: the point is to arrive at the drop at the
             * same instant as the others, and a yield hands that instant
             * away. */
            continue;
        }
        seen = g;
        struct vnode *vn = __atomic_load_n(&r->vn, __ATOMIC_ACQUIRE);
        vnode_put(vn);
        __atomic_fetch_add(&r->acks, 1u, __ATOMIC_RELEASE);
    }
}

bool selftest_vfs_put_race(const char **reason)
{
    /* The racers spin, so each needs a CPU of its own and the driver
     * needs one too: racers on CPUs 1..n-1, the driver left CPU 0. The
     * first version put a spinning racer on every CPU and the driver ran
     * only on preemption ticks -- 51 seconds for what takes a fraction of
     * one. Two racers is enough to race; fewer than three CPUs is a skip. */
    unsigned ncpu = cpu_count();
    if (ncpu < 3) {
        kinfo("selftest: vfs-put-race: %u CPU(s); skipping", ncpu);
        return true;
    }
    unsigned racers = ncpu - 1 > 8 ? 8 : ncpu - 1;
    unsigned vnodes0 = vfs_vnode_count();
    struct vnode *rootv;
    CHECK(vfs_lookup(NULL, "/", &rootv) == 0);
    struct mount *mnt = rootv->mnt;   /* held through rootv for the whole test */

    struct put_racer r = { 0 };
    struct thread *t[8];
    for (unsigned i = 0; i < racers; i++)
        t[i] = thread_create_on(put_racer_main, &r, "put-racer", SCHED_PRIO_DEFAULT, CPUMASK_OF(i + 1));

    /* Bounded by time as well as count, so a slow host runs fewer
     * rounds rather than a long test. */
    const unsigned max_rounds = 4000;
    uint64_t stop_at = clock_now_ns() + 500ull * 1000000ull;
    unsigned round = 0;
    for (; round < max_rounds && clock_now_ns() < stop_at; round++) {
        /* A fresh hashed vnode holding exactly one reference per racer:
         * the creator's reference and racers-1 more. */
        struct vnode *vn = vnode_alloc(mnt, 0x7f000000ull + round);
        CHECK(vn != NULL);
        vn->type = VNODE_REG;
        vn->ops = NULL;
        vnode_hash_insert(vn);
        for (unsigned i = 1; i < racers; i++)
            vnode_get(vn);
        __atomic_store_n(&r.vn, vn, __ATOMIC_RELEASE);
        __atomic_store_n(&r.acks, 0u, __ATOMIC_RELEASE);
        __atomic_fetch_add(&r.generation, 1u, __ATOMIC_RELEASE);
        uint64_t deadline = clock_now_ns() + 2ull * NS_PER_SEC;
        while (__atomic_load_n(&r.acks, __ATOMIC_ACQUIRE) < racers && clock_now_ns() < deadline)
            sched_yield();
        CHECK(__atomic_load_n(&r.acks, __ATOMIC_ACQUIRE) == racers);
    }
    __atomic_store_n(&r.stop, 1u, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < racers; i++)
        if (t[i])
            thread_join(t[i]);
    vnode_put(rootv);
    /* Every vnode was released exactly once and left the hash: the
     * census is back where it started. */
    CHECK(vfs_vnode_count() == vnodes0);
    kinfo("selftest: vfs-put-race: %u rounds of %u concurrent last drops, every vnode released once", round, racers);
    return true;
}


/* --- per-open character device lifecycle (chrdev open/release) ------------ */

struct chropen_inst { unsigned id; };
static unsigned g_chropen_opens, g_chropen_releases, g_chropen_refuse;

static int chropen_open(struct vnode *vn, struct file *f)
{
    (void)vn;
    if (g_chropen_refuse)
        return -EBUSY;
    struct chropen_inst *i = kmalloc(sizeof(*i), 0);
    if (i == NULL)
        return -ENOMEM;
    i->id = ++g_chropen_opens;
    f->priv = i;
    return 0;
}

static void chropen_release(struct vnode *vn, struct file *f)
{
    (void)vn;
    g_chropen_releases++;
    kfree(f->priv);
    f->priv = NULL;
}

/* read returns this open's id, so two files prove they carry distinct state */
static int64_t chropen_read_file(struct vnode *vn, struct file *f, uint64_t off, void *buf, size_t len)
{
    (void)vn; (void)off;
    struct chropen_inst *i = f->priv;
    if (len < sizeof(unsigned))
        return -EMSGSIZE;
    memcpy(buf, &i->id, sizeof(unsigned));
    return (int64_t)sizeof(unsigned);
}

static const struct chrdev_ops chropen_ops = {
    .open = chropen_open, .release = chropen_release, .read_file = chropen_read_file,
};

/*
 * A mount's name (docs/audit/next-subsystem-fsctl.md). The assertion
 * that matters is the third: an id is not an index. A filesystem
 * unmounted and another mounted at the same path must not inherit the
 * first one's name, or an operator holding a stale id commands a
 * filesystem they never listed.
 */
bool selftest_vfs_mount_id(const char **reason)
{
    CHECK(vfs_mkdir(NULL, "/tmp/idA", 0755) == 0 || true);
    (void)vfs_umount("/tmp/idA");
    int mk = vfs_mkdir(NULL, "/tmp/idA", 0755);
    CHECK(mk == 0 || mk == -EEXIST);
    mk = vfs_mkdir(NULL, "/tmp/idB", 0755);
    CHECK(mk == 0 || mk == -EEXIST);

    /* Every mount has one, and no two share it. */
    CHECK(vfs_mount("/tmp/idA", "ramfs", NULL, 0) == 0);
    CHECK(vfs_mount("/tmp/idB", "ramfs", NULL, 0) == 0);
    struct mount *a = mount_at("/tmp/idA"), *b = mount_at("/tmp/idB");
    CHECK(a != NULL && b != NULL);
    CHECK(a->id != 0 && b->id != 0);
    CHECK(a->id != b->id);
    uint64_t first = a->id;

    /* The root filesystem has one too, and it is not either of these. */
    struct mount *root = mount_at("/");
    CHECK(root != NULL && root->id != 0);
    CHECK(root->id != a->id && root->id != b->id);

    /* A number is never handed out twice: unmount and mount again at the
     * same path, and the new filesystem is a new name. An index would
     * give the freed slot back and fail here. */
    CHECK(vfs_umount("/tmp/idA") == 0);
    CHECK(vfs_mount("/tmp/idA", "ramfs", NULL, 0) == 0);
    struct mount *again = mount_at("/tmp/idA");
    CHECK(again != NULL);
    CHECK(again->id != first);
    CHECK(again->id != b->id);

    CHECK(vfs_umount("/tmp/idA") == 0);
    CHECK(vfs_umount("/tmp/idB") == 0);
    CHECK(vfs_rmdir(NULL, "/tmp/idA") == 0);
    CHECK(vfs_rmdir(NULL, "/tmp/idB") == 0);
    kinfo("selftest: vfs-mount-id: ids %llu, %llu, then %llu at the same path",
          (unsigned long long)first, (unsigned long long)b->id, (unsigned long long)again->id);
    return true;
}

bool selftest_vfs_chrdev_open(const char **reason)
{
    struct vnode *node = NULL;
    CHECK(ramfs_mkchr("/dev/chropen-test", 0600, &chropen_ops, NULL, &node) == 0);
    g_chropen_opens = g_chropen_releases = 0; g_chropen_refuse = 0;

    /* (1) two opens: two instances with distinct state, each read sees its own */
    struct file *a = NULL, *b = NULL;
    CHECK(vfs_open(NULL, "/dev/chropen-test", COSMO_O_RDWR, 0, &a) == 0 && a != NULL);
    CHECK(vfs_open(NULL, "/dev/chropen-test", COSMO_O_RDWR, 0, &b) == 0 && b != NULL);
    CHECK(g_chropen_opens == 2 && a->priv != b->priv);
    unsigned ida = 0, idb = 0;
    CHECK(file_read(a, &ida, sizeof(ida)) == (int64_t)sizeof(ida) && ida == 1);
    CHECK(file_read(b, &idb, sizeof(idb)) == (int64_t)sizeof(idb) && idb == 2);

    /* (2) release runs exactly once, on the last reference, and not before */
    file_get(a);                                  /* a second reference */
    file_put(a);
    CHECK(g_chropen_releases == 0);               /* not the last one yet */
    file_put(a);
    CHECK(g_chropen_releases == 1);               /* now */
    file_put(b);
    CHECK(g_chropen_releases == 2);

    /* (3) a refused open leaves no file and runs no release */
    g_chropen_refuse = 1;
    struct file *c = NULL;
    CHECK(vfs_open(NULL, "/dev/chropen-test", COSMO_O_RDWR, 0, &c) == -EBUSY && c == NULL);
    CHECK(g_chropen_opens == 2 && g_chropen_releases == 2);
    g_chropen_refuse = 0;

    kinfo("selftest: vfs-chrdev-open: per-open instances distinct, release once on last close, refusal clean");
    return true;
}


/* --- the syscall bounce and a read that fills its buffer -------------------
 *
 * docs/audit/next-subsystem-file-path.md. The bounce every user copy goes
 * through is sized to the request (the stack for a kilobyte and less, the
 * heap above, up to 64 KiB) and degrades to the stack chunk when the heap
 * refuses; one object call returns what it returns. The syscall itself
 * needs a user address, which the kernel's own thread has none of: the
 * helper and the object side are proved here, the syscall's wiring by
 * `init --selftest` (fs_selftest). */

#define RB_FILE_SIZE (200u * 1024u)
static uint8_t rb_pattern(size_t i) { return (uint8_t)((i * 7u) + (i >> 8)); }

bool selftest_read_bounce(const char **reason)
{
    char stack[IO_CHUNK];
    struct io_bounce b;

    /* The helper's sizing. */
    syscall_bounce_get(&b, stack, 512);
    CHECK(!b.heap && b.cap == IO_CHUNK && b.buf == stack);
    syscall_bounce_put(&b);
    syscall_bounce_get(&b, stack, IO_CHUNK);
    CHECK(!b.heap && b.cap == IO_CHUNK);
    syscall_bounce_put(&b);
    syscall_bounce_get(&b, stack, 65536);
    CHECK(b.heap && b.cap == 65536 && b.buf != stack);
    syscall_bounce_put(&b);
    syscall_bounce_get(&b, stack, 100000);
    CHECK(b.heap && b.cap == IO_BOUNCE_MAX);
    syscall_bounce_put(&b);

#if CONFIG_FAULTINJECT
    /* A heap that refuses: the stack chunk, never an error. */
    uint64_t fb0 = syscall_bounce_fallback_count();
    faultinject_set(FI_KMALLOC, 1, 1, thread_current());
    syscall_bounce_get(&b, stack, 65536);
    faultinject_clear(FI_KMALLOC);
    struct fi_stats fst;
    faultinject_stats(FI_KMALLOC, &fst);
    CHECK(fst.hits == 1);
    CHECK(!b.heap && b.cap == IO_CHUNK && b.buf == stack);
    CHECK(syscall_bounce_fallback_count() == fb0 + 1);
    syscall_bounce_put(&b);
#else
    kinfo("selftest: read-bounce: the fallback needs fault injection, compiled out of this build");
#endif

    /* The object side: a 200 KiB ramfs file answers one 64 KiB call with
     * 64 KiB of the right bytes, and 3 KiB from its end with 3 KiB. */
    uint8_t *big = kmalloc(65536, 0);
    CHECK(big != NULL);
    struct file *f;
    CHECK(vfs_open(NULL, "/tmp/bounce.bin", COSMO_O_WRONLY | COSMO_O_CREAT | COSMO_O_TRUNC, 0644, &f) == 0);
    for (size_t off = 0; off < RB_FILE_SIZE; off += 65536) {
        size_t n = RB_FILE_SIZE - off < 65536 ? RB_FILE_SIZE - off : 65536;
        for (size_t i = 0; i < n; i++)
            big[i] = rb_pattern(off + i);
        CHECK(file_write(f, big, n) == (int64_t)n);
    }
    file_put(f);
    CHECK(vfs_open(NULL, "/tmp/bounce.bin", COSMO_O_RDONLY, 0, &f) == 0);
    memset(big, 0, 65536);
    CHECK(file_read(f, big, 65536) == 65536);
    bool ok = true;
    for (size_t i = 0; i < 65536 && ok; i++)
        ok = big[i] == rb_pattern(i);
    CHECK(ok);
    CHECK(file_seek(f, (int64_t)(RB_FILE_SIZE - 3072), COSMO_SEEK_SET) == (int64_t)(RB_FILE_SIZE - 3072));
    CHECK(file_read(f, big, 65536) == 3072);
    CHECK(big[0] == rb_pattern(RB_FILE_SIZE - 3072) && big[3071] == rb_pattern(RB_FILE_SIZE - 1));
    file_put(f);
    CHECK(vfs_unlink(NULL, "/tmp/bounce.bin") == 0);

    /* A pipe holding 300 bytes answers a 64 KiB request with 300: the
     * ceiling rose, the semantics did not. */
    struct kobject *r, *w;
    CHECK(pipe_create(&r, &w) == 0);
    const struct kobject_io_type *wio = kobject_io_of(w), *rio = kobject_io_of(r);
    CHECK(wio && rio);
    CHECK(wio->write(w, big, 300) == 300);
    CHECK(rio->read(r, big, 65536) == 300);
    kobject_put(w);
    kobject_put(r);
    kfree(big);
    return true;
}

/* --- write-back errors: recorded, reported once, counted when lost ---------
 *
 * cosmofs on a RAM block device, which completes every bio in its
 * submitter's context, so FI_BLK_COMPLETE scoped to this thread refuses
 * exactly the next `budget` write-backs this thread issues. */

static int wb_setup(struct blkdev **out)
{
    /* A test that failed mid-way left its mount: take it down first, so
     * one failure does not fail every test after it at setup. */
    (void)vfs_umount2("/mnt/wb", VFS_UMOUNT_FORCE);
    struct blkdev *bd = ramblk_create(256);
    if (bd == NULL)
        return -ENOMEM;
    int rc = cosmofs_format(bd);
    if (rc == 0) {
        rc = vfs_mkdir(NULL, "/mnt/wb", 0755);
        if (rc == -EEXIST)
            rc = 0;
    }
    if (rc == 0)
        rc = vfs_mount("/mnt/wb", "cosmofs", bd, 0);
    if (rc) {
        ramblk_destroy(bd);
        return rc;
    }
    *out = bd;
    return 0;
}

static void wb_teardown(struct blkdev *bd)
{
    vfs_umount("/mnt/wb");
    vfs_rmdir(NULL, "/mnt/wb");
    ramblk_destroy(bd);
}

#if CONFIG_FAULTINJECT

/* A page of a known pattern, `k` distinguishing files. */
static void wb_fill(uint8_t *page, unsigned k)
{
    for (size_t i = 0; i < PAGE_SIZE; i++)
        page[i] = (uint8_t)(i * 3u + k);
}

static bool wb_reads_back(const char *path, const uint8_t *page)
{
    struct file *f;
    if (vfs_open(NULL, path, COSMO_O_RDONLY, 0, &f) != 0)
        return false;
    uint8_t *buf = kmalloc(PAGE_SIZE, 0);
    bool ok = buf != NULL && file_read(f, buf, PAGE_SIZE) == (int64_t)PAGE_SIZE && memcmp(buf, page, PAGE_SIZE) == 0;
    kfree(buf);
    file_put(f);
    return ok;
}

bool selftest_wb_error_fsync(const char **reason)
{
    struct blkdev *bd;
    CHECK(wb_setup(&bd) == 0);
    uint8_t *data = kmalloc(3 * PAGE_SIZE, 0);
    CHECK(data != NULL);
    for (unsigned k = 0; k < 3; k++)
        wb_fill(data + k * PAGE_SIZE, k);
    struct pagecache_stats p0, p1;
    pagecache_get_stats(&p0);

    struct file *f;
    CHECK(vfs_open(NULL, "/mnt/wb/f", COSMO_O_WRONLY | COSMO_O_CREAT, 0644, &f) == 0);
    CHECK(file_write(f, data, 3 * PAGE_SIZE) == 3 * (int64_t)PAGE_SIZE);
    CHECK(f->vn->pc.nr_dirty == 3);

    /* One refused write-back: fsync says so, and throws nothing away. */
    faultinject_set(FI_BLK_COMPLETE, 1, 1, thread_current());
    int s1 = file_sync(f);
    faultinject_clear(FI_BLK_COMPLETE);
    struct fi_stats fst;
    faultinject_stats(FI_BLK_COMPLETE, &fst);
    CHECK(fst.hits == 1);
    CHECK(s1 == -EIO);
    CHECK(f->vn->pc.nr_dirty > 0);
    pagecache_get_stats(&p1);
    CHECK(p1.wb_errors == p0.wb_errors + 1);

    /* The next attempt writes, and the file has already been told. */
    CHECK(file_sync(f) == 0);
    CHECK(f->vn->pc.nr_dirty == 0);
    CHECK(file_sync(f) == 0);
    file_put(f);

    /* On disk: a fresh mount reads it back. */
    CHECK(vfs_umount("/mnt/wb") == 0);
    CHECK(vfs_mount("/mnt/wb", "cosmofs", bd, 0) == 0);
    CHECK(wb_reads_back("/mnt/wb/f", data));
    kfree(data);
    wb_teardown(bd);
    return true;
}

bool selftest_wb_error_once(const char **reason)
{
    struct blkdev *bd;
    CHECK(wb_setup(&bd) == 0);
    uint8_t *page = kmalloc(PAGE_SIZE, 0);
    CHECK(page != NULL);
    wb_fill(page, 9);

    struct file *a, *b, *c;
    CHECK(vfs_open(NULL, "/mnt/wb/o", COSMO_O_RDWR | COSMO_O_CREAT, 0644, &a) == 0);
    CHECK(vfs_open(NULL, "/mnt/wb/o", COSMO_O_RDWR, 0, &b) == 0);
    CHECK(a->vn == b->vn);
    CHECK(file_write(a, page, PAGE_SIZE) == (int64_t)PAGE_SIZE);

    /* A's attempt fails; A hears its own failure once. */
    faultinject_set(FI_BLK_COMPLETE, 1, 1, thread_current());
    int sa = file_sync(a);
    faultinject_clear(FI_BLK_COMPLETE);
    struct fi_stats fst;
    faultinject_stats(FI_BLK_COMPLETE, &fst);
    CHECK(fst.hits == 1 && sa == -EIO);

    /* B's attempt writes the page, and B is told of the failure it was
     * open for -- once. A was told already. C, opened later, never. */
    CHECK(file_sync(b) == -EIO);
    CHECK(b->vn->pc.nr_dirty == 0);
    CHECK(file_sync(b) == 0);
    CHECK(file_sync(a) == 0);
    CHECK(vfs_open(NULL, "/mnt/wb/o", COSMO_O_RDONLY, 0, &c) == 0);
    CHECK(file_sync(c) == 0);
    file_put(c);
    file_put(b);
    file_put(a);
    CHECK(wb_reads_back("/mnt/wb/o", page));
    kfree(page);
    wb_teardown(bd);
    return true;
}

/* A file in a handle table, as the syscall holds it. */
static int wb_open_in_table(struct handle_table *t, const char *path, const uint8_t *page)
{
    struct file *f;
    int rc = vfs_open(NULL, path, COSMO_O_WRONLY | COSMO_O_CREAT, 0644, &f);
    if (rc)
        return rc;
    if (file_write(f, page, PAGE_SIZE) != (int64_t)PAGE_SIZE) {
        file_put(f);
        return -EIO;
    }
    int h = handle_install(t, &f->obj, HANDLE_RIGHT_OWNER);
    file_put(f);   /* the table holds the reference now */
    return h;
}

bool selftest_wb_error_close(const char **reason)
{
    struct blkdev *bd;
    CHECK(wb_setup(&bd) == 0);
    uint8_t *page = kmalloc(PAGE_SIZE, 0);
    CHECK(page != NULL);
    wb_fill(page, 5);
    struct handle_table *t = kmalloc(sizeof(*t), KMEM_ZERO);
    CHECK(t != NULL);
    handle_table_init(t);
    struct pagecache_stats p0, p1;
    pagecache_get_stats(&p0);

    int h = wb_open_in_table(t, "/mnt/wb/c", page);
    CHECK(h >= 0);
    /* close: the flush's write-back is refused and close says so; the
     * handle is closed regardless; the release's own retry then writes
     * the page (the injection is spent), so nothing is lost. */
    faultinject_set(FI_BLK_COMPLETE, 1, 1, thread_current());
    int rc = handle_close(t, h);
    faultinject_clear(FI_BLK_COMPLETE);
    struct fi_stats fst;
    faultinject_stats(FI_BLK_COMPLETE, &fst);
    CHECK(fst.hits == 1);
    CHECK(rc == -EIO);
    CHECK(handle_close(t, h) == -EBADF);
    pagecache_get_stats(&p1);
    CHECK(p1.dropped_dirty == p0.dropped_dirty);
    CHECK(p1.wb_errors == p0.wb_errors + 1);
    /* A clean close reports nothing. */
    h = wb_open_in_table(t, "/mnt/wb/c2", page);
    CHECK(h >= 0);
    CHECK(handle_close(t, h) == 0);
    handle_table_destroy(t);
    kfree(t);

    CHECK(vfs_umount("/mnt/wb") == 0);
    CHECK(vfs_mount("/mnt/wb", "cosmofs", bd, 0) == 0);
    CHECK(wb_reads_back("/mnt/wb/c", page));
    CHECK(wb_reads_back("/mnt/wb/c2", page));
    kfree(page);
    wb_teardown(bd);
    return true;
}

bool selftest_wb_error_lost(const char **reason)
{
    struct blkdev *bd;
    CHECK(wb_setup(&bd) == 0);
    uint8_t *page = kmalloc(PAGE_SIZE, 0);
    CHECK(page != NULL);
    wb_fill(page, 2);
    struct handle_table *t = kmalloc(sizeof(*t), KMEM_ZERO);
    CHECK(t != NULL);
    handle_table_init(t);
    struct pagecache_stats p0, p1;
    pagecache_get_stats(&p0);

    int h = wb_open_in_table(t, "/mnt/wb/l", page);
    CHECK(h >= 0);
    /* Three refusals: the flush's, the file release's retry, the vnode
     * release's last attempt. Then the page is gone -- counted, said. */
    faultinject_set(FI_BLK_COMPLETE, 1, 3, thread_current());
    int rc = handle_close(t, h);
    faultinject_clear(FI_BLK_COMPLETE);
    struct fi_stats fst;
    faultinject_stats(FI_BLK_COMPLETE, &fst);
    CHECK(fst.hits == 3);
    CHECK(rc == -EIO);
    pagecache_get_stats(&p1);
    CHECK(p1.dropped_dirty == p0.dropped_dirty + 1);
    CHECK(p1.wb_errors == p0.wb_errors + 3);
    handle_table_destroy(t);
    kfree(t);

    /* The mount is not poisoned: a new file writes and syncs. */
    struct file *f;
    CHECK(vfs_open(NULL, "/mnt/wb/after", COSMO_O_WRONLY | COSMO_O_CREAT, 0644, &f) == 0);
    CHECK(file_write(f, page, PAGE_SIZE) == (int64_t)PAGE_SIZE);
    CHECK(file_sync(f) == 0);
    file_put(f);
    kinfo("selftest: wb-error-lost: one page lost after three refusals, counted; the mount still writes");
    kfree(page);
    wb_teardown(bd);
    return true;
}

#else

#define WB_STUB(fn, name)                                                                     \
    bool fn(const char **reason)                                                              \
    {                                                                                         \
        (void)reason;                                                                         \
        kinfo("selftest: " name ": fault injection is compiled out of this build");          \
        return true;                                                                          \
    }
WB_STUB(selftest_wb_error_fsync, "wb-error-fsync")
WB_STUB(selftest_wb_error_once, "wb-error-once")
WB_STUB(selftest_wb_error_close, "wb-error-close")
WB_STUB(selftest_wb_error_lost, "wb-error-lost")

#endif

/* --- benchmarks: the object path per request size ------------------------
 *
 * Prints, asserts nothing. The audit's missing "read/write bandwidth at
 * 1 KiB chunking": a 1 MiB file read and written through file_read /
 * file_write with kernel buffers of 1 KiB, 4 KiB and 64 KiB -- the
 * object side of what the syscall bounce now allows per call. The
 * syscall side (entry, range check, copy) is measured from user mode
 * (init --selftest, USERBENCH lines). */

static void bench_pass(const char *what, struct file *f, uint8_t *buf, size_t req, bool write)
{
    file_seek(f, 0, COSMO_SEEK_SET);
    uint64_t t0 = clock_now_ns();
    unsigned calls = 0;
    size_t total = 0;
    while (total < 1024u * 1024u) {
        int64_t n = write ? file_write(f, buf, req) : file_read(f, buf, req);
        if (n <= 0)
            break;
        total += (size_t)n;
        calls++;
    }
    uint64_t dt = clock_now_ns() - t0;
    kinfo("selftest: %s-bench: %s %zu KiB requests: %u calls, %llu us, %llu MiB/s", write ? "write" : "read", what,
          req / 1024, calls, (unsigned long long)(dt / 1000),
          (unsigned long long)(dt ? (uint64_t)total * 1000000000ull / dt / (1024 * 1024) : 0));
}

/* The benches' CHECK: frees the bench buffer before returning on a
 * failure, so a failing check leaks nothing (the analyzer's finding). */
#define BCHECK(cond)                                                           \
    do {                                                                       \
        if (!(cond)) {                                                         \
            kfree(buf);                                                        \
            *reason = "check failed: " #cond " at line " STR(__LINE__);        \
            return false;                                                      \
        }                                                                      \
    } while (0)

bool selftest_read_bench(const char **reason)
{
    static const size_t reqs[] = { 1024, 4096, 65536 };
    uint8_t *buf = kmalloc(65536, 0);
    CHECK(buf != NULL);
    memset(buf, 0x5a, 65536);

    /* ramfs: the copy alone. */
    struct file *f;
    BCHECK(vfs_open(NULL, "/tmp/bench.bin", COSMO_O_RDWR | COSMO_O_CREAT | COSMO_O_TRUNC, 0644, &f) == 0);
    for (unsigned i = 0; i < 16; i++)
        BCHECK(file_write(f, buf, 65536) == 65536);
    for (unsigned i = 0; i < 3; i++)
        bench_pass("ramfs", f, buf, reqs[i], false);
    file_put(f);
    BCHECK(vfs_unlink(NULL, "/tmp/bench.bin") == 0);

    /* cosmofs on ramblk: cold (after a remount, through the device) and
     * warm (the page cache). */
    struct blkdev *bd;
    BCHECK(wb_setup(&bd) == 0);
    BCHECK(vfs_open(NULL, "/mnt/wb/bench", COSMO_O_WRONLY | COSMO_O_CREAT, 0644, &f) == 0);
    for (unsigned i = 0; i < 16; i++)
        BCHECK(file_write(f, buf, 65536) == 65536);
    BCHECK(file_sync(f) == 0);
    file_put(f);
    for (unsigned i = 0; i < 3; i++) {
        BCHECK(vfs_umount("/mnt/wb") == 0);
        BCHECK(vfs_mount("/mnt/wb", "cosmofs", bd, 0) == 0);
        BCHECK(vfs_open(NULL, "/mnt/wb/bench", COSMO_O_RDONLY, 0, &f) == 0);
        bench_pass("cosmofs cold", f, buf, reqs[i], false);
        bench_pass("cosmofs warm", f, buf, reqs[i], false);
        file_put(f);
    }
    wb_teardown(bd);
    kfree(buf);
    return true;
}

bool selftest_write_bench(const char **reason)
{
    static const size_t reqs[] = { 1024, 4096, 65536 };
    uint8_t *buf = kmalloc(65536, 0);
    CHECK(buf != NULL);
    memset(buf, 0xa5, 65536);
    for (unsigned i = 0; i < 3; i++) {
        struct file *f;
        BCHECK(vfs_open(NULL, "/tmp/wbench.bin", COSMO_O_WRONLY | COSMO_O_CREAT | COSMO_O_TRUNC, 0644, &f) == 0);
        bench_pass("ramfs", f, buf, reqs[i], true);
        file_put(f);
        BCHECK(vfs_unlink(NULL, "/tmp/wbench.bin") == 0);
    }
    kfree(buf);
    return true;
}
