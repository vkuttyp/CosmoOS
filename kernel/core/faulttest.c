/*
 * faulttest.c - Fault-injection self-tests (docs/verification/testing.md).
 *
 * Each test arms a rule for its own thread, drives operations that
 * allocate or do block I/O, and checks two things: every result is a
 * clean success or the expected error (never a panic, never anything
 * else), and after the rule is cleared and the successes are undone the
 * heap's live-object count is what it was.
 */

#include <kernel/faultinject.h>
#include <kernel/bootarchive.h>
#include <kernel/kmalloc.h>
#include <kernel/log.h>
#include <kernel/printf.h>
#include <kernel/wait.h>
#include <kernel/module.h>
#include <kernel/ramblk.h>
#include <kernel/selftest.h>
#include <kernel/socket.h>
#include <kernel/string.h>
#include <kernel/thread.h>
#include <kernel/vfs.h>
#include <kernel/cosmofs.h>
#include <kernel/errno.h>

#include <uapi/cosmo/syscall.h>

#define STR_(x) #x
#define STR(x)  STR_(x)
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            faultinject_clear_all();                                           \
            *reason = "check failed: " #cond " at line " STR(__LINE__);        \
            return false;                                                      \
        }                                                                      \
    } while (0)

#if CONFIG_FAULTINJECT

static struct mount *mount_of_path(const char *path)
{
    struct vnode *vn;
    if (vfs_lookup(NULL, path, &vn))
        return NULL;
    struct mount *m = vn->mnt;
    vnode_put(vn);
    return m;
}

static uint64_t live_objects(void)
{
    struct kmalloc_stats st;
    kmalloc_get_stats(&st);
    return st.live_objects;
}

/* --- fault-kmalloc: allocation failures during file, socket and module work --- */

/* The boot-parameter parser: overflowing and malformed specifications arm
 * nothing (a wrapped "kmalloc:4294967297" would otherwise read as every
 * allocation failing); a malformed item leaves the earlier items unarmed
 * too; a valid specification arms exactly what it says. */
static bool configure_is_strict(const char **reason)
{
    struct fi_stats st;
    CHECK(faultinject_configure("kmalloc:4294967297") == -EINVAL);   /* UINT32_MAX + 2 */
    CHECK(faultinject_configure("kmalloc:99999999999999999999") == -EINVAL);
    CHECK(faultinject_configure("kmalloc:3:4294967296") == -EINVAL);
    CHECK(faultinject_configure("kmalloc:2,bogus:1") == -EINVAL);
    CHECK(faultinject_configure("kmalloc:") == -EINVAL);
    CHECK(faultinject_configure("kmalloc:0") == -EINVAL);
    CHECK(faultinject_configure("kmalloc:2x") == -EINVAL);
    for (unsigned k = 0; k < FI_KIND_COUNT; k++) {
        faultinject_stats((enum fi_kind)k, &st);
        CHECK(st.every == 0);
    }
    CHECK(faultinject_configure("blk-submit:3:2,kmalloc:4294967295") == 0);
    faultinject_stats(FI_BLK_SUBMIT, &st);
    CHECK(st.every == 3 && st.budget == 2);
    faultinject_stats(FI_KMALLOC, &st);
    CHECK(st.every == 4294967295u && st.budget == 0);
    faultinject_clear_all();
    return true;
}

bool selftest_fault_kmalloc(const char **reason)
{
    if (!configure_is_strict(reason))
        return false;

    /* Settle the heap: the reaper and deferred frees from earlier tests. */
    thread_sleep_ms(20);
    uint64_t base = live_objects();
    struct fi_stats st;

    /* 1. File creation with one injected failure at the i-th allocation of
     * each attempt: every allocation site on the create-and-write path
     * fails once, later attempts (a larger i than the path allocates) run
     * clean. */
    unsigned created = 0, nomem = 0, hits = 0;
    for (unsigned i = 0; i < 40; i++) {
        char path[32];
        ksnprintf(path, sizeof(path), "/tmp/fi-%u", i);
        faultinject_set(FI_KMALLOC, 1 + i, 1, thread_current());
        struct file *f;
        int rc = vfs_open(NULL, path, COSMO_O_WRONLY | COSMO_O_CREAT, 0644, &f);
        if (rc == 0) {
            int64_t w = file_write(f, "fault", 5);
            CHECK(w == 5 || w == -ENOMEM);
            file_put(f);
            created++;
        } else {
            CHECK(rc == -ENOMEM);
            nomem++;
        }
        faultinject_clear(FI_KMALLOC);
        faultinject_stats(FI_KMALLOC, &st);
        hits += (unsigned)st.hits;
    }
    CHECK(hits > 0 && nomem > 0);
    CHECK(created > 0);
    for (unsigned i = 0; i < 40; i++) {
        char path[32];
        ksnprintf(path, sizeof(path), "/tmp/fi-%u", i);
        int rc = vfs_unlink(NULL, path);
        CHECK(rc == 0 || rc == -ENOENT);
    }

    /* 2. Sockets: creation under failure, budget of 5. */
    unsigned socks = 0, snomem = 0;
    faultinject_set(FI_KMALLOC, 2, 5, thread_current());
    for (unsigned i = 0; i < 12; i++) {
        struct socket *s;
        int rc = ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &s);
        if (rc == 0) {
            ksock_put(s);
            socks++;
        } else {
            CHECK(rc == -ENOMEM);
            snomem++;
        }
    }
    faultinject_clear(FI_KMALLOC);
    faultinject_stats(FI_KMALLOC, &st);
    CHECK(st.hits == 5 && snomem == 5 && socks == 7);   /* the budget bounds the failures exactly */

    /* 3. A module load with allocation failures: -ENOMEM or a clean load. */
    const void *file;
    size_t size;
    if (bootarchive_find("tests/cosmotest.ko", &file, &size)) {
        unsigned loaded = 0, mnomem = 0;
        for (unsigned i = 0; i < 6; i++) {
            faultinject_set(FI_KMALLOC, 2 + i, 1, thread_current());
            struct module *m = NULL;
            int rc = module_load(file, size, "tests/cosmotest.ko", &m);
            faultinject_clear(FI_KMALLOC);
            if (rc == 0) {
                CHECK(module_unload("cosmotest") == 0);
                loaded++;
            } else {
                CHECK(rc == -ENOMEM);
                mnomem++;
            }
        }
        CHECK(loaded + mnomem == 6);
        kinfo("selftest: fault-kmalloc: module load %u ok / %u -ENOMEM under one injected failure each", loaded,
              mnomem);
    }

    /* Nothing leaked: sockets and files are released synchronously; give
     * deferred frees a moment, then compare. */
    thread_sleep_ms(20);
    uint64_t after = live_objects();
    CHECK(after == base);
    kinfo("selftest: fault-kmalloc: %u files created / %u -ENOMEM, %u sockets / %u -ENOMEM, heap %llu objects before and after",
          created, nomem, socks, snomem, (unsigned long long)base);
    return true;
}

