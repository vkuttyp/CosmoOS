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
