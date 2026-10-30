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
#include <kernel/epoll.h>
#include <kernel/eventfd.h>
#include <kernel/handle.h>
#include <kernel/object.h>
#include <kernel/pipe.h>
#include <kernel/process.h>
#include <kernel/unix.h>

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

/* --- object-fault-sweep: every allocation of object creation failed in turn (roadmap 2.0 SA2) --- */

/* An operation makes kernel objects and installs them in `t`, as the
 * system calls do: 0, or the first error. The sweep destroys the table
 * afterwards, which drops what it holds. */
typedef int (*sweep_op)(struct handle_table *t);
/* Undoes what outlives the table on purpose (a created file stays on its
 * filesystem); a leaked reference survives it, and the census sees that. */
typedef void (*sweep_undo)(void);

/* Installs `obj` and drops the creator's reference: the table's is then the only one. */
static int sweep_install(struct handle_table *t, struct kobject *obj, unsigned rights)
{
    int h = handle_install(t, obj, rights);
    kobject_put(obj);
    return h;
}

static int sweep_pipe(struct handle_table *t)
{
    struct kobject *r, *w;
    int rc = pipe_create(&r, &w);
    if (rc)
        return rc;
    int a = sweep_install(t, r, HANDLE_RIGHT_READ);
    int b = sweep_install(t, w, HANDLE_RIGHT_WRITE);
    return a < 0 ? a : b < 0 ? b : 0;
}

static int sweep_eventfd(struct handle_table *t)
{
    struct kobject *e;
    int rc = eventfd_obj_create(0, false, &e);
    if (rc)
        return rc;
    int h = sweep_install(t, e, HANDLE_RIGHT_ALL);
    return h < 0 ? h : 0;
}

/* An epoll instance watching a pipe's read end: the registration holds a
 * reference that only the last close of the pipe end takes back. */
static int sweep_epoll(struct handle_table *t)
{
    struct kobject *ep, *r, *w;
    int rc = epoll_obj_create(&ep);
    if (rc)
        return rc;
    int he = sweep_install(t, ep, HANDLE_RIGHT_ALL);
    rc = pipe_create(&r, &w);
    if (rc)
        return rc;
    int hr = sweep_install(t, r, HANDLE_RIGHT_READ);
    int hw = sweep_install(t, w, HANDLE_RIGHT_WRITE);
    if (he < 0 || hr < 0 || hw < 0)
        return -EMFILE;
    struct kobject *epobj = handle_lookup(t, he, 0);
    struct kobject *target = handle_lookup(t, hr, 0);
    rc = epoll_obj_add(epobj, hr, target, COSMO_IO_READABLE, 0, (uint64_t)hr, false, false);
    if (rc)
        kobject_put(target);   /* a registration takes the lookup's reference; a failed one leaves it ours */
    kobject_put(epobj);
    return rc;
}

static int sweep_tcp(struct handle_table *t)
{
    struct socket *s;
    int rc = ksock_create(COSMO_AF_INET, COSMO_SOCK_STREAM, 0, &s);
    if (rc)
        return rc;
    int h = sweep_install(t, &s->obj, HANDLE_RIGHT_ALL);
    return h < 0 ? h : 0;
}

static int sweep_unix(struct handle_table *t)
{
    struct socket *a, *b;
    int rc = unix_socketpair(COSMO_SOCK_STREAM, &a, &b);
    if (rc)
        return rc;
    int ha = sweep_install(t, &a->obj, HANDLE_RIGHT_ALL);
    int hb = sweep_install(t, &b->obj, HANDLE_RIGHT_ALL);
    return ha < 0 ? ha : hb < 0 ? hb : 0;
}

static void sweep_file_undo(void)
{
    (void)vfs_unlink(NULL, "/tmp/fi-sweep");
}

static int sweep_file(struct handle_table *t)
{
    struct file *f;
    int rc = vfs_open(NULL, "/tmp/fi-sweep", COSMO_O_WRONLY | COSMO_O_CREAT | COSMO_O_TRUNC, 0644, &f);
    if (rc)
        return rc;
    int h = sweep_install(t, &f->obj, HANDLE_RIGHT_ALL);
    return h < 0 ? h : 0;
}

/* A process from the boot archive's init that exits at once: its
 * address space, threads, handle table and process object. */
static int sweep_process(struct handle_table *t)
{
    (void)t;
    const void *image;
    size_t size;
    if (!bootarchive_find("init", &image, &size))
        return 0;
    static const char *const argv[] = { "init", "--exec-done", "0", NULL };
    struct process *p = NULL;
    int rc = process_create_from_elf(image, size, "fi-sweep", argv, NULL, NULL, &p);
    if (rc)
        return rc;
    (void)process_wait_exit(p);
    process_put(p);
    return 0;
}

/* The census back at `before`, allowing two seconds for late releases (a
 * reaped process, a deferred free), as the runner does. */
static bool census_returns(uint64_t before)
{
    for (unsigned ms = 0; ms < 2000; ms += 10) {
        if (kobject_census() == before)
            return true;
        thread_sleep_ms(10);
    }
    return kobject_census() == before;
}

bool selftest_object_fault_sweep(const char **reason)
{
    static const struct {
        const char *name;
        sweep_op op;
        sweep_undo undo;
    } ops[] = {
        { "pipe", sweep_pipe, NULL }, { "eventfd", sweep_eventfd, NULL }, { "epoll", sweep_epoll, NULL },
        { "tcp", sweep_tcp, NULL },   { "unix", sweep_unix, NULL },       { "file", sweep_file, sweep_file_undo },
        { "process", sweep_process, NULL },
    };
    static struct handle_table t;   /* 64 slots: too big for a stack frame */
    thread_sleep_ms(20);            /* the reaper and deferred frees from earlier tests */
    unsigned total = 0;
    for (size_t k = 0; k < sizeof(ops) / sizeof(ops[0]); k++) {
        unsigned failed = 0;
        bool clean = false;
        for (unsigned n = 1; n <= 1000 && !clean; n++) {
            uint64_t before = kobject_census();
            handle_table_init(&t);
            faultinject_set(FI_KMALLOC, n, 1, thread_current());
            int rc = ops[k].op(&t);
            struct fi_stats st;
            faultinject_stats(FI_KMALLOC, &st);
            faultinject_clear(FI_KMALLOC);
            if (st.hits == 0) {
                CHECK(rc == 0);   /* past the operation's last allocation: a clean run ends the sweep */
                clean = true;
            } else {
                CHECK(rc == 0 || rc == -ENOMEM);
                failed += rc != 0;
            }
            handle_table_destroy(&t);
            if (ops[k].undo != NULL)
                ops[k].undo();
            if (!census_returns(before)) {
                faultinject_clear_all();
                kinfo("selftest: object-fault-sweep: %s with allocation %u failed: census %llu before, %llu after",
                      ops[k].name, n, (unsigned long long)before, (unsigned long long)kobject_census());
                *reason = "an object leaked under an allocation failure";
                return false;
            }
            total++;
        }
        CHECK(clean);
        kinfo("selftest: object-fault-sweep: %s: %u attempts, %u ended -ENOMEM, census unchanged after each",
              ops[k].name, total, failed);
        total = 0;
    }
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

bool selftest_object_fault_sweep(const char **reason)
{
    (void)reason;
    kinfo("selftest: object-fault-sweep: fault injection is compiled out of this build");
    return true;
}

#endif