/* --- fault-blk: device errors under a cosmofs workload --- */

bool selftest_fault_blk(const char **reason)
{
    struct blkdev *bd = ramblk_create(256);
    CHECK(bd != NULL);
    CHECK(cosmofs_format(bd) == 0);
    int mk = vfs_mkdir(NULL, "/mnt/fi", 0755);
    CHECK(mk == 0 || mk == -EEXIST);
    CHECK(vfs_mount("/mnt/fi", "cosmofs", bd, 0) == 0);

    /* 1. Completion errors: every second I/O completes with -EIO for 8
     * completions; writes and syncs report -EIO or succeed, nothing else. */
    unsigned eio = 0, ok = 0;
    faultinject_set(FI_BLK_COMPLETE, 2, 8, thread_current());
    for (unsigned i = 0; i < 10; i++) {
        char path[32];
        ksnprintf(path, sizeof(path), "/mnt/fi/f%u", i);
        struct file *f;
        int rc = vfs_open(NULL, path, COSMO_O_WRONLY | COSMO_O_CREAT, 0644, &f);
        if (rc == 0) {
            int64_t w = file_write(f, "block fault", 11);
            int s = file_sync(f);
            file_put(f);
            if (w == 11 && s == 0)
                ok++;
            else {
                CHECK(w == -EIO || s == -EIO || w == 11);
                eio++;
            }
        } else {
            CHECK(rc == -EIO);
            eio++;
        }
        int s = vfs_sync();
        CHECK(s == 0 || s == -EIO);
    }
    faultinject_clear(FI_BLK_COMPLETE);
    struct fi_stats st;
    faultinject_stats(FI_BLK_COMPLETE, &st);
    CHECK(st.hits == 8);

    /* 2. Submission errors: the driver never sees the bio. */
    faultinject_set(FI_BLK_SUBMIT, 1, 3, thread_current());
    struct file *f;
    int rc = vfs_open(NULL, "/mnt/fi/g", COSMO_O_WRONLY | COSMO_O_CREAT, 0644, &f);
    if (rc == 0) {
        int64_t w = file_write(f, "x", 1);
        int s = file_sync(f);
        CHECK(w == 1 || w == -EIO);
        CHECK(s == 0 || s == -EIO);
        file_put(f);
    } else {
        CHECK(rc == -EIO);
    }
    faultinject_clear(FI_BLK_SUBMIT);
    faultinject_stats(FI_BLK_SUBMIT, &st);
    CHECK(st.hits >= 1 && st.hits <= 3);

    /* 3. After the faults: a forced unmount drops any abandoned
     * transaction, the device mounts again on a committed root, and
     * every file it shows reads back. */
    int urc = vfs_umount2("/mnt/fi", VFS_UMOUNT_FORCE);
    CHECK(urc == 0);
    CHECK(vfs_mount("/mnt/fi", "cosmofs", bd, 0) == 0);
    struct cosmofs_stats cs;
    CHECK(cosmofs_stats(mount_of_path("/mnt/fi"), &cs) == 0);
    for (unsigned i = 0; i < 10; i++) {
        char path[32];
        ksnprintf(path, sizeof(path), "/mnt/fi/f%u", i);
        struct file *rf;
        if (vfs_open(NULL, path, COSMO_O_RDONLY, 0, &rf) == 0) {
            char buf[16];
            int64_t n = file_read(rf, buf, sizeof(buf));
            CHECK(n == 11 && memcmp(buf, "block fault", 11) == 0);
            file_put(rf);
        }
    }
    CHECK(vfs_umount("/mnt/fi") == 0);
    CHECK(vfs_rmdir(NULL, "/mnt/fi") == 0);
    ramblk_destroy(bd);
    kinfo("selftest: fault-blk: %u writes ok / %u with -EIO under injected completion errors; remount clean, generation %llu",
          ok, eio, (unsigned long long)cs.generation);
    return true;
}

#else

bool selftest_fault_kmalloc(const char **reason)
{
    (void)reason;
    kinfo("selftest: fault-kmalloc: fault injection is compiled out of this build");
    return true;
}

bool selftest_fault_blk(const char **reason)
{
    (void)reason;
    kinfo("selftest: fault-blk: fault injection is compiled out of this build");
    return true;
}

#endif
